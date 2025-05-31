#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>
#include <ctype.h> 

// Forward declarations
String htmlDashboard();
String htmlAnalytics();
String htmlAnalyticsCompare();
String htmlConfig();
void sendHtmlEventsPage(); 
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
void setupTime(); 
String urlEncode(const String& str); 
String urlDecode(const String& str);

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
  int longStopThresholdSec; 
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
  unsigned long lastRunDuration;   
  unsigned long lastStopDuration;  
};

// Event struct (no changes)
struct Event {
  time_t timestamp; char assetName[32]; char eventType[8]; int state; 
  float availability; float runtime; float downtime; float mtbf; float mttr; 
  unsigned int stops; char runDuration[8]; char stopDuration[8]; char note[64];
  Event() { /* constructor */ 
    timestamp = 0; assetName[0] = '\0'; eventType[0] = '\0'; state = 0;
    availability = 0; runtime = 0; downtime = 0; mtbf = 0; mttr = 0;
    stops = 0; runDuration[0] = '\0'; stopDuration[0] = '\0'; note[0] = '\0';
  }
};

WebServer server(80);
Preferences prefs;
AssetState assetStates[MAX_ASSETS];
char wifi_ssid[33] = "";
char wifi_pass[65] = "";

// wifiConfigHTML, handleWifiConfigPost, startConfigPortal, setupWiFiSmart (no changes)
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
    prefs.begin("assetmon", false);
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
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AssetMonitor_Config", "setpassword");
  Serial.print("Config Portal Started. Connect to AP 'AssetMonitor_Config', IP: "); 
  Serial.println(WiFi.softAPIP());                                                    
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", wifiConfigHTML()); });
  server.on("/wifi_save_config", HTTP_POST, handleWifiConfigPost);
  server.begin();
  while (true) { server.handleClient(); delay(10); }
}

void setupWiFiSmart() {
  prefs.begin("assetmon", true);
  String ssid_from_prefs = prefs.getString("ssid", ""); 
  String pass_from_prefs = prefs.getString("pass", ""); 
  prefs.end();
  if (ssid_from_prefs.length() == 0) { 
    Serial.println("SSID not found. Starting Config Portal."); 
    startConfigPortal(); 
    return; 
  }
  strncpy(wifi_ssid, ssid_from_prefs.c_str(), 32); wifi_ssid[32] = '\0';
  strncpy(wifi_pass, pass_from_prefs.c_str(), 64); wifi_pass[64] = '\0';
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_pass);
  Serial.printf("Connecting to %s", wifi_ssid);
  for (int i=0; i<20 && WiFi.status()!=WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println(); 
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed. Starting config portal."); 
    startConfigPortal();
  }
}


// loadConfig, saveConfig, setupTime (no changes)
void loadConfig() {
  Preferences localPrefs; 
  bool prefsOpenedForRead = localPrefs.begin("assetmon", true); 

  if (!prefsOpenedForRead) {
    Serial.println("loadConfig: Failed to open preferences in read-only, trying read-write.");
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
    if (config.longStopThresholdSec == 0 && config.maxEvents !=0 ) { 
        config.longStopThresholdSec = 5*60; 
        Serial.println("loadConfig: Initialized longStopThresholdSec to default 5 minutes.");
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
    
    if(prefsOpenedForRead) localPrefs.end(); 
    saveConfig(); 
  }
  
  if (prefsOpenedForRead) { 
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
     Serial.println("saveConfig: Configuration saved successfully.");
  } else {
     Serial.println("saveConfig: Error writing configuration to preferences.");
  }
  localSavePrefs.end(); 
}

void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov"); 
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1); 
  tzset(); 

  Serial.print("Waiting for NTP time sync...");
  time_t now_time = time(nullptr); 
  int retry = 0;
  while (now_time < 1000000000 && retry < 60) { 
    delay(500); 
    Serial.print("."); 
    now_time = time(nullptr); 
    retry++; 
  }
  Serial.println(" done");

  if (now_time >= 1000000000) {
    struct tm timeinfo;
    getLocalTime(&timeinfo); 
    Serial.printf("NTP sync successful. Current local time: %s", asctime(&timeinfo)); 
  } else {
    Serial.println("NTP time sync failed. Using system time (if any).");
  }
}


void setup() {
  Serial.begin(115200); 
  Serial.println("\n--- Device Starting ---");

  if (!SPIFFS.begin(true)) { Serial.println("SPIFFS.begin() failed! Halting."); return; }
  Serial.println("SPIFFS initialized.");

  if (!prefs.begin("assetmon", false)) { Serial.println("Global prefs.begin() failed!"); } 
  else { Serial.println("Global Preferences initialized."); }

  loadConfig();
  Serial.println("Configuration loaded/initialized.");

  setupWiFiSmart(); 
  setupTime(); 
  Serial.println("WiFi and Time setup complete.");

  Serial.printf("Initializing %u assets...\n", config.assetCount);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i < MAX_ASSETS) { 
      pinMode(config.assets[i].pin, INPUT_PULLUP);
      assetStates[i].lastState = digitalRead(config.assets[i].pin);
      assetStates[i].lastChangeTime = time(nullptr);
      assetStates[i].sessionStart = assetStates[i].lastChangeTime;
      assetStates[i].runningTime = 0; assetStates[i].stoppedTime = 0;
      assetStates[i].runCount = 0; assetStates[i].stopCount = 0;
      assetStates[i].lastEventTime = 0; 
      assetStates[i].lastRunDuration = 0; assetStates[i].lastStopDuration = 0;
      Serial.printf("Asset %u ('%s', pin %u) init. Pin State: %s\n", i, config.assets[i].name, config.assets[i].pin, assetStates[i].lastState ? "HIGH" : "LOW");
    }
  }
  
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
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Web server started. Device is ready.");
}

// loop, logEvent, formatMMSS, eventToCSV, urlEncode, urlDecode (no changes)
void loop() {
  server.handleClient();
  time_t now = time(nullptr);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) continue; 

    bool current_pin_state = digitalRead(config.assets[i].pin); 
    if (current_pin_state != assetStates[i].lastState) { // State changed
      unsigned long elapsed = now - assetStates[i].lastChangeTime;
      unsigned long runDuration = 0;
      unsigned long stopDuration = 0;

      // Pin state HIGH means machine STOPPED (for INPUT_PULLUP, active LOW sensor)
      // Pin state LOW means machine RUNNING
      if (current_pin_state == true) { // Machine just STOPPED (was running)
        assetStates[i].runningTime += elapsed; 
        assetStates[i].stopCount++;
        runDuration = elapsed; 
        assetStates[i].lastRunDuration = runDuration;
        assetStates[i].lastStopDuration = 0; 
        logEvent(i, false, now, nullptr, runDuration, 0); // false = machine stopped
      } else { // Machine just STARTED (was stopped)
        assetStates[i].stoppedTime += elapsed; 
        assetStates[i].runCount++; // Increment when a run period starts
        stopDuration = elapsed; 
        assetStates[i].lastStopDuration = stopDuration;
        assetStates[i].lastRunDuration = 0; 
        logEvent(i, true, now, nullptr, 0, stopDuration); // true = machine running
      }
      assetStates[i].lastState = current_pin_state; 
      assetStates[i].lastChangeTime = now;
    }
  }
  delay(200); 
}

void logEvent(uint8_t assetIdx, bool machineIsRunning, time_t now, const char* note, unsigned long runDuration, unsigned long stopDuration) {
  if (assetIdx >= MAX_ASSETS) return; 

  AssetState& as = assetStates[assetIdx];
  unsigned long cumulative_runningTime = as.runningTime; // Total before this event's ended period
  unsigned long cumulative_stoppedTime = as.stoppedTime; // Total before this event's ended period
  
  float avail = (cumulative_runningTime + cumulative_stoppedTime) > 0 
                ? (100.0 * cumulative_runningTime / (cumulative_runningTime + cumulative_stoppedTime)) 
                : (machineIsRunning ? 100.0 : 0.0); // If no history, avail is 100 if starting, 0 if stopping
  
  float total_runtime_min = cumulative_runningTime / 60.0;
  float total_downtime_min = cumulative_stoppedTime / 60.0;
  
  // For MTBF/MTTR, use the stop count *after* this event if it's a stop, or current if it's a start.
  // 'as.stopCount' is the count *before* this event's effect.
  // If machineIsRunning is false (STOP event), stopCount used for MTBF will be the new total.
  // If machineIsRunning is true (START event), stopCount for MTTR is the count of completed stops.
  uint32_t stops_for_calc = as.stopCount; // This is the count of *completed* stops.

  float mtbf_val = (stops_for_calc > 0) ? (float)cumulative_runningTime / stops_for_calc / 60.0 : total_runtime_min; 
  float mttr_val = (stops_for_calc > 0) ? (float)cumulative_stoppedTime / stops_for_calc / 60.0 : 0; 

  struct tm * ti = localtime(&now);
  char datebuf[11], timebuf[9];
  strftime(datebuf, sizeof(datebuf), "%d/%m/%Y", ti);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti);

  File f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
  if (!f) { Serial.println("Failed to open log file for writing!"); return; }
  f.printf("%s,%s,%s,%s,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%s,%s,%s\n",
    datebuf, timebuf, config.assets[assetIdx].name,
    machineIsRunning ? "START" : "STOP", // Event Type
    machineIsRunning ? 1 : 0,            // State (1 for running, 0 for stopped)
    avail, total_runtime_min, total_downtime_min, mtbf_val, mttr_val,
    as.stopCount, // Log the stop count at the time of the event
    (runDuration > 0 ? formatMMSS(runDuration).c_str() : ""),   
    (stopDuration > 0 ? formatMMSS(stopDuration).c_str() : ""), 
    note ? note : ""
  );
  f.close();
  as.lastEventTime = now; 
  Serial.printf("Event logged for %s: %s. RunD: %s, StopD: %s. Stops: %u\n", 
    config.assets[assetIdx].name, machineIsRunning ? "START" : "STOP",
    formatMMSS(runDuration).c_str(), formatMMSS(stopDuration).c_str(), as.stopCount
  );
}

String formatMMSS(unsigned long seconds) {
  if (seconds == 0) return ""; 
  unsigned int min_val = seconds / 60; 
  unsigned int sec_val = seconds % 60; 
  char buf[8];
  sprintf(buf, "%02u:%02u", min_val, sec_val);
  return String(buf);
}

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
  String decoded = ""; char temp[] = "0x00"; unsigned int len = str.length(); unsigned int i = 0;
  while (i < len) {
    char c = str.charAt(i);
    if (c == '%') {
      if (i+2 < len) { temp[2] = str.charAt(i+1); temp[3] = str.charAt(i+2); decoded += char(strtol(temp, NULL, 16)); i += 3; } 
      else { i++; }
    } else if (c == '+') { decoded += ' '; i++; } 
    else { decoded += c; i++; }
  }
  return decoded;
}

// htmlDashboard, htmlAnalytics, htmlAnalyticsCompare (no changes from previous version with fixes)
String htmlDashboard() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>";
  html += "body{font-family:Roboto,Arial,sans-serif;background:#f3f7fa;margin:0;padding:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.3rem 0 1.3rem 2rem;text-align:left;font-size:2em;font-weight:700;box-shadow:0 2px 10px #0001;}";
  html += ".nav{display:flex;justify-content:center;align-items:center;gap:1rem;margin:1.5rem 0 1rem 0;flex-wrap:wrap;}";
  html += ".nav .nav-btn{background:#fff;color:#1976d2;border:none;border-radius:8px;padding:0.7em 1.3em;font-size:1.13em;font-weight:700;box-shadow:0 2px 12px #1976d222;cursor:pointer;transition:.2s;text-decoration:none;}"; 
  html += ".nav .nav-btn:hover{background:#e3f0fc;}";
  html += ".main{max-width:1200px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += "#chart-container{width:100%;overflow-x:auto;}";
  html += ".statrow{display:flex;gap:1.5em;flex-wrap:wrap;justify-content:center;margin:2em 0 2em 0;}";
  html += ".stat{flex:1 1 220px;border-radius:10px;padding:1.2em;text-align:left;font-size:1.1em;margin:0.4em 0;box-shadow:0 2px 8px #0001;font-weight:500;background:#f5f7fa;border:2px solid #e0e0e0;min-width:200px;}"; 
  html += ".stat.stopped{background:#ffeaea;border-color:#f44336;}";
  html += ".stat.running{background:#e6fbe7;border-color:#54c27c;}";
  html += "table{width:100%;border-collapse:collapse;font-size:1em;margin-top:2em;}";
  html += "th,td{padding:0.7em 0.5em;text-align:left;border-bottom:1px solid #eee;}";
  html += "th{background:#2196f3;color:#fff;}";
  html += "tr{background:#fcfcfd;} tr:nth-child(even){background:#f3f7fa;}";
  html += "td:last-child .nav-btn{margin:0;}"; 
  html += ".nav-btn{background:#fff;color:#1976d2;border:none;border-radius:8px;padding:0.45em 1.1em;font-size:1em;font-weight:700;box-shadow:0 2px 12px #1976d222;cursor:pointer;margin:0 0.1em;}"; 
  html += ".nav-btn:hover{background:#e3f0fc;}";
  html += "@media (max-width:900px){.main{padding:0.5em;}.statrow{gap:0.5em;}.stat{min-width:150px;max-width:100%;font-size:1em;padding:0.6em;}}";
  html += "@media (max-width:700px){header{font-size:1.3em;padding:1em 0 1em 1em;}.nav{flex-direction:column;align-items:center;margin:1em 0 1em 0;gap:0.4em;}.card{padding:0.7em;}.statrow{gap:0.4em;max-width:100%;}}"; 
  html += "</style>";
  html += "</head><body>";
  html += "<header>Dashboard</header>";
  html += "<nav class='nav'>";
  html += "<form action='/events' style='margin:0;'><button type='submit' class='nav-btn'>Event Log</button></form>";
  html += "<form action='/config' style='margin:0;'><button type='submit' class='nav-btn'>Setup</button></form>";
  html += "<a href='/analytics-compare' class='nav-btn'>Compare Assets</a>";
  html += "<form action='/export_log' style='margin:0;'><button type='submit' class='nav-btn'>Export CSV</button></form>";
  html += "</nav>";
  html += "<div class='main'>";
  html += "<div class='card'>";
  html += "<div id='chart-container'><canvas id='barChart' height='200'></canvas></div>";
  html += "<div class='statrow' id='statrow'></div>";
  html += "<div style='overflow-x:auto;'><table id='summaryTable'><thead><tr>";
  html += "<th>Name</th><th>State</th><th>Avail (%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Actions</th></tr></thead><tbody></tbody></table>"; 
  html += "</div></div></div>"; 
  html += "<script>";
  html += R"rawliteral(
function formatHHMMSS(val) {
  if (isNaN(val) || val < 0.01) return "00:00:00";
  let totalSeconds = Math.round(val * 60);
  let h = Math.floor(totalSeconds / 3600);
  let m = Math.floor((totalSeconds % 3600) / 60) % 60; 
  let s = totalSeconds % 60;
  return (h < 10 ? "0" : "") + h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
}
let chartObj=null;
function updateDashboard() {
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    let tbody = document.querySelector('#summaryTable tbody');
    if (!tbody) return; 
    let assets = data.assets;
    let rows = tbody.rows;
    let statrow = document.getElementById('statrow');
    if(statrow) statrow.innerHTML = ''; 
    
    let n = assets.length;
    for(let i=0;i<n;++i){
      let asset = assets[i];
      let stateClass = asset.state==1 ? "running" : "stopped"; 
      let row = rows[i];
      if (!row) {
        row = tbody.insertRow();
        for (let j=0;j<9;++j) row.insertCell(); 
      }
      let assetNameEncoded = encodeURIComponent(asset.name);
      let v0 = asset.name,
          v1 = `<span style="color:${asset.state==1?'#256029':'#b71c1c'};font-weight:bold">${asset.state==1?'RUNNING':'STOPPED'}</span>`,
          v2 = asset.availability.toFixed(2),
          v3 = formatHHMMSS(asset.total_runtime),
          v4 = formatHHMMSS(asset.total_downtime),
          v5 = formatHHMMSS(asset.mtbf),
          v6 = formatHHMMSS(asset.mttr),
          v7 = asset.stop_count,
          v8 = `<form action='/analytics' method='GET' style='display:inline; margin:0;'><input type='hidden' name='asset' value="${assetNameEncoded}"><button type='submit' class='nav-btn'>Analytics</button></form>`;
      let vals = [v0,v1,v2,v3,v4,v5,v6,v7,v8];
      for(let j=0;j<9;++j) row.cells[j].innerHTML = vals[j];
      
      if(statrow) {
        let statHtml = `<div class='stat ${stateClass}'><b>${asset.name}</b><br>Avail: ${asset.availability.toFixed(1)}%<br>Run: ${formatHHMMSS(asset.total_runtime)}<br>Stops: ${asset.stop_count}</div>`;
        statrow.innerHTML += statHtml;
      }
    }
    while (rows.length > n) tbody.deleteRow(rows.length-1); 
    
    let availData=[], names=[], runtimeData=[], downtimeData=[];
    for (let asset of assets) {
      availData.push(asset.availability);
      runtimeData.push(asset.total_runtime);
      downtimeData.push(asset.total_downtime);
      names.push(asset.name);
    }
    let ctxEl = document.getElementById('barChart'); 
    if (!ctxEl) return; 
    let ctx = ctxEl.getContext('2d'); 

    if (!window.chartObj) {
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
    } else {
      window.chartObj.data.labels = names;
      window.chartObj.data.datasets[0].data = availData;
      window.chartObj.data.datasets[1].data = runtimeData;
      window.chartObj.data.datasets[2].data = downtimeData;
      window.chartObj.update();
    }
  }).catch(e => console.error("Dashboard update error:", e)); 
}
if (document.readyState === 'loading') { document.addEventListener('DOMContentLoaded', updateDashboard); } 
else { updateDashboard(); }
setInterval(updateDashboard, 5000);
)rawliteral";
  html += "</script></body></html>";
  return html;
}

String htmlAnalytics() {
  String assetName = server.hasArg("asset") ? urlDecode(server.arg("asset")) : "";
  String html = "<!DOCTYPE html><html lang='en'><head><title>Analytics: ";
  html += assetName + "</title>";
  // ... (rest of htmlAnalytics, assumed unchanged and correct from previous versions)
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}th,td{text-align:left;}header{background:#1976d2;color:#fff;padding:1.3rem 0;text-align:center;box-shadow:0 2px 10px #0001;font-size:1.6em;font-weight:700;}"; 
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;} .nav a{text-decoration:none;background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;} .nav a:hover{background:#e3f0fc;}"; 
  html += ".main{max-width:1100px;margin:1rem auto;padding:1rem;} .metrics{display:flex;flex-wrap:wrap;gap:1em;justify-content:center;margin-bottom:1.5em;} .metric{background:#fff;padding:1em;border-radius:8px;box-shadow:0 2px 8px #0001;text-align:center;flex:1 1 150px;font-size:1.1em;} .metric b{display:block;font-size:1.4em;color:#1976d2;}"; 
  html += ".controls{display:flex;flex-wrap:wrap;gap:1em;align-items:center;margin-bottom:1.5em;padding:1em;background:#fff;border-radius:8px;box-shadow:0 2px 8px #0001;} .controls label{margin-right:0.5em;} .controls input[type=datetime-local]{padding:0.5em;border-radius:4px;border:1px solid #ccc;} .controls .toggle{display:flex;align-items:center;} .controls .toggle input{margin-right:0.3em;} .export-btn{margin-left:auto;padding:0.6em 1em;background:#66bb6a;color:#fff;border:none;border-radius:6px;font-weight:700;cursor:pointer;}"; 
  html += ".chartcard{background:#fff;padding:1.5em;border-radius:8px;box-shadow:0 2px 10px #0001;margin-bottom:1.5em;} .tablecard{background:#fff;padding:1.5em;border-radius:8px;box-shadow:0 2px 10px #0001;} table{width:100%;border-collapse:collapse;} th,td{padding:0.6em;border-bottom:1px solid #eee;} th{background:#e3f0fc;color:#1976d2;}"; 
  html += "@media (max-width:700px){.main{padding:0.5em;} .controls{flex-direction:column;align-items:stretch;} .controls label{width:100%;margin-bottom:0.5em;} .export-btn{margin-left:0;width:100%;text-align:center;} .metrics{gap:0.5em;} .metric{flex-basis:calc(50% - 0.5em);font-size:1em;} .metric b{font-size:1.2em;}}";
  html += "</style>";
  html += "</head><body>";
  html += "<header>Analytics: <span id='assetNameInHeader'>" + assetName + "</span></header>";
  html += "<nav class='nav'>";
  html += "<a href='/'>Dashboard</a>";
  html += "<a href='/events'>Event Log</a>";
  html += "<a href='/analytics-compare' class='analytics-btn'>Compare Assets</a>"; 
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
          "<th>Date</th><th>Time</th><th>Asset</th><th>Event</th><th>Avail(%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Run Duration</th><th>Stop Duration</th><th>Note</th>" 
          "</tr></thead><tbody id='recentEvents'></tbody></table></div>"; 
  html += "<script>";
  html += R"rawliteral(
console.log('Analytics script started (v14 - Enhanced MTBF/MTTR tooltips).');
function floatMinToMMSS(val) { 
  if (typeof val === "string") val = parseFloat(val);
  if (isNaN(val) || val < 0) { 
      let totalSeconds = 0; let m = Math.floor(totalSeconds / 60); let s = totalSeconds % 60;
      return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
  }
  let totalSeconds = Math.round(val * 60);
  if (totalSeconds >= 3600) { 
    let h = Math.floor(totalSeconds / 3600); let rem_secs = totalSeconds % 3600;
    let m = Math.floor(rem_secs / 60); let s = rem_secs % 60;
    return `${h.toString()}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
  } else { 
    let m = Math.floor(totalSeconds / 60); let s = totalSeconds % 60; 
    return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`; 
  }
}
function mmssToSeconds(mmss) { 
  if (!mmss || typeof mmss !== "string") return 0; 
  let parts = mmss.split(":");
  if (parts.length !== 2 && parts.length !==3) return 0; 
  let h=0,m=0,s=0;
  if (parts.length === 3) { h=parseInt(parts[0],10);m=parseInt(parts[1],10);s=parseInt(parts[2],10); } 
  else { m=parseInt(parts[0],10);s=parseInt(parts[1],10); }
  if(isNaN(h)||isNaN(m)||isNaN(s)) return 0; return (h*3600)+(m*60)+s;
}
function parseEventDate(eventRow) {
  if (!eventRow || eventRow.length < 2) return new Date(0); 
  try {
    let [d,m,y]=eventRow[0].split('/').map(Number); let [hh,mm,ss]=eventRow[1].split(':').map(Number);
    if(isNaN(d)||isNaN(m)||isNaN(y)||isNaN(hh)||isNaN(mm)||isNaN(ss)) return new Date(0);
    return new Date(Date.UTC(y,m-1,d,hh,mm,ss));
  } catch(e){console.error('Err parsing date for eventRow:',eventRow,e); return new Date(0);}
}
function toDatetimeLocal(dt) {
  if(!(dt instanceof Date)||isNaN(dt)) dt=new Date(); 
  try {
    const tzo=dt.getTimezoneOffset()*60000; const ld=new Date(dt.getTime()-tzo);
    const pad=n=>n<10?'0'+n:n;
    return ld.getFullYear()+'-'+pad(ld.getMonth()+1)+'-'+pad(ld.getDate())+'T'+pad(ld.getHours())+':'+pad(ld.getMinutes());
  } catch(e){console.error('Err in toDatetimeLocal:',e,'Input date:',dt); const n=new Date(Date.now()-(new Date().getTimezoneOffset()*60000)); return n.toISOString().slice(0,16);}
}
let asset=''; try{asset=decodeURIComponent(new URLSearchParams(window.location.search).get("asset")||""); const el=document.getElementById('assetNameInHeader'); if(el)el.textContent=asset;}catch(e){console.error('Err getting asset from URL:',e);}
let allEvents=[],eventChart=null,filteredEventsGlobal=[]; 
function fetchAnalyticsData(){
  if(!asset){console.warn('No asset specified'); const d=document.getElementById('kpiMetrics'); if(d)d.innerHTML="<div class='metric'>No asset. Add ?asset=YourAssetName</div>"; return;}
  fetch('/api/events').then(r=>{if(!r.ok)throw new Error(`HTTP error ${r.status}`);return r.json();}).then(raw=>{
    if(!Array.isArray(raw)){console.error('Fetched data not array:',raw);allEvents=[];}
    else{allEvents=raw.map(l=>(typeof l==='string')?l.split(','):[]).filter(er=>er.length>13&&er[2]&&er[2].trim()===asset.trim());}
    if(allEvents.length===0){const d=document.getElementById('kpiMetrics');if(d)d.innerHTML=`<div class='metric'>No data for asset: ${asset}</div>`;}
    setupRangePickers();renderEventChart();renderRecentEvents();
  }).catch(e=>{console.error('Err fetching/processing analytics:',e); const d=document.getElementById('kpiMetrics'); if(d)d.innerHTML=`<div class='metric'>Error: ${e.message}</div>`; const cd=document.getElementById('eventChart'); if(cd){const ctx=cd.getContext('2d');if(ctx){ctx.clearRect(0,0,cd.width,cd.height);ctx.textAlign='center';ctx.fillText('Failed to load chart data.',cd.width/2,cd.height/2);}}});
}
function setupRangePickers(){
  let dfd,dtd;
  if(allEvents.length>0){try{let ed=allEvents.map(e=>parseEventDate(e)).filter(d=>d.getTime()!==0);if(ed.length>0){dtd=new Date(Math.max.apply(null,ed));dfd=new Date(dtd.getTime()-12*60*60*1000);}else{dtd=new Date();dfd=new Date(dtd.getTime()-12*60*60*1000);}}catch(e){dtd=new Date();dfd=new Date(dtd.getTime()-12*60*60*1000);}}else{dtd=new Date();dfd=new Date(dtd.getTime()-12*60*60*1000);}
  const ft=document.getElementById('fromTime'); const tt=document.getElementById('toTime');
  if(ft)ft.value=toDatetimeLocal(dfd); if(tt)tt.value=toDatetimeLocal(dtd);
  ['fromTime','toTime','showStart','showStop','showMTBF','showMTTR'].forEach(id=>{const el=document.getElementById(id);if(el)el.onchange=renderEventChart;});
  const eb=document.getElementById('exportPng'); if(eb){eb.onclick=function(){if(!eventChart||!eventChart.canvas){console.warn('Export PNG: Chart not ready');return;}try{let u=eventChart.toBase64Image();let a=document.createElement('a');a.href=u;a.download=`analytics_${asset}.png`;a.click();}catch(e){console.error('Err exporting chart:',e);}};}
}
function renderKPIs(fEvents){
  const d=document.getElementById('kpiMetrics');if(!d)return; if(!fEvents||fEvents.length===0){d.innerHTML="<div class='metric'>No data for range</div>";return;}
  try{const le=fEvents[fEvents.length-1]; const s=le[10]!==undefined?le[10]:'0'; const r=le[6]!==undefined?le[6]:'0'; const dt=le[7]!==undefined?le[7]:'0'; const a=le[5]!==undefined?le[5]:'0'; const bf=le[8]!==undefined?le[8]:'0'; const tr=le[9]!==undefined?le[9]:'0';
  d.innerHTML=`<div class='metric'>Stops: <b>${s}</b></div><div class='metric'>Runtime: <b>${floatMinToMMSS(r)}</b></div><div class='metric'>Downtime: <b>${floatMinToMMSS(dt)}</b></div><div class='metric'>Availability: <b>${parseFloat(a).toFixed(2)}%</b></div><div class='metric'>MTBF: <b>${floatMinToMMSS(bf)}</b></div><div class='metric'>MTTR: <b>${floatMinToMMSS(tr)}</b></div>`;
  }catch(e){console.error('Err rendering KPIs:',e,'Event data:',fEvents.length>0?fEvents[fEvents.length-1]:'No events');d.innerHTML="<div class='metric'>Error KPIs</div>";}
}
function renderEventChart(){
  const cc=document.getElementById('eventChart');if(!cc){console.error("Chart canvas not found");return;}
  if(!allEvents||allEvents.length===0){if(eventChart){eventChart.destroy();eventChart=null;}const ctx=cc.getContext('2d');if(ctx){ctx.clearRect(0,0,cc.width,cc.height);ctx.textAlign='center';ctx.font='16px Roboto';ctx.fillStyle='#555';ctx.fillText('No event data.',cc.width/2,cc.height/2);}renderKPIs([]);return;}
  let fd,td; try{const fv=document.getElementById('fromTime').value;const tv=document.getElementById('toTime').value;if(!fv||!tv){console.error("Date pickers empty");return;}fd=new Date(fv);td=new Date(tv);if(isNaN(fd.getTime())||isNaN(td.getTime())){console.error("Invalid dates");return;}}catch(e){console.error('Err parsing dates:',e);return;}
  const ss=document.getElementById('showStart').checked; const sst=document.getElementById('showStop').checked; const sm=document.getElementById('showMTBF').checked; const str=document.getElementById('showMTTR').checked;
  filteredEventsGlobal=allEvents.filter(er=>{try{const ed=parseEventDate(er);if(ed.getTime()===0)return false;if(ed<fd||ed>td)return false;if(!er[3])return false;const et=er[3].trim().toUpperCase();if(et==="START"&&!ss)return false;if(et==="STOP"&&!sst)return false;return true;}catch(e){console.error("Err filtering event:",er,e);return false;}});
  renderKPIs(filteredEventsGlobal);
  if(filteredEventsGlobal.length===0){if(eventChart){eventChart.destroy();eventChart=null;}const ctx=cc.getContext('2d');if(ctx){ctx.clearRect(0,0,cc.width,cc.height);ctx.textAlign='center';ctx.font='16px Roboto';ctx.fillStyle='#555';ctx.fillText('No data for range.',cc.width/2,cc.height/2);}return;}
  try{let t=filteredEventsGlobal.map(e=>e[1]);let a=filteredEventsGlobal.map(e=>parseFloat(e[5]));let mv=filteredEventsGlobal.map(e=>parseFloat(e[8]));let tv=filteredEventsGlobal.map(e=>parseFloat(e[9]));let sa=filteredEventsGlobal.map(e=>e[4]?e[4].trim():'UNKNOWN');
  let pc=filteredEventsGlobal.map((e,i,arr)=>{const et=e[3]?e[3].trim().toUpperCase():"";let dur="0:0";if(et==="STOP"){if(i+1<arr.length){const ne=arr[i+1];const net=ne[3]?ne[3].trim().toUpperCase():"";if(net==="START"){dur=ne[12]||"0:0";}else{dur=e[12]||"0:0";}}else{dur=e[12]||"0:0";}}if(et==="STOP"&&mmssToSeconds(dur)>=(window.longStopThresholdSec||300))return"#c62828";if(et==="STOP")return"#ff9800";return"#43a047";});
  let ps=filteredEventsGlobal.map((e,i,arr)=>{const et=e[3]?e[3].trim().toUpperCase():"";let dur="0:0";if(et==="STOP"){if(i+1<arr.length){const ne=arr[i+1];const net=ne[3]?ne[3].trim().toUpperCase():"";if(net==="START"){dur=ne[12]||"0:0";}else{dur=e[12]||"0:0";}}else{dur=e[12]||"0:0";}}let ds=7;if(et==="STOP"&&mmssToSeconds(dur)>=(window.longStopThresholdSec||300)){ds=12;}return ds;});
  let ds=[{label:'Availability (%)',data:a,yAxisID:'y',stepped:true,tension:0,pointRadius:ps,pointBackgroundColor:pc,pointBorderColor:pc,showLine:true,segment:{borderColor:ctxS=>{const sv=sa[ctxS.p0DataIndex];if(sv==="1")return"#43a047";if(sv==="0")return"#c62828";return"#000";},borderWidth:3}}];
  if(sm)ds.push({label:'MTBF',data:mv,yAxisID:'y1',borderColor:"#1565c0",borderWidth:2,tension:0,pointRadius:4,fill:false});
  if(str)ds.push({label:'MTTR',data:tv,yAxisID:'y1',borderColor:"#FFD600",borderWidth:2,tension:0,pointRadius:4,fill:false});
  if(eventChart)eventChart.destroy(); const ctxR=cc.getContext('2d');
  eventChart=new Chart(ctxR,{type:'line',data:{labels:t,datasets:ds},options:{responsive:true,maintainAspectRatio:false,interaction:{mode:'nearest',axis:'x',intersect:true},layout:{padding:{top:15}},plugins:{tooltip:{callbacks:{title:(ti)=>{if(!ti.length)return'';const idx=ti[0].dataIndex;if(!filteredEventsGlobal[idx])return'Err: No data';return`Event: ${filteredEventsGlobal[idx][3]} at ${filteredEventsGlobal[idx][0]} ${filteredEventsGlobal[idx][1]}`;},label:(tti)=>{const idx=tti.dataIndex;const er=filteredEventsGlobal[idx];if(!er)return null;const et=er[3]?er[3].trim().toUpperCase():"";const dsl=tti.dataset.label||'';let l=[];if(dsl==='Availability (%)'){const ca=parseFloat(er[5]).toFixed(2);l.push(`Availability: ${ca}%`);if(et==="START"){const sds=mmssToSeconds(er[12]||"0:0");if(sds>0){l.push(`(Prior Stop: ${floatMinToMMSS(sds/60.0)})`);}}else if(et==="STOP"){const rds=mmssToSeconds(er[11]||"0:0");if(rds>0){l.push(`(Prior Run: ${floatMinToMMSS(rds/60.0)})`);}}}else if(dsl==='MTBF'||dsl==='MTTR'){const cv=tti.raw;const fcv=floatMinToMMSS(cv);l.push(`${dsl}: ${fcv}`);if(idx>0){const pv=tti.dataset.data[idx-1];if(typeof pv==='number'&&typeof cv==='number'){const ch=cv-pv;if(Math.abs(ch)>1e-7){const fc=floatMinToMMSS(Math.abs(ch));let ci="";if(dsl==='MTBF'){ci=ch>0?`(Increased by ${fc} - Good)`:`(Decreased by ${fc} - Bad)`;}else{ci=ch>0?`(Increased by ${fc} - Bad)`:`(Decreased by ${fc} - Good)`;}l.push(ci);}else{l.push(`(No significant change)`);}}}else{l.push(`(Initial value)`);}if(dsl==='MTBF'&&et==="STOP"){const lrds=mmssToSeconds(er[11]||"0:0");if(lrds>0){l.push(`(Influenced by last run: ${floatMinToMMSS(lrds/60.0)})`);}}else if(dsl==='MTTR'&&et==="START"){const lsds=mmssToSeconds(er[12]||"0:0");if(lsds>0){l.push(`(Influenced by last stop: ${floatMinToMMSS(lsds/60.0)})`);}}}return l.length>0?l:null;}}},legend:{position:'top'}},scales:{x:{title:{display:true,text:'Time (HH:MM:SS)'}},y:{title:{display:true,text:'Availability (%)'},beginAtZero:true,suggestedMax:105,ticks:{stepSize:20,callback:function(v,i,vs){if(v>100&&v<105)return undefined;if(v===100)return 100;if(v<100&&v>=0&&(v%(this.chart.options.scales.y.ticks.stepSize||20)===0))return v;return undefined;}}},y1:{type:'linear',display:true,position:'right',title:{display:true,text:'MTBF/MTTR'},beginAtZero:true,grid:{drawOnChartArea:false},ticks:{callback:val=>floatMinToMMSS(val)}}}}});
  }catch(e){console.error('Err rendering chart:',e);}
}
function renderRecentEvents(){
  const tb=document.getElementById('recentEvents');if(!tb)return;tb.innerHTML="";if(!allEvents||allEvents.length===0){tb.innerHTML="<tr><td colspan='13'>No event data.</td></tr>";return;}
  const d=allEvents.slice(-10).reverse();if(d.length===0){tb.innerHTML="<tr><td colspan='13'>No recent events.</td></tr>";return;}
  d.forEach(er=>{try{if(er.length<14){let tr=tb.insertRow();let td=tr.insertCell();td.colSpan=13;td.textContent="Malformed.";td.style.color="orange";return;}
  let tr=tb.insertRow(); tr.insertCell().textContent=er[0];tr.insertCell().textContent=er[1];tr.insertCell().textContent=er[2];tr.insertCell().textContent=er[3];tr.insertCell().textContent=parseFloat(er[5]).toFixed(2);tr.insertCell().textContent=floatMinToMMSS(er[6]);tr.insertCell().textContent=floatMinToMMSS(er[7]);tr.insertCell().textContent=floatMinToMMSS(er[8]);tr.insertCell().textContent=floatMinToMMSS(er[9]);tr.insertCell().textContent=er[10];tr.insertCell().textContent=floatMinToMMSS(mmssToSeconds(er[11]||"0:0")/60.0);tr.insertCell().textContent=floatMinToMMSS(mmssToSeconds(er[12]||"0:0")/60.0);tr.insertCell().textContent=er[13]||"";}catch(e){console.error('Err rendering row:',er,e);let tr=tb.insertRow();let td=tr.insertCell();td.colSpan=13;td.textContent="Error row.";td.style.color="red";}});
}
function initAnalyticsPage(){fetch('/api/config').then(r=>{if(!r.ok)throw new Error("Cfg fetch failed:"+r.status);return r.json();}).then(cfg=>{window.longStopThresholdSec=cfg.longStopThresholdSec||300;fetchAnalyticsData();}).catch(e=>{console.error("Failed cfg fetch, default LST.",e);window.longStopThresholdSec=300;fetchAnalyticsData();});}
if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',initAnalyticsPage);}else{initAnalyticsPage();}
)rawliteral";
  html += "</script>";
  html += "</div></body></html>"; 
  return html;
}

String htmlAnalyticsCompare() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Compare Assets</title>";
  // ... (rest of htmlAnalyticsCompare, assumed unchanged and correct from previous versions)
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.3rem 0;text-align:center;box-shadow:0 2px 10px #0001; font-size:1.6em; font-weight:700;}"; 
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;}";
  html += ".nav a, .nav button {text-decoration:none;background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;}";
  html += ".nav a:hover, .nav button:hover {background:#e3f0fc;}";
  html += ".main{max-width:1100px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += ".flexrow{display:flex;flex-wrap:wrap;gap:2em;justify-content:center;}"; 
  html += ".chartcard{flex:1 1 320px;min-width:300px; max-width: calc(50% - 1em);}"; 
  html += ".tablecard{overflow-x:auto;width:100%;}"; 
  html += "th, td { text-align: left !important; padding:0.5em;}"; 
  html += "table{width:100%; border-collapse:collapse;} th{background:#e3f0fc;color:#1976d2;} tr:nth-child(even){background:#f8f9fa;}";
  html += "@media(max-width:700px){.flexrow{flex-direction:column;gap:1em;}.chartcard{max-width:100%;}.card{padding:0.7em;}}";
  html += "</style></head><body>";
  html += "<header>Compare Assets</header>"; 
  html += "<nav class='nav'>";
  html += "<a href='/'>Dashboard</a>"; 
  html += "<a href='/events'>Event Log</a>";
  html += "<a href='/export_log'>Export CSV</a>"; 
  html += "</nav>";
  html += "<div class='main'>";
  html += "<div class='flexrow'>";
  html += "<div class='card chartcard'><canvas id='barAvail'></canvas></div>";
  html += "<div class='card chartcard'><canvas id='barStops'></canvas></div>";
  html += "<div class='card chartcard'><canvas id='barMTBF'></canvas></div>";
  html += "<div class='card chartcard'><canvas id='pieReasons'></canvas></div>";
  html += "</div>";
  html += "<div class='card tablecard'>";
  html += "<h3>Last Event Log Summary</h3><table style='width:100%;'><thead><tr>" 
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
let allEventsCompare = [], allAssetNamesCompare = [], configDowntimeReasonsCompare = []; 
function formatMinutesToHHMMSSCompare(val) { 
  if (isNaN(val) || val <= 0.001) return "0:00:00"; 
  let totalSeconds = Math.round(val * 60);
  let h = Math.floor(totalSeconds / 3600); let m = Math.floor((totalSeconds % 3600) / 60); let s = totalSeconds % 60;
  return h.toString().padStart(1, '0') + ":" + m.toString().padStart(2, '0') + ":" + s.toString().padStart(2, '0'); 
}
function fetchCompareDataPage() { 
  fetch('/api/config').then(r=>r.json()).then(cfg=>{
    configDowntimeReasonsCompare = cfg.downtimeReasons||[]; allAssetNamesCompare = cfg.assets.map(a=>a.name);
    fetch('/api/events').then(r=>r.json()).then(events=>{
      allEventsCompare = events.map(l=>(typeof l === 'string' ? l.split(',') : [])).filter(v=>v.length>13 && allAssetNamesCompare.includes(v[2]));
      renderCompareChartsPage(); renderCompareTablePage();
    }).catch(e=>console.error("CompareEventsFetchErr:", e));
  }).catch(e=>console.error("CompareConfigFetchErr:", e));
}
function getLastMetricCompare(events, idx) { return events && events.length ? parseFloat(events[events.length-1][idx]) : 0; }
function renderCompareChartsPage() { 
  let byAsset = {}; allAssetNamesCompare.forEach(a=>{byAsset[a]=[];});
  for (let e of allEventsCompare) { if(byAsset[e[2]]) byAsset[e[2]].push(e); } 
  let labels = allAssetNamesCompare;
  let avail = labels.map(a=>getLastMetricCompare(byAsset[a]||[],5)); 
  let stops = labels.map(a=>(byAsset[a]||[]).filter(e=>e[3] && e[3].toUpperCase()==="STOP").length); 
  let mtbf = labels.map(a=>getLastMetricCompare(byAsset[a]||[],8));
  let reasons = {}; configDowntimeReasonsCompare.forEach(r => reasons[r] = 0); 
  for (let e of allEventsCompare) {
    if (e.length < 14) continue; let note = e[13]||""; let res = "";
    if (note.indexOf(" - ")>-1) res = note.split(" - ")[0].trim(); else res = note.trim();
    if (res && configDowntimeReasonsCompare.includes(res)) reasons[res] = (reasons[res]||0)+1;
  }
  const pieLabels = Object.keys(reasons).filter(r => reasons[r] > 0);
  const pieData = pieLabels.map(r => reasons[r]);
  const pieColors = ['#ffa726','#ef5350','#66bb6a','#42a5f5','#ab47bc', '#FFEE58', '#26A69A', '#78909C'];
  ['barAvail', 'barStops', 'barMTBF', 'pieReasons'].forEach(id => { const c = document.getElementById(id); if (c && c.chartInstance) c.chartInstance.destroy(); });
  if (document.getElementById('barAvail')) { document.getElementById('barAvail').chartInstance = new Chart(document.getElementById('barAvail').getContext('2d'), { type:'bar',data:{labels:labels,datasets:[{label:'Availability (%)',data:avail,backgroundColor:'#42a5f5'}]}, options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true,max:100, title:{display:true, text:'Availability (%)'}}}} }); }
  if (document.getElementById('barStops')) { document.getElementById('barStops').chartInstance = new Chart(document.getElementById('barStops').getContext('2d'), { type:'bar',data:{labels:labels,datasets:[{label:'Stops',data:stops,backgroundColor:'#ef5350'}]}, options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true, ticks:{stepSize:1}, title:{display:true, text:'Number of Stops'}}}} }); }
  if (document.getElementById('barMTBF')) { document.getElementById('barMTBF').chartInstance = new Chart(document.getElementById('barMTBF').getContext('2d'), { type:'bar',data:{labels:labels,datasets:[{label:'MTBF (min)',data:mtbf,backgroundColor:'#66bb6a'}]}, options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true, title:{display:true, text:'MTBF (minutes)' }}}} }); }
  const pc = document.getElementById('pieReasons');
  if (pc) { if (pieLabels.length > 0) { pc.chartInstance = new Chart(pc.getContext('2d'), { type:'pie',data:{ labels:pieLabels,datasets:[{data:pieData,backgroundColor:pieColors.slice(0, pieLabels.length)}] },options:{responsive:true,maintainAspectRatio:false, plugins:{legend:{position:'right', labels:{boxWidth:15}}, title:{display:true, text:'Downtime Reasons'}}} }); } else { const ctx = pc.getContext('2d'); ctx.clearRect(0,0,ctx.canvas.width, ctx.canvas.height); ctx.textAlign = 'center'; ctx.font = '14px Roboto'; ctx.fillStyle = '#555'; ctx.fillText('No downtime reasons logged.', ctx.canvas.width/2, ctx.canvas.height/2);}}
}
function renderCompareTablePage() { 
  let tb = document.getElementById('compareTable'); if(!tb) return; tb.innerHTML = "";
  let byAsset = {}; allAssetNamesCompare.forEach(a=>{byAsset[a]=[];});
  for (let e of allEventsCompare) { if(byAsset[e[2]]) byAsset[e[2]].push(e); }
  for (let a of allAssetNamesCompare) {
    let evs = byAsset[a]||[], e_last = evs.length?evs[evs.length-1]:null; 
    tb.innerHTML += `<tr><td>${a}</td><td>${e_last&&e_last[5]?parseFloat(e_last[5]).toFixed(2):"-"}</td><td>${e_last&&e_last[6]?formatMinutesToHHMMSSCompare(parseFloat(e_last[6])):"-"}</td><td>${e_last&&e_last[7]?formatMinutesToHHMMSSCompare(parseFloat(e_last[7])):"-"}</td><td>${evs.filter(ev=>ev[3]&&ev[3].toUpperCase()==="STOP").length}</td><td>${e_last&&e_last[8]?formatMinutesToHHMMSSCompare(parseFloat(e_last[8])):"-"}</td><td>${e_last&&e_last[9]?formatMinutesToHHMMSSCompare(parseFloat(e_last[9])):"-"}</td></tr>`;
  }
}
if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',fetchCompareDataPage);}else{fetchCompareDataPage();}
)rawliteral";
  html += "</script>";
  html += "</div></body></html>";
  return html;
}

// CHUNKED SENDING FUNCTION FOR EVENT LOG PAGE
void sendHtmlEventsPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", ""); 

  server.sendContent("<!DOCTYPE html><html lang='en'><head><title>Event Log</title>");
  server.sendContent("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  server.sendContent("<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>");
  server.sendContent("<style>");
  server.sendContent("body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001; font-size:1.6em; font-weight:700;}");
  server.sendContent(".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0 0 0;flex-wrap:wrap;}");
  server.sendContent(".nav button, .nav a {text-decoration:none;background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;transition:.2s;cursor:pointer;margin-bottom:0.5em;}"); 
  server.sendContent(".nav button:hover, .nav a:hover {background:#e3f0fc;}"); 
  server.sendContent(".main{max-width:1300px;margin:1rem auto;padding:1rem;}"); // Increased max-width
  server.sendContent(".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}");
  server.sendContent(".filterrow{display:flex;gap:1em;align-items:center;margin-bottom:1em;flex-wrap:wrap;}");
  server.sendContent(".filterrow label{font-weight:bold; margin-right:0.3em;} .filterrow select, .filterrow input {padding:0.5em; border-radius:4px; border:1px solid #ccc; margin-right:1em;}"); 
  server.sendContent(".scrollToggle{margin-left:auto;font-size:1em; padding: 0.5em 0.8em; border-radius:4px; background-color:#6c757d; color:white; border:none; cursor:pointer;} .scrollToggle:hover{background-color:#5a6268;}"); 
  server.sendContent("table{width:100%;border-collapse:collapse;font-size:1em;margin-top:0.8em;}");
  server.sendContent("th,td{padding:0.7em 0.5em;text-align:left;border-bottom:1px solid #eee;}");
  server.sendContent("th{background:#2196f3;color:#fff;}");
  server.sendContent("tr{background:#fcfcfd;} tr:nth-child(even){background:#f3f7fa;}");
  server.sendContent(".note{font-style:italic;color:#555;white-space:normal;word-break:break-word;display:inline-block;max-width:200px;vertical-align:middle;}"); // Adjusted for inline display
  server.sendContent(".notebtn{padding:4px 8px;font-size:0.9em;border-radius:4px;background:#1976d2;color:#fff;border:none;cursor:pointer;margin-left:5px;white-space:nowrap;vertical-align:middle;} .notebtn:hover{background:#0d47a1;}");
  // Removed .noteform CSS as it's replaced by modal
  server.sendContent("td:last-child{min-width:220px;overflow-wrap:anywhere;word-break:break-word;}"); // Increased min-width for Note column
  
  // Modal CSS
  server.sendContent(".modal-overlay{position:fixed;top:0;left:0;width:100%;height:100%;background-color:rgba(0,0,0,0.5);display:none;justify-content:center;align-items:center;z-index:1000;}");
  server.sendContent(".modal-content{background-color:#fff;padding:20px;border-radius:8px;box-shadow:0 4px 15px rgba(0,0,0,0.2);width:90%;max-width:450px;display:flex;flex-direction:column;}");
  server.sendContent(".modal-title{margin-top:0;color:#1976d2;}");
  server.sendContent(".modal-content label{margin-top:10px;margin-bottom:3px;font-weight:bold;}");
  server.sendContent(".modal-content select, .modal-content input[type=\"text\"]{width:100%;padding:8px;margin-bottom:10px;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;}");
  server.sendContent(".modal-actions{margin-top:15px;display:flex;justify-content:flex-end;gap:10px;}");
  server.sendContent(".modal-actions button{padding:8px 15px;border-radius:4px;border:none;cursor:pointer;font-weight:bold;}");
  server.sendContent(".modal-actions .btn-save{background-color:#1976d2;color:white;}");
  server.sendContent(".modal-actions .btn-cancel{background-color:#6c757d;color:white;}");

  server.sendContent("@media (max-width:700px){");
  server.sendContent("  #eventTable{display:none;}");
  server.sendContent("  .eventCard {background: #fff;border-radius: 10px;box-shadow: 0 2px 10px #0001;margin-bottom: 1.2em;padding: 1em;font-size: 1.05em;}");
  server.sendContent("  .eventCard div {margin-bottom: 0.3em;}");
  server.sendContent("  #mobileEvents {max-height:70vh;overflow-y:auto;}"); 
  server.sendContent("  .filterrow {flex-direction:column; align-items:stretch;} .filterrow label, .filterrow select, .filterrow input {width:100%; margin-bottom:0.5em;} .scrollToggle{margin-left:0; width:100%; text-align:center;}"); 
  server.sendContent("}");
  server.sendContent("@media (min-width:701px){");
  server.sendContent("  #mobileEvents{display:none;}"); 
  server.sendContent("}");
  server.sendContent("</style>");
  server.sendContent("<script>");
  server.sendContent(R"rawliteral(
// JS for Event Log Page - MODAL IMPLEMENTATION
let eventData = []; 
let channelList = []; 
let filterValue = "ALL"; 
let stateFilter = "ALL"; 
window.downtimeReasons = []; 
let scrollInhibit = false; 
window.refreshIntervalId = null;

function startAutoRefresh() {
  if (window.refreshIntervalId) clearInterval(window.refreshIntervalId);
  window.refreshIntervalId = setInterval(fetchAndRenderEvents, 5000); 
}
function stopAutoRefresh() {
  if (window.refreshIntervalId) clearInterval(window.refreshIntervalId);
  window.refreshIntervalId = null;
}

function initializeEventPage() {
  console.log("DOMContentLoaded. Initializing event page (modal version)...");
  let testSel = document.getElementById('channelFilter');
  if (!testSel) {
    console.error("!!! TEST FAIL: 'channelFilter' is NULL after DOMContentLoaded.");
    const mainCard = document.querySelector(".main.card"); 
    if(mainCard) mainCard.innerHTML = "<h1 style='color:red;'>CRITICAL DOM ERROR: 'channelFilter' not found.</h1>";
    else document.body.innerHTML = "<h1 style='color:red;'>CRITICAL DOM ERROR: 'channelFilter' not found.</h1>";
  } else {
    console.log("+++ TEST PASS: 'channelFilter' found. Proceeding.");
    fetchChannelsAndStart(); 
  }
}

function fetchChannelsAndStart() { 
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    channelList = data.assets.map(a=>a.name);
    let sel = document.getElementById('channelFilter');
    if (!sel) { console.error("CRIT: 'channelFilter' null in fetchChannelsAndStart."); return; }
    sel.innerHTML = "<option value='ALL'>All Assets</option>"; 
    channelList.forEach(c => { let o=document.createElement("option"); o.value=c; o.text=c; sel.appendChild(o); });
    sel.onchange = function() { filterValue = sel.value; renderTable(); };
    
    let stateSel = document.getElementById('stateFilter');
    if (!stateSel) { console.error("CRIT: 'stateFilter' null."); return; }
    stateSel.onchange = function() { stateFilter = this.value; renderTable(); };
    
    fetchReasonsAndEvents(); 
  }).catch(e => {
      console.error("Error fetching channels/filters:", e); 
      const mc = document.querySelector(".main.card");
      if(mc) mc.innerHTML = "<p style='color:red;font-weight:bold;'>Error loading filter data.</p>";
  });
}
function fetchReasonsAndEvents() { 
  fetch('/api/config').then(r=>r.json()).then(cfg=>{
    window.downtimeReasons = cfg.downtimeReasons || [];
    fetchAndRenderEvents(); 
    startAutoRefresh(); 
  }).catch(e => console.error("Error fetching config reasons:", e));
}
function fetchAndRenderEvents() { 
  fetch('/api/events').then(r=>r.json()).then(events=>{
    eventData = events; renderTable(); 
  }).catch(e => {
      console.error("Error fetching events:", e);
      const tb = document.getElementById('tbody'); if(tb) tb.innerHTML = "<tr><td colspan='14' style='color:red;text-align:center;'>Failed to load events.</td></tr>";
      const md = document.getElementById('mobileEvents'); if(md) md.innerHTML = "<p style='color:red;text-align:center;'>Failed to load events.</p>";
  });
}
function cleanNote(val) { 
  if (!val) return ""; let v = val.trim();
  if (v === "" || v === "," || v === ",," || v === "0,0," || v === "0.00,0,") return "";
  return v.replace(/^,+|,+$/g, ""); 
}
function minToHHMMSS(valStr) { 
  let val = parseFloat(valStr); if (isNaN(val) || val <= 0.001) return "00:00:00"; 
  let s_total = Math.round(val * 60);
  let h=Math.floor(s_total/3600), m=Math.floor((s_total%3600)/60), s=s_total%60;
  return (h<10?"0":"")+h+":"+(m<10?"0":"")+m+":"+(s<10?"0":"")+s;
}
function durationStrToHHMMSS(str) {
  if (!str || typeof str !== "string" || str.trim()==="") return ""; 
  let p=str.split(":").map(Number); let h=0,m=0,s=0;
  if(p.length===3){[h,m,s]=p;}else if(p.length===2){[m,s]=p;h=Math.floor(m/60);m%=60;}else return "";
  if(isNaN(h)||isNaN(m)||isNaN(s))return ""; 
  return (h<10?"0":"")+h+":"+(m<10?"0":"")+m+":"+(s<10?"0":"")+s;
}

function populateModalReasons() {
  const reasonSelect = document.getElementById('modalNoteReason');
  if (!reasonSelect) return;
  reasonSelect.innerHTML = '<option value=""></option>'; // Clear existing, add blank
  (window.downtimeReasons || []).forEach(r => {
    let opt = document.createElement('option');
    opt.value = r; opt.text = r;
    reasonSelect.appendChild(opt);
  });
}

function showNoteModal(date, time, asset, currentFullNote) {
  stopAutoRefresh();
  const modal = document.getElementById('noteEditModal');
  if (!modal) { console.error("Modal not found!"); return; }

  populateModalReasons();

  document.getElementById('modalNoteDate').value = date;
  document.getElementById('modalNoteTime').value = time;
  document.getElementById('modalNoteAsset').value = asset;

  let currentReason = "";
  let currentTextNote = currentFullNote; 

  if (currentFullNote) {
    if ((window.downtimeReasons || []).includes(currentFullNote)) {
        currentReason = currentFullNote;
        currentTextNote = "";
    } else {
        for (const reason of (window.downtimeReasons || [])) {
            if (currentFullNote.startsWith(reason + " - ")) {
                currentReason = reason;
                currentTextNote = currentFullNote.substring(reason.length + " - ".length);
                break; 
            }
        }
    }
  }
  
  document.getElementById('modalNoteReason').value = currentReason;
  document.getElementById('modalNoteText').value = currentTextNote;
  
  modal.style.display = 'flex';
}

function hideNoteModal() {
  const modal = document.getElementById('noteEditModal');
  if (modal) modal.style.display = 'none';
  startAutoRefresh();
}

function submitModalNote(event) {
  event.preventDefault();
  const form = document.getElementById('modalNoteForm');
  const params = new URLSearchParams();
  params.append('date', form.date.value);
  params.append('time', form.time.value);
  params.append('asset', form.asset.value);
  params.append('reason', form.reason.value); 
  params.append('note', form.note.value); 

  fetch('/api/note', {
    method: 'POST',
    headers: {'Content-Type':'application/x-www-form-urlencoded'},
    body: params.toString()
  }).then(r => {
    if (r.ok) { fetchAndRenderEvents(); } 
    else { alert("Failed to save note. Status: " + r.status); }
  }).catch(err => { console.error("Error saving note:", err); alert("Error saving note."); });
  
  hideNoteModal();
  return false;
}

function renderTable() {
  let tbody = document.getElementById('tbody');
  let mobileDiv = document.getElementById('mobileEvents');
  if(!tbody || !mobileDiv) { console.error("Table/mobile div not found"); return; }

  tbody.innerHTML = ''; mobileDiv.innerHTML = ''; 
  let stateMatcher = (sVal) => (stateFilter==="ALL")||(stateFilter==="RUNNING"&&sVal==="1")||(stateFilter==="STOPPED"&&sVal==="0");
  let isMobile = window.innerWidth <= 700;
  let displayData = eventData.slice().reverse(); 

  for (let i=0; i<displayData.length; ++i) {
    let csvLine = displayData[i];
    if (typeof csvLine !== 'string') continue; 
    let vals = csvLine.split(',');
    if (vals.length < 14) continue; 
    let [ldate,ltime,lasset,levent,lstateVal,lavail,lrun,lstop,lmtbf,lmttr,lsc,runDurStr,stopDurStr] = vals.slice(0,13);
    let lnote = vals.slice(13).join(',').replace(/\n$/, ""); 
    let stopsInt = Math.round(Number(lsc));

    if (filterValue !== "ALL" && lasset !== filterValue) continue;
    if (!stateMatcher(lstateVal)) continue; 
    
    // Prepare note string for passing to JS function (escape quotes)
    let escapedNote = cleanNote(lnote).replace(/"/g, "&quot;").replace(/'/g, "\\'");

    if (!isMobile) {
      let tr = document.createElement('tr');
      function td(txt){let e=document.createElement('td');e.innerHTML=txt;return e;} 
      tr.appendChild(td(ldate)); tr.appendChild(td(ltime)); tr.appendChild(td(lasset));
      tr.appendChild(td(levent)); 
      tr.appendChild(td(lstateVal=="1"?"<span style='color:#256029;font-weight:bold;'>RUNNING</span>":"<span style='color:#b71c1c;font-weight:bold;'>STOPPED</span>"));
      tr.appendChild(td(Number(lavail).toFixed(2)));
      tr.appendChild(td(minToHHMMSS(lrun))); tr.appendChild(td(minToHHMMSS(lstop)));
      tr.appendChild(td(minToHHMMSS(lmtbf))); tr.appendChild(td(minToHHMMSS(lmttr)));
      tr.appendChild(td(String(stopsInt))); 
      tr.appendChild(td(levent.toUpperCase()==="STOP"?durationStrToHHMMSS(runDurStr):"")); 
      tr.appendChild(td(levent.toUpperCase()==="START"?durationStrToHHMMSS(stopDurStr):""));
      let tdNote = document.createElement('td');
      tdNote.innerHTML = `<span class='note'>${cleanNote(lnote)}</span> <button class='notebtn' onclick='showNoteModal("${ldate}","${ltime}","${lasset}","${escapedNote}")'>Edit</button>`;
      tr.appendChild(tdNote);
      tbody.appendChild(tr);
    } else { 
      let card = document.createElement('div');
      card.className = 'eventCard';
      card.innerHTML =
        `<div><b>Date:</b> ${ldate}</div><div><b>Time:</b> ${ltime}</div><div><b>Asset:</b> ${lasset}</div><div><b>Event:</b> ${levent}</div>` +
        `<div><b>State:</b> ${lstateVal=="1"?"<span style='color:#256029;'>RUNNING</span>":"<span style='color:#b71c1c;'>STOPPED</span>"}</div>` +
        `<div><b>Avail(%):</b> ${Number(lavail).toFixed(2)}</div><div><b>Runtime:</b> ${minToHHMMSS(lrun)}</div><div><b>Downtime:</b> ${minToHHMMSS(lstop)}</div>` +
        `<div><b>MTBF:</b> ${minToHHMMSS(lmtbf)}</div><div><b>MTTR:</b> ${minToHHMMSS(lmttr)}</div><div><b>Stops:</b> ${stopsInt}</div>` +
        `<div><b>Run Dur:</b> ${levent.toUpperCase()==="STOP"?durationStrToHHMMSS(runDurStr):""}</div><div><b>Stop Dur:</b> ${levent.toUpperCase()==="START"?durationStrToHHMMSS(stopDurStr):""}</div>` +
        `<div><b>Note:</b> <span class='note'>${cleanNote(lnote)}</span> <button class='notebtn' onclick='showNoteModal("${ldate}","${ltime}","${lasset}","${escapedNote}")'>Edit</button></div>`;
      mobileDiv.appendChild(card);
    }
  }
  const ec=document.getElementById('eventCount'); if(ec)ec.innerHTML="<b>Events:</b> "+(isMobile?mobileDiv.children.length:tbody.children.length); 
  const et=document.getElementById('eventTable'); if(et)et.style.display=isMobile?'none':'';
  if(mobileDiv)mobileDiv.style.display=isMobile?'':'none';
  if(!scrollInhibit){if(isMobile){if(mobileDiv)mobileDiv.scrollTop=0;}else{window.scrollTo({top:0,behavior:'auto'});}}
}
function toggleScrollInhibit(btn) {
  scrollInhibit = !scrollInhibit;
  if(btn) btn.innerText = scrollInhibit ? "Enable Auto-Scroll" : "Inhibit Auto-Scroll"; 
}

if (document.readyState === 'loading') { document.addEventListener('DOMContentLoaded', initializeEventPage); } 
else { initializeEventPage(); }
)rawliteral");
  server.sendContent("</script>");
  server.sendContent("</head><body>");
  server.sendContent("<header><div style='font-size:1.6em;font-weight:700;'>Event Log</div></header>");
  String navHtml = "<nav class='nav'>";
  navHtml += "<a href='/' class='nav-btn'>Dashboard</a>";
  navHtml += "<a href='/config' class='nav-btn'>Setup</a>";
  navHtml += "<a href='/export_log' class='nav-btn'>Export CSV</a>";
  navHtml += "</nav>";
  server.sendContent(navHtml);

  String mainCardOpen = "<div class='main card'>";
  server.sendContent(mainCardOpen);

  String filterRowHtml = "<div class='filterrow'><label for='channelFilter'>Filter by Channel:</label> <select id='channelFilter'><option value='ALL'>All Assets</option></select>";
  filterRowHtml += "<label for='stateFilter'>Event State:</label> <select id='stateFilter'><option value='ALL'>All</option><option value='RUNNING'>Running</option><option value='STOPPED'>Stopped</option></select>";
  filterRowHtml += "<span id='eventCount' style='margin-left:1em;'></span>";
  filterRowHtml += "<button class='scrollToggle' id='scrollBtn' type='button' onclick='toggleScrollInhibit(this)'>Inhibit Auto-Scroll</button></div>";
  server.sendContent(filterRowHtml);
  
  String tableHtml = "<div style='overflow-x:auto;'><table id='eventTable'><thead><tr>";
  tableHtml += "<th>Date</th><th>Time</th><th>Asset</th><th>Event</th><th>State</th><th>Avail(%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Run Duration</th><th>Stop Duration</th><th>Note</th>";
  tableHtml += "</tr></thead><tbody id='tbody'></tbody></table>";
  tableHtml += "<div id='mobileEvents'></div></div>"; // mobileEvents div for responsive view
  server.sendContent(tableHtml);
  
  // MODAL HTML - Add this before closing the main card or body
  String modalHtml = R"rawliteral(
<div id="noteEditModal" class="modal-overlay">
  <div class="modal-content">
    <h3 class="modal-title">Edit Event Note</h3>
    <form id="modalNoteForm" onsubmit="return submitModalNote(event)">
      <input type="hidden" id="modalNoteDate" name="date">
      <input type="hidden" id="modalNoteTime" name="time">
      <input type="hidden" id="modalNoteAsset" name="asset">
      <label for="modalNoteReason">Reason:</label>
      <select id="modalNoteReason" name="reason"><option value=""></option></select>
      <label for="modalNoteText">Note:</label>
      <input type="text" id="modalNoteText" name="note" maxlength="64" placeholder="Add/Edit note">
      <div class="modal-actions">
        <button type="submit" class="btn-save">Save</button>
        <button type="button" class="btn-cancel" onclick="hideNoteModal()">Cancel</button>
      </div>
    </form>
  </div>
</div>
)rawliteral";
  server.sendContent(modalHtml);

  server.sendContent("</div></body></html>"); // Closing main card, body, html
  server.sendContent(""); 
}


// htmlConfig, handleWiFiReconfigurePost, htmlAssetDetail, handleConfigPost, handleClearLog, handleExportLog (no changes)
// handleApiSummary, handleApiEvents, handleApiConfig, handleApiNote, updateEventNote, handleNotFound (no changes)
String htmlConfig() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Setup</title><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001; font-size:1.6em; font-weight:700;}"; 
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;align-items:center;}";
  html += ".nav button, .nav a {text-decoration:none;background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;}"; 
  html += ".nav button:hover, .nav a:hover {background:#e3f0fc;}"; 
  html += ".nav .right{margin-left:auto;}"; 
  html += ".main{max-width:700px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += "label{font-weight:500;margin-top:1em;display:block;}input[type=text],input[type=number],select{width:100%;padding:0.6em;margin-top:0.2em;margin-bottom:1em;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;}"; 
  html += "input[type=submit].form-button,button.form-button{width:100%;margin-top:1em;padding:0.8em 1.5em;font-size:1.15em;border-radius:8px;border:none;background:#1976d2;color:#fff;font-weight:700;cursor:pointer;}"; 
  html += ".notice{background:#e6fbe7;color:#256029;font-weight:bold;padding:0.6em 1em;border-radius:7px;margin-bottom:1em;text-align:center;}"; 
  html += "button.wifi-reconfig{background:#f44336 !important; color:#fff !important;}"; 
  html += ".config-tile { margin-bottom: 1rem; border: 1px solid #e0e0e0; border-radius: 8px; overflow: hidden; }";
  html += ".config-tile-header { background-color: #e9eff4; color: #1976d2; padding: 0.8em 1em; width: 100%; border: none; text-align: left; font-size: 1.1em; font-weight: 700; cursor: pointer; display:flex; justify-content:space-between; align-items:center;}"; 
  html += ".config-tile-header:hover { background-color: #dce7f0; }";
  html += ".config-tile-header .toggle-icon { font-size: 1.2em; transition: transform 0.2s; margin-left: 10px; }";
  html += ".config-tile-header.active .toggle-icon { transform: rotate(45deg); }"; 
  html += ".config-tile-content { padding: 0 1.3rem 1.3rem 1.3rem; display: none; background-color: #fff; border-top: 1px solid #e0e0e0;}";
  html += ".config-tile-content.open { display: block; }"; 
  html += ".config-tile-content fieldset { border:1px solid #e0e0e0;padding:1em;border-radius:7px;margin-top:1em;margin-bottom:0.5em; }"; 
  html += ".config-tile-content fieldset legend { font-weight:700;color:#2196f3;font-size:1.05em;padding:0 0.5em; }";
  html += "@media(max-width:700px){.main{padding:0.5rem;} .card{padding:0.7rem;} input[type=submit].form-button,button.form-button{font-size:1em;} .config-tile-content{padding:0 0.7rem 0.7rem 0.7rem;}}"; 
  html += "</style>";
  html += "<script>";
  html += "function clearLogDblConfirm(e){ if(!confirm('Are you sure you want to CLEAR ALL LOG DATA?')){e.preventDefault();return false;} if(!confirm('Double check: This cannot be undone! Are you REALLY sure?')){e.preventDefault();return false;} return true; }"; 
  html += "function showSavedMsg(){ const notice = document.getElementById('saveNotice'); if(notice) notice.style.display='block'; }"; 
  html += "function confirmWiFiReconfig(e){ if(!confirm('Are you sure you want to enter WiFi setup mode? The device will disconnect from the current network and restart as an Access Point.')){e.preventDefault();return false;} return true;}"; 
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
  html += "  const firstTileHeader = document.querySelector('.config-tile:first-child .config-tile-header');";
  html += "  if (firstTileHeader && !firstTileHeader.classList.contains('active')) {"; 
  html += "    firstTileHeader.click();"; 
  html += "  }";
  html += "}";
  html += "document.addEventListener('DOMContentLoaded', setupConfigTiles);";
  html += "</script>";
  html += "</head><body>"; 
  html += "<header>Setup</header>"; 
  html += "<nav class='nav'>";
  html += "<a href='/' class='nav-btn'>Dashboard</a>"; 
  html += "<a href='/events' class='nav-btn'>Event Log</a>";
  html += "<a href='/export_log' class='nav-btn'>Export CSV</a>";
  html += "<form action='/clear_log' method='POST' style='display:inline;margin:0;' onsubmit='return clearLogDblConfirm(event);'><button type='submit' style='background:#f44336;color:#fff;' class='nav-btn right'>Clear Log Data</button></form>"; 
  html += "</nav>";
  html += "<div class='main'><div class='card'>"; 
  html += "<form method='POST' action='/save_config' id='setupform' onsubmit='setTimeout(showSavedMsg, 500);'>"; 
  html += "<div id='saveNotice' class='notice' style='display:none;'>Settings saved! Device is rebooting...</div>";

  html += "<div class='config-tile'>";
  html += "  <button type='button' class='config-tile-header'>Asset Setup <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  html += "    <label>Asset count (max " + String(MAX_ASSETS) + "): <input type='number' name='assetCount' min='1' max='" + String(MAX_ASSETS) + "' value='" + String(config.assetCount) + "' required></label>";
  html += "    <p style='font-size:0.9em; color:#555; margin-top:-0.5em; margin-bottom:1em;'>To change the number of assets, update this count and click 'Save All Settings & Reboot'. The page will refresh.</p>"; 
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) break; 
    html += "    <fieldset><legend>Asset #" + String(i+1) + "</legend>";
    html += "      <label>Name: <input type='text' name='name" + String(i) + "' value='" + String(config.assets[i].name) + "' maxlength='31' required></label>";
    html += "      <label>GPIO Pin: <input type='number' name='pin" + String(i) + "' value='" + String(config.assets[i].pin) + "' min='0' max='39' required></label>";
    html += "    </fieldset>";
  }
  html += "  </div>"; 
  html += "</div>"; 

  html += "<div class='config-tile'>";
  html += "  <button type='button' class='config-tile-header'>Operational Settings <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  html += "    <label>Max events per asset (log size): <input type='number' name='maxEvents' min='100' max='5000' value='" + String(config.maxEvents) + "' required></label>";
  html += "    <label>Timezone offset from UTC (hours): <input type='number' name='tzOffset' min='-12' max='14' step='0.5' value='" + String(config.tzOffset / 3600.0, 1) + "' required></label>";
  html += "    <label>Highlight stops longer than (min): <input type='number' name='longStopThreshold' min='1' max='1440' value='" + String(config.longStopThresholdSec/60) + "' required></label>"; 
  html += "  </div>"; 
  html += "</div>"; 
  
  html += "<div class='config-tile'>";
  html += "  <button type='button' class='config-tile-header'>Downtime Quick Reasons <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  for (int i = 0; i < 5; ++i) { 
    html += "    <label>Reason " + String(i+1) + ": <input type='text' name='reason" + String(i) + "' value='" + String(config.downtimeReasons[i]) + "' maxlength='31'></label>";
  }
  html += "  </div>"; 
  html += "</div>"; 

  html += "<input type='submit' value='Save All Settings & Reboot' class='form-button' style='width:100%; margin-top:1.5rem;'>"; 
  html += "</form>"; 

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
  html += "    <p style='margin-top:0.5em; margin-bottom:1em;'>If you need to connect to a different WiFi network or re-enter credentials, use the button below. The device will restart in WiFi Setup Mode.</p>"; 
  html += "    <form method='POST' action='/reconfigure_wifi' onsubmit='return confirmWiFiReconfig(event);' style='margin-top:0.5em;'>"; 
  html += "      <button type='submit' class='form-button wifi-reconfig'>Enter WiFi Setup Mode</button>"; 
  html += "    </form>";
  html += "  </div>"; 
  html += "</div>"; 

  html += "</div></div></body></html>"; 
  return html;
}

void handleWiFiReconfigurePost() {
  prefs.begin("assetmon", false); prefs.remove("ssid"); prefs.remove("pass"); prefs.end();
  Serial.println("WiFi credentials cleared. Restarting in AP mode.");
  String message = "<!DOCTYPE html><html><head><title>WiFi Reconfiguration</title><style>body{font-family:Arial,sans-serif;margin:20px;padding:15px;border:1px solid #ddd;border-radius:5px;text-align:center;}h2{color:#333;}</style></head><body><h2>Device Restarting for WiFi Setup</h2><p>Device will create AP '<strong>AssetMonitor_Config</strong>'. Connect to it.</p><p>Open browser to <strong>http://192.168.4.1</strong> to configure new WiFi.</p><p>Redirecting shortly...</p><meta http-equiv='refresh' content='7;url=http://192.168.4.1/' /></body></html>";
  server.sendHeader("Connection", "close"); server.send(200, "text/html", message);
  delay(1000); ESP.restart(); 
}

String htmlAssetDetail(uint8_t idx) {
  if (idx >= config.assetCount || idx >= MAX_ASSETS) return "Invalid Asset Index"; 
  String assetNameStr = String(config.assets[idx].name); 
  String html = "<!DOCTYPE html><html><head><title>Asset Detail: " + assetNameStr + "</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'><style>body{font-family:Roboto,Arial,sans-serif;margin:2em;background:#f3f7fa;} .card{background:#fff;padding:1.5em;border-radius:8px;box-shadow:0 2px 10px #0001;} a{color:#1976d2;text-decoration:none;} a:hover{text-decoration:underline;}</style></head><body><div class='card'>";
  html += "<h1>Asset Detail: " + assetNameStr + "</h1><p><strong>GPIO Pin:</strong> " + String(config.assets[idx].pin) + "</p>";
  html += "<p><a href='/'>Back to Dashboard</a></p><p><a href='/analytics?asset=" + urlEncode(assetNameStr) + "'>View Analytics</a></p></div></body></html>";
  return html;
}

void handleConfigPost() {
  if (server.hasArg("assetCount")) {
    uint8_t oldAssetCount = config.assetCount; 
    config.assetCount = constrain(server.arg("assetCount").toInt(), 1, MAX_ASSETS);
    if (config.assetCount > oldAssetCount) {
      for (uint8_t i = oldAssetCount; i < config.assetCount; ++i) {
        if (i < MAX_ASSETS) { strcpy(config.assets[i].name, ""); config.assets[i].pin = 0; }
      }
    }
    for (uint8_t i = 0; i < config.assetCount; ++i) {
      if (i < MAX_ASSETS) { 
        if (server.hasArg("name"+String(i))) { strncpy(config.assets[i].name, server.arg("name"+String(i)).c_str(), 31); config.assets[i].name[31] = '\0'; }
        if (server.hasArg("pin"+String(i))) { config.assets[i].pin = server.arg("pin"+String(i)).toInt(); }
      }
    }
    if (server.hasArg("maxEvents")) config.maxEvents = constrain(server.arg("maxEvents").toInt(), 100, 5000);
    if (server.hasArg("tzOffset")) config.tzOffset = static_cast<int>(server.arg("tzOffset").toFloat() * 3600);
    for (int i=0; i<5; ++i) if(server.hasArg("reason"+String(i))) { strncpy(config.downtimeReasons[i], server.arg("reason"+String(i)).c_str(), 31); config.downtimeReasons[i][31] = '\0'; }
    if (server.hasArg("longStopThreshold")) config.longStopThresholdSec = constrain(server.arg("longStopThreshold").toInt() * 60, 60, 1440 * 60);
    saveConfig(); server.sendHeader("Location", "/config#saveNotice"); server.send(303);
    delay(1000); ESP.restart();
  } else server.send(400, "text/plain", "Bad Request");
}

void handleClearLog() { 
  SPIFFS.remove(LOG_FILENAME); Serial.println("Log cleared.");
  server.sendHeader("Location", "/config"); server.send(303); 
}

void handleExportLog() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f || f.size() == 0) { server.send(404, "text/plain", "Log empty."); if(f)f.close(); return; }
  time_t now = time(nullptr); struct tm *t = localtime(&now); char fn[64]; strftime(fn, sizeof(fn), "AssetLog-%Y%m%d-%H%M.csv", t); 
  server.sendHeader("Content-Type", "text/csv"); server.sendHeader("Content-Disposition", String("attachment; filename=\"")+fn+"\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN); server.send(200, "text/csv", ""); 
  server.sendContent("Date,Time,Asset,Event,State,Avail(%),Runtime(min),Downtime(min),MTBF(min),MTTR(min),Stops,RunDur,StopDur,Note\n");
  char buf[1025]; size_t br; while(f.available()){ br=f.readBytes(buf,1024); if(br>0){buf[br]='\0'; server.sendContent(String(buf));}}
  f.close(); server.sendContent(""); Serial.println("Log exported.");
}

void handleApiSummary() {
  String json = "{\"assets\":["; time_t now = time(nullptr); 
  for (uint8_t i=0; i<config.assetCount; ++i) {
    if (i>=MAX_ASSETS) continue; if (i>0) json+=",";
    AssetState& as=assetStates[i]; bool pin_s=digitalRead(config.assets[i].pin);
    unsigned long runT=as.runningTime, stopT=as.stoppedTime;
    if(as.lastState==false) runT+=(now-as.lastChangeTime); else stopT+=(now-as.lastChangeTime);
    float avail=(runT+stopT)>0?(100.0*runT/(runT+stopT)):(pin_s==false?100.0:0.0);
    float rt_m=runT/60.0, st_m=stopT/60.0;
    float mtbf=(as.stopCount>0)?(float)runT/as.stopCount/60.0:rt_m;
    float mttr=(as.stopCount>0)?(float)stopT/as.stopCount/60.0:0;
    json+="{";
    json+="\"name\":\""+String(config.assets[i].name)+"\",";
    json+="\"pin\":"+String(config.assets[i].pin)+",";
    json+="\"state\":"+String(pin_s?0:1)+","; // pin_s HIGH (true)=STOPPED (0), LOW (false)=RUNNING (1)
    json+="\"availability\":"+String(avail,2)+",";
    json+="\"total_runtime\":"+String(rt_m,2)+","; 
    json+="\"total_downtime\":"+String(st_m,2)+","; 
    json+="\"mtbf\":"+String(mtbf,2)+","; 
    json+="\"mttr\":"+String(mttr,2)+","; 
    json+="\"stop_count\":"+String(as.stopCount)+"}";
  }
  json+="]}"; server.send(200,"application/json",json);
}

void handleApiEvents() {
  File f=SPIFFS.open(LOG_FILENAME,FILE_READ); String json="[";
  if(f&&f.size()>0){String l;bool first=true; while(f.available()){l=f.readStringUntil('\n');l.trim();if(l.length()<5)continue;if(!first)json+=",";first=false;String escL="";for(unsigned int i=0;i<l.length();++i){char c=l.charAt(i);if(c=='"')escL+="\\\"";else if(c=='\\')escL+="\\\\";else if(c<32||c>126){}else escL+=c;}json+="\""+escL+"\"";}}
  json+="]"; server.sendHeader("Cache-Control","no-cache,no-store,must-revalidate"); server.sendHeader("Pragma","no-cache"); server.sendHeader("Expires","-1"); server.send(200,"application/json",json);
}

void handleApiConfig() {
  String json="{"; json+="\"assetCount\":"+String(config.assetCount)+","; json+="\"maxEvents\":"+String(config.maxEvents)+","; json+="\"tzOffset\":"+String(config.tzOffset)+","; json+="\"assets\":[";
  for(uint8_t i=0;i<config.assetCount;++i){if(i>=MAX_ASSETS)continue;if(i>0)json+=",";json+="{";json+="\"name\":\""+String(config.assets[i].name)+"\",";json+="\"pin\":"+String(config.assets[i].pin)+"}";}
  json+="],\"downtimeReasons\":["; for(int i=0;i<5;++i){if(i>0)json+=",";json+="\""+String(config.downtimeReasons[i])+"\"";} json+="]";
  json+=",\"longStopThresholdSec\":"+String(config.longStopThresholdSec)+"}"; server.send(200,"application/json",json);
}

void handleApiNote() {
  if(server.method()==HTTP_POST&&server.hasArg("date")&&server.hasArg("time")&&server.hasArg("asset")){
    String d=server.arg("date"),t=server.arg("time"),a=server.arg("asset"),n=server.arg("note"),r=server.hasArg("reason")?server.arg("reason"):"";
    Serial.printf("API Note: D=%s,T=%s,A=%s,R=%s,N=%s\n",d.c_str(),t.c_str(),a.c_str(),r.c_str(),n.c_str());
    updateEventNote(d,t,a,n,r); server.sendHeader("Location","/events"); server.send(303); return;
  } server.send(400,"text/plain","Bad Request: Note params missing");
}

void updateEventNote(String date_str, String time_str, String assetName_str, String note_text_str, String reason_str) { 
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ); if (!f) { Serial.println("updateEventNote: Fail read log."); return; }
  String tempLog = ""; bool updated = false;
  String combinedNote = "";
  if (reason_str.length()>0 && note_text_str.length()>0) combinedNote = reason_str + " - " + note_text_str;
  else if (reason_str.length()>0) combinedNote = reason_str;
  else combinedNote = note_text_str;
  combinedNote.replace(",", " "); combinedNote.replace("\n", " "); combinedNote.replace("\r", " "); 

  String line;
  while (f.available()) {
    line = f.readStringUntil('\n');
    String trimmedLine = line; trimmedLine.trim(); 
    if (trimmedLine.length() < 5) { tempLog += line; if(f.available() || line.endsWith("\n")) tempLog+=""; else tempLog+="\n"; continue; } // Preserve empty lines or add newline if last line has no \n

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
      else tempLog += trimmedLine + "," + combinedNote + "\n"; 
      updated = true; Serial.println("Updated log line.");
    } else {
      tempLog += line;  // Add original line including its newline (if it had one)
      if (!line.endsWith("\n") && f.available()) tempLog += "\n"; // Add newline if readStringUntil cut it and not EOF
    }
  }
  f.close();

  if (updated) {
    File f2 = SPIFFS.open(LOG_FILENAME, FILE_WRITE); 
    if (!f2) { Serial.println("updateEventNote: Fail write log."); return; }
    f2.print(tempLog); f2.close(); Serial.println("Log rewritten.");
  } else Serial.println("Event for note update not found.");
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }
