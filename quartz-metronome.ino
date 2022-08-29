#include <ThreeWire.h>         // External RTC Library Helper
#include <RtcDS1302.h>         // External RTC library https://github.com/Makuna/Rtc/wiki/RtcDS1302-object
#include <PNGdec.h>            // PNG Decoder Library https://github.com/bitbank2/PNGdec
#include <FS.h>                // ESP32 Filesystem Functions
#include <SPIFFS.h>            // ESP32 Flash Filesystem helper
#include <WiFi.h>              // WiFi Functions
#include <AsyncTCP.h>          // Async Server Helper
#include <ESPAsyncWebServer.h> // Async Server
#include <AsyncElegantOTA.h>   // Async Update Helper
#include <Adafruit_GFX.h>      // Adafruit Canvas Library
#include <time.h>              // POSIX time functions
#include <sntp.h>              // SNTP sync helpers

// Fonts
#include "Inconsolata_Black8pt7b.h"
#include "Inconsolata_Black14pt7b.h"
#include "Inconsolata_Black16pt7b.h"
#include "Inconsolata_Black32pt7b.h"

// Pin Definitions
#define HEARTBEAT_LED_PIN 0    // Visible Program Running Status
#define RTC_CLOCK_PIN 15       // External Backup RTC CLK Pin
#define RTC_IO_PIN 2           // External Backup RTC IO Pin
#define RTC_ENABLE_PIN 4       // External Backup RTC CE Pin
#define OUTPUT_ENABLE_PIN 25   // When Output Enable is LOW, all displays are turned on
#define CLOCK_PIN 32           // Clock shifts Data on Rising Edge
#define DATA_PIN 33            // Data Input
#define LATCH_1_PIN 26         // Serial Data is transferred to the output latch when Latch is HIGH
#define LATCH_2_PIN 27         // The data is latched when Latch goes LOW
#define LATCH_3_PIN 14         // There are 5 latches
#define LATCH_4_PIN 12         // For 5 rows of LED Panels
#define LATCH_5_PIN 13         // Latching them one by one is better?

// Display Constants
#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 40
#define PANEL_WIDTH 32
#define MAX_PIXELBUFFERS 3

// Config
const char* ssid                = "VS-243";
const char* password            = "techcore";
const char* ntpServer1          = "pool.ntp.org";
const char* ntpServer2          = "time.nist.gov";
const int   daylightOffset_sec  = 3600;
// GMT to EST = 5 hours * 60 min/hour * 60 sec/min
const long  gmtOffset_sec       = -18000;                   
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv New York
const char* time_zone           = "EST5EDT,M3.2.0,M11.1.0"; 

// Globals
GFXcanvas1 canvas(DISPLAY_WIDTH, DISPLAY_HEIGHT);
uint8_t pixelBuffer[MAX_PIXELBUFFERS][5][DISPLAY_WIDTH];
uint8_t activePixelBuffer = 0;
ThreeWire rtcWire(RTC_IO_PIN, RTC_CLOCK_PIN, RTC_ENABLE_PIN);
RtcDS1302<ThreeWire> Rtc(rtcWire);
uint8_t latches[5] = {LATCH_1_PIN, LATCH_2_PIN, LATCH_3_PIN, LATCH_4_PIN, LATCH_5_PIN};
IPAddress local_IP(192, 168, 0, 99);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 0, 0);
IPAddress dns1(8, 8, 8, 8);
IPAddress dns2(8, 8, 4, 4);
AsyncWebServer server(80);
PNG png;
File pngFile;
String lastStatus;

// Tasks
void TaskShowMoney(void *pvParameters);
TaskHandle_t showMoneyHandle;
void TaskShowCountdown(void *pvParameters);
TaskHandle_t showCountdownHandle;
void TaskShowSlogans(void *pvParameters);
TaskHandle_t showSlogansHandle;
void TaskLoopAnimation(void *pvParameters);
TaskHandle_t loopAnimationHandle;
void TaskPrintDebugInfo(void *pvParameters);
TaskHandle_t printDebugInfoHandle;
void TaskHeartBeat(void *pvParameters);
TaskHandle_t heartBeatHandle;
void TaskPrintCanvas(void *pvParameters);

// Canvas Helpers
void printEqualWidth(String str, int offset) {
  canvas.setFont(&Inconsolata_Black32pt7b);
  for (int i = 0; i < str.length(); i++) {
    int y_offset = 39;
    if (str[i] == ',') y_offset = 32;
    canvas.setCursor(offset + (i * PANEL_WIDTH), y_offset);
    canvas.print(str[i]);
  }
}
void printHalfHeight(String str, int offset, bool top) {
  const int halfHeightOffsets[30] = {0, 19, 32, 51, 64, 83, 96, 115, 128, 147, 160, 179, 192, 211, 224, 243, 256, 275, 288, 307, 320, 339, 352, 371, 384, 403, 416, 435, 448, 467};
  canvas.setFont(&Inconsolata_Black14pt7b);
  for (int i = 0; i < str.length(); i++) {
    canvas.setCursor(halfHeightOffsets[offset + i], top ? 17 : 38);
    canvas.print(str[i]);
  }
}
void flipPixelBuffer() {
  activePixelBuffer = (activePixelBuffer + 1) % MAX_PIXELBUFFERS;
}
void clearPixelBuffer() {
  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < DISPLAY_WIDTH; j++) {
      pixelBuffer[(activePixelBuffer + 1) % MAX_PIXELBUFFERS][i][j] = 0;
    }
  }
}
void transferCanvasToBuffer() {
  clearPixelBuffer();
  for (int y = 0; y < DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
      pixelBuffer[(activePixelBuffer + 1) % MAX_PIXELBUFFERS][y / 8][x] |= canvas.getPixel(x, y) << (y%8);
    }
  }
  flipPixelBuffer();
}

// Log to Serial and local string for webserver to read
void clock_log(String status) {
  lastStatus = status;
  Serial.println(status);
}

// In case of emergency
void panic(String reason) {
  clock_log(reason);
  Serial.flush();
  ESP.restart();
}

// SNTP Time Callback Function
void timeavailable(struct timeval *t){
  clock_log(String(printf("Time Information received from NTP: %d\n", t->tv_sec)));
  RtcDateTime utc;
  // Set External RTC to timestamp received from network
  utc.InitWithEpoch32Time(t->tv_sec);
  Rtc.SetDateTime(utc);
}

// Main Setup / Program Entry Point
void setup() {
  // Start Serial Hardware
  Serial.begin(115200);
  clock_log("Starting Setup...");
  // Set Initial Canvas Parameters
  canvas.setFont(&Inconsolata_Black32pt7b);
  canvas.setTextColor(1);
  canvas.setTextWrap(false);
  // Set Output Pins using Arduino API
  pinMode(OUTPUT_ENABLE_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(DATA_PIN, OUTPUT);
  pinMode(LATCH_1_PIN, OUTPUT);
  pinMode(LATCH_2_PIN, OUTPUT);
  pinMode(LATCH_3_PIN, OUTPUT);
  pinMode(LATCH_4_PIN, OUTPUT);
  pinMode(LATCH_5_PIN, OUTPUT);
  // Set Time Zones for Internal RTC
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
  tzset();
  // Check External RTC and set Internal RTC is it's valid
  clock_log("Starting RTC...");
  Rtc.Begin();
  if (!Rtc.IsDateTimeValid()) {clock_log("RTC uninitialized or battery dead!");}
  if (Rtc.GetIsWriteProtected()){
      clock_log("RTC was write protected, enabling writing...");
      Rtc.SetIsWriteProtected(false);
  }
  struct timeval tv;
  if (!Rtc.GetIsRunning()){
      clock_log("RTC was not actively running, starting...");
      Rtc.SetIsRunning(true);
      tv.tv_sec = 1661745275; //Aug 28 2022 11:33 EST
  } else {
      RtcDateTime utc = Rtc.GetDateTime();
      clock_log(String(printf("External RTC Time: %02d:%02d:%02d\n", utc.Hour(), utc.Minute(), utc.Second())));  
      tv.tv_sec = utc.Epoch32Time();
  }
  settimeofday(&tv, NULL);
  // Check Internal RTC Time
  time_t now;
  struct tm tm;
  time(&now);
  localtime_r(&now, &tm);
  clock_log(String(printf("Internal RTC Time - %02d:%02d:%02d\n", tm.tm_hour, tm.tm_min, tm.tm_sec)));
  // Set static credentials to connect to our router
  if (!WiFi.config(local_IP, gateway, subnet, dns1, dns2)) {clock_log("STA Failed to configure!");}
  // Set SNTP Callback Function
  sntp_set_time_sync_notification_cb(timeavailable);
  // Set SNTP To Get NTP From DHCP
  sntp_servermode_dhcp(1);
  // sntp_set_sync_interval(60000); // Only for testing
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  // Initialize Update Handler and Server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {request->send(200, "text/plain", lastStatus);});
  AsyncElegantOTA.begin(&server);
  server.begin();
  // Set Internal RTC to poll preconfigured Time Zone and NTP Servers when available
  configTzTime(time_zone, ntpServer1, ntpServer2);
  // Create Asynchronous Tasks to be scheduled by FreeRTOS
  //xTaskCreatePinnedToCore(TaskHeartBeat, "TaskHeartBeat", 2048, NULL, 1, &heartBeatHandle, 0);
  //xTaskCreatePinnedToCore(TaskPrintDebugInfo, "TaskPrintDebugInfo", 2048, NULL, 1, &printDebugInfoHandle, 0);
  //xTaskCreatePinnedToCore(TaskPrintCanvas, "TaskPrintCanvas", 4096, NULL, 1, NULL, 1);
  //xTaskCreatePinnedToCore(TaskLoopAnimation, "TaskLoopAnimation", 4096, NULL, 1, &loopAnimationHandle, 0);
  xTaskCreatePinnedToCore(TaskShowCountdown, "TaskShowCountdown", 4096, NULL, 1, &showCountdownHandle, 0);
  vTaskSuspend(showCountdownHandle);
  xTaskCreatePinnedToCore(TaskShowMoney, "TaskShowMoney", 4096, NULL, 1, &showMoneyHandle, 0);
  vTaskSuspend(showMoneyHandle);
  xTaskCreatePinnedToCore(TaskShowSlogans, "TaskShowSlogans", 4096, NULL, 1, &showSlogansHandle, 0);
  // Brightness control, 0 is Full On, 255 is Full Off
  analogWrite(OUTPUT_ENABLE_PIN, 32);
  // Runs on App Core 1
  clock_log("Starting Main Loop");
}

// Main Program Loop / Entry After Setup Complete - Keeps Pixel Data streaming to Pixel Display
void loop() {
  // Latch one pixel buffer in case they swap mid draw
  uint8_t pixelBufferIndex = activePixelBuffer;
  // There are 5 Banks of LEDs, each controlled by a seperate Latch pin
  for (int i = 0; i < 5; i++) {
    // Data is streamed into the banks, then 1 of 5 are latched to show to pixel data on the display
    for (int col = 0; col < DISPLAY_WIDTH; col++) {
      // MSBFIRST
      for (int k = 7; k >= 0; k--) {
        // GPIO.out functions were used because they seem to be the fastest, and timing is critical in this loop
        if (pixelBuffer[activePixelBuffer][i][col] & (1 << k)){
          GPIO.out1_w1ts.val = (1 << (DATA_PIN-32));
        } else {
          GPIO.out1_w1tc.val = (1 << (DATA_PIN-32));
        }
        GPIO.out1_w1ts.val = (1 << (CLOCK_PIN-32));
        GPIO.out1_w1tc.val = (1 << (CLOCK_PIN-32));
      }
    }
    // Wait a little for the pixel data to settle down before latching
    delayMicroseconds(3);
    GPIO.out_w1ts = (1 << latches[i]);
    GPIO.out_w1tc = (1 << latches[i]);
    delayMicroseconds(2);
  }
}

// Shows the Climate Countdown Clock
void TaskShowCountdown(void *pvParameters) {
  (void) pvParameters;
  struct tm tm;
  getLocalTime(&tm);
  struct tm deadline = {0};
  strptime("2029-07-20 11:00:00", "%Y-%m-%d %H:%M:%S", &deadline);
  time_t started_time = mktime(&tm);
  
  while (1) {
    getLocalTime(&tm);
    long time_left = difftime(mktime(&deadline), mktime(&tm));
    long years_left = time_left / 31536000;
    long days_left = (time_left - (years_left * 31536000)) / 86400;
    long hours_left = (time_left - (years_left * 31536000) - (days_left * 86400)) / 3600;
    long minutes_left = (time_left - (years_left * 31536000) - (days_left * 86400) - (hours_left * 3600)) / 60;
    long seconds_left = (time_left - (years_left * 31536000) - (days_left * 86400) - (hours_left * 3600) - (minutes_left * 60));
    
    canvas.fillScreen(0);
    
    canvas.setFont(&Inconsolata_Black32pt7b);
    canvas.setCursor(1, 38);
    canvas.print(0);
    canvas.setCursor(33, 38);
    canvas.print(years_left);
    canvas.setFont(&Inconsolata_Black8pt7b);
    canvas.setCursor(65, 38);
    canvas.print("YRS");

    canvas.setFont(&Inconsolata_Black32pt7b);
    canvas.setCursor(97, 38);
    canvas.print((days_left / 100) % 10);
    canvas.setCursor(129, 38);
    canvas.print((days_left / 10) % 10);
    canvas.setCursor(161, 38);
    canvas.print(days_left % 10);
    canvas.setFont(&Inconsolata_Black8pt7b);
    canvas.setCursor(192, 38);
    canvas.print("DAYS");

    canvas.setFont(&Inconsolata_Black32pt7b);
    canvas.setCursor(225, 38);
    canvas.print(hours_left / 10);
    canvas.setCursor(257, 38);
    canvas.print(hours_left % 10);
    canvas.setCursor(289, 38);
    canvas.print(":");
    canvas.setCursor(321, 38);
    canvas.print(minutes_left / 10);
    canvas.setCursor(353, 38);
    canvas.print(minutes_left % 10);
    canvas.setCursor(385, 38);
    canvas.print(":");
    canvas.setCursor(417, 38);
    canvas.print(seconds_left / 10);
    canvas.setCursor(449, 38);
    canvas.print(seconds_left % 10);

    transferCanvasToBuffer();
    // Serial.printf("%02dYRS%03dDAYS%02d:%02d:%02d\n", years_left, days_left, hours_left, minutes_left, seconds_left);
    vTaskDelay(100);

    time_t current_time = mktime(&tm);
    if ((current_time - started_time) > 30) {
      vTaskResume(showSlogansHandle);
      vTaskSuspend(showCountdownHandle);
      // clock_log(String(printf("%02dYRS%03dDAYS%02d:%02d:%02d\n", years_left, days_left, hours_left, minutes_left, seconds_left)));
      getLocalTime(&tm);
      started_time = mktime(&tm);
    }
  }
}

// Show Me The Money
void TaskShowMoney(void *pvParameters) {
  (void) pvParameters;
  struct tm tm;
  getLocalTime(&tm);
  struct tm deadline = {0};
  strptime("2020-01-01 00:00:00", "%Y-%m-%d %H:%M:%S", &deadline);
  time_t started_time = mktime(&tm);
  
  while (1) {
    getLocalTime(&tm);
    long time_elapsed = difftime(mktime(&tm), mktime(&deadline));
    double money_owed = time_elapsed * 4.71408520521309e-8 + 29.754;
    String money_owed_string = String(money_owed, 12);
    String money_string = "$" + money_owed_string.substring(0,2) + "," + money_owed_string.substring(3,6) + "," + money_owed_string.substring(6,9) + "," + money_owed_string.substring(9,12);

    canvas.fillScreen(0);
    printEqualWidth(money_string, 0);
    transferCanvasToBuffer();
    vTaskDelay(100);

    time_t current_time = mktime(&tm);
    if ((current_time - started_time) > 15) {
      vTaskResume(showSlogansHandle);
      vTaskSuspend(showMoneyHandle);
      getLocalTime(&tm);
      started_time = mktime(&tm);
    }
  }
}

// Blinks the Slogans into the display
void TaskShowSlogans(void *pvParameters) {
  (void) pvParameters;
  while (1) {
    canvas.fillScreen(0);
    printHalfHeight("HOW MUCH $ DO RICH NATIONS OWE", 0, true);
    printHalfHeight("WORLD 4 CLIMATE LOSS & DAMAGE?", 0, false);
    transferCanvasToBuffer();
    vTaskDelay(5000);
    
    vTaskResume(showMoneyHandle);
    vTaskSuspend(showSlogansHandle);

    canvas.fillScreen(0);
    printHalfHeight("OVER $33 TRILLION", 7, true);
    printHalfHeight("OWED IN CLIMATE DEBT", 5, false);
    transferCanvasToBuffer();
    vTaskDelay(5000);
    
    canvas.fillScreen(0);
    printEqualWidth("G20 PAY UR DEBT", 0);
    transferCanvasToBuffer();
    vTaskDelay(5000);
    
    canvas.fillScreen(0);
    printEqualWidth("DELAY = DENIAL", 0);
    transferCanvasToBuffer();
    vTaskDelay(5000);
    
    canvas.fillScreen(0);
    printEqualWidth("Let", 0);
    printEqualWidth("'s #ActInTime", 96);
    transferCanvasToBuffer();
    vTaskDelay(5000);

    canvas.fillScreen(0);
    printEqualWidth("#LossAndDamage", 0);
    transferCanvasToBuffer();
    vTaskDelay(5000);

    canvas.fillScreen(0);
    printHalfHeight("WE HAVE LESS THAN 7 YEARS", 2, true);
    printHalfHeight("TO PROTECT PLANET & PEOPLE", 2, false);
    transferCanvasToBuffer();
    vTaskDelay(5000);

    vTaskResume(showCountdownHandle);
    vTaskSuspend(showSlogansHandle);
  }
}

// These functions provide the code required by the PNG library's callback handlers to work with ESP32 Flash file objects easily
void *PNGOpen(const char *filename, int32_t *size) {
  pngFile = SPIFFS.open(filename, FILE_READ);
  if (!pngFile) return 0;
  *size = pngFile.size();
  return &pngFile;
}
void PNGClose(void *handle) {
  if (pngFile) pngFile.close();
}
int32_t PNGRead(PNGFILE *handle, uint8_t *buffer, int32_t length) {
  // return {pngFile) ? pngFile.read(buffer, length) : 0;
  if (!pngFile) return 0;
  return pngFile.read(buffer, length);
}
int32_t PNGSeek(PNGFILE *handle, int32_t position) {
  // return {pngFile) ? pngFile.seek(position) : 0;
  if (!pngFile) return 0;
  return pngFile.seek(position);
}
// PNGDraw is a callback function from the PNG decoder library that returns line-by-line pixel data about a given PNG file.
void PNGDraw(PNGDRAW *pDraw) {
  // TODO Benchmark removing the clear buffer step and replacing this with |= and &= to set or clear the pixel
  for (int col = 0; col < DISPLAY_WIDTH; col++) {
    pixelBuffer[(activePixelBuffer + 1) % MAX_PIXELBUFFERS][pDraw->y / 8][col] |= (pDraw->pPixels[col] > 0) << (pDraw->y%8);
  }
}
// Loops through the animation PNGs in the SPIFFS filesystem
void TaskLoopAnimation(void *pvParameters) {
  (void) pvParameters;
  if (!SPIFFS.begin()) {clock_log("SPIFFS Error while mounting");}
  int image_index = 0;
  while (1) {
    char filenameBuffer[10];
    sprintf(filenameBuffer, "/%04d.png", image_index);
    String filename = String(filenameBuffer);
    // Attempt to decode requested PNG file
    if (png.open((const char *)filename.c_str(), PNGOpen, PNGClose, PNGRead, PNGSeek, PNGDraw) != PNG_SUCCESS) panic("PNG Error while opening " + filename);  
    clock_log(String(printf("%s specs: (%d x %d), %d bpp, pixel type: %d, buffer %d\n", filename, png.getWidth(), png.getHeight(), png.getBpp(), png.getPixelType(), png.getBufferSize())));
    clearPixelBuffer();
    if (png.decode(NULL, 0) != PNG_SUCCESS) clock_log("PNG Error while decoding " + filename);  
    flipPixelBuffer();
    png.close();                                                                            
    vTaskDelay(3000);                                                         
    image_index++;                                        
    if (image_index > 5) {
      vTaskResume(showCountdownHandle);
      vTaskSuspend(loopAnimationHandle);
      image_index = 0;
    }
  }
}

// Prints helpful debug info to the Serial Monitor
void TaskPrintDebugInfo(void *pvParameters) {
  (void) pvParameters;
  while (1) {
    time_t now;
    struct tm info;
    time(&now);
    localtime_r(&now, &info);
    RtcDateTime utc = Rtc.GetDateTime();
    clock_log(String(printf("External RTC Time: %d:%d:%d\n", utc.Hour(), utc.Minute(), utc.Second())));
    clock_log(String(printf("Internal RTC Time - %d:%d:%d\n", info.tm_hour, info.tm_min, info.tm_sec)));
    clock_log(String(printf("Wifi connected: %s\n", (WiFi.status() == WL_CONNECTED) ? "true" : "false")));
    clock_log(String(printf("Status updating using core %d\n", xPortGetCoreID())));
    clock_log(String(printf("Free Memory: %d\n", ESP.getFreeHeap())));
    clock_log(String(printf("Heart Beat: %d\n", uxTaskGetStackHighWaterMark( heartBeatHandle ))));
    clock_log(String(printf("Loop Animation: %d\n", uxTaskGetStackHighWaterMark( loopAnimationHandle ))));
    vTaskDelay(3000);
  }
}

// Pulses the onboard blue PROGRAM OK LED to provide visual indication of program running status
void TaskHeartBeat(void *pvParameters) {
  (void) pvParameters;
  uint8_t heartbeat_index = 0;
  uint8_t heartbeat_lut[256] = {
    0x80,0x83,0x86,0x89,0x8c,0x8f,0x92,0x95,0x98,0x9b,0x9e,0xa2,0xa5,0xa7,0xaa,0xad,
    0xb0,0xb3,0xb6,0xb9,0xbc,0xbe,0xc1,0xc4,0xc6,0xc9,0xcb,0xce,0xd0,0xd3,0xd5,0xd7,
    0xda,0xdc,0xde,0xe0,0xe2,0xe4,0xe6,0xe8,0xea,0xeb,0xed,0xee,0xf0,0xf1,0xf3,0xf4,
    0xf5,0xf6,0xf8,0xf9,0xfa,0xfa,0xfb,0xfc,0xfd,0xfd,0xfe,0xfe,0xfe,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xfe,0xfe,0xfe,0xfd,0xfd,0xfc,0xfb,0xfa,0xfa,0xf9,0xf8,0xf6,
    0xf5,0xf4,0xf3,0xf1,0xf0,0xee,0xed,0xeb,0xea,0xe8,0xe6,0xe4,0xe2,0xe0,0xde,0xdc,
    0xda,0xd7,0xd5,0xd3,0xd0,0xce,0xcb,0xc9,0xc6,0xc4,0xc1,0xbe,0xbc,0xb9,0xb6,0xb3,
    0xb0,0xad,0xaa,0xa7,0xa5,0xa2,0x9e,0x9b,0x98,0x95,0x92,0x8f,0x8c,0x89,0x86,0x83,
    0x80,0x7c,0x79,0x76,0x73,0x70,0x6d,0x6a,0x67,0x64,0x61,0x5d,0x5a,0x58,0x55,0x52,
    0x4f,0x4c,0x49,0x46,0x43,0x41,0x3e,0x3b,0x39,0x36,0x34,0x31,0x2f,0x2c,0x2a,0x28,
    0x25,0x23,0x21,0x1f,0x1d,0x1b,0x19,0x17,0x15,0x14,0x12,0x11,0xf,0xe,0xc,0xb,0xa,
    0x9,0x7,0x6,0x5,0x5,0x4,0x3,0x2,0x2,0x1,0x1,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x1,
    0x1,0x1,0x2,0x2,0x3,0x4,0x5,0x5,0x6,0x7,0x9,0xa,0xb,0xc,0xe,0xf,0x11,0x12,0x14,
    0x15,0x17,0x19,0x1b,0x1d,0x1f,0x21,0x23,0x25,0x28,0x2a,0x2c,0x2f,0x31,0x34,0x36,
    0x39,0x3b,0x3e,0x41,0x43,0x46,0x49,0x4c,0x4f,0x52,0x55,0x58,0x5a,0x5d,0x61,0x64,
    0x67,0x6a,0x6d,0x70,0x73,0x76,0x79,0x7c}; // or to taste
  pinMode(HEARTBEAT_LED_PIN, OUTPUT);
  while (1) {
    GPIO.out_w1ts = (1 << HEARTBEAT_LED_PIN);
    vTaskDelay(1000);
    GPIO.out_w1tc = (1 << HEARTBEAT_LED_PIN);
    vTaskDelay(1000);
  }
}

// Prints the canvas slowly over Serial
void TaskPrintCanvas(void *pvParameters) {
  (void) pvParameters;
  int buffer_index = 0;
  while (1) {
    for (uint16_t x = 0; x < DISPLAY_WIDTH; x++) {
      char pixel_buffer[8];
      bool pixel = canvas.getPixel(x, buffer_index);
      sprintf(pixel_buffer, "%d", pixel);
      Serial.print(pixel_buffer);
      if (x%32 == 31) {Serial.print(" ");}
    }
    Serial.println();
    buffer_index++;
    if (buffer_index >= DISPLAY_HEIGHT) {
      Serial.print("\n\n");
      buffer_index = 0;
    }
    vTaskDelay(5);
  }
}
