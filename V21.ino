#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>

// Forward declarations for all handlers and HTML generators
String htmlDashboard();
String htmlAnalytics();
String htmlAnalyticsCompare();
String htmlConfig();
String htmlEvents();
String wifiConfigHTML();
String htmlAssetDetail(uint8_t idx);
void handleConfigPost();
void handleClearLog();
void handleExportLog();
void handleApiSummary();
void handleApiEvents();
void handleApiConfig();
void handleApiNote();
void handleNotFound();
void updateEventNote(String date, String time, String assetName, String note, String reason);
void logEvent(uint8_t assetIdx, bool state, time_t now, const char* note = nullptr, unsigned long runDuration = 0, unsigned long stopDuration = 0);
void handleWiFiReconfigurePost();
// ADDED/ENSURED Forward Declarations for functions called before definition:
void setupWiFiSmart();
void setupTime();
void saveConfig();
String urlDecode(const String& str); // Existing
String urlEncode(const String& str); // ADDED Forward Declaration for urlEncode

// USER CONFIGURATION
#define MAX_ASSETS 10
#define LOG_FILENAME "/log.csv"
const char* DEFAULT_DOWNTIME_REASONS[5] = {
  "Maintenance", "Material Shortage", "Operator Break", "Equipment Failure", "Changeover"
};

struct AssetConfig { char name[32]; uint8_t pin; };
struct Config {
  uint8_t assetCount;
  uint16_t maxEvents;
  AssetConfig assets[MAX_ASSETS];
  char downtimeReasons[5][32];
  int tzOffset;
  int longStopThresholdSec; // in seconds
} config;

struct AssetState {
  bool lastState;
  time_t lastChangeTime;
  unsigned long runningTime;
  unsigned long stoppedTime;
  unsigned long sessionStart;
  unsigned long lastEventTime;
  uint32_t runCount;
  uint32_t stopCount;
  unsigned long lastRunDuration;   // New: For event log
  unsigned long lastStopDuration;  // New: For event log
};

// Define the Event struct
struct Event {
  time_t timestamp;          // Event time (seconds since epoch)
  char assetName[32];        // Asset name
  char eventType[8];         // "START" or "STOP"
  int state;                 // 1 = running, 0 = stopped
  float availability;        // Percentage
  float runtime;             // Minutes
  float downtime;            // Minutes
  float mtbf;                // Minutes
  float mttr;                // Minutes
  unsigned int stops;        // Stop count
  char runDuration[8];       // "MM:SS", may be empty
  char stopDuration[8];      // "MM:SS", may be empty
  char note[64];             // Note, may be empty

  Event() {
    timestamp = 0;
    assetName[0] = '\0';
    eventType[0] = '\0';
    state = 0;
    availability = 0;
    runtime = 0;
    downtime = 0;
    mtbf = 0;
    mttr = 0;
    stops = 0;
    runDuration[0] = '\0';
    stopDuration[0] = '\0';
    note[0] = '\0';
  }
};

WebServer server(80);
Preferences prefs; // Note: Using local Preferences objects in functions is often safer
AssetState assetStates[MAX_ASSETS];

// --- WiFi Config Section ---
char wifi_ssid[33] = "";
char wifi_pass[65] = "";

String wifiConfigHTML() {
  String html = "<!DOCTYPE html><html><head><title>WiFi Setup</title>"
                "<meta charset='UTF-8'>"
                "<style>"
                "body { font-family: Arial; margin: 2em; background: #f6f8fa; }"
                "form { background: #fff; padding: 2em; border-radius: 8px; box-shadow: 0 0 8px #ccc; max-width: 400px; margin:auto;}"
                "h1 { color: #0366d6; }"
                "label { display:block; margin-top:1em; }"
                "input[type=text], input[type=password] { width:100%; padding:0.5em; }"
                "input[type=submit] { background: #0366d6; color: #fff; border: none; padding: 0.7em 1.5em; margin-top:1em; border-radius: 4px;}"
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
  // html += wifi_pass; // Typically don't pre-fill password for security
  html += "'>"
          "<div class='note'>Enter your WiFi details above. Device will reboot after saving.</div>"
          "<input type='submit' value='Save & Reboot'>"
          "</form>"
          "</body></html>";
  return html;
}

void handleWifiConfigPost() {
  if (server.hasArg("ssid")) {
    String ssid_arg = server.arg("ssid"); // Use different var name
    String pass_arg = server.arg("password"); // Use different var name
    strncpy(wifi_ssid, ssid_arg.c_str(), 32);
    wifi_ssid[32] = '\0';
    strncpy(wifi_pass, pass_arg.c_str(), 64);
    wifi_pass[64] = '\0';

    Preferences localPrefs; // Use local instance
    if (localPrefs.begin("assetmon", false)) {
        localPrefs.putString("ssid", wifi_ssid);
        localPrefs.putString("pass", wifi_pass);
        localPrefs.end();
    } else {
        Serial.println("handleWifiConfigPost: Failed to open preferences.");
    }
    server.send(200, "text/html", "<h2>Saved! Rebooting...</h2><meta http-equiv='refresh' content='3;url=/' />");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing WiFi credentials");
  }
}

void startConfigPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AssetMonitor_Config", "setpassword"); // Consider a more secure password or no password
  Serial.print("Config Portal Started. Connect to AP 'AssetMonitor_Config', IP: ");
  Serial.println(WiFi.softAPIP());
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", wifiConfigHTML()); });
  server.on("/wifi_save_config", HTTP_POST, handleWifiConfigPost); // Ensure this route is correct
  server.begin();
  while (true) { server.handleClient(); delay(10); } // Blocking loop for config portal
}

void setupWiFiSmart() {
  Preferences localPrefs; // Use local instance
  String ssid_from_prefs = "";
  String pass_from_prefs = "";

  if (localPrefs.begin("assetmon", true)) { // true for read-only
    ssid_from_prefs = localPrefs.getString("ssid", "");
    pass_from_prefs = localPrefs.getString("pass", "");
    localPrefs.end();
  } else {
      Serial.println("setupWiFiSmart: Failed to open preferences.");
  }

  if (ssid_from_prefs.length() == 0) {
    Serial.println("SSID not found in preferences. Starting Config Portal.");
    startConfigPortal(); // This will block until configured
    return; // Should not be reached if startConfigPortal blocks
  }

  strncpy(wifi_ssid, ssid_from_prefs.c_str(), 32); wifi_ssid[32] = '\0';
  strncpy(wifi_pass, pass_from_prefs.c_str(), 64); wifi_pass[64] = '\0';

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_pass);
  Serial.printf("Connecting to %s", wifi_ssid);
  for (int i=0; i<20 && WiFi.status()!=WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println(); // Newline after dots

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("WiFi connection failed. Starting Config Portal.");
    startConfigPortal(); // Fallback to config portal
  }
}
// --- End WiFi Config Section ---

// --- Time Setup Function --- MOVED HERE, OUTSIDE of setup()
void setupTime() {
  // Use tzOffset from config. Ensure config is loaded before this.
  // configTime's first param is offset in seconds from UTC.
  // configTime's second param is daylight offset in seconds (0 if not used or handled by TZ string).
  configTime(config.tzOffset, 0, "pool.ntp.org", "time.nist.gov", "europe.pool.ntp.org");
  
  // Example of setting a TZ string for more complex DST rules (e.g., UK GMT/BST)
  // This would override the simple offsets from configTime if used *after* it for time functions like localtime()
  // setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1); // For UK
  // tzset(); // Apply the TZ environment variable

  Serial.print("Waiting for NTP time sync...");
  time_t now = time(nullptr); 
  int retry = 0;
  // Wait until time is past a reasonable point (e.g., after 2001)
  while (now < 1000000000 && retry < 60) { // Roughly check if time is after year 2001
    delay(500); 
    Serial.print("."); 
    now = time(nullptr); 
    retry++; 
  }
  Serial.println(); // Newline

  if (now < 1000000000) {
    Serial.println("NTP time sync failed. Using system time (if any).");
  } else {
    Serial.println("NTP time sync successful.");
    struct tm timeinfo;
    getLocalTime(&timeinfo); // Updates timeinfo based on TZ settings
    Serial.printf("Current local time: %s", asctime(&timeinfo)); // asctime() adds a newline
  }
}
// --- End Time Setup ---


// CONFIG LOAD/SAVE
void loadConfig() {
  Preferences localPrefs; 
  bool prefsOpenedForRead = localPrefs.begin("assetmon", true); 

  if (!prefsOpenedForRead) {
    prefsOpenedForRead = localPrefs.begin("assetmon", false); 
  }

  if (!prefsOpenedForRead) {
    Serial.println("loadConfig: Failed to open preferences. Using defaults.");
    config.assetCount = 1; 
    config.maxEvents = 100;
    strcpy(config.assets[0].name, "Default Asset"); config.assets[0].pin = 0;
    for (int i = 0; i < 5; ++i) {
        strncpy(config.downtimeReasons[i], DEFAULT_DOWNTIME_REASONS[i], sizeof(config.downtimeReasons[i]) - 1);
        config.downtimeReasons[i][sizeof(config.downtimeReasons[i]) - 1] = '\0';
    }
    config.tzOffset = 0;
    config.longStopThresholdSec = 5 * 60;
    return; 
  }

  if (localPrefs.isKey("cfg")) { 
    size_t len = localPrefs.getBytes("cfg", &config, sizeof(config)); 
    if (len != sizeof(config)) {
      Serial.println("loadConfig: Config size mismatch. Using defaults and saving.");
      goto use_defaults_and_save; 
    }
    Serial.println("loadConfig: Configuration loaded from Preferences.");
    if (config.longStopThresholdSec == 0 && config.maxEvents !=0 ) { // Example migration/default for new field
        config.longStopThresholdSec = 5*60; 
        Serial.println("loadConfig: Initialized longStopThresholdSec.");
    }
  } else {
    Serial.println("loadConfig: No 'cfg' key. Using defaults and saving.");
use_defaults_and_save: 
    config.assetCount = 2; 
    config.maxEvents = 1000;
    strcpy(config.assets[0].name, "Line 1"); config.assets[0].pin = 4;
    strcpy(config.assets[1].name, "Line 2"); config.assets[1].pin = 12;
    for (uint8_t i = config.assetCount; i < MAX_ASSETS; ++i) {
        strcpy(config.assets[i].name, "");
        config.assets[i].pin = 0; 
    }
    for (int i = 0; i < 5; ++i) {
      strncpy(config.downtimeReasons[i], DEFAULT_DOWNTIME_REASONS[i], sizeof(config.downtimeReasons[i]) - 1);
      config.downtimeReasons[i][sizeof(config.downtimeReasons[i]) - 1] = '\0';
    }
    config.tzOffset = 0;
    config.longStopThresholdSec = 5*60; 
    
    // Important: end read-only session if it was open, before saveConfig tries to open for write
    if(prefsOpenedForRead) localPrefs.end(); 
    saveConfig(); // Save the defaults
    // No need to re-read here, config struct is already populated with defaults
  }
  
  if (prefsOpenedForRead) { // Ensure it's ended if it was opened
    localPrefs.end(); 
  }
}

void saveConfig() { 
  Preferences localSavePrefs; 
  if (!localSavePrefs.begin("assetmon", false)) { 
    Serial.println("saveConfig: Failed to begin preferences for writing.");
    return; 
  }
  if (localSavePrefs.putBytes("cfg", &config, sizeof(config)) == sizeof(config)) {
    Serial.println("saveConfig: Configuration saved.");
  } else {
    Serial.println("saveConfig: Error writing configuration.");
  }
  localSavePrefs.end(); 
}

void setup() {
  Serial.begin(115200); 
  Serial.println("\n--- Device Starting ---");

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS.begin() failed! Halting."); 
    return; 
  }
  Serial.println("SPIFFS initialized.");

  // It's generally safer for loadConfig/saveConfig to manage their own Preferences instances.
  // The global 'prefs' object isn't strictly needed if all uses are localized.
  // If you were to use the global 'prefs' object:
  // if (!prefs.begin("assetmon", false)) { 
  //   Serial.println("Global prefs.begin() failed!"); 
  // } else {
  //   Serial.println("Global preferences initialized.");
  //   prefs.end(); // Close if only used for an initial check
  // }

  loadConfig(); // Load app config (assets, etc.)
  setupWiFiSmart(); // Setup WiFi connection or config portal
  setupTime(); // Setup time from NTP using tzOffset from loaded config

  Serial.printf("Initializing %u assets defined in config...\n", config.assetCount);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i < MAX_ASSETS) { // Boundary check
      pinMode(config.assets[i].pin, INPUT_PULLUP);
      assetStates[i].lastState = digitalRead(config.assets[i].pin);
      assetStates[i].lastChangeTime = time(nullptr);
      assetStates[i].sessionStart = assetStates[i].lastChangeTime;
      assetStates[i].runningTime = 0;
      assetStates[i].stoppedTime = 0;
      assetStates[i].runCount = 0;
      assetStates[i].stopCount = 0;
      assetStates[i].lastEventTime = 0;
      assetStates[i].lastRunDuration = 0;
      assetStates[i].lastStopDuration = 0;
      Serial.printf("Asset %u ('%s', pin %u) initialized. Initial state: %s\n",
                    i, config.assets[i].name, config.assets[i].pin,
                    assetStates[i].lastState ? "HIGH/RUNNING" : "LOW/STOPPED");
    }
  }

  // Main pages
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
  server.on("/dashboard", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
  server.on("/config", HTTP_GET, []() { server.send(200, "text/html", htmlConfig()); });
  server.on("/events", HTTP_GET, []() { server.send(200, "text/html", htmlEvents()); });
  server.on("/asset", HTTP_GET, []() {
    if (server.hasArg("idx")) {
      uint8_t idx = server.arg("idx").toInt();
      if (idx < config.assetCount && idx < MAX_ASSETS) { // Check against MAX_ASSETS too
        server.send(200, "text/html", htmlAssetDetail(idx));
        return;
      }
    }
    server.send(404, "text/plain", "Asset not found or index invalid");
  });

  // Analytics
  server.on("/analytics", HTTP_GET, []() { server.send(200, "text/html", htmlAnalytics()); });
  server.on("/analytics-compare", HTTP_GET, []() { server.send(200, "text/html", htmlAnalyticsCompare()); });
  
  // WiFi Reconfiguration
  // The GET request to /reconfigure_wifi will now be handled by handleWiFiReconfigurePost
  // which calls startConfigPortal().
  server.on("/reconfigure_wifi", HTTP_GET, handleWiFiReconfigurePost); // Changed from POST to GET to simplify triggering
                                                                    // Or keep as POST if you have a form submitting to it.
                                                                    // The current handleWiFiReconfigurePost doesn't expect form data.

  // Config and Log actions
  server.on("/save_config", HTTP_POST, handleConfigPost);
  server.on("/clear_log", HTTP_POST, handleClearLog);
  server.on("/export_log", HTTP_GET, handleExportLog);

  // API Endpoints
  server.on("/api/summary", HTTP_GET, handleApiSummary);
  server.on("/api/events", HTTP_GET, handleApiEvents);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/note", HTTP_POST, handleApiNote);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Web server started. Device is ready."); 
}

// --- loop() function and other function definitions follow ---
// (Your existing loop, logEvent, formatMMSS, eventToCSV, urlDecode, HTML generators, API handlers, etc.)
// Ensure the definition for handleConfigPost, htmlDashboard, etc., are present below.

void loop() {
  server.handleClient();
  time_t now = time(nullptr);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) continue; // Should not happen if config.assetCount is constrained

    bool currentState = digitalRead(config.assets[i].pin); // Read current pin state
    
    if (currentState != assetStates[i].lastState) {
      unsigned long elapsedSinceLastChange = now - assetStates[i].lastChangeTime;
      unsigned long currentRunDuration = 0;
      unsigned long currentStopDuration = 0;

      if (currentState) { // Pin is HIGH - asset just STARTED running
        assetStates[i].stoppedTime += elapsedSinceLastChange; // Add elapsed time to total stopped time
        assetStates[i].runCount++;
        currentStopDuration = elapsedSinceLastChange; // This was the duration of the stop that just ended
        assetStates[i].lastStopDuration = currentStopDuration;
        assetStates[i].lastRunDuration = 0; // Reset last run duration as a new run is starting
        logEvent(i, currentState, now, nullptr, 0, currentStopDuration);
      } else { // Pin is LOW - asset just STOPPED running
        assetStates[i].runningTime += elapsedSinceLastChange; // Add elapsed time to total running time
        assetStates[i].stopCount++;
        currentRunDuration = elapsedSinceLastChange; // This was the duration of the run that just ended
        assetStates[i].lastRunDuration = currentRunDuration;
        assetStates[i].lastStopDuration = 0; // Reset last stop duration as a new stop is starting
        logEvent(i, currentState, now, nullptr, currentRunDuration, 0);
      }
      assetStates[i].lastState = currentState;
      assetStates[i].lastChangeTime = now;
      assetStates[i].lastEventTime = now; // Update last event time
    }
  }
  delay(200); // Poll inputs every 200ms
}

// LOGGING (now with runDuration, stopDuration fields)

void logEvent(uint8_t assetIdx, bool state, time_t now, const char* note, unsigned long runDuration, unsigned long stopDuration) {
  if (assetIdx >= MAX_ASSETS) return; // Boundary check

  AssetState& as = assetStates[assetIdx]; // Get reference to the specific asset's state

  // Calculate overall metrics up to this event
  // Note: runningTime and stoppedTime in AssetState are cumulative *excluding* the current ongoing period.
  // For the event log, we want the state *at the moment of the event*.
  // The runDuration and stopDuration passed to this function are for the *completed* phase that just ended.

  unsigned long totalRunningTimeForEvent = as.runningTime;
  unsigned long totalStoppedTimeForEvent = as.stoppedTime;

  // If the event is a START, the 'stopDuration' that just completed is added to totalStoppedTime.
  // If the event is a STOP, the 'runDuration' that just completed is added to totalRunningTime.
  // This logic is already handled before calling logEvent by updating as.runningTime and as.stoppedTime in loop().

  float avail = (totalRunningTimeForEvent + totalStoppedTimeForEvent) > 0 ? (100.0 * totalRunningTimeForEvent / (totalRunningTimeForEvent + totalStoppedTimeForEvent)) : 0;
  float total_runtime_min = totalRunningTimeForEvent / 60.0;
  float total_downtime_min = totalStoppedTimeForEvent / 60.0;
  
  // MTBF/MTTR calculation:
  // MTBF = Total Running Time / Number of Stops
  // MTTR = Total Stopped Time / Number of Stops
  // These should ideally use the *completed* running and stopped periods.
  float mtbf_val = (as.stopCount > 0) ? (float)totalRunningTimeForEvent / as.stopCount : 0; // MTBF in seconds
  float mttr_val = (as.stopCount > 0) ? (float)totalStoppedTimeForEvent / as.stopCount : 0; // MTTR in seconds
  
  mtbf_val = mtbf_val / 60.0; // Convert to minutes
  mttr_val = mttr_val / 60.0; // Convert to minutes


  struct tm * ti = localtime(&now); // Use localtime for display based on TZ settings
  char datebuf[11], timebuf[9];
  strftime(datebuf, sizeof(datebuf), "%d/%m/%Y", ti); // DD/MM/YYYY
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti); // HH:MM:SS

  File f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
  if (!f) { Serial.println("Failed to open log file for writing!"); return; }
  
  f.printf("%s,%s,%s,%s,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%s,%s,%s\n",
    datebuf, timebuf, config.assets[assetIdx].name,
    state ? "START" : "STOP", // Event Type
    state,                    // Current State (1 for START/Running, 0 for STOP/Stopped)
    avail, total_runtime_min, total_downtime_min, 
    mtbf_val, mttr_val,
    as.stopCount,             // Number of stops recorded so far
    (runDuration > 0 ? formatMMSS(runDuration).c_str() : ""),   // Duration of the run phase that just ENDED (if event is STOP)
    (stopDuration > 0 ? formatMMSS(stopDuration).c_str() : ""), // Duration of the stop phase that just ENDED (if event is START)
    note ? note : ""
  );
  f.close();
  // as.lastEventTime = now; // This is already updated in loop()
  Serial.printf("Event logged for %s: %s\n", config.assets[assetIdx].name, state ? "START" : "STOP");
}

// MM:SS format helper for durations
String formatMMSS(unsigned long seconds) {
  if (seconds == 0 && seconds != assetStates[0].lastRunDuration && seconds != assetStates[0].lastStopDuration) return ""; // Avoid returning "" for actual zero durations if needed
  unsigned int min = seconds / 60;
  unsigned int sec = seconds % 60;
  char buf[8];
  sprintf(buf, "%02u:%02u", min, sec);
  return String(buf);
}

// Converts an Event to a CSV line using UK local time (BST/GMT)
String eventToCSV(const Event& e) {
  struct tm * ti = localtime(&e.timestamp);
  char datebuf[16], timebuf[16];
  strftime(datebuf, sizeof(datebuf), "%d/%m/%Y", ti);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti);

  String csv = String(datebuf) + "," + String(timebuf) + "," +
               String(e.assetName) + "," + String(e.eventType) + "," +
               String(e.state) + "," + String(e.availability, 2) + "," +
               String(e.runtime, 2) + "," + String(e.downtime, 2) + "," +
               String(e.mtbf, 2) + "," + String(e.mttr, 2) + "," +
               String(e.stops) + "," +
               String(e.runDuration) + "," + String(e.stopDuration) + "," +
               String(e.note);
  return csv;
}

String urlEncode(const String& str) {
  String encodedString = "";
  char c;
  char code0;
  char code1;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encodedString += '+';
    } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encodedString += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) {
        code1 = (c & 0xf) - 10 + 'A';
      }
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) {
        code0 = c - 10 + 'A';
      }
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }
  return encodedString;
}

String urlDecode(const String& str) {
  String decoded = "";
  char temp[] = "0x00"; // For strtol
  unsigned int len = str.length();
  unsigned int i = 0;
  while (i < len) {
    char c = str.charAt(i);
    if (c == '%') {
      if (i + 2 < len) {
        temp[2] = str.charAt(i + 1);
        temp[3] = str.charAt(i + 2);
        decoded += char(strtol(temp, NULL, 16));
        i += 3;
      } else { // Invalid percent encoding, skip
        i++;
      }
    } else if (c == '+') {
      decoded += ' ';
      i++;
    } else {
      decoded += c;
      i++;
    }
  }
  return decoded;
}

// --- htmlDashboard() function ---
String htmlDashboard() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>";
  html += "body{font-family:Roboto,Arial,sans-serif;background:#f3f7fa;margin:0;padding:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.3rem 0 1.3rem 2rem;text-align:left;font-size:2em;font-weight:700;box-shadow:0 2px 10px #0001;}";
  html += ".nav{display:flex;justify-content:center;align-items:center;gap:1rem;margin:1.5rem 0 1rem 0;flex-wrap:wrap;}";
  html += ".nav .nav-btn{background:#fff;color:#1976d2;border:none;border-radius:8px;padding:0.7em 1.3em;font-size:1.13em;font-weight:700;box-shadow:0 2px 12px #1976d222;cursor:pointer;transition:.2s;text-decoration:none;}"; // Added text-decoration
  html += ".nav .nav-btn:hover{background:#e3f0fc;}";
  html += ".main{max-width:1200px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += "#chart-container{width:100%;overflow-x:auto;}";
  html += ".statrow{display:flex;gap:1.5em;flex-wrap:wrap;justify-content:center;margin:2em 0 2em 0;}";
  html += ".stat{flex:1 1 220px;border-radius:10px;padding:1.2em;text-align:left;font-size:1.1em;margin:0.4em 0;box-shadow:0 2px 8px #0001;font-weight:500;background:#f5f7fa;border:2px solid #e0e0e0;min-width:200px;}"; // Added min-width
  html += ".stat.stopped{background:#ffeaea;border-color:#f44336;}";
  html += ".stat.running{background:#e6fbe7;border-color:#54c27c;}";
  html += "table{width:100%;border-collapse:collapse;font-size:1em;margin-top:2em;}";
  html += "th,td{padding:0.7em 0.5em;text-align:left;border-bottom:1px solid #eee;}";
  html += "th{background:#2196f3;color:#fff;}";
  html += "tr{background:#fcfcfd;} tr:nth-child(even){background:#f3f7fa;}";
  html += "td:last-child .nav-btn{margin:0;}";
  // html += ".nav-btn{background:#fff;color:#1976d2;border:none;border-radius:8px;padding:0.45em 1.1em;font-size:1em;font-weight:700;box-shadow:0 2px 12px #1976d222;cursor:pointer;margin:0 0.1em;}"; // Redundant with .nav .nav-btn
  // html += ".nav-btn:hover{background:#e3f0fc;}"; // Redundant
  html += "@media (max-width:900px){.main{padding:0.5em;}.statrow{gap:0.5em;}.stat{min-width:150px;max-width:100%;font-size:1em;padding:0.6em;}}";
  html += "@media (max-width:700px){header{font-size:1.3em;padding:1em 0 1em 1em;}.nav{flex-direction:column;align-items:center;margin:1em 0 1em 0;gap:0.4em;}.card{padding:0.7em;}.statrow{gap:0.4em;max-width:100%;}.stat{min-width: calc(50% - 0.4em);}}"; // Adjusted .stat for 2-column
  html += "</style>";
  html += "</head><body>";
  html += "<header>Dashboard</header>";
  html += "<nav class='nav'>";
  html += "<form action='/events' style='margin:0;'><button type='submit' class='nav-btn'>Event Log</button></form>";
  html += "<form action='/config' style='margin:0;'><button type='submit' class='nav-btn'>Setup</button></form>";
  html += "<a href='/analytics-compare' class='nav-btn'>Compare Assets</a>"; // Changed to <a> tag
  html += "<form action='/export_log' style='margin:0;'><button type='submit' class='nav-btn'>Export CSV</button></form>";
  html += "</nav>";
  html += "<div class='main'>";
  html += "<div class='card'>";
  html += "<div id='chart-container'><canvas id='barChart' height='200'></canvas></div>";
  html += "<div class='statrow' id='statrow'></div>";
  html += "<div style='overflow-x:auto;'><table id='summaryTable'><thead><tr>";
  html += "<th>Name</th><th>State</th><th>Avail (%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th></th></tr></thead><tbody></tbody></table>";
  html += "</div></div>";
  html += "<script>";
  html += R"rawliteral(
// Format float minutes as hh:mm:ss
function formatHHMMSS(val) {
  if (isNaN(val) || val < 0.01) return "00:00:00";
  let totalSeconds = Math.round(val * 60);
  let h = Math.floor(totalSeconds / 3600);
  let m = Math.floor((totalSeconds % 3600) / 60) % 60; // Ensure minutes are within 0-59
  let s = totalSeconds % 60;
  return (h < 10 ? "0" : "") + h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
}
let chartObj=null;
function updateDashboard() {
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    let tbody = document.querySelector('#summaryTable tbody');
    let assets = data.assets;
    let rows = tbody.rows;
    let statrow = document.getElementById('statrow');
    statrow.innerHTML = ''; // Clear previous stat cards
    let n=assets.length;
    for(let i=0;i<n;++i){
      let asset = assets[i];
      let stateClass = asset.state==1 ? "running" : "stopped";
      // Table row
      let row = rows[i];
      if (!row) {
        row = tbody.insertRow();
        for (let j=0;j<9;++j) row.insertCell(); // Add 9 cells for 9 columns
      }
      let v0 = asset.name,
          v1 = `<span style="color:${asset.state==1?'#256029':'#b71c1c'};font-weight:bold">${asset.state==1?'RUNNING':'STOPPED'}</span>`,
          v2 = asset.availability.toFixed(2),
          v3 = formatHHMMSS(asset.total_runtime),
          v4 = formatHHMMSS(asset.total_downtime),
          v5 = formatHHMMSS(asset.mtbf),
          v6 = formatHHMMSS(asset.mttr),
          v7 = asset.stop_count,
          v8 = `<form action='/analytics' method='GET' style='display:inline;'><input type='hidden' name='asset' value="${encodeURIComponent(asset.name)}"><button type='submit' class='nav-btn'>Analytics</button></form>`; // Corrected button text
      let vals = [v0,v1,v2,v3,v4,v5,v6,v7,v8];
      for(let j=0;j<9;++j) row.cells[j].innerHTML = vals[j];
      // Stat card
      let statHtml = `<div class='stat ${stateClass}'><b>${asset.name}</b><br>Avail: ${asset.availability.toFixed(1)}%<br>Run: ${formatHHMMSS(asset.total_runtime)}<br>Stops: ${asset.stop_count}</div>`; // Corrected class for state
      statrow.innerHTML += statHtml;
    }
    while (rows.length > n) tbody.deleteRow(rows.length-1); // Remove extra rows if assets decrease
    
    let availData=[], names=[], runtimeData=[], downtimeData=[];
    for (let asset of assets) {
      availData.push(asset.availability);
      runtimeData.push(asset.total_runtime);
      downtimeData.push(asset.total_downtime);
      names.push(asset.name);
    }
    let ctx = document.getElementById('barChart').getContext('2d');
    if (!window.chartObj) { // Create chart if it doesn't exist
      window.chartObj = new Chart(ctx, {
        type: 'bar',
        data: {
          labels: names,
          datasets: [
            { label: 'Availability (%)', data: availData, backgroundColor: '#42a5f5', yAxisID: 'y' },
            { label: 'Runtime (min)', data: runtimeData, backgroundColor: '#66bb6a', yAxisID: 'y1' },
            { label: 'Downtime (min)', data: downtimeData, backgroundColor: '#ef5350', yAxisID: 'y1' }
          ]
        },
        options: {
          responsive:true, maintainAspectRatio:false,
          scales: {
            y: { beginAtZero:true, max:100, title:{display:true,text:'Availability (%)'} },
            y1: { beginAtZero:true, position: 'right', grid: { drawOnChartArea: false }, title: { display:true, text:'Runtime/Downtime (min)' }}
          }
        }
      });
    } else { // Update existing chart
      window.chartObj.data.labels = names;
      window.chartObj.data.datasets[0].data = availData;
      window.chartObj.data.datasets[1].data = runtimeData;
      window.chartObj.data.datasets[2].data = downtimeData;
      window.chartObj.update();
    }
  });
}
updateDashboard(); setInterval(updateDashboard, 5000);
)rawliteral";
  html += "</script></body></html>";
  return html;
}

// --- htmlAnalytics() function ---
String htmlAnalytics() {
  String assetName = server.hasArg("asset") ? urlDecode(server.arg("asset")) : "";
  String html = "<!DOCTYPE html><html lang='en'><head><title>Analytics: ";
  html += assetName + "</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}th,td{text-align:left;}header{background:#1976d2;color:#fff;padding:1.3rem 0;text-align:center;box-shadow:0 2px 10px #0001;font-size:1.6em;font-weight:700;}"; // Added font size/weight
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;} .nav a{text-decoration:none;background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;} .nav a:hover{background:#e3f0fc;}"; // Style for nav links
  html += ".main{max-width:1100px;margin:1rem auto;padding:1rem;} .metrics{display:flex;flex-wrap:wrap;gap:1em;justify-content:center;margin-bottom:1.5em;} .metric{background:#fff;padding:1em;border-radius:8px;box-shadow:0 2px 8px #0001;text-align:center;flex:1 1 150px;font-size:1.1em;} .metric b{display:block;font-size:1.4em;color:#1976d2;}"; // KPI styles
  html += ".controls{display:flex;flex-wrap:wrap;gap:1em;align-items:center;margin-bottom:1.5em;padding:1em;background:#fff;border-radius:8px;box-shadow:0 2px 8px #0001;} .controls label{margin-right:0.5em;} .controls input[type=datetime-local]{padding:0.5em;border-radius:4px;border:1px solid #ccc;} .controls .toggle{display:flex;align-items:center;} .controls .toggle input{margin-right:0.3em;} .export-btn{margin-left:auto;padding:0.6em 1em;background:#66bb6a;color:#fff;border:none;border-radius:6px;font-weight:700;cursor:pointer;}"; // Controls styling
  html += ".chartcard{background:#fff;padding:1.5em;border-radius:8px;box-shadow:0 2px 10px #0001;margin-bottom:1.5em;} .tablecard{background:#fff;padding:1.5em;border-radius:8px;box-shadow:0 2px 10px #0001;} table{width:100%;border-collapse:collapse;} th,td{padding:0.6em;border-bottom:1px solid #eee;} th{background:#e3f0fc;color:#1976d2;}"; // Card and table styles
  html += "@media (max-width:700px){.main{padding:0.5em;} .controls{flex-direction:column;align-items:stretch;} .controls label{width:100%;margin-bottom:0.5em;} .export-btn{margin-left:0;width:100%;text-align:center;} .metrics{gap:0.5em;} .metric{flex-basis:calc(50% - 0.5em);font-size:1em;} .metric b{font-size:1.2em;}}";
  html += "</style>";
  html += "</head><body>";
  html += "<header>Analytics: <span id='assetNameInHeader'>" + assetName + "</span></header>";
  html += "<nav class='nav'>";
  html += "<a href='/'>Dashboard</a>"; // Changed to <a>
  html += "<a href='/events'>Event Log</a>"; // Changed to <a>
  html += "<a href='/analytics-compare'>Compare Assets</a>"; // Already <a>
  html += "</nav>";
  html += "<div class='main'>";
  html += "<div class='metrics' id='kpiMetrics'></div>";
  html += "<div style='margin-bottom:0.5em; color:#888; font-size:1em;'>(Durations in <b>h:mm:ss</b> or <b>mm:ss</b>)</div>";
  html += "<div class='controls'>";
  html += "<label>From: <input type='datetime-local' id='fromTime'></label>";
  html += "<label>To: <input type='datetime-local' id='toTime'></label>";
  html += "<label class='toggle'><input type='checkbox' id='showStart' checked> Show START</label>";
  html += "<label class='toggle'><input type='checkbox' id='showStop' checked> Show STOP</label>";
  html += "<label class='toggle'><input type='checkbox' id='showMTBF'> Show MTBF</label>";
  html += "<label class='toggle'><input type='checkbox' id='showMTTR'> Show MTTR</label>";
  html += "<button class='export-btn' id='exportPng'>Export PNG</button>";
  html += "</div>";
  html += "<div class='chartcard'><canvas id='eventChart' style='width:100%;max-width:1050px;height:340px;'></canvas></div>";
  html += "<div class='tablecard'>";
  html += "<h3>Recent Events</h3><table style='width:100%;'><thead><tr>"
          "<th>Date</th><th>Time</th><th>Asset</th><th>Event</th><th>Avail(%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Run Duration</th><th>Stop Duration</th><th>Note</th>" // Added <th> for Note
          "</tr></thead><tbody id='recentEvents'></tbody></table></div>";
  html += "<script>";
  html += R"rawliteral(
console.log('Analytics script started (v14 - Enhanced MTBF/MTTR tooltips).');

// --- Utility Functions ---
function floatMinToMMSS(val) { 
  if (typeof val === "string") val = parseFloat(val);
  if (isNaN(val) || val < 0) { 
      let totalSeconds = 0; 
      let m = Math.floor(totalSeconds / 60); 
      let s = totalSeconds % 60;
      return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
  }
  let totalSeconds = Math.round(val * 60);
  if (totalSeconds >= 3600) { 
    let h = Math.floor(totalSeconds / 3600); 
    let remainingSecondsAfterHours = totalSeconds % 3600;
    let m = Math.floor(remainingSecondsAfterHours / 60); 
    let s = remainingSecondsAfterHours % 60;
    return `${h.toString()}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
  } else { 
    let m = Math.floor(totalSeconds / 60); 
    let s = totalSeconds % 60; 
    return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`; 
  }
}
function mmssToSeconds(mmss) { 
  if (!mmss || typeof mmss !== "string") return 0; 
  let parts = mmss.split(":");
  if (parts.length !== 2 && parts.length !==3) return 0; 
  let h = 0, m = 0, s = 0;
  if (parts.length === 3) {
    h = parseInt(parts[0], 10); m = parseInt(parts[1], 10); s = parseInt(parts[2], 10);
  } else { 
    m = parseInt(parts[0], 10); s = parseInt(parts[1], 10);
  }
  return (isNaN(h) ? 0 : h * 3600) + (isNaN(m) ? 0 : m * 60) + (isNaN(s) ? 0 : s);
}
function parseEventDate(eventRow) {
  if (!eventRow || eventRow.length < 2) return new Date(0); 
  try {
    let [d, m, y] = eventRow[0].split('/').map(Number); let [hh, mm, ss] = eventRow[1].split(':').map(Number);
    if (isNaN(d) || isNaN(m) || isNaN(y) || isNaN(hh) || isNaN(mm) || isNaN(ss)) return new Date(0);
    return new Date(Date.UTC(y, m - 1, d, hh, mm, ss)); // Use UTC to avoid timezone issues with date parsing
  } catch (e) { console.error('Error parsing date for eventRow:', eventRow, e); return new Date(0); }
}
function toDatetimeLocal(dt) { // Converts a Date object to "yyyy-MM-ddThh:mm" for datetime-local input
  if (!(dt instanceof Date) || isNaN(dt)) dt = new Date(); 
  try {
    // Create a new date object adjusted for the local timezone offset for display purposes
    const timezoneOffset = dt.getTimezoneOffset() * 60000; // offset in milliseconds
    const localDate = new Date(dt.getTime() - timezoneOffset);
    const pad = n => n < 10 ? '0' + n : n;
    return localDate.getFullYear() + '-' + pad(localDate.getMonth() + 1) + '-' + pad(localDate.getDate()) +
           'T' + pad(localDate.getHours()) + ':' + pad(localDate.getMinutes());
  } catch (e) {
    console.error('Error in toDatetimeLocal:', e, 'Input date:', dt);
    const now = new Date(Date.now() - (new Date().getTimezoneOffset() * 60000)); // Fallback to current local time
    return now.toISOString().slice(0, 16);
  }
}

// --- Global Variables ---
let asset = '';
try {
  asset = decodeURIComponent(new URLSearchParams(window.location.search).get("asset") || "");
  const assetNameElement = document.getElementById('assetNameInHeader');
  if (assetNameElement) assetNameElement.textContent = asset;
} catch (e) { console.error('Error getting asset from URL:', e); }

let allEvents = []; let eventChart = null; let filteredEventsGlobal = []; 

// --- Core Logic ---
function fetchAnalyticsData() {
  if (!asset) {
    console.warn('No asset specified, aborting fetch.');
    const kpiDiv = document.getElementById('kpiMetrics');
    if (kpiDiv) kpiDiv.innerHTML = "<div class='metric'>No asset specified. Add ?asset=YourAssetName to URL.</div>";
    return;
  }
  fetch('/api/events')
    .then(response => { if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`); return response.json(); })
    .then(rawEvents => {
      if (!Array.isArray(rawEvents)) { console.error('Fetched data is not an array:', rawEvents); allEvents = []; }
      else {
        allEvents = rawEvents.map(line => (typeof line === 'string') ? line.split(',') : []) // Ensure lines are split
                             .filter(eventRow => eventRow.length > 13 && eventRow[2] && eventRow[2].trim() === asset.trim()); // Filter by asset and ensure valid row length
      }
      if (allEvents.length === 0) {
        const kpiDiv = document.getElementById('kpiMetrics');
        if (kpiDiv) kpiDiv.innerHTML = `<div class='metric'>No data found for asset: ${asset}</div>`;
      }
      setupRangePickers(); renderEventChart(); renderRecentEvents();
    })
    .catch(error => {
      console.error('Error fetching or processing analytics data:', error);
      const kpiDiv = document.getElementById('kpiMetrics');
      if (kpiDiv) kpiDiv.innerHTML = `<div class='metric'>Error loading data: ${error.message}</div>`;
    });
}
function setupRangePickers() {
  let defaultFromDate, defaultToDate;
  if (allEvents.length > 0) {
    try {
      let eventDates = allEvents.map(e => parseEventDate(e)).filter(d => d.getTime() !== 0); 
      if (eventDates.length > 0) {
        defaultToDate = new Date(Math.max.apply(null, eventDates)); 
        defaultFromDate = new Date(defaultToDate.getTime() - 12 * 60 * 60 * 1000); // Default to last 12 hours
      } else { // Fallback if no valid dates parsed
        defaultToDate = new Date(); defaultFromDate = new Date(defaultToDate.getTime() - 12 * 60 * 60 * 1000); 
      }
    } catch (e) { // General fallback
      defaultToDate = new Date(); defaultFromDate = new Date(defaultToDate.getTime() - 12 * 60 * 60 * 1000); 
    }
  } else { // Fallback if no events
    defaultToDate = new Date(); defaultFromDate = new Date(defaultToDate.getTime() - 12 * 60 * 60 * 1000); 
  }
  document.getElementById('fromTime').value = toDatetimeLocal(defaultFromDate);
  document.getElementById('toTime').value = toDatetimeLocal(defaultToDate);
  
  ['fromTime', 'toTime', 'showStart', 'showStop', 'showMTBF', 'showMTTR'].forEach(id => {
    const el = document.getElementById(id); if (el) el.onchange = renderEventChart;
  });
  const exportButton = document.getElementById('exportPng');
  if (exportButton) {
    exportButton.onclick = function () {
      if (!eventChart) { console.warn('Export PNG: Chart not ready.'); return; }
      try { let url = eventChart.toBase64Image(); let a = document.createElement('a'); a.href = url; a.download = `analytics_${asset}.png`; a.click(); }
      catch (e) { console.error('Error exporting chart to PNG:', e); }
    };
  }
}
function renderKPIs(currentFilteredEventsArray) {
  const kpiDiv = document.getElementById('kpiMetrics'); if (!kpiDiv) return;
  if (!currentFilteredEventsArray || currentFilteredEventsArray.length === 0) { kpiDiv.innerHTML = "<div class='metric'>No data for selected range</div>"; return; }
  try {
    const latestEvent = currentFilteredEventsArray[currentFilteredEventsArray.length - 1];
    // Ensure all indices are valid before accessing
    const stops = latestEvent[10] || '0';
    const runtime = latestEvent[6] || '0';
    const downtime = latestEvent[7] || '0';
    const availability = latestEvent[5] || '0';
    const mtbf = latestEvent[8] || '0';
    const mttr = latestEvent[9] || '0';

    kpiDiv.innerHTML =
      `<div class='metric'>Stops: <b>${stops}</b></div>
       <div class='metric'>Runtime: <b>${floatMinToMMSS(runtime)}</b></div>
       <div class='metric'>Downtime: <b>${floatMinToMMSS(downtime)}</b></div>
       <div class='metric'>Availability: <b>${parseFloat(availability).toFixed(2)}%</b></div>
       <div class='metric'>MTBF: <b>${floatMinToMMSS(mtbf)}</b></div>
       <div class='metric'>MTTR: <b>${floatMinToMMSS(mttr)}</b></div>`;
  } catch (e) { console.error('Error rendering KPIs:', e, 'Event data:', currentFilteredEventsArray.length > 0 ? currentFilteredEventsArray[currentFilteredEventsArray.length - 1] : 'No events'); kpiDiv.innerHTML = "<div class='metric'>Error rendering KPIs</div>"; }
}
function renderEventChart() {
  if (!allEvents || allEvents.length === 0) { if (eventChart) { eventChart.destroy(); eventChart = null; } renderKPIs([]); return; } // Pass empty array to clear KPIs
  let fromDate, toDate;
  try { 
    // Parse as UTC from the datetime-local input, then treat as local for filtering
    // The datetime-local input provides a string like "YYYY-MM-DDTHH:MM" which is local time.
    // new Date() will parse this as local time.
    fromDate = new Date(document.getElementById('fromTime').value); 
    toDate = new Date(document.getElementById('toTime').value); 
  }
  catch (e) { console.error('Error parsing date/time input values:', e); return; }

  const showStart = document.getElementById('showStart').checked; 
  const showStop = document.getElementById('showStop').checked;
  const showMTBF = document.getElementById('showMTBF').checked; 
  const showMTTR = document.getElementById('showMTTR').checked;
  
  filteredEventsGlobal = allEvents.filter(eventRow => {
    try {
      const eventDate = parseEventDate(eventRow); // parseEventDate returns UTC Date object
      if (eventDate.getTime() === 0) return false; 
      // Compare eventDate (UTC) with fromDate/toDate (which should be treated as local boundaries but parsed into Date objects)
      // For correct filtering, convert fromDate and toDate to UTC if eventDate is UTC, or ensure consistent comparison.
      // Since parseEventDate creates UTC dates, let's ensure from/to are also treated as such for comparison.
      // However, datetime-local inputs are local. A robust way is to get UTC epoch ms for comparison.
      const eventEpoch = eventDate.getTime();
      const fromEpoch = fromDate.getTime();
      const toEpoch = toDate.getTime();

      if (eventEpoch < fromEpoch || eventEpoch > toEpoch) return false; 
      if (!eventRow[3]) return false; 
      
      const eventType = eventRow[3].trim().toUpperCase();
      if (eventType === "START" && !showStart) return false;
      if (eventType === "STOP" && !showStop) return false; 
      return true;
    } catch (e) { return false; }
  });

  renderKPIs(filteredEventsGlobal); 
  if (filteredEventsGlobal.length === 0) { if (eventChart) { eventChart.destroy(); eventChart = null; } return; }

  try {
    let times = filteredEventsGlobal.map(e => e[1]); // HH:MM:SS (local time from log)
    let avail = filteredEventsGlobal.map(e => parseFloat(e[5]));
    let mtbfValues = filteredEventsGlobal.map(e => parseFloat(e[8])); 
    let mttrValues = filteredEventsGlobal.map(e => parseFloat(e[9]));
    let stateArr = filteredEventsGlobal.map(e => e[4] ? e[4].trim() : 'UNKNOWN_STATE'); 
    
    let pointColors = filteredEventsGlobal.map((e, index, arr) => {
      const eventType = e[3] ? e[3].trim().toUpperCase() : "";
      let durationForStopDecision = "0:00"; // Default to ensure it's a valid mm:ss string
      if (eventType === "STOP") {
         // For a STOP event, the relevant duration is the 'Stop Duration' of the *next* START event,
         // or if it's the last event, its own 'Stop Duration' (which might be ongoing or just logged)
         // The CSV format is: 11: RunDur, 12: StopDur
         // A STOP event logs the RunDur that just ended. The StopDur is logged by the *next* START.
         durationForStopDecision = e[12] || "0:00"; // This is the stop duration that *ended* if this event was a START
                                                  // For a STOP event, this field is usually empty.
                                                  // We need to look at the next event if it's a START
        if (index + 1 < arr.length) {
            const nextEvent = arr[index+1];
            const nextEventType = nextEvent[3] ? nextEvent[3].trim().toUpperCase() : "";
            if (nextEventType === "START") {
                durationForStopDecision = nextEvent[12] || "0:00"; // Stop duration logged by the START event
            }
        } else {
            // If this is the last STOP event, there's no subsequent START to log its duration.
            // This stop might be ongoing. For coloring, we might not have a completed stop duration.
            // Or, if your logging for a STOP event *does* include its own (potentially ongoing) stop duration in col 12, use that.
            // Assuming col 12 for a STOP event is empty or refers to a previous stop for a START.
            // For now, let's assume if it's a stop, and we don't have a *next* start, we can't determine its completed duration for coloring.
        }
      }
      // Use config.longStopThresholdSec for comparison (fetch it or have it globally)
      // Assuming longStopThresholdSec is available globally (e.g. fetched from /api/config)
      const longStopThreshold = window.longStopThresholdSec || 300; // Default to 5 mins (300s)
      if (eventType === "STOP" && mmssToSeconds(durationForStopDecision) >= longStopThreshold) return "#c62828"; // Darker red for long stops
      if (eventType === "STOP") return "#ff9800"; // Orange for normal stops
      return "#43a047"; // Green for starts
    });

    let pointSizes = filteredEventsGlobal.map((e, index, arr) => {
       const eventType = e[3] ? e[3].trim().toUpperCase() : "";
       let durationForStopDecision = "0:00";
       if (eventType === "STOP") {
        if (index + 1 < arr.length) {
            const nextEvent = arr[index+1];
            const nextEventType = nextEvent[3] ? nextEvent[3].trim().toUpperCase() : "";
            if (nextEventType === "START") {
                durationForStopDecision = nextEvent[12] || "0:00";
            }
        }
       }
      const longStopThreshold = window.longStopThresholdSec || 300;
      let defaultSize = 7;
      if (eventType === "STOP" && mmssToSeconds(durationForStopDecision) >= longStopThreshold) { defaultSize = 12; }
      return defaultSize;
    });

    let datasets = [{
      label: 'Availability (%)', data: avail, yAxisID: 'y', stepped: true, tension: 0,
      pointRadius: pointSizes, pointBackgroundColor: pointColors, pointBorderColor: pointColors, showLine: true,
      segment: {
        borderColor: ctx => {
          // Use the state at the *beginning* of the segment (p0)
          const stateValue = stateArr[ctx.p0DataIndex]; 
          if (stateValue === "1") return "#43a047"; // Green for running
          if (stateValue === "0") return "#c62828"; // Red for stopped
          return "#000000"; // Black for unknown/default
        },
        borderWidth: 3
      }
    }];
    if (showMTBF) datasets.push({ label: 'MTBF (min)', data: mtbfValues, yAxisID: 'y1', borderColor: "#1565c0", borderWidth: 2, tension: 0.1, pointRadius: 4, fill:false });
    if (showMTTR) datasets.push({ label: 'MTTR (min)', data: mttrValues, yAxisID: 'y1', borderColor: "#FF8F00", borderWidth: 2, tension: 0.1, pointRadius: 4, fill:false }); // Changed color for MTTR
    
    if (eventChart) eventChart.destroy();
    const ctx = document.getElementById('eventChart').getContext('2d');
    eventChart = new Chart(ctx, {
      type: 'line', data: { labels: times, datasets: datasets },
      options: {
        responsive: true, maintainAspectRatio: false,
        interaction: { mode: 'nearest', axis: 'x', intersect: false }, // intersect false for easier tooltip hover
        layout: { padding: { top: 15 }},
        plugins: {
          tooltip: {
            callbacks: {
              title: (tooltipItems) => {
                if (!tooltipItems.length) return ''; 
                const idx = tooltipItems[0].dataIndex;
                if (!filteredEventsGlobal[idx]) return 'Error: No data for this point.';
                return `Event: ${filteredEventsGlobal[idx][3]} at ${filteredEventsGlobal[idx][0]} ${filteredEventsGlobal[idx][1]}`;
              },
              label: (tooltipItem) => {
                const idx = tooltipItem.dataIndex;
                const eventRow = filteredEventsGlobal[idx];
                if (!eventRow) return null;
                const eventType = eventRow[3] ? eventRow[3].trim().toUpperCase() : "";
                const datasetLabel = tooltipItem.dataset.label || '';
                let lines = [];

                if (datasetLabel === 'Availability (%)') {
                  const currentAvail = parseFloat(eventRow[5]).toFixed(2);
                  lines.push(`Availability: ${currentAvail}%`);
                  if (eventType === "START") { // This is a START event
                    const stopDurationSeconds = mmssToSeconds(eventRow[12] || "0:00"); // Stop duration that just ENDED
                    if (stopDurationSeconds > 0) {
                         lines.push(`(Prior Stop: ${floatMinToMMSS(stopDurationSeconds / 60.0)})`);
                    }
                  } else if (eventType === "STOP") { // This is a STOP event
                    const runDurationSeconds = mmssToSeconds(eventRow[11] || "0:00"); // Run duration that just ENDED
                    if (runDurationSeconds > 0) {
                        lines.push(`(Prior Run: ${floatMinToMMSS(runDurationSeconds / 60.0)})`);
                    }
                  }
                } else if (datasetLabel === 'MTBF (min)' || datasetLabel === 'MTTR (min)') {
                  const currentValue = tooltipItem.raw; 
                  const formattedCurrentValue = floatMinToMMSS(currentValue);
                  lines.push(`${datasetLabel}: ${formattedCurrentValue}`);

                  if (idx > 0) {
                    const previousValue = tooltipItem.dataset.data[idx - 1];
                    if (typeof previousValue === 'number' && typeof currentValue === 'number') {
                        const change = currentValue - previousValue;
                        if (Math.abs(change) > 1e-7) { 
                            const formattedChange = floatMinToMMSS(Math.abs(change));
                            let changeIndicator = "";
                            if (datasetLabel === 'MTBF (min)') {
                                changeIndicator = change > 0 ? `(Increased by ${formattedChange} - Good)` : `(Decreased by ${formattedChange} - Bad)`;
                            } else { // MTTR
                                changeIndicator = change > 0 ? `(Increased by ${formattedChange} - Bad)` : `(Decreased by ${formattedChange} - Good)`;
                            }
                            lines.push(changeIndicator);
                        } else {
                            lines.push(`(No significant change)`);
                        }
                    }
                  } else {
                    lines.push(`(Initial value)`);
                  }
                  // Additional context for MTBF/MTTR based on the event type
                  if (datasetLabel === 'MTBF (min)' && eventType === "STOP") { // MTBF is updated after a stop (i.e., a run has completed)
                    const lastRunDurationSeconds = mmssToSeconds(eventRow[11] || "0:00");
                    if (lastRunDurationSeconds > 0) {
                      lines.push(`(Influenced by last run: ${floatMinToMMSS(lastRunDurationSeconds / 60.0)})`);
                    }
                  } else if (datasetLabel === 'MTTR (min)' && eventType === "START") { // MTTR is updated after a start (i.e., a stop has completed)
                    const lastStopDurationSeconds = mmssToSeconds(eventRow[12] || "0:00");
                    if (lastStopDurationSeconds > 0) {
                      lines.push(`(Influenced by last stop: ${floatMinToMMSS(lastStopDurationSeconds / 60.0)})`);
                    }
                  }
                }
                return lines.length > 0 ? lines : null; 
              }
            }
          },
          legend: { position: 'top' }
        },
        scales: {
          x: { title: { display: true, text: 'Time (HH:MM:SS Local)' } }, // Clarify local time
          y: { 
            title: { display: true, text: 'Availability (%)' }, 
            beginAtZero: true, 
            suggestedMax: 105, // Allow a bit of space above 100
            ticks: {
                stepSize: 20,    // Try to make ticks at 0, 20, 40, 60, 80, 100
                callback: function(value, index, values) {
                    if (value > 100 && value < 105) return undefined; // Hide ticks slightly above 100 unless it's the max
                    if (value === 100 || value === 0 || (value > 0 && value < 100 && value % 20 === 0)) return value;
                    return undefined; 
                }
            }
          }, 
          // ...
y1: { type: 'linear', display: true, position: 'right', title: { display: true, text: 'MTBF/MTTR (min)' }, beginAtZero: true, grid: { drawOnChartArea: false }, ticks: { callback: val => floatMinToMMSS(val) } // Ensure this is the full function name
        }
      }
    });
  } catch (e) { console.error('Error rendering chart:', e); }
}
function renderRecentEvents() {
  const tbody = document.getElementById('recentEvents'); if (!tbody) return; tbody.innerHTML = ""; 
  if (!allEvents || allEvents.length === 0) { tbody.innerHTML = "<tr><td colspan='13'>No event data for this asset.</td></tr>"; return; }
  
  // Use filteredEventsGlobal if available and populated, otherwise fallback to allEvents for recent view
  const eventsSource = (filteredEventsGlobal && filteredEventsGlobal.length > 0) ? filteredEventsGlobal : allEvents;
  const eventsToDisplay = eventsSource.slice(-10).reverse(); // Last 10 events from the (potentially filtered) source
  
  if (eventsToDisplay.length === 0) { tbody.innerHTML = "<tr><td colspan='13'>No recent events for this asset or filter.</td></tr>"; return; }
  
  eventsToDisplay.forEach(eventRow => {
    try {
      if (eventRow.length < 14) { 
          let tr = tbody.insertRow(); let td = tr.insertCell(); td.colSpan = 13; 
          td.textContent = "Malformed data row."; td.style.color = "orange"; return; 
      }
      let tr = tbody.insertRow();
      tr.insertCell().textContent = eventRow[0];  // Date
      tr.insertCell().textContent = eventRow[1];  // Time
      tr.insertCell().textContent = eventRow[2];  // Asset Name
      tr.insertCell().textContent = eventRow[3];  // Event Type (START/STOP)
      tr.insertCell().textContent = parseFloat(eventRow[5]).toFixed(2); // Availability
      tr.insertCell().textContent = floatMinToMMSS(eventRow[6]); // Total Runtime
      tr.insertCell().textContent = floatMinToMMSS(eventRow[7]); // Total Downtime
      tr.insertCell().textContent = floatMinToMMSS(eventRow[8]); // MTBF
      tr.insertCell().textContent = floatMinToMMSS(eventRow[9]); // MTTR
      tr.insertCell().textContent = eventRow[10]; // Stop Count
      // Display Run Duration if event is STOP, Stop Duration if event is START
      const eventType = eventRow[3] ? eventRow[3].trim().toUpperCase() : "";
      tr.insertCell().textContent = eventType === "STOP" ? floatMinToMMSS(mmssToSeconds(eventRow[11] || "0:00") / 60.0) : ""; 
      tr.insertCell().textContent = eventType === "START" ? floatMinToMMSS(mmssToSeconds(eventRow[12] || "0:00") / 60.0) : ""; 
      tr.insertCell().textContent = eventRow[13] || ""; // Note
    } catch (e) {
      console.error('Error rendering row for event:', eventRow, e);
      let tr = tbody.insertRow(); let td = tr.insertCell(); td.colSpan = 13; 
      td.textContent = "Error displaying row."; td.style.color = "red";
    }
  });
}

// --- Initialisation ---
function initAnalyticsPage() {
    // Fetch longStopThreshold from config to use for coloring points
    fetch('/api/config')
        .then(r => r.json())
        .then(cfg => {
            window.longStopThresholdSec = cfg.longStopThresholdSec || 300; // Store globally for chart rendering
            fetchAnalyticsData(); // Now fetch event data
        })
        .catch(e => {
            console.error("Failed to fetch config for longStopThreshold, using default.", e);
            window.longStopThresholdSec = 300; // Default if config fetch fails
            fetchAnalyticsData(); // Proceed to fetch event data
        });
}

if (document.readyState === 'loading') { 
  document.addEventListener('DOMContentLoaded', initAnalyticsPage); 
} else { 
  initAnalyticsPage(); 
}
)rawliteral";
  html += "</script>";
  html += "</div></body></html>";
  return html;
}

// --- htmlAnalyticsCompare() function ---
String htmlAnalyticsCompare() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Compare Assets</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.3rem 0;text-align:center;box-shadow:0 2px 10px #0001;font-size:1.6em;font-weight:700;}"; // Added font size/weight
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;}";
  html += ".nav button, .nav a {text-decoration:none;background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;}"; // Combined button and <a>
  html += ".nav button:hover, .nav a:hover {background:#e3f0fc;}"; // Combined hover
  html += ".main{max-width:1100px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += ".flexrow{display:flex;flex-wrap:wrap;gap:2em;justify-content:center;}"; // Added justify-content
  html += ".chartcard{flex:1 1 320px;min-width:300px; max-width: calc(50% - 1em);}"; // Adjusted for 2 per row, added max-width
  html += ".tablecard{overflow-x:auto; width:100%;}"; // Ensure table card takes full width
  html += "th, td { text-align: left !important; padding: 0.5em;}"; 
  html += "table {width:100%; border-collapse:collapse;} th{background:#e3f0fc; color:#1976d2;} tr:nth-child(even){background:#f8f9fa;}"; // Table styling
  html += "@media(max-width:700px){.flexrow{flex-direction:column;gap:1em;}.chartcard{max-width:100%;}.card{padding:0.7em;}}";
  html += "</style></head><body>";
  html += "<header>Compare Assets</header>";
  html += "<nav class='nav'>";
  html += "<a href='/'>Dashboard</a>"; // Changed to <a>
  html += "<a href='/events'>Event Log</a>"; // Changed to <a>
  html += "<a href='/export_log'>Export CSV</a>"; // Changed to <a>
  html += "</nav>";
  html += "<div class='main'>";
  html += "<div class='flexrow'>";
  html += "<div class='card chartcard'><canvas id='barAvail'></canvas></div>";
  html += "<div class='card chartcard'><canvas id='barStops'></canvas></div>";
  html += "<div class='card chartcard'><canvas id='barMTBF'></canvas></div>";
  html += "<div class='card chartcard'><canvas id='pieReasons'></canvas></div>";
  html += "</div>";
  html += "<div class='card tablecard'>";
  html += "<h3>Last Event Log Summary</h3><table style='width:100%;'><thead><tr>" // Clarified table title
          "<th>Asset</th>"
          "<th>Availability (%)</th>"
          "<th>Runtime</th>"
          "<th>Downtime</th>"
          "<th>Stops</th>"
          "<th>MTBF</th>"
          "<th>MTTR</th>"
          "</tr></thead><tbody id='compareTable'></tbody></table></div>";
  html += "<script>";
  html += R"rawliteral(
let allEvents = [], allAssetNames = [], configDowntimeReasons = []; // Renamed to avoid conflict

function formatMinutesToHHMMSS(val) { // Renamed for clarity
  if (isNaN(val) || val <= 0) return "0:00:00";
  let totalSeconds = Math.round(val * 60);
  let h = Math.floor(totalSeconds / 3600);
  let m = Math.floor((totalSeconds % 3600) / 60);
  let s = totalSeconds % 60;
  return h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
}

function fetchCompareData() { // Renamed
  fetch('/api/config').then(r=>r.json()).then(cfg=>{
    configDowntimeReasons = cfg.downtimeReasons || [];
    allAssetNames = cfg.assets.map(a=>a.name); // Get names from config
    
    fetch('/api/events').then(r=>r.json()).then(events=>{
      allEvents = events
        .map(l => (typeof l === 'string' ? l.split(',') : [])) // Ensure splitting
        .filter(v => v.length > 13 && allAssetNames.includes(v[2])); // Filter by known asset names
      
      renderCompareCharts();
      renderCompareTable();
    }).catch(e => console.error("Error fetching events for compare:", e));
  }).catch(e => console.error("Error fetching config for compare:", e));
}

function getLastValidMetric(events, metricIndex, defaultValue = 0) { // Renamed and added default
  if (!events || events.length === 0) return defaultValue;
  const lastEvent = events[events.length - 1];
  if (lastEvent && lastEvent.length > metricIndex) {
    const metric = parseFloat(lastEvent[metricIndex]);
    return isNaN(metric) ? defaultValue : metric;
  }
  return defaultValue;
}

function renderCompareCharts() {
  let dataByAsset = {};
  allAssetNames.forEach(assetName => { dataByAsset[assetName] = []; });
  allEvents.forEach(eventRow => {
    if (eventRow[2] && dataByAsset[eventRow[2]]) { // Check if asset name exists in map
        dataByAsset[eventRow[2]].push(eventRow);
    }
  });

  let labels = allAssetNames;
  let availabilityData = labels.map(name => getLastValidMetric(dataByAsset[name], 5));
  let stopsData = labels.map(name => (dataByAsset[name] || []).filter(e => e[3] === "STOP").length);
  let mtbfData = labels.map(name => getLastValidMetric(dataByAsset[name], 8));
  
  let reasonCounts = {};
  configDowntimeReasons.forEach(r => reasonCounts[r] = 0); // Initialize all configured reasons

  allEvents.forEach(eventRow => {
    if (eventRow[3] === "START" && eventRow.length > 13) { // Look at START events for preceding stop reason
        let noteFromEvent = eventRow[13] || "";
        let reasonPart = "";
        if (noteFromEvent.includes(" - ")) {
            reasonPart = noteFromEvent.split(" - ")[0].trim();
        } else {
            reasonPart = noteFromEvent.trim();
        }
        if (reasonPart && configDowntimeReasons.includes(reasonPart)) {
            reasonCounts[reasonPart] = (reasonCounts[reasonPart] || 0) + 1;
        }
    }
  });
  
  const pieLabels = Object.keys(reasonCounts).filter(r => reasonCounts[r] > 0); // Only show reasons with counts
  const pieData = pieLabels.map(r => reasonCounts[r]);
  const pieColors = ['#ffa726','#ef5350','#66bb6a','#42a5f5','#ab47bc', '#FFEE58', '#26A69A', '#78909C']; // Added more colors

  if(document.getElementById('barAvail').chart) document.getElementById('barAvail').chart.destroy();
  document.getElementById('barAvail').chart = new Chart(document.getElementById('barAvail').getContext('2d'), {
    type:'bar',data:{labels:labels,datasets:[{label:'Availability (%)',data:availabilityData,backgroundColor:'#42a5f5'}]},
    options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true,max:100}}}
  });
  
  if(document.getElementById('barStops').chart) document.getElementById('barStops').chart.destroy();
  document.getElementById('barStops').chart = new Chart(document.getElementById('barStops').getContext('2d'), {
    type:'bar',data:{labels:labels,datasets:[{label:'Stops',data:stopsData,backgroundColor:'#ef5350'}]},
    options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true, ticks:{stepSize:1}}}} // Ensure integer ticks for stops
  });
  
  if(document.getElementById('barMTBF').chart) document.getElementById('barMTBF').chart.destroy();
  document.getElementById('barMTBF').chart = new Chart(document.getElementById('barMTBF').getContext('2d'), {
    type:'bar',data:{labels:labels,datasets:[{label:'MTBF (min)',data:mtbfData,backgroundColor:'#66bb6a'}]},
    options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}
  });
  
  if(document.getElementById('pieReasons').chart) document.getElementById('pieReasons').chart.destroy();
  if (pieLabels.length > 0) { // Only render pie chart if there's data
    document.getElementById('pieReasons').chart = new Chart(document.getElementById('pieReasons').getContext('2d'), {
      type:'pie',data:{
        labels:pieLabels,datasets:[{data:pieData,backgroundColor:pieColors.slice(0, pieLabels.length)}]
      },options:{responsive:true,maintainAspectRatio:false, plugins:{legend:{position:'right'}}} // Legend on the right
    });
  } else {
    const ctxPie = document.getElementById('pieReasons').getContext('2d');
    ctxPie.clearRect(0,0,ctxPie.canvas.width, ctxPie.canvas.height);
    ctxPie.textAlign = 'center'; ctxPie.fillText('No downtime reasons logged.', ctxPie.canvas.width/2, ctxPie.canvas.height/2);
  }
}

function renderCompareTable() {
  let tbody = document.getElementById('compareTable'); // Corrected ID
  tbody.innerHTML = ""; // Clear previous table data
  
  let dataByAsset = {};
  allAssetNames.forEach(assetName => { dataByAsset[assetName] = []; });
  allEvents.forEach(eventRow => {
    if (eventRow[2] && dataByAsset[eventRow[2]]) {
        dataByAsset[eventRow[2]].push(eventRow);
    }
  });

  allAssetNames.forEach(assetName => {
    let assetEvents = dataByAsset[assetName] || [];
    let lastEvent = assetEvents.length ? assetEvents[assetEvents.length - 1] : null;
    
    let tr = tbody.insertRow();
    tr.insertCell().textContent = assetName;
    tr.insertCell().textContent = lastEvent ? parseFloat(lastEvent[5]).toFixed(2) : "-";
    tr.insertCell().textContent = lastEvent ? formatMinutesToHHMMSS(parseFloat(lastEvent[6])) : "-";
    tr.insertCell().textContent = lastEvent ? formatMinutesToHHMMSS(parseFloat(lastEvent[7])) : "-";
    tr.insertCell().textContent = assetEvents.filter(e => e[3] === "STOP").length;
    tr.insertCell().textContent = lastEvent ? formatMinutesToHHMMSS(parseFloat(lastEvent[8])) : "-";
    tr.insertCell().textContent = lastEvent ? formatMinutesToHHMMSS(parseFloat(lastEvent[9])) : "-";
  });
}

fetchCompareData(); // Initial fetch
)rawliteral";
  html += "</script>";
  html += "</div></body></html>";
  return html;
}

// --- htmlEvents() function ---
String htmlEvents() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Event Log</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001;font-size:1.6em;font-weight:700;}"; // Style header
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0 0 0;flex-wrap:wrap;align-items:center;}"; // Added align-items
  html += ".nav button, .nav a {text-decoration:none;background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;transition:.2s;cursor:pointer;margin-bottom:0.5em;}"; // Combined button and <a>
  html += ".nav button:hover, .nav a:hover {background:#e3f0fc;}"; // Combined hover
  html += ".main{max-width:1100px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += ".filterrow{display:flex;gap:1em;align-items:center;margin-bottom:1em;flex-wrap:wrap;}";
  html += ".filterrow label{font-weight:bold; margin-right:0.3em;} .filterrow select, .filterrow input {padding:0.5em; border-radius:4px; border:1px solid #ccc; margin-right:1em;}"; // Style filters
  html += ".scrollToggle{margin-left:auto;font-size:1em; padding: 0.5em 0.8em; border-radius:4px; background-color:#6c757d; color:white; border:none; cursor:pointer;} .scrollToggle:hover{background-color:#5a6268;}"; // Style toggle button
  html += "table{width:100%;border-collapse:collapse;font-size:1em;margin-top:0.8em;}";
  html += "th,td{padding:0.7em 0.5em;text-align:left;border-bottom:1px solid #eee;}";
  html += "th{background:#2196f3;color:#fff;}";
  html += "tr{background:#fcfcfd;} tr:nth-child(even){background:#f3f7fa;}";
  html += ".note{font-style:italic;color:#555;white-space:normal;word-break:break-word;display:block;max-width:260px;}";
  html += ".notebtn{padding:2px 8px;font-size:1em;border-radius:4px;background:#1976d2;color:#fff;border:none;cursor:pointer;margin-left:0.5em;} .notebtn:hover{background:#0d47a1;}";
  html += ".noteform{display:flex;flex-direction:column;background:#e3f0fc;padding:0.7em;margin:0.5em 0 0.5em 0;border-radius:8px;width:100%;box-sizing:border-box;}";
  html += ".noteform label{margin-bottom:0.3em;}";
  html += ".noteform select,.noteform input[type=\"text\"]{width:100%;margin-bottom:0.4em;font-size:1em;padding:0.2em 0.5em;box-sizing:border-box; border-radius:4px; border:1px solid #ccc;}"; // Added border
  html += ".noteform button{margin-top:0.1em;margin-right:0.3em;width:auto;align-self:flex-start; padding:0.4em 0.8em; border-radius:4px;}"; // Added padding & radius
  html += "td:last-child{max-width:280px;overflow-wrap:anywhere;word-break:break-word;}";
  html += "@media (max-width:700px){";
  html += "  #eventTable{display:none;}"; // Hide table on mobile
  html += "  .eventCard {background: #fff;border-radius: 10px;box-shadow: 0 2px 10px #0001;margin-bottom: 1.2em;padding: 1em;font-size: 1.05em;}";
  html += "  .eventCard div {margin-bottom: 0.3em;}";
  html += "  #mobileEvents {max-height:70vh;overflow-y:auto;}"; // Make mobile scrollable
  html += "  .filterrow {flex-direction:column; align-items:stretch;} .filterrow label, .filterrow select, .filterrow input {width:100%; margin-bottom:0.5em;} .scrollToggle{margin-left:0; width:100%; text-align:center;}"; // Stack filters on mobile
  html += "}";
  html += "@media (min-width:701px){";
  html += "  #mobileEvents{display:none;}"; // Hide mobile cards on desktop
  html += "}";
  html += "</style>";
  html += "<script>";
  html += R"rawliteral(
// Track currently open note form between refreshes:
window.openNoteFormId = null;
window.refreshIntervalId = null;
let eventData = []; // Holds all fetched events
let channelList = []; // Holds asset names for filter dropdown
let filterValue = "ALL"; // Current asset filter
let stateFilter = "ALL"; // Current state filter (RUNNING/STOPPED/ALL)
window.downtimeReasons = []; // Loaded from /api/config
let scrollInhibit = false; // To control auto-scrolling

function startAutoRefresh() {
  if (window.refreshIntervalId) clearInterval(window.refreshIntervalId);
  window.refreshIntervalId = setInterval(fetchAndRenderEvents, 5000); // Refresh every 5 seconds
}
function stopAutoRefresh() {
  if (window.refreshIntervalId) clearInterval(window.refreshIntervalId);
  window.refreshIntervalId = null;
}
function fetchChannelsAndStart() { // Fetches asset names for the filter dropdown
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    channelList = data.assets.map(a=>a.name);
    let sel = document.getElementById('channelFilter');
    sel.innerHTML = "<option value='ALL'>All Assets</option>"; // Default option
    for (let i=0;i<channelList.length;++i) {
      let opt = document.createElement("option");
      opt.value = channelList[i];
      opt.text = channelList[i];
      sel.appendChild(opt);
    }
    sel.onchange = function() { filterValue = sel.value; renderTable(); };
    document.getElementById('stateFilter').onchange = function() { stateFilter = this.value; renderTable(); };
    fetchReasonsAndEvents(); // After channels, fetch reasons then events
  }).catch(e => console.error("Error fetching channels:", e));
}
function fetchReasonsAndEvents() { // Fetches downtime reasons then event data
  fetch('/api/config').then(r=>r.json()).then(cfg=>{
    window.downtimeReasons = cfg.downtimeReasons || [];
    fetchAndRenderEvents(); // Initial fetch of events
    startAutoRefresh(); // Start auto-refreshing events
  }).catch(e => console.error("Error fetching config for reasons:", e));
}
function fetchAndRenderEvents() { // Fetches event log data
  fetch('/api/events').then(r=>r.json()).then(events=>{
    eventData = events; // Store raw event lines
    renderTable(); // Render the table with new data
  }).catch(e => console.error("Error fetching events:", e));
}
function cleanNote(val) { // Helper to clean up note string for display
  if (!val) return "";
  let v = val.trim();
  // Remove common empty/default values that might come from CSV parsing if note was empty
  if (v === "" || v === "," || v === ",," || v === "0,0," || v === "0.00,0,") return "";
  return v.replace(/^,+|,+$/g, ""); // Remove leading/trailing commas
}

// Format minutes (float) as hh:mm:ss
function minToHHMMSS(valStr) {
  let val = parseFloat(valStr); // Ensure it's a number
  if (isNaN(val) || val <= 0.001) return "00:00:00"; // Handle very small or zero values
  let totalSeconds = Math.round(val * 60);
  let h = Math.floor(totalSeconds / 3600);
  let m = Math.floor((totalSeconds % 3600) / 60); // Corrected to ensure minutes are 0-59
  let s = totalSeconds % 60;
  return (h < 10 ? "0" : "") + h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
}

// Format mm:ss or h:mm:ss from mm:ss or h:mm:ss string (for Run/Stop Duration)
function durationStrToHHMMSS(str) {
  if (!str || typeof str !== "string" || str.trim() === "") return ""; // Return empty if no duration
  let parts = str.split(":").map(Number);
  let h=0, m=0, s=0;
  if (parts.length === 3) { // h:mm:ss
    [h, m, s] = parts;
  } else if (parts.length === 2) { // mm:ss
    [m, s] = parts;
    h = Math.floor(m / 60);
    m = m % 60;
  } else {
    return ""; // Invalid format
  }
  if (isNaN(h) || isNaN(m) || isNaN(s)) return ""; // Invalid numbers
  return (h < 10 ? "0":"") + h + ":" + (m < 10 ? "0":"") + m + ":" + (s < 10 ? "0":"") + s;
}

function renderTable() {
  let tbody = document.getElementById('tbody');
  let mobileDiv = document.getElementById('mobileEvents');
  tbody.innerHTML = ''; // Clear desktop table
  mobileDiv.innerHTML = ''; // Clear mobile cards

  let stateMatcher = function(eventStateVal) { // Corrected function name
    if (stateFilter === "ALL") return true;
    if (stateFilter === "RUNNING") return eventStateVal === "1";
    if (stateFilter === "STOPPED") return eventStateVal === "0";
    return true; // Default if filter is unexpected
  };

  let isMobile = window.innerWidth <= 700;
  let displayData = eventData.slice().reverse(); // Show newest first

  for (let i=0; i<displayData.length; ++i) {
    let csvLine = displayData[i];
    if (typeof csvLine !== 'string') continue; // Skip if not a string
    let vals = csvLine.split(',');
    if (vals.length < 14) continue; // Expect at least 14 columns based on logEvent format

    let ldate = vals[0], ltime = vals[1], lasset = vals[2], levent = vals[3], lstateVal = vals[4];
    let lavail = vals[5], lrun = vals[6], lstop = vals[7], lmtbf = vals[8], lmttr = vals[9], lsc = vals[10];
    let runDurStr = vals[11], stopDurStr = vals[12];
    let lnote = vals.slice(13).join(',').replace(/\n$/, ""); // Join remaining parts for note, remove trailing newline

    let stopsInt = Math.round(Number(lsc));

    if (filterValue !== "ALL" && lasset !== filterValue) continue;
    if (!stateMatcher(lstateVal)) continue; // Use corrected stateMatcher

    let noteFormId = 'noteform-' + btoa(ldate + "|" + ltime + "|" + lasset).replace(/[^a-zA-Z0-9]/g, "_");
    let noteFormHtml = `
      <form class='noteform' id='${noteFormId}' onsubmit='return submitNote(event,"${ldate}","${ltime}","${lasset}")' style='display:none;'>
        <label>Reason: <select name='reason'>
          <option value=''></option>
          ${window.downtimeReasons.map(r =>
            `<option value="${r.replace(/"/g, "&quot;")}">${r}</option>`).join("")}
        </select></label>
        <input type='text' name='note' value='${cleanNote(lnote).replace(/"/g,"&quot;")}' maxlength='64' placeholder='Add/Edit note'>
        <button type='submit'>Save</button>
        <button type='button' onclick='hideNoteForm("${noteFormId}")'>Cancel</button>
        <input type='hidden' name='date' value='${ldate}'>
        <input type='hidden' name='time' value='${ltime}'>
        <input type='hidden' name='asset' value='${lasset}'>
      </form>
    `;

    if (!isMobile) {
      let tr = document.createElement('tr');
      function td(txt) { let tdEl=document.createElement('td'); tdEl.innerHTML=txt; return tdEl; } // Corrected 'td' var name
      tr.appendChild(td(ldate));
      tr.appendChild(td(ltime));
      tr.appendChild(td(lasset));
      tr.appendChild(td(levent)); // START or STOP
      tr.appendChild(td(lstateVal=="1" ? "<span style='color:#256029;font-weight:bold;'>RUNNING</span>" : "<span style='color:#b71c1c;font-weight:bold;'>STOPPED</span>"));
      tr.appendChild(td(Number(lavail).toFixed(2)));
      tr.appendChild(td(minToHHMMSS(lrun))); // Use minToHHMMSS for consistency
      tr.appendChild(td(minToHHMMSS(lstop)));
      tr.appendChild(td(minToHHMMSS(lmtbf)));
      tr.appendChild(td(minToHHMMSS(lmttr)));
      tr.appendChild(td(String(stopsInt))); // Ensure it's a string
      tr.appendChild(td(levent.toUpperCase()==="STOP" ? durationStrToHHMMSS(runDurStr) : "")); // Show RunDur for STOP events
      tr.appendChild(td(levent.toUpperCase()==="START" ? durationStrToHHMMSS(stopDurStr) : ""));// Show StopDur for START events
      
      let tdNote = document.createElement('td');
      tdNote.innerHTML = `<span class='note'>${cleanNote(lnote)}</span>`;
      // Show Edit button only for STOP events (as per original logic, assuming notes are for downtime)
      // Or adjust if notes can be for START events too. Let's assume for STOP events.
      if (levent.toUpperCase() === "STOP") { 
        tdNote.innerHTML += ` <button class='notebtn' onclick='showNoteForm("${noteFormId}")'>Edit</button>`;
        tdNote.innerHTML += noteFormHtml;
      }
      tr.appendChild(tdNote);
      tbody.appendChild(tr);
    } else { // Mobile card view
      let card = document.createElement('div');
      card.className = 'eventCard';
      card.innerHTML =
        `<div><b>Date:</b> ${ldate}</div>
        <div><b>Time:</b> ${ltime}</div>
        <div><b>Asset:</b> ${lasset}</div>
        <div><b>Event:</b> ${levent}</div>
        <div><b>State:</b> ${(lstateVal == "1"
          ? "<span style='color:#256029;font-weight:bold;'>RUNNING</span>"
          : "<span style='color:#b71c1c;font-weight:bold;'>STOPPED</span>")}</div>
        <div><b>Avail(%):</b> ${Number(lavail).toFixed(2)}</div>
        <div><b>Runtime:</b> ${minToHHMMSS(lrun)}</div>
        <div><b>Downtime:</b> ${minToHHMMSS(lstop)}</div>
        <div><b>MTBF:</b> ${minToHHMMSS(lmtbf)}</div>
        <div><b>MTTR:</b> ${minToHHMMSS(lmttr)}</div>
        <div><b>Stops:</b> ${stopsInt}</div>
        <div><b>Run Duration:</b> ${(levent.toUpperCase()==="STOP"? durationStrToHHMMSS(runDurStr) : "")}</div>
        <div><b>Stop Duration:</b> ${(levent.toUpperCase()==="START"? durationStrToHHMMSS(stopDurStr) : "")}</div>
        <div><b>Note:</b> <span class='note'>${cleanNote(lnote)}</span>
        ${(levent.toUpperCase() === "STOP" ? ` <button class='notebtn' onclick='showNoteForm("${noteFormId}")'>Edit</button>` : "")}
        ${levent.toUpperCase() === "STOP" ? noteFormHtml : ""}</div>`;
      mobileDiv.appendChild(card);
    }
  }
  document.getElementById('eventCount').innerHTML = "<b>Total Events Displayed:</b> " + (isMobile ? mobileDiv.children.length : tbody.children.length);
  document.getElementById('eventTable').style.display = isMobile ? 'none' : '';
  mobileDiv.style.display = isMobile ? '' : 'none';

  if (window.openNoteFormId) showNoteForm(window.openNoteFormId);

  if (!scrollInhibit) {
    if (isMobile) {
      let mobDiv = document.getElementById('mobileEvents');
      if (mobDiv) mobDiv.scrollTop = 0;
    } else {
      // For desktop, scrolling the table container might be better if it's the scrollable part
      // let tableContainer = document.querySelector('#eventTable').parentElement; // Assuming table is in a scrollable div
      // if (tableContainer) tableContainer.scrollTop = 0;
      window.scrollTo({top:0, behavior:'auto'}); // 'auto' for instant scroll
    }
  }
}
function showNoteForm(noteFormId) {
  document.querySelectorAll('.noteform').forEach(f => f.style.display='none');
  let form = document.getElementById(noteFormId);
  if (form) { form.style.display = 'flex'; window.openNoteFormId = noteFormId; }
  stopAutoRefresh(); // Pause refresh when editing a note
}
function hideNoteForm(noteFormId) {
  let form = document.getElementById(noteFormId);
  if (form) form.style.display = 'none';
  window.openNoteFormId = null;
  startAutoRefresh(); // Resume refresh
}
function submitNote(e, ldate, ltime, lasset) {
  e.preventDefault();
  let form = e.target;
  let fd = new FormData(form);
  let params = new URLSearchParams();
  for (const pair of fd.entries()) params.append(pair[0], pair[1]);
  
  fetch('/api/note', {
    method: 'POST',
    headers: {'Content-Type':'application/x-www-form-urlencoded'},
    body: params.toString()
  }).then(r => {
    if (r.ok) {
        // Optimistically update or just re-fetch. Re-fetching is simpler.
        fetchAndRenderEvents(); 
    } else {
        alert("Failed to save note.");
    }
  }).catch(err => {
    console.error("Error saving note:", err);
    alert("Error saving note.");
  });

  form.style.display = 'none'; // Hide form immediately
  window.openNoteFormId = null;
  startAutoRefresh(); // Resume refresh
  return false; // Prevent default form submission
}
function toggleScrollInhibit(btn) {
  scrollInhibit = !scrollInhibit;
  btn.innerText = scrollInhibit ? "Enable Auto-Scroll" : "Inhibit Auto-Scroll";
}

// Initialize page
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', fetchChannelsAndStart);
} else {
  fetchChannelsAndStart();
}
)rawliteral";
  html += "</script>";
  html += "</head><body>";
  html += "<header>Event Log</header>";
  html += "<nav class='nav'>";
  html += "<a href='/'>Dashboard</a>"; // Changed to <a>
  html += "<a href='/config'>Setup</a>"; // Changed to <a>
  html += "<a href='/export_log'>Export CSV</a>"; // Changed to <a>
  html += "</nav>";
  html += "<div class='main card'>";
  html += "<div class='filterrow'><label for='channelFilter'>Filter by Asset:</label> <select id='channelFilter'><option value='ALL'>All Assets</option></select>"; // Changed label
  html += "<label for='stateFilter'>Event State:</label> <select id='stateFilter'><option value='ALL'>All</option><option value='RUNNING'>Running</option><option value='STOPPED'>Stopped</option></select>";
  html += "<span id='eventCount' style='margin-left:1em;'></span>";
  html += "<button class='scrollToggle' id='scrollBtn' type='button' onclick='toggleScrollInhibit(this)'>Inhibit Auto-Scroll</button></div>";
  html += "<div style='overflow-x:auto;'><table id='eventTable'><thead><tr>"; // Table container
  html += "<th>Date</th><th>Time</th><th>Asset</th><th>Event</th><th>State</th><th>Avail(%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Run Duration</th><th>Stop Duration</th><th>Note</th>"; // Table Headers
  html += "</tr></thead><tbody id='tbody'></tbody></table>";
  html += "<div id='mobileEvents'></div>"; // Container for mobile cards
  html += "</div></div></body></html>";
  return html;
}

// --- htmlConfig() function ---
String htmlConfig() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Setup</title><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001;font-size:1.6em;font-weight:700;}"; // Styled header
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem auto;flex-wrap:wrap;align-items:center;max-width:700px;}"; // Centered nav, max-width
  html += ".nav button, .nav a {text-decoration:none;background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;margin-bottom:0.5em;}"; // Combined button and <a>
  html += ".nav button:hover, .nav a:hover {background:#e3f0fc;}";
  html += ".nav .right{margin-left:auto;}"; // For right-aligning specific items if needed
  html += ".main{max-width:700px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += "label{font-weight:500;margin-top:1em;display:block;}input[type=text],input[type=number],select{width:100%;padding:0.6em;margin-top:0.2em;margin-bottom:1em;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;}"; // Added box-sizing
  html += "input[type=submit],button.form-button{width:100%;margin-top:1em;padding:0.8em 1.5em;font-size:1.15em;border-radius:8px;border:none;background:#1976d2;color:#fff;font-weight:700;cursor:pointer;}"; // Generic form button class
  html += ".notice{background:#e6fbe7;color:#256029;font-weight:bold;padding:0.6em 1em;border-radius:7px;margin-bottom:1em;text-align:center;}"; // Centered notice
  html += "button.wifi-reconfig{background:#f44336 !important; color:#fff !important;}"; 
  html += ".config-tile { margin-bottom: 1rem; border: 1px solid #e0e0e0; border-radius: 8px; overflow: hidden; }";
  html += ".config-tile-header { background-color: #e9eff4; color: #1976d2; padding: 0.8em 1em; width: 100%; border: none; text-align: left; font-size: 1.1em; font-weight: 700; cursor: pointer; display:flex; justify-content:space-between; align-items:center;}"; // Flex for icon alignment
  html += ".config-tile-header:hover { background-color: #dce7f0; }";
  html += ".config-tile-header .toggle-icon { font-size: 1.2em; transition: transform 0.2s; }"; // Removed margin-left
  html += ".config-tile-header.active .toggle-icon { transform: rotate(45deg); }";
  html += ".config-tile-content { padding: 0 1.3rem 1.3rem 1.3rem; display: none; background-color: #fff; border-top: 1px solid #e0e0e0;}";
  html += ".config-tile-content.open { display: block; }";
  html += ".config-tile-content fieldset { border:1px solid #e0e0e0;padding:1em;border-radius:7px;margin-top:1em;margin-bottom:0.5em; }";
  html += ".config-tile-content fieldset legend { font-weight:700;color:#2196f3;font-size:1.05em;padding:0 0.5em; }";
  html += "@media(max-width:700px){.main{padding:0.5rem;} .card{padding:0.7rem;} input[type=submit],button.form-button{font-size:1em;} .config-tile-content{padding:0 0.7rem 0.7rem 0.7rem;}}";
  html += "</style>";
  html += "<script>";
  html += "function clearLogDblConfirm(e){ if(!confirm('Are you sure you want to CLEAR ALL LOG DATA?')){e.preventDefault();return false;} if(!confirm('Double check: This cannot be undone! Are you REALLY sure?')){e.preventDefault();return false;} return true; }"; // Simplified confirm
  html += "function showSavedMsg(){ const notice = document.getElementById('saveNotice'); if(notice) notice.style.display='block'; }"; // Check if notice exists
  html += "function confirmWiFiReconfig(e){ if(!confirm('Are you sure you want to enter WiFi setup mode? The device will disconnect from the current network and restart as an Access Point.')){e.preventDefault();return false;} return true;}"; // Simplified confirm
  html += "function setupConfigTiles() {";
  html += "  document.querySelectorAll('.config-tile-header').forEach(header => {";
  html += "    header.addEventListener('click', () => {";
  html += "      const content = header.nextElementSibling;";
  html += "      const icon = header.querySelector('.toggle-icon');";
  html += "      header.classList.toggle('active');";
  html += "      if (content.classList.contains('open')) {";
  html += "        content.classList.remove('open');";
  html += "        if(icon) icon.textContent = '+';";
  html += "      } else {";
  html += "        content.classList.add('open');";
  html += "        if(icon) icon.textContent = '-';";
  html += "      }";
  html += "    });";
  html += "  });";
  html += "  const firstTileHeader = document.querySelector('.config-tile:first-child .config-tile-header');"; // Auto-open first tile
  html += "  if (firstTileHeader) {";
  html += "    firstTileHeader.click();"; // Simulate click to open and set icon
  html += "  }";
  html += "}";
  html += "document.addEventListener('DOMContentLoaded', setupConfigTiles);";
  html += "</script>";
  html += "</head><body>";
  html += "<header>Setup</header>"; // Simpler header text
  html += "<nav class='nav'>";
  html += "<a href='/'>Dashboard</a>"; // Changed to <a>
  html += "<a href='/events'>Event Log</a>"; // Changed to <a>
  html += "<a href='/export_log'>Export CSV</a>"; // Changed to <a>
  html += "<form action='/clear_log' method='POST' style='display:inline;margin:0;' onsubmit='return clearLogDblConfirm(event);'><button type='submit' style='background:#f44336;color:#fff;' class='right'>Clear Log Data</button></form>"; // Removed extra margin
  html += "</nav>";
  html += "<div class='main'><div class='card'>"; 

  html += "<form method='POST' action='/save_config' id='setupform' onsubmit='setTimeout(showSavedMsg, 500);'>"; // Shorter delay for notice
  html += "<div id='saveNotice' class='notice' style='display:none;'>Settings saved! Device is rebooting...</div>"; // Updated notice text

  // Tile 1: Asset Setup
  html += "<div class='config-tile'>";
  html += "  <button type='button' class='config-tile-header'>Asset Setup <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  html += "    <label>Asset count (max " + String(MAX_ASSETS) + "): <input type='number' name='assetCount' min='1' max='" + String(MAX_ASSETS) + "' value='" + String(config.assetCount) + "' required></label>";
  html += "    <p style='font-size:0.9em; color:#555; margin-top:-0.5em; margin-bottom:1em;'>To change the number of assets, update this count and click 'Save All Settings & Reboot'. The page will refresh with the new number of asset fields after reboot.</p>";
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) break; // Safety break
    html += "    <fieldset><legend>Asset #" + String(i+1) + "</legend>";
    html += "      <label>Name: <input type='text' name='name" + String(i) + "' value='" + String(config.assets[i].name) + "' maxlength='31' required></label>";
    html += "      <label>GPIO Pin: <input type='number' name='pin" + String(i) + "' value='" + String(config.assets[i].pin) + "' min='0' max='39' required></label>"; // Max GPIO for ESP32
    html += "    </fieldset>";
  }
  html += "  </div>"; 
  html += "</div>"; 

  // Tile 2: Operational Settings
  html += "<div class='config-tile'>";
  html += "  <button type='button' class='config-tile-header'>Operational Settings <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  html += "    <label>Max events per asset (log size): <input type='number' name='maxEvents' min='100' max='5000' value='" + String(config.maxEvents) + "' required></label>";
  html += "    <label>Timezone offset from UTC (hours): <input type='number' name='tzOffset' min='-12' max='14' step='0.5' value='" + String(config.tzOffset / 3600.0, 1) + "' required></label>";
  html += "    <label>Highlight stops longer than (min): <input type='number' name='longStopThreshold' min='1' max='1440' value='" + String(config.longStopThresholdSec/60) + "' required></label>"; 
  html += "  </div>"; 
  html += "</div>"; 
  
  // Tile 3: Downtime Reasons
  html += "<div class='config-tile'>";
  html += "  <button type='button' class='config-tile-header'>Downtime Quick Reasons <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  for (int i = 0; i < 5; ++i) { // Assuming 5 reasons
    html += "    <label>Reason " + String(i+1) + ": <input type='text' name='reason" + String(i) + "' value='" + String(config.downtimeReasons[i]) + "' maxlength='31'></label>";
  }
  html += "  </div>"; 
  html += "</div>"; 

  html += "<input type='submit' value='Save All Settings & Reboot' class='form-button'>"; // Used class for styling
  html += "</form>"; 

  // Tile 4: Network Configuration
  html += "<div class='config-tile' style='margin-top:1.5rem;'>";
  html += "  <button type='button' class='config-tile-header'>Network Configuration <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  html += "    <p style='margin-top:0; margin-bottom:0.5em;'><strong>Current WiFi Status:</strong> ";
  if (WiFi.status() == WL_CONNECTED) {
    html += "Connected to " + WiFi.SSID();
    html += " (IP: " + WiFi.localIP().toString() + ")";
  } else if (WiFi.getMode() == WIFI_AP) {
    html += "Currently in Access Point Mode ('AssetMonitor_Config')";
  } else {
    html += "Not Connected / Status Unknown";
  }
  html += "</p>";
  html += "    <p style='margin-top:0.5em; margin-bottom:1em;'>If you need to connect to a different WiFi network or re-enter credentials, use the button below. The device will restart in WiFi Setup Mode where you can enter new details.</p>";
  // Changed to a GET link for simplicity, matching the setup() route for /reconfigure_wifi
  html += "    <a href='/reconfigure_wifi' class='form-button wifi-reconfig' onclick='return confirmWiFiReconfig(event);' style='text-decoration:none;display:block;text-align:center;'>Enter WiFi Setup Mode</a>";
  html += "  </div>"; 
  html += "</div>"; 

  html += "</div></div></body></html>"; 
  return html;
}

// --- handleWiFiReconfigurePost() ---
// This function is called when the "Enter WiFi Setup Mode" button is clicked (if it's a POST)
// Or can be triggered by a GET request to /reconfigure_wifi as well.
void handleWiFiReconfigurePost() {
  // This function will now be responsible for initiating the config portal.
  // It should ideally clear existing WiFi credentials from Preferences first.
  Serial.println("handleWiFiReconfigurePost: Clearing WiFi credentials and starting Config Portal.");
  
  Preferences localPrefs;
  if (localPrefs.begin("assetmon", false)) { // Open for read/write
    localPrefs.remove("ssid");
    localPrefs.remove("pass");
    localPrefs.end();
    Serial.println("WiFi credentials cleared from Preferences.");
  } else {
    Serial.println("Failed to open preferences to clear WiFi credentials.");
  }

  String message = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>WiFi Reconfiguration</title>";
  message += "<style>body{font-family: Arial, sans-serif; margin: 20px; padding: 15px; border:1px solid #ddd; border-radius:5px; text-align:center;} h2{color:#333;}</style>";
  message += "</head><body>";
  message += "<h2>Device Restarting for WiFi Setup</h2>"; // Updated message
  message += "<p>The device will now restart and then create an Access Point named '<strong>AssetMonitor_Config</strong>'.</p>";
  message += "<p>Please connect your computer or phone to that WiFi network.</p>";
  message += "<p>Then, open a web browser and go to <strong>192.168.4.1</strong> to configure the new WiFi settings.</p>";
  message += "<p>The device will restart again after you save the new settings from that page.</p>";
  message += "<p>This page will attempt to redirect shortly...</p>";
  message += "<meta http-equiv='refresh' content='7;url=http://192.168.4.1/' />"; // Try to redirect after a delay
  message += "</body></html>";
  
  server.sendHeader("Connection", "close"); 
  server.send(200, "text/html", message);
  
  delay(1000); // Allow time for the message to be sent
  ESP.restart(); // Restart the ESP. On next boot, setupWiFiSmart will see no creds and start AP.
  // startConfigPortal(); // Calling this directly might not work as expected if client is still connected or if ESP needs full reboot.
                       // A full reboot ensures a clean start for AP mode.
}

// --- ASSET DETAIL PAGE ---
String htmlAssetDetail(uint8_t idx) {
  if (idx >= config.assetCount || idx >= MAX_ASSETS) return "Invalid Asset Index"; // Added MAX_ASSETS check
  String html = "<!DOCTYPE html><html><head><title>Asset Detail: ";
  html += String(config.assets[idx].name) + "</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:Roboto,Arial,sans-serif;margin:2em;background:#f3f7fa;} .card{background:#fff;padding:1.5em;border-radius:8px;box-shadow:0 2px 10px #0001;} a{color:#1976d2;text-decoration:none;} a:hover{text-decoration:underline;}</style>";
  html += "</head><body><div class='card'>";
  html += "<h1>Asset Detail: " + String(config.assets[idx].name) + "</h1>";
  html += "<p><strong>GPIO Pin:</strong> " + String(config.assets[idx].pin) + "</p>";
  // Add more details here if needed, e.g., current state, recent events for this asset
  html += "<p><a href='/'>Back to Dashboard</a></p>";
  html += "<p><a href='/analytics?asset=" + urlEncode(config.assets[idx].name) + "'>View Analytics for this Asset</a></p>";
  html += "</div></body></html>";
  return html;
}


// --- handleConfigPost() --- (Main configuration save)
void handleConfigPost() {
  if (server.hasArg("assetCount")) {
    uint8_t oldAssetCount = config.assetCount; 
    int submittedAssetCount = server.arg("assetCount").toInt();

    config.assetCount = constrain(submittedAssetCount, 1, MAX_ASSETS);

    if (config.assetCount > oldAssetCount) {
      for (uint8_t i = oldAssetCount; i < config.assetCount; ++i) {
        if (i < MAX_ASSETS) { 
          strcpy(config.assets[i].name, ""); 
          config.assets[i].pin = 0;          
        }
      }
    } // Note: Does not handle decreasing assetCount (data for removed assets remains in struct but isn't used)

    for (uint8_t i = 0; i < config.assetCount; ++i) {
      if (i < MAX_ASSETS) { 
        String nameKey = "name" + String(i);
        String pinKey = "pin" + String(i);
        if (server.hasArg(nameKey)) {
          String val = server.arg(nameKey);
          strncpy(config.assets[i].name, val.c_str(), sizeof(config.assets[i].name) - 1); 
          config.assets[i].name[sizeof(config.assets[i].name)-1] = '\0'; 
        }
        if (server.hasArg(pinKey)) { 
          config.assets[i].pin = server.arg(pinKey).toInt(); 
        }
      }
    }

    if (server.hasArg("maxEvents")) {
        config.maxEvents = constrain(server.arg("maxEvents").toInt(), 100, 5000);
    }
    if (server.hasArg("tzOffset")) {
        float offsetHours = server.arg("tzOffset").toFloat();
        config.tzOffset = static_cast<int>(offsetHours * 3600); // Convert hours to seconds
        config.tzOffset = constrain(config.tzOffset, -12 * 3600, 14 * 3600);
    }
    for (int i = 0; i < 5; ++i) { // Assuming 5 downtime reasons
      String key = "reason" + String(i);
      if (server.hasArg(key)) {
        String v = server.arg(key);
        strncpy(config.downtimeReasons[i], v.c_str(), sizeof(config.downtimeReasons[i]) - 1);
        config.downtimeReasons[i][sizeof(config.downtimeReasons[i])-1] = '\0';
      }
    }
    if (server.hasArg("longStopThreshold")) { // Assuming value from form is in minutes
      config.longStopThresholdSec = constrain(server.arg("longStopThreshold").toInt() * 60, 60, 3600 * 24); // Convert mins to secs
    }

    saveConfig(); 
    
    server.sendHeader("Location", "/config#notice"); // Redirect to config page, potentially to an anchor
    server.send(303);
    
    // Ensure client connection is closed before restarting to avoid issues
    if(server.client().connected()) {
        server.client().stop();
    }
    delay(1000); // Give time for response to send and client to disconnect
    
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request: Missing assetCount or other required fields");
  }
}

// --- Other Handlers ---
void handleClearLog() { 
  if (SPIFFS.remove(LOG_FILENAME)) {
    Serial.println("Log file cleared.");
  } else {
    Serial.println("Failed to clear log file.");
  }
  // Re-initialize asset states related to log calculations if necessary,
  // or simply let them continue from current operational data.
  // For simplicity, just redirect. New logs will start fresh.
  server.sendHeader("Location", "/config"); 
  server.send(303); 
}

void handleExportLog() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f || f.size() == 0) { // Check if file exists and is not empty
    server.send(404, "text/plain", "No log file or log is empty."); 
    if(f) f.close();
    return; 
  }
  
  time_t now_time = time(nullptr); // Renamed to avoid conflict with 'now' in other scopes
  struct tm * ti = localtime(&now_time);
  char fn[64];
  strftime(fn, sizeof(fn), "AssetMonitorLog-%Y%m%d-%H%M%S.csv", ti); // More descriptive filename
  
  // Send headers first to indicate a download
  server.sendHeader("Content-Type", "text/csv"); // Set correct MIME type
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + fn + "\"");
  
  // Start streaming the file content
  server.setContentLength(CONTENT_LENGTH_UNKNOWN); // Use chunked encoding if size is large or unknown
  server.send(200, "text/csv", ""); // Send headers with empty body to start

  // Send the header row for the CSV
  server.sendContent("Date,Time,Asset,Event,State,Availability (%),Total Runtime (min),Total Downtime (min),MTBF (min),MTTR (min),No. of Stops,Run Duration (mm:ss),Stop Duration (mm:ss),Note\n");
  
  // Send file content in chunks
  const size_t chunkSize = 1024;
  char buffer[chunkSize];
  while (f.available()) {
    size_t bytesRead = f.readBytes(buffer, chunkSize);
    if (bytesRead > 0) {
      server.sendContent(String(buffer).substring(0, bytesRead)); // Send only actual bytes read
    }
  }
  f.close();
  server.sendContent(""); // Finalize the chunked response
  Serial.println("Log file exported.");
}

// --- API HANDLERS ---
void handleApiSummary() {
  String json = "{\"assets\":[";
  time_t current_time_epoch = time(nullptr); // Get current time once

  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) continue;
    if (i > 0) json += ",";

    AssetState& as = assetStates[i];
    bool currentPinState = digitalRead(config.assets[i].pin); // Read pin state once

    // Calculate current running/stopped time including the ongoing period
    unsigned long currentPeriodRunningTime = as.runningTime;
    unsigned long currentPeriodStoppedTime = as.stoppedTime;

    if (as.lastState) { // Was running
      currentPeriodRunningTime += (current_time_epoch - as.lastChangeTime);
    } else { // Was stopped
      currentPeriodStoppedTime += (current_time_epoch - as.lastChangeTime);
    }

    float avail = (currentPeriodRunningTime + currentPeriodStoppedTime) > 0 ? (100.0 * currentPeriodRunningTime / (currentPeriodRunningTime + currentPeriodStoppedTime)) : 0;
    float total_runtime_min = currentPeriodRunningTime / 60.0;
    float total_downtime_min = currentPeriodStoppedTime / 60.0;
    
    // MTBF/MTTR based on completed cycles (stopCount)
    float mtbf_val = (as.stopCount > 0) ? (float)as.runningTime / as.stopCount / 60.0 : 0; // Use cumulative completed running time
    float mttr_val = (as.stopCount > 0) ? (float)as.stoppedTime / as.stopCount / 60.0 : 0; // Use cumulative completed stopped time

    json += "{";
    json += "\"name\":\"" + String(config.assets[i].name) + "\",";
    json += "\"pin\":" + String(config.assets[i].pin) + ",";
    json += "\"state\":" + String(currentPinState ? 1 : 0) + ","; // Current actual pin state
    json += "\"availability\":" + String(avail, 2) + ",";
    json += "\"total_runtime\":" + String(total_runtime_min, 2) + ","; // Reflects up-to-the-second
    json += "\"total_downtime\":" + String(total_downtime_min, 2) + ","; // Reflects up-to-the-second
    json += "\"mtbf\":" + String(mtbf_val, 2) + ","; // Based on completed cycles
    json += "\"mttr\":" + String(mttr_val, 2) + ","; // Based on completed cycles
    json += "\"stop_count\":" + String(as.stopCount) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleApiEvents() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  String json = "["; // Start of JSON array
  if (f && f.size() > 0) {
    String line;
    bool firstEntry = true;
    while (f.available()) {
      line = f.readStringUntil('\n');
      line.trim(); // Remove any leading/trailing whitespace including \r
      if (line.length() < 5) continue; // Skip empty or very short lines
      
      if (!firstEntry) {
        json += ",";
      }
      firstEntry = false;
      
      // Escape quotes within the CSV line if it's to be a JSON string
      String escapedLine = "";
      for (unsigned int i = 0; i < line.length(); ++i) {
        char c = line.charAt(i);
        if (c == '"') escapedLine += "\\\"";
        else if (c == '\\') escapedLine += "\\\\";
        // Add other escapes if necessary (e.g., \n, \r, \t within fields, though unlikely for this CSV)
        else escapedLine += c;
      }
      json += "\"" + escapedLine + "\"";
    }
    f.close();
  }
  json += "]"; // End of JSON array
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "application/json", json);
}

void handleApiConfig() {
  String json = "{";
  json += "\"assetCount\":" + String(config.assetCount) + ",";
  json += "\"maxEvents\":" + String(config.maxEvents) + ",";
  json += "\"tzOffset\":" + String(config.tzOffset) + ","; // In seconds
  json += "\"assets\":[";
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) continue;
    if (i > 0) json += ",";
    json += "{";
    json += "\"name\":\"" + String(config.assets[i].name) + "\",";
    json += "\"pin\":" + String(config.assets[i].pin);
    json += "}";
  }
  json += "],";
  json += "\"downtimeReasons\":[";
  for (int i = 0; i < 5; ++i) { // Assuming 5 reasons
    if (i > 0) json += ",";
    json += "\"" + String(config.downtimeReasons[i]) + "\"";
  }
  json += "],"; // Added comma
  json += "\"longStopThresholdSec\":" + String(config.longStopThresholdSec);
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiNote() {
  if (server.method() == HTTP_POST && server.hasArg("date") && server.hasArg("time") && server.hasArg("asset")) {
    // Extract all arguments. Note: server.arg() URL decodes by default.
    String dateVal = server.arg("date");
    String timeVal = server.arg("time");
    String assetVal = server.arg("asset");
    String noteVal = server.arg("note"); // Note might contain spaces, special chars
    String reasonVal = server.hasArg("reason") ? server.arg("reason") : "";

    Serial.printf("API Note Received: Date=%s, Time=%s, Asset=%s, Reason=%s, Note=%s\n",
                  dateVal.c_str(), timeVal.c_str(), assetVal.c_str(), reasonVal.c_str(), noteVal.c_str());

    updateEventNote(dateVal, timeVal, assetVal, noteVal, reasonVal);
    
    // It's common for POST APIs to return a success status or the updated resource,
    // rather than redirecting, unless it's a full page form submission.
    // For a pure API, a 200 OK or 204 No Content might be more appropriate.
    // server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Note updated\"}");
    
    // If the client expects a redirect (e.g., if called from a simple HTML form action):
    server.sendHeader("Location", "/events"); // Redirect back to events page
    server.send(303); 
    return;
  }
  server.send(400, "text/plain", "Bad Request: Missing required parameters for note update.");
}


void updateEventNote(String date, String time, String assetName, String note, String reason) {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f) { Serial.println("updateEventNote: Failed to open log file for reading."); return; }
  
  String tempLogContent = "";
  bool updated = false;

  String combinedNewNote = "";
  if (reason.length() > 0 && note.length() > 0) {
    combinedNewNote = reason + " - " + note;
  } else if (reason.length() > 0) {
    combinedNewNote = reason;
  } else {
    combinedNewNote = note;
  }
  // Sanitize combinedNewNote: remove commas to prevent breaking CSV structure, and newlines
  combinedNewNote.replace(",", " "); 
  combinedNewNote.replace("\n", " "); 
  combinedNewNote.replace("\r", " ");


  while (f.available()) {
    String line = f.readStringUntil('\n');
    String originalLine = line; // Keep original line ending if present
    line.trim(); // Work with trimmed line for parsing

    if (line.length() < 5) { // Skip empty or malformed lines
      tempLogContent += originalLine + "\n"; // Preserve original line ending for non-target lines
      continue;
    }

    // Smart split CSV: handles cases where note itself might have had commas (though we try to avoid it)
    // This basic split assumes note is the last field and doesn't contain unescaped commas.
    String parts[14]; // Date,Time,Asset,Event,State,Avail,RunT,DownT,MTBF,MTTR,Stops,RunDur,StopDur,Note
    int partIdx = 0;
    int lastComma = -1;
    for(int i=0; i<13; ++i) { // Read first 13 fields
        int nextComma = line.indexOf(',', lastComma + 1);
        if (nextComma == -1 && i < 12) { // Should find 12 commas for 13 fields
            parts[partIdx++] = line.substring(lastComma + 1); // take rest of line if not enough commas
            for (int j=partIdx; j<13; ++j) parts[j] = ""; // fill rest with empty
            break;
        } else if (nextComma == -1 && i == 12) { // Last field before note
             parts[partIdx++] = line.substring(lastComma + 1);
             break;
        }
        parts[partIdx++] = line.substring(lastComma + 1, nextComma);
        lastComma = nextComma;
    }
    // The rest is the note
    if (lastComma != -1 && lastComma < (int)line.length() - 1) {
        parts[13] = line.substring(lastComma + 1);
    } else if (partIdx == 13) { // If we read 13 fields, the 14th (note) might be empty
        parts[13] = "";
    }


    if (partIdx >= 3 && parts[0] == date && parts[1] == time && parts[2] == assetName) {
      // Found the line to update. Reconstruct it with the new note.
      String updatedLine = "";
      for(int i=0; i<13; ++i) { // First 13 fields
          updatedLine += parts[i] + ",";
      }
      updatedLine += combinedNewNote; // Add the new sanitized note
      tempLogContent += updatedLine + "\n";
      updated = true;
      Serial.println("Found and updated event line.");
    } else {
      tempLogContent += originalLine + "\n"; // Preserve original line ending
    }
  }
  f.close();

  if (updated) {
    File f2 = SPIFFS.open(LOG_FILENAME, FILE_WRITE); // Open in write mode (truncates)
    if (!f2) { Serial.println("updateEventNote: Failed to open log file for writing."); return; }
    f2.print(tempLogContent);
    f2.close();
    Serial.println("Log file rewritten with updated note.");
  } else {
    Serial.println("Event to update not found in log.");
  }
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }