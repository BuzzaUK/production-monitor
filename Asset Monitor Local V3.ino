/*
 * Asset Monitor Local V3 - Production Monitor System
 * 
 * COMPILATION FIXES APPLIED:
 * - Fixed duplicate 'time_t initialTime' variable declarations causing redeclaration errors
 * - Ensured all JavaScript is properly wrapped in C++ multi-line string literals using R"()" syntax
 * - Verified all asset variables (MINIMAL_BOOTSTRAP_JS, SIMPLE_CHARTS_JS, etc.) are properly declared
 * - Removed duplicate asset initialization code block that was causing compilation issues
 * - Fixed string literal termination and quote/parenthesis matching
 * - Updated all web asset handlers to serve correct assets matching declared constants
 * - Maintained all core functionality: WiFi, shift logic, asset monitoring, web server, logging, API endpoints
 * - Code structure verified for proper Arduino compilation
 */

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>
#include <ctype.h>
#include <WiFiManager.h>

// --- OLED Display Includes and Defines (ENHANCED) ---
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// --- END OLED DEFINES ---

// --- WiFi State Tracking (NEW) ---
enum WiFiState {
  WIFI_IDLE,
  WIFI_CONNECTING_STA,
  WIFI_STA_CONNECTED,
  WIFI_AP_MODE
};
WiFiState currentWiFiState = WIFI_IDLE;
unsigned long wifiConnectStartTime = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 30000; // 30 seconds
// --- END WiFi State Tracking ---

// --- OLED Display State (ENHANCED) ---
String lastSsidDisplayed = "";
String lastIpDisplayed = "";
String lastStatusDisplayed = "";
unsigned long lastOledUpdate = 0;
const unsigned long OLED_UPDATE_INTERVAL = 1000; // Update every second during boot

// --- Enhanced OLED Display Functions (NEW/ENHANCED) ---
void displayBootMessage(const char* message) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Asset Monitor V2");
  display.println("================");
  display.println();
  display.println(message);
  display.display();
  Serial.println(String("OLED: ") + message);
}

void displayWiFiStatus(const char* status, const char* detail = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Asset Monitor V2");
  display.println("================");
  display.println();
  display.println(status);
  if (strlen(detail) > 0) {
    display.println(detail);
  }
  display.display();
  Serial.println(String("OLED: ") + status + (strlen(detail) > 0 ? String(" - ") + detail : ""));
}

void updateOledDisplay() {
  String ssid;
  String ip;
  String status;
  
  switch (currentWiFiState) {
    case WIFI_STA_CONNECTED:
      ssid = WiFi.SSID();
      ip = WiFi.localIP().toString();
      status = "STA Connected";
      break;
    case WIFI_AP_MODE:
      ssid = "AssetMonitor-Config";
      ip = WiFi.softAPIP().toString();
      status = "AP Mode";
      break;
    case WIFI_CONNECTING_STA:
      ssid = "Connecting...";
      ip = "Waiting for IP";
      status = "Connecting STA";
      break;
    default:
      ssid = "Not Connected";
      ip = "No IP";
      status = "Disconnected";
  }
  
  // Only update if something changed
  if (ssid != lastSsidDisplayed || ip != lastIpDisplayed || status != lastStatusDisplayed) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Asset Monitor V2");
    display.println("================");
    display.println();
    display.print("Mode: ");
    display.println(status);
    display.print("Network: ");
    display.println(ssid);
    display.print("IP: ");
    display.println(ip);
    display.display();
    
    lastSsidDisplayed = ssid;
    lastIpDisplayed = ip;
    lastStatusDisplayed = status;
  }
}
// --- END Enhanced OLED Functions ---

// --- Essential Defines for Structs ---
#define MAX_ASSETS 10
#define MAX_CONFIGURABLE_SHIFTS 5 // Allow up to 5 shifts to be defined

// --- Struct Definitions ---
struct ShiftInfo {
  char startTime[6]; // "HH:MM" format, e.g., "06:00"
};

struct AssetConfig {
  char name[32];
  uint8_t pin;
};

struct Config {
  uint8_t assetCount;
  uint16_t maxEvents;
  AssetConfig assets[MAX_ASSETS];
  char downtimeReasons[5][32];
  int tzOffset;
  int longStopThresholdSec;
  int monitoringMode; // <<< ADDED for monitoring mode

  // --- NEW SHIFT CONFIGURATION ---
  bool enableShiftArchiving;
  uint8_t numShifts;
  ShiftInfo shifts[MAX_CONFIGURABLE_SHIFTS];
  // --- END NEW SHIFT CONFIGURATION ---
};

struct AssetState {
  bool lastState; // true if pin is HIGH (STOPPED for INPUT_PULLUP), false if pin is LOW (RUNNING)
  time_t lastChangeTime;
  unsigned long runningTime;  // Cumulative for current session/since last log clear
  unsigned long stoppedTime;  // Cumulative for current session/since last log clear
  unsigned long sessionStart; // Timestamp of when current session stats started (e.g. boot or log clear)
  unsigned long lastEventTime; // Timestamp of the last logged event for this asset
  uint32_t runCount;      // Number of times asset has started
  uint32_t stopCount;     // Number of times asset has stopped
  unsigned long lastRunDuration;  // Duration of the most recent run period
  unsigned long lastStopDuration; // Duration of the most recent stop period
};

struct Event {
  time_t timestamp;
  char assetName[32];
  char eventType[8];
  int state;
  float availability;
  float runtime;
  float downtime;
  float mtbf;
  float mttr;
  unsigned int stops;
  char runDuration[8];
  char stopDuration[8];
  char note[64];
  Event() { // constructor
    timestamp = 0; assetName[0] = '\0'; eventType[0] = '\0'; state = 0;
    availability = 0; runtime = 0; downtime = 0; mtbf = 0; mttr = 0;
    stops = 0; runDuration[0] = '\0'; stopDuration[0] = '\0'; note[0] = '\0';
  }
};

// --- Other Defines ---
#define LOG_FILENAME "/log.csv"
// --- Added Monitoring Mode Defines ---
#define MONITORING_MODE_PARALLEL 0
#define MONITORING_MODE_SERIAL 1
// --- End Added Defines ---

// --- Forward declarations ---
String htmlDashboard();
String htmlAnalytics();
String htmlAnalyticsCompare();
String htmlConfig();
void sendHtmlEventsPage();
String wifiConfigHTML();
String htmlAssetDetail(uint8_t idx);
void handleConfigPost();
void handleDeleteLogs();
void handleClearLog();
void handleExportLog();
void handleApiSummary();
void handleApiEvents();
void handleApiConfig();
void handleApiNote();
void handleNotFound();
void updateEventNote(String date, String time, String assetName, String note, String reason);
void logEvent(uint8_t assetIdx, bool machineIsRunning, time_t now, const char* customNote = nullptr, unsigned long runDuration = 0, unsigned long stopDuration = 0);
void logSystemEvent(bool systemIsStarting, time_t now, const char* triggerAssetNameOrReason); // Added
void handleWiFiReconfigurePost();
void setupTime();
void handleShiftLogsPage();
void handleDownloadShiftLog();
String urlEncode(const String& str);
String urlDecode(const String& str);
String eventToCSV(const Event& e); // Moved Event struct definition before this
String formatMMSS(unsigned long seconds);
void loadConfig();
void saveConfig();
void setupWiFiSmartV2(); // Enhanced WiFi setup function
bool connectToWiFi(); // New helper function

// --- Global Constants ---
const char* DEFAULT_DOWNTIME_REASONS[5] = {
  "Maintenance", "Material Shortage", "Operator Break", "Equipment Failure", "Changeover"
};

// --- Global Variables ---
Config config;
WebServer server(80);
Preferences prefs;
AssetState assetStates[MAX_ASSETS];
char wifi_ssid[33] = "";
char wifi_pass[65] = "";

// --- Local Assets for Offline Functionality ---
// Minimal Bootstrap CSS (critical styles only)
const char MINIMAL_BOOTSTRAP_CSS[] PROGMEM = R"(
*,*::before,*::after{box-sizing:border-box}
body{margin:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;font-size:1rem;font-weight:400;line-height:1.5;color:#212529;background-color:#fff}
.container,.container-fluid{width:100%;padding-right:var(--bs-gutter-x,.75rem);padding-left:var(--bs-gutter-x,.75rem);margin-right:auto;margin-left:auto}
@media (min-width:576px){.container{max-width:540px}}
@media (min-width:768px){.container{max-width:720px}}
@media (min-width:992px){.container{max-width:960px}}
@media (min-width:1200px){.container{max-width:1140px}}
.row{--bs-gutter-x:1.5rem;--bs-gutter-y:0;display:flex;flex-wrap:wrap;margin-top:calc(-1 * var(--bs-gutter-y));margin-right:calc(-.5 * var(--bs-gutter-x));margin-left:calc(-.5 * var(--bs-gutter-x))}
.col,.col-auto,.col-1,.col-2,.col-3,.col-4,.col-5,.col-6,.col-7,.col-8,.col-9,.col-10,.col-11,.col-12,.col-md-1,.col-md-2,.col-md-3,.col-md-4,.col-md-5,.col-md-6,.col-md-7,.col-md-8,.col-md-9,.col-md-10,.col-md-11,.col-md-12{flex-shrink:0;width:100%;max-width:100%;padding-right:calc(var(--bs-gutter-x) * .5);padding-left:calc(var(--bs-gutter-x) * .5);margin-top:var(--bs-gutter-y)}
.col-1{flex:0 0 auto;width:8.33333333%}.col-2{flex:0 0 auto;width:16.66666667%}.col-3{flex:0 0 auto;width:25%}.col-4{flex:0 0 auto;width:33.33333333%}.col-6{flex:0 0 auto;width:50%}.col-12{flex:0 0 auto;width:100%}
@media (min-width:768px){.col-md-2{flex:0 0 auto;width:16.66666667%}.col-md-3{flex:0 0 auto;width:25%}.col-md-4{flex:0 0 auto;width:33.33333333%}.col-md-6{flex:0 0 auto;width:50%}.col-md-8{flex:0 0 auto;width:66.66666667%}}
.table{--bs-table-color-type:initial;--bs-table-bg-type:initial;--bs-table-color-state:initial;--bs-table-bg-state:initial;--bs-table-color:#212529;--bs-table-bg:transparent;--bs-table-border-color:#dee2e6;--bs-table-accent-bg:transparent;--bs-table-striped-color:#212529;--bs-table-striped-bg:rgba(0,0,0,.05);--bs-table-active-color:#212529;--bs-table-active-bg:rgba(0,0,0,.1);--bs-table-hover-color:#212529;--bs-table-hover-bg:rgba(0,0,0,.075);width:100%;margin-bottom:1rem;color:var(--bs-table-color);vertical-align:top;border-color:var(--bs-table-border-color)}
.table>:not(caption)>*>*{padding:.5rem .5rem;background-color:var(--bs-table-bg);border-bottom-width:1px;box-shadow:inset 0 0 0 9999px var(--bs-table-bg-state,var(--bs-table-bg-type,var(--bs-table-accent-bg)))}
.table-striped>tbody>tr:nth-of-type(odd)>td,.table-striped>tbody>tr:nth-of-type(odd)>th{--bs-table-color-type:var(--bs-table-striped-color);--bs-table-bg-type:var(--bs-table-striped-bg)}
.table-hover>tbody>tr:hover>td,.table-hover>tbody>tr:hover>th{--bs-table-color-type:var(--bs-table-hover-color);--bs-table-bg-type:var(--bs-table-hover-bg)}
.table-sm>:not(caption)>*>*{padding:.25rem .25rem}
.table-responsive{overflow-x:auto;-webkit-overflow-scrolling:touch}
.btn{--bs-btn-padding-x:0.75rem;--bs-btn-padding-y:0.375rem;--bs-btn-font-size:1rem;--bs-btn-font-weight:400;--bs-btn-line-height:1.5;--bs-btn-color:#212529;--bs-btn-bg:transparent;--bs-btn-border-width:1px;--bs-btn-border-color:transparent;--bs-btn-border-radius:0.375rem;--bs-btn-hover-border-color:transparent;--bs-btn-box-shadow:inset 0 1px 0 rgba(255,255,255,.15),0 1px 1px rgba(0,0,0,.075);--bs-btn-disabled-opacity:0.65;--bs-btn-focus-box-shadow:0 0 0 0.25rem rgba(var(--bs-btn-focus-shadow-rgb),.5);display:inline-block;padding:var(--bs-btn-padding-y) var(--bs-btn-padding-x);margin-bottom:0;font-size:var(--bs-btn-font-size);font-weight:var(--bs-btn-font-weight);line-height:var(--bs-btn-line-height);color:var(--bs-btn-color);text-align:center;text-decoration:none;vertical-align:middle;cursor:pointer;-webkit-user-select:none;-moz-user-select:none;user-select:none;border:var(--bs-btn-border-width) solid var(--bs-btn-border-color);border-radius:var(--bs-btn-border-radius);background-color:var(--bs-btn-bg);transition:color .15s ease-in-out,background-color .15s ease-in-out,border-color .15s ease-in-out,box-shadow .15s ease-in-out}
.btn:hover{color:var(--bs-btn-hover-color);background-color:var(--bs-btn-hover-bg);border-color:var(--bs-btn-hover-border-color)}
.btn-primary{--bs-btn-color:#fff;--bs-btn-bg:#0d6efd;--bs-btn-border-color:#0d6efd;--bs-btn-hover-color:#fff;--bs-btn-hover-bg:#0b5ed7;--bs-btn-hover-border-color:#0a58ca;--bs-btn-focus-shadow-rgb:49,132,253;--bs-btn-active-color:#fff;--bs-btn-active-bg:#0a58ca;--bs-btn-active-border-color:#0a53be;--bs-btn-active-shadow:inset 0 3px 5px rgba(0,0,0,.125);--bs-btn-disabled-color:#fff;--bs-btn-disabled-bg:#0d6efd;--bs-btn-disabled-border-color:#0d6efd}
.btn-secondary{--bs-btn-color:#fff;--bs-btn-bg:#6c757d;--bs-btn-border-color:#6c757d;--bs-btn-hover-color:#fff;--bs-btn-hover-bg:#5c636a;--bs-btn-hover-border-color:#565e64;--bs-btn-focus-shadow-rgb:130,138,145;--bs-btn-active-color:#fff;--bs-btn-active-bg:#565e64;--bs-btn-active-border-color:#51585e;--bs-btn-active-shadow:inset 0 3px 5px rgba(0,0,0,.125);--bs-btn-disabled-color:#fff;--bs-btn-disabled-bg:#6c757d;--bs-btn-disabled-border-color:#6c757d}
.btn-success{--bs-btn-color:#fff;--bs-btn-bg:#198754;--bs-btn-border-color:#198754;--bs-btn-hover-color:#fff;--bs-btn-hover-bg:#157347;--bs-btn-hover-border-color:#146c43;--bs-btn-focus-shadow-rgb:60,153,110;--bs-btn-active-color:#fff;--bs-btn-active-bg:#146c43;--bs-btn-active-border-color:#13653f;--bs-btn-active-shadow:inset 0 3px 5px rgba(0,0,0,.125);--bs-btn-disabled-color:#fff;--bs-btn-disabled-bg:#198754;--bs-btn-disabled-border-color:#198754}
.btn-info{--bs-btn-color:#000;--bs-btn-bg:#0dcaf0;--bs-btn-border-color:#0dcaf0;--bs-btn-hover-color:#000;--bs-btn-hover-bg:#31d2f2;--bs-btn-hover-border-color:#25cff2;--bs-btn-focus-shadow-rgb:11,172,204;--bs-btn-active-color:#000;--bs-btn-active-bg:#3dd5f3;--bs-btn-active-border-color:#25cff2;--bs-btn-active-shadow:inset 0 3px 5px rgba(0,0,0,.125);--bs-btn-disabled-color:#000;--bs-btn-disabled-bg:#0dcaf0;--bs-btn-disabled-border-color:#0dcaf0}
.btn-warning{--bs-btn-color:#000;--bs-btn-bg:#ffc107;--bs-btn-border-color:#ffc107;--bs-btn-hover-color:#000;--bs-btn-hover-bg:#ffca2c;--bs-btn-hover-border-color:#ffc720;--bs-btn-focus-shadow-rgb:217,164,6;--bs-btn-active-color:#000;--bs-btn-active-bg:#ffcd39;--bs-btn-active-border-color:#ffc720;--bs-btn-active-shadow:inset 0 3px 5px rgba(0,0,0,.125);--bs-btn-disabled-color:#000;--bs-btn-disabled-bg:#ffc107;--bs-btn-disabled-border-color:#ffc107}
.btn-danger{--bs-btn-color:#fff;--bs-btn-bg:#dc3545;--bs-btn-border-color:#dc3545;--bs-btn-hover-color:#fff;--bs-btn-hover-bg:#bb2d3b;--bs-btn-hover-border-color:#b02a37;--bs-btn-focus-shadow-rgb:225,83,97;--bs-btn-active-color:#fff;--bs-btn-active-bg:#b02a37;--bs-btn-active-border-color:#a52834;--bs-btn-active-shadow:inset 0 3px 5px rgba(0,0,0,.125);--bs-btn-disabled-color:#fff;--bs-btn-disabled-bg:#dc3545;--bs-btn-disabled-border-color:#dc3545}
.btn-dark{--bs-btn-color:#fff;--bs-btn-bg:#212529;--bs-btn-border-color:#212529;--bs-btn-hover-color:#fff;--bs-btn-hover-bg:#424649;--bs-btn-hover-border-color:#373b3e;--bs-btn-focus-shadow-rgb:66,70,73;--bs-btn-active-color:#fff;--bs-btn-active-bg:#4d5154;--bs-btn-active-border-color:#373b3e;--bs-btn-active-shadow:inset 0 3px 5px rgba(0,0,0,.125);--bs-btn-disabled-color:#fff;--bs-btn-disabled-bg:#212529;--bs-btn-disabled-border-color:#212529}
.card{--bs-card-spacer-y:1rem;--bs-card-spacer-x:1rem;--bs-card-title-spacer-y:0.5rem;--bs-card-title-color:#212529;--bs-card-subtitle-color:#6c757d;--bs-card-border-width:1px;--bs-card-border-color:rgba(0,0,0,.125);--bs-card-border-radius:0.375rem;--bs-card-box-shadow:0 0.125rem 0.25rem rgba(0,0,0,.075);--bs-card-inner-border-radius:calc(0.375rem - 1px);--bs-card-cap-padding-y:0.5rem;--bs-card-cap-padding-x:1rem;--bs-card-cap-bg:rgba(0,0,0,.03);--bs-card-cap-color:inherit;--bs-card-height:auto;--bs-card-color:inherit;--bs-card-bg:#fff;--bs-card-img-overlay-padding:1rem;--bs-card-group-margin:0.75rem;position:relative;display:flex;flex-direction:column;min-width:0;height:var(--bs-card-height);color:var(--bs-card-color);word-wrap:break-word;background-color:var(--bs-card-bg);background-clip:border-box;border:var(--bs-card-border-width) solid var(--bs-card-border-color);border-radius:var(--bs-card-border-radius);box-shadow:var(--bs-card-box-shadow)}
.card-body{flex:1 1 auto;padding:var(--bs-card-spacer-y) var(--bs-card-spacer-x);color:var(--bs-card-color)}
.card-header{padding:var(--bs-card-cap-padding-y) var(--bs-card-cap-padding-x);margin-bottom:0;color:var(--bs-card-cap-color);background-color:var(--bs-card-cap-bg);border-bottom:var(--bs-card-border-width) solid var(--bs-card-border-color)}
.navbar{--bs-navbar-padding-x:0;--bs-navbar-padding-y:0.5rem;--bs-navbar-color:rgba(255,255,255,.55);--bs-navbar-hover-color:rgba(255,255,255,.75);--bs-navbar-disabled-color:rgba(255,255,255,.25);--bs-navbar-active-color:#fff;--bs-navbar-brand-padding-y:0.3125rem;--bs-navbar-brand-margin-end:1rem;--bs-navbar-brand-font-size:1.25rem;--bs-navbar-brand-color:#fff;--bs-navbar-brand-hover-color:#fff;--bs-navbar-nav-link-padding-x:0.5rem;--bs-navbar-toggler-padding-y:0.25rem;--bs-navbar-toggler-padding-x:0.75rem;--bs-navbar-toggler-font-size:1.25rem;--bs-navbar-toggler-icon-bg:url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 30'%3e%3cpath stroke='rgba%2833, 37, 41, 0.75%29' stroke-linecap='round' stroke-miterlimit='10' stroke-width='2' d='M4 7h22M4 15h22M4 23h22'/%3e%3c/svg%3e");--bs-navbar-toggler-border-color:rgba(0,0,0,.1);--bs-navbar-toggler-border-radius:0.375rem;--bs-navbar-toggler-focus-width:0.25rem;--bs-navbar-toggler-transition:box-shadow .15s ease-in-out;position:relative;display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;padding:var(--bs-navbar-padding-y) var(--bs-navbar-padding-x)}
.navbar-brand{padding-top:var(--bs-navbar-brand-padding-y);padding-bottom:var(--bs-navbar-brand-padding-y);margin-right:var(--bs-navbar-brand-margin-end);font-size:var(--bs-navbar-brand-font-size);color:var(--bs-navbar-brand-color);text-decoration:none;white-space:nowrap}
.navbar-nav{--bs-nav-link-padding-x:0;--bs-nav-link-padding-y:0.5rem;--bs-nav-link-font-weight:;--bs-nav-link-color:var(--bs-navbar-color);--bs-nav-link-hover-color:var(--bs-navbar-hover-color);--bs-nav-link-disabled-color:var(--bs-navbar-disabled-color);display:flex;flex-direction:column;padding-left:0;margin-bottom:0;list-style:none}
.navbar-dark{--bs-navbar-color:rgba(255,255,255,.55);--bs-navbar-hover-color:rgba(255,255,255,.75);--bs-navbar-disabled-color:rgba(255,255,255,.25);--bs-navbar-active-color:#fff;--bs-navbar-brand-color:#fff;--bs-navbar-brand-hover-color:#fff;--bs-navbar-toggler-border-color:rgba(255,255,255,.1);--bs-navbar-toggler-icon-bg:url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 30'%3e%3cpath stroke='rgba%28255, 255, 255, 0.55%29' stroke-linecap='round' stroke-miterlimit='10' stroke-width='2' d='M4 7h22M4 15h22M4 23h22'/%3e%3c/svg%3e")}
.bg-dark{--bs-bg-opacity:1;background-color:rgba(33,37,41,var(--bs-bg-opacity))!important}
.badge{--bs-badge-padding-x:0.65em;--bs-badge-padding-y:0.35em;--bs-badge-font-size:0.75em;--bs-badge-font-weight:700;--bs-badge-color:#fff;--bs-badge-border-radius:0.375rem;display:inline-block;padding:var(--bs-badge-padding-y) var(--bs-badge-padding-x);font-size:var(--bs-badge-font-size);font-weight:var(--bs-badge-font-weight);line-height:1;color:var(--bs-badge-color);text-align:center;white-space:nowrap;vertical-align:baseline;border-radius:var(--bs-badge-border-radius)}
.bg-success{--bs-bg-opacity:1;background-color:rgba(25,135,84,var(--bs-bg-opacity))!important}
.bg-danger{--bs-bg-opacity:1;background-color:rgba(220,53,69,var(--bs-bg-opacity))!important}
.bg-warning{--bs-bg-opacity:1;background-color:rgba(255,193,7,var(--bs-bg-opacity))!important}
.bg-info{--bs-bg-opacity:1;background-color:rgba(13,202,240,var(--bs-bg-opacity))!important}
.bg-primary{--bs-bg-opacity:1;background-color:rgba(13,110,253,var(--bs-bg-opacity))!important}
.text-dark{--bs-text-opacity:1;color:rgba(33,37,41,var(--bs-text-opacity))!important}
.text-muted{--bs-text-opacity:1;color:#6c757d!important}
.text-center{text-align:center!important}
.text-end{text-align:right!important}
.d-flex{display:flex!important}
.d-none{display:none!important}
.d-block{display:block!important}
.d-inline-block{display:inline-block!important}
.justify-content-between{justify-content:space-between!important}
.align-items-center{align-items:center!important}
.align-items-start{align-items:flex-start!important}
.align-items-end{align-items:flex-end!important}
.align-middle{vertical-align:middle!important}
.flex-wrap{flex-wrap:wrap!important}
.mb-0{margin-bottom:0!important}
.mb-1{margin-bottom:0.25rem!important}
.mb-2{margin-bottom:0.5rem!important}
.mb-3{margin-bottom:1rem!important}
.mb-4{margin-bottom:1.5rem!important}
.mt-1{margin-top:0.25rem!important}
.mt-2{margin-top:0.5rem!important}
.mt-3{margin-top:1rem!important}
.mt-4{margin-top:1.5rem!important}
.ms-auto{margin-left:auto!important}
.ms-3{margin-left:1rem!important}
.me-2{margin-right:0.5rem!important}
.me-3{margin-right:1rem!important}
.p-2{padding:0.5rem!important}
.p-3{padding:1rem!important}
.pt-2{padding-top:0.5rem!important}
.pb-2{padding-bottom:0.5rem!important}
.px-3{padding-left:1rem!important;padding-right:1rem!important}
.py-2{padding-top:0.5rem!important;padding-bottom:0.5rem!important}
.w-100{width:100%!important}
.h1,.h2,.h3,.h4,.h5,.h6,h1,h2,h3,h4,h5,h6{margin-top:0;margin-bottom:0.5rem;font-weight:500;line-height:1.2}
.h1,h1{font-size:calc(1.375rem + 1.5vw)}
.h2,h2{font-size:calc(1.325rem + 0.9vw)}
.h3,h3{font-size:calc(1.3rem + 0.6vw)}
.h4,h4{font-size:calc(1.275rem + 0.3vw)}
.h5,h5{font-size:1.25rem}
.h6,h6{font-size:1rem}
.form-label{margin-bottom:0.5rem}
.form-control,.form-select{display:block;width:100%;padding:0.375rem 0.75rem;font-size:1rem;font-weight:400;line-height:1.5;color:#212529;background-color:#fff;background-image:none;border:1px solid #ced4da;-webkit-appearance:none;-moz-appearance:none;appearance:none;border-radius:0.375rem;transition:border-color .15s ease-in-out,box-shadow .15s ease-in-out}
.form-control:focus,.form-select:focus{color:#212529;background-color:#fff;border-color:#86b7fe;outline:0;box-shadow:0 0 0 0.25rem rgba(13,110,253,.25)}
.shadow-sm{box-shadow:0 0.125rem 0.25rem rgba(0,0,0,.075)!important}
.modal{--bs-modal-zindex:1055;--bs-modal-width:500px;--bs-modal-padding:1rem;--bs-modal-margin:0.5rem;--bs-modal-color:;--bs-modal-bg:#fff;--bs-modal-border-color:rgba(0,0,0,.2);--bs-modal-border-width:1px;--bs-modal-border-radius:0.5rem;--bs-modal-box-shadow:0 0.125rem 0.25rem rgba(0,0,0,.075);--bs-modal-inner-border-radius:calc(0.5rem - 1px);--bs-modal-header-padding-x:1rem;--bs-modal-header-padding-y:1rem;--bs-modal-header-padding:1rem 1rem;--bs-modal-header-border-color:rgba(0,0,0,.2);--bs-modal-header-border-width:1px;--bs-modal-title-line-height:1.5;--bs-modal-footer-gap:0.5rem;--bs-modal-footer-border-color:rgba(0,0,0,.2);--bs-modal-footer-border-width:1px;position:fixed;top:0;left:0;z-index:var(--bs-modal-zindex);display:none;width:100%;height:100%;overflow-x:hidden;overflow-y:auto;outline:0}
.g-3{--bs-gutter-x:1rem;--bs-gutter-y:1rem}
@media (max-width:700px){@media (max-width:700px){#eventTable{display:none}}@media (min-width:701px){#mobileEvents{display:none}}}
)";

// Minimal Bootstrap Icons (just the essential ones)
const char MINIMAL_BOOTSTRAP_ICONS[] PROGMEM = R"(
.bi{display:inline-block;width:1em;height:1em;fill:currentColor;vertical-align:-.125em}
.bi-trophy-fill::before{content:"\f1b3"}
.bi-star-fill::before{content:"\f586"}
.bi-exclamation-triangle-fill::before{content:"\f33a"}
.bi-stopwatch-fill::before{content:"\f59d"}
.bi::before{font-family:"Bootstrap Icons";font-size:1em;font-style:normal;font-weight:normal!important;font-variant:normal;text-transform:none;line-height:1;vertical-align:-.125em;-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale}
)";

// Simple Chart.js replacement for basic charts
const char SIMPLE_CHARTS_JS[] PROGMEM = R"(
class SimpleChart{constructor(t,e){this.canvas=t,this.ctx=t.getContext("2d"),this.config=e,this.data=e.data,this.options=e.options||{},this.render()}render(){const t=this.canvas,e=this.ctx,n=this.data;e.clearRect(0,0,t.width,t.height);const i=t.width-80,a=t.height-80;if("bar"===this.config.type)this.renderBarChart(e,n,40,40,i,a);else if("line"===this.config.type)this.renderLineChart(e,n,40,40,i,a)}renderBarChart(t,e,n,i,a,o){const r=e.labels,l=e.datasets[0].data,s=Math.max(...l),h=a/r.length,c=o/s;t.fillStyle="#f0f0f0",t.fillRect(n,i,a,o),t.strokeStyle="#ddd",t.lineWidth=1;for(let e=0;e<=10;e++){const r=i+o-e*o/10;t.beginPath(),t.moveTo(n,r),t.lineTo(n+a,r),t.stroke()}r.forEach(((e,r)=>{const s=l[r]*c,u=n+r*h+h/4,f=i+o-s,d=h/2;t.fillStyle="rgba(66,165,245,0.7)",t.fillRect(u,f,d,s),t.fillStyle="#333",t.font="12px Arial",t.textAlign="center",t.fillText(e,u+d/2,i+o+15),t.textAlign="right",t.fillText(l[r].toFixed(1),n-5,f+s/2+3)}))}renderLineChart(t,e,n,i,a,o){const r=e.labels,l=e.datasets[0].data,s=Math.max(...l),h=a/(r.length-1),c=o/s;t.fillStyle="#f0f0f0",t.fillRect(n,i,a,o),t.strokeStyle="#ddd",t.lineWidth=1;for(let e=0;e<=10;e++){const r=i+o-e*o/10;t.beginPath(),t.moveTo(n,r),t.lineTo(n+a,r),t.stroke()}t.strokeStyle="rgba(66,165,245,1)",t.lineWidth=2,t.beginPath();for(let e=0;e<r.length;e++){const t=n+e*h,a=i+o-l[e]*c;0===e?this.ctx.moveTo(t,a):this.ctx.lineTo(t,a)}t.stroke(),t.fillStyle="rgba(66,165,245,1)",r.forEach(((e,r)=>{const s=n+r*h,u=i+o-l[r]*c;t.beginPath(),t.arc(s,u,3,0,2*Math.PI),t.fill(),t.fillStyle="#333",t.font="12px Arial",t.textAlign="center",t.fillText(e,s,i+o+15)}))}}
function Chart(t,e){return new SimpleChart(t,e)}
)";

// Minimal Bootstrap JS functionality
const char MINIMAL_BOOTSTRAP_JS[] PROGMEM = R"(
(function(){
class Modal{constructor(t){this.element=t,this.backdrop=null}show(){this.element.style.display="block",this.element.classList.add("show"),this.createBackdrop()}hide(){this.element.style.display="none",this.element.classList.remove("show"),this.removeBackdrop()}createBackdrop(){this.backdrop=document.createElement("div"),this.backdrop.className="modal-backdrop fade show",this.backdrop.style.cssText="position:fixed;top:0;left:0;z-index:1050;width:100vw;height:100vh;background-color:#000;opacity:0.5",document.body.appendChild(this.backdrop),this.backdrop.addEventListener("click",(()=>this.hide()))}removeBackdrop(){this.backdrop&&(document.body.removeChild(this.backdrop),this.backdrop=null)}}
window.bootstrap={Modal:Modal};
document.addEventListener("click",(function(t){t.target.matches('[data-bs-dismiss="modal"]')&&t.target.closest(".modal")&&new Modal(t.target.closest(".modal")).hide()}));
})();
)";

// Forward declarations for asset serving functions
void handleBootstrapCSS();
void handleBootstrapJS();
void handleChartsJS();
void handleBootstrapIcons();

bool g_isSystemSerialDown = false;
char g_serialSystemTriggerAssetName[32] = "";
time_t g_systemLastStateChangeTime = 0;
unsigned long g_systemTotalRunningTimeSecs = 0;
unsigned long g_systemTotalStoppedTimeSecs = 0;
uint32_t g_systemStopCount = 0;
bool g_systemStateInitialized = false;

int g_currentShiftIndex = -1;
time_t g_currentShiftStartTimeEpoch = 0;
bool g_shiftFeatureInitialized = false;

// --- Local Asset Serving Functions (NEW) ---
void handleBootstrapCSS() {
  server.sendHeader("Content-Type", "text/css");
  server.sendHeader("Cache-Control", "public, max-age=86400"); // Cache for 24 hours
  server.send_P(200, "text/css", MINIMAL_BOOTSTRAP_CSS);
}

void handleBootstrapJS() {
  server.sendHeader("Content-Type", "application/javascript");
  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.send_P(200, "application/javascript", MINIMAL_BOOTSTRAP_JS);
}

void handleChartsJS() {
  server.sendHeader("Content-Type", "application/javascript");
  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.send_P(200, "application/javascript", SIMPLE_CHARTS_JS);
}

void handleBootstrapIcons() {
  server.sendHeader("Content-Type", "text/css");
  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.send_P(200, "text/css", MINIMAL_BOOTSTRAP_ICONS);
}

// --- Enhanced WiFi Functions (NEW/ENHANCED) ---

// Load stored WiFi credentials
void loadWiFiCredentials() {
  prefs.begin("assetmon", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  
  if (ssid.length() > 0) {
    strncpy(wifi_ssid, ssid.c_str(), 32);
    wifi_ssid[32] = '\0';
    strncpy(wifi_pass, pass.c_str(), 64);
    wifi_pass[64] = '\0';
  }
  prefs.end();
}

// Try to connect to stored WiFi network
bool connectToWiFi() {
  if (strlen(wifi_ssid) == 0) {
    Serial.println("No stored WiFi credentials found");
    return false;
  }

  displayWiFiStatus("Connecting to WiFi", wifi_ssid);
  currentWiFiState = WIFI_CONNECTING_STA;
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_pass);
  
  wifiConnectStartTime = millis();
  Serial.printf("Attempting to connect to: %s\n", wifi_ssid);
  
  // Wait for connection with timeout
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiConnectStartTime) < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print(".");
    
    // Update OLED every second during connection attempt
    if (millis() - lastOledUpdate > OLED_UPDATE_INTERVAL) {
      unsigned long elapsed = (millis() - wifiConnectStartTime) / 1000;
      char timeStr[16];
      sprintf(timeStr, "(%lu sec)", elapsed);
      displayWiFiStatus("Connecting to WiFi", timeStr);
      lastOledUpdate = millis();
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    currentWiFiState = WIFI_STA_CONNECTED;
    Serial.println("\nWiFi connected!");
    Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
    
    displayWiFiStatus("WiFi Connected!", WiFi.localIP().toString().c_str());
    delay(2000); // Show success message
    updateOledDisplay(); // Switch to normal display
    return true;
  } else {
    Serial.println("\nWiFi connection failed!");
    return false;
  }
}

// Start Access Point mode
void startAccessPoint() {
  displayWiFiStatus("Starting AP Mode", "AssetMonitor-Config");
  
  WiFi.mode(WIFI_AP);
  // Use "AssetMonitor-Config" with no password as specified
  bool apStarted = WiFi.softAP("AssetMonitor-Config");
  
  if (apStarted) {
    currentWiFiState = WIFI_AP_MODE;
    Serial.println("Access Point started successfully");
    Serial.printf("AP SSID: AssetMonitor-Config\n");
    Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    
    displayWiFiStatus("AP Mode Active", WiFi.softAPIP().toString().c_str());
    delay(2000); // Show AP info
    updateOledDisplay(); // Switch to normal display
  } else {
    Serial.println("Failed to start Access Point!");
    displayWiFiStatus("AP Start Failed", "Check settings");
  }
}

// Enhanced WiFi setup function (replaces setupWiFiSmart)
void setupWiFiSmartV2() {
  displayBootMessage("Initializing WiFi...");
  
  // Load any stored credentials
  loadWiFiCredentials();
  
  // Always try STA mode first if we have credentials
  bool staConnected = false;
  if (strlen(wifi_ssid) > 0) {
    staConnected = connectToWiFi();
  }
  
  // If STA connection failed, start AP mode
  if (!staConnected) {
    Serial.println("STA connection failed or no credentials. Starting AP mode.");
    displayWiFiStatus("STA Failed", "Starting AP Mode");
    delay(1000);
    startAccessPoint();
  }
}
// --- END Enhanced WiFi Functions ---

// --- Function Implementations (unchanged from original, except where noted) ---

bool deleteOldestLogFile() {
    File root = SPIFFS.open("/");
    File f = root.openNextFile();
    String oldestFile;
    uint32_t oldestTime = UINT32_MAX;
    while (f) {
        String name = f.name();
        if (!f.isDirectory() && name.indexOf("Log-") == 0) {
            uint32_t t = f.getLastWrite();
            if (t < oldestTime) {
                oldestTime = t;
                oldestFile = name;
            }
        }
        f = root.openNextFile();
    }
    if (oldestFile.length()) {
        SPIFFS.remove(oldestFile);
        Serial.println("Deleted oldest log: " + oldestFile);
        return true;
    }
    Serial.println("No log file found to delete.");
    return false;
}

String wifiConfigHTML() {
  String html = "<!DOCTYPE html><html><head><title>WiFi Setup</title>"
                "<meta charset='UTF-8'>"
                "<style>"
                "body { font-family: Arial; margin: 2em; background: #f6f8fa; }"
                "form { background: #fff; padding: 2em; border-radius: 8px; box-shadow: 0 0 8px #ccc; max-width: 400px; margin:auto;}"
                "h1 { color: #0366d6; }"
                "label { display:block; margin-top:1em; }"
                "input[type=text], input[type=password] { width:100%; padding:0.5em; box-sizing: border-box; }"
                "input[type=submit] { background: #0366d6; color: #fff; border: none; padding: 0.7em 1.5em; margin-top:1em; border-radius: 4px; cursor:pointer;}"
                "input[type=submit]:hover { background: #0356b6; }"
                ".note { color: #888; font-size: 0.95em; margin: 1em 0; }"
                "</style>"
                "</head><body>"
                "<form method='POST' action='/wifi_save_config'>"
                "<h1>WiFi Setup</h1>"
                "<label>SSID:</label>"
                "<input type='text' name='ssid' maxlength='32' required value='";
  html += wifi_ssid;
  html += "'>"
          "<label>Password:</label>"
          "<input type='password' name='password' maxlength='64' value='";
  html += wifi_pass;
  html += "'>"
          "<div class='note'>Enter your WiFi details above. Device will reboot after saving.</div>"
          "<input type='submit' value='Save & Reboot'>"
          "</form>"
          "</body></html>";
  return html;
}

void handleWifiConfigPost() {
  if (server.hasArg("ssid")) {
    String ssid_arg = server.arg("ssid");
    String pass_arg = server.arg("password");
    strncpy(wifi_ssid, ssid_arg.c_str(), 32);
    wifi_ssid[32] = '\0';
    strncpy(wifi_pass, pass_arg.c_str(), 64);
    wifi_pass[64] = '\0';
    
    // Use the global prefs object for saving WiFi credentials
    prefs.begin("assetmon", false); // Ensure it's open for write
    prefs.putString("ssid", wifi_ssid);
    prefs.putString("pass", wifi_pass);
    prefs.end();

    server.send(200, "text/html", "<h2>Saved! Rebooting...</h2><meta http-equiv='refresh' content='3;url=/' />");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing WiFi credentials");
  }
}

// Remove the old startConfigPortal function - it's replaced by our enhanced AP mode

// This function is now replaced by setupWiFiSmartV2
void setupWiFiSmart() {
  setupWiFiSmartV2(); // Use the enhanced version
}

void loadConfig() {
  Preferences localPrefs;
  bool configLoaded = false; 
  bool prefsWereOpened = localPrefs.begin("assetmon", true); 

  if (prefsWereOpened && localPrefs.isKey("cfg")) {
    char tempConfigBuffer[sizeof(Config) + 128]; // Increased buffer slightly for future-proofing
    size_t len = localPrefs.getBytes("cfg", tempConfigBuffer, sizeof(tempConfigBuffer));

    if (len > 0 && len <= sizeof(Config)) { 
      memcpy(&config, tempConfigBuffer, len); 
      if (len < sizeof(Config)) {
        Serial.println("loadConfig: Loaded config is smaller than current struct. Initializing new/trailing fields.");
        char* newFieldsStart = (char*)&config + len;
        size_t newFieldsSize = sizeof(Config) - len;
        memset(newFieldsStart, 0, newFieldsSize); 

        // Sensible defaults for potentially new fields if not covered by memset(0) or needing specific values
        // This primarily targets the shift configuration if it was added.
        bool recheckShiftDefaults = false;
        // Check if the part of the struct containing shift info was part of the uninitialized (memset) block
        if ((char*)&config.enableShiftArchiving >= newFieldsStart) {
            recheckShiftDefaults = true;
        }

        if (recheckShiftDefaults) {
             config.enableShiftArchiving = false; 
             config.numShifts = 3; 
             strcpy(config.shifts[0].startTime, "06:00");
             strcpy(config.shifts[1].startTime, "14:00");
             strcpy(config.shifts[2].startTime, "22:00");
             for(uint8_t i = config.numShifts; i < MAX_CONFIGURABLE_SHIFTS; ++i) {
                 strcpy(config.shifts[i].startTime, "00:00");
             }
             Serial.println("loadConfig: Applied default values for new shift config fields after struct upgrade.");
        }
      }
      configLoaded = true;
      Serial.println("loadConfig: Configuration loaded from Preferences.");
    } else if (len > sizeof(Config)) {
      Serial.println("loadConfig: Saved config is LARGER than current struct! Possible corruption or version issue. Using defaults.");
      configLoaded = false; 
    } else { 
      Serial.println("loadConfig: Config read error (len=0 or other). Using defaults.");
      configLoaded = false;
    }
  }

  if (!configLoaded) {
    Serial.println("loadConfig: No 'cfg' key or load error. Using defaults and saving.");
    config.assetCount = 2; 
    config.maxEvents = 1000;
    strcpy(config.assets[0].name, "Line 1"); config.assets[0].pin = 4;
    strcpy(config.assets[1].name, "Line 2"); config.assets[1].pin = 12;
    for (uint8_t i = config.assetCount; i < MAX_ASSETS; ++i) { strcpy(config.assets[i].name, ""); config.assets[i].pin = 0; }
    for (int i = 0; i < 5; ++i) { strncpy(config.downtimeReasons[i], DEFAULT_DOWNTIME_REASONS[i], 31); config.downtimeReasons[i][31] = '\0';}
    config.tzOffset = 0; config.longStopThresholdSec = 300; config.monitoringMode = MONITORING_MODE_PARALLEL;
    
    config.enableShiftArchiving = false;
    config.numShifts = 3; 
    strcpy(config.shifts[0].startTime, "06:00");
    strcpy(config.shifts[1].startTime, "14:00");
    strcpy(config.shifts[2].startTime, "22:00");
    for (uint8_t i = config.numShifts; i < MAX_CONFIGURABLE_SHIFTS; ++i) { strcpy(config.shifts[i].startTime, "00:00"); }
    
    if(prefsWereOpened) localPrefs.end(); 
    saveConfig(); // saveConfig will open prefs for write
  }
  
  // Final validation pass on critical config values, especially shifts
  if (config.assetCount == 0 || config.assetCount > MAX_ASSETS) config.assetCount = 1;
  if (config.maxEvents < 100) config.maxEvents = 100;
  if (config.enableShiftArchiving) {
      if (config.numShifts == 0 || config.numShifts > MAX_CONFIGURABLE_SHIFTS) {
          config.numShifts = (config.numShifts == 0 && MAX_CONFIGURABLE_SHIFTS > 0) ? 1 : MAX_CONFIGURABLE_SHIFTS; 
          Serial.printf("loadConfig: Corrected numShifts to %u for enabled archiving.\n", config.numShifts);
      }
      // Ensure all active shift start times are valid HH:MM format
      for (uint8_t i = 0; i < config.numShifts; i++) {
          if (strlen(config.shifts[i].startTime) != 5 || config.shifts[i].startTime[2] != ':') {
              Serial.printf("loadConfig: Invalid time format for shift %d ('%s'). Defaulting to 00:00.\n", i + 1, config.shifts[i].startTime);
              strcpy(config.shifts[i].startTime, "00:00"); // A safe default, user should reconfigure
          }
      }
  } else {
      // If archiving is disabled, numShifts value is less critical but ensure it's not out of bounds if accessed
      if (config.numShifts > MAX_CONFIGURABLE_SHIFTS) config.numShifts = 0;
  }

  if(prefsWereOpened && localPrefs.freeEntries() > 0 ) localPrefs.end();
  
  Serial.printf("Shift Archiving: %s, Num Shifts: %u\n", config.enableShiftArchiving ? "Yes" : "No", config.numShifts);
  if(config.enableShiftArchiving && config.numShifts > 0){
    for(uint8_t i=0; i<config.numShifts; i++){
        Serial.printf("  Shift %u Start: %s\n", i+1, config.shifts[i].startTime);
    }
  }
}

void saveConfig() {
  Preferences localSavePrefs; // Your existing code uses a local Preferences object here

  if (!localSavePrefs.begin("assetmon", false)) { // false for read-write
    Serial.println("saveConfig: Failed to begin preferences for writing.");
    return;
  }

  if (localSavePrefs.putBytes("cfg", &config, sizeof(config)) == sizeof(config)) {
     Serial.println("saveConfig: Configuration saved successfully.");
  } else {
     Serial.println("saveConfig: Error writing configuration to preferences.");
  }
  localSavePrefs.end();
}

void ensureLogFileHasHeader() {
  if (!SPIFFS.exists(LOG_FILENAME) || SPIFFS.open(LOG_FILENAME, FILE_READ).size() == 0) {
    File f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
    if (f) {
      f.println("Date,Time,AssetName,EventType,State,Availability,Runtime,Downtime,MTBF,MTTR,Stops,RunDuration,StopDuration,Note");
      f.close();
    }
  }
}

void setupTime() {
  // Step 1: Start NTP to get the accurate universal time (UTC).
  // The first two arguments are temporary offsets and will be overridden by the TZ string later.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync...");
  time_t now = time(nullptr);
  int retry = 0;
  // Wait until the time is valid (post-2021 in this case)
  while (now < 1609459200 && retry < 100) { // Increased retries slightly
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    retry++;
  }
  Serial.println(" done");

  if (now < 1609459200) {
    Serial.println("NTP time sync FAILED. Timestamps will be incorrect!");
    // We'll still set the TZ rule in case time syncs later.
  } else {
    Serial.println("NTP sync successful. Base UTC time acquired.");
  }
  
  // Step 2: Now that base time is established, set the specific local timezone rules.
  // This string explicitly defines GMT (standard) and BST (daylight, 1 hour ahead of GMT).
  // It also contains the exact rules for when the switch happens each year.
  setenv("TZ", "GMT0BST-1,M3.5.0/1,M10.5.0/2", 1);
  tzset(); // Applies the new TZ environment variable

  // Step 3: Display the final, correctly converted local time.
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    Serial.printf("Current local time (GMT/BST): %s", asctime(&timeinfo));
  } else {
    Serial.println("Failed to obtain local time.");
  }
}// --- New function to log system-wide events (for Serial mode) ---
void logSystemEvent(bool systemIsStarting, time_t now, const char* triggerAssetNameOrReason) {
  if (config.monitoringMode != MONITORING_MODE_SERIAL) return;

  struct tm* ti = localtime(&now);
  char datebuf[11], timebuf[9];
  strftime(datebuf, sizeof(datebuf), "%d/%m/%Y", ti);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti);

  unsigned long currentTotalRun = g_systemTotalRunningTimeSecs;
  unsigned long currentTotalStop = g_systemTotalStoppedTimeSecs;
  // Note: Durations *leading up to this event* were added in loop().
  // For display at the moment of the event, these are the correct totals *before* this event's impact on future accumulation.

  float sysAvail = (currentTotalRun + currentTotalStop) > 0
                   ? (100.0f * currentTotalRun / (currentTotalRun + currentTotalStop))
                   : (systemIsStarting ? 100.0f : 0.0f);
  float sysRTM_minutes = (float)currentTotalRun / 60.0f;
  float sysSTM_minutes = (float)currentTotalStop / 60.0f;

  float sysMTBF_minutes = (g_systemStopCount > 0)
                          ? ((float)currentTotalRun / g_systemStopCount / 60.0f)
                          : (currentTotalRun > 0 ? sysRTM_minutes : 0.0f);
  float sysMTTR_minutes = (g_systemStopCount > 0)
                          ? ((float)currentTotalStop / g_systemStopCount / 60.0f)
                          : 0.0f;

  String eventName = systemIsStarting ? "SYS_START" : "SYS_STOP";
  String assetColumnValue = "SYSTEM"; // Default for SYS_START
  String noteForLog = "";

  if (!systemIsStarting) { // For SYS_STOP
    assetColumnValue = (triggerAssetNameOrReason && strlen(triggerAssetNameOrReason) > 0) ? triggerAssetNameOrReason : "SYSTEM_WIDE";
    noteForLog = "System Stop Triggered";
    if (triggerAssetNameOrReason && strlen(triggerAssetNameOrReason) > 0 && strcmp(triggerAssetNameOrReason, "SYSTEM_WIDE") != 0 && strcmp(triggerAssetNameOrReason, "SYSTEM") != 0) {
        noteForLog = String("Root Cause: ") + triggerAssetNameOrReason;
    }
  } else { // For SYS_START
    noteForLog = (triggerAssetNameOrReason && strlen(triggerAssetNameOrReason) > 0) ? triggerAssetNameOrReason : "System Recovered";
  }

  File f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
  if (!f) {
    Serial.println("logSystemEvent: Failed to open log file!");
    return;
  }
  f.printf("%s,%s,%s,%s,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%u,,, %s\n",
           datebuf, timebuf,
           assetColumnValue.c_str(),
           eventName.c_str(),
           systemIsStarting ? 1 : 0,
           sysAvail, sysRTM_minutes, sysSTM_minutes, sysMTBF_minutes, sysMTTR_minutes,
           g_systemStopCount,
           noteForLog.c_str());
  f.close();

  Serial.printf("SYSTEM Event: %s. AssetCol: %s. Note: %s. SysAvail:%.2f, SysStops:%u\n",
                eventName.c_str(), assetColumnValue.c_str(), noteForLog.c_str(), sysAvail, g_systemStopCount);
}

// ============================================================================
// SHIFT LOGIC FUNCTIONS
// ============================================================================

void initializeShiftState() {
  if (!config.enableShiftArchiving || config.numShifts == 0) {
    g_shiftFeatureInitialized = true;
    g_currentShiftIndex = -1; 
    Serial.println("Shift archiving is disabled or no shifts configured.");
    return;
  }

  time_t now_ts = time(nullptr); // renamed from 'now' to avoid conflict with global 'now' in other functions if any
  struct tm timeinfo;
  localtime_r(&now_ts, &timeinfo);
  
  int bestMatchIndex = -1;
  time_t latestPassedShiftStartEpoch = 0;

  for (uint8_t i = 0; i < config.numShifts; ++i) {
    int shiftH, shiftM;
    if (sscanf(config.shifts[i].startTime, "%d:%d", &shiftH, &shiftM) == 2) {
      struct tm shift_tm_template = timeinfo; 
      shift_tm_template.tm_hour = shiftH; shift_tm_template.tm_min = shiftM; shift_tm_template.tm_sec = 0;
      time_t currentShiftStartToday = mktime(&shift_tm_template);
      time_t currentShiftStartYesterday = currentShiftStartToday - 86400; 

      if (now_ts >= currentShiftStartToday) {
        if (currentShiftStartToday >= latestPassedShiftStartEpoch) { // Use >= for current day to catch exact start time
          latestPassedShiftStartEpoch = currentShiftStartToday;
          bestMatchIndex = i;
        }
      } else if (now_ts >= currentShiftStartYesterday) { // For shifts that started yesterday and crossed midnight
         if (currentShiftStartYesterday >= latestPassedShiftStartEpoch) { // Use >= here too
          latestPassedShiftStartEpoch = currentShiftStartYesterday;
          bestMatchIndex = i;
        }
      }
    }
  }
  
  if (bestMatchIndex != -1) {
    g_currentShiftIndex = bestMatchIndex;
    g_currentShiftStartTimeEpoch = latestPassedShiftStartEpoch;
    Serial.printf("INITIALIZED. Current Active Shift: #%d (%s). Effective Start Time: %lu\n", 
                  g_currentShiftIndex + 1, config.shifts[g_currentShiftIndex].startTime, (unsigned long)g_currentShiftStartTimeEpoch);
  } else { // Fallback if no logic matched (e.g. bad shift times, or just after midnight before any shift today)
    if (config.numShifts > 0) { // Try to pick the "last" shift from yesterday if applicable
        int latestTimeMinutes = -1;
        int fallbackIndex = 0; // Default to first shift
        for(uint8_t i=0; i<config.numShifts; ++i) {
            int sh, sm;
            if(sscanf(config.shifts[i].startTime, "%d:%d", &sh, &sm) == 2) {
                int currentShiftMinutes = sh * 60 + sm;
                if(currentShiftMinutes > latestTimeMinutes) {
                    latestTimeMinutes = currentShiftMinutes;
                    fallbackIndex = i; // This will be the numerically latest starting shift
                }
            }
        }
        g_currentShiftIndex = fallbackIndex;
        struct tm shift_tm_instance = timeinfo;
        sscanf(config.shifts[g_currentShiftIndex].startTime, "%d:%d", &(shift_tm_instance.tm_hour), &(shift_tm_instance.tm_min));
        shift_tm_instance.tm_sec = 0;
        g_currentShiftStartTimeEpoch = mktime(&shift_tm_instance) - 86400; // Assume it started yesterday
        Serial.printf("INITIALIZED (complex fallback). Active Shift: #%d (%s). Effective Start: %lu\n", 
                  g_currentShiftIndex + 1, config.shifts[g_currentShiftIndex].startTime, (unsigned long)g_currentShiftStartTimeEpoch);
    } else {
        Serial.println("INITIALIZED (no shifts defined). Could not determine active shift.");
        g_currentShiftIndex = -1;
        g_currentShiftStartTimeEpoch = now_ts; 
    }
  }
  g_shiftFeatureInitialized = true;
}

void archiveLogAndResetForShift(int endedShiftIndex, const char* endedShiftStartTimeStr) {
  Serial.printf("Attempting to archive log for ended shift #%d (%s)\n", endedShiftIndex + 1, endedShiftStartTimeStr);

  // Check for existence and non-emptiness of the log file
  bool logExistedAndNotEmpty = false;
  if (SPIFFS.exists(LOG_FILENAME)) {
      File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
      if (f && f.size() > 0) logExistedAndNotEmpty = true;
      if (f) f.close();
  }

  // --- BEGIN PATCHED LOG ARCHIVING BLOCK ---
  if (logExistedAndNotEmpty) {
    // No /shiftlogs directory: archive directly to root
    time_t archiveTime = time(nullptr); 
    struct tm timeinfo; localtime_r(&archiveTime, &timeinfo);
    int shiftH_start, shiftM_start; sscanf(endedShiftStartTimeStr, "%d:%d", &shiftH_start, &shiftM_start);
    char newFilename[70];
    snprintf(newFilename, sizeof(newFilename), "/Log-%04d%02d%02d_%02d%02d-S%d-%02d%02d.csv",
            timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
            timeinfo.tm_hour, timeinfo.tm_min, endedShiftIndex + 1, shiftH_start, shiftM_start);
    Serial.printf("Archiving current log to %s\n", newFilename);

    // --- PATCH: Workaround for SPIFFS.rename() limitations with diagnostics ---
    if (SPIFFS.rename(LOG_FILENAME, newFilename)) {
        Serial.println("Log file archived successfully.");
    } else {
        Serial.println("SPIFFS.rename() failed, trying copy+delete workaround.");
        File src = SPIFFS.open(LOG_FILENAME, FILE_READ);
        if (!src) {
            Serial.println("Failed to open source log file for reading!");
        }

        File dst = SPIFFS.open(newFilename, FILE_WRITE);
        // PATCH: Try to free space by deleting oldest log if opening destination fails
        if (!dst) {
            Serial.printf("Failed to open destination file for writing: %s\n", newFilename);
            Serial.println("Attempting to delete oldest log file and retry...");
            if (deleteOldestLogFile()) {
                dst = SPIFFS.open(newFilename, FILE_WRITE);
                if (!dst) {
                    Serial.println("Still failed to open destination after deleting oldest log. Giving up on archiving this shift's log.");
                    if (src) src.close();
                    return;
                }
            } else {
                Serial.println("No log file deleted (none found), cannot recover space.");
                if (src) src.close();
                return;
            }
        }
        // END PATCH

        if (src && dst) {
            size_t bytesCopied = 0;
            while (src.available()) {
                int b = src.read();
                if (b < 0) break;
                dst.write((uint8_t)b);
                bytesCopied++;
            }
            src.close();
            dst.close();
            SPIFFS.remove(LOG_FILENAME);
            Serial.printf("Log file archived via copy+delete workaround. Bytes copied: %u\n", (unsigned)bytesCopied);
            ensureLogFileHasHeader();
        } else {
            Serial.println("Failed to archive log file via copy+delete workaround.");
            if (src) src.close();
            if (dst) dst.close();
        }
    }
} else {
    Serial.printf("Shift %d (%s) ended. Current log file is empty or missing. No archive created.\n", endedShiftIndex + 1, endedShiftStartTimeStr);
}


  Serial.println("Resetting runtime statistics for new shift period.");
  time_t newShiftActualStartTime = time(nullptr); 
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i < MAX_ASSETS) {
      assetStates[i].sessionStart = newShiftActualStartTime;
      assetStates[i].runningTime = 0; assetStates[i].stoppedTime = 0;
      assetStates[i].runCount = 0; assetStates[i].stopCount = 0;
      if (config.assets[i].pin > 0 && config.assets[i].pin < 40) { assetStates[i].lastState = digitalRead(config.assets[i].pin); }
      else { assetStates[i].lastState = true; }
      assetStates[i].lastChangeTime = newShiftActualStartTime; assetStates[i].lastEventTime = newShiftActualStartTime;
      assetStates[i].lastRunDuration = 0; assetStates[i].lastStopDuration = 0;
      logEvent(i, !assetStates[i].lastState, newShiftActualStartTime, "NEW_SHIFT_START"); 
    }
  }
  if (config.monitoringMode == MONITORING_MODE_SERIAL) {
    g_systemTotalRunningTimeSecs = 0; g_systemTotalStoppedTimeSecs = 0;
    g_systemStopCount = 0; g_systemLastStateChangeTime = newShiftActualStartTime;
    g_systemStateInitialized = false; 
    // System state will be re-evaluated in the main loop based on new asset states
    // For an immediate log:
    // bool anyAssetDown = false; char tempTrigger[32] = "";
    // for (uint8_t k=0; k<config.assetCount; ++k) if(assetStates[k].lastState) { anyAssetDown=true; strncpy(tempTrigger, config.assets[k].name, 31); break;}
    // logSystemEvent(!anyAssetDown, newShiftActualStartTime, anyAssetDown ? tempTrigger : "SYSTEM_SHIFT_START_OK");
  }
  
  File f_newlog = SPIFFS.open(LOG_FILENAME, FILE_APPEND); 
  if(f_newlog){
      struct tm *ti_log = localtime(&newShiftActualStartTime);
      char datebuf_log[11], timebuf_log[9];
      strftime(datebuf_log, sizeof(datebuf_log), "%d/%m/%Y", ti_log);
      strftime(timebuf_log, sizeof(timebuf_log), "%H:%M:%S", ti_log);
      const char* newShiftStartTimeDisplay = (g_currentShiftIndex >=0 && g_currentShiftIndex < config.numShifts) ? config.shifts[g_currentShiftIndex].startTime : "??:??";
      f_newlog.printf("%s,%s,SYSTEM,SHIFT_TRANSITION,,,,,,,,,,Ended Shift #%d (%s). New shift (#%d - %s) started.\n", 
               datebuf_log, timebuf_log, 
               endedShiftIndex + 1, endedShiftStartTimeStr,
               g_currentShiftIndex + 1, newShiftStartTimeDisplay);
      f_newlog.close();
      Serial.println("Logged SHIFT_TRANSITION to new log file.");
  }
}

void processShiftLogic() {
  if (!g_shiftFeatureInitialized || !config.enableShiftArchiving || config.numShifts == 0) { return; }
  static time_t lastShiftCheck = 0; time_t now_ts = time(nullptr); // Renamed to now_ts
  if (now_ts < 1609459200) return; // Don't run if time is not synced

  if (now_ts - lastShiftCheck < 58 && lastShiftCheck != 0) { // Check approx once per minute (58 to avoid exact minute skip)
    return;
  }
  lastShiftCheck = now_ts;

  struct tm timeinfo; localtime_r(&now_ts, &timeinfo);
  int newlyDeterminedShiftIndex = -1; time_t newShiftStartTimeEpochThisInstance = 0;
  time_t latestPassedShiftStartEpoch = 0; int tempBestMatchIndex = -1;

  for (uint8_t i = 0; i < config.numShifts; ++i) {
    int shiftH, shiftM;
    if (sscanf(config.shifts[i].startTime, "%d:%d", &shiftH, &shiftM) == 2) {
      struct tm shift_tm_template = timeinfo; 
      shift_tm_template.tm_hour = shiftH; shift_tm_template.tm_min = shiftM; shift_tm_template.tm_sec = 0;
      time_t currentShiftStartToday = mktime(&shift_tm_template);
      time_t currentShiftStartYesterday = currentShiftStartToday - 86400; 
      if (now_ts >= currentShiftStartToday) { // Shift could have started today
        if (currentShiftStartToday >= latestPassedShiftStartEpoch) { 
          latestPassedShiftStartEpoch = currentShiftStartToday;
          tempBestMatchIndex = i;
        }
      } else if (now_ts >= currentShiftStartYesterday) { // Shift must have started yesterday
         if (currentShiftStartYesterday >= latestPassedShiftStartEpoch) {
          latestPassedShiftStartEpoch = currentShiftStartYesterday;
          tempBestMatchIndex = i;
        }
      }
    }
  }
  
  if (tempBestMatchIndex != -1) {
      newlyDeterminedShiftIndex = tempBestMatchIndex;
      newShiftStartTimeEpochThisInstance = latestPassedShiftStartEpoch;
  } else { // Fallback if logic above failed (e.g. bad shift times not covering 24h)
      Serial.println("processShiftLogic: Could not determine current shift based on start times. Using stored index if valid.");
      newlyDeterminedShiftIndex = g_currentShiftIndex; 
      newShiftStartTimeEpochThisInstance = g_currentShiftStartTimeEpoch;
      // If g_currentShiftIndex is also invalid, try to re-initialize (though it should have been done in setup)
      if (g_currentShiftIndex == -1 && config.numShifts > 0) {
          initializeShiftState(); // Try re-initializing
          newlyDeterminedShiftIndex = g_currentShiftIndex; // Use the newly initialized value
          newShiftStartTimeEpochThisInstance = g_currentShiftStartTimeEpoch;
          Serial.printf("processShiftLogic: Re-initialized shift state. New index: %d\n", newlyDeterminedShiftIndex);
      } else if (g_currentShiftIndex == -1){
          Serial.println("processShiftLogic: Fallback failed, no valid shift index.");
          return; // Cannot proceed
      }
  }

  if (g_currentShiftIndex == -1 && newlyDeterminedShiftIndex != -1) { 
    g_currentShiftIndex = newlyDeterminedShiftIndex;
    g_currentShiftStartTimeEpoch = newShiftStartTimeEpochThisInstance;
    Serial.printf("PROCESSLOGIC: Initial active shift set to #%d (%s). Effective Start: %lu\n", 
                  g_currentShiftIndex + 1, config.shifts[g_currentShiftIndex].startTime, (unsigned long)g_currentShiftStartTimeEpoch);
  } else if (newlyDeterminedShiftIndex != -1 && newShiftStartTimeEpochThisInstance > g_currentShiftStartTimeEpoch) {
    Serial.printf("PROCESSLOGIC: Shift change! Old Shift #%d (%s) ended.\n", g_currentShiftIndex + 1, config.shifts[g_currentShiftIndex].startTime);
    int endedShiftIdx = g_currentShiftIndex; 
    String endedShiftStart = String(config.shifts[endedShiftIdx].startTime);
    g_currentShiftIndex = newlyDeterminedShiftIndex; 
    g_currentShiftStartTimeEpoch = newShiftStartTimeEpochThisInstance;
    Serial.printf("PROCESSLOGIC: New Shift #%d (%s) starting. Effective Start: %lu\n", g_currentShiftIndex + 1, config.shifts[g_currentShiftIndex].startTime, (unsigned long)g_currentShiftStartTimeEpoch);
    archiveLogAndResetForShift(endedShiftIdx, endedShiftStart.c_str());
  } else if (newlyDeterminedShiftIndex != -1 && newlyDeterminedShiftIndex != g_currentShiftIndex && newShiftStartTimeEpochThisInstance == g_currentShiftStartTimeEpoch ) {
      Serial.printf("PROCESSLOGIC: Index changed (%d to %d) but start time (%lu) same. Updating index.\n", g_currentShiftIndex +1, newlyDeterminedShiftIndex+1, (unsigned long)newShiftStartTimeEpochThisInstance);
      g_currentShiftIndex = newlyDeterminedShiftIndex; 
  }
}


// =================================================================
// ---  setup() function 
// =================================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Asset Monitor Local V2 Starting ---");

  // --- Enhanced OLED Initialization ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
  } else {
    displayBootMessage("Booting...");
    delay(1000);
  }

  displayBootMessage("Initializing Storage...");
  if (!SPIFFS.begin(true)) { 
    Serial.println("SPIFFS.begin() failed! Halting."); 
    displayBootMessage("Storage Failed!");
    return; 
  }
  Serial.println("SPIFFS initialized.");

  ensureLogFileHasHeader();

  displayBootMessage("Loading Configuration...");
  if (!prefs.begin("assetmon", false)) {
      Serial.println("Global prefs.begin() failed during setup! WiFi saving might fail.");
  } else {
      Serial.println("Global Preferences initialized.");
  }

  loadConfig();
  Serial.println("Configuration loaded/initialized.");

  displayBootMessage("Initializing Shifts...");
  initializeShiftState();
  Serial.printf("DEBUG: enableShiftArchiving=%d, g_shiftFeatureInitialized=%d\n", config.enableShiftArchiving, g_shiftFeatureInitialized);

  // Enhanced WiFi setup with OLED status messages
  setupWiFiSmartV2();

  displayBootMessage("Syncing Time...");
  setupTime();
  Serial.println("WiFi and Time setup complete.");

  displayBootMessage("Initializing Assets...");
  Serial.printf("Initializing %u assets...\n", config.assetCount);
  time_t initialTime = time(nullptr);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i < MAX_ASSETS) {
      if (config.assets[i].pin > 0 && config.assets[i].pin < 40) {
          pinMode(config.assets[i].pin, INPUT_PULLUP);
          assetStates[i].lastState = digitalRead(config.assets[i].pin);
      } else {
          Serial.printf("Warning: Asset %s (idx %u) has invalid pin %u. Defaulting to STOPPED state and marking pin unusable.\n", config.assets[i].name, i, config.assets[i].pin);
          assetStates[i].lastState = true;
      }
      assetStates[i].lastChangeTime = initialTime;
      assetStates[i].sessionStart = initialTime;
      assetStates[i].runningTime = 0; assetStates[i].stoppedTime = 0;
      assetStates[i].runCount = 0; assetStates[i].stopCount = 0;
      assetStates[i].lastEventTime = initialTime;
      assetStates[i].lastRunDuration = 0; assetStates[i].lastStopDuration = 0;
      Serial.printf("Asset %u ('%s', pin %u) init. Pin State: %s (Input means: %s)\n",
                      i, config.assets[i].name, config.assets[i].pin,
                      assetStates[i].lastState ? "HIGH" : "LOW",
                      assetStates[i].lastState ? "STOPPED" : "RUNNING");
    }
  }

  if (config.monitoringMode == MONITORING_MODE_SERIAL) {
    g_systemLastStateChangeTime = initialTime;
    bool anyAssetInitiallyStopped = false;
    g_serialSystemTriggerAssetName[0] = '\0';
    for (uint8_t i = 0; i < config.assetCount; ++i) {
      if (i < MAX_ASSETS) {
        bool assetEffectivelyStopped = assetStates[i].lastState;
        if (config.assets[i].pin == 0 || config.assets[i].pin >= 40) {
            assetEffectivelyStopped = true;
        }
        if (assetEffectivelyStopped) {
            anyAssetInitiallyStopped = true;
            if (g_serialSystemTriggerAssetName[0] == '\0') {
                strncpy(g_serialSystemTriggerAssetName, config.assets[i].name, 31);
                g_serialSystemTriggerAssetName[31] = '\0';
            }
        }
      }
    }
    g_isSystemSerialDown = anyAssetInitiallyStopped;
    g_systemStateInitialized = true;
    Serial.printf("Serial Mode: Initial system state set. System Down: %s. Root Cause: '%s'\n",
                  g_isSystemSerialDown ? "Yes" : "No",
                  g_isSystemSerialDown ? g_serialSystemTriggerAssetName : "N/A");
    if (g_isSystemSerialDown) {
        logSystemEvent(false, initialTime, g_serialSystemTriggerAssetName);
    } else {
        logSystemEvent(true, initialTime, "System Initialized - All Assets Up");
    }
  }

  displayBootMessage("Starting Web Server...");
  
  // Register all routes - FULL FUNCTIONALITY in both STA and AP modes
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
  server.on("/dashboard", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
  server.on("/config", HTTP_GET, []() { server.send(200, "text/html", htmlConfig()); });
  server.on("/events", HTTP_GET, sendHtmlEventsPage);
  server.on("/asset", HTTP_GET, []() {
    if (server.hasArg("idx")) {
      uint8_t idx = server.arg("idx").toInt();
      if (idx < config.assetCount && idx < MAX_ASSETS) { server.send(200, "text/html", htmlAssetDetail(idx)); return; }
    }
    server.send(404, "text/plain", "Asset not found");
  });
  server.on("/shiftlogs_page", HTTP_GET, handleShiftLogsPage);
  server.on("/download_shiftlog", HTTP_GET, handleDownloadShiftLog);
  server.on("/analytics", HTTP_GET, []() { server.send(200, "text/html", htmlAnalytics()); });
  server.on("/analytics-compare", HTTP_GET, []() { server.send(200, "text/html", htmlAnalyticsCompare()); });
  
  // --- LOCAL ASSET ROUTES (NEW) - Serve CSS/JS locally for offline functionality ---
  server.on("/assets/bootstrap.min.css", HTTP_GET, handleBootstrapCSS);
  server.on("/assets/bootstrap.bundle.min.js", HTTP_GET, handleBootstrapJS);
  server.on("/assets/chart.min.js", HTTP_GET, handleChartsJS);
  server.on("/assets/bootstrap-icons.min.css", HTTP_GET, handleBootstrapIcons);
  
  server.on("/reconfigure_wifi", HTTP_POST, handleWiFiReconfigurePost);
  server.on("/save_config", HTTP_POST, handleConfigPost);
  server.on("/clear_log", HTTP_POST, handleClearLog);
  server.on("/export_log", HTTP_GET, handleExportLog);
  server.on("/api/summary", HTTP_GET, handleApiSummary);
  server.on("/api/events", HTTP_GET, handleApiEvents);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/note", HTTP_POST, handleApiNote);
  server.on("/delete_logs", HTTP_POST, handleDeleteLogs);
  
  // WiFi configuration routes for AP mode
  server.on("/wifi_config", HTTP_GET, []() { server.send(200, "text/html", wifiConfigHTML()); });
  server.on("/wifi_save_config", HTTP_POST, handleWifiConfigPost);
  
  server.onNotFound(handleNotFound);
  server.begin();
  
  // Show final status on OLED
  updateOledDisplay();
  
  Serial.println("Web server started. Device is ready.");
  Serial.printf("Device is accessible via %s mode at IP: %s\n", 
                currentWiFiState == WIFI_STA_CONNECTED ? "STA" : "AP",
                currentWiFiState == WIFI_STA_CONNECTED ? WiFi.localIP().toString().c_str() : WiFi.softAPIP().toString().c_str());
}

// =========================================================================
// --- Block 1:  loop() function 
// =========================================================================

void loop() {
  server.handleClient();

  if (config.enableShiftArchiving && g_shiftFeatureInitialized) {
    processShiftLogic();
  }


  time_t now_ts = time(nullptr);

  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) continue;
    bool current_pin_state = true;
    if (config.assets[i].pin > 0 && config.assets[i].pin < 40) {
        current_pin_state = digitalRead(config.assets[i].pin);
    } else {
        if (assetStates[i].lastState == false) { 
            assetStates[i].lastState = true;
            assetStates[i].lastChangeTime = now_ts;
        }
        if (config.monitoringMode == MONITORING_MODE_PARALLEL) continue;
    }
    if (current_pin_state != assetStates[i].lastState) {
      unsigned long elapsed = now_ts - assetStates[i].lastChangeTime;
      unsigned long runDuration = 0;
      unsigned long stopDuration = 0;
      char eventNoteBuffer[128] = "";

      if (current_pin_state == true) {
        assetStates[i].runningTime += elapsed;
        assetStates[i].stopCount++;
        runDuration = elapsed;
        assetStates[i].lastRunDuration = runDuration;
        assetStates[i].lastStopDuration = 0;
        if (config.monitoringMode == MONITORING_MODE_SERIAL && g_isSystemSerialDown &&
            strlen(g_serialSystemTriggerAssetName) > 0 &&
            strcmp(config.assets[i].name, g_serialSystemTriggerAssetName) != 0) {
          snprintf(eventNoteBuffer, sizeof(eventNoteBuffer), "Root Cause (System: %s)", g_serialSystemTriggerAssetName);
        }
        logEvent(i, false, now_ts, eventNoteBuffer[0] ? eventNoteBuffer : nullptr, runDuration, 0);
      } else {
        assetStates[i].stoppedTime += elapsed;
        assetStates[i].runCount++;
        stopDuration = elapsed;
        assetStates[i].lastStopDuration = stopDuration;
        assetStates[i].lastRunDuration = 0;
        logEvent(i, true, now_ts, nullptr, 0, stopDuration);
      }
      assetStates[i].lastState = current_pin_state;
      assetStates[i].lastChangeTime = now_ts;
    }
  }

  if (config.monitoringMode == MONITORING_MODE_SERIAL) {
    if (!g_systemStateInitialized) {
        g_systemLastStateChangeTime = now_ts;
        bool anyAssetInitiallyStoppedLoop = false;
        g_serialSystemTriggerAssetName[0] = '\0';
        for (uint8_t k = 0; k < config.assetCount; ++k) {
            if (k < MAX_ASSETS) {
                bool assetEffectivelyStoppedLoop = assetStates[k].lastState;
                if (config.assets[k].pin == 0 || config.assets[k].pin >= 40) {
                    assetEffectivelyStoppedLoop = true;
                }
                if (assetEffectivelyStoppedLoop) {
                    anyAssetInitiallyStoppedLoop = true;
                    if (g_serialSystemTriggerAssetName[0] == '\0') {
                        strncpy(g_serialSystemTriggerAssetName, config.assets[k].name, 31);
                        g_serialSystemTriggerAssetName[31] = '\0';
                    }
                }
            }
        }
        g_isSystemSerialDown = anyAssetInitiallyStoppedLoop;
        g_systemStateInitialized = true;
        Serial.println("Serial Mode: System state re-initialized in loop (fallback).");
        logSystemEvent(!g_isSystemSerialDown, now_ts, g_isSystemSerialDown ? g_serialSystemTriggerAssetName : "System Initialized - All Up (Loop)");
    }

    bool isAnyAssetCurrentlyStopped = false;
    char currentCycleTriggerAssetName[32] = ""; 
    currentCycleTriggerAssetName[0] = '\0';

    for (uint8_t j = 0; j < config.assetCount; ++j) {
      if (j < MAX_ASSETS) {
        bool assetEffectivelyStopped = assetStates[j].lastState;
        if (config.assets[j].pin == 0 || config.assets[j].pin >= 40) { 
            assetEffectivelyStopped = true;
        }
        if (assetEffectivelyStopped) {
          isAnyAssetCurrentlyStopped = true;
          if (currentCycleTriggerAssetName[0] == '\0') {
            strncpy(currentCycleTriggerAssetName, config.assets[j].name, 31);
            currentCycleTriggerAssetName[31] = '\0';
          }
        }
      }
    }

    if (isAnyAssetCurrentlyStopped && !g_isSystemSerialDown) {
      unsigned long systemUpDuration = now_ts - g_systemLastStateChangeTime;
      g_systemTotalRunningTimeSecs += systemUpDuration;
      g_isSystemSerialDown = true;
      g_systemLastStateChangeTime = now_ts;
      g_systemStopCount++;
      strncpy(g_serialSystemTriggerAssetName, currentCycleTriggerAssetName, 31); 
      g_serialSystemTriggerAssetName[31] = '\0';
      if (strlen(g_serialSystemTriggerAssetName) == 0 && isAnyAssetCurrentlyStopped) strcpy(g_serialSystemTriggerAssetName, "Unknown");
      logSystemEvent(false, now_ts, g_serialSystemTriggerAssetName);
      Serial.printf("Serial Mode: System -> DOWN. Root Cause: %s. Up Duration: %lu s\n", g_serialSystemTriggerAssetName, systemUpDuration);
    }
    else if (!isAnyAssetCurrentlyStopped && g_isSystemSerialDown) {
      unsigned long systemDownDuration = now_ts - g_systemLastStateChangeTime;
      g_systemTotalStoppedTimeSecs += systemDownDuration;
      g_isSystemSerialDown = false;
      g_systemLastStateChangeTime = now_ts;
      g_serialSystemTriggerAssetName[0] = '\0';
      logSystemEvent(true, now_ts, "All Assets Recovered");
      Serial.printf("Serial Mode: System -> UP. Down Duration: %lu s\n", systemDownDuration);
    }
  }

  // --- Enhanced OLED and WiFi monitoring (ENHANCED) ---
  if (millis() - lastOledUpdate > 5000) {
    // Check WiFi status and update state if needed
    if (currentWiFiState == WIFI_STA_CONNECTED && WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection lost in STA mode");
      currentWiFiState = WIFI_IDLE;
      // Could attempt reconnection here, but for simplicity, just update display
    }
    
    updateOledDisplay();
    lastOledUpdate = millis();
  }

  delay(200);
}


// logEvent: Accepts customNote for "Consequence Stop"
void logEvent(uint8_t assetIdx, bool machineIsRunning, time_t now_ts, const char* customNote, unsigned long runDuration, unsigned long stopDuration) {
  if (assetIdx >= MAX_ASSETS) return; // Renamed 'now' to 'now_ts'

  AssetState& as = assetStates[assetIdx];
  // Cumulative stats are updated *before* this event for calculation of metrics *at the time of this event*
  unsigned long cumulative_runningTime = as.runningTime;
  unsigned long cumulative_stoppedTime = as.stoppedTime;

  // If this event is a STOP, the runDuration that just ENDED is added to cumulative_runningTime for MTBF calc
  // If this event is a START, the stopDuration that just ENDED is added to cumulative_stoppedTime for MTTR calc
  // The assetStates[i].runningTime/stoppedTime in loop() are updated *before* calling logEvent.

  float avail = (cumulative_runningTime + cumulative_stoppedTime) > 0
                ? (100.0 * cumulative_runningTime / (cumulative_runningTime + cumulative_stoppedTime))
                : (machineIsRunning ? 100.0 : 0.0);

  float total_runtime_min = cumulative_runningTime / 60.0;
  float total_downtime_min = cumulative_stoppedTime / 60.0;

  // as.stopCount is the count of *completed* stop cycles.
  // If this is a STOP event, stopCount has just been incremented in loop()
  // If this is a START event, stopCount reflects stops before this run.
  uint32_t stops_for_mtbf_calc = machineIsRunning ? as.stopCount : as.stopCount; // if running, use current stopCount for MTBF of runs ending in those stops
                                                                                   // if stopping, stopCount was just incremented, use that.
  uint32_t stops_for_mttr_calc = as.stopCount; // MTTR is based on completed stop cycles.

  float mtbf_val = (stops_for_mtbf_calc > 0) ? (float)cumulative_runningTime / stops_for_mtbf_calc / 60.0 : total_runtime_min;
  float mttr_val = (stops_for_mttr_calc > 0) ? (float)cumulative_stoppedTime / stops_for_mttr_calc / 60.0 : 0;

  struct tm * ti = localtime(&now_ts); // used now_ts
  char datebuf[11], timebuf[9];
  strftime(datebuf, sizeof(datebuf), "%d/%m/%Y", ti);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti);

  File f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
  if (!f) { Serial.println("Failed to open log file for writing!"); return; }
  f.printf("%s,%s,%s,%s,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%s,%s,%s\n",
    datebuf, timebuf, config.assets[assetIdx].name,
    machineIsRunning ? "START" : "STOP",
    machineIsRunning ? 1 : 0,
    avail, total_runtime_min, total_downtime_min, mtbf_val, mttr_val,
    as.stopCount, // Log the current stop count of the asset
    (runDuration > 0 ? formatMMSS(runDuration).c_str() : ""),
    (stopDuration > 0 ? formatMMSS(stopDuration).c_str() : ""),
    customNote ? customNote : "" // Use customNote if provided
  );
  f.close();
  as.lastEventTime = now_ts; // used now_ts
  Serial.printf("Event logged for %s: %s. Note: '%s'. RunD: %s, StopD: %s. Stops: %u\n",
    config.assets[assetIdx].name, machineIsRunning ? "START" : "STOP",
    customNote ? customNote : "",
    formatMMSS(runDuration).c_str(), formatMMSS(stopDuration).c_str(), as.stopCount
  );
}

String formatMMSS(unsigned long seconds) {
  if (seconds == 0) return ""; // Return empty for 0, or "00:00" if you prefer
  unsigned int min_val = seconds / 60;
  unsigned int sec_val = seconds % 60;
  char buf[8]; // "MM:SS" + null
  sprintf(buf, "%02u:%02u", min_val, sec_val);
  return String(buf);
}

String eventToCSV(const Event& e) {
  struct tm * ti = localtime(&e.timestamp);
  char datebuf[16], timebuf[16];
  strftime(datebuf, sizeof(datebuf), "%d/%m/%Y", ti);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti);

  String csv_str = String(datebuf) + "," + String(timebuf) + "," + // Renamed csv to csv_str to avoid conflict
               String(e.assetName) + "," + String(e.eventType) + "," +
               String(e.state) + "," + String(e.availability, 2) + "," +
               String(e.runtime, 2) + "," + String(e.downtime, 2) + "," +
               String(e.mtbf, 2) + "," + String(e.mttr, 2) + "," +
               String(e.stops) + "," +
               String(e.runDuration) + "," + String(e.stopDuration) + "," +
               String(e.note);
  return csv_str;
}

String urlEncode(const String& str) {
  String encodedString = ""; char c; char code0; char code1;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') encodedString += '+';
    else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') encodedString += c;
    else {
      code1 = (c & 0xf) + '0'; if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf; code0 = c + '0'; if (c > 9) code0 = c - 10 + 'A';
      encodedString += '%'; encodedString += code0; encodedString += code1;
    }
  }
  return encodedString;
}

String urlDecode(const String& str) {
  String decodedString = ""; char a, b;
  for (unsigned int i = 0; i < str.length(); i++) {
    if (str.charAt(i) == '+') decodedString += ' ';
    else if (str.charAt(i) == '%') {
      if (i + 2 < str.length()) {
        a = str.charAt(++i); b = str.charAt(++i);
        if (isxdigit(a) && isxdigit(b)) {
          if (a >= 'a') a -= 'a' - 'A'; if (a >= 'A') a -= ('A' - 10); else a -= '0';
          if (b >= 'a') b -= 'a' - 'A'; if (b >= 'A') b -= ('A' - 10); else b -= '0';
          decodedString += (char)(16 * a + b);
        } else { // Malformed % sequence
          decodedString += '%'; decodedString += a; decodedString += b;
        }
      } else { // Truncated % sequence
        decodedString += '%';
        if (i + 1 < str.length()) decodedString += str.charAt(++i);
      }
    } else decodedString += str.charAt(i);
  }
  return decodedString;
}


// HTML Functions

String htmlAssetDetail(uint8_t idx) {
  if (idx >= config.assetCount || idx >= MAX_ASSETS) {
    return "Error: Invalid Asset Index";
  }

  String assetNameStr = String(config.assets[idx].name);
  String encodedAssetName = urlEncode(assetNameStr); // Ensure name is URL-safe for the link

  String html = "<!DOCTYPE html><html lang='en'><head><title>Asset Detail: " + assetNameStr + "</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='/assets/bootstrap.min.css' rel='stylesheet'>";
  html += "<style>body{background-color:#f8f9fa;}</style>";
  html += "</head><body>";

  // --- Standard Bootstrap Navbar ---
  html += "<nav class='navbar navbar-expand-lg navbar-dark bg-dark shadow-sm'>";
  html += "  <div class='container-fluid'>";
  html += "    <a class='navbar-brand' href='/'>Asset Availability</a>";
  html += "    <div class='ms-auto'><a href='/' class='btn btn-secondary'>Back to Dashboard</a></div>";
  html += "  </div></nav>";

  html += "<div class='container mt-4'>";
  html += "  <div class='card shadow-sm'>";
  html += "    <div class='card-header'><h3>Asset Detail</h3></div>";
  html += "    <div class='card-body'>";
  html += "      <h4 class='card-title'>" + assetNameStr + "</h4>";
  html += "      <p class='card-text'><strong>Output:</strong> " + String(config.assets[idx].pin) + "</p>";
  html += "      <hr>";
  html += "      <a href='/analytics?asset=" + encodedAssetName + "' class='btn btn-primary'>View Full Analytics</a>";
  html += "      <a href='/' class='btn btn-outline-secondary'>Back to Dashboard</a>";
  html += "    </div>";
  html += "  </div>";
  html += "</div>";

  html += "<script src='/assets/bootstrap.bundle.min.js'></script>";
  html += "</body></html>";
  return html;
}

// =================================================================
// --- Block 5:  htmlConfig() function 
// =================================================================

String htmlConfig() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Setup</title><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='/assets/bootstrap.min.css' rel='stylesheet'>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001; font-size:1.6em; font-weight:700;}";
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;align-items:center;}";
  html += ".nav button, .nav a {text-decoration:none;background:#fff;color:#1976d2;border:1px solid #1976d2;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;margin:0.2em 0.4em;}";
  html += ".nav button:hover, .nav a:hover {background:#e3f0fc;}";
  html += ".nav .right{margin-left:auto;}";
  html += ".main{max-width:700px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += "label.form-label{font-weight:500;margin-top:1em;display:block;}";
  html += "input.form-control[type=text],input.form-control[type=number],select.form-select{width:100%;padding:0.6em;margin-top:0.2em;margin-bottom:1em;border:1px solid #ccc;border-radius:5px;font-size:1em;}";
  html += "button.btn-primary, input.btn-primary[type=submit]{width:100%;margin-top:1em;padding:0.8em 1.5em;font-size:1.15em;font-weight:700;}";
  html += ".notice{background:#e6fbe7;color:#256029;font-weight:bold;padding:0.6em 1em;border-radius:7px;margin-bottom:1em;text-align:center;}";
  html += ".accordion-button:not(.collapsed){color: #0c63e4; background-color: #e7f1ff;}";
  html += ".accordion-button:focus {box-shadow: 0 0 0 .25rem rgba(13,110,253,.25);}";
  html += "</style>";
  html += "</head><body>";
  html += "<nav class='navbar navbar-expand-lg navbar-dark bg-dark shadow-sm'>";
  html += "  <div class='container-fluid'>";
  html += "    <a class='navbar-brand' href='/'>Asset Availability</a>";
  html += "    <button class='navbar-toggler' type='button' data-bs-toggle='collapse' data-bs-target='#navbarNav'><span class='navbar-toggler-icon'></span></button>";
  html += "    <div class='collapse navbar-collapse' id='navbarNav'>";
  html += "      <ul class='navbar-nav ms-auto'>";
  html += "        <li class='nav-item'><a class='nav-link' href='/'>Dashboard</a></li>";
  html += "        <li class='nav-item'><a class='nav-link' href='/events'>Event Log</a></li>";
  html += "        <li class='nav-item'><a class='nav-link' href='/analytics-compare'>Compare Assets</a></li>";
  html += "        <li class='nav-item'><a class='nav-link active' href='/config'>Setup</a></li>";
  html += "      </ul>";
  html += "    </div>";
  html += "  </div>";
  html += "</nav>";
  html += "<div class='container mt-4'>";
  html += "  <div class='row justify-content-center'>";
  html += "    <div class='col-lg-9 col-xl-8'>";

  html += "      <div class='card shadow-sm'>";
  html += "        <div class='card-header bg-light'><h3 class='mb-0'>Configuration</h3></div>";
  html += "        <div class='card-body'>";
  html += "          <div id='saveNotice' class='alert alert-success' style='display:none;'>Settings saved! Device is rebooting...</div>";
  html += "          <form method='POST' action='/save_config' id='setupform' onsubmit='setTimeout(function() { const notice = document.getElementById(\"saveNotice\"); if(notice) notice.style.display=\"block\"; }, 500);'>";
  html += "            <div class='accordion' id='configAccordion'>";
  
  html += "              <div class='accordion-item'>";
  html += "                <h2 class='accordion-header' id='headingAssets'><button class='accordion-button collapsed' type='button' data-bs-toggle='collapse' data-bs-target='#collapseAssets' aria-expanded='false' aria-controls='collapseAssets'>Asset Setup</button></h2>";
  html += "                <div id='collapseAssets' class='accordion-collapse collapse' aria-labelledby='headingAssets' data-bs-parent='#configAccordion'><div class='accordion-body'>";
  html += "                  <div class='mb-3'><label for='assetCountField' class='form-label'>Asset Count (1-" + String(MAX_ASSETS) + ")</label><input type='number' class='form-control' id='assetCountField' name='assetCount' min='1' max='" + String(MAX_ASSETS) + "' value='" + String(config.assetCount) + "' required></div>";
  html += "                  <p class='form-text'>After changing asset count, 'Save All Settings & Reboot'. The page will then update to show fields for each asset.</p>";
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) break;
    html += "                  <fieldset class='border p-3 mb-3 rounded bg-light-subtle'><legend class='w-auto px-2 fs-6 fw-semibold'>Asset #" + String(i + 1) + "</legend>";
    html += "                    <div class='mb-3'><label for='name" + String(i) + "' class='form-label'>Name</label><input type='text' class='form-control' id='name" + String(i) + "' name='name" + String(i) + "' value='" + String(config.assets[i].name) + "' maxlength='31' required></div>";
    html += "                    <div><label for='pin" + String(i) + "' class='form-label'>Output</label><input type='number' class='form-control' id='pin" + String(i) + "' name='pin" + String(i) + "' value='" + String(config.assets[i].pin) + "' min='0' max='39' required></div>";
    html += "                  </fieldset>";
  }
  html += "                </div></div>";
  html += "              </div>";

  html += "              <div class='accordion-item'>";
  html += "                <h2 class='accordion-header' id='headingOps'><button class='accordion-button collapsed' type='button' data-bs-toggle='collapse' data-bs-target='#collapseOps' aria-expanded='false' aria-controls='collapseOps'>Operational Settings</button></h2>";
  html += "                <div id='collapseOps' class='accordion-collapse collapse' aria-labelledby='headingOps' data-bs-parent='#configAccordion'><div class='accordion-body'>";
  html += "                  <div class='mb-3'><label for='maxEvents' class='form-label'>Max Events in Log</label><input type='number' id='maxEvents' class='form-control' name='maxEvents' min='100' max='5000' value='" + String(config.maxEvents) + "' required></div>";
  html += "                  <div class='mb-3'><label for='tzOffset' class='form-label'>Timezone Offset (hours from UTC)</label><input type='number' id='tzOffset' class='form-control' name='tzOffset' min='-12' max='14' step='0.5' value='" + String(config.tzOffset / 3600.0, 1) + "' required></div>";
  html += "                  <div class='mb-3'><label for='longStopThreshold' class='form-label'>Highlight Stops Longer Than (minutes)</label><input type='number' id='longStopThreshold' class='form-control' name='longStopThreshold' min='1' max='1440' value='" + String(config.longStopThresholdSec/60) + "' required></div>";
  html += "                  <div><label for='monitoringMode' class='form-label'>Monitoring Mode</label><select id='monitoringMode' name='monitoringMode' class='form-select'>";
  html += "                    <option value='" + String(MONITORING_MODE_PARALLEL) + "'" + (config.monitoringMode == MONITORING_MODE_PARALLEL ? " selected" : "") + ">Parallel (Assets Independent)</option>";
  html += "                    <option value='" + String(MONITORING_MODE_SERIAL) + "'" + (config.monitoringMode == MONITORING_MODE_SERIAL ? " selected" : "") + ">Serial (System Stops if Any Asset Stops)</option>";
  html += "                  </select></div>";
  html += "                </div></div>";
  html += "              </div>";
  
  html += "              <div class='accordion-item'>";
  html += "                <h2 class='accordion-header' id='headingShifts'><button class='accordion-button collapsed' type='button' data-bs-toggle='collapse' data-bs-target='#collapseShifts' aria-expanded='false' aria-controls='collapseShifts'>Production Data & Shift Setup</button></h2>";
  html += "                <div id='collapseShifts' class='accordion-collapse collapse' aria-labelledby='headingShifts' data-bs-parent='#configAccordion'><div class='accordion-body'>";
  html += "                  <div class='form-check mb-3'><input class='form-check-input' type='checkbox' name='enableShiftArchiving' id='enableShiftArchiving' value='1'" + String(config.enableShiftArchiving ? " checked" : "") + " onchange='toggleShiftSettingsDisplay(this.checked)'><label class='form-check-label' for='enableShiftArchiving'>Enable Automatic Log Archival & Clearing per Shift</label></div>";
  html += "                  <div id='shiftSettingsBlockGlobal'" + String(config.enableShiftArchiving ? "" : " style='display:none;'") +">";
  html += "                    <div class='mb-3'><label for='numShiftsInput' class='form-label'>Number of Shifts (1-" + String(MAX_CONFIGURABLE_SHIFTS) + ")</label><input type='number' class='form-control' name='numShifts' id='numShiftsInput' min='1' max='" + String(MAX_CONFIGURABLE_SHIFTS) + "' value='" + String(config.numShifts > 0 ? config.numShifts : 1) + "' oninput='renderShiftTimeInputs(this.value)'></div>";
  html += "                    <div id='shiftTimeInputsContainer'>";
  if (config.enableShiftArchiving) {
    for(uint8_t i = 0; i < config.numShifts; ++i) {
      if (i < MAX_CONFIGURABLE_SHIFTS) {
        html += "                    <div class='mb-3'><label for='shiftStartTime" + String(i) + "' class='form-label'>Shift " + String(i + 1) + " Start Time (HH:MM)</label><input type='time' class='form-control' id='shiftStartTime" + String(i) + "' name='shiftStartTime" + String(i) + "' value='" + String(config.shifts[i].startTime) + "' required></div>";
      }
    }
  }
  html += "                    </div>";
  html += "                    <p class='form-text'>Define the start time for each shift (e.g., 06:00, 14:00, 22:00). Logs are archived at the detected start of a new shift.</p>";
  html += "                  </div>";
  // html += "                  <div class='d-grid gap-2 mt-4'><a href='/shiftlogs_page' class='btn btn-info btn-lg'>View Archived Shift Logs</a></div>";
  html += "                </div></div>";
  html += "              </div>";

  html += "              <div class='accordion-item'>";
  html += "                <h2 class='accordion-header' id='headingReasons'><button class='accordion-button collapsed' type='button' data-bs-toggle='collapse' data-bs-target='#collapseReasons' aria-expanded='false' aria-controls='collapseReasons'>Downtime Quick Reasons</button></h2>";
  html += "                <div id='collapseReasons' class='accordion-collapse collapse' aria-labelledby='headingReasons' data-bs-parent='#configAccordion'><div class='accordion-body'>";
  for (int i = 0; i < 5; ++i) {
    html += "                  <div class='mb-3'><label for='reason" + String(i) + "' class='form-label'>Reason " + String(i+1) + "</label><input type='text' class='form-control' id='reason" + String(i) + "' name='reason" + String(i) + "' value='" + String(config.downtimeReasons[i]) + "' maxlength='31'></div>";
  }
  html += "                </div></div>";
  html += "              </div>";
  html += "            </div>";
  html += "            <div class='d-grid gap-2 mt-4'><button type='submit' class='btn btn-primary btn-lg'>Save All Settings & Reboot</button></div>";
  html += "          </form>";
  html += "        </div>";
  html += "      </div>";
  html += "      <div class='card shadow-sm mt-4'><div class='card-header bg-light'><h4 class='mb-0'>System Actions</h4></div><div class='card-body'>";
  html += "        <p>Current WiFi: <strong>" + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() + " (IP: " + WiFi.localIP().toString() + ")" : "Not Connected / AP Mode") + "</strong></p>";
  html += "        <form method='POST' action='/reconfigure_wifi' onsubmit='return confirm(\"Enter WiFi setup mode? Device will restart as an Access Point.\");' class='d-grid gap-2 mb-3'><button type='submit' class='btn btn-warning'>Reconfigure WiFi</button></form>";
  html += "        <form method='POST' action='/clear_log' onsubmit='return confirm(\"Are you sure you want to CLEAR THE CURRENT EVENT LOG? This cannot be undone!\");' class='d-grid gap-2'><button type='submit' class='btn btn-danger'>Clear Current Event Log</button></form>";
  html += "      </div></div>";
  html += "    </div>";
  html += "  </div>";
  html += "</div>";
  html += "<script>";
  html += R"rawliteral(
    function toggleShiftSettingsDisplay(enabled) {
      document.getElementById('shiftSettingsBlockGlobal').style.display = enabled ? 'block' : 'none';
    }
    function renderShiftTimeInputs(numShiftsStr) {
      const num = parseInt(numShiftsStr, 10);
      const container = document.getElementById('shiftTimeInputsContainer');
      container.innerHTML = '';
      const maxShifts = )rawliteral";
  html += String(MAX_CONFIGURABLE_SHIFTS);
  html += R"rawliteral(;
      if (isNaN(num) || num < 1 || num > maxShifts) { return; }
      for (let i = 0; i < num; i++) {
        const shiftDiv = document.createElement('div');
        shiftDiv.classList.add('mb-3');
        shiftDiv.innerHTML = `<label class="form-label">Shift ${i + 1} Start Time (HH:MM)</label>
          <input type="time" class="form-control" name="shiftStartTime${i}" value="00:00" required>`;
        container.appendChild(shiftDiv);
      }
    }
    document.addEventListener('DOMContentLoaded', function() {
      const enableCheckbox = document.getElementById('enableShiftArchiving');
      if (enableCheckbox) {
        toggleShiftSettingsDisplay(enableCheckbox.checked);
      }
              
    });
  )rawliteral";
  html += "</script>";
  html += "<script src='/assets/bootstrap.bundle.min.js'></script>";
  html += "</body></html>";
  return html;
}

String htmlDashboard() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='/assets/bootstrap.min.css' rel='stylesheet'>";
  html += "<script src='/assets/chart.min.js'></script>";
  html += "<style>body{background-color:#f8f9fa;}</style>";
  html += "</head><body>";

  // --- Standard Bootstrap Navbar ---
  html += "<nav class='navbar navbar-expand-lg navbar-dark bg-dark shadow-sm'>";
  html += "  <div class='container-fluid'>";
  html += "    <a class='navbar-brand' href='/'>Asset Availability Dashboard</a>";
  html += "    <button class='navbar-toggler' type='button' data-bs-toggle='collapse' data-bs-target='#navbarNav'><span class='navbar-toggler-icon'></span></button>";
  html += "    <div class='collapse navbar-collapse' id='navbarNav'>";
  html += "      <ul class='navbar-nav ms-auto'>";
  html += "        <li class='nav-item'><a class='nav-link active' href='/'>Dashboard</a></li>";
  html += "        <li class='nav-item'><a class='nav-link' href='/events'>Event Log</a></li>";
  html += "        <li class='nav-item'><a class='nav-link' href='/analytics-compare'>Compare Assets</a></li>";
  html += "        <li class='nav-item'><a class='nav-link' href='/config'>Setup</a></li>";
  html += "      </ul>";
  html += "    </div>";
  html += "  </div>";
  html += "</nav>";

  html += "<div class='container mt-4'>";
  // --- Placeholder for System Status Card (Original Style) ---
  html += "<div id='systemWideStatus'></div>";

  html += "<div class='card shadow-sm mb-4'>";
  html += "  <div class='card-header'><h4>Asset Overview</h4></div>";
  html += "  <div class='card-body'>";
  // --- Statrow for individual asset quick stats (Original Style) ---
  html += "    <div class='row g-2 mb-3' id='statrow'></div>";
  html += "    <div id='chart-container' style='min-height: 300px;'><canvas id='barChart'></canvas></div>";
  html += "  </div>";
  html += "</div>";

  html += "<div class='card shadow-sm'>";
  html += "  <div class='card-header'><h4>Live Status Details</h4></div>";
  html += "  <div class='card-body table-responsive'>";
  html += "    <table id='summaryTable' class='table table-striped table-hover table-sm align-middle'><thead><tr>";
  html += "      <th>Name</th><th>State</th><th>Avail (%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Actions</th>";
  html += "    </tr></thead><tbody></tbody></table>";
  html += "  </div>";
  html += "</div>";
  html += "</div>";

  html += "<script>";
  html += R"rawliteral(
function formatHHMMSS(valInMinutes) {
  if (isNaN(valInMinutes) || valInMinutes < 0.01) return "00:00:00";
  let totalSeconds = Math.round(valInMinutes * 60);
  let h = Math.floor(totalSeconds / 3600);
  let m = Math.floor((totalSeconds % 3600) / 60);
  let s = totalSeconds % 60;
  return (h < 10 ? "0" : "") + h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
}
let chartObj=null;
function updateDashboard() {
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    const systemStatusContainer = document.getElementById('systemWideStatus');
    let systemCardHtml = "";
    if (data.monitoringMode === 1 && data.systemStats) { // MONITORING_MODE_SERIAL is 1
        const isDown = data.systemStats.isDown;
        const systemStateClass = isDown ? "bg-danger text-white" : "bg-success text-white";
        const systemStatusText = isDown ? "SYSTEM DOWN" : "SYSTEM RUNNING";
        systemCardHtml = `
            <div class='card ${systemStateClass} shadow-sm mb-4'>
                <div class='card-body'>
                    <div class='d-flex justify-content-between align-items-center'>
                        <div>
                            <h5 class='card-title mb-1'>${systemStatusText}</h5>
                            ${isDown && data.systemStats.triggerAsset ? `<small>Triggered by: ${data.systemStats.triggerAsset}</small><br>` : ''}
                            <small>Availability: ${data.systemStats.availability.toFixed(2)}% | MTBF: ${formatHHMMSS(data.systemStats.mtbf_min)} | MTTR: ${formatHHMMSS(data.systemStats.mttr_min)}</small>
                        </div>
                        <div class='fs-1 fw-bold'>${data.systemStats.stopCount} <small class='fs-6 fw-normal'>Stops</small></div>
                    </div>
                </div>
            </div>`;
    }
    if (systemStatusContainer) { systemStatusContainer.innerHTML = systemCardHtml; }

    let tbody = document.querySelector('#summaryTable tbody');
    if (!tbody) return;
    tbody.innerHTML = ''; // Clear previous rows

    let statrow = document.getElementById('statrow');
    if(statrow) statrow.innerHTML = ''; // Clear previous stat cards

    let assets = data.assets;
    let n = assets.length;
    for(let i=0;i<n;++i){
      let asset = assets[i];
      let stateBadge = asset.state == 1 ? "<span class='badge bg-success'>RUNNING</span>" : "<span class='badge bg-danger'>STOPPED</span>";
      let assetNameEncoded = encodeURIComponent(asset.name);
      let newRow = tbody.insertRow();
      newRow.innerHTML = `
        <td><strong>${asset.name}</strong></td>
        <td>${stateBadge}</td>
        <td>${asset.availability.toFixed(2)}</td>
        <td>${formatHHMMSS(asset.total_runtime)}</td>
        <td>${formatHHMMSS(asset.total_downtime)}</td>
        <td>${formatHHMMSS(asset.mtbf)}</td>
        <td>${formatHHMMSS(asset.mttr)}</td>
        <td>${asset.stop_count}</td>
        <td><a href='/analytics?asset=${assetNameEncoded}' class='btn btn-sm btn-outline-primary py-0'>Analytics</a></td>
      `;

      // --- Reverted Statrow Cards ---
      if(statrow) {
        let statCardClass = asset.state == 1 ? "border-success bg-success-subtle" : "border-danger bg-danger-subtle";
        let statHtml = `
          <div class="col">
            <div class="card ${statCardClass} text-center h-100">
              <div class="card-header bg-transparent border-0 fw-bold">${asset.name}</div>
              <div class="card-body py-2">
                <h5 class="card-title mb-0">${asset.availability.toFixed(1)}%</h5>
                <small class="text-muted">Availability</small>
              </div>
              <div class="card-footer bg-transparent border-0 pt-0">
                 <small>${stateBadge} | Stops: ${asset.stop_count}</small>
              </div>
            </div>
          </div>`;
        statrow.innerHTML += statHtml;
      }
    }
    
    let availData=[], names=[], runtimeData=[], downtimeData=[];
    for (let asset of assets) {
      availData.push(asset.availability);
      runtimeData.push(asset.total_runtime);
      downtimeData.push(asset.total_downtime);
      names.push(asset.name);
    }
    
    // --- Reverted Bar Chart Datasets (Original Style) ---
    if (!window.chartObj) {
      const ctx = document.getElementById('barChart').getContext('2d');
      window.chartObj = new Chart(ctx, {
        type: 'bar',
        data: {
          labels: names,
          datasets: [
            { label: 'Availability (%)', data: availData, backgroundColor: 'rgba(66, 165, 245, 0.7)', yAxisID: 'y' }, // #42a5f5
            { label: 'Runtime (min)', data: runtimeData, backgroundColor: 'rgba(102, 187, 106, 0.7)', yAxisID: 'y1' }, // #66bb6a
            { label: 'Downtime (min)', data: downtimeData, backgroundColor: 'rgba(239, 83, 80, 0.7)', yAxisID: 'y1' } // #ef5350
          ]
        },
        options: {
          responsive:true, maintainAspectRatio:false, indexAxis: 'x',
          scales: {
            y: { beginAtZero:true, max:100, title:{display:true,text:'Availability (%)'} },
            y1: { beginAtZero:true, position: 'right', grid: { drawOnChartArea: false }, title: { display:true, text:'Time (min)' }}
          },
          plugins: { legend: { display: true } }
        }
      });
    } else {
      window.chartObj.data.labels = names;
      window.chartObj.data.datasets[0].data = availData;
      window.chartObj.data.datasets[1].data = runtimeData;
      window.chartObj.data.datasets[2].data = downtimeData;
      window.chartObj.update();
    }
  }).catch(e => console.error("Dashboard update error:", e));
}
// Find this section at the end of your htmlDashboard() function
// ... (your JavaScript code inside the raw literal) ...
if (document.readyState === 'loading') { document.addEventListener('DOMContentLoaded', updateDashboard); }
else { updateDashboard(); }
setInterval(updateDashboard, 5000);
)rawliteral";   // <--- THIS IS THE CORRECTED LINE
  html += "</script>";
  html += "<script src='/assets/bootstrap.bundle.min.js'></script>";
  html += "</body></html>";
  return html;
}

String htmlAnalytics() {
  String assetName = server.hasArg("asset") ? urlDecode(server.arg("asset")) : "";
  String html = "<!DOCTYPE html><html lang='en'><head><title>Asset Analytics: ";
  html += assetName + "</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='/assets/bootstrap.min.css' rel='stylesheet'>";
  html += "<script src='/assets/chart.min.js'></script>";
  html += "<style>body{background-color:#f8f9fa;} #ganttChartContainer {min-height: 250px; margin-bottom:20px;} .card-header h4 { margin-bottom:0; }</style>";
  html += "</head><body>";

  html += "<nav class='navbar navbar-expand-lg navbar-dark bg-dark shadow-sm'>";
  html += "  <div class='container-fluid'>";
  html += "    <a class='navbar-brand' href='/'>Asset Availability</a>";
  html += "    <button class='navbar-toggler' type='button' data-bs-toggle='collapse' data-bs-target='#navbarNav'><span class='navbar-toggler-icon'></span></button>";
  html += "    <div class='collapse navbar-collapse' id='navbarNav'>";
  html += "      <ul class='navbar-nav ms-auto'>";
  html += "        <li class='nav-item'><a class='nav-link' href='/'>Dashboard</a></li>";
  html += "        <li class='nav-item'><a class='nav-link' href='/events'>Event Log</a></li>";
  html += "        <li class='nav-item'><a class='nav-link' href='/analytics-compare'>Compare Assets</a></li>";
  html += "        <li class='nav-item'><a class='nav-link' href='/config'>Setup</a></li>";
  html += "      </ul>";
  html += "    </div>";
  html += "  </div>";
  html += "</nav>";

  html += "<div class='container mt-4'>";
  html += "  <h2 class='mb-3'>Asset Analytics: <span id='assetNameInHeader'>" + assetName + "</span></h2>";
  html += "  <div id='alertPlaceholder'></div>"; 
  html += "  <div class='row mb-4' id='kpiMetrics'></div>";
  html += "  <div class='row g-4'>";
  html += "    <div class='col-lg-4'>"; 
  html += "      <div class='card h-100 shadow-sm'><div class='card-header'><h4>Controls</h4></div><div class='card-body'>";
  html += "          <div class='mb-3'><label for='fromTime' class='form-label'>From:</label><input type='datetime-local' id='fromTime' class='form-control'></div>";
  html += "          <div class='mb-3'><label for='toTime' class='form-label'>To:</label><input type='datetime-local' id='toTime' class='form-control'></div>";
  html += "          <p class='form-text small'>Timeline shows asset-specific START/STOP events and system events where this asset is primary, within the selected date range.</p>";
  html += "          <button class='btn btn-primary w-100 mt-3' id='refreshTimelineBtn'>Refresh Timeline</button>";
  html += "          <button class='btn btn-secondary w-100 mt-2' id='exportPng'>Export Chart as PNG</button>";
  html += "      </div></div>";
  html += "    </div>";
  html += "    <div class='col-lg-8'>"; 
  html += "      <div class='card h-100 shadow-sm'><div class='card-header'><h4 id='chartTitle'>Asset State Timeline</h4></div>";
  html += "        <div class='card-body'><div id='ganttChartContainer'><canvas id='eventGanttChart'></canvas></div></div>";
  html += "      </div>";
  html += "    </div>";
  html += "  </div>";
  html += "  <div class='card mt-4 shadow-sm'><div class='card-header'><h4>Top Longest Stops (in selected range)</h4></div><div class='card-body table-responsive'><table class='table table-sm table-striped' id='topStopsTable'><thead><tr><th>#</th><th>Duration</th><th>Start Time</th><th>End Time</th><th>Reason/Note</th></tr></thead><tbody></tbody></table></div></div>";
  html += "  <div class='card mt-4 shadow-sm'><div class='card-header'><h4>Recent Raw Events for this Asset (Last 5)</h4></div><div class='card-body table-responsive'>";
  html += "    <table class='table table-sm table-striped table-hover'><thead><tr><th>Date</th><th>Time</th><th>Event</th><th>State Val</th><th>Avail%</th><th>Note</th></tr></thead><tbody id='recentEvents'></tbody></table>";
  html += "  </div></div>";
  html += "</div>"; 
  html += "<script>";
  html += R"rawliteral(

// --- Constants and Global Variables (defined at the very top) ---
const GREEN_COLOR = 'rgba(25, 135, 84, 0.8)'; 
const RED_COLOR = 'rgba(220, 53, 69, 0.8)';   
const AMBER_COLOR = 'rgba(255, 193, 7, 0.8)'; 
const GREY_COLOR = 'rgba(108, 117, 125, 0.7)'; 
const INFO_COLOR = 'rgba(13, 202, 240, 0.7)';  

let asset = ''; 
let allEvents = []; 
let eventGanttChart = null; 
let downtimeReasonsConfig = []; 
let longStopThresholdSecs = 300; 

// --- ALL FUNCTION DEFINITIONS ARE PLACED HERE, BEFORE ANY EXECUTABLE CODE ---

const alertPlaceholderDOM = document.getElementById('alertPlaceholder');
function showAlert(message, type) { 
    if (!alertPlaceholderDOM) { console.error("alertPlaceholder not found for message:", message); return; }
    const wrapper = document.createElement('div'); 
    wrapper.innerHTML = `<div class="alert alert-${type} alert-dismissible fade show" role="alert"><div>${message}</div><button type="button" class="btn-close" data-bs-dismiss="alert" aria-label="Close"></button></div>`; 
    alertPlaceholderDOM.innerHTML = ''; 
    alertPlaceholderDOM.append(wrapper); 
};

function formatSecondsToHHMMSS(totalSeconds) {
    if (isNaN(totalSeconds) || totalSeconds < 0) totalSeconds = 0;
    const h = Math.floor(totalSeconds / 3600);
    const m = Math.floor((totalSeconds % 3600) / 60);
    const s = Math.floor(totalSeconds % 60);
    const pad = (num) => String(num).padStart(2, '0');
    if (h > 0) { return `${pad(h)}:${pad(m)}:${pad(s)}`; } 
    else { return `${pad(m)}:${pad(s)}`; }
}

function formatShortDateTime(dateObj) {
    if (!(dateObj instanceof Date) || isNaN(dateObj.getTime())) return 'N/A';
    return dateObj.toLocaleDateString([], {day:'2-digit',month:'2-digit',year:'2-digit'}) + ' ' + dateObj.toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'});
}

function normalizeAssetName(name) { return (name || '').toLowerCase().replace(/\s+/g, ''); }

function parseEventDate(eventRow) { 
    if (!eventRow || eventRow.length < 2) return null; 
    try { 
        let [d, m, y] = eventRow[0].split('/').map(Number); 
        let [hh, mm, ss] = eventRow[1].split(':').map(Number); 
        if (isNaN(d) || isNaN(m) || isNaN(y) || isNaN(hh) || isNaN(mm) || isNaN(ss)) { console.warn("NaN in date/time parts for parseEventDate:", eventRow[0], eventRow[1]); return null;}
        return new Date(Date.UTC(y, m - 1, d, hh, mm, ss)); 
    } catch (e) { console.error('Error parsing date for eventRow:', eventRow[0],eventRow[1], e); return null; } 
}

function toDatetimeLocal(dt) { 
    if (!(dt instanceof Date) || isNaN(dt.getTime())) dt = new Date(); 
    try { const tzo = dt.getTimezoneOffset() * 60000; const loc = new Date(dt.getTime() - tzo); return loc.toISOString().slice(0, 16); } 
    catch (e) { const n = new Date(Date.now() - (new Date().getTimezoneOffset() * 60000)); return n.toISOString().slice(0, 16); } 
}

function getReasonFromNote(note) {
    if (!note || !Array.isArray(downtimeReasonsConfig)) return null;
    const cleanedNote = note.trim().toLowerCase();
    for (const reason of downtimeReasonsConfig) {
        if (reason && reason.length > 0 && cleanedNote.includes(reason.toLowerCase())) {
            return reason;
        }
    }
    if (cleanedNote.startsWith("root cause:")) return "System Trigger";
    return null; 
}

function renderKPIs(kpiSummaryData, customMessage = null) {
  const kpiDiv = document.getElementById('kpiMetrics'); if (!kpiDiv) { console.error("KPI div not found"); return; }
  if (customMessage) { kpiDiv.innerHTML = `<div class='col-12'><div class='alert alert-warning'>${customMessage}</div></div>`; return; }
  if (!kpiSummaryData || Object.keys(kpiSummaryData).length === 0) { kpiDiv.innerHTML = "<div class='col-12'><div class='alert alert-info'>No KPI data for current selection.</div></div>"; return; }
  try {
    let kpiHtml = "";
    const formattedKpis = {
        "Stops in Period": kpiSummaryData.stopFrequency,
        "Total Run Time": formatSecondsToHHMMSS(kpiSummaryData.totalRunTimeS),
        "Total Stop Time": formatSecondsToHHMMSS(kpiSummaryData.totalStopTimeS),
        "Availability": `${parseFloat(kpiSummaryData.availability).toFixed(2)}%`
    };
    for (const [key, value] of Object.entries(formattedKpis)) { kpiHtml += `<div class="col-lg-3 col-md-6 col-6 mb-4"><div class="card text-center h-100 shadow-sm"><div class="card-header small fw-bold">${key}</div><div class="card-body d-flex align-items-center justify-content-center p-2"><p class="card-text fs-5 fw-bold mb-0">${value !== null && value !== undefined ? value : '-'}</p></div></div></div>`; }
    kpiDiv.innerHTML = kpiHtml;
  } catch (e) { console.error('Error rendering KPIs:', e); kpiDiv.innerHTML = "<div class='col-12'><div class='alert alert-danger'>Error rendering KPIs.</div></div>"; }
}

function processEventsForGanttData(sourceEvents, viewStartDate, viewEndDate) {
    let kpiDefaults = { stopFrequency: 0, totalRunTimeS: 0, totalStopTimeS: 0, availability: 0 };
    if (!sourceEvents || !(viewStartDate instanceof Date) || !(viewEndDate instanceof Date) || isNaN(viewStartDate.getTime()) || isNaN(viewEndDate.getTime()) || viewStartDate >= viewEndDate) {
        console.warn("processEventsForGanttData: Invalid input. Events:", sourceEvents, "Start:", viewStartDate, "End:", viewEndDate);
        return { segments: [], kpiData: kpiDefaults, longestStops: [] };
    }

    let processedSegments = [];
    let kpiStopFrequency = 0, kpiTotalRunTimeS = 0, kpiTotalStopTimeS = 0;

    let sortedEvents = sourceEvents
        .map(e_raw => ({ date: parseEventDate(e_raw), raw: e_raw })) // Keep raw for note access
        .filter(e => e.date && !isNaN(e.date.getTime()))
        .sort((a, b) => a.date.getTime() - b.date.getTime());

    if (sortedEvents.length === 0) {
        // No valid events for this asset at all, assume one state for the period or based on wider knowledge (difficult here)
        // For now, let's assume it was effectively stopped if no specific events are present.
        processedSegments.push({
            start: viewStartDate, end: viewEndDate, state: 'UNKNOWN', color: GREY_COLOR, 
            note: 'No event data for asset in this period', duration: (viewEndDate.getTime() - viewStartDate.getTime()) / 1000, 
            rawEventNote: 'No event data'
        });
        kpiTotalStopTimeS = (viewEndDate.getTime() - viewStartDate.getTime()) / 1000; 
    } else {
        let currentSegmentStartTime = viewStartDate;
        let currentStateIsRunning = null;
        let currentSegmentNote = 'Initial state';

        let lastEventBeforeWindow = sortedEvents.filter(e => e.date <= viewStartDate).pop();
        if (lastEventBeforeWindow) {
            currentStateIsRunning = (lastEventBeforeWindow.raw[4] === '1'); // State from col 4
            currentSegmentNote = lastEventBeforeWindow.raw[13] || (currentStateIsRunning ? 'Running' : 'Stopped');
            // If the last event before window is exactly AT window start, segment starts there.
            // If it's BEFORE, the state carries forward, and segment starts at viewStartDate.
            currentSegmentStartTime = (lastEventBeforeWindow.date.getTime() < viewStartDate.getTime()) ? viewStartDate : lastEventBeforeWindow.date;
        } else { // No event before viewStartDate. State before the first event IN window?
            if (sortedEvents[0].date > viewStartDate) { // First event is after window start
                currentStateIsRunning = !(sortedEvents[0].raw[4] === '1'); // Assume opposite of first event's outcome
                currentSegmentNote = currentStateIsRunning ? 'Assumed Running' : 'Assumed Stopped';
            } else { // First event IS AT window start
                currentStateIsRunning = (sortedEvents[0].raw[4] === '1');
                currentSegmentNote = sortedEvents[0].raw[13] || (currentStateIsRunning ? 'Running' : 'Stopped');
            }
            currentSegmentStartTime = viewStartDate;
        }
        
        for (const event of sortedEvents) {
            if (event.date < currentSegmentStartTime) continue; // Skip if already processed or before current segment start
            if (event.date >= viewEndDate) { // Event is at or after the end of our window
                 if (currentSegmentStartTime < viewEndDate && currentStateIsRunning !== null) {
                     processedSegments.push({start: new Date(currentSegmentStartTime.getTime()), end: new Date(viewEndDate.getTime()), state: currentStateIsRunning ? 'RUNNING':'STOPPED', note: currentSegmentNote, rawEventNote: currentSegmentNote});
                }
                currentSegmentStartTime = viewEndDate; 
                break; 
            }

            // Segment from currentSegmentStartTime up to this event's time
            if (event.date > currentSegmentStartTime && currentStateIsRunning !== null) {
                 processedSegments.push({start: new Date(currentSegmentStartTime.getTime()), end: new Date(event.date.getTime()), state: currentStateIsRunning ? 'RUNNING':'STOPPED', note: currentSegmentNote, rawEventNote: currentSegmentNote });
            }
            
            // Current event defines the new state from its time
            currentSegmentStartTime = event.date;
            currentStateIsRunning = (event.raw[4] === '1'); // State from col 4
            currentSegmentNote = event.raw[13] || (currentStateIsRunning ? 'Running' : 'Stopped'); // Note from col 13
        }

        // Add final segment from last processed event's time to viewEndDate
        if (currentSegmentStartTime < viewEndDate && currentStateIsRunning !== null) {
            processedSegments.push({start: new Date(currentSegmentStartTime.getTime()), end: new Date(viewEndDate.getTime()), state: currentStateIsRunning ? 'RUNNING':'STOPPED', note: currentSegmentNote, rawEventNote: currentSegmentNote});
        }
    }
    
    let finalGanttSegments = [];
    let longestStopsList = [];
    processedSegments.forEach(seg => {
        if (!(seg.start instanceof Date) || !(seg.end instanceof Date) || isNaN(seg.start.getTime()) || isNaN(seg.end.getTime())) {
            console.warn("Invalid segment dates encountered during post-processing:", seg); return;
        }
        seg.duration = (seg.end.getTime() - seg.start.getTime()) / 1000;
        if (seg.duration < 0.1) return; // Skip very tiny segments

        if (seg.state === 'RUNNING') {
            kpiTotalRunTimeS += seg.duration;
            seg.color = GREEN_COLOR;
        } else { // STOPPED or UNKNOWN
            kpiTotalStopTimeS += seg.duration;
            if (seg.start < seg.end) { kpiStopFrequency++; } // Count as a stop only if it has some duration
            const reason = getReasonFromNote(seg.rawEventNote); // Use rawEventNote for reason parsing
            seg.actualReason = reason; 
            if (reason === 'Maintenance') seg.color = AMBER_COLOR;
            else if (reason === 'Material Shortage') seg.color = 'orange'; 
            else if (reason === 'System Trigger') seg.color = GREY_COLOR;
            else if (reason) seg.color = GREY_COLOR; 
            else if (seg.state === 'UNKNOWN') seg.color = GREY_COLOR;
            else seg.color = RED_COLOR;
            if (seg.duration >=1 ) longestStopsList.push(seg);
        }
        finalGanttSegments.push(seg);
    });

    longestStopsList.sort((a, b) => b.duration - a.duration);
    const totalEffectiveDurationS = kpiTotalRunTimeS + kpiTotalStopTimeS;
    const availability = totalEffectiveDurationS > 0 ? (kpiTotalRunTimeS / totalEffectiveDurationS) * 100 : 0;
    
    console.log("processEventsForGanttData - Output:", { viewStartDate: viewStartDate.toISOString(), viewEndDate: viewEndDate.toISOString(), calculatedSegments: JSON.parse(JSON.stringify(finalGanttSegments)), calculatedKpiData: JSON.parse(JSON.stringify({ stopFrequency: kpiStopFrequency, totalRunTimeS: kpiTotalRunTimeS, totalStopTimeS: kpiTotalStopTimeS, availability: availability })), calculatedLongestStops: JSON.parse(JSON.stringify(longestStopsList.slice(0,5))) });

    return { 
        segments: finalGanttSegments, 
        kpiData: { stopFrequency: kpiStopFrequency, totalRunTimeS: kpiTotalRunTimeS, totalStopTimeS: kpiTotalStopTimeS, availability: availability },
        longestStops: longestStopsList.slice(0, 5) 
    };
}

function renderGanttChart(ganttData, viewStartDate, viewEndDate) {
  const chartTitleEl = document.getElementById('chartTitle');
  const assetNameForTitle = asset || "Selected Asset"; 
  chartTitleEl.textContent = `Asset State Timeline for ${assetNameForTitle}`; 

  const minTimeSafe = (viewStartDate instanceof Date && !isNaN(viewStartDate.getTime())) ? viewStartDate.getTime() : (Date.now() - 12*60*60*1000);
  const maxTimeSafe = (viewEndDate instanceof Date && !isNaN(viewEndDate.getTime())) ? viewEndDate.getTime() : Date.now();

  if (!ganttData || !ganttData.segments || ganttData.segments.length === 0) {
    renderKPIs(null, `No displayable event data for ${assetNameForTitle} in the selected range.`);
    if (eventGanttChart) { eventGanttChart.destroy(); eventGanttChart = null; }
    renderTopStopsTable([]);
    chartTitleEl.textContent += ` (No data in range)`;
    return;
  }
  
  chartTitleEl.textContent = `Asset State Timeline for ${assetNameForTitle} from ${formatShortDateTime(viewStartDate)} to ${formatShortDateTime(viewEndDate)}`;
  renderKPIs(ganttData.kpiData);

  if (eventGanttChart) { eventGanttChart.destroy(); eventGanttChart = null; }
  
  const chartDataForGantt = {
        labels: [assetNameForTitle], 
        datasets: [{
            label: 'Asset State', 
            data: ganttData.segments.map(segment => ({
                x: [segment.start.getTime(), segment.end.getTime()],
                y: assetNameForTitle, 
                _details: segment 
            })),
            backgroundColor: ganttData.segments.map(segment => segment.color),
            borderColor: ganttData.segments.map(segment => tinycolor(segment.color).darken(10).toString()),
            borderWidth: 1, barPercentage: 0.9, categoryPercentage: 0.9  
        }]
    };

  const ctx = document.getElementById('eventGanttChart').getContext('2d');
  try {
    eventGanttChart = new Chart(ctx, {
      type: 'bar', data: chartDataForGantt,
      options: {
        indexAxis: 'y', responsive: true, maintainAspectRatio: false,
        scales: { x: { type: 'time', min: minTimeSafe, max: maxTimeSafe, time: { unit: 'minute', tooltipFormat: 'MMM d, HH:mm:ss', displayFormats: { minute: 'HH:mm', hour: 'MMM d HH:mm' } }, title: { display: true, text: 'Time' } }, y: { title: { display: false } } },
        plugins: { legend: { display: false }, tooltip: { callbacks: { title: function() { return ''; }, label: function(context) { const details = context.raw._details; if (!details) return ''; let label = []; label.push(`State: ${details.state}`); label.push(`Duration: ${formatSecondsToHHMMSS(details.duration)}`); if (details.actualReason && details.state !== 'RUNNING') { label.push(`Reason: ${details.actualReason}`); } else if (details.note && details.state !== 'RUNNING' && details.note.trim() !== "") { label.push(`Note: ${details.note}`); } label.push(`Start: ${formatShortDateTime(new Date(details.start))}`); label.push(`End: ${formatShortDateTime(new Date(details.end))}`); return label; } } } }
      }
    });
  } catch (e) { console.error("Chart.js rendering error:", e); showAlert("Could not render the timeline chart.", "danger"); }
  renderTopStopsTable(ganttData.longestStops);
}

function renderTopStopsTable(longestStops) {
    const tbody = document.querySelector("#topStopsTable tbody"); if (!tbody) return; tbody.innerHTML = "";
    if (!longestStops || longestStops.length === 0) { tbody.innerHTML = "<tr><td colspan='5' class='text-center'>No stop events in the selected range.</td></tr>"; return; }
    longestStops.forEach((stop, index) => { let tr = tbody.insertRow(); tr.insertCell().textContent = index + 1; tr.insertCell().textContent = formatSecondsToHHMMSS(stop.duration); tr.insertCell().textContent = formatShortDateTime(new Date(stop.start)); tr.insertCell().textContent = formatShortDateTime(new Date(stop.end)); tr.insertCell().textContent = stop.actualReason || stop.note || 'N/A'; });
}

function renderRecentEvents() { 
  const tbody = document.getElementById('recentEvents'); if (!tbody) return; tbody.innerHTML = "";
  if (!allEvents || allEvents.length === 0) { 
    tbody.innerHTML = `<tr><td colspan='6' class='text-center'>No events recorded for asset: ${asset||"Unknown"}.</td></tr>`; 
    return; 
  }
  const eventsToDisplay = allEvents.slice(-5).reverse(); 
  if (eventsToDisplay.length === 0) { 
    tbody.innerHTML = `<tr><td colspan='6' class='text-center'>No recent events for ${asset||"Unknown"}.</td></tr>`; 
    return; 
  }
  eventsToDisplay.forEach(eventRow => { 
    try {
      if (!Array.isArray(eventRow) || eventRow.length < 6) { // Minimum check
        let tr = tbody.insertRow(), td = tr.insertCell(); 
        td.colSpan = 6; 
        td.textContent = "Malformed event data."; 
        td.style.color = "orange"; 
        return; 
      } 
            let [date, time, , eventType, state, avail, , , , , , , , note] = eventRow;
            let tr = tbody.insertRow();
            tr.insertCell().textContent = date || "N/A"; 
            tr.insertCell().textContent = time || "N/A"; 
            tr.insertCell().textContent = eventType || "N/A";
            tr.insertCell().textContent = state === '1' ? 'UP/RUN' : (state === '0' ? 'DOWN/STOP' : (state || "N/A")); 
            tr.insertCell().textContent = !isNaN(parseFloat(avail)) ? parseFloat(avail).toFixed(1) : "N/A";
            tr.insertCell().textContent = note || ""; // Now correctly displays the note
    } catch (e) { 
      console.error('Error rendering recent event row:', eventRow, e); 
      let tr = tbody.insertRow(), td = tr.insertCell(); 
      td.colSpan = 6; 
      td.textContent = "Error displaying row."; 
      td.style.color = "red"; 
    } 
  });
}

var tinycolor=function(c){
  return{darken:function(a){ var i,rgb="#",p=String(c).replace(/[^0-9a-f]/gi,"");
  if(p.length<6)p=p[0]+p[0]+p[1]+p[1]+p[2]+p[2];a=a||0;for(i=0;i<3;i++){var h=parseInt(p.substr(i*2,2),16);h=Math.round(Math.min(Math.max(0,h+(h*-(a/100))),255)).toString(16);rgb+=("00"+h).substr(h.length)}
  return rgb;},toString:function(){
    return c;}
    }
    };

// --- Main Logic Functions (that call the above renderers) ---
function setupRangePickers() { 
  let defaultFromDate, defaultToDate;
  let sortedValidEvents = allEvents.map(e_raw => ({ date: parseEventDate(e_raw), raw: e_raw })).filter(e => e.date && !isNaN(e.date.getTime())).sort((a, b) => a.date.getTime() - b.date.getTime());
  if (sortedValidEvents.length > 0) { defaultToDate = new Date(sortedValidEvents[sortedValidEvents.length - 1].date.getTime()); defaultFromDate = new Date(defaultToDate.getTime() - (12 * 60 * 60 * 1000)); if (sortedValidEvents.length > 0 && sortedValidEvents[0].date.getTime() > defaultFromDate.getTime()) { defaultFromDate = new Date(sortedValidEvents[0].date.getTime()); } if (defaultFromDate > defaultToDate && sortedValidEvents.length > 0) { defaultFromDate = new Date(sortedValidEvents[0].date.getTime());} }
  else { defaultToDate = new Date(); defaultFromDate = new Date(defaultToDate.getTime() - (12 * 60 * 60 * 1000)); }
  const fromTimeEl = document.getElementById('fromTime'); const toTimeEl = document.getElementById('toTime');
  if(fromTimeEl) fromTimeEl.value = toDatetimeLocal(defaultFromDate); if(toTimeEl) toTimeEl.value = toDatetimeLocal(defaultToDate);

  const refreshButton = document.getElementById('refreshTimelineBtn');
  if(refreshButton) { 
      const refreshHandler = () => {
          const fromDate = new Date(document.getElementById('fromTime').value);
          const toDate = new Date(document.getElementById('toTime').value);
           if (!isNaN(fromDate.getTime()) && !isNaN(toDate.getTime()) && fromDate <= toDate) { 
              const ganttData = processEventsForGanttData(allEvents, fromDate, toDate); 
              renderGanttChart(ganttData, fromDate, toDate);
          } else if(fromDate > toDate) {
               showAlert("Date range: 'From' date cannot be after 'To'.", "warning");
               if(eventGanttChart){eventGanttChart.destroy();eventGanttChart=null;}
               renderKPIs(null, "Invalid date range selected.");
               renderTopStopsTable([]);
          } else {
               showAlert("Invalid date input. Please check From/To dates.", "danger");
          }
      };
      refreshButton.onclick = refreshHandler;
      // Also attach to date inputs if direct refresh button is not the only trigger
      if(fromTimeEl) fromTimeEl.onchange = refreshHandler;
      if(toTimeEl) toTimeEl.onchange = refreshHandler;
  }
  const exportButton = document.getElementById('exportPng'); if (exportButton) { exportButton.onclick = function () { if (!eventGanttChart ) return showAlert("No chart to export.", "warning"); try { let url = eventGanttChart.toBase64Image('image/png', 1.0); let a = document.createElement('a'); a.href = url; a.download = `Gantt_${asset || 'chart'}.png`; a.click(); } catch (e) { console.error('Error exporting chart:', e), showAlert("Error exporting chart.", "danger"); } }; }
}

// =========================================================================
// --- Block 1:  fetchAnalyticsData() function 
// =========================================================================

function fetchAnalyticsData() {
  try { asset = decodeURIComponent(new URLSearchParams(window.location.search).get("asset") || ""); } catch(e) {console.error("Error decoding asset from URL", e);asset = "";}
  const assetNameElement = document.getElementById('assetNameInHeader'); if (assetNameElement) assetNameElement.textContent = asset || "None Selected";
  if (!asset) { showAlert("No asset selected.", "warning"); if(typeof renderKPIs === 'function')renderKPIs(null, "No asset specified."); if (eventGanttChart) {eventGanttChart.destroy();eventGanttChart=null;} const ct=document.getElementById('chartTitle');if(ct)ct.textContent='Asset State Timeline'; if(typeof renderTopStopsTable === 'function')renderTopStopsTable([]); return; }
  
  // --- PATCH: Corrected Promise.all with cache-busting parameter ---
  Promise.all([ 
    fetch('/api/events?_=' + new Date().getTime()).then(r => { if(!r.ok) throw new Error(`API Events: ${r.status}`); return r.json(); }), 
    fetch('/api/config').then(r => { if(!r.ok) throw new Error(`API Config: ${r.status}`); return r.json(); }) 
  ])
  .then(([rawEvents, configData]) => { 
      console.log("Analytics Page: rawEvents FROM SERVER (entire log):", JSON.parse(JSON.stringify(rawEvents)));
      longStopThresholdSecs = (configData && configData.longStopThresholdSec != undefined) ? configData.longStopThresholdSec : 300; 
      downtimeReasonsConfig = (configData && configData.downtimeReasons) ? configData.downtimeReasons : [];
      allEvents = Array.isArray(rawEvents) ? rawEvents.filter(e_row => {
          if (!e_row) return false;
          const eventAssetField = e_row[2]; const eventTypeField = e_row[3]; 
          const rowLength = e_row.length;
          const targetAssetNormalized_fetch = normalizeAssetName(asset);
          let passes = true; let reasonForFailure = "";
          if (rowLength < 12) { passes = false; reasonForFailure = `Length < 12 (is ${rowLength}).`;}
          else if (!eventAssetField && typeof eventAssetField !== 'string') { passes = false; reasonForFailure = "Asset field e[2] missing/invalid.";}
          else { const normalizedEventAsset = normalizeAssetName(eventAssetField); if (normalizedEventAsset !== targetAssetNormalized_fetch) { passes = false; reasonForFailure = `Asset mismatch: '${normalizedEventAsset}' !== '${targetAssetNormalized_fetch}'.`;}}
          if (targetAssetNormalized_fetch === "line 1" || targetAssetNormalized_fetch === "line 2" ) { console.log( `FILTER ROW (Target: ${asset}): Raw: [${e_row.map(f => `"${f}"`).join(',')}] Len: ${rowLength}, Asset: '${eventAssetField}', Type: '${eventTypeField}', Passes: ${passes}${passes ? '' : ` (FAIL: ${reasonForFailure})`}`); }
          return passes;
      }) : [];
      console.log("Analytics Page: allEvents for asset '" + asset + "' AFTER JS FILTER:", JSON.parse(JSON.stringify(allEvents)));
      if (typeof setupRangePickers === 'function') setupRangePickers(); 
      const refreshBtn = document.getElementById('refreshTimelineBtn');
      if (refreshBtn) { refreshBtn.click(); } 
      else { console.warn("Refresh button not found. Manually triggering render."); const fromDate = new Date(document.getElementById('fromTime').value); const toDate = new Date(document.getElementById('toTime').value); if (!isNaN(fromDate.getTime()) && !isNaN(toDate.getTime()) && fromDate <= toDate) { const ganttData = processEventsForGanttData(allEvents, fromDate, toDate); renderGanttChart(ganttData, fromDate, toDate); } else { showAlert("Initial dates invalid.", "warning"); if(typeof renderKPIs === 'function')renderKPIs(null, "Initial chart render failed."); if(eventGanttChart){eventGanttChart.destroy();eventGanttChart=null;} if(typeof renderTopStopsTable === 'function')renderTopStopsTable([]); } }
      if (typeof renderRecentEvents === 'function') renderRecentEvents(); 
  }).catch(e => { console.error("Fetch error or .then() error:", e); showAlert(`Data Load Failed: ${e.message||'Unknown error'}`, 'danger'); if(typeof renderKPIs === 'function')renderKPIs(null, "Data load failed."); if(eventGanttChart){eventGanttChart.destroy();eventGanttChart=null;} const ct=document.getElementById('chartTitle');if(ct)ct.textContent = 'Asset State Timeline'; if(typeof renderTopStopsTable === 'function')renderTopStopsTable([]); });
}

// --- DOMContentLoaded LISTENER (at the very end) ---
document.addEventListener('DOMContentLoaded', fetchAnalyticsData);
)rawliteral";
  html += "</script>";
  html += "<script src='/assets/bootstrap.bundle.min.js'></script>";
  html += "</body></html>";
  return html;
}
  
// ------------------------------------------------------------------------------------------------------------------------------------
//  Compare Assets Page Function
// ------------------------------------------------------------------------------------------------------------------------------------

String htmlAnalyticsCompare() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Compare Assets</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='/assets/bootstrap.min.css' rel='stylesheet'>";
  html += "<link rel='stylesheet' href='/assets/bootstrap-icons.min.css'>";
  html += "<script src='/assets/chart.min.js'></script>";
  html += "<style>";
  html += "body{background-color:#f8f9fa;font-family:Roboto,Arial,sans-serif;}";
  html += ".leaderboard-badge{font-size:1.4em;margin-right:0.5em;}";
  html += ".leaderboard-row{background:linear-gradient(90deg,#f8ffec,#e6f2ff);border-radius:16px;box-shadow:0 2px 8px #0001;margin-bottom:11px;padding:12px 20px;display:flex;align-items:center;}";
  html += ".leaderboard-row.top{background:linear-gradient(90deg,#fffbe6,#ffe5ec);border:2px solid #ffd700;}";
  html += ".leaderboard-row .asset-name{font-weight:700;font-size:1.15em;flex:1;}";
  html += ".kpi-cards{display:flex;gap:1em;flex-wrap:wrap;justify-content:center;margin-bottom:24px;}";
  html += ".kpi-card{background:#fff;flex:1 1 160px;min-width:140px;border-radius:10px;box-shadow:0 1px 5px #0002;display:flex;flex-direction:column;align-items:center;justify-content:center;padding:16px 10px 12px 10px;}";
  html += ".kpi-card .kpi-icon{font-size:2em;margin-bottom:5px;}";
  html += ".kpi-card .kpi-label{color:#888;font-size:0.93em;}";
  html += ".asset-of-day{background:linear-gradient(90deg,#e0ffe0 60%,#fffbe6 100%);border-left:8px solid #4caf50;font-size:1.05em;padding:12px 16px;margin-bottom:18px;border-radius:6px;}";
  html += ".bar-charts-row{display:flex;flex-wrap:wrap;gap:1.5em;}";
  html += ".bar-charts-row .chart-col{flex:1 1 340px;min-width:320px;}";
  html += "</style>";
  html += "</head><body>";

  html += "<nav class='navbar navbar-expand-lg navbar-dark bg-dark shadow-sm'>";
  html += "<div class='container-fluid'>";
  html += "  <a class='navbar-brand' href='/'>Asset Availability</a>";
  html += "  <button class='navbar-toggler' type='button' data-bs-toggle='collapse' data-bs-target='#navbarNav'><span class='navbar-toggler-icon'></span></button>";
  html += "  <div class='collapse navbar-collapse' id='navbarNav'>";
  html += "    <ul class='navbar-nav ms-auto'>";
  html += "      <li class='nav-item'><a class='nav-link' href='/'>Dashboard</a></li>";
  html += "      <li class='nav-item'><a class='nav-link' href='/events'>Event Log</a></li>";
  html += "      <li class='nav-item'><a class='nav-link active' href='/analytics-compare'>Compare Assets</a></li>";
  html += "      <li class='nav-item'><a class='nav-link' href='/config'>Setup</a></li>";
  html += "    </ul>";
  html += "  </div>";
  html += "</div>";
  html += "</nav>";

  html += "<div class='container-fluid mt-4' style='max-width:1600px;'>";
  html += "<div id='assetOfDay' class='asset-of-day mb-3'></div>";
  html += "<div id='leaderboard'></div>";
  html += "<div class='kpi-cards' id='kpiCards'></div>";

  html += "<div class='bar-charts-row'>";
  html += "  <div class='chart-col'><div class='card mb-3'><div class='card-body'><canvas id='barAvail' height='220'></canvas></div></div></div>";
  html += "  <div class='chart-col'><div class='card mb-3'><div class='card-body'><canvas id='barStops' height='220'></canvas></div></div></div>";
  html += "  <div class='chart-col'><div class='card mb-3'><div class='card-body'><canvas id='barMTBF' height='220'></canvas></div></div></div>";
  html += "  <div class='chart-col'><div class='card mb-3'><div class='card-body'><canvas id='barReasons' height='220'></canvas></div></div></div>";
  html += "</div>";

  html += "<div class='card mt-3 shadow-sm'><div class='card-header'><h4>Last Event Log Summary</h4></div><div class='card-body table-responsive'>";
  html += "  <table class='table table-striped table-hover'><thead><tr><th>Asset</th><th>Availability (%)</th><th>Runtime</th><th>Downtime</th><th>Stops</th><th>MTBF</th><th>MTTR</th></tr></thead><tbody id='compareTable'></tbody></table>";
  html += "</div></div>";
  html += "</div>";

  html += "<script>";
  html += R"rawliteral(
let allEventsCompare = [], allAssetNamesCompare = [], configDowntimeReasonsCompare = [];

function fetchCompareDataPage() {
  fetch('/api/config').then(e => e.json()).then(configData => {
    configDowntimeReasonsCompare = configData.downtimeReasons || [];
    allAssetNamesCompare = configData.assets.map(e => e.name);
    
    // --- SIM PATCH: Added cache-busting parameter ---
    fetch('/api/events?_=' + new Date().getTime()).then(e => e.json()).then(eventsData => {
      allEventsCompare = (eventsData || []).map(e => Array.isArray(e) ? e : [])
                                         .filter(e => e.length >= 12 && allAssetNamesCompare.includes(e[2]));
      
      renderLeaderboardPage();
      renderAssetOfDay();
      renderKPICards();
      renderCompareChartsPage();
      renderCompareTablePage();
    }).catch(e => console.error("CompareEventsFetchErr:", e));
  }).catch(e => console.error("CompareConfigFetchErr:", e));
}

function getLastMetricCompare(events, metricIndex) {
  if (!events || events.length === 0) return 0;
  const lastEvent = events[events.length - 1];
  return lastEvent ? parseFloat(lastEvent[metricIndex]) : 0;
}

function renderLeaderboardPage() {
  let byAsset = {};
  allAssetNamesCompare.forEach(e => { byAsset[e] = [] });
  for (let e of allEventsCompare) { byAsset[e[2]] && byAsset[e[2]].push(e); }

  let leaderboard = allAssetNamesCompare.map(name => {
    const assetEvents = byAsset[name] || [];
    const availability = getLastMetricCompare(assetEvents, 5);
    const totalStops = parseInt(getLastMetricCompare(assetEvents, 10), 10) || 0;
    return { name: name, availability: availability, stops: totalStops };
  });

  leaderboard.sort((a, b) => b.availability - a.availability);
  
  const medals = ["<i class='bi bi-trophy-fill' style='color:#ffd700'></i>", "<i class='bi bi-trophy-fill' style='color:#aaa'></i>", "<i class='bi bi-trophy-fill' style='color:#cd7f32'></i>"];
  let html = "";
  leaderboard.forEach((row, i) => {
    html += `<div class='leaderboard-row${i==0?" top":""}'><span class='leaderboard-badge'>${medals[i]||""}</span><span class='asset-name'>${row.name}</span><span>Availability: <b>${row.availability.toFixed(2)}%</b></span><span style='margin-left:2em'>Stops: <b>${row.stops}</b></span></div>`;
  });
  document.getElementById("leaderboard").innerHTML = html;
}

function renderAssetOfDay() {
  let byAsset = {};
  allAssetNamesCompare.forEach(e => { byAsset[e] = [] });
  for (let e of allEventsCompare) { byAsset[e[2]] && byAsset[e[2]].push(e); }
  
  const best = allAssetNamesCompare.map(name => ({
    name: name,
    av: getLastMetricCompare(byAsset[name], 5)
  })).sort((a, b) => b.av - a.av)[0];
  
  if (best && best.av > 0) {
    document.getElementById("assetOfDay").innerHTML = `<i class="bi bi-star-fill" style="color:#43cea2;font-size:1.3em"></i> <b>Asset of the Day:</b> <span style="color:#388e3c;font-weight:700;">${best.name}</span> with <span style="color:#388e3c;">${best.av.toFixed(2)}% Availability</span>.`;
  }
}

function renderKPICards() {
  let byAsset = {};
  allAssetNamesCompare.forEach(e => { byAsset[e] = [] });
  for (let e of allEventsCompare) { byAsset[e[2]] && byAsset[e[2]].push(e); }

  let topAvail = allAssetNamesCompare.map(name => getLastMetricCompare(byAsset[name], 5)).reduce((a,b) => Math.max(a,b), 0);
  let totalStops = allAssetNamesCompare.reduce((sum, name) => sum + (parseInt(getLastMetricCompare(byAsset[name], 10), 10) || 0), 0);
  let mtbfValues = allAssetNamesCompare.map(name => getLastMetricCompare(byAsset[name], 8)).filter(x => x > 0);
  let avgMTBF = mtbfValues.length ? mtbfValues.reduce((a,b) => a+b, 0) / mtbfValues.length : 0;
  
  let html = "";
  html += `<div class='kpi-card'><div class='kpi-icon'><i class="bi bi-trophy-fill" style="color:#ffc107"></i></div><div class='kpi-label'>Top Availability</div><div class='h4 mb-0'>${topAvail.toFixed(2)}%</div></div>`;
  html += `<div class='kpi-card'><div class='kpi-icon'><i class="bi bi-exclamation-triangle-fill" style="color:#ff9800"></i></div><div class='kpi-label'>Total Stops</div><div class='h4 mb-0'>${totalStops}</div></div>`;
  html += `<div class='kpi-card'><div class='kpi-icon'><i class="bi bi-stopwatch-fill" style="color:#4caf50"></i></div><div class='kpi-label'>Avg MTBF</div><div class='h4 mb-0'>${avgMTBF.toFixed(1)} min</div></div>`;
  document.getElementById("kpiCards").innerHTML = html;
}

function renderCompareChartsPage() {
  // --- PATCH: Standardize chart color and remove gradient function ---
  const CHART_COLOR = 'rgba(66, 165, 245, 0.7)'; // The blue color (#42a5f5) from your dashboard

  let byAsset = {}; allAssetNamesCompare.forEach(e => { byAsset[e] = [] });
  for (let e of allEventsCompare) { byAsset[e[2]] && byAsset[e[2]].push(e); }
  
  let labels = allAssetNamesCompare;
  let availData = labels.map(name => getLastMetricCompare(byAsset[name], 5));
  let stopsData = labels.map(name => parseInt(getLastMetricCompare(byAsset[name], 10), 10) || 0);
  let mtbfData = labels.map(name => getLastMetricCompare(byAsset[name], 8));
  
  let reasons = {};
  configDowntimeReasonsCompare.forEach(r => { if (r) reasons[r] = 0; });
  const startEventsWithReasons = allEventsCompare.filter(e => (e[3] || "").toUpperCase() === 'START' && e.length >= 14 && e[13]);
  for (let e of startEventsWithReasons) {
      let reasonText = e[13];
      let reason = reasonText.split(" - ")[0].trim();
      if (reason && reasons.hasOwnProperty(reason)) {
          reasons[reason]++;
      }
  }
  const reasonLabels = Object.keys(reasons).filter(e => reasons[e] > 0);
  const reasonData = reasonLabels.map(e => reasons[e]);

  ["barAvail", "barStops", "barMTBF", "barReasons"].forEach(id => {let el=document.getElementById(id); if(el && el.chartInstance) el.chartInstance.destroy();});
  const opts = t => ({plugins:{title:{display:true,text:t,font:{size:16}},legend:{display:false}},scales:{y:{beginAtZero:true}},responsive:true,maintainAspectRatio:false});

  let barStopsOpts = opts("Total Stops by Asset");
  barStopsOpts.scales.y.ticks = { precision: 0 };

  new Chart(document.getElementById("barAvail").getContext("2d"), {type:'bar',data:{labels:labels,datasets:[{label:'Avail %',data:availData,backgroundColor:CHART_COLOR, yAxisID:'y'}]},options:opts("Availability by Asset (%)")});
  new Chart(document.getElementById("barStops").getContext("2d"), {type:'bar',data:{labels:labels,datasets:[{label:'Total Stops',data:stopsData,backgroundColor:CHART_COLOR, yAxisID:'y'}]},options:barStopsOpts});
  new Chart(document.getElementById("barMTBF").getContext("2d"), {type:'bar',data:{labels:labels,datasets:[{label:'MTBF (min)',data:mtbfData,backgroundColor:CHART_COLOR, yAxisID:'y'}]},options:opts("MTBF by Asset (minutes)")});
  
  let brc=document.getElementById("barReasons");
  if(brc){if(reasonLabels.length > 0){new Chart(brc.getContext("2d"),{type:'bar',data:{labels:reasonLabels,datasets:[{label:'Event Count',data:reasonData,backgroundColor:CHART_COLOR, yAxisID:'y'}]},options:opts("Downtime Reason Distribution")});} else {let c=brc.getContext("2d"); c.clearRect(0,0,brc.width,brc.height); c.textAlign="center"; c.fillText("No reason data.", brc.width/2, brc.height/2);}}
}

function formatMinutesToHHMMSSCompare(val) { if (isNaN(val) || val <= 0.001) return "0:00:00"; let t = Math.round(60 * val), n = Math.floor(t / 3600), a = Math.floor(t % 3600 / 60); return t %= 60, `${n}:${a.toString().padStart(2,"0")}:${(t).toString().padStart(2,"0")}` }

function renderCompareTablePage() {
  let tableBody = document.getElementById("compareTable"); if (!tableBody) return;
  tableBody.innerHTML = "";
  let byAsset = {}; allAssetNamesCompare.forEach(e => { byAsset[e] = []; });
  for (let event of allEventsCompare) { byAsset[event[2]] && byAsset[event[2]].push(event); }
  
  for (let assetName of allAssetNamesCompare) {
    let assetEvents = byAsset[assetName] || [];
    let lastEvent = assetEvents.length ? assetEvents[assetEvents.length - 1] : null;
    let row = `<tr>
      <td>${assetName}</td>
      <td>${lastEvent && lastEvent[5] ? parseFloat(lastEvent[5]).toFixed(2) : "-"}</td>
      <td>${lastEvent && lastEvent[6] ? formatMinutesToHHMMSSCompare(parseFloat(lastEvent[6])) : "-"}</td>
      <td>${lastEvent && lastEvent[7] ? formatMinutesToHHMMSSCompare(parseFloat(lastEvent[7])) : "-"}</td>
      <td>${lastEvent && lastEvent[10] ? parseInt(lastEvent[10], 10) : "-"}</td>
      <td>${lastEvent && lastEvent[8] ? formatMinutesToHHMMSSCompare(parseFloat(lastEvent[8])) : "-"}</td>
      <td>${lastEvent && lastEvent[9] ? formatMinutesToHHMMSSCompare(parseFloat(lastEvent[9])) : "-"}</td>
    </tr>`;
    tableBody.innerHTML += row;
  }
}

document.readyState === "loading" ? document.addEventListener("DOMContentLoaded", fetchCompareDataPage) : fetchCompareDataPage();
)rawliteral";
  html += "</script>";
  html += "<script src='/assets/bootstrap.bundle.min.js'></script>";
  html += "</body></html>";
  return html;
}

// ------------------------------------------------------------------------------------------------------------------------------------
//  Events Page Function
// ------------------------------------------------------------------------------------------------------------------------------------
void sendHtmlEventsPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", ""); // Send headers

  // --- Start of HTML Document ---
  server.sendContent("<!DOCTYPE html><html lang='en'><head><title>Event Log</title>");
  server.sendContent("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  server.sendContent("<link href='/assets/bootstrap.min.css' rel='stylesheet'>");

  // --- ENHANCED CSS ---
  server.sendContent("<style>");
  server.sendContent("body{background-color:#f8f9fa;font-family:Roboto,Arial,sans-serif;}");
  server.sendContent(".table { width: 100%; min-width: 1200px; }");
  server.sendContent(".table th, .table td { padding-left: 18px; padding-right: 18px; padding-top: 14px; padding-bottom: 14px; }");
  server.sendContent(".stops-col, .stops-col td { text-align: center !important; }");
  server.sendContent(".table .note-col { max-width: 250px; white-space: normal; word-break: break-word; }");
  server.sendContent(".table .action-col { width: 1%; white-space: nowrap; text-align: center; }");

  // Row coloring classes
  server.sendContent(".table-row-stop { background-color: rgba(220, 53, 69, 0.08) !important; }"); // Faint red
  server.sendContent(".table-row-start { background-color: rgba(25, 135, 84, 0.10) !important; }"); // Faint green
  server.sendContent(".table-row-system { background-color: rgba(13, 202, 240, 0.11) !important; }"); // Faint cyan/blue
  server.sendContent(".evt-type { font-weight: bold; }");
  server.sendContent(".evt-type.START { color: #198754; }");
  server.sendContent(".evt-type.STOP { color: #dc3545; }");
  server.sendContent(".evt-type.SYS_START,.evt-type.SYS_STOP,.evt-type.SHIFT_TRANSITION { color: #0dcaf0; }");
  server.sendContent(".table th { position: sticky; top: 0; background: #0366d6; color: #fff; z-index: 2; }");
  server.sendContent(".table th[title] { cursor: help; border-bottom: 2px dotted #fff; }");
  server.sendContent(".note-col { font-size: 0.97em; color: #555; }");
  server.sendContent(".legend-dot{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:6px;vertical-align:middle;}");
  
  // Mobile card styling
  server.sendContent("@media (max-width:700px){ #eventTable{display:none;} .event-card{margin-bottom:0.8rem; border-left-width: 5px;} .event-card-stop{border-left-color: #dc3545;} .event-card-start{border-left-color:#198754;} .event-card-system{border-left-color:#0dcaf0;} }");
  server.sendContent("@media (min-width:701px){#mobileEvents{display:none;}}");
  server.sendContent("</style></head>");

  // --- Body, Navbar, and Page Structure ---
  server.sendContent("<body>");
  String navHtml = "<nav class='navbar navbar-expand-lg navbar-dark bg-dark shadow-sm'>"
                   "<div class='container-fluid'>"
                   "<a class='navbar-brand' href='/'>Asset Availability</a>"
                   "<button class='navbar-toggler' type='button' data-bs-toggle='collapse' data-bs-target='#navbarNav'><span class='navbar-toggler-icon'></span></button>"
                   "<div class='collapse navbar-collapse' id='navbarNav'>"
                   "<ul class='navbar-nav ms-auto'>"
                   "<li class='nav-item'><a class='nav-link' href='/'>Dashboard</a></li>"
                   "<li class='nav-item'><a class='nav-link active' href='/events'>Event Log</a></li>"
                   "<li class='nav-item'><a class='nav-link' href='/analytics-compare'>Compare Assets</a></li>"
                   "<li class='nav-item'><a class='nav-link' href='/config'>Setup</a></li>"
                   "</ul></div></div></nav>";
  server.sendContent(navHtml);

  server.sendContent("<div class='container-fluid mt-4'><div class='card shadow-sm'><div class='card-body'>");

  // --- Page title and legend for event types
  server.sendContent("<h1 class='mb-3'>Events Log</h1>");
  server.sendContent("<div class='mb-3' style='font-size:0.97em;'>"
    "<b>Legend:</b> "
    "<span class='legend-dot' style='background:#198754;'></span>Start "
    "<span class='legend-dot' style='background:#dc3545;'></span>Stop "
    "<span class='legend-dot' style='background:#0dcaf0;'></span>System/Shift Events "
    "<span class='ms-3'>Hover column headers for explanations.</span>"
    "</div>"
  );

  // --- Filters & controls row
String filterRowHtml =
  "<div class='row g-3 align-items-end mb-3'>"
    "<div class='col-md-3'><label for='channelFilter' class='form-label'>Filter by Asset:</label><select id='channelFilter' class='form-select'><option value='ALL' selected>All Assets</option></select></div>"
    "<div class='col-md-3'><label for='stateFilter' class='form-label'>Event State:</label><select id='stateFilter' class='form-select'><option value='ALL'>All</option><option value='RUNNING'>Up / Running</option><option value='STOPPED'>Down / Stopped</option></select></div>"
    "<div class='col-md-2'><button class='btn btn-secondary w-100' id='scrollBtn' type='button' onclick='toggleScrollInhibit(this)'>Pause Scroll</button></div>"
    "<div class='col-md-2'><a href='/export_log' class='btn btn-info w-100'>Export CSV</a></div>"
    "<div class='col-md-2'><a href='/shiftlogs_page' class='btn btn-warning w-100'>View Archive</a></div>"
    "<div class='col-md-2'><div id='eventCount' class='text-muted text-end pt-2'></div></div>"
  "</div>";
server.sendContent(filterRowHtml);

  // --- Table with improved headers and tooltips
  String tableHtml =
    "<div class='table-responsive' style='overflow-x:auto;'><table id='eventTable' class='table table-sm table-hover align-middle'>"
    "<thead><tr>"
    "<th title='Event date (DD/MM/YYYY)'>Date</th>"
    "<th title='Event time (HH:MM:SS)'>Time</th>"
    "<th title='Asset involved in the event'>Asset</th>"
    "<th title='Event type (START, STOP, SYSTEM, SHIFT)'>Type</th>"
    "<th title='Current machine state after event: Running (green), Stopped (red), or System Info'>State</th>"
    "<th title='Availability % (uptime/(uptime+downtime))'>Avail&nbsp;(%)</th>"
    "<th title='Total runtime this period (minutes)'>Runtime</th>"
    "<th title='Total downtime this period (minutes)'>Downtime</th>"
    "<th title='Mean Time Between Failures (minutes)'>MTBF</th>"
    "<th title='Mean Time To Repair (minutes)'>MTTR</th>"
    "<th class='stops-col' title='Number of stops'>Stops</th>"
    "<th title='Duration of this run (mm:ss)'>Run&nbsp;Dur</th>"
    "<th title='Duration of this stop (mm:ss)'>Stop&nbsp;Dur</th>"
    "<th class='note-col' title='User note or system message'>Note</th>"
    "<th class='action-col'></th>"
    "</tr></thead><tbody id='tbody'></tbody></table></div>"
    "<div id='mobileEvents'></div>";
  server.sendContent(tableHtml);
  server.sendContent("</div></div></div>");

  // --- Modal for editing notes
  String modalHtml =
    "<div class='modal fade' id='noteEditModal' tabindex='-1' aria-labelledby='noteModalLabel' aria-hidden='true'>"
    "<div class='modal-dialog'><div class='modal-content'>"
    "<div class='modal-header'><h5 class='modal-title' id='noteModalLabel'>Edit Event Note</h5><button type='button' class='btn-close' data-bs-dismiss='modal' aria-label='Close'></button></div>"
    "<form id='modalNoteForm' onsubmit='return submitModalNote(event)'><div class='modal-body'>"
    "<input type='hidden' id='modalNoteDate' name='date'><input type='hidden' id='modalNoteTime' name='time'><input type='hidden' id='modalNoteAsset' name='asset'>"
    "<div class='mb-3'><label for='modalNoteReason' class='form-label'>Reason:</label><select id='modalNoteReason' name='reason' class='form-select'></select></div>"
    "<div class='mb-3'><label for='modalNoteText' class='form-label'>Additional Note:</label><input type='text' id='modalNoteText' name='note' class='form-control' maxlength='64' placeholder='Add custom details...'></div>"
    "</div><div class='modal-footer'>"
    "<button type='button' class='btn btn-secondary' data-bs-dismiss='modal'>Cancel</button>"
    "<button type='submit' class='btn btn-primary'>Save Note</button>"
    "</div></form></div></div></div>";
  server.sendContent(modalHtml);

  // --- Bootstrap JS and icons
  server.sendContent("<script src='/assets/bootstrap.bundle.min.js'></script>");
  server.sendContent(R"rawliteral(<svg xmlns="http://www.w3.org/2000/svg" style="display: none;"><symbol id="icon-edit" viewBox="0 0 16 16"><path d="M12.854.146a.5.5 0 0 0-.707 0L10.5 1.793 14.207 5.5l1.647-1.646a.5.5 0 0 0 0-.708l-3-3zm.646 6.061L9.793 2.5 3.293 9H3.5a.5.5 0 0 1 .5.5v.5h.5a.5.5 0 0 1 .5.5v.5h.5a.5.5 0 0 1 .5.5v.5h.5a.5.5 0 0 1 .5.5v.207l6.5-6.5zm-7.468 7.468A.5.5 0 0 1 6 13.5V13h-.5a.5.5 0 0 1-.5-.5V12h-.5a.5.5 0 0 1-.5-.5V11h-.5a.5.5 0 0 1-.5-.5V10h-.5a.499.499 0 0 1-.175-.032l-.179.178a.5.5 0 0 0-.11.168l-2 5a.5.5 0 0 0 .65.65l5-2a.5.5 0 0 0 .168-.11l.178-.178z"/></symbol></svg>)rawliteral");

  // --- Main JS: all logic for page, filters, events, modal, rendering ---
  server.sendContent("<script>");
  server.sendContent(R"rawliteral(
// --- BEGIN initializeEventPage and helpers ---

let eventData = [], channelList = [], filterValue = "ALL", stateFilter = "ALL";
window.downtimeReasons = [];
let scrollInhibit = false, refreshIntervalId = null, noteModal = null;

function startAutoRefresh() {
  if (refreshIntervalId) clearInterval(refreshIntervalId);
  refreshIntervalId = setInterval(fetchAndRenderEvents, 5000);
}
function stopAutoRefresh() {
  if (refreshIntervalId) clearInterval(refreshIntervalId);
  refreshIntervalId = null;
}
function toggleScrollInhibit(btn) {
  scrollInhibit = !scrollInhibit;
  if(btn) btn.innerText = scrollInhibit ? "Resume Scroll" : "Pause Scroll";
}

function initializeEventPage() {
  noteModal = new bootstrap.Modal(document.getElementById('noteEditModal'));
  fetchChannelsAndStart();
}

function fetchChannelsAndStart() {
  fetch('/api/summary').then(r => { if (!r.ok) throw new Error(`API Summary Error: ${r.status}`); return r.json(); })
    .then(data => {
      channelList = (data && data.assets && Array.isArray(data.assets)) ? data.assets.map(a => a.name).filter(name => name && name.trim() !== "") : [];
      let sel = document.getElementById('channelFilter');
      if (!sel) return console.error("CRIT: 'channelFilter' not found.");
      sel.innerHTML = "<option value='ALL' selected>All Assets</option><option value='SYSTEM_EVENTS_ONLY'>SYSTEM Events Only</option>";
      channelList.forEach(c => { let o = document.createElement("option"); o.value = c; o.text = c; sel.appendChild(o); });
      sel.onchange = () => { filterValue = sel.value; renderTable(); };
      document.getElementById('stateFilter').onchange = () => { stateFilter = document.getElementById('stateFilter').value; renderTable(); };
      fetchReasonsAndEvents();
    }).catch(e => console.error("Error fetching channels/summary:", e));
}

function fetchReasonsAndEvents() {
  fetch('/api/config').then(r => { if (!r.ok) throw new Error(`API Config Error: ${r.status}`); return r.json(); })
    .then(cfg => { window.downtimeReasons = (cfg && cfg.downtimeReasons) || []; fetchAndRenderEvents(); startAutoRefresh(); })
    .catch(e => { console.error("Error fetching config reasons:", e); fetchAndRenderEvents(); startAutoRefresh(); });
}

function fetchAndRenderEvents() {
  // --- SIM PATCH: Added cache-busting parameter ---
  fetch('/api/events?_=' + new Date().getTime()).then(r => { if (!r.ok) throw new Error(`API Events Error: ${r.status}`); return r.json(); })
    .then(events => { eventData = Array.isArray(events) ? events : []; renderTable(); })
    .catch(e => { console.error("Error fetching events:", e); eventData = []; renderTable(); });
}

function populateModalReasons() {
  const reasonSelect = document.getElementById('modalNoteReason'); if (!reasonSelect) return;
  reasonSelect.innerHTML = '<option value=\"\"></option>'; 
  (window.downtimeReasons || []).forEach(r => { if (r && r.trim() !== "") { let opt = document.createElement('option'); opt.value = r; opt.text = r; reasonSelect.appendChild(opt); } });
  let otherOpt = document.createElement('option'); otherOpt.value = "Other"; otherOpt.text = "Other (Specify Below)";
  reasonSelect.appendChild(otherOpt);
}

function showNoteModal(date, time, asset, currentFullNote) {
  stopAutoRefresh(); populateModalReasons();
  document.getElementById('modalNoteDate').value = date; document.getElementById('modalNoteTime').value = time; document.getElementById('modalNoteAsset').value = asset;
  let currentReason = ""; let currentTextNote = currentFullNote || "";
  if (currentFullNote) {
    if ((window.downtimeReasons || []).includes(currentFullNote)) { currentReason = currentFullNote; currentTextNote = ""; }
    else { for (const reason of (window.downtimeReasons || [])) { if (reason && currentFullNote.startsWith(reason + " - ")) { currentReason = reason; currentTextNote = currentFullNote.substring(reason.length + 3); break; } } }
  }
  const reasonDropdown = document.getElementById('modalNoteReason'); if (reasonDropdown) reasonDropdown.value = currentReason;
  document.getElementById('modalNoteText').value = currentTextNote;
  noteModal.show();
}

document.getElementById('noteEditModal').addEventListener('hidden.bs.modal', event => { startAutoRefresh(); });

function submitModalNote(event) {
  event.preventDefault(); const form = document.getElementById('modalNoteForm'); const params = new URLSearchParams();
  params.append('date', form.date.value); params.append('time', form.time.value); params.append('asset', form.asset.value); params.append('reason', form.reason.value); params.append('note', form.note.value);
  fetch('/api/note', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: params.toString() }).then(r => { if (r.ok) { fetchAndRenderEvents(); } else { alert("Failed to save note. Status: " + r.status); } }).catch(err => { console.error("Error saving note:", err); alert("Error saving note."); });
  noteModal.hide(); return false;
}

function minToHHMMSS(valStr) { 
  let val = parseFloat(valStr); 
  if (isNaN(val) || val <= 0.001) 
  return "00:00:00"; let s_total = Math.round(val * 60); 
  let h=Math.floor(s_total/3600), m=Math.floor((s_total%3600)/60), s=s_total%60; 
  return `${h<10?"0":""}${h}:${m<10?"0":""}${m}:${s<10?"0":""}${s}`; 
}

function durationStrToHHMMSS(str) { 
  if (!str || typeof str !== "string" || str.trim()==="") 
  return ""; let p=str.split(":").map(Number); 
  let h=0,m=0,s=0; if(p.length===2){ m = p[0]; s = p[1]; if (m >= 60) { h = Math.floor(m/60); m %= 60; } } 
  else if(p.length===3){ [h,m,s]=p; } 
  else return ""; if(isNaN(h)||isNaN(m)||isNaN(s)) 
  return ""; 
  let hourString = (h > 0 ? `${h<10?"0":""}${h}:` : ""); 
  return `${hourString}${m<10?"0":""}${m}:${s<10?"0":""}${s}`; 
}

function normalizeAssetName(name) { 
  return (name || '').toLowerCase().replace(/\s+/g, ''); 
}

// --- END initializeEventPage and helpers ---

function renderTable() {
    let tbody = document.getElementById('tbody');
    let mobileDiv = document.getElementById('mobileEvents');
    if (!tbody || !mobileDiv) { console.error("Table/mobile div not found in renderTable"); return; }
    tbody.innerHTML = ''; mobileDiv.innerHTML = '';

    let displayedCount = 0;
    if (!eventData || !Array.isArray(eventData)) eventData = [];
    let stateMatcher = (sVal) => (stateFilter === "ALL") || (stateFilter === "RUNNING" && sVal === "1") || (stateFilter === "STOPPED" && sVal === "0");
    let isMobile = window.innerWidth <= 700;
    const eventsToRender = eventData.slice().reverse();

    for (let i = 0; i < eventsToRender.length; ++i) {
        let vals = eventsToRender[i];
        if (!Array.isArray(vals) || vals.length < 13) continue;

        let [ldate, ltime, lasset, levent, lstateVal, lavail, lrun, lstop, lmtbf, lmttr, lsc, runDurStr, stopDurStr] = vals;
        let lnote = vals.length > 13 ? vals.slice(13).join(',').replace(/\n$/, "").trim() : "";
        let isSystemEvent = (levent === "SYS_START" || levent === "SYS_STOP" || levent === "SHIFT_TRANSITION");

        if (filterValue === "SYSTEM_EVENTS_ONLY" && !isSystemEvent) continue;
        if (filterValue !== "ALL" && filterValue !== "SYSTEM_EVENTS_ONLY" && normalizeAssetName(lasset) !== normalizeAssetName(filterValue)) continue;
        if (!isSystemEvent && !stateMatcher(lstateVal)) continue;

        displayedCount++;
        let escapedNote = lnote.replace(/"/g, "&quot;").replace(/'/g, "\\'");

        // --- NEW COLORING LOGIC ---
        let rowClass = "";
        if (isSystemEvent) rowClass = "table-row-system";
        else if (lstateVal == "1") rowClass = "table-row-start";
        else rowClass = "table-row-stop";

        if (!isMobile) {
            let tr = tbody.insertRow();
            tr.className = rowClass;

            // Function to create and append a cell
            const addCell = (html) => {
                let cell = tr.insertCell();
                cell.innerHTML = html;
                return cell;
            };

            addCell(ldate); addCell(ltime); addCell(lasset);
            addCell(`<span class='evt-type ${levent}'>${levent}</span>`);
            let stateDisplay = lstateVal == "1"
                ? "<span class='badge bg-success'>RUNNING</span>"
                : "<span class='badge bg-danger'>STOPPED</span>";
            if (isSystemEvent)
                stateDisplay = levent === "SHIFT_TRANSITION"
                    ? "<span class='badge bg-primary'>INFO</span>"
                    : (lstateVal == "1"
                        ? "<span class='badge bg-info text-dark'>SYSTEM UP</span>"
                        : "<span class='badge bg-warning text-dark'>SYSTEM DOWN</span>");
            addCell(stateDisplay);

            addCell(Number(lavail).toFixed(2)); addCell(minToHHMMSS(lrun)); addCell(minToHHMMSS(lstop));
            addCell(minToHHMMSS(lmtbf)); addCell(minToHHMMSS(lmttr));
            let stopsCell = addCell(String(Math.round(Number(lsc))));
            stopsCell.className = 'stops-col';
            addCell(isSystemEvent ? "" : (levent.toUpperCase() === "STOP" ? durationStrToHHMMSS(runDurStr) : ""));
            addCell(isSystemEvent ? "" : (levent.toUpperCase() === "START" ? durationStrToHHMMSS(stopDurStr) : ""));

            let noteCell = addCell(lnote);
            noteCell.className = 'note-col';

            let actionCell = addCell('');
            actionCell.className = 'action-col';
            if (!isSystemEvent) {
                actionCell.innerHTML = `<a href=\"#\" class=\"text-secondary\" onclick='event.preventDefault(); showNoteModal(\"${ldate}\",\"${ltime}\",\"${lasset}\",\"${escapedNote}\")' title=\"Edit Note\"><svg class=\"bi\" width=\"16\" height=\"16\" fill=\"currentColor\"><use xlink:href=\"#icon-edit\"/></svg></a>`;
            }

        } else { // Mobile card view
            let card = document.createElement('div');
            let cardClass = "event-card shadow-sm card mb-3";
            if (isSystemEvent) cardClass += " event-card-system";
            else if (lstateVal == "1") cardClass += " event-card-start";
            else cardClass += " event-card-stop";
            card.className = cardClass;
            let cardBody = document.createElement('div'); cardBody.className = 'card-body p-3';
            let stateDisplayMob = lstateVal == "1" ? "<span class='badge bg-success'>RUNNING</span>" : "<span class='badge bg-danger'>STOPPED</span>";
            if (isSystemEvent) stateDisplayMob = levent === "SHIFT_TRANSITION" ? "<span class='badge bg-primary'>INFO</span>" : (lstateVal == "1" ? "<span class='badge bg-info text-dark'>SYSTEM UP</span>" : "<span class='badge bg-warning text-dark'>SYSTEM DOWN</span>");

            cardBody.innerHTML = `
              <div class="d-flex justify-content-between align-items-start">
                  <div>
                      <strong class="me-2">${lasset}</strong>${stateDisplayMob}
                      <small class='d-block text-muted'>${ltime} on ${ldate}</small>
                  </div>
                  ${!isSystemEvent ? `<a href=\"#\" class=\"text-secondary\" onclick='event.preventDefault(); showNoteModal(\"${ldate}\",\"${ltime}\",\"${lasset}\",\"${escapedNote}\")' title=\"Edit Note\"><svg class=\"bi\" width=\"20\" height=\"20\" fill=\"currentColor\"><use xlink:href=\"#icon-edit\"/></svg></a>` : ''}
              </div>
              <p class="card-text mt-2 mb-0"><strong>Event:</strong> <span class="evt-type ${levent}">${levent}</span></p>
              <p class="card-text mt-1"><strong>Note:</strong> <span class="text-muted">${lnote || 'N/A'}</span></p>`;
            card.appendChild(cardBody);
            mobileDiv.appendChild(card);
        }
    }
    const ec = document.getElementById('eventCount');
    if (ec) ec.innerHTML = `Showing <b>${displayedCount}</b> event(s)`;
    if (!scrollInhibit && displayedCount > 0 && !isMobile) {
        window.scrollTo({ top: 0, behavior: 'auto' });
    }
    // scrollInhibit is now honored for Pause/Resume
}

if (document.readyState === 'loading') { document.addEventListener('DOMContentLoaded', initializeEventPage); }
else { initializeEventPage(); }

)rawliteral");
  server.sendContent("</script>");
  server.sendContent("</body></html>");
  server.sendContent("");
}

// Handler Functions)

void handleWiFiReconfigurePost() {
  // Clear WiFiManager stored credentials using proper WiFiManager method
  WiFiManager wm;
  wm.resetSettings();
  
  // Also clear any locally stored credentials in preferences for completeness
  prefs.begin("assetmon", false); 
  prefs.remove("ssid"); 
  prefs.remove("pass"); 
  prefs.end();
  
  Serial.println("WiFi credentials cleared from both WiFiManager and preferences. Restarting in AP mode.");
  String message = "<!DOCTYPE html><html><head><title>WiFi Reconfiguration</title><style>body{font-family:Arial,sans-serif;margin:20px;padding:15px;border:1px solid #ddd;border-radius:5px;text-align:center;} h2{color:green;}</style></head><body><h2>WiFi Credentials Cleared</h2><p>Device will now restart in Access Point mode ('AssetMonitor-Config'). Connect to this AP to set up new WiFi.</p><p>This page will attempt to redirect in 5 seconds, or you can manually go to the device's new IP (usually 192.168.4.1) once connected to the AP.</p><meta http-equiv='refresh' content='7;url=http://192.168.4.1/' /></body></html>";
  server.sendHeader("Connection", "close"); 
  server.send(200, "text/html", message);
  delay(1500);
  ESP.restart();
}

void handleConfigPost() {
  bool rebootNeeded = false; 

  // --- Asset Config ---
  if (server.hasArg("assetCount")) {
    uint8_t oldAssetCount = config.assetCount;
    // Use the ID "assetCountField" if that's what you used in htmlConfig, or "assetCount" if that's the name attribute
    config.assetCount = constrain(server.arg("assetCount").toInt(), 1, MAX_ASSETS); 
    if (config.assetCount != oldAssetCount) rebootNeeded = true;
    for (uint8_t i = 0; i < MAX_ASSETS; ++i) {
      if (i < config.assetCount) {
        if (server.hasArg("name"+String(i))) { 
            String newName = server.arg("name"+String(i));
            if(strcmp(config.assets[i].name, newName.c_str()) != 0) rebootNeeded = true;
            strncpy(config.assets[i].name, newName.c_str(), 31); config.assets[i].name[31] = '\0'; 
        }
        if (server.hasArg("pin"+String(i))) { 
            uint8_t newPin = server.arg("pin"+String(i)).toInt();
            if(config.assets[i].pin != newPin) rebootNeeded = true;
            config.assets[i].pin = newPin;
        }
      } else {
        if(strlen(config.assets[i].name) > 0) rebootNeeded = true;
        config.assets[i].name[0] = '\0'; config.assets[i].pin = 0;
      }
    }
  } else {
     server.send(400, "text/plain", "Bad Request: assetCount missing."); return;
  }

  // --- Operational Settings ---
  if (server.hasArg("maxEvents")) {
      uint16_t newVal = constrain(server.arg("maxEvents").toInt(), 100, 5000);
      if(config.maxEvents != newVal) rebootNeeded = true;
      config.maxEvents = newVal;
  }
  if (server.hasArg("tzOffset")) {
      int newVal = static_cast<int>(server.arg("tzOffset").toFloat() * 3600);
      if(config.tzOffset != newVal) rebootNeeded = true;
      config.tzOffset = newVal;
  }
  if (server.hasArg("longStopThreshold")) {
      int newVal = constrain(server.arg("longStopThreshold").toInt() * 60, 60, 1440 * 60);
      // if(config.longStopThresholdSec != newVal) rebootNeeded = true; // This alone might not need a reboot
      config.longStopThresholdSec = newVal;
  }
   if (server.hasArg("monitoringMode")) {
    int newMode = server.arg("monitoringMode").toInt();
    if (newMode != config.monitoringMode && (newMode == MONITORING_MODE_PARALLEL || newMode == MONITORING_MODE_SERIAL)) {
      if(config.monitoringMode != newMode) rebootNeeded = true;
      config.monitoringMode = newMode;
      g_systemStateInitialized = false; // Force re-evaluation of system state
    }
  }
  for (int i=0; i<5; ++i) {
    if(server.hasArg("reason"+String(i))) { 
        strncpy(config.downtimeReasons[i], server.arg("reason"+String(i)).c_str(), 31); 
        config.downtimeReasons[i][31] = '\0'; 
    }
  }

  // --- Parse Shift Configuration ---
  bool oldEnableShiftArchiving = config.enableShiftArchiving;
  config.enableShiftArchiving = server.hasArg("enableShiftArchiving");
  if (oldEnableShiftArchiving != config.enableShiftArchiving) rebootNeeded = true;

  if (config.enableShiftArchiving) {
    uint8_t oldNumShifts = config.numShifts;
    if (server.hasArg("numShifts")) {
      config.numShifts = constrain(server.arg("numShifts").toInt(), 1, MAX_CONFIGURABLE_SHIFTS);
    } else {
      config.numShifts = (oldNumShifts > 0 && oldNumShifts <=MAX_CONFIGURABLE_SHIFTS) ? oldNumShifts : 1; // Keep old if valid, else 1
    }
    if (oldNumShifts != config.numShifts) rebootNeeded = true;

    for (uint8_t i = 0; i < config.numShifts; ++i) {
      if (i < MAX_CONFIGURABLE_SHIFTS) {
        String argName = "shiftStartTime" + String(i);
        if (server.hasArg(argName)) {
          String newTime = server.arg(argName);
          if (newTime.length() == 5 && newTime.charAt(2) == ':') { // Basic HH:MM validation
            if (strcmp(config.shifts[i].startTime, newTime.c_str()) != 0) rebootNeeded = true;
            strncpy(config.shifts[i].startTime, newTime.c_str(), 5);
            config.shifts[i].startTime[5] = '\0';
          } else { // Invalid time format from form
            Serial.printf("Invalid time format received for %s: '%s'. Keeping old or default '00:00'.\n", argName.c_str(), newTime.c_str());
            if (strlen(config.shifts[i].startTime) == 0) strcpy(config.shifts[i].startTime,"00:00");
          }
        } else { // Shift time arg not found, but expected based on numShifts
           Serial.printf("Shift time arg %s not found for numShifts=%d. Ensuring default '00:00'.\n", argName.c_str(), config.numShifts);
           if (strlen(config.shifts[i].startTime) == 0) strcpy(config.shifts[i].startTime,"00:00");
        }
      }
    }
    for (uint8_t i = config.numShifts; i < MAX_CONFIGURABLE_SHIFTS; ++i) { // Clear unused shift slots
      if(strlen(config.shifts[i].startTime) > 0 && strcmp(config.shifts[i].startTime, "00:00") != 0) rebootNeeded = true;
      strcpy(config.shifts[i].startTime, "00:00");
    }
  } else { // Shift archiving disabled
    if (config.numShifts != 0) rebootNeeded = true; // If it was enabled and now disabled
    config.numShifts = 0;
    for (uint8_t i = 0; i < MAX_CONFIGURABLE_SHIFTS; ++i) {
      strcpy(config.shifts[i].startTime, "00:00");
    }
  }
  // --- End Parse Shift Configuration ---

  saveConfig();
  
  // Send a proper response with user feedback before rebooting
  String message = "<!DOCTYPE html><html><head><title>Settings Saved</title><style>body{font-family:Arial,sans-serif;margin:20px;padding:15px;border:1px solid #ddd;border-radius:5px;text-align:center;} h2{color:green;}</style></head><body><h2>Settings Saved Successfully!</h2><p>Device is rebooting now to apply all settings...</p><p>You will be redirected to the setup page in 8 seconds.</p><meta http-equiv='refresh' content='8;url=/config' /></body></html>";
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", message);
  delay(2000); // Give client time to receive response
  ESP.restart(); // Reboot to apply all settings cleanly
}

void handleClearLog() { // Patched version
  SPIFFS.remove(LOG_FILENAME);
  Serial.println("Log cleared.");

  time_t now = time(nullptr);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i < MAX_ASSETS) {
      assetStates[i].lastChangeTime = now;
      assetStates[i].sessionStart = now;
      assetStates[i].runningTime = 0; assetStates[i].stoppedTime = 0;
      assetStates[i].runCount = 0; assetStates[i].stopCount = 0;
      assetStates[i].lastEventTime = now;
      assetStates[i].lastRunDuration = 0; assetStates[i].lastStopDuration = 0;
      if (config.assets[i].pin > 0 && config.assets[i].pin < 40) {
          assetStates[i].lastState = digitalRead(config.assets[i].pin);
      } else {
          assetStates[i].lastState = true;
      }
    }
  }

  if (config.monitoringMode == MONITORING_MODE_SERIAL) {
    g_systemTotalRunningTimeSecs = 0;
    g_systemTotalStoppedTimeSecs = 0;
    g_systemStopCount = 0;
    g_systemLastStateChangeTime = now;
    g_systemStateInitialized = false; // Will force re-evaluation

    bool anyAssetCurrentlyStopped = false;
    g_serialSystemTriggerAssetName[0] = '\0';
    for (uint8_t k = 0; k < config.assetCount; ++k) {
      if (k < MAX_ASSETS) {
          bool assetEffectivelyStopped = assetStates[k].lastState;
          if (config.assets[k].pin == 0 || config.assets[k].pin >= 40) assetEffectivelyStopped = true;
          if (assetEffectivelyStopped) {
            anyAssetCurrentlyStopped = true;
            if (g_serialSystemTriggerAssetName[0] == '\0') {
                strncpy(g_serialSystemTriggerAssetName, config.assets[k].name, 31);
                g_serialSystemTriggerAssetName[31] = '\0';
            }
          }
      }
    }
    g_isSystemSerialDown = anyAssetCurrentlyStopped;
    g_systemStateInitialized = true;
    Serial.printf("Log cleared. Serial Mode: System state re-evaluated. Down: %s. Root Cause: %s\n",
                  g_isSystemSerialDown ? "Yes" : "No",
                  g_isSystemSerialDown ? g_serialSystemTriggerAssetName : "N/A");
    if (g_isSystemSerialDown) {
        logSystemEvent(false, now, g_serialSystemTriggerAssetName);
    } else {
        logSystemEvent(true, now, "System State After Log Clear - All Assets Up");
    }
  }
  
  // Send proper feedback to user before redirecting
  String message = "<!DOCTYPE html><html><head><title>Log Cleared</title><style>body{font-family:Arial,sans-serif;margin:20px;padding:15px;border:1px solid #ddd;border-radius:5px;text-align:center;} h2{color:green;}</style></head><body><h2>Event Log Cleared Successfully!</h2><p>All event history has been cleared and statistics have been reset.</p><p>Redirecting to setup page in 3 seconds...</p><meta http-equiv='refresh' content='3;url=/config' /></body></html>";
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", message);
}

void handleShiftLogsPage() {
  String html = "<!DOCTYPE html><html><head><title>Shift Logs</title>"
                "<meta charset='UTF-8'>"
                "<style>"
                "body { font-family: Arial; margin:2em; background:#f6f8fa; }"
                "h1 { color: #0366d6; }"
                ".loglist { background:#fff; padding:1.5em; border-radius:8px; box-shadow:0 0 8px #ccc; max-width:700px; margin:auto; }"
                "table { width:100%; border-collapse:collapse; }"
                "th, td { padding:0.4em 1em; border-bottom:1px solid #eee; }"
                "a.button, button.button { padding:0.25em 0.75em; background:#0366d6; color:#fff; border-radius:4px; text-decoration:none; border:none; cursor:pointer; }"
                ".delete-row { text-align:center; }"
                "</style></head><body>";
  html += "<div class='loglist'><h1>Archived Shift Logs</h1>";

  // Form for deletion
  html += "<form method='POST' action='/delete_logs' id='deleteLogsForm'>";
  html += "<table><tr><th>Select</th><th>Filename</th><th>Size (bytes)</th><th>Download</th></tr>";

  File root = SPIFFS.open("/");
  bool any = false;
  File f = root.openNextFile();
  while (f) {
    String name = f.name();
    if (!f.isDirectory() && name.indexOf("Log-") == 0) {
      any = true;
      String displayName = name.startsWith("/") ? name.substring(1) : name;
      html += "<tr>";
      html += "<td class='delete-row'><input type='checkbox' name='logfile' value='" + displayName + "'></td>";
      html += "<td>" + displayName + "</td>";
      html += "<td>" + String(f.size()) + "</td>";
      html += "<td><a class='button' href='/download_shiftlog?file=" + urlEncode("/"+displayName) + "'>Download</a></td>";
      html += "</tr>";
    }
    f = root.openNextFile();
  }
  html += "</table>";

  if (!any) html += "<p>No shift log files archived yet.</p>";

  // Buttons for manual cleaning
  html += "<div style='margin-top:1em;'>";
  html += "<button type='submit' class='button' name='action' value='delete_checked' onclick='return confirm(\"Delete selected logs?\")'>Delete Checked Logs</button>&nbsp;";
  html += "<button type='submit' class='button' name='action' value='delete_all' onclick='return confirm(\"Delete ALL logs? This cannot be undone.\")'>Delete All Logs</button>";
  html += "</div>";

  html += "</form>";
  html += "<br><a href='/config' class='button'>Back to Config</a></div></body></html>";
  server.send(200, "text/html", html);
}

void handleDeleteLogs() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  String action = server.arg("action");
  if (action == "delete_all") {
    File root = SPIFFS.open("/");
    File f = root.openNextFile();
    while (f) {
      String name = f.name();
      if (!f.isDirectory() && name.indexOf("Log-") == 0) {
        SPIFFS.remove(name);
      }
      f = root.openNextFile();
    }
    server.sendHeader("Location", "/shiftlogs_page");
    server.send(303);
  } else if (action == "delete_checked") {
    for (uint8_t i = 0; i < server.args(); i++) {
      if (server.argName(i) == "logfile") {
        String toDelete = "/" + server.arg(i); // Always prefix with slash for SPIFFS
        SPIFFS.remove(toDelete);
      }
    }
    server.sendHeader("Location", "/shiftlogs_page");
    server.send(303);
  } else {
    server.send(400, "text/plain", "Unknown action");
  }
}

void handleDownloadShiftLog() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Missing file parameter");
    return;
  }
  String filename = urlDecode(server.arg("file"));
  // Changed: Only allow download of root-level log files starting with "/Log-"
  if (!filename.startsWith("/Log-")) {
    server.send(403, "text/plain", "Invalid file path");
    return;
  }
  File f = SPIFFS.open(filename, FILE_READ);
  if (!f) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  // Remove the leading '/' for the download name
  String downloadName = filename.substring(1);
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + downloadName + "\"");
  server.streamFile(f, "text/csv");
  f.close();
}

void handleExportLog() { // From your V21
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f || f.size() == 0) { server.send(404, "text/plain", "Log empty or not found."); if(f)f.close(); return; }
  time_t now_val = time(nullptr); struct tm *t_val = localtime(&now_val); char fn[64]; strftime(fn, sizeof(fn), "AssetLog-%Y%m%d-%H%M.csv", t_val);
  server.sendHeader("Content-Type", "text/csv"); server.sendHeader("Content-Disposition", String("attachment; filename=\"")+fn+"\"");
  server.setContentLength(f.size() + strlen("Date,Time,Asset,Event,State,Avail(%),Runtime(min),Downtime(min),MTBF(min),MTTR(min),Stops,RunDur,StopDur,Note\n")); // Approximate for progress bars
  server.send(200, "text/csv", "");
  server.sendContent("Date,Time,Asset,Event,State,Avail(%),Runtime(min),Downtime(min),MTBF(min),MTTR(min),Stops,RunDur,StopDur,Note\n");
  char buf[1025]; size_t br;
  while(f.available()){
    br=f.readBytes(buf,1024);
    if(br>0){ server.sendContent(buf, br); } // Send exact bytes read
  }
  f.close();
  // server.sendContent(""); // Final empty chunk to signify end if not using chunked encoding explicitly
  Serial.println("Log exported.");
}

void handleApiSummary() {
  String json = "{";
  time_t now = time(nullptr);

  json += "\"monitoringMode\":" + String(config.monitoringMode) + ",";

  if (config.monitoringMode == MONITORING_MODE_SERIAL) {
    json += "\"systemStats\":{";
    
    bool isSystemDownNow = g_isSystemSerialDown;
    String triggerAssetNow = String(g_serialSystemTriggerAssetName);
    
    json += "\"isDown\":" + String(isSystemDownNow ? "true" : "false") + ",";
    json += "\"triggerAsset\":\"" + triggerAssetNow + "\",";

    unsigned long currentSystemRunTime = g_systemTotalRunningTimeSecs;
    unsigned long currentSystemStopTime = g_systemTotalStoppedTimeSecs;
    if (g_systemStateInitialized && g_systemLastStateChangeTime > 0) {
        unsigned long durationSinceLastChange = now - g_systemLastStateChangeTime;
        if (!g_isSystemSerialDown) { currentSystemRunTime += durationSinceLastChange; }
        else { currentSystemStopTime += durationSinceLastChange; }
    }

    float systemAvailability = (currentSystemRunTime + currentSystemStopTime) > 0
                                ? (100.0f * currentSystemRunTime / (currentSystemRunTime + currentSystemStopTime))
                                : (isSystemDownNow ? 0.0f : 100.0f);
    json += "\"availability\":" + String(systemAvailability, 2) + ",";
    json += "\"totalRunningTimeSecs\":" + String(currentSystemRunTime) + ",";
    json += "\"totalStoppedTimeSecs\":" + String(currentSystemStopTime) + ",";
    json += "\"stopCount\":" + String(g_systemStopCount) + ",";

    float systemMtbf_min = (g_systemStopCount > 0) ? ((float)currentSystemRunTime / 60.0f / g_systemStopCount) : (currentSystemRunTime > 0 ? (float)currentSystemRunTime / 60.0f : 0.0f) ;
    float systemMttr_min = (g_systemStopCount > 0) ? ((float)currentSystemStopTime / 60.0f / g_systemStopCount) : 0.0f;
    json += "\"mtbf_min\":" + String(systemMtbf_min, 2) + ",";
    json += "\"mttr_min\":" + String(systemMttr_min, 2);
    json += "},";
  }

  json += "\"assets\":[";
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) continue;
    if (i > 0) json += ",";
    
    AssetState& as = assetStates[i];
    
    bool assetIsCurrentlyRunning;
    bool pin_state = true; // Assume stopped if pin invalid
    if (config.assets[i].pin > 0 && config.assets[i].pin < 40) {
        pin_state = digitalRead(config.assets[i].pin);
    }
    assetIsCurrentlyRunning = !pin_state; // In INPUT_PULLUP, LOW (false) means running

    unsigned long runT_current = as.runningTime;
    unsigned long stopT_current = as.stoppedTime;
    if (as.lastChangeTime > 0) {
      unsigned long timeSinceLastChange = now - as.lastChangeTime;
      if (as.lastState == false) { // Was RUNNING
          runT_current += timeSinceLastChange;
      } else { // Was STOPPED
          stopT_current += timeSinceLastChange;
      }
    }

    float avail = (runT_current + stopT_current) > 0 ? (100.0 * runT_current / (runT_current + stopT_current)) : (assetIsCurrentlyRunning ? 100.0 : 0.0);
    float rt_m = runT_current / 60.0;
    float st_m = stopT_current / 60.0;
    float mtbf_calc = (as.stopCount > 0) ? rt_m / as.stopCount : (runT_current > 0 ? rt_m : 0.0);
    float mttr_calc = (as.stopCount > 0) ? st_m / as.stopCount : 0.0;

    json += "{";
    json += "\"name\":\"" + String(config.assets[i].name) + "\",";
    json += "\"pin\":" + String(config.assets[i].pin) + ",";
    json += "\"state\":" + String(assetIsCurrentlyRunning ? 1 : 0) + ","; // Use the determined state
    json += "\"availability\":" + String(avail, 2) + ",";
    json += "\"total_runtime\":" + String(rt_m, 2) + ",";
    json += "\"total_downtime\":" + String(st_m, 2) + ",";
    json += "\"mtbf\":" + String(mtbf_calc, 2) + ",";
    json += "\"mttr\":" + String(mttr_calc, 2) + ",";
    json += "\"stop_count\":" + String(as.stopCount);
    json += ",\"lastRunDurationSecs\":" + String(as.lastRunDuration);
    json += ",\"lastStopDurationSecs\":" + String(as.lastStopDuration);
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// Replace your existing handleApiEvents with this one:
void handleApiEvents() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  String jsonOutput = "[";
  bool firstLine = true;

  if (f && f.size() > 0) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() < 5) continue; // Skip empty or too short lines

      if (!firstLine) {
        jsonOutput += ",";
      }
      firstLine = false;

      jsonOutput += "[";
      int start = 0;
      int commaPos = 0;
      bool firstField = true;
      // Split the CSV line into fields and make each a JSON string
      while(start < line.length()) {
          commaPos = line.indexOf(',', start);
          if (commaPos == -1) { // Last field
              commaPos = line.length();
          }
          String field = line.substring(start, commaPos);
          // Escape quotes and backslashes within the field for JSON string validity
          String escapedField = "";
          for (unsigned int k=0; k < field.length(); k++) {
              char c = field.charAt(k);
              if (c == '"') escapedField += "\\\"";
              else if (c == '\\') escapedField += "\\\\";
              else escapedField += c;
          }

          if (!firstField) {
              jsonOutput += ",";
          }
          jsonOutput += "\"" + escapedField + "\"";
          firstField = false;
          start = commaPos + 1;
          if (start >= line.length() && commaPos == line.length()) break; // handles trailing empty field if line ends with comma
      }
      jsonOutput += "]";
    }
    f.close();
  }
  jsonOutput += "]";

  server.sendHeader("Cache-Control", "no-cache,no-store,must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "application/json", jsonOutput);
}

void handleApiConfig() { // From your V21
  String json="{"; json+="\"assetCount\":"+String(config.assetCount)+","; json+="\"maxEvents\":"+String(config.maxEvents)+","; json+="\"tzOffset\":"+String(config.tzOffset)+","; json+="\"assets\":[";
  for(uint8_t i=0;i<config.assetCount;++i){if(i>=MAX_ASSETS)continue;if(i>0)json+=",";json+="{";json+="\"name\":\""+String(config.assets[i].name)+"\",";json+="\"pin\":"+String(config.assets[i].pin)+"}";}
  json+="],\"downtimeReasons\":["; for(int i=0;i<5;++i){if(i>0)json+=",";json+="\""+String(config.downtimeReasons[i])+"\"";} json+="]";
  json+=",\"longStopThresholdSec\":"+String(config.longStopThresholdSec);
  json+=",\"monitoringMode\":"+String(config.monitoringMode); // Added monitoring mode
  json+="}"; server.send(200,"application/json",json);
}

void handleApiNote() { // From your V21
  if(server.method()==HTTP_POST&&server.hasArg("date")&&server.hasArg("time")&&server.hasArg("asset")){
    String d=server.arg("date"),t=server.arg("time"),a=server.arg("asset"),n=server.arg("note"),r=server.hasArg("reason")?server.arg("reason"):"";
    Serial.printf("API Note: D=%s,T=%s,A=%s,R=%s,N=%s\n",d.c_str(),t.c_str(),a.c_str(),r.c_str(),n.c_str());
    updateEventNote(d,t,a,n,r); server.sendHeader("Location","/events"); server.send(303); return;
  } server.send(400,"text/plain","Bad Request: Note params missing");
}

void updateEventNote(String date_str, String time_str, String assetName_str, String note_text_str, String reason_str) { // From your V21
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ); if (!f) { Serial.println("updateEventNote: Fail read log."); return; }
  String tempLog = ""; bool updated = false;
  String combinedNote = "";
  // Ensure reason_str and note_text_str are properly handled if one is empty
  if (reason_str.length()>0 && reason_str != "Other") { // "Other" is just a selector, not part of the note
    combinedNote = reason_str;
    if (note_text_str.length()>0) {
        combinedNote += " - " + note_text_str;
    }
  } else {
    combinedNote = note_text_str;
  }
  combinedNote.replace(",", ";"); // Replace commas with semicolons to avoid breaking CSV
  combinedNote.replace("\n", " "); combinedNote.replace("\r", " ");

  String line;
  while (f.available()) {
    line = f.readStringUntil('\n');
    String trimmedLine = line; trimmedLine.trim();
    if (trimmedLine.length() < 5) { tempLog += line; if(f.available() || line.endsWith("\n")) tempLog+=""; else tempLog+="\n"; continue; }

    String parts[3]; int pIdx = 0; int lastC = -1;
    for(int k=0; k<3; ++k) {
        int nextC = trimmedLine.indexOf(',', lastC + 1);
        if (nextC == -1) { parts[pIdx++] = trimmedLine.substring(lastC + 1); break; }
        parts[pIdx++] = trimmedLine.substring(lastC + 1, nextC); lastC = nextC;
    }
    for(int k=pIdx; k<3; ++k) parts[k] = "";

    if (parts[0] == date_str && parts[1] == time_str && parts[2] == assetName_str) {
      int finalComma = -1; int commaCount = 0;
      for(int i=0; i<trimmedLine.length(); ++i){ if(trimmedLine.charAt(i)==','){commaCount++; if(commaCount==13){finalComma=i;break;}}}
      if (finalComma != -1) tempLog += trimmedLine.substring(0, finalComma + 1) + combinedNote + "\n";
      else tempLog += trimmedLine + "," + combinedNote + "\n"; // Should not happen if 13 commas is correct
      updated = true; Serial.println("Updated log line for: " + date_str + " " + time_str + " " + assetName_str);
    } else {
      tempLog += line;
      if (!line.endsWith("\n") && f.available()) tempLog += "\n";
    }
  }
  f.close();

  if (updated) {
    File f2 = SPIFFS.open(LOG_FILENAME, FILE_WRITE);
    if (!f2) { Serial.println("updateEventNote: Fail write log."); return; }
    f2.print(tempLog); f2.close(); Serial.println("Log rewritten with updated note.");
  } else Serial.println("Event for note update not found: " + date_str + " " + time_str + " " + assetName_str);
}

void handleNotFound() { 
  // Enhanced not found handler - in AP mode, redirect to dashboard for better UX
  if (currentWiFiState == WIFI_AP_MODE) {
    // In AP mode, redirect unknown requests to the main dashboard
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "Redirecting to dashboard");
  } else {
    server.send(404, "text/plain", "Not found"); 
  }
}

// End of file




