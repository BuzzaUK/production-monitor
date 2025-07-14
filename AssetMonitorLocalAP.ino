#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>
#include <ctype.h>
// NOTE: WiFiManager removed - using custom WiFi logic

// --- OLED Display Includes and Defines ---
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// --- END OLED DEFINES ---

// --- OLED Display State ---
String lastSsidDisplayed = "";
String lastIpDisplayed = "";
unsigned long lastOledUpdate = 0;

// --- Function to update OLED with Network info ---
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

// --- Function to show startup status on OLED ---
void updateOledStartupStatus(const String& status) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Production Monitor");
  display.println("Starting...");
  display.println();
  display.println(status);
  display.display();
}
// --- END OLED FUNCTIONS ---

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
  char reason[32];
};

// --- Monitoring modes ---
#define MONITORING_MODE_PARALLEL 0
#define MONITORING_MODE_SERIAL 1

// --- File System Constants ---
const char* LOG_FILENAME = "/events.csv";

// --- Function Declarations ---
String wifiConfigHTML();
String htmlDashboard();
String htmlConfig();
String htmlAssetDetail(uint8_t idx);
String htmlAnalytics();
String htmlAnalyticsCompare();
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
void handleWifiConfigPost();
void handleRunLocalAP();
void startConfigPortal();
void startLocalAPMode();
void setupCustomWiFi();
void setupTime();
void handleShiftLogsPage();
void handleDownloadShiftLog();
String urlEncode(const String& str);
String urlDecode(const String& str);
String eventToCSV(const Event& e); // Moved Event struct definition before this
String formatMMSS(unsigned long seconds);
void loadConfig();
void saveConfig();
bool checkInternetConnectivity();
String getLocalResources();
void handleManualTimeSet();
String getManualTimeSetHTML();
void sendHtmlEventsPage();

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

// --- WiFi Configuration HTML with improved styling and "Run Locally" button ---
String wifiConfigHTML() {
  String html = "<!DOCTYPE html><html><head><title>WiFi Setup - Production Monitor</title>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>"
                "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); margin: 0; padding: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center; }"
                ".container { background: #fff; border-radius: 12px; box-shadow: 0 15px 35px rgba(0,0,0,0.1); padding: 2.5rem; max-width: 450px; width: 90%; margin: 20px; }"
                "h1 { text-align: center; color: #333; margin-bottom: 1.5rem; font-size: 1.8rem; font-weight: 600; }"
                ".form-group { margin-bottom: 1.5rem; }"
                "label { display: block; margin-bottom: 0.5rem; color: #555; font-weight: 500; font-size: 0.95rem; }"
                "input[type='text'], input[type='password'] { width: 100%; padding: 0.8rem; border: 2px solid #e1e5e9; border-radius: 8px; font-size: 1rem; transition: border-color 0.3s ease, box-shadow 0.3s ease; box-sizing: border-box; }"
                "input[type='text']:focus, input[type='password']:focus { outline: none; border-color: #667eea; box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1); }"
                "input[type='submit'] { width: 100%; padding: 0.9rem; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: #fff; border: none; border-radius: 8px; font-size: 1rem; font-weight: 600; cursor: pointer; transition: transform 0.2s ease, box-shadow 0.3s ease; }"
                "input[type='submit']:hover { transform: translateY(-2px); box-shadow: 0 8px 25px rgba(102, 126, 234, 0.3); }"
                ".localap-btn { width: 100%; background: linear-gradient(135deg, #f39c12 0%, #e67e22 100%); color: #fff; border: none; padding: 0.9rem; border-radius: 8px; margin-top: 1rem; cursor: pointer; font-size: 1rem; font-weight: 600; transition: transform 0.2s ease, box-shadow 0.3s ease; }"
                ".localap-btn:hover { transform: translateY(-2px); box-shadow: 0 8px 25px rgba(243, 156, 18, 0.3); }"
                ".note { background: #f8f9fa; border-left: 4px solid #667eea; padding: 1rem; border-radius: 4px; margin: 1.5rem 0; color: #666; font-size: 0.9rem; line-height: 1.5; }"
                ".divider { text-align: center; margin: 2rem 0; position: relative; color: #999; font-size: 0.9rem; }"
                ".divider::before { content: ''; position: absolute; top: 50%; left: 0; right: 0; height: 1px; background: #e1e5e9; z-index: 1; }"
                ".divider span { background: #fff; padding: 0 1rem; position: relative; z-index: 2; }"
                ".status-info { background: #e3f2fd; border: 1px solid #90caf9; border-radius: 8px; padding: 1rem; margin-bottom: 1.5rem; color: #0d47a1; font-size: 0.9rem; }"
                ".device-info { text-align: center; margin-bottom: 1.5rem; }"
                ".device-info h2 { color: #667eea; font-size: 1.3rem; margin-bottom: 0.5rem; }"
                ".device-info p { color: #666; font-size: 0.9rem; margin: 0; }"
                "@media (max-width: 480px) { .container { padding: 1.5rem; margin: 10px; } h1 { font-size: 1.5rem; } }"
                "</style>"
                "</head><body>"
                "<div class='container'>"
                "<div class='device-info'>"
                "<h2>Production Monitor</h2>"
                "<p>Asset Monitoring System Configuration</p>"
                "</div>"
                "<div class='status-info'>"
                "📡 Device is in Access Point mode. Configure WiFi to connect to your network or run locally."
                "</div>"
                "<form method='POST' action='/wifi_save_config'>"
                "<h1>WiFi Network Setup</h1>"
                "<div class='form-group'>"
                "<label>Network Name (SSID):</label>"
                "<input type='text' name='ssid' maxlength='32' required placeholder='Enter WiFi network name' value='";
  html += wifi_ssid;
  html += "'></div>"
          "<div class='form-group'>"
          "<label>Network Password:</label>"
          "<input type='password' name='password' maxlength='64' placeholder='Enter WiFi password' value='";
  html += wifi_pass;
  html += "'></div>"
          "<div class='note'>⚠️ Device will automatically reboot and connect to the specified network after saving. The device IP may change.</div>"
          "<input type='submit' value='Save WiFi Settings & Connect'>"
          "</form>"
          "<div class='divider'><span>OR</span></div>"
          "<form method='POST' action='/run_local_ap'>"
          "<input type='submit' class='localap-btn' value='🏠 Run Locally (Access Point Mode)'>"
          "</form>"
          "<div class='note'>💡 Local mode allows you to use the monitoring system without internet connectivity. Connect to this device's WiFi network and access the dashboard directly.</div>"
          "</div></body></html>";
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

void startConfigPortal() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AssetMonitor_Config"); // No password required as per requirements
  Serial.print("Config Portal Started. Connect to AP 'AssetMonitor_Config', IP: ");
  Serial.println(WiFi.softAPIP());
  
  // Update OLED to show AP info
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("AP Mode Active");
  display.print("SSID: AssetMonitor");
  display.println("_Config");
  display.print("IP: ");
  display.println(WiFi.softAPIP().toString());
  display.println("No password needed");
  display.display();
  
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", wifiConfigHTML()); });
  server.on("/wifi_save_config", HTTP_POST, handleWifiConfigPost);
  server.on("/run_local_ap", HTTP_POST, handleRunLocalAP); // Handler for the button
  server.onNotFound([](){ server.send(200, "text/html", wifiConfigHTML()); });
  server.begin();
  while (true) { server.handleClient(); delay(10); }
}

void startLocalAPMode() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("AssetMonitor_Config"); // No password required
    Serial.print("Local AP Mode Started. Connect to AP 'AssetMonitor_Config', IP: ");
    Serial.println(WiFi.softAPIP());

    // Update OLED to show local AP info
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Local AP Mode");
    display.print("SSID: AssetMonitor");
    display.println("_Config");
    display.print("IP: ");
    display.println(WiFi.softAPIP().toString());
    display.println("Ready for access");
    display.display();

    // Set up all the necessary routes for full functionality in AP mode
    server.on("/", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
    server.on("/dashboard", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
    server.on("/config", HTTP_GET, []() { server.send(200, "text/html", htmlConfig()); });
    server.on("/events", HTTP_GET, sendHtmlEventsPage);
    server.on("/asset", HTTP_GET, []() {
        if (server.hasArg("idx")) {
            uint8_t idx = server.arg("idx").toInt();
            if (idx < config.assetCount && idx < MAX_ASSETS) { 
                server.send(200, "text/html", htmlAssetDetail(idx)); 
                return; 
            }
        }
        server.send(404, "text/plain", "Asset not found");
    });
    server.on("/shiftlogs_page", HTTP_GET, handleShiftLogsPage);
    server.on("/download_shiftlog", HTTP_GET, handleDownloadShiftLog);
    server.on("/analytics", HTTP_GET, []() { server.send(200, "text/html", htmlAnalytics()); });
    server.on("/analytics-compare", HTTP_GET, []() { server.send(200, "text/html", htmlAnalyticsCompare()); });
    server.on("/reconfigure_wifi", HTTP_POST, handleWiFiReconfigurePost);
    server.on("/save_config", HTTP_POST, handleConfigPost);
    server.on("/clear_log", HTTP_POST, handleClearLog);
    server.on("/export_log", HTTP_GET, handleExportLog);
    server.on("/api/summary", HTTP_GET, handleApiSummary);
    server.on("/api/events", HTTP_GET, handleApiEvents);
    server.on("/api/config", HTTP_GET, handleApiConfig);
    server.on("/api/note", HTTP_POST, handleApiNote);
    server.on("/delete_logs", HTTP_POST, handleDeleteLogs);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("Web server started in local AP mode. Device is ready.");
}

// --- Add the handler function for "Run Locally" button ---
void handleRunLocalAP() {
    Preferences prefs;
    prefs.begin("assetmon", false);
    prefs.putBool("run_local_ap", true);
    prefs.end();
    // HTML meta refresh to redirect after 2 seconds
    String html = "<html><head><meta http-equiv='refresh' content='2;url=/'></head><body>";
    html += "<h2>Device will reboot and run locally as an Access Point.<br>";
    html += "You will be redirected to the dashboard.</h2></body></html>";
    server.send(200, "text/html", html);
    delay(1200);
    ESP.restart();
}

// Custom WiFi setup function (replaces WiFiManager)
void setupCustomWiFi() {
    updateOledStartupStatus("Checking WiFi prefs...");
    
    Preferences prefs;
    prefs.begin("assetmon", true);
    bool runLocalAP = prefs.getBool("run_local_ap", false);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (runLocalAP) {
        updateOledStartupStatus("Starting Local AP...");
        prefs.begin("assetmon", false);
        prefs.putBool("run_local_ap", false);
        prefs.end();
        startLocalAPMode();
        return;
    }

    if (ssid == "" || pass == "") {
        updateOledStartupStatus("No WiFi config, starting AP...");
        startConfigPortal(); // <-- THIS USES THE CUSTOM HTML!
        return;
    }

    // Normal WiFi connection logic
    updateOledStartupStatus("Connecting to WiFi...");
    Serial.println("Attempting to connect to WiFi...");
    WiFi.mode(WIFI_STA);
    strncpy(wifi_ssid, ssid.c_str(), 32);
    wifi_ssid[32] = '\0';
    strncpy(wifi_pass, pass.c_str(), 64);
    wifi_pass[64] = '\0';
    
    WiFi.begin(wifi_ssid, wifi_pass);
    
    int attempt = 0;
    while (WiFi.status() != WL_CONNECTED && attempt < 20) { // Try for 10 seconds
        delay(500);
        Serial.print(".");
        attempt++;
        
        // --- OLED update during WiFi connection attempt ---
        if (attempt % 4 == 0) { // Update every 2 seconds
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(0, 0);
            display.print("Connecting WiFi...");
            display.println();
            display.print("SSID: ");
            display.println(wifi_ssid);
            display.print("Attempt: ");
            display.print(attempt/4);
            display.display();
        }
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi connection failed. Starting config portal...");
        updateOledStartupStatus("WiFi failed, starting AP...");
        
        // Clear saved credentials since they don't work
        prefs.begin("assetmon", false);
        prefs.remove("ssid");
        prefs.remove("pass");
        prefs.end();
        
        startConfigPortal();
        return;
    }
    
    Serial.println("");
    Serial.print("WiFi connected successfully! IP address: ");
    Serial.println(WiFi.localIP());
    
    // --- OLED update after WiFi connected ---
    updateOledDisplay();
}

// --- Core Utility Functions ---

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

  size_t storedBytes = localSavePrefs.putBytes("cfg", &config, sizeof(Config));
  localSavePrefs.end();

  if (storedBytes != sizeof(Config)) {
    Serial.printf("saveConfig: WARNING: Expected to store %u bytes, but stored %u bytes\n", sizeof(Config), storedBytes);
  } else {
    Serial.println("saveConfig: Configuration saved successfully.");
  }
}

void ensureLogFileHasHeader() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f) {
    f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
    if (f) {
      f.println("Date,Time,Asset,Event,State,Availability,Runtime,Downtime,MTBF,MTTR,Stops,RunDuration,StopDuration,Note");
      f.close();
      Serial.println("Created log file with header.");
    } else {
      Serial.println("Failed to create log file!");
    }
  } else {
    f.close();
    Serial.println("Log file already exists.");
  }
}

void setupTime() {
  // Only try NTP if we're connected to WiFi (not in AP mode)
  if (WiFi.status() == WL_CONNECTED) {
    // Step 1: Start NTP to get the accurate universal time (UTC).
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    Serial.print("Waiting for NTP time sync...");
    time_t now = time(nullptr);
    int retry = 0;
    // Wait until the time is valid (post-2021 in this case)
    while (now < 1609459200 && retry < 100) {
      delay(500);
      Serial.print(".");
      now = time(nullptr);
      retry++;
    }
    Serial.println(" done");

    if (now < 1609459200) {
      Serial.println("NTP time sync FAILED. Timestamps will be incorrect!");
    } else {
      Serial.println("NTP sync successful. Base UTC time acquired.");
    }
    
    // Step 2: Set the specific local timezone rules.
    setenv("TZ", "GMT0BST-1,M3.5.0/1,M10.5.0/2", 1);
    tzset(); // Applies the new TZ environment variable

    // Step 3: Display the final, correctly converted local time.
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)){
      Serial.printf("Current local time (GMT/BST): %s", asctime(&timeinfo));
    } else {
      Serial.println("Failed to obtain local time.");
    }
  } else {
    Serial.println("Not connected to WiFi - skipping NTP time sync");
    // Set a basic timezone for local AP mode
    setenv("TZ", "GMT0BST-1,M3.5.0/1,M10.5.0/2", 1);
    tzset();
  }
}

// --- New function to log system-wide events (for Serial mode) ---
void logSystemEvent(bool systemIsStarting, time_t now, const char* triggerAssetNameOrReason) {
  if (config.monitoringMode != MONITORING_MODE_SERIAL) return;

  struct tm* ti = localtime(&now);
  char datebuf[11], timebuf[9];
  strftime(datebuf, sizeof(datebuf), "%d/%m/%Y", ti);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti);

  unsigned long currentTotalRun = g_systemTotalRunningTimeSecs;
  unsigned long currentTotalStop = g_systemTotalStoppedTimeSecs;

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
  String assetColumnValue = "SYSTEM";
  String noteForLog = "";

  if (!systemIsStarting) {
    assetColumnValue = (triggerAssetNameOrReason && strlen(triggerAssetNameOrReason) > 0) ? triggerAssetNameOrReason : "SYSTEM_WIDE";
    noteForLog = "System Stop Triggered";
    if (triggerAssetNameOrReason && strlen(triggerAssetNameOrReason) > 0 && strcmp(triggerAssetNameOrReason, "SYSTEM_WIDE") != 0 && strcmp(triggerAssetNameOrReason, "SYSTEM") != 0) {
        noteForLog = String("Root Cause: ") + triggerAssetNameOrReason;
    }
  } else {
    noteForLog = (triggerAssetNameOrReason && strlen(triggerAssetNameOrReason) > 0) ? triggerAssetNameOrReason : "System Recovered";
  }

  File f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
  if (f) {
    f.printf("%s,%s,%s,%s,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%u,00:00,00:00,%s\n",
             datebuf, timebuf, assetColumnValue.c_str(), eventName.c_str(),
             systemIsStarting ? 1 : 0, sysAvail, sysRTM_minutes, sysSTM_minutes,
             sysMTBF_minutes, sysMTTR_minutes, g_systemStopCount, noteForLog.c_str());
    f.close();
    Serial.printf("Logged %s event for system (note: %s)\n", eventName.c_str(), noteForLog.c_str());
  } else {
    Serial.println("Failed to open log file for system event!");
  }
}

void initializeShiftState() {
  if (!config.enableShiftArchiving || config.numShifts == 0) {
    g_shiftFeatureInitialized = true;
    g_currentShiftIndex = -1; 
    Serial.println("Shift archiving is disabled or no shifts configured.");
    return;
  }

  time_t now_ts = time(nullptr);
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
        if (currentShiftStartToday >= latestPassedShiftStartEpoch) {
          latestPassedShiftStartEpoch = currentShiftStartToday;
          bestMatchIndex = i;
        }
      } else if (now_ts >= currentShiftStartYesterday) {
         if (currentShiftStartYesterday >= latestPassedShiftStartEpoch) {
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
  } else {
    if (config.numShifts > 0) {
        int latestTimeMinutes = -1;
        int fallbackIndex = 0;
        for(uint8_t i=0; i<config.numShifts; ++i) {
            int sh, sm;
            if(sscanf(config.shifts[i].startTime, "%d:%d", &sh, &sm) == 2) {
                int currentShiftMinutes = sh * 60 + sm;
                if(currentShiftMinutes > latestTimeMinutes) {
                    latestTimeMinutes = currentShiftMinutes;
                    fallbackIndex = i;
                }
            }
        }
        g_currentShiftIndex = fallbackIndex;
        struct tm shift_tm_instance = timeinfo;
        sscanf(config.shifts[g_currentShiftIndex].startTime, "%d:%d", &(shift_tm_instance.tm_hour), &(shift_tm_instance.tm_min));
        shift_tm_instance.tm_sec = 0;
        g_currentShiftStartTimeEpoch = mktime(&shift_tm_instance) - 86400;
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

// =================================================================
// --- setup() function 
// =================================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Asset Monitor LocalAP Starting ---");

  // --- OLED INIT ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 allocation failed");
  } else {
    updateOledStartupStatus("Initializing...");
  }

  if (!SPIFFS.begin(true)) { 
    Serial.println("SPIFFS.begin() failed! Halting."); 
    return; 
  }
  Serial.println("SPIFFS initialized.");
  updateOledStartupStatus("SPIFFS ready");

  ensureLogFileHasHeader();

  if (!prefs.begin("assetmon", false)) {
      Serial.println("Global prefs.begin() failed during setup! WiFi saving might fail.");
  } else {
      Serial.println("Global Preferences initialized.");
  }

  loadConfig();
  Serial.println("Configuration loaded/initialized.");
  updateOledStartupStatus("Config loaded");

  initializeShiftState();
  Serial.printf("DEBUG: enableShiftArchiving=%d, g_shiftFeatureInitialized=%d\n", config.enableShiftArchiving, g_shiftFeatureInitialized);

  // Use custom WiFi setup instead of WiFiManager
  setupCustomWiFi();

  // --- OLED update after WiFi setup ---
  updateOledDisplay();

  setupTime();
  Serial.println("WiFi and Time setup complete.");

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

  // Set up web server routes - these work in both WiFi and AP modes
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
  server.on("/reconfigure_wifi", HTTP_POST, handleWiFiReconfigurePost);
  server.on("/save_config", HTTP_POST, handleConfigPost);
  server.on("/clear_log", HTTP_POST, handleClearLog);
  server.on("/export_log", HTTP_GET, handleExportLog);
  server.on("/api/summary", HTTP_GET, handleApiSummary);
  server.on("/api/events", HTTP_GET, handleApiEvents);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/note", HTTP_POST, handleApiNote);
  server.on("/delete_logs", HTTP_POST, handleDeleteLogs);
  
  server.onNotFound(handleNotFound);
  
  // Only start server if not already started in AP mode
  if (!server.isStarted()) {
    server.begin();
  }
  
  Serial.println("Web server started. Device is ready.");
  updateOledStartupStatus("Ready!");
  delay(1000);
  updateOledDisplay(); // Show final network status
}