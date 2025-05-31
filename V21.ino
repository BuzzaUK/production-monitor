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
// String htmlEvents(); // Will be replaced by void sendHtmlEventsPage()
void sendHtmlEventsPage(); // New declaration for chunked sending
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
Preferences prefs;
AssetState assetStates[MAX_ASSETS];

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

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS.begin() failed! Halting."); 
    return; 
  }
  Serial.println("SPIFFS initialized.");

  if (!prefs.begin("assetmon", false)) { 
    Serial.println("Global prefs.begin() failed! Default settings will be used and may not save.");
  } else {
    Serial.println("Global Preferences initialized.");
  }

  loadConfig();
  Serial.println("Configuration loaded/initialized.");

  setupWiFiSmart(); 
  setupTime(); 
  Serial.println("WiFi and Time setup complete.");

  Serial.printf("Initializing %u assets defined in config...\n", config.assetCount);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i < MAX_ASSETS) { 
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
                    assetStates[i].lastState ? "HIGH/INPUT_PULLUP (implies RUNNING for active LOW)" : "LOW (implies STOPPED for active LOW)");
    }
  }
  
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
  server.on("/dashboard", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
  server.on("/config", HTTP_GET, []() { server.send(200, "text/html", htmlConfig()); });
  // server.on("/events", HTTP_GET, []() { server.send(200, "text/html", htmlEvents()); }); // Old way
  server.on("/events", HTTP_GET, sendHtmlEventsPage); // New chunked way
  server.on("/asset", HTTP_GET, []() {
    if (server.hasArg("idx")) {
      uint8_t idx = server.arg("idx").toInt();
      if (idx < config.assetCount && idx < MAX_ASSETS) { 
        server.send(200, "text/html", htmlAssetDetail(idx));
        return;
      }
    }
    server.send(404, "text/plain", "Asset not found or index invalid");
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

void loop() {
  server.handleClient();
  time_t now = time(nullptr);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) continue; 

    bool current_state = digitalRead(config.assets[i].pin); 
    if (current_state != assetStates[i].lastState) {
      unsigned long elapsed = now - assetStates[i].lastChangeTime;
      unsigned long runDuration = 0;
      unsigned long stopDuration = 0;
      if (current_state) { // Assuming active LOW, so HIGH means STOPPED, transition to HIGH is a STOP event
        assetStates[i].runningTime += elapsed; // The period that just ended was a RUN period
        assetStates[i].stopCount++;
        runDuration = elapsed; 
        assetStates[i].lastRunDuration = runDuration;
        assetStates[i].lastStopDuration = 0; 
        logEvent(i, false, now, nullptr, runDuration, 0); // Log STOP event (state=false)
      }
      else { // Transition to LOW means STARTED
        assetStates[i].stoppedTime += elapsed; // The period that just ended was a STOP period
        assetStates[i].runCount++;
        stopDuration = elapsed; 
        assetStates[i].lastStopDuration = stopDuration;
        assetStates[i].lastRunDuration = 0; 
        logEvent(i, true, now, nullptr, 0, stopDuration); // Log START event (state=true)
      }
      assetStates[i].lastState = current_state; // Save the pin's current state (HIGH or LOW)
      assetStates[i].lastChangeTime = now;
    }
  }
  delay(200); 
}

// In logEvent, 'state' parameter now means machine RUNNING (true) or STOPPED (false)
// This aligns with typical understanding, not the direct pin state if active LOW.
void logEvent(uint8_t assetIdx, bool machineIsRunning, time_t now, const char* note, unsigned long runDuration, unsigned long stopDuration) {
  if (assetIdx >= MAX_ASSETS) return; 

  AssetState& as = assetStates[assetIdx];
  // These are cumulative totals *before* the current just-ended period.
  unsigned long current_total_runningTime = as.runningTime;
  unsigned long current_total_stoppedTime = as.stoppedTime;
  
  float avail = (current_total_runningTime + current_total_stoppedTime) > 0 
                ? (100.0 * current_total_runningTime / (current_total_runningTime + current_total_stoppedTime)) 
                : (machineIsRunning ? 100.0 : 0.0); 
  float total_runtime_min = current_total_runningTime / 60.0;
  float total_downtime_min = current_total_stoppedTime / 60.0;
  
  // MTBF/MTTR calculation should use the state *after* the event for counts.
  // If it's a STOP event (machineIsRunning = false), stopCount for MTBF calculation is relevant.
  // If it's a START event (machineIsRunning = true), a stop has just ended, so stopCount for MTTR is relevant.
  // The as.stopCount is the count *before* this event.
  uint32_t relevant_stop_count_for_mtbf = as.stopCount; // If machine just stopped, this is the new total number of stops.
  uint32_t relevant_stop_count_for_mttr = as.stopCount; // If machine just started, this is the number of stops that have completed.

  float mtbf_val = (relevant_stop_count_for_mtbf > 0) ? (float)current_total_runningTime / relevant_stop_count_for_mtbf / 60.0 : 0; 
  float mttr_val = (relevant_stop_count_for_mttr > 0) ? (float)current_total_stoppedTime / relevant_stop_count_for_mttr / 60.0 : 0; 

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
    as.stopCount, // Log the stop count *at the time of the event*
    (runDuration > 0 ? formatMMSS(runDuration).c_str() : ""),   
    (stopDuration > 0 ? formatMMSS(stopDuration).c_str() : ""), 
    note ? note : ""
  );
  f.close();
  as.lastEventTime = now; 
  Serial.printf("Event logged for %s: %s. RunD: %s, StopD: %s. Stops: %u\n", 
    config.assets[assetIdx].name,
    machineIsRunning ? "START" : "STOP",
    formatMMSS(runDuration).c_str(),
    formatMMSS(stopDuration).c_str(),
    as.stopCount
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
  char temp[] = "0x00";
  unsigned int len = str.length();
  unsigned int i = 0;
  while (i < len) {
    char c = str.charAt(i);
    if (c == '%') {
      if (i+2 < len) {
        temp[2] = str.charAt(i+1);
        temp[3] = str.charAt(i+2);
        decoded += char(strtol(temp, NULL, 16));
        i += 3;
      } else { 
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
      let stateClass = asset.state==1 ? "running" : "stopped"; // Assuming state 1 is running from API
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
// ... (JavaScript for Analytics page, assumed to be okay for now based on no reported errors)
// ... (Ensure floatMinToMMSS, mmssToSeconds, parseEventDate, toDatetimeLocal are defined)
// ... (Ensure initAnalyticsPage, fetchAnalyticsData, setupRangePickers, renderKPIs, renderEventChart, renderRecentEvents are defined)
// This is just a placeholder to indicate the full script for analytics should be here.
// The critical fix for floatMinToMMSS was applied in a previous version.
console.log('Analytics script started (v14 - Enhanced MTBF/MTTR tooltips).');

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
  if(isNaN(h) || isNaN(m) || isNaN(s)) return 0; 
  return (h * 3600) + (m * 60) + s;
}
function parseEventDate(eventRow) {
  if (!eventRow || eventRow.length < 2) return new Date(0); 
  try {
    let [d, m, y] = eventRow[0].split('/').map(Number); let [hh, mm, ss] = eventRow[1].split(':').map(Number);
    if (isNaN(d) || isNaN(m) || isNaN(y) || isNaN(hh) || isNaN(mm) || isNaN(ss)) return new Date(0);
    return new Date(Date.UTC(y, m - 1, d, hh, mm, ss));
  } catch (e) { console.error('Error parsing date for eventRow:', eventRow, e); return new Date(0); }
}
function toDatetimeLocal(dt) {
  if (!(dt instanceof Date) || isNaN(dt)) dt = new Date(); 
  try {
    const timezoneOffset = dt.getTimezoneOffset() * 60000; const localDate = new Date(dt.getTime() - timezoneOffset);
    const pad = n => n < 10 ? '0' + n : n;
    return localDate.getFullYear() + '-' + pad(localDate.getMonth() + 1) + '-' + pad(localDate.getDate()) + 'T' + pad(localDate.getHours()) + ':' + pad(localDate.getMinutes());
  } catch (e) {
    console.error('Error in toDatetimeLocal:', e, 'Input date:', dt);
    const now = new Date(Date.now() - (new Date().getTimezoneOffset() * 60000)); return now.toISOString().slice(0, 16);
  }
}

let asset = '';
try {
  asset = decodeURIComponent(new URLSearchParams(window.location.search).get("asset") || "");
  const assetNameElement = document.getElementById('assetNameInHeader');
  if (assetNameElement) assetNameElement.textContent = asset;
} catch (e) { console.error('Error getting asset from URL:', e); }

let allEvents = []; let eventChart = null; let filteredEventsGlobal = []; 

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
        allEvents = rawEvents.map(line => (typeof line === 'string') ? line.split(',') : [])
                             .filter(eventRow => eventRow.length > 13 && eventRow[2] && eventRow[2].trim() === asset.trim());
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
      const chartDiv = document.getElementById('eventChart');
      if(chartDiv) { 
        const ctx = chartDiv.getContext('2d');
        if(ctx) { 
            ctx.clearRect(0, 0, chartDiv.width, chartDiv.height);
            ctx.textAlign = 'center';
            ctx.fillText('Failed to load chart data.', chartDiv.width / 2, chartDiv.height / 2);
        }
      }
    });
}
function setupRangePickers() {
  let defaultFromDate, defaultToDate;
  if (allEvents.length > 0) {
    try {
      let eventDates = allEvents.map(e => parseEventDate(e)).filter(d => d.getTime() !== 0); 
      if (eventDates.length > 0) {
        defaultToDate = new Date(Math.max.apply(null, eventDates)); defaultFromDate = new Date(defaultToDate.getTime() - 12 * 60 * 60 * 1000); 
      } else { defaultToDate = new Date(); defaultFromDate = new Date(defaultToDate.getTime() - 12 * 60 * 60 * 1000); }
    } catch (e) { defaultToDate = new Date(); defaultFromDate = new Date(defaultToDate.getTime() - 12 * 60 * 60 * 1000); }
  } else { defaultToDate = new Date(); defaultFromDate = new Date(defaultToDate.getTime() - 12 * 60 * 60 * 1000); }
  
  const fromTimeEl = document.getElementById('fromTime');
  const toTimeEl = document.getElementById('toTime');
  if(fromTimeEl) fromTimeEl.value = toDatetimeLocal(defaultFromDate);
  if(toTimeEl) toTimeEl.value = toDatetimeLocal(defaultToDate);
  
  ['fromTime', 'toTime', 'showStart', 'showStop', 'showMTBF', 'showMTTR'].forEach(id => {
    const el = document.getElementById(id); if (el) el.onchange = renderEventChart;
  });
  const exportButton = document.getElementById('exportPng');
  if (exportButton) {
    exportButton.onclick = function () {
      if (!eventChart || !eventChart.canvas) { console.warn('Export PNG: Chart not ready or canvas missing.'); return; } 
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
    const stops = latestEvent[10] !== undefined ? latestEvent[10] : '0';
    const runtime = latestEvent[6] !== undefined ? latestEvent[6] : '0';
    const downtime = latestEvent[7] !== undefined ? latestEvent[7] : '0';
    const availability = latestEvent[5] !== undefined ? latestEvent[5] : '0';
    const mtbf = latestEvent[8] !== undefined ? latestEvent[8] : '0';
    const mttr = latestEvent[9] !== undefined ? latestEvent[9] : '0';

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
  const chartCanvas = document.getElementById('eventChart'); 
  if (!chartCanvas) { console.error("Event chart canvas not found!"); return; }

  if (!allEvents || allEvents.length === 0) { 
    if (eventChart) { eventChart.destroy(); eventChart = null; } 
    const ctx = chartCanvas.getContext('2d');
    if(ctx){
        ctx.clearRect(0, 0, chartCanvas.width, chartCanvas.height);
        ctx.textAlign = 'center'; ctx.font = '16px Roboto'; ctx.fillStyle = '#555';
        ctx.fillText('No event data available for this asset.', chartCanvas.width / 2, chartCanvas.height / 2);
    }
    renderKPIs([]); 
    return; 
  }
  let fromDate, toDate;
  try { 
    const fromTimeVal = document.getElementById('fromTime').value;
    const toTimeVal = document.getElementById('toTime').value;
    if(!fromTimeVal || !toTimeVal) { console.error("Date range pickers not found or empty."); return; }
    fromDate = new Date(fromTimeVal); 
    toDate = new Date(toTimeVal); 
    if(isNaN(fromDate.getTime()) || isNaN(toDate.getTime())) { console.error("Invalid date format from pickers."); return; }
  }
  catch (e) { console.error('Error parsing date/time input values:', e); return; }

  const showStart = document.getElementById('showStart').checked; const showStop = document.getElementById('showStop').checked;
  const showMTBF = document.getElementById('showMTBF').checked; const showMTTR = document.getElementById('showMTTR').checked;
  
  filteredEventsGlobal = allEvents.filter(eventRow => {
    try {
      const eventDate = parseEventDate(eventRow); if (eventDate.getTime() === 0) return false; 
      if (eventDate < fromDate || eventDate > toDate) return false; 
      if (!eventRow[3]) return false; 
      const eventType = eventRow[3].trim().toUpperCase();
      if (eventType === "START" && !showStart) return false;
      if (eventType === "STOP" && !showStop) return false; 
      return true;
    } catch (e) { console.error("Error filtering event:", eventRow, e); return false; } 
  });

  renderKPIs(filteredEventsGlobal); 
  if (filteredEventsGlobal.length === 0) { 
    if (eventChart) { eventChart.destroy(); eventChart = null; } 
    const ctx = chartCanvas.getContext('2d');
    if(ctx){
        ctx.clearRect(0, 0, chartCanvas.width, chartCanvas.height);
        ctx.textAlign = 'center'; ctx.font = '16px Roboto'; ctx.fillStyle = '#555';
        ctx.fillText('No data for selected range.', chartCanvas.width / 2, chartCanvas.height / 2);
    }
    return; 
  }

  try {
    let times = filteredEventsGlobal.map(e => e[1]); 
    let avail = filteredEventsGlobal.map(e => parseFloat(e[5]));
    let mtbfValues = filteredEventsGlobal.map(e => parseFloat(e[8])); 
    let mttrValues = filteredEventsGlobal.map(e => parseFloat(e[9]));
    let stateArr = filteredEventsGlobal.map(e => e[4] ? e[4].trim() : 'UNKNOWN_STATE'); 
    
    let pointColors = filteredEventsGlobal.map((e, index, arr) => {
      const eventType = e[3] ? e[3].trim().toUpperCase() : "";
      let durationForStopDecision = "0:0"; 
      if (eventType === "STOP") {
        if (index + 1 < arr.length) { 
          const nextEvent = arr[index + 1];
          const nextEventType = nextEvent[3] ? nextEvent[3].trim().toUpperCase() : "";
          if (nextEventType === "START") { durationForStopDecision = nextEvent[12] || "0:0"; } 
          else { durationForStopDecision = e[12] || "0:0"; } 
        } else { durationForStopDecision = e[12] || "0:0"; } 
      }
      if (eventType === "STOP" && mmssToSeconds(durationForStopDecision) >= (window.longStopThresholdSec || 300) ) return "#c62828"; 
      if (eventType === "STOP") return "#ff9800"; 
      return "#43a047"; 
    });

    let pointSizes = filteredEventsGlobal.map((e, index, arr) => {
       const eventType = e[3] ? e[3].trim().toUpperCase() : "";
       let durationForStopDecision = "0:0"; 
       if (eventType === "STOP") {
         if (index + 1 < arr.length) {
           const nextEvent = arr[index + 1];
           const nextEventType = nextEvent[3] ? nextEvent[3].trim().toUpperCase() : "";
           if (nextEventType === "START") { durationForStopDecision = nextEvent[12] || "0:0"; } 
           else { durationForStopDecision = e[12] || "0:0"; }
         } else { durationForStopDecision = e[12] || "0:0"; }
       }
      let defaultSize = 7;
      if (eventType === "STOP" && mmssToSeconds(durationForStopDecision) >= (window.longStopThresholdSec || 300)) { defaultSize = 12; }
      return defaultSize;
    });

    let datasets = [{
      label: 'Availability (%)', data: avail, yAxisID: 'y', stepped: true, tension: 0,
      pointRadius: pointSizes, pointBackgroundColor: pointColors, pointBorderColor: pointColors, showLine: true,
      segment: {
        borderColor: ctxSeg => { 
          const stateValue = stateArr[ctxSeg.p0DataIndex];
          if (stateValue === "1") return "#43a047"; if (stateValue === "0") return "#c62828"; 
          return "#000000"; 
        },
        borderWidth: 3
      }
    }];
    if (showMTBF) datasets.push({ label: 'MTBF', data: mtbfValues, yAxisID: 'y1', borderColor: "#1565c0", borderWidth: 2, tension: 0, pointRadius: 4, fill: false }); 
    if (showMTTR) datasets.push({ label: 'MTTR', data: mttrValues, yAxisID: 'y1', borderColor: "#FFD600", borderWidth: 2, tension: 0, pointRadius: 4, fill: false }); 
    
    if (eventChart) eventChart.destroy();
    const ctxRender = chartCanvas.getContext('2d'); 
    eventChart = new Chart(ctxRender, {
      type: 'line', data: { labels: times, datasets: datasets },
      options: {
        responsive: true, maintainAspectRatio: false,
        interaction: { mode: 'nearest', axis: 'x', intersect: true }, 
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
                if(!eventRow) return null; 
                const eventType = eventRow[3] ? eventRow[3].trim().toUpperCase() : "";
                const datasetLabel = tooltipItem.dataset.label || '';
                let lines = [];

                if (datasetLabel === 'Availability (%)') {
                  const currentAvail = parseFloat(eventRow[5]).toFixed(2);
                  lines.push(`Availability: ${currentAvail}%`);
                  if (eventType === "START") {
                    const stopDurationSeconds = mmssToSeconds(eventRow[12] || "0:0"); 
                    if (stopDurationSeconds > 0) {
                         lines.push(`(Prior Stop: ${floatMinToMMSS(stopDurationSeconds / 60.0)})`);
                    }
                  } else if (eventType === "STOP") {
                    const runDurationSeconds = mmssToSeconds(eventRow[11] || "0:0"); 
                    if (runDurationSeconds > 0) {
                        lines.push(`(Prior Run: ${floatMinToMMSS(runDurationSeconds / 60.0)})`);
                    }
                  }
                } else if (datasetLabel === 'MTBF' || datasetLabel === 'MTTR') {
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
                            if (datasetLabel === 'MTBF') {
                                changeIndicator = change > 0 ? `(Increased by ${formattedChange} - Good)` : `(Decreased by ${formattedChange} - Bad)`;
                            } else { 
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

                  if (datasetLabel === 'MTBF' && eventType === "STOP") {
                    const lastRunDurationSeconds = mmssToSeconds(eventRow[11] || "0:0"); 
                    if (lastRunDurationSeconds > 0) {
                      lines.push(`(Influenced by last run: ${floatMinToMMSS(lastRunDurationSeconds / 60.0)})`);
                    }
                  } else if (datasetLabel === 'MTTR' && eventType === "START") {
                    const lastStopDurationSeconds = mmssToSeconds(eventRow[12] || "0:0"); 
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
          x: { title: { display: true, text: 'Time (HH:MM:SS)' } },
          y: { 
            title: { display: true, text: 'Availability (%)' }, 
            beginAtZero: true, 
            suggestedMax: 105, 
            ticks: {
                stepSize: 20,    
                callback: function(value, index, values) {
                    if (value > 100 && value < 105 ) return undefined; 
                    if (value === 100) return 100;
                    if (value < 100 && value >= 0 && (value % (this.chart.options.scales.y.ticks.stepSize || 20) === 0) ) return value;
                    return undefined; 
                }
            }
          }, 
          y1: { 
            type: 'linear', 
            display: true, 
            position: 'right', 
            title: { display: true, text: 'MTBF/MTTR' }, 
            beginAtZero: true, 
            grid: { drawOnChartArea: false }, 
            ticks: { callback: val => floatMinToMMSS(val) } 
          }
        }
      }
    });
  } catch (e) { console.error('Error rendering chart:', e); }
}
function renderRecentEvents() {
  const tbody = document.getElementById('recentEvents'); if (!tbody) return; tbody.innerHTML = ""; 
  if (!allEvents || allEvents.length === 0) { tbody.innerHTML = "<tr><td colspan='13'>No event data for this asset.</td></tr>"; return; }
  const eventsToDisplay = allEvents.slice(-10).reverse(); 
  
  if (eventsToDisplay.length === 0) { tbody.innerHTML = "<tr><td colspan='13'>No recent events for this asset.</td></tr>"; return; }
  
  eventsToDisplay.forEach(eventRow => {
    try {
      if (eventRow.length < 14) { 
          let tr = tbody.insertRow(); let td = tr.insertCell(); td.colSpan = 13; 
          td.textContent = "Malformed data."; td.style.color = "orange"; return; 
      }
      let tr = tbody.insertRow();
      tr.insertCell().textContent = eventRow[0];  tr.insertCell().textContent = eventRow[1];  
      tr.insertCell().textContent = eventRow[2];  tr.insertCell().textContent = eventRow[3];  
      tr.insertCell().textContent = parseFloat(eventRow[5]).toFixed(2); 
      tr.insertCell().textContent = floatMinToMMSS(eventRow[6]); 
      tr.insertCell().textContent = floatMinToMMSS(eventRow[7]); 
      tr.insertCell().textContent = floatMinToMMSS(eventRow[8]); 
      tr.insertCell().textContent = floatMinToMMSS(eventRow[9]);
      tr.insertCell().textContent = eventRow[10]; 
      tr.insertCell().textContent = floatMinToMMSS(mmssToSeconds(eventRow[11] || "0:0") / 60.0); 
      tr.insertCell().textContent = floatMinToMMSS(mmssToSeconds(eventRow[12] || "0:0") / 60.0); 
      tr.insertCell().textContent = eventRow[13] || ""; 
    } catch (e) {
      console.error('Error rendering row for event:', eventRow, e);
      let tr = tbody.insertRow(); let td = tr.insertCell(); td.colSpan = 13; 
      td.textContent = "Error displaying row."; td.style.color = "red";
    }
  });
}

function initAnalyticsPage() {
    fetch('/api/config')
        .then(r => { if(!r.ok) throw new Error("Config fetch failed: " + r.status); return r.json();})
        .then(cfg => {
            window.longStopThresholdSec = cfg.longStopThresholdSec || 300; 
            fetchAnalyticsData(); 
        })
        .catch(e => {
            console.error("Failed to fetch config for longStopThreshold, using default.", e);
            window.longStopThresholdSec = 300; 
            fetchAnalyticsData(); 
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

String htmlAnalyticsCompare() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Compare Assets</title>";
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
  let h = Math.floor(totalSeconds / 3600);
  let m = Math.floor((totalSeconds % 3600) / 60);
  let s = totalSeconds % 60;
  return h.toString().padStart(1, '0') + ":" + m.toString().padStart(2, '0') + ":" + s.toString().padStart(2, '0'); 
}

function fetchCompareDataPage() { 
  fetch('/api/config').then(r=>r.json()).then(cfg=>{
    configDowntimeReasonsCompare = cfg.downtimeReasons||[];
    allAssetNamesCompare = cfg.assets.map(a=>a.name);
    
    fetch('/api/events').then(r=>r.json()).then(events=>{
      allEventsCompare = events
        .map(l=> (typeof l === 'string' ? l.split(',') : []) ) 
        .filter(v=>v.length>13 && allAssetNamesCompare.includes(v[2]));
      renderCompareChartsPage();
      renderCompareTablePage();
    }).catch(e=>console.error("CompareEventsFetchErr:", e));
  }).catch(e=>console.error("CompareConfigFetchErr:", e));
}

function getLastMetricCompare(events, idx) { 
  return events && events.length ? parseFloat(events[events.length-1][idx]) : 0; 
}

function renderCompareChartsPage() { 
  let byAsset = {};
  allAssetNamesCompare.forEach(a=>{byAsset[a]=[];});
  for (let e of allEventsCompare) { if(byAsset[e[2]]) byAsset[e[2]].push(e); } 
  
  let labels = allAssetNamesCompare;
  let avail = labels.map(a=>getLastMetricCompare(byAsset[a]||[],5)); 
  let stops = labels.map(a=>(byAsset[a]||[]).filter(e=>e[3] && e[3].toUpperCase()==="STOP").length); 
  let mtbf = labels.map(a=>getLastMetricCompare(byAsset[a]||[],8));
  
  let reasons = {};
  configDowntimeReasonsCompare.forEach(r => reasons[r] = 0); 

  for (let e of allEventsCompare) {
    if (e.length < 14) continue; 
    let note = e[13]||"";
    let res = "";
    if (note.indexOf(" - ")>-1) res = note.split(" - ")[0].trim();
    else res = note.trim();
    if (res && configDowntimeReasonsCompare.includes(res)) reasons[res] = (reasons[res]||0)+1;
  }
  
  const pieLabels = Object.keys(reasons).filter(r => reasons[r] > 0);
  const pieData = pieLabels.map(r => reasons[r]);
  const pieColors = ['#ffa726','#ef5350','#66bb6a','#42a5f5','#ab47bc', '#FFEE58', '#26A69A', '#78909C'];


  ['barAvail', 'barStops', 'barMTBF', 'pieReasons'].forEach(id => {
    const canvas = document.getElementById(id);
    if (canvas && canvas.chartInstance) canvas.chartInstance.destroy(); 
  });

  if (document.getElementById('barAvail')) {
    document.getElementById('barAvail').chartInstance = new Chart(document.getElementById('barAvail').getContext('2d'), {
      type:'bar',data:{labels:labels,datasets:[{label:'Availability (%)',data:avail,backgroundColor:'#42a5f5'}]},
      options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true,max:100, title:{display:true, text:'Availability (%)'}}}} 
    });
  }
  if (document.getElementById('barStops')) {
    document.getElementById('barStops').chartInstance = new Chart(document.getElementById('barStops').getContext('2d'), {
      type:'bar',data:{labels:labels,datasets:[{label:'Stops',data:stops,backgroundColor:'#ef5350'}]},
      options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true, ticks:{stepSize:1}, title:{display:true, text:'Number of Stops'}}}} 
    });
  }
  if (document.getElementById('barMTBF')) {
    document.getElementById('barMTBF').chartInstance = new Chart(document.getElementById('barMTBF').getContext('2d'), {
      type:'bar',data:{labels:labels,datasets:[{label:'MTBF (min)',data:mtbf,backgroundColor:'#66bb6a'}]},
      options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true, title:{display:true, text:'MTBF (minutes)' }}}} 
    });
  }
  
  const pieCanvas = document.getElementById('pieReasons');
  if (pieCanvas) {
    if (pieLabels.length > 0) {
      pieCanvas.chartInstance = new Chart(pieCanvas.getContext('2d'), {
        type:'pie',data:{
          labels:pieLabels,datasets:[{data:pieData,backgroundColor:pieColors.slice(0, pieLabels.length)}]
        },options:{responsive:true,maintainAspectRatio:false, plugins:{legend:{position:'right', labels:{boxWidth:15}}, title:{display:true, text:'Downtime Reasons'}}} 
      });
    } else {
      const ctx = pieCanvas.getContext('2d');
      ctx.clearRect(0,0,ctx.canvas.width, ctx.canvas.height);
      ctx.textAlign = 'center'; ctx.font = '14px Roboto'; ctx.fillStyle = '#555'; 
      ctx.fillText('No downtime reasons logged.', ctx.canvas.width/2, ctx.canvas.height/2);
    }
  }
}

function renderCompareTablePage() { 
  let tb = document.getElementById('compareTable');
  if(!tb) return; 
  tb.innerHTML = "";
  let byAsset = {};
  allAssetNamesCompare.forEach(a=>{byAsset[a]=[];});
  for (let e of allEventsCompare) { if(byAsset[e[2]]) byAsset[e[2]].push(e); }

  for (let a of allAssetNamesCompare) {
    let evs = byAsset[a]||[], e_last = evs.length?evs[evs.length-1]:null; 
    tb.innerHTML += `<tr>
      <td>${a}</td>
      <td>${e_last && e_last[5] ?parseFloat(e_last[5]).toFixed(2):"-"}</td>
      <td>${e_last && e_last[6] ?formatMinutesToHHMMSSCompare(parseFloat(e_last[6])):"-"}</td>
      <td>${e_last && e_last[7] ?formatMinutesToHHMMSSCompare(parseFloat(e_last[7])):"-"}</td>
      <td>${evs.filter(ev=>ev[3] && ev[3].toUpperCase()==="STOP").length}</td>
      <td>${e_last && e_last[8] ?formatMinutesToHHMMSSCompare(parseFloat(e_last[8])):"-"}</td>
      <td>${e_last && e_last[9] ?formatMinutesToHHMMSSCompare(parseFloat(e_last[9])):"-"}</td>
    </tr>`;
  }
}
if (document.readyState === 'loading') { document.addEventListener('DOMContentLoaded', fetchCompareDataPage); }
else { fetchCompareDataPage(); }
)rawliteral";
  html += "</script>";
  html += "</div></body></html>";
  return html;
}

// CHUNKED SENDING FUNCTION FOR EVENT LOG PAGE
void sendHtmlEventsPage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", ""); // Send headers

  server.sendContent("<!DOCTYPE html><html lang='en'><head><title>Event Log</title>");
  server.sendContent("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  server.sendContent("<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>");
  server.sendContent("<style>");
  server.sendContent("body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001; font-size:1.6em; font-weight:700;}");
  server.sendContent(".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0 0 0;flex-wrap:wrap;}");
  server.sendContent(".nav button, .nav a {text-decoration:none;background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;transition:.2s;cursor:pointer;margin-bottom:0.5em;}"); 
  server.sendContent(".nav button:hover, .nav a:hover {background:#e3f0fc;}"); 
  server.sendContent(".main{max-width:1100px;margin:1rem auto;padding:1rem;}");
  server.sendContent(".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}");
  server.sendContent(".filterrow{display:flex;gap:1em;align-items:center;margin-bottom:1em;flex-wrap:wrap;}");
  server.sendContent(".filterrow label{font-weight:bold; margin-right:0.3em;} .filterrow select, .filterrow input {padding:0.5em; border-radius:4px; border:1px solid #ccc; margin-right:1em;}"); 
  server.sendContent(".scrollToggle{margin-left:auto;font-size:1em; padding: 0.5em 0.8em; border-radius:4px; background-color:#6c757d; color:white; border:none; cursor:pointer;} .scrollToggle:hover{background-color:#5a6268;}"); 
  server.sendContent("table{width:100%;border-collapse:collapse;font-size:1em;margin-top:0.8em;}");
  server.sendContent("th,td{padding:0.7em 0.5em;text-align:left;border-bottom:1px solid #eee;}");
  server.sendContent("th{background:#2196f3;color:#fff;}");
  server.sendContent("tr{background:#fcfcfd;} tr:nth-child(even){background:#f3f7fa;}");
  server.sendContent(".note{font-style:italic;color:#555;white-space:normal;word-break:break-word;display:block;max-width:260px;}");
  server.sendContent(".notebtn{padding:2px 8px;font-size:1em;border-radius:4px;background:#1976d2;color:#fff;border:none;cursor:pointer;margin-left:0.5em;} .notebtn:hover{background:#0d47a1;}");
  server.sendContent(".noteform{display:flex;flex-direction:column;background:#e3f0fc;padding:0.7em;margin:0.5em 0 0.5em 0;border-radius:8px;width:100%;box-sizing:border-box;}");
  server.sendContent(".noteform label{margin-bottom:0.3em;}");
  server.sendContent(".noteform select,.noteform input[type=\"text\"]{width:100%;margin-bottom:0.4em;font-size:1em;padding:0.2em 0.5em;box-sizing:border-box; border-radius:4px; border:1px solid #ccc;}"); 
  server.sendContent(".noteform button{margin-top:0.1em;margin-right:0.3em;width:auto;align-self:flex-start; padding:0.4em 0.8em; border-radius:4px;}"); 
  server.sendContent("td:last-child{max-width:280px;overflow-wrap:anywhere;word-break:break-word;}"); 
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
window.openNoteFormId = null;
window.refreshIntervalId = null;
let eventData = []; 
let channelList = []; 
let filterValue = "ALL"; 
let stateFilter = "ALL"; 
window.downtimeReasons = []; 
let scrollInhibit = false; 

function startAutoRefresh() {
  if (window.refreshIntervalId) clearInterval(window.refreshIntervalId);
  window.refreshIntervalId = setInterval(fetchAndRenderEvents, 5000); 
}
function stopAutoRefresh() {
  if (window.refreshIntervalId) clearInterval(window.refreshIntervalId);
  window.refreshIntervalId = null;
}

function initializeEventPage() {
  console.log("DOMContentLoaded event fired. Initializing event page...");
  let testSel = document.getElementById('channelFilter');
  if (!testSel) {
    console.error("!!! TEST FAIL: 'channelFilter' is NULL immediately after DOMContentLoaded.");
    const mainCard = document.querySelector(".main.card"); 
    if(mainCard) mainCard.innerHTML = "<h1 style='color:red;'>CRITICAL DOM ERROR: 'channelFilter' element not found. Page cannot load.</h1>";
    else document.body.innerHTML = "<h1 style='color:red;'>CRITICAL DOM ERROR: 'channelFilter' element not found. Page cannot load.</h1>";
  } else {
    console.log("+++ TEST PASS: 'channelFilter' was found after DOMContentLoaded. Proceeding to fetchChannelsAndStart.");
    fetchChannelsAndStart(); 
  }
}

function fetchChannelsAndStart() { 
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    channelList = data.assets.map(a=>a.name);
    let sel = document.getElementById('channelFilter');
    if (!sel) { 
      console.error("CRITICAL: 'channelFilter' still null in fetchChannelsAndStart. This shouldn't happen if initializeEventPage worked.");
      return; 
    }
    sel.innerHTML = "<option value='ALL'>All Assets</option>"; 
    for (let i=0;i<channelList.length;++i) {
      let opt = document.createElement("option");
      opt.value = channelList[i];
      opt.text = channelList[i];
      sel.appendChild(opt);
    }
    sel.onchange = function() { filterValue = sel.value; renderTable(); };
    
    let stateSel = document.getElementById('stateFilter');
    if (!stateSel) {
      console.error("CRITICAL: HTML element with ID 'stateFilter' not found.");
      return;
    }
    stateSel.onchange = function() { stateFilter = this.value; renderTable(); };
    
    fetchReasonsAndEvents(); 
  }).catch(e => {
      console.error("Error fetching channels or setting up filters:", e); 
      const mainCard = document.querySelector(".main.card");
      if(mainCard) {
          mainCard.innerHTML = "<p style='color:red; font-weight:bold;'>Error loading event log: Could not fetch initial filter data. Please check console.</p>" + (e.message ? `<p>${e.message}</p>` : "");
      }
  });
}
function fetchReasonsAndEvents() { 
  fetch('/api/config').then(r=>r.json()).then(cfg=>{
    window.downtimeReasons = cfg.downtimeReasons || [];
    fetchAndRenderEvents(); 
    startAutoRefresh(); 
  }).catch(e => {
      console.error("Error fetching config for reasons:", e);
  });
}
function fetchAndRenderEvents() { 
  fetch('/api/events').then(r=>r.json()).then(events=>{
    eventData = events; 
    renderTable(); 
  }).catch(e => {
      console.error("Error fetching events:", e);
      const mainCardTbody = document.getElementById('tbody');
      if(mainCardTbody) mainCardTbody.innerHTML = "<tr><td colspan='14' style='color:red;text-align:center;'>Failed to load events.</td></tr>";
      const mobileEventsDiv = document.getElementById('mobileEvents');
      if(mobileEventsDiv) mobileEventsDiv.innerHTML = "<p style='color:red;text-align:center;'>Failed to load events.</p>";
  });
}
function cleanNote(val) { 
  if (!val) return "";
  let v = val.trim();
  if (v === "" || v === "," || v === ",," || v === "0,0," || v === "0.00,0,") return "";
  return v.replace(/^,+|,+$/g, ""); 
}
function minToHHMMSS(valStr) { 
  let val = parseFloat(valStr); 
  if (isNaN(val) || val <= 0.001) return "00:00:00"; 
  let totalSeconds = Math.round(val * 60);
  let h = Math.floor(totalSeconds / 3600);
  let m = Math.floor((totalSeconds % 3600) / 60); 
  let s = totalSeconds % 60;
  return (h < 10 ? "0" : "") + h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
}
function durationStrToHHMMSS(str) {
  if (!str || typeof str !== "string" || str.trim() === "") return ""; 
  let parts = str.split(":").map(Number);
  let h=0, m=0, s=0;
  if (parts.length === 3) { 
    [h, m, s] = parts;
  } else if (parts.length === 2) { 
    [m, s] = parts;
    h = Math.floor(m / 60); 
    m = m % 60;
  } else {
    return ""; 
  }
  if (isNaN(h) || isNaN(m) || isNaN(s)) return ""; 
  return (h < 10 ? "0":"") + h + ":" + (m < 10 ? "0":"") + m + ":" + (s < 10 ? "0":"") + s;
}
function renderTable() {
  let tbody = document.getElementById('tbody');
  let mobileDiv = document.getElementById('mobileEvents');
  if(!tbody || !mobileDiv) { console.error("Table body or mobile div not found in renderTable"); return; }

  tbody.innerHTML = ''; 
  mobileDiv.innerHTML = ''; 
  let stateMatcher = function(eventStateVal) { 
    if (stateFilter === "ALL") return true;
    if (stateFilter === "RUNNING") return eventStateVal === "1";
    if (stateFilter === "STOPPED") return eventStateVal === "0";
    return true; 
  };
  let isMobile = window.innerWidth <= 700;
  let displayData = eventData.slice().reverse(); 
  for (let i=0; i<displayData.length; ++i) {
    let csvLine = displayData[i];
    if (typeof csvLine !== 'string') continue; 
    let vals = csvLine.split(',');
    if (vals.length < 14) continue; 
    let ldate = vals[0], ltime = vals[1], lasset = vals[2], levent = vals[3], lstateVal = vals[4];
    let lavail = vals[5], lrun = vals[6], lstop = vals[7], lmtbf = vals[8], lmttr = vals[9], lsc = vals[10];
    let runDurStr = vals[11], stopDurStr = vals[12];
    let lnote = vals.slice(13).join(',').replace(/\n$/, ""); 
    let stopsInt = Math.round(Number(lsc));
    if (filterValue !== "ALL" && lasset !== filterValue) continue;
    if (!stateMatcher(lstateVal)) continue; 
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
      function td(txt) { let tdEl=document.createElement('td'); tdEl.innerHTML=txt; return tdEl; } 
      tr.appendChild(td(ldate));
      tr.appendChild(td(ltime));
      tr.appendChild(td(lasset));
      tr.appendChild(td(levent)); 
      tr.appendChild(td(lstateVal=="1" ? "<span style='color:#256029;font-weight:bold;'>RUNNING</span>" : "<span style='color:#b71c1c;font-weight:bold;'>STOPPED</span>"));
      tr.appendChild(td(Number(lavail).toFixed(2)));
      tr.appendChild(td(minToHHMMSS(lrun))); 
      tr.appendChild(td(minToHHMMSS(lstop)));
      tr.appendChild(td(minToHHMMSS(lmtbf)));
      tr.appendChild(td(minToHHMMSS(lmttr)));
      tr.appendChild(td(String(stopsInt))); 
      tr.appendChild(td(levent.toUpperCase()==="STOP" ? durationStrToHHMMSS(runDurStr) : "")); 
      tr.appendChild(td(levent.toUpperCase()==="START" ? durationStrToHHMMSS(stopDurStr) : ""));
      let tdNote = document.createElement('td');
      tdNote.innerHTML = `<span class='note'>${cleanNote(lnote)}</span>`;
      tdNote.innerHTML += ` <button class='notebtn' onclick='showNoteForm("${noteFormId}")'>Edit</button>`;
      tdNote.innerHTML += noteFormHtml;
      tr.appendChild(tdNote);
      tbody.appendChild(tr);
    } else { 
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
        <button class='notebtn' onclick='showNoteForm("${noteFormId}")'>Edit</button>
        ${noteFormHtml}</div>`; 
      mobileDiv.appendChild(card);
    }
  }
  const eventCountEl = document.getElementById('eventCount');
  if(eventCountEl) eventCountEl.innerHTML = "<b>Events Displayed:</b> " + (isMobile ? mobileDiv.children.length : tbody.children.length); 
  
  const eventTableEl = document.getElementById('eventTable');
  if(eventTableEl) eventTableEl.style.display = isMobile ? 'none' : '';
  if(mobileDiv) mobileDiv.style.display = isMobile ? '' : 'none';

  if (window.openNoteFormId) showNoteForm(window.openNoteFormId);
  if (!scrollInhibit) {
    if (isMobile) {
      if (mobileDiv) mobileDiv.scrollTop = 0;
    } else {
      window.scrollTo({top:0, behavior:'auto'}); 
    }
  }
}
function showNoteForm(noteFormId) {
  document.querySelectorAll('.noteform').forEach(f => f.style.display='none');
  let form = document.getElementById(noteFormId);
  if (form) { form.style.display = 'flex'; window.openNoteFormId = noteFormId; }
  stopAutoRefresh(); 
}
function hideNoteForm(noteFormId) {
  let form = document.getElementById(noteFormId);
  if (form) form.style.display = 'none';
  window.openNoteFormId = null;
  startAutoRefresh(); 
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
        fetchAndRenderEvents(); 
    } else {
        alert("Failed to save note. Status: " + r.status); 
    }
  }).catch(err => {
    console.error("Error saving note:", err);
    alert("Error saving note. Check console.");
  });
  form.style.display = 'none'; 
  window.openNoteFormId = null;
  startAutoRefresh(); 
  return false; 
}
function toggleScrollInhibit(btn) {
  scrollInhibit = !scrollInhibit;
  if(btn) btn.innerText = scrollInhibit ? "Enable Auto-Scroll" : "Inhibit Auto-Scroll"; 
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initializeEventPage);
} else {
  initializeEventPage();
}
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

  String mainContentHtml = "<div class='main card'>";
  mainContentHtml += "<div class='filterrow'><label for='channelFilter'>Filter by Channel:</label> <select id='channelFilter'><option value='ALL'>All Assets</option></select>";
  mainContentHtml += "<label for='stateFilter'>Event State:</label> <select id='stateFilter'><option value='ALL'>All</option><option value='RUNNING'>Running</option><option value='STOPPED'>Stopped</option></select>";
  mainContentHtml += "<span id='eventCount' style='margin-left:1em;'></span>";
  mainContentHtml += "<button class='scrollToggle' id='scrollBtn' type='button' onclick='toggleScrollInhibit(this)'>Inhibit Auto-Scroll</button></div>";
  
  mainContentHtml += "<div style='overflow-x:auto;'><table id='eventTable'><thead><tr>";
  mainContentHtml += "<th>Date</th><th>Time</th><th>Asset</th><th>Event</th><th>State</th><th>Avail(%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Run Duration</th><th>Stop Duration</th><th>Note</th>";
  mainContentHtml += "</tr></thead><tbody id='tbody'></tbody></table>";
  mainContentHtml += "<div id='mobileEvents'></div>";
  mainContentHtml += "</div></div></body></html>"; // Closing main card, main div, body, html
  server.sendContent(mainContentHtml);
  
  server.sendContent(""); // Finalize
}


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
  prefs.begin("assetmon", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  Serial.println("WiFi credentials cleared. Restarting in AP mode for reconfiguration.");

  String message = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>WiFi Reconfiguration</title>";
  message += "<style>body{font-family: Arial, sans-serif; margin: 20px; padding: 15px; border:1px solid #ddd; border-radius:5px; text-align:center;} h2{color:#333;}</style>";
  message += "</head><body>";
  message += "<h2>Device Restarting for WiFi Setup</h2>"; 
  message += "<p>The device will now restart and then create an Access Point named '<strong>AssetMonitor_Config</strong>'.</p>";
  message += "<p>Please connect your computer or phone to that WiFi network.</p>";
  message += "<p>Then, open a web browser and go to <strong>http://192.168.4.1</strong> to configure the new WiFi settings.</p>";
  message += "<p>The device will restart again after you save the new settings from that page.</p>";
  message += "<p>This page will attempt to redirect shortly...</p>";
  message += "<meta http-equiv='refresh' content='7;url=http://192.168.4.1/' />"; 
  message += "</body></html>";
  
  server.sendHeader("Connection", "close"); 
  server.send(200, "text/html", message);
  delay(1000); 
  
  ESP.restart(); 
}

String htmlAssetDetail(uint8_t idx) {
  if (idx >= config.assetCount || idx >= MAX_ASSETS) return "Invalid Asset Index"; 
  String assetNameStr = String(config.assets[idx].name); 
  String html = "<!DOCTYPE html><html><head><title>Asset Detail: ";
  html += assetNameStr + "</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:Roboto,Arial,sans-serif;margin:2em;background:#f3f7fa;} .card{background:#fff;padding:1.5em;border-radius:8px;box-shadow:0 2px 10px #0001;} a{color:#1976d2;text-decoration:none;} a:hover{text-decoration:underline;}</style>";
  html += "</head><body><div class='card'>";
  html += "<h1>Asset Detail: " + assetNameStr + "</h1>";
  html += "<p><strong>GPIO Pin:</strong> " + String(config.assets[idx].pin) + "</p>";
  html += "<p><a href='/'>Back to Dashboard</a></p>";
  html += "<p><a href='/analytics?asset=" + urlEncode(assetNameStr) + "'>View Analytics for this Asset</a></p>";
  html += "</div></body></html>";
  return html;
}

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
    }

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
        config.tzOffset = static_cast<int>(offsetHours * 3600);
        config.tzOffset = constrain(config.tzOffset, -12 * 3600, 14 * 3600);
    }
    for (int i = 0; i < 5; ++i) {
      String key = "reason" + String(i);
      if (server.hasArg(key)) {
        String v = server.arg(key);
        strncpy(config.downtimeReasons[i], v.c_str(), sizeof(config.downtimeReasons[i]) - 1);
        config.downtimeReasons[i][sizeof(config.downtimeReasons[i])-1] = '\0';
      }
    }
    if (server.hasArg("longStopThreshold")) {
      config.longStopThresholdSec = constrain(server.arg("longStopThreshold").toInt() * 60, 60, 3600 * 24); 
    }

    saveConfig(); 
    
    server.sendHeader("Location", "/config#saveNotice"); 
    server.send(303);
    
    if(server.client().connected()) { 
        server.client().stop(); 
    }
    delay(1000); 
    
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request: Missing assetCount or other required fields");
  }
}

void handleClearLog() { 
  if(SPIFFS.remove(LOG_FILENAME)) {
    Serial.println("Log file cleared.");
  } else {
    Serial.println("Failed to clear log file.");
  }
  server.sendHeader("Location", "/config"); 
  server.send(303); 
}

void handleExportLog() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f || f.size() == 0) { 
    server.send(404, "text/plain", "No log file or log is empty."); 
    if(f) f.close(); 
    return; 
  }
  time_t now_time = time(nullptr); 
  struct tm * ti = localtime(&now_time);
  char fn[64];
  strftime(fn, sizeof(fn), "AssetMonitorLog-%Y%m%d-%H%M%S.csv", ti); 
  
  server.sendHeader("Content-Type", "text/csv"); 
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + fn + "\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN); 
  
  server.send(200, "text/csv", ""); 
  server.sendContent("Date,Time,Asset,Event,State,Availability (%),Total Runtime (min),Total Downtime (min),MTBF (min),MTTR (min),No. of Stops,Run Duration (mm:ss),Stop Duration (mm:ss),Note\n");
  
  const size_t chunkSize = 1024; 
  char buffer[chunkSize + 1]; 
  while (f.available()) {
    size_t bytesRead = f.readBytes(buffer, chunkSize);
    if (bytesRead > 0) {
      buffer[bytesRead] = '\0'; 
      server.sendContent(String(buffer)); 
    }
  }
  f.close();
  server.sendContent(""); 
  Serial.println("Log file exported.");
}

void handleApiSummary() {
  String json = "{\"assets\":[";
  time_t current_time_epoch = time(nullptr); 
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i >= MAX_ASSETS) continue; 
    if (i > 0) json += ",";
    AssetState& as = assetStates[i];
    bool current_pin_state = digitalRead(config.assets[i].pin); 
    
    unsigned long current_period_runningTime = as.runningTime; 
    unsigned long current_period_stoppedTime = as.stoppedTime;
    
    if (as.lastState == false ) { 
      current_period_runningTime += (current_time_epoch - as.lastChangeTime);
    } else { 
      current_period_stoppedTime += (current_time_epoch - as.lastChangeTime);
    }

    float avail = (current_period_runningTime + current_period_stoppedTime) > 0 ? (100.0 * current_period_runningTime / (current_period_runningTime + current_period_stoppedTime)) : ( (current_pin_state == false) ? 100.0 : 0.0);
    float total_runtime_min = current_period_runningTime / 60.0;
    float total_downtime_min = current_period_stoppedTime / 60.0;
    
    float mtbf_val = (as.stopCount > 0) ? (float)current_period_runningTime / as.stopCount / 60.0 : total_runtime_min; 
    float mttr_val = (as.stopCount > 0) ? (float)current_period_stoppedTime / as.stopCount / 60.0 : 0; 

    json += "{";
    json += "\"name\":\"" + String(config.assets[i].name) + "\",";
    json += "\"pin\":" + String(config.assets[i].pin) + ",";
    json += "\"state\":" + String(current_pin_state ? 0 : 1) + ","; 
    json += "\"availability\":" + String(avail, 2) + ",";
    json += "\"total_runtime\":" + String(total_runtime_min, 2) + ","; 
    json += "\"total_downtime\":" + String(total_downtime_min, 2) + ","; 
    json += "\"mtbf\":" + String(mtbf_val, 2) + ","; 
    json += "\"mttr\":" + String(mttr_val, 2) + ","; 
    json += "\"stop_count\":" + String(as.stopCount) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleApiEvents() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  String json = "["; 
  if (f && f.size() > 0) { 
    String line_content; 
    bool firstEntry = true;
    while (f.available()) {
      line_content = f.readStringUntil('\n');
      line_content.trim(); 
      if (line_content.length() < 5) continue; 
      if (!firstEntry) {
        json += ",";
      }
      firstEntry = false;
      String escapedLine = "";
      for (unsigned int char_idx = 0; char_idx < line_content.length(); ++char_idx) { 
        char c = line_content.charAt(char_idx);
        if (c == '"') escapedLine += "\\\"";
        else if (c == '\\') escapedLine += "\\\\";
        else if (c < 32 || c > 126) {} 
        else escapedLine += c;
      }
      json += "\"" + escapedLine + "\"";
    }
    f.close();
  }
  json += "]"; 
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate"); 
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "application/json", json);
}

void handleApiConfig() {
  String json = "{";
  json += "\"assetCount\":" + String(config.assetCount) + ",";
  json += "\"maxEvents\":" + String(config.maxEvents) + ",";
  json += "\"tzOffset\":" + String(config.tzOffset) + ","; 
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
  for (int i = 0; i < 5; ++i) { 
    if (i > 0) json += ",";
    json += "\"" + String(config.downtimeReasons[i]) + "\"";
  }
  json += "]"; 
  json += ",\"longStopThresholdSec\":" + String(config.longStopThresholdSec);
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiNote() {
  if (server.method() == HTTP_POST && server.hasArg("date") && server.hasArg("time") && server.hasArg("asset")) {
    String dateVal = server.arg("date"); 
    String timeVal = server.arg("time"); 
    String assetVal = server.arg("asset"); 
    String noteVal = server.arg("note"); 
    String reasonVal = server.hasArg("reason") ? server.arg("reason") : ""; 
    Serial.printf("API Note Received: Date=%s, Time=%s, Asset=%s, Reason=%s, Note=%s\n",
                  dateVal.c_str(), timeVal.c_str(), assetVal.c_str(), reasonVal.c_str(), noteVal.c_str());
    updateEventNote(dateVal, timeVal, assetVal, noteVal, reasonVal);
    server.sendHeader("Location", "/events"); 
    server.send(303); 
    return;
  }
  server.send(400, "text/plain", "Bad Request: Missing required parameters for note update.");
}

void updateEventNote(String date_str, String time_str, String assetName_str, String note_str, String reason_str) { 
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f) { Serial.println("updateEventNote: Failed to open log file for reading."); return; }
  
  String tempLogContent = ""; 
  bool updated = false;
  
  String combinedNewNote = "";
  if (reason_str.length() > 0 && note_str.length() > 0) {
    combinedNewNote = reason_str + " - " + note_str;
  } else if (reason_str.length() > 0) {
    combinedNewNote = reason_str;
  } else {
    combinedNewNote = note_str;
  }
  combinedNewNote.replace(",", " "); 
  combinedNewNote.replace("\n", " "); 
  combinedNewNote.replace("\r", " "); 

  String lineBuffer = "";
  while (f.available()) {
    char char_read = f.read();
    if (char_read == '\n' || !f.available()) { 
        if (char_read != '\n' && !f.available()) lineBuffer += char_read; 

        String current_line_trimmed = lineBuffer;
        current_line_trimmed.trim(); 
        
        String original_line_to_write = lineBuffer + (char_read == '\n' ? "\n" : ""); 

        if (current_line_trimmed.length() < 5) { 
          tempLogContent += original_line_to_write; 
          lineBuffer = ""; 
          continue;
        }

        String parts[3]; 
        int partIdx = 0;
        int lastComma = -1;
        for(int k=0; k<3; ++k) { 
            int nextComma = current_line_trimmed.indexOf(',', lastComma + 1);
            if (nextComma == -1) { 
                parts[partIdx++] = current_line_trimmed.substring(lastComma + 1); 
                break; 
            }
            parts[partIdx++] = current_line_trimmed.substring(lastComma + 1, nextComma);
            lastComma = nextComma;
        }
        for(int k=partIdx; k<3; ++k) parts[k] = ""; 


        if (parts[0] == date_str && parts[1] == time_str && parts[2] == assetName_str) {
          int finalCommaIndex = -1;
          int commaCount = 0;
          for(int char_idx_inner = 0; char_idx_inner < current_line_trimmed.length(); char_idx_inner++){ 
              if(current_line_trimmed.charAt(char_idx_inner) == ','){
                  commaCount++;
                  if(commaCount == 13) { 
                      finalCommaIndex = char_idx_inner;
                      break;
                  }
              }
          }
          if (finalCommaIndex != -1) {
            String lineBeforeNote = current_line_trimmed.substring(0, finalCommaIndex + 1);
            tempLogContent += lineBeforeNote + combinedNewNote + "\n";
          } else { 
            tempLogContent += current_line_trimmed + "," + combinedNewNote + "\n"; 
          }
          updated = true;
          Serial.println("Found and updated event line in log.");
        } else {
          tempLogContent += original_line_to_write; 
        }
        lineBuffer = ""; 
    } else {
        lineBuffer += char_read;
    }
  }
  f.close();

  if (updated) {
    File f2 = SPIFFS.open(LOG_FILENAME, FILE_WRITE); 
    if (!f2) { Serial.println("updateEventNote: Failed to open log file for writing."); return; }
    f2.print(tempLogContent);
    f2.close();
    Serial.println("Log file rewritten with updated note.");
  } else {
    Serial.println("Event to update note for was not found in log.");
  }
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }
