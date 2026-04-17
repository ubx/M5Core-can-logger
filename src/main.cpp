#include <M5Unified.h>
#include <mcp_can.h>
#include <SPI.h>
#include <SD.h>
#include "m5_logo.h"

// MCP2515 setup
MCP_CAN CAN0(12); // CS pin

// SD Card settings
unsigned long lastDisplayUpdate = 0;
unsigned long lastMessageCount = 0;
unsigned long messagesPerSecond = 0;
File root;
File dataFile;
unsigned long transmitCount = 0;
bool fileFound = false;

bool initCAN();
void CANTransmitTask(void* pvParameters);
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
    }
    else
    {
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

    double lastTimestamp = -1.0;

    while (dataFile.available())
    {
        String line = dataFile.readStringUntil('\n');
        line.trim();

        // Format: (timestamp) can0 ID#DATA
        // Example: (1713351000.000000) can0 123#0102030405060708
        int openParen = line.indexOf('(');
        int closeParen = line.indexOf(')');
        int hashPos = line.indexOf('#');
        int canPos = line.indexOf("can0 ");

        if (hashPos != -1 && canPos != -1 && openParen != -1 && closeParen > openParen)
        {
            String tsStr = line.substring(openParen + 1, closeParen);
            double currentTimestamp = strtod(tsStr.c_str(), NULL);

            if (lastTimestamp >= 0)
            {
                double diff = currentTimestamp - lastTimestamp;
                if (diff > 0)
                {
                    // Delay for the time gap between messages
                    // vTaskDelay works in ticks, we might need more precision if gaps are very small
                    // but for log playback vTaskDelay should be okay for ms resolution.
                    // For better precision we could use ets_delay_us or a high res timer, 
                    // but sticking to FreeRTOS tasks style.
                    uint32_t delayMs = (uint32_t)(diff * 1000.0);
                    if (delayMs > 0)
                    {
                        vTaskDelay(pdMS_TO_TICKS(delayMs));
                    }
                }
            }
            lastTimestamp = currentTimestamp;

            String idStr = line.substring(canPos + 5, hashPos);
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
