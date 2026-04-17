#include <M5Unified.h>
#include <mcp_can.h>
#include <SPI.h>
#include <SD.h>
#include "m5_logo.h"

// MCP2515 setup
MCP_CAN CAN0(12); // CS pin

// SD Card settings
File logFile;
unsigned long messageCount = 0;
unsigned long sdCartWriteCount = 0;
unsigned long lastDisplayUpdate = 0;
bool canInitialized = false;
unsigned long lastMessageCount = 0;
unsigned long messagesPerSecond = 0;
bool sdCardAvailable = false;
File root;
File dataFile;
unsigned long transmitCount = 0;
bool fileFound = false;

// Dual buffer system
// char buffer1[BUFFER_SIZE];
// char buffer2[BUFFER_SIZE];
// char* activeBuffer = buffer1;
// char* writeBuffer = buffer2;
// size_t activeBufferPos = 0;
// bool bufferReadyToWrite = false;

struct CANMessage
{
    long unsigned int id;
    unsigned char len;
    unsigned char buf[8];
    double timestamp;
};

// QueueHandle_t canQueue;

String getTimestampFilename();
bool initCAN();
// void CANReceiverTask(void* pvParameters);
void CANTransmitTask(void* pvParameters);
// void CANProcessorTask(void* pvParameters);
// void SDWriterTask(void* pvParameters);
double getUnixTimestamp();
void displayMessageCount();

void setup()
{
    auto cfg = M5.config();
    cfg.external_spk = false;
    M5.begin(cfg);
    M5.Power.begin();
    M5.Lcd.setRotation(1);
    M5.Lcd.setTextSize(1);

    Serial.begin(115200);
    M5.Lcd.pushImage(0, 0, 320, 240, (uint16_t*)gImage_logoM5);
    delay(1000);
    M5.Lcd.clear();

    // Init SD card
    if (!SD.begin(GPIO_NUM_4, SPI, 25000000))
    {
        M5.Lcd.println("SD init failed!");
        sdCardAvailable = false;
    }
    else
    {
        sdCardAvailable = true;
        root = SD.open("/");
        while (true)
        {
            dataFile = root.openNextFile();
            if (!dataFile)
            {
                M5.Lcd.println("No files on SD!");
                break;
            }
            if (!dataFile.isDirectory())
            {
                M5.Lcd.printf("Found file: %s\n", dataFile.name());
                Serial.printf("Found file: %s\n", dataFile.name());
                fileFound = true;
                break;
            }
            dataFile.close();
        }
    }

    // Initialize CAN bus
    if (!initCAN())
    {
        M5.Lcd.println("CAN Init Failed! Retrying...");
        delay(1000);
        initCAN();
    }

    if (fileFound)
    {
        // Start transmit task
        xTaskCreatePinnedToCore(CANTransmitTask, "CANTransmit", 8192, NULL, 1, NULL, 1);
    }

    // Initial display
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("CAN Messages Transmitted:");
    displayMessageCount();
}

void loop()
{
    // Update display once per second
    if (millis() - lastDisplayUpdate >= 1000)
    {
        lastDisplayUpdate = millis();
        messagesPerSecond = transmitCount - lastMessageCount;
        lastMessageCount = transmitCount;
        displayMessageCount();
    }

    // System monitoring
    static uint32_t lastHeapCheck = 0;
    if (millis() - lastHeapCheck > 5000)
    {
        lastHeapCheck = millis();
        Serial.printf("System Status - Free Heap: %d, Transmitted: %lu\n",
                      ESP.getFreeHeap(),
                      transmitCount);
    }
    delay(10);
}

// ==================== CAN Transmit Task ====================

uint8_t hexToByte(char high, char low)
{
    auto charToHex = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    return (charToHex(high) << 4) | charToHex(low);
}

void CANTransmitTask(void* pvParameters)
{
    if (!dataFile)
    {
        vTaskDelete(NULL);
        return;
    }

    Serial.printf("Starting transmission of file: %s\n", dataFile.name());

    while (dataFile.available())
    {
        String line = dataFile.readStringUntil('\n');
        line.trim();

        // Format: (timestamp) can ID#DATA
        // Example: (1713351000.000000) can 123#0102030405060708
        int hashPos = line.indexOf('#');
        int canPos = line.indexOf("can0 ");

        if (hashPos != -1 && canPos != -1)
        {
            String idStr = line.substring(canPos + 4, hashPos);
            String dataStr = line.substring(hashPos + 1);

            unsigned long id = strtoul(idStr.c_str(), NULL, 16);
            uint8_t len = dataStr.length() / 2;
            if (len > 8) len = 8;
            uint8_t data[8];

            for (int i = 0; i < len; i++)
            {
                data[i] = hexToByte(dataStr[i * 2], dataStr[i * 2 + 1]);
            }

            if (CAN0.sendMsgBuf(id, 0, len, data) == CAN_OK)
            {
                transmitCount++;
            }
            else
            {
                Serial.println("Error sending CAN message");
            }
            // Small delay to prevent flooding the bus too much
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    dataFile.close();
    Serial.println("Finished transmitting log file");
    vTaskDelete(NULL);
}

// ==================== Utility Functions ====================

bool initCAN()
{
    SPI.begin();
    SPI.setClockDivider(SPI_CLOCK_DIV4);

    uint8_t retries = 3;
    while (retries--)
    {
        if (CAN0.begin(MCP_ANY, CAN_500KBPS, MCP_8MHZ) == CAN_OK)
        {
            CAN0.setMode(MCP_NORMAL);
            pinMode(CAN0_INT, INPUT_PULLUP);
            canInitialized = true;
            return true;
        }
        delay(100);
    }
    return false;
}

void displayMessageCount()
{
    M5.Lcd.fillRect(0, 20, 320, 60, BLACK); // Clear display area
    M5.Lcd.setCursor(0, 20);
    M5.Lcd.setTextSize(4);
    M5.Lcd.printf("%9lu", transmitCount); // Total count

    // Show messages/second
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(220, 25);
    M5.Lcd.printf("%d/s", messagesPerSecond);

    M5.Lcd.setTextSize(1); // Reset text size
}

double getUnixTimestamp()
{
    m5::rtc_datetime_t now = M5.Rtc.getDateTime();
    struct tm tm;
    tm.tm_year = now.date.year - 1900;
    tm.tm_mon = now.date.month - 1;
    tm.tm_mday = now.date.date;
    tm.tm_hour = now.time.hours;
    tm.tm_min = now.time.minutes;
    tm.tm_sec = now.time.seconds;
    time_t time = mktime(&tm);

    static unsigned long startMillis = millis();
    double secondsFraction = (millis() - startMillis) / 1000.0;

    return (double)time + secondsFraction;
}

String getTimestampFilename()
{
    m5::rtc_datetime_t now = M5.Rtc.getDateTime();
    char filename[64];
    snprintf(filename, sizeof(filename),
             "/candump-%04d%02d%02d-%02d%02d%02d.log",
             now.date.year,
             now.date.month,
             now.date.date,
             now.time.hours,
             now.time.minutes,
             now.time.seconds);
    return String(filename);
}
