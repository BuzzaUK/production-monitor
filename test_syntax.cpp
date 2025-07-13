#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>
#include <ctype.h>
#include <WiFiManager.h>

// --- OLED Display Includes and Defines (ADDITION) ---
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// --- END OLED DEFINES ---

// --- OLED Display State (ADDITION) ---
String lastSsidDisplayed = "";
String lastIpDisplayed = "";

// --- Function to update OLED with Network info (ADDITION) ---
void updateOledDisplay() {
  String ssid = WiFi.SSID();
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "No IP";
  if (ssid != lastSsidDisplayed || ip != lastIpDisplayed) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Network: ");
    display.println(ssid);
    display.println();
    display.print("IP: ");
    display.println(ip);
    display.display();
    lastSsidDisplayed = ssid;
    lastIpDisplayed = ip;
  }
}
// --- END OLED FUNCTION ---

// --- Essential Defines for Structs ---
#define MAX_ASSETS 10
#define MAX_CONFIGURABLE_SHIFTS 5 // Allow up to 5 shifts to be defined

// --- Struct Definitions ---
struct ShiftInfo {
  char startTime[6]; // "HH:MM" format, e.g., "06:00"
void test() {}
