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
unsigned long lastDisplayUpdate = 0;
bool canInitialized = false;
unsigned long lastMessageCount = 0;
unsigned long messagesPerSecond = 0;
bool sdCardAvailable = false;

// Dual buffer system
char buffer1[BUFFER_SIZE];
char buffer2[BUFFER_SIZE];
char* activeBuffer = buffer1;
char* writeBuffer = buffer2;
size_t activeBufferPos = 0;
bool bufferReadyToWrite = false;

struct CANMessage
{
    long unsigned int id;
    unsigned char len;
    unsigned char buf[8];
    double timestamp;
};

QueueHandle_t canQueue;

String getTimestampFilename();
bool initCAN();
void CANReceiverTask(void* pvParameters);
void CANTransmitTask(void* pvParameters);
void CANProcessorTask(void* pvParameters);
void SDWriterTask(void* pvParameters);
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
        String filename = getTimestampFilename();
        logFile = SD.open(filename, FILE_WRITE);
        if (logFile)
        {
            M5.Lcd.printf("Logging to:\n%s\n", filename.c_str());
            logFile.println("CAN Bus Log Started");
            logFile.flush();
            sdCardAvailable = true;
        }
        else
        {
            M5.Lcd.println("Failed to open file!");
            sdCardAvailable = false;
        }
    }

    // Initialize CAN bus
    if (!initCAN())
    {
        M5.Lcd.println("CAN Init Failed! Retrying...");
        delay(1000);
        initCAN();
    }

    // Create message queue
    canQueue = xQueueCreate(QUEUE_SIZE, sizeof(CANMessage));

    // Start tasks
    xTaskCreatePinnedToCore(CANReceiverTask, "CANReceiver", 8192, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(CANProcessorTask, "CANProcessor", 8192, NULL, 1, NULL, 1);
    if (sdCardAvailable)
    {
        xTaskCreatePinnedToCore(SDWriterTask, "SDWriter", 8192, NULL, 1, NULL, 1);
    }

    // Initial display
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.println("CAN Messages Received:");
    displayMessageCount();
}

void loop()
{
    // Update display once per second
    if (millis() - lastDisplayUpdate >= 1000)
    {
        lastDisplayUpdate = millis();
        messagesPerSecond = messageCount - lastMessageCount;
        lastMessageCount = messageCount;
        displayMessageCount();
    }

    // System monitoring
    static uint32_t lastHeapCheck = 0;
    if (millis() - lastHeapCheck > 5000)
    {
        lastHeapCheck = millis();
        Serial.printf("System Status - Free Heap: %d, Queue: %d, Buffer: %d/%d\n",
                      ESP.getFreeHeap(),
                      uxQueueMessagesWaiting(canQueue),
                      activeBufferPos,
                      BUFFER_SIZE);
    }
    delay(10);
}

// ==================== CAN Tasks ====================

void CANReceiverTask(void* pvParameters)
{
    for (;;)
    {
        if (!digitalRead(CAN0_INT))
        {
            while (!digitalRead(CAN0_INT))
            {
                long unsigned int rxId;
                unsigned char len = 0;
                unsigned char rxBuf[8];

                if (CAN0.readMsgBuf(&rxId, &len, rxBuf) == CAN_OK)
                {
                    CANMessage msg;
                    msg.id = rxId;
                    msg.len = len;
                    memcpy(msg.buf, rxBuf, len);
                    msg.timestamp = getUnixTimestamp();

                    if (xQueueSend(canQueue, &msg, pdMS_TO_TICKS(10)) != pdTRUE)
                    {
                        Serial.println("Queue full! Dropped message");
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void CANProcessorTask(void* pvParameters)
{
    CANMessage msg;
    for (;;)
    {
        if (xQueueReceive(canQueue, &msg, portMAX_DELAY) == pdTRUE)
        {
            // Format CAN message directly into buffer
            int needed = snprintf(NULL, 0, "%.6f %lX#", msg.timestamp, msg.id) + (msg.len * 2) + 2;

            // Check if we need to switch buffers
            if (activeBufferPos + needed >= BUFFER_SIZE)
            {
                bufferReadyToWrite = true;
                while (bufferReadyToWrite)
                {
                    vTaskDelay(1); // Wait for SD writer to swap buffers
                }
            }

            // Write to active buffer
            activeBufferPos += snprintf(activeBuffer + activeBufferPos, BUFFER_SIZE - activeBufferPos,
                                        "(%.6f) can %lX#", msg.timestamp, msg.id);
            for (byte i = 0; i < msg.len; i++)
            {
                activeBufferPos += snprintf(activeBuffer + activeBufferPos, BUFFER_SIZE - activeBufferPos,
                                            "%02X", msg.buf[i]);
            }
            activeBufferPos += snprintf(activeBuffer + activeBufferPos, BUFFER_SIZE - activeBufferPos, "\n");

            messageCount++;

            if (!sdCardAvailable)
            {
                // Output to serial (optional)
                Serial.printf("%.6f %lX#", msg.timestamp, msg.id);
                for (byte i = 0; i < msg.len; i++)
                {
                    Serial.printf("%02X", msg.buf[i]);
                }
                Serial.println();
            }
        }
    }
}

void SDWriterTask(void* pvParameters)
{
    for (;;)
    {
        if (bufferReadyToWrite)
        {
            // Swap buffers
            char* temp = activeBuffer;
            activeBuffer = writeBuffer;
            writeBuffer = temp;
            size_t writeSize = activeBufferPos;
            activeBufferPos = 0;
            bufferReadyToWrite = false;

            // Write to SD card
            if (logFile)
            {
                size_t written = logFile.write((const uint8_t*)writeBuffer, writeSize);
                if (written != writeSize)
                {
                    Serial.println("SD write error!");
                }
                if (messageCount % 400 == 0) logFile.flush();
            }
        }
        vTaskDelay(1);
    }
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
    M5.Lcd.printf("%9lu", messageCount); // Total count

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
