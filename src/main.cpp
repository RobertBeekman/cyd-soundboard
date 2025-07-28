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
#include <XPT2046_Bitbang.h>
#include <SD.h>
#include <FS.h>
#include <vector>
#include "CYD28_audio.h"

// Pin definitions for CYD hardware
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
#define SD_CS 5

// Configuration file name
#define CONFIG_FILE "/soundboard.conf"

// Display configuration
#define TFT_HOR_RES   320
#define TFT_VER_RES   240
#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))

// Structure to hold button configuration
struct ButtonConfig {
    String filename;
    String label;
    String color;
    int order = 0;
    bool found = false;  // Whether the MP3 file was found on SD card
};

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
// Audio volume slider components
static lv_obj_t * volSlider;
static lv_obj_t * volSlider_label;
static void volSlider_event_cb(lv_event_t * e);

std::vector<ButtonConfig> buttonConfigs;  // Configured buttons
std::vector<String> unconfiguredFiles;    // MP3 files not in config

// Global SD card initialization flag
bool sdCardInitialized = false;
SPIClass sdSPI = SPIClass(VSPI);

// Audio player - now using CYD28_audio system
bool audioInitialized = false;
String currentlyPlaying = "";

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

/* Convert color name to LVGL color */
lv_color_t getColorFromName(const String& colorName) {
    // Check if it's a hex color (starts with # or 0x)
    if (colorName.startsWith("#")) {
        // Parse hex color starting with #
        String hexStr = colorName.substring(1);
        if (hexStr.length() == 6) {
            long hexValue = strtol(hexStr.c_str(), nullptr, 16);
            return lv_color_hex(hexValue);
        }
    } else if (colorName.startsWith("0x") || colorName.startsWith("0X")) {
        // Parse hex color starting with 0x
        String hexStr = colorName.substring(2);
        if (hexStr.length() == 6) {
            long hexValue = strtol(hexStr.c_str(), nullptr, 16);
            return lv_color_hex(hexValue);
        }
    }

    // Check named colors
    if (colorName == "red") return lv_color_hex(0xFF0000);
    if (colorName == "green") return lv_color_hex(0x00FF00);
    if (colorName == "blue") return lv_color_hex(0x0000FF);
    if (colorName == "yellow") return lv_color_hex(0xFFFF00);
    if (colorName == "orange") return lv_color_hex(0xFF8000);
    if (colorName == "purple") return lv_color_hex(0x800080);
    if (colorName == "pink") return lv_color_hex(0xFF69B4);
    if (colorName == "cyan") return lv_color_hex(0x00FFFF);
    if (colorName == "lime") return lv_color_hex(0x32CD32);
    if (colorName == "magenta") return lv_color_hex(0xFF00FF);
    if (colorName == "brown") return lv_color_hex(0x8B4513);
    if (colorName == "gray") return lv_color_hex(0x808080);
    if (colorName == "white") return lv_color_hex(0xFFFFFF);
    if (colorName == "black") return lv_color_hex(0x000000);

    // Default color if not recognized
    return lv_color_hex(0x2196F3);  // Material blue
}

/* Determine if text should be white or black based on background color brightness */
bool shouldUseWhiteText(const String& colorName) {
    // For simplicity, use a predefined list of dark colors that need white text
    // This avoids LVGL version compatibility issues with color extraction

    if (colorName == "black" || colorName == "brown" || colorName == "purple" ||
        colorName == "blue" || colorName == "red" || colorName == "green") {
        return true;  // Use white text on dark backgrounds
    }

    // Check if it's a dark hex color (rough approximation)
    if (colorName.startsWith("#") || colorName.startsWith("0x") || colorName.startsWith("0X")) {
        String hexStr = colorName;
        if (hexStr.startsWith("#")) hexStr = hexStr.substring(1);
        else if (hexStr.startsWith("0x") || hexStr.startsWith("0X")) hexStr = hexStr.substring(2);

        if (hexStr.length() == 6) {
            long hexValue = strtol(hexStr.c_str(), nullptr, 16);
            // Simple brightness check: if all RGB components are below 128, use white text
            uint8_t r = (hexValue >> 16) & 0xFF;
            uint8_t g = (hexValue >> 8) & 0xFF;
            uint8_t b = hexValue & 0xFF;

            float brightness = (static_cast<float>(r) + static_cast<float>(g) + static_cast<float>(b)) / 3.0f;
            return brightness < 128.0f;
        }
    }

    // Default to black text for light colors
    return false;
}

/* Initialize SD card once and keep it available */
bool initializeSDCard() {
    if (sdCardInitialized) {
        return true; // Already initialized
    }

    // Try to initialize SD card multiple times
    for (int attempt = 0; attempt < 3; attempt++) {
        if (SD.begin(SD_CS, sdSPI, 80000000)) {
            sdCardInitialized = true;
            Serial.println("SD Card initialized successfully");
            return true;
        }
        Serial.println("SD Card initialization attempt " + String(attempt + 1) + " failed, retrying...");
        delay(500);
    }

    Serial.println("SD Card initialization failed after multiple attempts!");
    return false;
}

/* Read and parse configuration file */
void readConfigFile() {
    buttonConfigs.clear();

    // Initialize SD card if not already done
    if (!initializeSDCard()) {
        Serial.println("SD Card not available for config reading");
        return;
    }

    File configFile = SD.open(CONFIG_FILE);
    if (!configFile) {
        Serial.println("Configuration file not found, using default settings");
        return;
    }

    Serial.println("Reading configuration file...");
    int order = 0;

    while (configFile.available()) {
        String line = configFile.readStringUntil('\n');
        line.trim();

        // Skip empty lines and comments
        if (line.length() == 0 || line.startsWith("#")) {
            continue;
        }

        // Parse format: filename|label|color
        int firstPipe = line.indexOf('|');
        int secondPipe = line.indexOf('|', firstPipe + 1);

        if (firstPipe > 0 && secondPipe > firstPipe) {
            ButtonConfig config;
            config.filename = line.substring(0, firstPipe);
            config.label = line.substring(firstPipe + 1, secondPipe);
            config.color = line.substring(secondPipe + 1);
            config.order = order++;
            config.found = false;

            buttonConfigs.push_back(config);
            Serial.println("Config: " + config.filename + " -> " + config.label + " (" + config.color + ")");
        }
    }

    configFile.close();
    Serial.println("Configuration loaded: " + String(buttonConfigs.size()) + " entries");
}

/* Scan SD card root directory and populate file list */
void scanSDCard() {
    unconfiguredFiles.clear();

    // Use already initialized SD card
    if (!sdCardInitialized) {
        Serial.println("SD Card not initialized!");
        unconfiguredFiles.emplace_back("SD Card Error");
        return;
    }

    // Check if card is present
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        unconfiguredFiles.emplace_back("No SD Card");
        return;
    }

    Serial.println("Scanning SD card for MP3 files...");

    // Mark configured files as found and collect unconfigured MP3 files
    for (auto& config : buttonConfigs) {
        // Add delay between file checks to avoid rapid SD access
        // delay(50);

        if (SD.exists("/" + config.filename)) {
            config.found = true;
            Serial.println("Found configured file: " + config.filename);
        } else {
            Serial.println("Configured file not found: " + config.filename);
        }
    }

    // Open root directory and scan for MP3 files
    File root = SD.open("/");
    if (!root) {
        Serial.println("Failed to open root directory");
        unconfiguredFiles.emplace_back("Directory Error");
        return;
    }

    // Read all MP3 files in root directory
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String fileName = String(file.name());

            // Only process MP3 files
            if (fileName.endsWith(".mp3") || fileName.endsWith(".MP3")) {
                bool isConfigured = false;

                // Check if file is in the configuration
                for (const auto& config : buttonConfigs) {
                    if (config.filename == fileName) {
                        isConfigured = true;
                        break;
                    }
                }

                if (!isConfigured) {
                    unconfiguredFiles.emplace_back(fileName);
                    Serial.println("Found unconfigured MP3 file: " + fileName);
                }
            }
        }

        file.close(); // Properly close each file
        delay(10); // Small delay between file operations
        file = root.openNextFile();
    }

    root.close();

    Serial.println("SD scan complete. Found " + String(buttonConfigs.size()) + " configured files, " +
                   String(unconfiguredFiles.size()) + " unconfigured MP3 files");
}

/* Initialize audio system */
bool initializeAudio() {
    if (audioInitialized) {
        return true;
    }

    // Initialize the CYD28_audio system
    audioInit();
    audioInitialized = true;
    Serial.println("Audio system initialized successfully");
    return true;
}

/* Play MP3 file from SD card */
void playMP3File(const String& filename) {
    if (!audioInitialized) {
        if (!initializeAudio()) {
            Serial.println("Cannot play audio - initialization failed");
            return;
        }
    }

    // Stop current playback if any
    if (audioIsPlaying()) {
        audioStopSong();
        Serial.println("Stopped current playback");
    }

    // Construct full path
    String fullPath = "/" + filename;

    // Play the selected file using CYD28_audio
    if (audioConnecttoSD(fullPath.c_str())) {
        currentlyPlaying = filename;
        Serial.println("Now playing: " + filename);
    } else {
        Serial.println("Failed to play: " + filename);
        currentlyPlaying = "";
    }
}

/* Stop audio playback */
void stopAudio() {
    if (audioInitialized && audioIsPlaying()) {
        audioStopSong();
        currentlyPlaying = "";
        Serial.println("Audio playback stopped");
    }
}

/* Handle button clicks on file list items */
static void file_list_event_handler(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target_obj(e);

    if (code == LV_EVENT_CLICKED) {
        // Get filename from user data
        const char* filename = (const char*)lv_obj_get_user_data(obj);
        if (filename) {
            Serial.println("Selected file: " + String(filename));
            playMP3File(String(filename));  // Play the selected MP3 file
        }
    }
}

/* Volume slider event handler */
static void volSlider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        // Get the new slider value (0-21 range)
        int16_t value = lv_slider_get_value(volSlider);

        // Set audio volume directly (assuming audioSetVolume expects 0-21 range)
        audioSetVolume(value);

        // Update volume label text
        lv_label_set_text_fmt(volSlider_label, "Volume: %d/21", value);

        Serial.println("Volume set to: " + String(value) + "/21");
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

    // Read configuration file
    readConfigFile();

    // Scan SD card for files
    scanSDCard();

    // Initialize audio system
    initializeAudio();

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

    // Create buttons for configured files that were found (in config order)
    for (const auto& config : buttonConfigs) {
        if (config.found) {
            lv_obj_t * btn = lv_button_create(file_list);
            lv_obj_set_size(btn, 280, 40);
            lv_obj_add_event_cb(btn, file_list_event_handler, LV_EVENT_ALL, nullptr);

            // Set button color based on configuration
            lv_color_t btnColor = getColorFromName(config.color);
            lv_obj_set_style_bg_color(btn, btnColor, LV_PART_MAIN);

            // Store filename in user data for event handler
            lv_obj_set_user_data(btn, (void*)config.filename.c_str());

            // Add label with custom text
            lv_obj_t * label = lv_label_create(btn);
            lv_label_set_text(label, config.label.c_str());
            lv_obj_center(label);

            // Set text color based on background brightness
            lv_color_t textColor = shouldUseWhiteText(config.color) ?
                lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);
            lv_obj_set_style_text_color(label, textColor, LV_PART_MAIN);
        }
    }

    // Create buttons for unconfigured MP3 files with default styling
    for (const auto& fileName : unconfiguredFiles) {
        lv_obj_t * btn = lv_button_create(file_list);
        lv_obj_set_size(btn, 280, 40);
        lv_obj_add_event_cb(btn, file_list_event_handler, LV_EVENT_ALL, nullptr);

        // Set default color
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x808080), LV_PART_MAIN);  // Gray

        // Store filename in user data for event handler
        lv_obj_set_user_data(btn, (void*)fileName.c_str());

        // Use filename as label (remove .mp3 extension for cleaner look)
        lv_obj_t * label = lv_label_create(btn);
        String displayName = fileName;
        if (displayName.endsWith(".mp3") || displayName.endsWith(".MP3")) {
            displayName = displayName.substring(0, displayName.length() - 4);
        }
        lv_label_set_text(label, displayName.c_str());
        lv_obj_center(label);

        // White text on gray background
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    }

    // Create volume slider
    volSlider = lv_slider_create(lv_scr_act());
    lv_obj_set_size(volSlider, 200, 20);
    lv_obj_align(volSlider, LV_ALIGN_TOP_MID, 0, 10);
    lv_slider_set_range(volSlider, 0, 21);
    lv_slider_set_value(volSlider, 10, LV_ANIM_OFF);  // Default to 50%
    lv_obj_add_event_cb(volSlider, volSlider_event_cb, LV_EVENT_ALL, nullptr);

    // Create volume label
    volSlider_label = lv_label_create(lv_scr_act());
    lv_label_set_text(volSlider_label, "Volume: 10/21");
    lv_obj_align(volSlider_label, LV_ALIGN_TOP_MID, 0, 40);

    Serial.println("Setup complete!");
}

void loop() {
    // Update LVGL timing and process UI events
    lv_tick_inc(millis() - lastTick);
    lastTick = millis();
    lv_timer_handler();

    // No need to process audio manually - CYD28_audio handles it in its own task
    delay(5);
}
