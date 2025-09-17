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
#include <TFT_eSPI.h>

// Pin definitions for CYD hardware
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33
#define SD_CS 5
#define TFT_BL 21      // Display backlight control pin

// Configuration file name
#define CONFIG_FILE "/soundboard.conf"

// Display configuration
#define TFT_HOR_RES   320
#define TFT_VER_RES   240
// Reduce buffer size for better performance - use smaller buffer
#define DRAW_BUF_SIZE (TFT_HOR_RES * 60) // Much smaller buffer for better performance

// Grid configuration - adjust these for performance tuning
#define DEFAULT_GRID_COLS 4        // Default number of columns per grid
#define DEFAULT_GRID_ROWS 3        // Default number of rows per grid
#define BUTTON_GAP 1       // Gap between buttons in pixels
#define GRID_GAP 2         // Gap between grids in pixels

// Performance optimization settings
#define SCROLL_THROW_SLOW 15    // Slower scroll deceleration for smoother performance
#define SCROLL_MOMENTUM_REDUCE 90 // Reduce momentum for better control

// Default settings
#define DEFAULT_VOLUME 12           // Default volume if not specified in config file
#define DEFAULT_SCREEN_TIMEOUT 30   // Default screen timeout in seconds
#define DEFAULT_FONT_SIZE 14        // Default font size for button text

// Structure to hold button configuration
struct ButtonConfig {
    String filename;
    String label;
    String color;
    int order = 0;
    bool found = false;  // Whether the MP3 file was found on SD card
    int fontSize = 0;    // Font size override (0 = use global setting)
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

std::vector<ButtonConfig> buttonConfigs;  // Configured buttons
std::vector<String> unconfiguredFiles;    // MP3 files not in config

// Visual feedback variables
lv_obj_t * currentlyPlayingButton = nullptr;  // Track which button is currently playing
lv_color_t originalButtonColor;               // Store original color to restore later

// Global configuration variables
int configuredVolume = DEFAULT_VOLUME;        // Volume setting from config file
int configuredScreenTimeout = DEFAULT_SCREEN_TIMEOUT; // Screen timeout in seconds
int configuredGridCols = DEFAULT_GRID_COLS;   // Number of columns per grid
int configuredGridRows = DEFAULT_GRID_ROWS;   // Number of rows per grid
int configuredGridButtonsMax = DEFAULT_GRID_COLS * DEFAULT_GRID_ROWS; // Maximum buttons per grid
int configuredFontSize = DEFAULT_FONT_SIZE;   // Font size for button text

// Screen timeout variables
unsigned long lastTouchTime = 0;     // Last time screen was touched
bool screenOn = true;                // Current screen state
bool touchWakeupMode = false;        // True when screen is off and waiting for wake touch
bool ignoreUntilRelease = false;     // True when we should ignore touches until finger is lifted

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
        // Update last touch time
        lastTouchTime = millis();

        // If screen is off (wake-up mode), turn it on and start ignoring touches until release
        if (touchWakeupMode) {
            screenOn = true;
            touchWakeupMode = false;
            ignoreUntilRelease = true;  // Start ignoring all touches until finger is lifted
            digitalWrite(TFT_BL, HIGH); // Turn on backlight
            Serial.println("Screen woken up by touch - ignoring input until release");
        }

        // If we're in ignore mode, don't process any touches
        if (ignoreUntilRelease) {
            data->state = LV_INDEV_STATE_RELEASED;
            data->point.x = 0;
            data->point.y = 0;
            return;
        }

        // Map raw touch coordinates to screen pixels
        // Note: Coordinates are inverted to match upside-down display
        data->point.x = map(p.x, touchScreenMinimumX, touchScreenMaximumX, TFT_HOR_RES, 1);
        data->point.y = map(p.y, touchScreenMinimumY, touchScreenMaximumY, TFT_VER_RES, 1);
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        // No touch detected - finger released
        if (ignoreUntilRelease) {
            ignoreUntilRelease = false;  // Stop ignoring touches now that finger is lifted
            Serial.println("Touch released - accepting input again");
        }
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* Convert hex color to LVGL color */
lv_color_t getColorFromHex(const String& colorHex) {
    // Check if it's a hex color (starts with #)
    if (colorHex.startsWith("#")) {
        // Parse hex color starting with #
        String hexStr = colorHex.substring(1);
        if (hexStr.length() == 6) {
            long hexValue = strtol(hexStr.c_str(), nullptr, 16);
            return lv_color_hex(hexValue);
        }
    }

    // Default color if not recognized
    return lv_color_hex(0x2196F3);  // Material blue
}

/* Determine if text should be white or black based on background color brightness */
bool shouldUseWhiteText(const String& colorHex) {
    // Check if it's a hex color starting with #
    if (colorHex.startsWith("#")) {
        String hexStr = colorHex.substring(1);

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

    // Default to black text for unrecognized colors
    return false;
}

/* Set font based on size - helper function to avoid code duplication */
void setFontBySize(lv_obj_t* label, int fontSize) {
    if (fontSize <= 12) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
    } else if (fontSize <= 14) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    } else if (fontSize <= 16) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    } else if (fontSize <= 18) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_PART_MAIN);
    } else if (fontSize <= 20) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    } else if (fontSize <= 22) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_22, LV_PART_MAIN);
    } else if (fontSize <= 24) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, LV_PART_MAIN);
    } else if (fontSize <= 26) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_26, LV_PART_MAIN);
    } else if (fontSize <= 28) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_32, LV_PART_MAIN);
    }
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
    configuredVolume = DEFAULT_VOLUME; // Reset to default
    configuredScreenTimeout = DEFAULT_SCREEN_TIMEOUT; // Reset to default
    configuredGridCols = DEFAULT_GRID_COLS; // Reset to default
    configuredGridRows = DEFAULT_GRID_ROWS; // Reset to default
    configuredFontSize = DEFAULT_FONT_SIZE; // Reset to default

    // Initialize SD card if not already done
    if (!initializeSDCard()) {
        Serial.println("SD Card not available for config reading");
        return;
    }

    File configFile = SD.open(CONFIG_FILE);
    if (!configFile) {
        Serial.println("Configuration file not found, using default settings");
        configuredGridButtonsMax = configuredGridCols * configuredGridRows; // Calculate with defaults
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

        // Check for volume setting (format: VOLUME=15)
        if (line.startsWith("VOLUME=")) {
            int volume = line.substring(7).toInt();
            if (volume >= 0 && volume <= 21) {
                configuredVolume = volume;
                Serial.println("Volume configured to: " + String(volume) + "/21");
            } else {
                Serial.println("Invalid volume value: " + String(volume) + ", using default");
            }
            continue;
        }

        // Check for screen timeout setting (format: TIMEOUT=30)
        if (line.startsWith("TIMEOUT=")) {
            int timeout = line.substring(8).toInt();
            if (timeout >= 1 && timeout <= 3600) { // Reasonable range for timeout
                configuredScreenTimeout = timeout;
                Serial.println("Screen timeout configured to: " + String(timeout) + " seconds");
            } else {
                Serial.println("Invalid screen timeout value: " + String(timeout) + ", using default");
            }
            continue;
        }

        // Check for grid columns setting (format: GRID_COLS=4)
        if (line.startsWith("GRID_COLS=")) {
            int cols = line.substring(10).toInt();
            if (cols >= 1 && cols <= 10) { // Reasonable range for columns
                configuredGridCols = cols;
                Serial.println("Grid columns configured to: " + String(cols));
            } else {
                Serial.println("Invalid grid columns value: " + String(cols) + ", using default");
            }
            continue;
        }

        // Check for grid rows setting (format: GRID_ROWS=3)
        if (line.startsWith("GRID_ROWS=")) {
            int rows = line.substring(10).toInt();
            if (rows >= 1 && rows <= 10) { // Reasonable range for rows
                configuredGridRows = rows;
                Serial.println("Grid rows configured to: " + String(rows));
            } else {
                Serial.println("Invalid grid rows value: " + String(rows) + ", using default");
            }
            continue;
        }

        // Check for font size setting (format: FONT_SIZE=14)
        if (line.startsWith("FONT_SIZE=")) {
            int fontSize = line.substring(10).toInt();
            if (fontSize >= 8 && fontSize <= 32) { // Reasonable range for font size
                configuredFontSize = fontSize;
                Serial.println("Font size configured to: " + String(fontSize));
            } else {
                Serial.println("Invalid font size value: " + String(fontSize) + ", using default");
            }
            continue;
        }

        // Parse button format: filename|label|color|fontSize (fontSize is optional)
        int firstPipe = line.indexOf('|');
        int secondPipe = line.indexOf('|', firstPipe + 1);
        int thirdPipe = line.indexOf('|', secondPipe + 1);

        if (firstPipe > 0 && secondPipe > firstPipe) {
            ButtonConfig config;
            config.filename = line.substring(0, firstPipe);
            config.label = line.substring(firstPipe + 1, secondPipe);
            config.color = line.substring(secondPipe + 1, thirdPipe > secondPipe ? thirdPipe : line.length());
            config.order = order++;
            config.found = false;
            config.fontSize = 0; // Default to use global setting

            // Check if there's a fourth field for font size
            if (thirdPipe > secondPipe) {
                int fontSize = line.substring(thirdPipe + 1).toInt();
                if (fontSize >= 8 && fontSize <= 32) {
                    config.fontSize = fontSize;
                    Serial.println("Config: " + config.filename + " -> " + config.label + " (" + config.color + ", font: " + String(fontSize) + ")");
                } else {
                    Serial.println("Config: " + config.filename + " -> " + config.label + " (" + config.color + ", invalid font: " + String(fontSize) + ")");
                }
            } else {
                Serial.println("Config: " + config.filename + " -> " + config.label + " (" + config.color + ")");
            }

            buttonConfigs.push_back(config);
        }
    }

    configFile.close();

    // Calculate max buttons per grid after reading configuration
    configuredGridButtonsMax = configuredGridCols * configuredGridRows;

    Serial.println("Configuration loaded: " + String(buttonConfigs.size()) + " entries, Volume: " + String(configuredVolume) + "/21, Timeout: " + String(configuredScreenTimeout) + "s, Grid: " + String(configuredGridCols) + "x" + String(configuredGridRows) + ", Font: " + String(configuredFontSize));
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
        // delay(10); // Small delay between file operations
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

/* Set button to playing state (red color) */
void setButtonPlaying(lv_obj_t* btn) {
    if (btn == nullptr) return;

    // Store the original color before changing to red
    originalButtonColor = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);

    // Set button to red to indicate playing
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF0000), LV_PART_MAIN); // Bright red

    // Set text to white for better contrast on red
    lv_obj_t* label = lv_obj_get_child(btn, 0);
    if (label) {
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    }

    currentlyPlayingButton = btn;
}

/* Restore button to original state */
void restoreButtonColor(lv_obj_t* btn) {
    if (btn == nullptr) return;

    // Restore original background color
    lv_obj_set_style_bg_color(btn, originalButtonColor, LV_PART_MAIN);

    // Find the original button configuration to restore proper text color
    const char* filename = (const char*)lv_obj_get_user_data(btn);
    if (filename) {
        // Look for the button configuration
        for (const auto& config : buttonConfigs) {
            if (config.filename == String(filename)) {
                // Restore proper text color based on background brightness
                lv_color_t textColor = shouldUseWhiteText(config.color) ?
                    lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);

                lv_obj_t* label = lv_obj_get_child(btn, 0);
                if (label) {
                    lv_obj_set_style_text_color(label, textColor, LV_PART_MAIN);
                }
                return;
            }
        }

        // If not found in config (unconfigured file), use white text on gray
        lv_obj_t* label = lv_obj_get_child(btn, 0);
        if (label) {
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        }
    }
}

/* Reset all button colors (call when audio stops) */
void resetAllButtonColors() {
    if (currentlyPlayingButton != nullptr) {
        restoreButtonColor(currentlyPlayingButton);
        currentlyPlayingButton = nullptr;
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
            // Check if this is the currently playing button
            if (currentlyPlayingButton == obj && audioIsPlaying()) {
                // Stop playback if clicking the same button that's currently playing
                Serial.println("Stopping playback: " + String(filename));
                stopAudio();
                resetAllButtonColors();
            } else {
                // Reset any previously playing button
                resetAllButtonColors();

                Serial.println("Selected file: " + String(filename));

                // Set this button to playing state (red)
                setButtonPlaying(obj);

                playMP3File(String(filename));  // Play the selected MP3 file
            }
        }
    }
}

/* Create a configurable grid of buttons within a container */
lv_obj_t* create_button_grid(lv_obj_t* parent, const std::vector<ButtonConfig>& configs, const std::vector<String>& unconfigured, int start_index) {
    // Create grid container with full screen size - no margins
    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_size(grid, TFT_HOR_RES, TFT_VER_RES); // Use full screen size without any margins
    lv_obj_set_style_pad_all(grid, 0, 0); // Remove all padding around the grid
    lv_obj_set_style_pad_gap(grid, BUTTON_GAP, 0); // Keep small gap between buttons
    lv_obj_set_style_border_width(grid, 0, 0); // Remove grid border
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0); // Make grid background transparent
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE); // Grid itself shouldn't scroll

    // Dynamically create grid descriptors based on configuration
    // Allocate arrays with maximum possible size (10x10 as per validation limits)
    static int32_t col_dsc[11]; // Max 10 columns + terminator
    static int32_t row_dsc[11]; // Max 10 rows + terminator

    // Fill column descriptors using configured grid columns
    for (int i = 0; i < configuredGridCols; i++) {
        col_dsc[i] = LV_GRID_FR(1);
    }
    col_dsc[configuredGridCols] = LV_GRID_TEMPLATE_LAST;

    // Fill row descriptors using configured grid rows
    for (int i = 0; i < configuredGridRows; i++) {
        row_dsc[i] = LV_GRID_FR(1);
    }
    row_dsc[configuredGridRows] = LV_GRID_TEMPLATE_LAST;

    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    // Combine configured and unconfigured files
    std::vector<std::pair<String, String>> all_files; // filename, label pairs

    // Add configured files first (in order)
    for (const auto& config : configs) {
        if (config.found) {
            all_files.emplace_back(config.filename, config.label);
        }
    }

    // Add unconfigured files
    for (const auto& fileName : unconfigured) {
        String displayName = fileName;
        if (displayName.endsWith(".mp3") || displayName.endsWith(".MP3")) {
            displayName = displayName.substring(0, displayName.length() - 4);
        }
        all_files.emplace_back(fileName, displayName);
    }

    // Create buttons for this grid (limited by configuredGridButtonsMax)
    int buttons_created = 0;
    for (int i = start_index; i < all_files.size() && buttons_created < configuredGridButtonsMax; i++, buttons_created++) {
        int row = buttons_created / configuredGridCols;
        int col = buttons_created % configuredGridCols;

        lv_obj_t* btn = lv_button_create(grid);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_add_event_cb(btn, file_list_event_handler, LV_EVENT_CLICKED, nullptr); // Only listen for clicks

        // Performance optimization: Disable rounded corners and shadows
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN); // Square corners instead of rounded
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN); // Disable drop shadow
        lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN); // Make shadow transparent
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN); // Thin border for button definition
        lv_obj_set_style_border_color(btn, lv_color_hex(0x404040), LV_PART_MAIN); // Dark border

        // Find button configuration for styling
        ButtonConfig* config = nullptr;
        for (auto& cfg : buttonConfigs) {
            if (cfg.filename == all_files[i].first) {
                config = &cfg;
                break;
            }
        }

        if (config) {
            // Use configured color
            lv_color_t btnColor = getColorFromHex(config->color);
            lv_obj_set_style_bg_color(btn, btnColor, LV_PART_MAIN);

            // Set text color based on background brightness
            lv_color_t textColor = shouldUseWhiteText(config->color) ?
                lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);

            lv_obj_t* label = lv_label_create(btn);
            lv_label_set_text(label, config->label.c_str());

            // Enable text wrapping and use full button width (no padding)
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(label, lv_pct(100)); // Use 100% of button width - no padding
            lv_obj_center(label);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, textColor, LV_PART_MAIN);

            // Set font size - use per-button override if available, otherwise use global setting
            int fontSizeToUse = (config->fontSize > 0) ? config->fontSize : configuredFontSize;
            setFontBySize(label, fontSizeToUse);

            // Store filename in user data - use the persistent string from buttonConfigs
            lv_obj_set_user_data(btn, (void*)config->filename.c_str());
        } else {
            // Use default styling for unconfigured files
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x808080), LV_PART_MAIN);  // Gray

            lv_obj_t* label = lv_label_create(btn);
            lv_label_set_text(label, all_files[i].second.c_str());

            // Enable text wrapping and use full button width (no padding)
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(label, lv_pct(100)); // Use 100% of button width - no padding
            lv_obj_center(label);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

            // Set configurable font size for unconfigured files too
            setFontBySize(label, configuredFontSize);

            // For unconfigured files, we need to find the persistent string from unconfiguredFiles vector
            // Find the original string in the unconfiguredFiles vector
            const char* persistent_filename = nullptr;
            for (const auto& unconfiguredFile : unconfigured) {
                if (unconfiguredFile == all_files[i].first) {
                    persistent_filename = unconfiguredFile.c_str();
                    break;
                }
            }
            lv_obj_set_user_data(btn, (void*)persistent_filename);
        }
    }

    return grid;
}

TFT_eSPI tft = TFT_eSPI();

void setup() {
    Serial.begin(115200);
    Serial.println("CYD Soundboard starting...");

    // Initialize backlight control pin
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH); // Turn on backlight initially

    // Initialize screen timeout variables
    lastTouchTime = millis();
    screenOn = true;
    touchWakeupMode = false;

    // Initialize TFT display early so we can show a loading message before LVGL
    tft.begin();
    tft.setRotation(3); // Set to 3 for correct orientation (was 1)
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Lol jonge doe rustig, ff laden", TFT_HOR_RES/2, TFT_VER_RES/2);

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

    // Set volume from configuration
    delay(100); // Allow time for audio system to initialize
    audioSetVolume(configuredVolume);
    Serial.println("Audio volume set to: " + String(configuredVolume) + "/21");

    // Create horizontal scrolling container for grids - now uses full screen height
    file_list = lv_obj_create(lv_screen_active());
    lv_obj_set_size(file_list, TFT_HOR_RES, TFT_VER_RES); // Use full screen size
    lv_obj_center(file_list);

    // Configure horizontal scrolling with snap
    lv_obj_set_scroll_dir(file_list, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(file_list, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(file_list, LV_SCROLLBAR_MODE_OFF); // Disable scrollbar for better performance
    lv_obj_add_flag(file_list, LV_OBJ_FLAG_SCROLL_ONE);

    // Performance optimizations for scrolling - use alternative methods
    lv_obj_set_style_anim_duration(file_list, 200, 0); // Faster animations for snappier feel

    // Optimize rendering during scroll
    lv_obj_add_flag(file_list, LV_OBJ_FLAG_SCROLL_ELASTIC); // Add elastic scroll for smoother feel
    lv_obj_set_style_bg_opa(file_list, LV_OPA_TRANSP, 0); // Transparent background for better performance

    // Set flex layout for horizontal arrangement
    lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(file_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(file_list, 0, 0);
    lv_obj_set_style_pad_gap(file_list, GRID_GAP, 0); // Use configurable gap between grids

    // Calculate total files
    int total_configured = 0;
    for (const auto& config : buttonConfigs) {
        if (config.found) total_configured++;
    }
    int total_files = total_configured + unconfiguredFiles.size();

    // Create grids using configurable grid size
    int num_grids = (total_files + configuredGridButtonsMax - 1) / configuredGridButtonsMax; // Ceiling division

    Serial.println("Grid config: " + String(configuredGridCols) + "x" + String(configuredGridRows) +
                   " (" + String(configuredGridButtonsMax) + " buttons per grid)");
    Serial.println("Creating " + String(num_grids) + " grids for " + String(total_files) + " files");

    for (int grid_index = 0; grid_index < num_grids; grid_index++) {
        int start_index = grid_index * configuredGridButtonsMax;
        lv_obj_t* grid = create_button_grid(file_list, buttonConfigs, unconfiguredFiles, start_index);
        // Grid is automatically added to the flex container
    }

    Serial.println("Setup complete!");
}

void loop() {
    // Update LVGL timing and process UI events
    lv_tick_inc(millis() - lastTick);
    lastTick = millis();
    lv_timer_handler();

    // Handle screen timeout functionality
    unsigned long currentTime = millis();
    if (screenOn && !touchWakeupMode) {
        // Check if screen should timeout
        if (currentTime - lastTouchTime > (configuredScreenTimeout * 1000UL)) {
            screenOn = false;
            touchWakeupMode = true;
            digitalWrite(TFT_BL, LOW); // Turn off backlight
            Serial.println("Screen timeout - display turned off");
        }
    }

    // Check if audio has stopped playing and reset button color
    static bool wasPlaying = false;
    bool isCurrentlyPlaying = audioIsPlaying();

    if (wasPlaying && !isCurrentlyPlaying) {
        // Audio just stopped, reset button colors
        resetAllButtonColors();
        currentlyPlaying = "";
        Serial.println("Audio playback ended - button color reset");
    }

    wasPlaying = isCurrentlyPlaying;

    // No need to process audio manually - CYD28_audio handles it in its own task
    delay(5);
}
