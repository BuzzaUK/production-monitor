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
Preferences prefs;
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
    String ssid = server.arg("ssid");
    String pass = server.arg("password");
    strncpy(wifi_ssid, ssid.c_str(), 32);
    wifi_ssid[32] = '\0';
    strncpy(wifi_pass, pass.c_str(), 64);
    wifi_pass[64] = '\0';
    prefs.begin("assetmon", false);
    prefs.putString("ssid", wifi_ssid);
    prefs.putString("pass", wifi_pass);
    prefs.end();
    server.send(200, "text/html", "<h2>Saved! Rebooting...</h2>");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing WiFi credentials");
  }
}

void startConfigPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AssetMonitor_Config", "setpassword");
  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", wifiConfigHTML()); });
  server.on("/wifi_save_config", HTTP_POST, handleWifiConfigPost);
  server.begin();
  while (true) { server.handleClient(); delay(10); }
}

void setupWiFiSmart() {
  prefs.begin("assetmon", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.length() == 0) { startConfigPortal(); return; }
  strncpy(wifi_ssid, ssid.c_str(), 32); wifi_ssid[32] = '\0';
  strncpy(wifi_pass, pass.c_str(), 64); wifi_pass[64] = '\0';
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_pass);
  Serial.printf("Connecting to %s", wifi_ssid);
  for (int i=0; i<20 && WiFi.status()!=WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed. Starting config portal.");
    startConfigPortal();
  }
}
// --- End WiFi Config Section ---

// CONFIG LOAD/SAVE
// CONFIG LOAD/SAVE (Corrected: removed isBegun, refined local Prefs handling)
void loadConfig() {
  Preferences localPrefs; 
  bool prefsOpenedForRead = localPrefs.begin("assetmon", true); // true for read-only

  if (!prefsOpenedForRead) {
    prefsOpenedForRead = localPrefs.begin("assetmon", false); // false for read/write
  }

  if (!prefsOpenedForRead) {
    // Fallback to hardcoded defaults if Preferences cannot be opened at all
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
      goto use_defaults_and_save; 
    }
    if (config.longStopThresholdSec == 0 && config.maxEvents !=0 ) { 
        config.longStopThresholdSec = 5*60; 
    }
  } else {
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
    
    localPrefs.end(); 
    saveConfig(); 
  }
  
  if (prefsOpenedForRead) {
    localPrefs.end(); 
  }
}

void saveConfig() { 
  Preferences localSavePrefs; 
  
  if (!localSavePrefs.begin("assetmon", false)) { 
    return; 
  }
  
  localSavePrefs.putBytes("cfg", &config, sizeof(config));
  localSavePrefs.end(); 
}

void setup() {
  Serial.begin(115200); // Keep this for general status/error messages if you want

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS.begin() failed! Halting."); // Keep this critical error message
    return; 
  }
  // Serial.println("SPIFFS initialized."); // Optional: remove if too verbose

  if (!prefs.begin("assetmon", false)) { 
    Serial.println("prefs.begin() failed! Default settings will be used for this session and may not save."); // Keep this
  } else {
    // Serial.println("Preferences initialized."); // Optional: remove if too verbose
  }

  loadConfig();
  // Serial.println("Configuration loaded/initialized."); // Optional

  setupWiFiSmart(); 
  setupTime();
  // Serial.println("WiFi and Time setup complete."); // Optional

  // Serial.printf("Initializing %u assets...\n", config.assetCount); // Optional
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
      // Serial.printf("Asset %u ('%s', pin %u) initialized.\n", i, config.assets[i].name, config.assets[i].pin); // Optional
    }
  }
// WIFI & TIME (setupWiFi replaced)
  void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
  tzset();
  Serial.print("Waiting for NTP time sync...");
  time_t now = time(nullptr); int retry = 0;
  while (now < 8 * 3600 * 2 && retry < 30) { delay(200); Serial.print("."); now = time(nullptr); retry++; }
  Serial.println(" done");
}
  // Main pages
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
  server.on("/dashboard", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
  server.on("/config", HTTP_GET, []() { server.send(200, "text/html", htmlConfig()); });
  server.on("/events", HTTP_GET, []() { server.send(200, "text/html", htmlEvents()); });
  server.on("/asset", HTTP_GET, []() {
    if (server.hasArg("idx")) {
      uint8_t idx = server.arg("idx").toInt();
      if (idx < config.assetCount) {
        server.send(200, "text/html", htmlAssetDetail(idx));
        return;
      }
    }
    server.send(404, "text/plain", "Asset not found");
  });

  // Analytics
  server.on("/analytics", HTTP_GET, []() { server.send(200, "text/html", htmlAnalytics()); });
  server.on("/analytics-compare", HTTP_GET, []() { server.send(200, "text/html", htmlAnalyticsCompare()); });
  server.on("/reconfigure_wifi", HTTP_POST, handleWiFiReconfigurePost);

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
  Serial.println("Web server started. Device is ready."); // Keep this useful status message
}

void loop() {
  server.handleClient();
  time_t now = time(nullptr);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    bool state = digitalRead(config.assets[i].pin);
    if (state != assetStates[i].lastState) {
      unsigned long elapsed = now - assetStates[i].lastChangeTime;
      unsigned long runDuration = 0;
      unsigned long stopDuration = 0;
      if (state) {
        assetStates[i].stoppedTime += elapsed;
        assetStates[i].runCount++;
        stopDuration = elapsed;
        assetStates[i].lastStopDuration = stopDuration;
        assetStates[i].lastRunDuration = 0;
        logEvent(i, state, now, nullptr, 0, stopDuration);
      }
      else {
        assetStates[i].runningTime += elapsed;
        assetStates[i].stopCount++;
        runDuration = elapsed;
        assetStates[i].lastRunDuration = runDuration;
        assetStates[i].lastStopDuration = 0;
        logEvent(i, state, now, nullptr, runDuration, 0);
      }
      assetStates[i].lastState = state;
      assetStates[i].lastChangeTime = now;
      assetStates[i].lastEventTime = now;
    }
  }
  delay(200);
}

// LOGGING (now with runDuration, stopDuration fields)

void logEvent(uint8_t assetIdx, bool state, time_t now, const char* note, unsigned long runDuration, unsigned long stopDuration) {
  AssetState& as = assetStates[assetIdx];
  unsigned long runningTime = as.runningTime;
  unsigned long stoppedTime = as.stoppedTime;
  if (as.lastState) runningTime += now - as.lastChangeTime;
  else stoppedTime += now - as.lastChangeTime;

  float avail = (runningTime + stoppedTime) > 0 ? (100.0 * runningTime / (runningTime + stoppedTime)) : 0;
  float total_runtime_min = runningTime / 60.0;
  float total_downtime_min = stoppedTime / 60.0;
  float mtbf = (as.stopCount > 0) ? runningTime / as.stopCount : 0;
  float mttr = (as.stopCount > 0) ? stoppedTime / as.stopCount : 0;
  mtbf = mtbf / 60.0; mttr = mttr / 60.0;

  // PATCH: This uses localtime (BST/GMT set by setupTime())
  struct tm * ti = localtime(&now);
  char datebuf[11], timebuf[9];
  strftime(datebuf, sizeof(datebuf), "%d/%m/%Y", ti);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti);

  File f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
  if (!f) { Serial.println("Failed to open log file for writing!"); return; }
  f.printf("%s,%s,%s,%s,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%s,%s,%s\n",
    datebuf, timebuf, config.assets[assetIdx].name,
    state ? "START" : "STOP",
    state, avail, total_runtime_min, total_downtime_min, mtbf, mttr,
    as.stopCount,
    (runDuration > 0 ? formatMMSS(runDuration).c_str() : ""),
    (stopDuration > 0 ? formatMMSS(stopDuration).c_str() : ""),
    note ? note : ""
  );
  f.close();
  as.lastEventTime = now;
  Serial.println("Wrote event to log.");
}

// MM:SS format helper for durations
String formatMMSS(unsigned long seconds) {
  if (seconds == 0) return "";
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

// --- htmlDashboard() with proper asset encoding and working Compare Assets button ---

String htmlDashboard() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>";
  html += "body{font-family:Roboto,Arial,sans-serif;background:#f3f7fa;margin:0;padding:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.3rem 0 1.3rem 2rem;text-align:left;font-size:2em;font-weight:700;box-shadow:0 2px 10px #0001;}";
  html += ".nav{display:flex;justify-content:center;align-items:center;gap:1rem;margin:1.5rem 0 1rem 0;flex-wrap:wrap;}";
  html += ".nav .nav-btn{background:#fff;color:#1976d2;border:none;border-radius:8px;padding:0.7em 1.3em;font-size:1.13em;font-weight:700;box-shadow:0 2px 12px #1976d222;cursor:pointer;transition:.2s;margin-bottom:0.5em;}";
  html += ".nav .nav-btn:hover{background:#e3f0fc;}";
  html += ".main{max-width:1200px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += "#chart-container{width:100%;overflow-x:auto;}";
  html += ".statrow{display:flex;gap:1.5em;flex-wrap:wrap;justify-content:center;margin:2em 0 2em 0;}";
  html += ".stat{flex:1 1 220px;border-radius:10px;padding:1.2em;text-align:left;font-size:1.1em;margin:0.4em 0;box-shadow:0 2px 8px #0001;font-weight:500;background:#f5f7fa;border:2px solid #e0e0e0;min-width:220px;max-width:300px;transition:background 0.2s, border-color 0.2s;}";
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
  html += "@media (max-width:700px){header{font-size:1.3em;padding:1em 0 1em 1em;}.nav{flex-direction:column;align-items:center;margin:1em 0 1em 0;gap:0.4em;}.card{padding:0.7em;}.statrow{gap:0.4em;}}";
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
  html += "<th>Name</th><th>State</th><th>Avail (%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th></th></tr></thead><tbody></tbody></table>";
  html += "</div></div>";
  html += "<script>";
  html += R"rawliteral(
// Format float minutes as hh:mm:ss
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
    let assets = data.assets;
    let rows = tbody.rows;
    let statrow = document.getElementById('statrow');
    statrow.innerHTML = '';
    let n=assets.length;
    for(let i=0;i<n;++i){
      let asset = assets[i];
      let stateClass = asset.state==1 ? "running" : "stopped";
      // Table row
      let row = rows[i];
      if (!row) {
        row = tbody.insertRow();
        for (let j=0;j<9;++j) row.insertCell();
      }
      let v0 = asset.name,
          v1 = `<span style="color:${asset.state==1?'#256029':'#b71c1c'};font-weight:bold">${asset.state==1?'RUNNING':'STOPPED'}</span>`,
          v2 = asset.availability.toFixed(2),
          v3 = formatHHMMSS(asset.total_runtime),
          v4 = formatHHMMSS(asset.total_downtime),
          v5 = formatHHMMSS(asset.mtbf),
          v6 = formatHHMMSS(asset.mttr),
          v7 = asset.stop_count,
          v8 = `<form action='/analytics' method='GET' style='display:inline;'><input type='hidden' name='asset' value="${encodeURIComponent(asset.name)}"><button type='submit' class='nav-btn'>Analytics</button></form>`;
      let vals = [v0,v1,v2,v3,v4,v5,v6,v7,v8];
      for(let j=0;j<9;++j) row.cells[j].innerHTML = vals[j];
      // Stat card
      let statHtml = `<div class='stat ${stateClass}'><b>${asset.name}</b><br>Avail: ${asset.availability.toFixed(1)}%<br>Run: ${formatHHMMSS(asset.total_runtime)}<br>Stops: ${asset.stop_count}</div>`;
      statrow.innerHTML += statHtml;
    }
    while (rows.length > n) tbody.deleteRow(rows.length-1);
    let availData=[], names=[], runtimeData=[], downtimeData=[];
    for (let asset of assets) {
      availData.push(asset.availability);
      runtimeData.push(asset.total_runtime);
      downtimeData.push(asset.total_downtime);
      names.push(asset.name);
    }
    let ctx = document.getElementById('barChart').getContext('2d');
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
  });
}
updateDashboard(); setInterval(updateDashboard, 5000);
)rawliteral";
  html += "</script></body></html>";
  return html;
}

// Full patched htmlAnalytics() function, including:
// - uses local time
// - KPI boxes use filtered events
// - x-axis is readable
// - long stop threshold is fetched from config
// Uses data from the Events log table

String htmlAnalytics() {
  String assetName = server.hasArg("asset") ? urlDecode(server.arg("asset")) : "";
  String html = "<!DOCTYPE html><html lang='en'><head><title>Analytics: ";
  html += assetName + "</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}th,td{text-align:left;}header{background:#1976d2;color:#fff;padding:1.3rem 0;text-align:center;box-shadow:0 2px 10px #0001;font-size:2em;}.nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;}.nav button, .nav a{background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;text-decoration:none;display:inline-block;}.nav button:hover, .nav a:hover{background:#e3f0fc;}.nav .analytics-btn{background:#fff;color:#ffa726;border:1.5px solid #ff9800;}.nav .analytics-btn:hover{background:#fff3e0;}.main{max-width:1100px;margin:1rem auto;padding:1rem;}.metrics{display:flex;gap:2em;flex-wrap:wrap;margin-bottom:1.2em;}.metric{background:#f8fafb;border-radius:7px;padding:0.9em 1.5em;font-size:1.2em;margin:0.3em 0;box-shadow:0 2px 8px #0001;font-weight:500;}.controls{display:flex;gap:1em;align-items:center;margin-bottom:1.5em;flex-wrap:wrap;}.controls label{font-weight:500;}.controls input[type='datetime-local']{padding:0.3em;font-size:1em;}.controls .toggle{margin-left:0.5em;}.controls .export-btn{margin-left:0.5em;padding:0.4em 1em;background:#1976d2;color:#fff;border:none;border-radius:4px;cursor:pointer;}.controls .export-btn:hover{background:#135da0;}.chartcard{background:#fff;border-radius:10px;box-shadow:0 2px 10px #0001;margin:1.5em 0;padding:1em;}.tablecard{overflow-x:auto;}@media(max-width:700px){header{font-size:1.3em;}.metrics{gap:1em;flex-direction:column;}.main{padding:0.6em;}.chartcard{padding:0.7em;}}</style>";
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
          "<th>Date</th><th>Time</th><th>Asset</th><th>Event</th><th>Avail(%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Run Duration</th><th>Stop Duration</th><th>Note</th></tr></thead>"
          "<tbody id='recentEvents'></tbody></table></div>";
  html += "<script>";
  html += R"rawliteral(
console.log('Analytics script started (v14 - Enhanced MTBF/MTTR tooltips).');

// --- Utility Functions ---
function floatMinToMMSS(val) { 
  if (typeof val === "string") val = parseFloat(val);
  if (isNaN(val) || val < 0) { // Handle NaN or negative by returning 00:00 or similar default
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
  if (parts.length !== 2 && parts.length !==3) return 0; // Allow h:mm:ss too
  let h = 0, m = 0, s = 0;
  if (parts.length === 3) {
    h = parseInt(parts[0], 10); m = parseInt(parts[1], 10); s = parseInt(parts[2], 10);
  } else { // mm:ss
    m = parseInt(parts[0], 10); s = parseInt(parts[1], 10);
  }
  return (isNaN(h) ? 0 : h * 3600) + (isNaN(m) ? 0 : m * 60) + (isNaN(s) ? 0 : s);
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
    kpiDiv.innerHTML =
      `<div class='metric'>Stops: <b>${latestEvent[10]}</b></div>
       <div class='metric'>Runtime: <b>${floatMinToMMSS(latestEvent[6])}</b></div>
       <div class='metric'>Downtime: <b>${floatMinToMMSS(latestEvent[7])}</b></div>
       <div class='metric'>Availability: <b>${parseFloat(latestEvent[5]).toFixed(2)}%</b></div>
       <div class='metric'>MTBF: <b>${floatMinToMMSS(latestEvent[8])}</b></div>
       <div class='metric'>MTTR: <b>${floatMinToMMSS(latestEvent[9])}</b></div>`;
  } catch (e) { console.error('Error rendering KPIs:', e); kpiDiv.innerHTML = "<div class='metric'>Error rendering KPIs</div>"; }
}
function renderEventChart() {
  if (!allEvents || allEvents.length === 0) { if (eventChart) { eventChart.destroy(); eventChart = null; } return; }
  let fromDate, toDate;
  try { fromDate = new Date(document.getElementById('fromTime').value); toDate = new Date(document.getElementById('toTime').value); }
  catch (e) { console.error('Error parsing date/time input values:', e); return; }
  const showStart = document.getElementById('showStart').checked; const showStop = document.getElementById('showStop').checked;
  const showMTBF = document.getElementById('showMTBF').checked; const showMTTR = document.getElementById('showMTTR').checked;
  
  filteredEventsGlobal = allEvents.filter(eventRow => {
    try {
      const eventDate = parseEventDate(eventRow); if (eventDate.getTime() === 0) return false; 
      if (eventDate < fromDate || eventDate > toDate) return false; if (!eventRow[3]) return false; 
      if (eventRow[3].trim().toUpperCase() === "START" && !showStart) return false;
      if (eventRow[3].trim().toUpperCase() === "STOP" && !showStop) return false; return true;
    } catch (e) { return false; }
  });
  renderKPIs(filteredEventsGlobal); 
  if (filteredEventsGlobal.length === 0) { if (eventChart) { eventChart.destroy(); eventChart = null; } return; }

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
      if (eventType === "STOP" && mmssToSeconds(durationForStopDecision) >= 300) return "#c62828"; 
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
      if (eventType === "STOP" && mmssToSeconds(durationForStopDecision) >= 300) { defaultSize = 12; }
      return defaultSize;
    });

    let datasets = [{
      label: 'Availability (%)', data: avail, yAxisID: 'y', stepped: true, tension: 0,
      pointRadius: pointSizes, pointBackgroundColor: pointColors, pointBorderColor: pointColors, showLine: true,
      segment: {
        borderColor: ctx => {
          const stateValue = stateArr[ctx.p0DataIndex];
          if (stateValue === "1") return "#43a047"; if (stateValue === "0") return "#c62828"; 
          return "#000000"; 
        },
        borderWidth: 3
      }
    }];
    if (showMTBF) datasets.push({ label: 'MTBF', data: mtbfValues, yAxisID: 'y1', borderColor: "#1565c0", borderWidth: 2, tension: 0, pointRadius: 4 });
    if (showMTTR) datasets.push({ label: 'MTTR', data: mttrValues, yAxisID: 'y1', borderColor: "#FFD600", borderWidth: 2, tension: 0, pointRadius: 4 });
    
    if (eventChart) eventChart.destroy();
    const ctx = document.getElementById('eventChart').getContext('2d');
    eventChart = new Chart(ctx, {
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
                  const currentValue = tooltipItem.raw; // Raw numerical value (float minutes)
                  const formattedCurrentValue = floatMinToMMSS(currentValue);
                  lines.push(`${datasetLabel}: ${formattedCurrentValue}`);

                  if (idx > 0) {
                    const previousValue = tooltipItem.dataset.data[idx - 1];
                    if (typeof previousValue === 'number' && typeof currentValue === 'number') {
                        const change = currentValue - previousValue;
                        if (Math.abs(change) > 1e-7) { // Tolerance for float comparison
                            const formattedChange = floatMinToMMSS(Math.abs(change));
                            let changeIndicator = "";
                            if (datasetLabel === 'MTBF') {
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
                return lines.length > 0 ? lines : null; // Return null if no lines to show for this dataset item
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
                    if (value > 100) return undefined; 
                    if (value === 100) return 100;
                    if (value < 100 && value >= 0 && (value % (this.chart.options.scales.y.ticks.stepSize || 20) === 0) ) return value;
                    return undefined; 
                }
            }
          }, 
          y1: { type: 'linear', display: true, position: 'right', title: { display: true, text: 'MTBF/MTTR' }, beginAtZero: true, grid: { drawOnChartArea: false }, ticks: { callback: val => floatMinToMMSS(val) } }
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
// --- Initialisation ---
if (document.readyState === 'loading') { document.addEventListener('DOMContentLoaded', fetchAnalyticsData); }
else { fetchAnalyticsData(); }
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
  html += "header{background:#1976d2;color:#fff;padding:1.3rem 0;text-align:center;box-shadow:0 2px 10px #0001;}";
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;}";
  html += ".nav button{background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;}";
  html += ".nav button:hover{background:#e3f0fc;}";
  html += ".main{max-width:1100px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += ".flexrow{display:flex;flex-wrap:wrap;gap:2em;}";
  html += ".chartcard{flex:1 1 320px;min-width:250px;}";
  html += ".tablecard{overflow-x:auto;}";
  html += "th, td { text-align: left !important; }"; // <-- Left justify all table headers and cells
  html += "@media(max-width:700px){.flexrow{flex-direction:column;gap:1em;}.card{padding:0.7em;}}";
  html += "</style></head><body>";
  html += "<header><div style='font-size:1.6em;font-weight:700;'>Compare Assets</div></header>";
  html += "<nav class='nav'>";
  html += "<form action='/'><button type='submit'>Dashboard</button></form>";
  html += "<form action='/events'><button type='submit'>Event Log</button></form>";
  html += "<form action='/export_log'><button type='submit'>Export CSV</button></form>";
  html += "</nav>";
  html += "<div class='main'>";
  html += "<div class='flexrow'>";
  html += "<div class='card chartcard'><canvas id='barAvail'></canvas></div>";
  html += "<div class='card chartcard'><canvas id='barStops'></canvas></div>";
  html += "<div class='card chartcard'><canvas id='barMTBF'></canvas></div>";
  html += "<div class='card chartcard'><canvas id='pieReasons'></canvas></div>";
  html += "</div>";
  html += "<div class='card tablecard'>";
  html += "<h3>Last Event Log</h3><table style='width:100%;'><thead><tr>"
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
let allEvents = [], allAssets = [], downtimeReasons = [];
function minToHMS(val) {
  // Convert float minutes to h:mm:ss format
  if (isNaN(val) || val <= 0) return "0:00:00";
  let totalSeconds = Math.round(val * 60);
  let h = Math.floor(totalSeconds / 3600);
  let m = Math.floor((totalSeconds % 3600) / 60);
  let s = totalSeconds % 60;
  return h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
}
function fetchCompare() {
  fetch('/api/config').then(r=>r.json()).then(cfg=>{
    downtimeReasons = cfg.downtimeReasons||[];
    allAssets = cfg.assets.map(a=>a.name);
    fetch('/api/events').then(r=>r.json()).then(events=>{
      allEvents = events
        .map(l=>l.split(','))
        .filter(v=>v.length>13 && allAssets.includes(v[2]));
      renderCompareCharts();
      renderCompareTable();
    });
  });
}
function lastMetric(events, idx) {
  return events.length ? parseFloat(events[events.length-1][idx]) : 0;
}
function renderCompareCharts() {
  let byAsset = {};
  allAssets.forEach(a=>{byAsset[a]=[];});
  for (let e of allEvents) byAsset[e[2]].push(e);
  let labels = allAssets;
  let avail = labels.map(a=>lastMetric(byAsset[a],5));
  let stops = labels.map(a=>byAsset[a].filter(e=>e[3]=="STOP").length);
  let mtbf = labels.map(a=>lastMetric(byAsset[a],8));
  // Pie: all downtime reasons across all assets
  let reasons = {};
  for (let e of allEvents) {
    let note = e[13]||"";
    let res = "";
    if (note.indexOf(" - ")>-1) res = note.split(" - ")[0].trim();
    else res = note.trim();
    if (res && downtimeReasons.includes(res)) reasons[res] = (reasons[res]||0)+1;
  }
  // Bar: Avail
  new Chart(document.getElementById('barAvail').getContext('2d'), {
    type:'bar',data:{labels:labels,datasets:[{label:'Availability (%)',data:avail,backgroundColor:'#42a5f5'}]},
    options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true,max:100}}}
  });
  // Bar: Stops
  new Chart(document.getElementById('barStops').getContext('2d'), {
    type:'bar',data:{labels:labels,datasets:[{label:'Stops',data:stops,backgroundColor:'#ef5350'}]},
    options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}
  });
  // Bar: MTBF
  new Chart(document.getElementById('barMTBF').getContext('2d'), {
    type:'bar',data:{labels:labels,datasets:[{label:'MTBF (min)',data:mtbf,backgroundColor:'#66bb6a'}]},
    options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}
  });
  // Pie: downtime reasons
  new Chart(document.getElementById('pieReasons').getContext('2d'), {
    type:'pie',data:{
      labels:Object.keys(reasons),datasets:[{data:Object.values(reasons),backgroundColor:['#ffa726','#ef5350','#66bb6a','#42a5f5','#ab47bc']}]
    },options:{responsive:true,maintainAspectRatio:false}
  });
}
function renderCompareTable() {
  let tb = document.getElementById('compareTable');
  tb.innerHTML = "";
  let byAsset = {};
  allAssets.forEach(a=>{byAsset[a]=[];});
  for (let e of allEvents) byAsset[e[2]].push(e);
  for (let a of allAssets) {
    let evs = byAsset[a], e = evs.length?evs[evs.length-1]:null;
    tb.innerHTML += `<tr>
      <td>${a}</td>
      <td>${e?parseFloat(e[5]).toFixed(2):"-"}</td>
      <td>${e?minToHMS(parseFloat(e[6])):"-"}</td>
      <td>${e?minToHMS(parseFloat(e[7])):"-"}</td>
      <td>${evs.filter(e=>e[3]=="STOP").length}</td>
      <td>${e?minToHMS(parseFloat(e[8])):"-"}</td>
      <td>${e?minToHMS(parseFloat(e[9])):"-"}</td>
    </tr>`;
  }
}
fetchCompare();
)rawliteral";
  html += "</script>";
  html += "</div></body></html>";
  return html;
}

// EVENTS PAGE (with fixed note form layout)

String htmlEvents() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Event Log</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001;}";
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0 0 0;flex-wrap:wrap;}";
  html += ".nav button{background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;transition:.2s;cursor:pointer;margin-bottom:0.7em;}";
  html += ".nav button:hover{background:#e3f0fc;}";
  html += ".main{max-width:1100px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += ".filterrow{display:flex;gap:1em;align-items:center;margin-bottom:1em;flex-wrap:wrap;}";
  html += ".filterrow label{font-weight:bold;}";
  html += ".scrollToggle{margin-left:auto;font-size:1em;}";
  html += "table{width:100%;border-collapse:collapse;font-size:1em;margin-top:0.8em;}";
  html += "th,td{padding:0.7em 0.5em;text-align:left;border-bottom:1px solid #eee;}";
  html += "th{background:#2196f3;color:#fff;}";
  html += "tr{background:#fcfcfd;} tr:nth-child(even){background:#f3f7fa;}";
  html += ".note{font-style:italic;color:#555;white-space:normal;word-break:break-word;display:block;max-width:260px;}";
  html += ".notebtn{padding:2px 8px;font-size:1em;border-radius:4px;background:#1976d2;color:#fff;border:none;cursor:pointer;margin-left:0.5em;} .notebtn:hover{background:#0d47a1;}";
  html += ".noteform{display:flex;flex-direction:column;background:#e3f0fc;padding:0.7em;margin:0.5em 0 0.5em 0;border-radius:8px;width:100%;box-sizing:border-box;}";
  html += ".noteform label{margin-bottom:0.3em;}";
  html += ".noteform select,.noteform input[type=\"text\"]{width:100%;margin-bottom:0.4em;font-size:1em;padding:0.2em 0.5em;box-sizing:border-box;}";
  html += ".noteform button{margin-top:0.1em;margin-right:0.3em;width:auto;align-self:flex-start;}";
  html += "td:last-child{max-width:280px;overflow-wrap:anywhere;word-break:break-word;}";
  html += "@media (max-width:700px){";
  html += "  #eventTable{display:none;}";
  html += "  .eventCard {background: #fff;border-radius: 10px;box-shadow: 0 2px 10px #0001;margin-bottom: 1.2em;padding: 1em;font-size: 1.05em;}";
  html += "  .eventCard div {margin-bottom: 0.3em;}";
  html += "  #mobileEvents {max-height:70vh;overflow-y:auto;}";
  html += "}";
  html += "@media (min-width:701px){";
  html += "  #mobileEvents{display:none;}";
  html += "}";
  html += "</style>";
  html += "<script>";
  html += R"rawliteral(
// Track currently open note form between refreshes:
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
function fetchChannelsAndStart() {
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    channelList = data.assets.map(a=>a.name);
    let sel = document.getElementById('channelFilter');
    sel.innerHTML = "<option value='ALL'>All</option>";
    for (let i=0;i<channelList.length;++i) {
      let opt = document.createElement("option");
      opt.value = channelList[i];
      opt.text = channelList[i];
      sel.appendChild(opt);
    }
    sel.onchange = function() { filterValue = sel.value; renderTable(); };
    document.getElementById('stateFilter').onchange = function() { stateFilter = this.value; renderTable(); };
    fetchReasonsAndEvents();
  });
}
function fetchReasonsAndEvents() {
  fetch('/api/config').then(r=>r.json()).then(cfg=>{
    window.downtimeReasons = cfg.downtimeReasons || [];
    fetchAndRenderEvents();
    startAutoRefresh();
  });
}
function fetchAndRenderEvents() {
  fetch('/api/events').then(r=>r.json()).then(events=>{
    eventData = events;
    renderTable();
  });
}
function cleanNote(val) {
  if (!val) return "";
  let v = val.trim();
  if (v === "") return "";
  if (v === "," || v === ",," || v === "0,0," || v === "0.00,0,") return "";
  return v.replace(/^,+|,+$/g, "");
}

// Format minutes (float) as hh:mm:ss
function minToHHMMSS(val) {
  if (isNaN(val) || val <= 0) return "00:00:00";
  let totalSeconds = Math.round(val * 60);
  let h = Math.floor(totalSeconds / 3600);
  let m = Math.floor((totalSeconds % 3600) / 60) % 60;
  let s = totalSeconds % 60;
  return (h < 10 ? "0" : "") + h + ":" + (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s;
}

// Format mm:ss or h:mm:ss from mm:ss or h:mm:ss string (for Run/Stop Duration)
function durationStrToHHMMSS(str) {
  if (!str || typeof str !== "string") return "00:00:00";
  let parts = str.split(":").map(Number);
  if (parts.length === 3) {
    // already h:mm:ss
    let [h, m, s] = parts;
    return (h < 10 ? "0":"") + h + ":" + (m < 10 ? "0":"") + m + ":" + (s < 10 ? "0":"") + s;
  } else if (parts.length === 2) {
    // mm:ss, convert to hh:mm:ss
    let [m, s] = parts;
    let h = Math.floor(m / 60);
    m = m % 60;
    return (h < 10 ? "0":"") + h + ":" + (m < 10 ? "0":"") + m + ":" + (s < 10 ? "0":"") + s;
  }
  return "00:00:00";
}

function renderTable() {
  let tbody = document.getElementById('tbody');
  let mobileDiv = document.getElementById('mobileEvents');
  tbody.innerHTML = '';
  mobileDiv.innerHTML = '';
  let stateMatch = function(lstate) {
    if (stateFilter=="ALL") return true;
    if (stateFilter=="RUNNING") return lstate=="1";
    if (stateFilter=="STOPPED") return lstate=="0";
    return true;
  };
  let isMobile = window.innerWidth <= 700;
  let displayData = eventData.slice().reverse();
  for (let i=0; i<displayData.length; ++i) {
    let vals = displayData[i].split(',');
    if (vals.length < 14) continue;
    let ldate = vals[0], ltime = vals[1], lasset = vals[2], levent = vals[3], lstate = vals[4];
    let lavail = vals[5], lrun = vals[6], lstop = vals[7], lmtbf = vals[8], lmttr = vals[9], lsc = vals[10];
    let runDur = vals[11], stopDur = vals[12];
    let lnote = vals.slice(13).join(',').replace(/\n$/, "");
    let stopsInt = Math.round(Number(lsc));
    if (filterValue != "ALL" && lasset != filterValue) continue;
    if (!stateMatch(lstate)) continue;
    let noteFormId = 'noteform-' + btoa(ldate + "|" + ltime + "|" + lasset).replace(/[^a-zA-Z0-9]/g, "_");
    let noteFormHtml = `
      <form class='noteform' id='${noteFormId}' onsubmit='return submitNote(event,"${ldate}","${ltime}","${lasset}")' style='display:none;'>
        <label>Reason: <select name='reason'>
          <option value=''></option>
          ${window.downtimeReasons.map(r =>
            `<option value="${r.replace(/"/g, "&quot;")}">${r}</option>`).join("")}
        </select></label>
        <input type='text' name='note' value='${cleanNote(lnote).replace(/["']/g,"&quot;")}' maxlength='64' placeholder='Add/Edit note'>
        <button type='submit'>Save</button>
        <button type='button' onclick='hideNoteForm("${noteFormId}")'>Cancel</button>
        <input type='hidden' name='date' value='${ldate}'>
        <input type='hidden' name='time' value='${ltime}'>
        <input type='hidden' name='asset' value='${lasset}'>
      </form>
    `;
    if (!isMobile) {
      let tr = document.createElement('tr');
      function td(txt) { let td=document.createElement('td'); td.innerHTML=txt; return td; }
      tr.appendChild(td(ldate));
      tr.appendChild(td(ltime));
      tr.appendChild(td(lasset));
      tr.appendChild(td(levent));
      tr.appendChild(td(lstate=="1" ? "<span style='color:#256029;font-weight:bold;'>RUNNING</span>" : "<span style='color:#b71c1c;font-weight:bold;'>STOPPED</span>"));
      tr.appendChild(td(Number(lavail).toFixed(2)));
      tr.appendChild(td(minToHHMMSS(Number(lrun))));
      tr.appendChild(td(minToHHMMSS(Number(lstop))));
      tr.appendChild(td(minToHHMMSS(Number(lmtbf))));
      tr.appendChild(td(minToHHMMSS(Number(lmttr))));
      tr.appendChild(td(stopsInt));
      tr.appendChild(td(levent=="STOP" ? durationStrToHHMMSS(runDur) : ""));
      tr.appendChild(td(levent=="START" ? durationStrToHHMMSS(stopDur) : ""));
      let tdNote = document.createElement('td');
      tdNote.innerHTML = `<span class='note'>${cleanNote(lnote)}</span>`;
      if (lstate == "1") {
        tdNote.innerHTML += ` <button class='notebtn' onclick='showNoteForm("${noteFormId}")'>Edit</button>`;
        tdNote.innerHTML += noteFormHtml;
      }
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
        <div><b>State:</b> ${(lstate == "1"
          ? "<span style='color:#256029;font-weight:bold;'>RUNNING</span>"
          : "<span style='color:#b71c1c;font-weight:bold;'>STOPPED</span>")}</div>
        <div><b>Avail(%):</b> ${Number(lavail).toFixed(2)}</div>
        <div><b>Runtime:</b> ${minToHHMMSS(Number(lrun))}</div>
        <div><b>Downtime:</b> ${minToHHMMSS(Number(lstop))}</div>
        <div><b>MTBF:</b> ${minToHHMMSS(Number(lmtbf))}</div>
        <div><b>MTTR:</b> ${minToHHMMSS(Number(lmttr))}</div>
        <div><b>Stops:</b> ${stopsInt}</div>
        <div><b>Run Duration:</b> ${(levent=="STOP"? durationStrToHHMMSS(runDur) : "")}</div>
        <div><b>Stop Duration:</b> ${(levent=="START"? durationStrToHHMMSS(stopDur) : "")}</div>
        <div><b>Note:</b> <span class='note'>${cleanNote(lnote)}</span>
        ${(lstate == "1" ? ` <button class='notebtn' onclick='showNoteForm("${noteFormId}")'>Edit</button>` : "")}
        ${lstate == "1" ? noteFormHtml : ""}</div>`;
      mobileDiv.appendChild(card);
    }
  }
  document.getElementById('eventCount').innerHTML = "<b>Total Events:</b> " + eventData.length;
  document.getElementById('eventTable').style.display = isMobile ? 'none' : '';
  mobileDiv.style.display = isMobile ? '' : 'none';

  // Restore open note form after refresh
  if (window.openNoteFormId) showNoteForm(window.openNoteFormId);

  // Auto-scroll to top unless inhibited
  if (!scrollInhibit) {
    if (isMobile) {
      let mobDiv = document.getElementById('mobileEvents');
      if (mobDiv) mobDiv.scrollTop = 0;
    } else {
      let cont = document.getElementById('eventTable');
      if (cont) cont.scrollTop = 0;
      window.scrollTo({top:0, behavior:'instant'});
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
    if (r.ok) fetchAndRenderEvents();
  });
  form.style.display = 'none';
  window.openNoteFormId = null;
  startAutoRefresh();
  return false;
}
function toggleScrollInhibit(btn) {
  scrollInhibit = !scrollInhibit;
  btn.innerText = scrollInhibit ? "Enable Auto-Scroll" : "Inhibit Auto-Scroll";
}
window.onload = fetchChannelsAndStart;
)rawliteral";
  html += "</script>";
  html += "</head><body>";
  html += "<header><div style='font-size:1.6em;font-weight:700;'>Event Log</div></header>";
  html += "<nav class='nav'>";
  html += "<form action='/'><button type='submit'>Dashboard</button></form>";
  html += "<form action='/config'><button type='submit'>Setup</button></form>";
  html += "<form action='/export_log'><button type='submit'>Export CSV</button></form>";
  html += "</nav>";
  html += "<div class='main card'>";
  html += "<div class='filterrow'><label for='channelFilter'>Filter by Channel:</label> <select id='channelFilter'><option value='ALL'>All</option></select>";
  html += "<label for='stateFilter'>Event State:</label> <select id='stateFilter'><option value='ALL'>All</option><option value='RUNNING'>Running</option><option value='STOPPED'>Stopped</option></select>";
  html += "<span id='eventCount' style='margin-left:1em;'></span>";
  html += "<button class='scrollToggle' id='scrollBtn' type='button' onclick='toggleScrollInhibit(this)'>Inhibit Auto-Scroll</button></div>";
  html += "<div style='overflow-x:auto;'><table id='eventTable'><thead><tr>";
  html += "<th>Date</th><th>Time</th><th>Asset</th><th>Event</th><th>State</th><th>Avail(%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Run Duration</th><th>Stop Duration</th><th>Note</th>";
  html += "</tr></thead><tbody id='tbody'></tbody></table>";
  html += "<div id='mobileEvents'></div>";
  html += "</div></div></body></html>";
  return html;
}

String htmlConfig() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Setup</title><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<style>";
  // Existing Styles
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001;}";
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;align-items:center;}";
  html += ".nav button{background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;}";
  html += ".nav button:hover{background:#e3f0fc;}";
  html += ".nav .right{margin-left:auto;}";
  html += ".main{max-width:700px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += "label{font-weight:500;margin-top:1em;display:block;}input[type=text],input[type=number],select{width:100%;padding:0.6em;margin-top:0.2em;margin-bottom:1em;border:1px solid #ccc;border-radius:5px;box-sizing:border-box;font-size:1em;}";
  html += "input[type=submit],button{margin-top:1em;padding:0.8em 1.5em;font-size:1.15em;border-radius:8px;border:none;background:#1976d2;color:#fff;font-weight:700;cursor:pointer;}";
  // Removed fieldset and legend styles as they are replaced by tiles
  html += ".notice{background:#e6fbe7;color:#256029;font-weight:bold;padding:0.6em 1em;border-radius:7px;margin-bottom:1em;}";
  html += "button.wifi-reconfig{background:#f44336 !important; color:#fff !important;}"; 
  
  // Styles for Config Tiles (Accordion)
  html += ".config-tile { margin-bottom: 1rem; border: 1px solid #e0e0e0; border-radius: 8px; overflow: hidden; }";
  html += ".config-tile-header { background-color: #e9eff4; color: #1976d2; padding: 0.8em 1em; width: 100%; border: none; text-align: left; font-size: 1.1em; font-weight: 700; cursor: pointer; display: flex; justify-content: space-between; align-items: center; }";
  html += ".config-tile-header:hover { background-color: #dce7f0; }";
  html += ".config-tile-header .toggle-icon { font-size: 1.2em; transition: transform 0.2s; margin-left: 10px; }";
  html += ".config-tile-header.active .toggle-icon { transform: rotate(45deg); }";
  html += ".config-tile-content { padding: 0 1.3rem 1.3rem 1.3rem; display: none; background-color: #fff; border-top: 1px solid #e0e0e0;}"; // Initially hidden
  html += ".config-tile-content.open { display: block; }"; // Class to show content
  html += ".config-tile-content fieldset { border:1px solid #e0e0e0;padding:1em;border-radius:7px;margin-top:1em;margin-bottom:0.5em; }"; // Keep fieldset style for sub-grouping if needed
  html += ".config-tile-content fieldset legend { font-weight:700;color:#2196f3;font-size:1.05em;padding:0 0.5em; }";

  html += "@media(max-width:700px){.main{padding:0.5rem;} .card{padding:0.7rem;} input[type=submit],button{font-size:1em;} .config-tile-content{padding:0 0.7rem 0.7rem 0.7rem;}}";
  html += "</style>";
  html += "<script>";
  html += "function clearLogDblConfirm(e){ if(!confirm('Are you sure you want to CLEAR ALL LOG DATA?')){e.preventDefault();return false;} if(!confirm('Double check: This cannot be undone! Are you REALLY sure you want to clear the log?')){e.preventDefault();return false;} return true; }";
  html += "function showSavedMsg(){ document.getElementById('saveNotice').style.display='block'; }";
  html += "function confirmWiFiReconfig(e){ if(!confirm('Are you sure you want to enter WiFi setup mode? The device will disconnect from the current network and restart as an Access Point.')){e.preventDefault();return false;} return true; }";
  
  // JavaScript for Accordion Tiles
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
  // Optionally, open the first tile by default or based on URL hash
  html += "  const firstTileHeader = document.querySelector('.config-tile:first-child .config-tile-header');";
  html += "  if (firstTileHeader) {";
  html += "    firstTileHeader.classList.add('active');";
  html += "    const firstContent = firstTileHeader.nextElementSibling;";
  html += "    if (firstContent) firstContent.classList.add('open');";
  html += "    const firstIcon = firstTileHeader.querySelector('.toggle-icon');";
  html += "    if(firstIcon) firstIcon.textContent = '-';";
  html += "  }";
  html += "}";
  // Call on DOMContentLoaded
  html += "document.addEventListener('DOMContentLoaded', setupConfigTiles);";

  html += "</script>";
  html += "</head><body>";
  html += "<header><div style='font-size:1.6em;font-weight:700;'>Setup</div></header>";
  html += "<nav class='nav'>";
  html += "<form action='/'><button type='submit'>Dashboard</button></form>";
  html += "<form action='/events'><button type='submit'>Event Log</button></form>";
  html += "<form action='/export_log'><button type='submit'>Export CSV</button></form>";
  html += "<form action='/clear_log' method='POST' style='display:inline;' onsubmit='return clearLogDblConfirm(event);'><button type='submit' style='background:#f44336;color:#fff;' class='right'>Clear Log</button></form>";
  html += "</nav>";
  html += "<div class='main'><div class='card'>"; // Main card starts

  html += "<form method='POST' action='/save_config' id='setupform' onsubmit='setTimeout(showSavedMsg, 1500);'>";
  html += "<div id='saveNotice' class='notice' style='display:none;'>Save &amp; reboot completed. You can continue setup or return to dashboard.</div>";

  // Tile 1: Asset Setup
  html += "<div class='config-tile'>";
  html += "  <button type='button' class='config-tile-header'>Asset Setup <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  html += "    <label>Asset count (max 10): <input type='number' name='assetCount' min='1' max='" + String(MAX_ASSETS) + "' value='" + String(config.assetCount) + "' required></label>";
  html += "    <p style='font-size:0.9em; color:#555; margin-top:-0.5em; margin-bottom:1em;'>To change the number of assets, update this count and click 'Save All Settings & Reboot'. The page will refresh with the new number of asset configuration sections after reboot.</p>";
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    html += "    <fieldset><legend>Asset #" + String(i+1) + "</legend>";
    html += "      <label>Name: <input type='text' name='name" + String(i) + "' value='" + String(config.assets[i].name) + "' maxlength='31' required></label>";
    html += "      <label>GPIO Pin: <input type='number' name='pin" + String(i) + "' value='" + String(config.assets[i].pin) + "' min='0' max='39' required></label>";
    html += "    </fieldset>";
  }
  html += "  </div>"; // end content
  html += "</div>"; // end tile

  // Tile 2: Operational Settings
  html += "<div class='config-tile'>";
  html += "  <button type='button' class='config-tile-header'>Operational Settings <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  html += "    <label>Max events per asset (log size): <input type='number' name='maxEvents' min='100' max='5000' value='" + String(config.maxEvents) + "' required></label>";
  html += "    <label>Timezone offset from UTC (hours): <input type='number' name='tzOffset' min='-12' max='14' step='0.5' value='" + String(config.tzOffset / 3600.0, 1) + "' required></label>";
  html += "    <label>Highlight stops longer than (min): <input type='number' name='longStopThreshold' min='1' max='1440' value='" + String(config.longStopThresholdSec/60) + "' required></label>"; // Max 1440 mins = 24 hours
  html += "  </div>"; // end content
  html += "</div>"; // end tile
  
  // Tile 3: Downtime Reasons
  html += "<div class='config-tile'>";
  html += "  <button type='button' class='config-tile-header'>Downtime Quick Reasons <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  for (int i = 0; i < 5; ++i) {
    html += "    <label>Reason " + String(i+1) + ": <input type='text' name='reason" + String(i) + "' value='" + String(config.downtimeReasons[i]) + "' maxlength='31'></label>";
  }
  html += "  </div>"; // end content
  html += "</div>"; // end tile

  html += "<input type='submit' value='Save All Settings & Reboot' style='width:100%; margin-top:1.5rem;'>";
  html += "</form>"; // End of main settings form

  // Tile 4: Network Configuration (Separate form for its action)
  html += "<div class='config-tile' style='margin-top:1.5rem;'>"; // Add some top margin
  html += "  <button type='button' class='config-tile-header'>Network Configuration <span class='toggle-icon'>+</span></button>";
  html += "  <div class='config-tile-content'>";
  html += "    <p style='margin-top:0; margin-bottom:0.5em;'><strong>Current WiFi Status:</strong> ";
  if (WiFi.status() == WL_CONNECTED) {
    html += "Connected to " + WiFi.SSID();
    html += " (IP: " + WiFi.localIP().toString() + ")";
  } else if (WiFi.getMode() == WIFI_AP) {
    html += "Currently in Access Point Mode (AssetMonitor_Config)";
  } else {
    html += "Not Connected / Status Unknown";
  }
  html += "</p>";
  html += "    <p style='margin-top:0.5em; margin-bottom:1em;'>If you need to connect to a different WiFi network or re-enter credentials, use the button below. The device will restart in WiFi Setup Mode (Access Point: 'AssetMonitor_Config').</p>";
  html += "    <form method='POST' action='/reconfigure_wifi' onsubmit='return confirmWiFiReconfig(event);' style='margin-top:0.5em;'>";
  html += "      <button type='submit' class='wifi-reconfig' style='padding:0.8em 1.5em; font-size:1.1em;'>Enter WiFi Setup Mode</button>"; // Matched style of main save button
  html += "    </form>";
  html += "  </div>"; // end content
  html += "</div>"; // end tile

  html += "</div></div></body></html>"; // Main card ends
  return html;
}

void handleWiFiReconfigurePost() {
  String message = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>WiFi Reconfiguration</title>";
  message += "<style>body{font-family: Arial, sans-serif; margin: 20px; padding: 15px; border:1px solid #ddd; border-radius:5px; text-align:center;} h2{color:#333;}</style>";
  message += "</head><body>";
  message += "<h2>Device Entering WiFi Setup Mode</h2>";
  message += "<p>The device will now switch to WiFi Setup Mode.</p>";
  message += "<p>It will create an Access Point named '<strong>AssetMonitor_Config</strong>'.</p>";
  message += "<p>Please connect your computer or phone to that WiFi network.</p>";
  message += "<p>Then, open a web browser and go to <strong>192.168.4.1</strong> to configure the new WiFi settings.</p>";
  message += "<p>The device will restart after you save the new settings from that page.</p>";
  message += "</body></html>";
  
  server.sendHeader("Connection", "close"); // Advise browser to close connection
  server.send(200, "text/html", message);
  
  delay(200); // Short delay to help ensure the HTTP response is sent before AP mode starts
  // === THIS IS WHERE YOU CALL startConfigPortal() ===
  startConfigPortal(); 
}

// ASSET DETAIL PAGE (simple)
String htmlAssetDetail(uint8_t idx) {
  if (idx >= config.assetCount) return "Invalid Asset";
  String html = "<!DOCTYPE html><html><head><title>Asset Detail</title></head><body>";
  html += "<h1>Asset Detail: " + String(config.assets[idx].name) + "</h1>";
  html += "<p>GPIO Pin: " + String(config.assets[idx].pin) + "</p>";
  html += "<a href=\"/\">Back to Dashboard</a>";
  html += "</body></html>";
  return html;
}

// CONFIG POST

// Full patched handleConfigPost() function:
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
    
    server.sendHeader("Location", "/config"); 
    server.send(303);
    
    server.client().stop(); 
    delay(1000); 
    
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request: Missing assetCount");
  }
}

void handleClearLog() { SPIFFS.remove(LOG_FILENAME); server.sendHeader("Location", "/config"); server.send(303); }
void handleExportLog() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f) { server.send(404, "text/plain", "No log file"); return; }
  time_t now = time(nullptr);
  struct tm * ti = localtime(&now);
  char fn[64];
  strftime(fn, sizeof(fn), "log-%Y%m%d-%H%M%S.csv", ti);
  String csv = "Date,Time,Asset,Event,State,Availability (%),Total Runtime (min),Total Downtime (min),MTBF (min),MTTR (min),No. of Stops,Run Duration,Stop Duration,Note\n";
  while (f.available()) { csv += f.readStringUntil('\n') + "\n"; }
  f.close();
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + fn + "\"");
  server.send(200, "text/csv", csv);
}

// API HANDLERS
void handleApiSummary() {
  String json = "{\"assets\":[";
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    if (i > 0) json += ",";
    AssetState& as = assetStates[i];
    bool state = digitalRead(config.assets[i].pin);
    unsigned long now = time(nullptr);

    unsigned long runningTime = as.runningTime;
    unsigned long stoppedTime = as.stoppedTime;
    if (as.lastState) runningTime += now - as.lastChangeTime;
    else stoppedTime += now - as.lastChangeTime;

    float avail = (runningTime + stoppedTime) > 0 ? (100.0 * runningTime / (runningTime + stoppedTime)) : 0;
    float total_runtime_min = runningTime / 60.0;
    float total_downtime_min = stoppedTime / 60.0;
    float mtbf = (as.stopCount > 0) ? runningTime / as.stopCount : 0;
    float mttr = (as.stopCount > 0) ? stoppedTime / as.stopCount : 0;
    mtbf = mtbf / 60.0;
    mttr = mttr / 60.0;

    json += "{";
    json += "\"name\":\"" + String(config.assets[i].name) + "\",";
    json += "\"pin\":" + String(config.assets[i].pin) + ",";
    json += "\"state\":" + String(state ? 1 : 0) + ",";
    json += "\"availability\":" + String(avail, 2) + ",";
    json += "\"total_runtime\":" + String(total_runtime_min, 2) + ",";
    json += "\"total_downtime\":" + String(total_downtime_min, 2) + ",";
    json += "\"mtbf\":" + String(mtbf, 2) + ",";
    json += "\"mttr\":" + String(mttr, 2) + ",";
    json += "\"stop_count\":" + String(as.stopCount) + "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}
void handleApiEvents() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  String json = "[";
  if (f) {
    String line;
    int first = 1;
    while (f.available()) {
      line = f.readStringUntil('\n');
      if (line.length() < 5) continue;
      if (!first) json += ",";
      first = 0;
      json += "\"" + line + "\"";
    }
    f.close();
  }
  json += "]";
  server.send(200, "application/json", json);
}

// In handleApiConfig(), add this to the JSON before sending to client:
void handleApiConfig() {
  String json = "{";
  json += "\"assetCount\":" + String(config.assetCount) + ",";
  json += "\"maxEvents\":" + String(config.maxEvents) + ",";
  json += "\"tzOffset\":" + String(config.tzOffset) + ",";
  json += "\"assets\":[";
  for (uint8_t i = 0; i < config.assetCount; ++i) {
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
  // PATCH: Add long stop threshold to config API
  json += ",\"longStopThresholdSec\":" + String(config.longStopThresholdSec);
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiNote() {
  if (server.hasArg("date") && server.hasArg("time") && server.hasArg("asset")) {
    String date = server.arg("date");
    String time = server.arg("time");
    String asset = server.arg("asset");
    String note = server.arg("note");
    String reason = server.hasArg("reason") ? server.arg("reason") : "";
    updateEventNote(date, time, asset, note, reason);
    server.sendHeader("Location", "/events");
    server.send(303);
    return;
  }
  server.send(400, "text/plain", "Invalid");
}

// Ensure 'server' is accessible (e.g., declared globally or passed appropriately)
// Ensure 'startConfigPortal()' is declared or defined before this if not already.

void updateEventNote(String date, String time, String assetName, String note, String reason) {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f) return;
  String all = "";
  String newNote = "";
  if (reason.length() > 0 && note.length() > 0) newNote = reason + " - " + note;
  else if (reason.length() > 0) newNote = reason;
  else newNote = note;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    String origLine = line;
    line.trim();
    if (line.length()<5) { all += origLine+"\n"; continue; }
    int p[14]; int count=0; p[0]=0;
    for(int i=0;i<13&&count<13;++i) { int idx=line.indexOf(',',p[count]); if(idx<0) break; p[++count]=idx+1; }
    String ldate = line.substring(0,p[1]-1);
    String ltime = line.substring(p[1],p[2]-1);
    String lasset = line.substring(p[2],p[3]-1);
    if (ldate == date && ltime == time && lasset == assetName) {
      int lastComma = line.lastIndexOf(',');
      if (lastComma!=-1) { all += line.substring(0,lastComma+1) + newNote + "\n"; }
      else { all += origLine + "\n"; }
    } else { all += origLine + "\n"; }
  }
  f.close();
  File f2 = SPIFFS.open(LOG_FILENAME, FILE_WRITE);
  if (!f2) return;
  f2.print(all);
  f2.close();
}
void handleNotFound() { server.send(404, "text/plain", "Not found"); }
