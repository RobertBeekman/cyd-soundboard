/*
 * CYD Soundboard - ESP32 Cheap Yellow Display File Browser
 *
 * This project creates a file browser interface on the ESP32-2432S028R (CYD)
 * using LVGL for the UI, with support for display, touch, and SD card.
 *
 * Hardware used:
 * - ESP32-2432S028R (Cheap Yellow Display)
 * - 320x240 TFT display with ILI9341 driver
 * - XPT2046 resistive touch controller
 * - MicroSD card slot
 */

#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <SD.h>
#include <FS.h>

// Pin definitions for CYD hardware
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
#define SD_CS 5

// Display configuration
#define TFT_HOR_RES   320
#define TFT_VER_RES   240
#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))

// Touch screen setup using software SPI to avoid conflicts
XPT2046_Bitbang touchscreen(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);

// Touch calibration values (determined from actual hardware testing)
uint16_t touchScreenMinimumX = 21, touchScreenMaximumX = 295;
uint16_t touchScreenMinimumY = 20, touchScreenMaximumY = 219;

// LVGL variables
lv_indev_t *indev;        // Touch input device
uint8_t *draw_buf;        // Display buffer
uint32_t lastTick = 0;    // Timer for LVGL

// File browser variables
lv_obj_t * file_list;     // Container for file buttons
String fileNames[50];     // Array to store file names
int fileCount = 0;        // Number of files found

#if LV_USE_LOG != 0
void my_print( lv_log_level_t level, const char * buf )
{
    LV_UNUSED(level);
    Serial.println(buf);
    Serial.flush();
}
#endif

/* LVGL display flush callback - required but handled by TFT_eSPI integration */
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    lv_disp_flush_ready(disp);
}

/* Read touch input and convert to screen coordinates */
void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
    TouchPoint p = touchscreen.getTouch();

    if (p.zRaw > 0) {  // Touch detected
        // Map raw touch coordinates to screen pixels
        // Note: Coordinates are inverted to match upside-down display
        data->point.x = map(p.x, touchScreenMinimumX, touchScreenMaximumX, TFT_HOR_RES, 1);
        data->point.y = map(p.y, touchScreenMinimumY, touchScreenMaximumY, TFT_VER_RES, 1);
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* Scan SD card root directory and populate file list */
void scanSDCard() {
    fileCount = 0;

    // Initialize SD card on VSPI bus
    SPIClass spi = SPIClass(VSPI);
    if (!SD.begin(SD_CS, spi, 80000000)) {
        Serial.println("SD Card initialization failed!");
        fileNames[0] = "SD Card Error";
        fileCount = 1;
        return;
    }

    Serial.println("SD Card initialized successfully");

    // Check if card is present
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        fileNames[0] = "No SD Card";
        fileCount = 1;
        return;
    }

    // Open root directory and scan for files
    File root = SD.open("/");
    if (!root) {
        Serial.println("Failed to open root directory");
        fileNames[0] = "Directory Error";
        fileCount = 1;
        return;
    }

    // Read all files in root directory
    File file = root.openNextFile();
    while (file && fileCount < 50) {
        if (!file.isDirectory()) {
            fileNames[fileCount] = String(file.name());
            Serial.println("Found file: " + fileNames[fileCount]);
            fileCount++;
        }
        file = root.openNextFile();
    }

    if (fileCount == 0) {
        fileNames[0] = "No files found";
        fileCount = 1;
    }

    root.close();
}

/* Handle button clicks on file list items */
static void file_list_event_handler(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target_obj(e);

    if (code == LV_EVENT_CLICKED) {
        // Get filename from button label
        lv_obj_t * label = lv_obj_get_child(obj, 0);
        const char * txt = lv_label_get_text(label);
        Serial.println("Selected file: " + String(txt));

        // TODO: Add file handling logic here (play audio, etc.)
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("CYD Soundboard starting...");

    // Initialize touch screen (uses software SPI)
    touchscreen.begin();

    // Initialize LVGL graphics library
    lv_init();
    draw_buf = new uint8_t[DRAW_BUF_SIZE];
    lv_display_t *disp = lv_tft_espi_create(TFT_HOR_RES, TFT_VER_RES, draw_buf, DRAW_BUF_SIZE);

    // Setup touch input device
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    // Scan SD card for files
    scanSDCard();

    // Create scrollable file list container
    file_list = lv_obj_create(lv_screen_active());
    lv_obj_set_size(file_list, 300, 220);
    lv_obj_center(file_list);

    // Configure scrolling behavior
    lv_obj_set_scroll_dir(file_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(file_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(file_list, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // Arrange items vertically
    lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(file_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Create a button for each file found
    for (int i = 0; i < fileCount; i++) {
        lv_obj_t * btn = lv_button_create(file_list);
        lv_obj_set_size(btn, 280, 40);
        lv_obj_add_event_cb(btn, file_list_event_handler, LV_EVENT_ALL, nullptr);

        // Add file icon and name to button
        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text_fmt(label, LV_SYMBOL_FILE " %s", fileNames[i].c_str());
        lv_obj_center(label);
    }

    Serial.println("Setup complete!");
}

void loop() {
    // Update LVGL timing and process UI events
    lv_tick_inc(millis() - lastTick);
    lastTick = millis();
    lv_timer_handler();
    delay(5);
}