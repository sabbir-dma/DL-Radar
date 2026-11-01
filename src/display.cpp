
// display.cpp
#include "display.h"
#include <Arduino.h>

#include "main.h"

unsigned long touchStartTime = 0;
bool longPressTriggered = false;
const unsigned long LONG_PRESS_TIME = 5000;


bool buttonState = false;  // Global variable to track button state

// ============================================================
// Global object definitions
// ============================================================
Adafruit_SSD1306 display(
    SCREEN_WIDTH,
    SCREEN_HEIGHT,
    &SPI,
    OLED_DC,
    OLED_RST,
    OLED_CS
);

DisplayData displayData = {
    NAN,      // airPressure
    NAN,      // airTemperature
    NAN,      // airHumidity
    NAN,      // windSpeed
    NAN,      // windDirection
    NAN,      // rainFall
    0,        // signalLevel
    "OFFLINE",// statusText
    75        // batteryPercent (new)
};

TaskHandle_t displayTaskHandle = NULL;
QueueHandle_t displayCommandQueue = NULL;

// ============================================================
// Helper functions (static to avoid external linkage)
// ============================================================
static void drawBatteryIcon(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    display.drawRect(2, 1, 16, 7, SSD1306_WHITE);
    display.fillRect(18, 3, 2, 3, SSD1306_WHITE);

    int fillWidth = (percent * 12) / 100;
    if (fillWidth > 0) {
        display.fillRect(4, 3, fillWidth, 3, SSD1306_WHITE);
    }
}

static void drawSignalBars(int level) {
    if (level >= 1) display.fillRect(112, 5, 2, 4,  SSD1306_WHITE);
    if (level >= 2) display.fillRect(115, 4, 2, 5,  SSD1306_WHITE);
    if (level >= 3) display.fillRect(118, 3, 2, 6,  SSD1306_WHITE);
    if (level >= 4) display.fillRect(121, 2, 2, 7,  SSD1306_WHITE);
    if (level >= 5) display.fillRect(124, 1, 2, 8,  SSD1306_WHITE);
}

// ============================================================
// Display functions
// ============================================================
static void displayBootAnimation() {
    const int barX = 14;
    const int barY = 50;
    const int barWidth = 100;
    const int barHeight = 8;
    const uint32_t animationTime = 5000;
    const uint32_t startTime = millis();

    while (millis() - startTime < animationTime) {
        uint32_t elapsed = millis() - startTime;
        int progress = (elapsed * 100) / animationTime;
        if (progress > 100) progress = 100;

        display.clearDisplay();
        display.setTextSize(3);
        display.setTextColor(SSD1306_WHITE);
        const char* title = "DMA";
        int16_t x1, y1;
        uint16_t w, h;
        display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
        display.setCursor((SCREEN_WIDTH - w) / 2, 5);
        display.println(title);

        display.setTextSize(1);
        const char* model = "DL301-26";
        display.getTextBounds(model, 0, 0, &x1, &y1, &w, &h);
        display.setCursor((SCREEN_WIDTH - w) / 2, 34);
        display.println(model);

        display.drawRoundRect(barX, barY, barWidth, barHeight, 2, SSD1306_WHITE);
        int fillWidth = ((barWidth - 4) * progress) / 100;
        if (fillWidth > 0) {
            display.fillRoundRect(barX + 2, barY + 2, fillWidth, barHeight - 4, 1, SSD1306_WHITE);
        }
        display.display();
        delay(20);
    }

    display.clearDisplay();
    display.setTextSize(3);
    const char* title = "DMA";
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 5);
    display.println(title);

    display.setTextSize(1);
    const char* model = "DL301-26";
    display.getTextBounds(model, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 34);
    display.println(model);

    display.drawRoundRect(barX, barY, barWidth, barHeight, 2, SSD1306_WHITE);
    display.fillRoundRect(barX + 2, barY + 2, barWidth - 4, barHeight - 4, 1, SSD1306_WHITE);
    display.display();
    delay(200);
}

void displayConfigPage() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    // Header - unchanged
    display.setTextSize(3);
    display.setCursor(43, 2);
    display.println("DMA");

    display.setTextSize(1);
    display.setCursor(40, 30);
    display.println("DL301-26");

    // Divider
    display.drawLine(10, 42, 118, 42, SSD1306_WHITE);

    // Bold CONFIG MODE
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(27, 49);
    display.print("CONFIG MODE");

    // Bold effect: print again with 1-pixel offset
    // display.setCursor(28, 49);
    // display.print("CONFIG MODE");

    display.display();
}

static void displayWelcomeScreen() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    int16_t x1, y1;
    uint16_t w, h;

    display.setTextSize(1);
    const char* welcome = "Welcome to";
    display.getTextBounds(welcome, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 6);
    display.println(welcome);

    const char* product = "DMA DataLogger";
    display.getTextBounds(product, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 22);
    display.println(product);

    const char* model = "Model: DL301-26";
    display.getTextBounds(model, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 38);
    display.println(model);

    const char* powered = "Powered by DMA";
    display.getTextBounds(powered, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 53);
    display.println(powered);

    display.display();
    delay(5000);
}

static void displayHomePage() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    const int LEFT  = 2;
    const int RIGHT = 125;
    const int DIV   = 63;

    // =========================================================
    // HEADER: Battery | Device Name | Signal
    // =========================================================
    drawBatteryIcon(displayData.batteryPercent);
    drawSignalBars(displayData.signalLevel);

    const char* title = "DMA_AWS";

    int16_t x1, y1;
    uint16_t w, h;

    display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 1);
    display.print(title);

    // Header separator
    display.drawLine(LEFT, 10, RIGHT, 10, SSD1306_WHITE);

    // =========================================================
    // DEVICE STATUS
    // =========================================================
    display.setCursor(LEFT, 13);
    display.print("Status: ");
    display.print(displayData.statusText);

    // Status separator
    display.drawLine(LEFT, 23, RIGHT, 23, SSD1306_WHITE);

    // =========================================================
    // TEMPERATURE | WIND SPEED
    // =========================================================
    char buffer[20];

    snprintf(buffer, sizeof(buffer), "Tmp:%.2f", displayData.airTemperature);
    display.setCursor(LEFT, 26);
    display.print(buffer);

    snprintf(buffer, sizeof(buffer), "Wsp:%.1f", displayData.windSpeed);
    display.setCursor(67, 26);
    display.print(buffer);

    // Middle vertical divider
    display.drawLine(DIV, 24, DIV, 34, SSD1306_WHITE);

    // Horizontal separator
    display.drawLine(LEFT, 35, RIGHT, 35, SSD1306_WHITE);

    // =========================================================
    // HUMIDITY | WIND DIRECTION
    // =========================================================
    snprintf(buffer, sizeof(buffer), "Hum:%.2f", displayData.airHumidity);
    display.setCursor(LEFT, 38);
    display.print(buffer);

    snprintf(buffer, sizeof(buffer), "Wdr:%.1f", displayData.windDirection);
    display.setCursor(67, 38);
    display.print(buffer);

    // Middle vertical divider
    display.drawLine(DIV, 36, DIV, 46, SSD1306_WHITE);

    // Horizontal separator
    display.drawLine(LEFT, 47, RIGHT, 47, SSD1306_WHITE);

    // =========================================================
    // PRESSURE | RAIN FALL
    // =========================================================
    snprintf(buffer, sizeof(buffer), "Prs:%.0f", displayData.airPressure);
    display.setCursor(LEFT, 50);
    display.print(buffer);

    snprintf(buffer, sizeof(buffer), "Rn:%.1f", displayData.rainFall);
    display.setCursor(67, 50);
    display.print(buffer);

    // Middle vertical divider
    display.drawLine(DIV, 48, DIV, 60, SSD1306_WHITE);

    // Bottom separator
    display.drawLine(LEFT, 61, RIGHT, 61, SSD1306_WHITE);

    display.display();
}


// ============================================================
// Public functions
// ============================================================
void Display_setup() {

    pinMode(DISPLAY_PIN, INPUT);

    SPI.begin(OLED_CLK, -1, OLED_MOSI, OLED_CS);
    if (!display.begin(SSD1306_SWITCHCAPVCC)) {
        Serial.println("SSD1306 initialization failed!");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    displayBootAnimation();
    // displayWelcomeScreen(); // optional
    displayHomePage();

    displayCommandQueue = xQueueCreate(DISPLAY_CMD_QUEUE_SIZE, sizeof(DisplayCommandType));
    if (displayCommandQueue == NULL) {
        Serial.println("Display queue creation failed!");
        return;
    }

    xTaskCreatePinnedToCore(
        displayTask,
        "DisplayTask",
        DISPLAY_TASK_STACK,
        NULL,
        DISPLAY_TASK_PRIORITY,
        &displayTaskHandle,
        0
    );
}

void sendDisplayCommand(DisplayCommandType type) {
    if (displayCommandQueue == NULL) return;
    xQueueSend(displayCommandQueue, &type, pdMS_TO_TICKS(10));
}

void updateAirPressure(float value) {
    displayData.airPressure = value;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void updateAirTemperature(float value) {
    displayData.airTemperature = value;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void updateBatteryPercentage(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    displayData.batteryPercent = percent;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void updateAirHumidity(float value) {
    displayData.airHumidity = value;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void updateWindSpeed(float value) {
    displayData.windSpeed = value;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void updateWindDirection(float value) {
    displayData.windDirection = value;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void updateRainFall(float value) {
    displayData.rainFall = value;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void updateSignalStrength(int level) {
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    displayData.signalLevel = level;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void updateStatus(const char* status) {
    displayData.statusText = status;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void updateAllDisplayValues(float pressure, float temperature, float humidity, float w_speed, float w_direction, float rainfall, int signal, const char* status) {

    displayData.airPressure = pressure;
    displayData.airTemperature = temperature;
    displayData.airHumidity = humidity;
    displayData.windSpeed = w_speed;
    displayData.windDirection = w_direction;
    displayData.rainFall = rainfall;

    if (signal < 0) signal = 0;
    if (signal > 5) signal = 5;
    displayData.signalLevel = signal;

    displayData.statusText = status;
    sendDisplayCommand(DISPLAY_UPDATE_HOME);
}

void displaySleep() {
    sendDisplayCommand(DISPLAY_SLEEP);
}

void displayWake() {
    sendDisplayCommand(DISPLAY_WAKE);
}

void displaySetBrightness(uint8_t brightness) {
    // Directly set contrast (0 = dimmest, 255 = brightest)
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(brightness);
}

// ============================================================
// Display task
// ============================================================
void displayTask(void* parameter) {
    DisplayCommandType cmd;
    const TickType_t checkInterval = pdMS_TO_TICKS(100);
    unsigned long wakeTimeout = 0;  // 0 = display sleeping

    Serial.println("Display Task started");

    while (1) {
        if (xQueueReceive(displayCommandQueue, &cmd, checkInterval) == pdTRUE) {
            switch (cmd) {
                case DISPLAY_BOOT:
                    // Turn on display and set timeout
                    display.ssd1306_command(SSD1306_DISPLAYON);
                    wakeTimeout = millis() + DISPLAY_WAKE_TIMEOUT_MS;
                    displayBootAnimation();
                    break;

                case DISPLAY_CONFIG:
                    display.ssd1306_command(SSD1306_DISPLAYON);
                    wakeTimeout = millis() + DISPLAY_WAKE_TIMEOUT_MS;
                    displayBootAnimation();
                    break;

                case DISPLAY_SLEEP:
                    display.ssd1306_command(SSD1306_DISPLAYOFF);
                    wakeTimeout = 0;
                    break;

                case DISPLAY_WAKE:
                    display.ssd1306_command(SSD1306_DISPLAYON);
                    wakeTimeout = millis() + DISPLAY_WAKE_TIMEOUT_MS;
                    break;

                case DISPLAY_HOME:
                case DISPLAY_UPDATE_HOME:
                    // Wake display if not already awake, then reset timeout
                    // display.ssd1306_command(SSD1306_DISPLAYON);
                    wakeTimeout = millis() + DISPLAY_WAKE_TIMEOUT_MS;
                    displayHomePage();
                    break;

                case DISPLAY_CLEAR:
                    display.clearDisplay();
                    display.display();
                    break;

                default:
                    break;
            }
        }

        // Auto-sleep check
        if (wakeTimeout != 0 && millis() > wakeTimeout) {
            display.ssd1306_command(SSD1306_DISPLAYOFF);
            wakeTimeout = 0;
        }

// ===== TOUCH DETECTION =====
bool touchDetected = (digitalRead(DISPLAY_PIN) == HIGH);

// 1. Touch starts
if (touchDetected && !buttonState) {
    buttonState = true;
    touchStartTime = millis();
    longPressTriggered = false;

    Serial.println("Touch detected - waking display");

    // Wake display immediately because we are already in displayTask
    display.ssd1306_command(SSD1306_DISPLAYON);
    wakeTimeout = millis() + DISPLAY_WAKE_TIMEOUT_MS;
}

// 2. Touch is being held
if (touchDetected &&
    buttonState &&
    !longPressTriggered &&
    (millis() - touchStartTime >= LONG_PRESS_TIME)) {

    longPressTriggered = true;

    Serial.println("Long press detected! Opening configuration...");
    displayConfigPage();
    dev_config();

    // Refresh display timeout after returning, if desired
    wakeTimeout = millis() + DISPLAY_WAKE_TIMEOUT_MS;
}

// 3. Touch released
if (!touchDetected && buttonState) {
    buttonState = false;

    Serial.println("Touch released");
}

    }
}

int rssiToSignalLevel(int rssi) {
    if (rssi < 10) return 1;
    if (rssi > 30) return 5;

    // Map 10..30 → 2..5
    // return map(rssi, 10, 30, 2, 5);
    return 2 + ((rssi - 10) * 3) / 20;
}

