/* Using LVGL with Arduino requires some extra steps...
 *
 * Be sure to read the docs here: https://docs.lvgl.io/master/integration/framework/arduino.html
 * but note you should use the lv_conf.h from the repo as it is pre-edited to work.
 *
 * You can always edit your own lv_conf.h later and exclude the example options once the build environment is working.
 *
 * Note you MUST move the 'examples' and 'demos' folders into the 'src' folder inside the lvgl library folder
 * otherwise this will not compile, please see README.md in the repo.
 *
 */

// TODO:
// https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/TROUBLESHOOTING.md#display-touch-and-sd-card-are-not-working-at-the-same-time

#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <SD.h>
#include <FS.h>

// A library for interfacing with the touch screen using software SPI
//
// Can be installed from the library manager (Search for "XPT2046 Slim")
// https://github.com/TheNitek/XPT2046_Bitbang_Arduino_Library
// ----------------------------
// Touch Screen pins
// ----------------------------

// The CYD touch uses some non default
// SPI pins

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// SD Card pin for ESP32-2432S028R
#define SD_CS 5

XPT2046_Bitbang touchscreen(XPT2046_MOSI, XPT2046_MISO, XPT2046_CLK, XPT2046_CS);
// Updated calibration values based on actual touch data
uint16_t touchScreenMinimumX = 21, touchScreenMaximumX = 295, touchScreenMinimumY = 20, touchScreenMaximumY = 219;

/*Set to your screen resolution*/
#define TFT_HOR_RES   320
#define TFT_VER_RES   240

/*LVGL draw into this buffer, 1/10 screen size usually works well. The size is in bytes*/
#define DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))

#if LV_USE_LOG != 0
void my_print( lv_log_level_t level, const char * buf )
{
    LV_UNUSED(level);
    Serial.println(buf);
    Serial.flush();
}
#endif

/* LVGL calls it when a rendered image needs to copied to the display*/
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    /*Call it to tell LVGL you are ready*/
    lv_disp_flush_ready(disp);
}

/*Read the touchpad*/
void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
    TouchPoint p = touchscreen.getTouch();
    if (p.zRaw > 0) {
        //Map this to the pixel position and invert both X and Y coordinates
        data->point.x = map(p.x, touchScreenMinimumX, touchScreenMaximumX, TFT_HOR_RES, 1);
        data->point.y = map(p.y, touchScreenMinimumY, touchScreenMaximumY, TFT_VER_RES, 1);

        data->state = LV_INDEV_STATE_PRESSED;

        /*
        Serial.print("Touch raw x: ");
        Serial.print(p.x);
        Serial.print(" y: ");
        Serial.print(p.y);
        Serial.print(" -> mapped x: ");
        Serial.print(data->point.x);
        Serial.print(" y: ");
        Serial.println(data->point.y);
        */

    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

lv_indev_t *indev; //Touchscreen input device
uint8_t *draw_buf; //draw_buf is allocated on heap otherwise the static area is too big on ESP32 at compile
uint32_t lastTick = 0; //Used to track the tick timer

// Global variables for file list
lv_obj_t * file_list;
String fileNames[50]; // Store up to 50 files
int fileCount = 0;

// Function to scan SD card root directory for files
void scanSDCard() {
    fileCount = 0;

    // Initialize SD card using VSPI (same as the working example)
    SPIClass spi = SPIClass(VSPI);

    if (!SD.begin(SS, spi, 80000000)) {
        Serial.println("SD Card initialization failed!");
        fileNames[0] = "SD Card Error";
        fileCount = 1;
        return;
    }

    Serial.println("SD Card initialized successfully");

    // Check card type
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        fileNames[0] = "No SD Card";
        fileCount = 1;
        return;
    }

    // Open root directory
    File root = SD.open("/");
    if (!root) {
        Serial.println("Failed to open root directory");
        fileNames[0] = "Directory Error";
        fileCount = 1;
        return;
    }

    // Scan files in root directory
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

// Event handler for file list items
static void file_list_event_handler(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target_obj(e);

    if (code == LV_EVENT_CLICKED) {
        // Get the label from the button and extract filename
        lv_obj_t * label = lv_obj_get_child(obj, 0);
        const char * txt = lv_label_get_text(label);
        Serial.println("Selected file: " + String(txt));

        // Here you can add code to handle the selected file
        // For example, play audio file, open file, etc.
    }
}

void setup() {
    //Some basic info on the Serial console
    Serial.begin(115200);

    //Initialise the touchscreen using software SPI
    touchscreen.begin(); /* Touchscreen init */

    //Initialise LVGL
    lv_init();
    draw_buf = new uint8_t[DRAW_BUF_SIZE];
    lv_display_t *disp;
    disp = lv_tft_espi_create(TFT_HOR_RES, TFT_VER_RES, draw_buf, DRAW_BUF_SIZE);

    //Initialize the XPT2046 input device driver
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    // Scan SD card for files
    scanSDCard();

    // Create a scrollable container instead of a list
    file_list = lv_obj_create(lv_screen_active());
    lv_obj_set_size(file_list, 300, 220);  // Set container size to fit screen
    lv_obj_center(file_list);              // Center the container on screen

    // Enable scrolling
    lv_obj_set_scroll_dir(file_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(file_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(file_list, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // Set flex layout for vertical arrangement
    lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(file_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Add files as individual buttons
    for (int i = 0; i < fileCount; i++) {
        lv_obj_t * btn = lv_button_create(file_list);
        lv_obj_set_size(btn, 280, 40);  // Button size
        lv_obj_add_event_cb(btn, file_list_event_handler, LV_EVENT_ALL, nullptr);

        // Create label for the file name
        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text_fmt(label, LV_SYMBOL_FILE " %s", fileNames[i].c_str());
        lv_obj_center(label);
    }

    //Done
    Serial.println("Setup done");
}

void loop() {
    lv_tick_inc(millis() - lastTick); //Update the tick timer. Tick is new for LVGL 9
    lastTick = millis();
    lv_timer_handler(); //Update the UI
    delay(5);
}