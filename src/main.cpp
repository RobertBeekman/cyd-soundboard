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

// Grid configuration - adjust these for performance tuning
#define GRID_COLS 4        // Number of columns per grid (was 5)
#define GRID_ROWS 3        // Number of rows per grid (was 6)
#define GRID_BUTTONS_MAX (GRID_COLS * GRID_ROWS)  // Maximum buttons per grid
#define BUTTON_GAP 1       // Gap between buttons in pixels
#define GRID_GAP 2         // Gap between grids in pixels

// Default volume setting (0-21 range)
#define DEFAULT_VOLUME 12  // Default volume if not specified in config file

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

std::vector<ButtonConfig> buttonConfigs;  // Configured buttons
std::vector<String> unconfiguredFiles;    // MP3 files not in config

// Global configuration variables
int configuredVolume = DEFAULT_VOLUME;    // Volume setting from config file

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
    configuredVolume = DEFAULT_VOLUME; // Reset to default

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

        // Parse button format: filename|label|color
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
    Serial.println("Configuration loaded: " + String(buttonConfigs.size()) + " entries, Volume: " + String(configuredVolume) + "/21");
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

/* Create a configurable grid of buttons within a container */
lv_obj_t* create_button_grid(lv_obj_t* parent, const std::vector<ButtonConfig>& configs, const std::vector<String>& unconfigured, int start_index) {
    // Create grid container with full screen height
    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_size(grid, TFT_HOR_RES - 10, TFT_VER_RES - 10); // Use full screen size minus small margin
    lv_obj_set_style_pad_all(grid, BUTTON_GAP, 0); // Use configurable padding
    lv_obj_set_style_pad_gap(grid, BUTTON_GAP, 0); // Use configurable gap
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE); // Grid itself shouldn't scroll

    // Dynamically create grid descriptors based on configuration
    static int32_t col_dsc[GRID_COLS + 1];
    static int32_t row_dsc[GRID_ROWS + 1];

    // Fill column descriptors
    for (int i = 0; i < GRID_COLS; i++) {
        col_dsc[i] = LV_GRID_FR(1);
    }
    col_dsc[GRID_COLS] = LV_GRID_TEMPLATE_LAST;

    // Fill row descriptors
    for (int i = 0; i < GRID_ROWS; i++) {
        row_dsc[i] = LV_GRID_FR(1);
    }
    row_dsc[GRID_ROWS] = LV_GRID_TEMPLATE_LAST;

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

    // Create buttons for this grid (limited by GRID_BUTTONS_MAX)
    int buttons_created = 0;
    for (int i = start_index; i < all_files.size() && buttons_created < GRID_BUTTONS_MAX; i++, buttons_created++) {
        int row = buttons_created / GRID_COLS;
        int col = buttons_created % GRID_COLS;

        lv_obj_t* btn = lv_button_create(grid);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_add_event_cb(btn, file_list_event_handler, LV_EVENT_CLICKED, nullptr); // Only listen for clicks

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
            lv_color_t btnColor = getColorFromName(config->color);
            lv_obj_set_style_bg_color(btn, btnColor, LV_PART_MAIN);

            // Set text color based on background brightness
            lv_color_t textColor = shouldUseWhiteText(config->color) ?
                lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);

            lv_obj_t* label = lv_label_create(btn);
            lv_label_set_text(label, config->label.c_str());
            lv_obj_center(label);
            lv_obj_set_style_text_color(label, textColor, LV_PART_MAIN);

            // Store filename in user data - use the persistent string from buttonConfigs
            lv_obj_set_user_data(btn, (void*)config->filename.c_str());
        } else {
            // Use default styling for unconfigured files
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x808080), LV_PART_MAIN);  // Gray

            lv_obj_t* label = lv_label_create(btn);
            lv_label_set_text(label, all_files[i].second.c_str());
            lv_obj_center(label);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

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

    // Set volume from configuration
    delay(100); // Allow time for audio system to initialize
    audioSetVolume(configuredVolume);
    Serial.println("Audio volume set to: " + String(configuredVolume) + "/21");

    // Create horizontal scrolling container for grids - now uses full screen height
    file_list = lv_obj_create(lv_screen_active());
    lv_obj_set_size(file_list, TFT_HOR_RES, TFT_VER_RES); // Use full screen size
    lv_obj_center(file_list); // Center on screen

    // Configure horizontal scrolling with snap
    lv_obj_set_scroll_dir(file_list, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(file_list, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(file_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(file_list, LV_OBJ_FLAG_SCROLL_ELASTIC);

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
    int num_grids = (total_files + GRID_BUTTONS_MAX - 1) / GRID_BUTTONS_MAX; // Ceiling division

    Serial.println("Grid config: " + String(GRID_COLS) + "x" + String(GRID_ROWS) +
                   " (" + String(GRID_BUTTONS_MAX) + " buttons per grid)");
    Serial.println("Creating " + String(num_grids) + " grids for " + String(total_files) + " files");

    for (int grid_index = 0; grid_index < num_grids; grid_index++) {
        int start_index = grid_index * GRID_BUTTONS_MAX;
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

    // No need to process audio manually - CYD28_audio handles it in its own task
    delay(5);
}
