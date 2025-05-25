#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>

// --------- USER CONFIGURATION ---------
const char* ssid = "New Router";
const char* password = "Diamond100";

#define MAX_ASSETS 10
#define LOG_FILENAME "/log.csv"

const char* DEFAULT_DOWNTIME_REASONS[5] = {
  "Maintenance", "Material Shortage", "Operator Break", "Equipment Failure", "Changeover"
};

// --------- DATA STRUCTURES ---------
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
};

// --------- GLOBALS ---------
WebServer server(80);
Preferences prefs;
AssetState assetStates[MAX_ASSETS];

// --------- FUNCTION PROTOTYPES ---------
void loadConfig();
void saveConfig();
void setupWiFi();
void setupTime();
void logEvent(uint8_t assetIdx, bool state, time_t now, const char* note = nullptr);
String htmlDashboard();
String htmlAssetDetail(uint8_t idx);
String htmlConfig();
String htmlSummary();
String htmlEvents();
void handleConfigPost();
void handleClearLog();
void handleExportLog();
void handleApiSummary();
void handleApiEvents();
void handleApiConfig();
void handleApiNote();
void handleNotFound();
void addNoteToEvent(time_t timestamp, uint8_t assetIdx, String note, String reason);

// --------- SETUP ---------
void setup() {
  Serial.begin(115200);
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS failed!");
    return;
  }
  prefs.begin("assetmon", false);

  loadConfig();
  setupWiFi();
  setupTime();

  for (uint8_t i = 0; i < config.assetCount; ++i) {
    pinMode(config.assets[i].pin, INPUT_PULLUP);
    assetStates[i].lastState = digitalRead(config.assets[i].pin);
    assetStates[i].lastChangeTime = time(nullptr);
    assetStates[i].sessionStart = assetStates[i].lastChangeTime;
    assetStates[i].runningTime = 0;
    assetStates[i].stoppedTime = 0;
    assetStates[i].runCount = 0;
    assetStates[i].stopCount = 0;
  }

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", htmlDashboard()); });
  server.on("/summary", HTTP_GET, []() { server.send(200, "text/html", htmlSummary()); });
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
  server.on("/config", HTTP_GET, []() { server.send(200, "text/html", htmlConfig()); });
  server.on("/save_config", HTTP_POST, handleConfigPost);
  server.on("/clear_log", HTTP_POST, handleClearLog);
  server.on("/export_log", HTTP_GET, handleExportLog);

  // REST API endpoints
  server.on("/api/summary", HTTP_GET, handleApiSummary);
  server.on("/api/events", HTTP_GET, handleApiEvents);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/note", HTTP_POST, handleApiNote);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Web server started.");
}

// --------- LOOP ---------
void loop() {
  server.handleClient();
  time_t now = time(nullptr);

  for (uint8_t i = 0; i < config.assetCount; ++i) {
    bool state = digitalRead(config.assets[i].pin);

    if (state != assetStates[i].lastState) {
      logEvent(i, state, now);
      unsigned long elapsed = now - assetStates[i].lastChangeTime;

      if (state) {
        assetStates[i].stoppedTime += elapsed;
        assetStates[i].runCount++;
      } else {
        assetStates[i].runningTime += elapsed;
        assetStates[i].stopCount++;
      }
      assetStates[i].lastState = state;
      assetStates[i].lastChangeTime = now;
    }
  }
  delay(200);
}

// --------- CONFIG LOAD/SAVE ---------
void loadConfig() {
  if (prefs.isKey("cfg")) {
    prefs.getBytes("cfg", &config, sizeof(config));
  } else {
    config.assetCount = 2;
    config.maxEvents = 1000;
    strcpy(config.assets[0].name, "Line 1");
    config.assets[0].pin = 4;
    strcpy(config.assets[1].name, "Line 2");
    config.assets[1].pin = 12;
    for (int i = 0; i < 5; ++i) strncpy(config.downtimeReasons[i], DEFAULT_DOWNTIME_REASONS[i], 31);
    config.tzOffset = 0;
    saveConfig();
  }
}

void saveConfig() {
  prefs.putBytes("cfg", &config, sizeof(config));
}

// --------- WIFI & TIME ---------
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected, IP: ");
  Serial.println(WiFi.localIP());
}

void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1); // UK/GB timezone with DST
  tzset();
  Serial.print("Waiting for NTP time sync...");
  time_t now = time(nullptr);
  int retry = 0;
  while (now < 8 * 3600 * 2 && retry < 30) {
    delay(200);
    Serial.print(".");
    now = time(nullptr);
    retry++;
  }
  Serial.println(" done");
}

// --------- EVENT LOGGING ---------
void logEvent(uint8_t assetIdx, bool state, time_t now, const char* note) {
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
  mtbf = mtbf / 60.0;
  mttr = mttr / 60.0;

  struct tm * ti = localtime(&now);
  char datebuf[11], timebuf[9];
  strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", ti);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti);

  File f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
  if (!f) {
    Serial.println("Failed to open log file for writing!");
    return;
  }
  f.printf("%s,%s,%s,%s,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%s\n",
    datebuf, timebuf, config.assets[assetIdx].name,
    state ? "START" : "STOP",
    state, avail, total_runtime_min, total_downtime_min, mtbf, mttr, as.stopCount, note ? note : ""
  );
  f.close();
  as.lastEventTime = now;
  Serial.println("Wrote event to log.");
}

// --------- WEB PAGES ---------
String htmlDashboard() {
  String html = "<!DOCTYPE html><html><head><title>ESP32 Asset Monitor</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;background:#eef;margin:0;padding:0;}header{background:#1565c0;color:#fff;padding:18px 0;text-align:center;}.main{margin:30px auto;max-width:900px;background:#fff;padding:2em;border-radius:15px;box-shadow:0 4px 32px #bbb;}";
  html += "input[type=submit],button{margin-top:1em;padding:0.8em 1.5em;font-size:1em;border-radius:8px;border:none;background:#1976d2;color:#fff;cursor:pointer;} .table-section{margin-top:2em;} </style></head><body>";
  html += "<header><h1>ESP32 Asset Monitor</h1><div style='font-size:0.9em;'>IP: ";
  html += WiFi.localIP().toString();
  html += "</div></header><div class='main'><h2>Assets Status & Performance</h2>";

  // Navigation
  html += "<div style='margin-bottom:2em;'>";
  html += "<a href='/summary'><button>Summary Table/Chart</button></a> ";
  html += "<a href='/events'><button>Event Log</button></a> ";
  html += "<a href='/config'><button>Configure</button></a> ";
  html += "<a href='/export_log'><button>Export CSV</button></a> ";
  html += "<form method='POST' action='/clear_log' style='display:inline'><button type='submit' style='background:#b71c1c;'>Clear Log</button></form>";
  html += "</div>";

  html += "</div></body></html>";
  return html;
}

String htmlSummary() {
  String html = "<!DOCTYPE html><html><head><title>Asset Summary</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>body{font-family:sans-serif;background:#eef;padding:0;margin:0;}header{background:#1565c0;color:#fff;padding:18px 0;text-align:center;}.main{margin:30px auto;max-width:1400px;background:#fff;padding:2em;border-radius:15px;box-shadow:0 4px 32px #bbb;}table{width:100%;margin-top:1em;border-collapse:collapse;}th,td{padding:0.3em 0.7em;border-bottom:1px solid #ddd;text-align:left;}th{background:#eee;}#chart-container{margin:2em 0;}</style>";
  html += "</head><body>";
  html += "<header><h1>Asset Summary</h1><div><a href='/'>Back to Dashboard</a> <a href='/events' style='margin-left:2em;'>Event Log</a></div></header>";
  html += "<div class='main'>";
  html += "<div id='chart-container'><canvas id='barChart'></canvas></div>";
  html += "<table id='summaryTable'><thead><tr>";
  html += "<th>Name</th><th>State</th><th>Availability (%)</th>"
          "<th>Total Runtime (mm:ss)</th><th>Total Downtime (mm:ss)</th>"
          "<th>MTBF (mm:ss)</th><th>MTTR (mm:ss)</th><th>No. of Stops</th>";
  html += "</tr></thead><tbody></tbody></table>";
  html += "</div>";
  html += "<script>";
  html += R"rawliteral(
function formatMMSS(val) {
  if (isNaN(val) || val < 0.01) return "00:00";
  let min = Math.floor(val);
  let sec = Math.round((val - min) * 60);
  if (sec == 60) { min++; sec = 0; }
  return (min<10?"0":"")+min+":"+(sec<10?"0":"")+sec;
}
let chartObj = null;
function updateSummary() {
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    let tbody = document.querySelector('#summaryTable tbody');
    let assets = data.assets;
    let rows = tbody.rows;
    for (let i=0; i<assets.length; ++i) {
      let asset = assets[i];
      let row = rows[i];
      if (!row) {
        row = tbody.insertRow();
        for (let j=0;j<8;++j) row.insertCell();
      }
      let v0 = asset.name,
          v1 = `<span style="color:${asset.state==1?'#256029':'#b71c1c'};font-weight:bold">${asset.state==1?'RUNNING':'STOPPED'}</span>`,
          v2 = asset.availability.toFixed(2),
          v3 = formatMMSS(asset.total_runtime),
          v4 = formatMMSS(asset.total_downtime),
          v5 = formatMMSS(asset.mtbf),
          v6 = formatMMSS(asset.mttr),
          v7 = asset.stop_count;
      let vals = [v0,v1,v2,v3,v4,v5,v6,v7];
      for(let j=0;j<8;++j) {
        if (row.cells[j].innerHTML !== vals[j]) row.cells[j].innerHTML = vals[j];
      }
    }
    while (rows.length > assets.length) tbody.deleteRow(rows.length-1);

    // Chart: show availability, total runtime, total downtime
    let availData=[], names=[], runtimeData=[], downtimeData=[];
    for (let asset of assets) {
      availData.push(asset.availability);
      runtimeData.push(asset.total_runtime);
      downtimeData.push(asset.total_downtime);
      names.push(asset.name);
    }
    let ctx = document.getElementById('barChart').getContext('2d');
    if (!chartObj) {
      chartObj = new Chart(ctx, {
        type: 'bar',
        data: {
          labels: names,
          datasets: [
            {
              label: 'Availability (%)',
              data: availData,
              backgroundColor: '#42a5f5',
              yAxisID: 'y'
            },
            {
              label: 'Total Runtime (min)',
              data: runtimeData,
              backgroundColor: '#66bb6a',
              yAxisID: 'y1'
            },
            {
              label: 'Total Downtime (min)',
              data: downtimeData,
              backgroundColor: '#ef5350',
              yAxisID: 'y1'
            }
          ]
        },
        options: {
          scales: {
            y: { beginAtZero:true, max:100, title:{display:true,text:'Availability (%)'} },
            y1: {
              beginAtZero:true,
              position: 'right',
              grid: { drawOnChartArea: false },
              title: { display:true, text:'Runtime/Downtime (min)' }
            }
          }
        }
      });
    } else {
      chartObj.data.labels = names;
      chartObj.data.datasets[0].data = availData;
      chartObj.data.datasets[1].data = runtimeData;
      chartObj.data.datasets[2].data = downtimeData;
      chartObj.update();
    }
  });
}
updateSummary();
setInterval(updateSummary, 5000);
  )rawliteral";
  html += "</script></body></html>";
  return html;
}

String htmlEvents() {
  String html = "<!DOCTYPE html><html><head><title>Event Log</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;background:#eef;margin:0;padding:0;}header{background:#1565c0;color:#fff;padding:18px 0;text-align:center;} .main{margin:30px auto;max-width:1400px;background:#fff;padding:2em;border-radius:15px;box-shadow:0 4px 32px #bbb;}table{width:100%;margin-top:1em;border-collapse:collapse;}th,td{padding:0.3em 0.7em;border-bottom:1px solid #ddd;text-align:left;}th{background:#eee;} .note{color:#444;} #filterRow{margin-bottom:1em;} </style>";
  html += "</head><body>";
  html += "<header><h1>Event Log</h1><div><a href='/'>Back to Dashboard</a> <a href='/summary' style='margin-left:2em;'>Summary Table</a></div></header>";
  html += "<div class='main'>";
  html += "<div id='filterRow'><label for='channelFilter'><b>Filter by Channel:</b></label> <select id='channelFilter'><option value='ALL'>All</option></select></div>";
  html += "<div id='event-summary'></div>";
  html += "<table id='eventTable'><thead><tr>";
  html += "<th>Date</th><th>Time</th><th>Asset</th><th>Event</th><th>State</th><th>Availability (%)</th>"
          "<th>Total Runtime (mm:ss)</th><th>Total Downtime (mm:ss)</th>"
          "<th>MTBF (mm:ss)</th><th>MTTR (mm:ss)</th><th>No. of Stops</th><th>Note</th></tr>";
  html += "</thead><tbody></tbody></table>";
  html += "</div>";
  html += "<script>";
  html += R"rawliteral(
function formatMMSS(val) {
  if (isNaN(val) || val < 0.01) return "00:00";
  let min = Math.floor(val);
  let sec = Math.round((val - min) * 60);
  if (sec == 60) { min++; sec = 0; }
  return (min<10?"0":"")+min+":"+(sec<10?"0":"")+sec;
}
function populateChannelFilter() {
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    var select = document.getElementById("channelFilter");
    while (select.options.length > 1) select.remove(1);
    for (let asset of data.assets) {
      let opt = document.createElement("option");
      opt.value = asset.name;
      opt.text = asset.name;
      select.appendChild(opt);
    }
  });
}
let allEvents = [];
function updateEventLog() {
  fetch('/api/events').then(r=>r.json()).then(events=>{
    allEvents = [];
    for (let i=0; i<events.length; ++i) {
      let vals = events[i].split(',');
      if (vals.length < 12) continue;
      allEvents.push({
        date: vals[0],
        time: vals[1],
        asset: vals[2],
        event: vals[3],
        state: vals[4],
        avail: vals[5],
        total_runtime: vals[6],
        total_downtime: vals[7],
        mtbf: vals[8],
        mttr: vals[9],
        stop_count: vals[10],
        note: vals.slice(11).join(',')
      });
    }
    allEvents.reverse(); // Show most recent first!
    renderEventTable();
  });
}
function renderEventTable() {
  let filter = document.getElementById("channelFilter").value;
  let tbody = document.querySelector('#eventTable tbody');
  let filtered = (filter === "ALL") ? allEvents : allEvents.filter(ev=>ev.asset===filter);
  let rows = tbody.rows;
  for (let i=0; i<filtered.length; ++i) {
    let ev = filtered[i];
    let row = rows[i];
    if (!row) {
      row = tbody.insertRow();
      for (let j=0;j<12;++j) row.insertCell();
    }
    row.cells[0].textContent = ev.date;
    row.cells[1].textContent = ev.time;
    row.cells[2].textContent = ev.asset;
    row.cells[3].textContent = ev.event;
    row.cells[4].innerHTML = ev.state == "1"
        ? "<span style='color:#256029;font-weight:bold;'>RUNNING</span>"
        : "<span style='color:#b71c1c;font-weight:bold;'>STOPPED</span>";
    row.cells[5].textContent = parseFloat(ev.avail).toFixed(2);
    row.cells[6].textContent = formatMMSS(parseFloat(ev.total_runtime));
    row.cells[7].textContent = formatMMSS(parseFloat(ev.total_downtime));
    row.cells[8].textContent = formatMMSS(parseFloat(ev.mtbf));
    row.cells[9].textContent = formatMMSS(parseFloat(ev.mttr));
    row.cells[10].textContent = ev.stop_count;
    row.cells[11].innerHTML = `<span class='note'>${ev.note}</span>`;
  }
  while (rows.length > filtered.length) tbody.deleteRow(rows.length-1);
  document.getElementById('event-summary').innerHTML = `<b>Total Events:</b> ${filtered.length}`;
}
document.addEventListener("DOMContentLoaded", function() {
  populateChannelFilter();
  updateEventLog();
  setInterval(updateEventLog, 5000);
  document.getElementById("channelFilter").addEventListener("change", renderEventTable);
});
  )rawliteral";
  html += "</script></body></html>";
  return html;
}

String htmlAssetDetail(uint8_t idx) {
  String html = "<!DOCTYPE html><html><head><title>Asset Detail</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "</head><body><header><h1>Asset Detail: ";
  html += String(config.assets[idx].name);
  html += "</h1></header>";
  html += "<div><a href='/summary'>Back to Summary</a></div>";
  html += "<div>Status: ";
  html += assetStates[idx].lastState ? "RUNNING" : "STOPPED";
  html += "</div></body></html>";
  return html;
}

String htmlConfig() {
  String html = "<!DOCTYPE html><html><head><title>Configure ESP32 Monitor</title><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;background:#eef;padding:0;}.main{margin:30px auto;max-width:700px;background:#fff;padding:2em;border-radius:15px;box-shadow:0 4px 32px #bbb;}input[type=text],input[type=number]{width:80%;padding:0.4em;margin-bottom:0.5em;font-size:1em;border-radius:8px;border:1px solid #bbb;}input[type=submit],button{margin-top:1em;padding:0.8em 1.5em;font-size:1em;border-radius:8px;border:none;background:#1976d2;color:#fff;cursor:pointer;}</style></head><body>";
  html += "<div class='main'><h2>Configuration</h2><form method='POST' action='/save_config'>";
  html += "<label>Asset count (max 10): <input type='number' name='assetCount' min='1' max='10' value='" + String(config.assetCount) + "' required></label><br>";
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    html += "<div style='margin-bottom:1em;border-bottom:1px solid #eee;padding-bottom:0.5em;'>";
    html += "<label>Name #" + String(i+1) + ": <input type='text' name='name" + String(i) + "' value='" + String(config.assets[i].name) + "' maxlength='31' required></label><br>";
    html += "<label>GPIO: <input type='number' name='pin" + String(i) + "' value='" + String(config.assets[i].pin) + "' min='0' max='39' required></label>";
    html += "</div>";
  }
  html += "<label>Max events per asset (log size): <input type='number' name='maxEvents' min='100' max='5000' value='" + String(config.maxEvents) + "' required></label><br>";
  html += "<label>Timezone offset (minutes): <input type='number' name='tzOffset' min='-720' max='840' value='" + String(config.tzOffset / 60) + "' required></label><br>";
  html += "<label>Downtime quick reasons (editable):<br>";
  for (int i = 0; i < 5; ++i) {
    html += "<input type='text' name='reason" + String(i) + "' value='" + String(config.downtimeReasons[i]) + "' maxlength='31'><br>";
  }
  html += "</label><br>";
  html += "<input type='submit' value='Save & Reboot'></form>";
  html += "<div style='margin-top:2em;'><a href='/'><button type='button'>Back to Dashboard</button></a></div></div></body></html>";
  return html;
}

// --------- CONFIG POST ---------
void handleConfigPost() {
  if (server.hasArg("assetCount")) {
    config.assetCount = constrain(server.arg("assetCount").toInt(), 1, MAX_ASSETS);
    for (uint8_t i = 0; i < config.assetCount; ++i) {
      String n = "name" + String(i);
      String p = "pin" + String(i);
      if (server.hasArg(n)) {
        String val = server.arg(n);
        strncpy(config.assets[i].name, val.c_str(), sizeof(config.assets[i].name));
        config.assets[i].name[sizeof(config.assets[i].name)-1] = '\0';
      }
      if (server.hasArg(p)) {
        config.assets[i].pin = server.arg(p).toInt();
      }
    }
    if (server.hasArg("maxEvents")) config.maxEvents = constrain(server.arg("maxEvents").toInt(), 100, 5000);
    if (server.hasArg("tzOffset")) config.tzOffset = constrain(server.arg("tzOffset").toInt() * 60, -720*60, 840*60);
    for (int i = 0; i < 5; ++i) {
      String key = "reason" + String(i);
      if (server.hasArg(key)) {
        String v = server.arg(key);
        strncpy(config.downtimeReasons[i], v.c_str(), 31);
        config.downtimeReasons[i][31] = '\0';
      }
    }
    saveConfig();
    server.sendHeader("Location", "/");
    server.send(303);
    delay(200);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

// --------- LOG CLEAR/EXPORT ---------
void handleClearLog() {
  SPIFFS.remove(LOG_FILENAME);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleExportLog() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f) {
    server.send(404, "text/plain", "No log file");
    return;
  }
  // Get current time for filename
  time_t now = time(nullptr);
  struct tm * ti = localtime(&now);
  char fn[64];
  strftime(fn, sizeof(fn), "log-%Y%m%d-%H%M%S.csv", ti);

  // Prepare CSV in memory with headings
  String csv = "Date,Time,Asset,Event,State,Availability (%),Total Runtime (min),Total Downtime (min),MTBF (min),MTTR (min),No. of Stops,Note\n";
  while (f.available()) {
    csv += f.readStringUntil('\n') + "\n";
  }
  f.close();

  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + fn + "\"");
  server.send(200, "text/csv", csv);
}

// --------- REST API HANDLERS ---------
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
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiNote() {
  if (server.hasArg("idx") && server.hasArg("note")) {
    uint8_t idx = server.arg("idx").toInt();
    String note = server.arg("note");
    String reason = server.hasArg("reason") ? server.arg("reason") : "";
    if (idx < config.assetCount) {
      time_t ts = assetStates[idx].lastEventTime;
      addNoteToEvent(ts, idx, note, reason);
      server.sendHeader("Location", "/asset?idx=" + String(idx));
      server.send(303);
      return;
    }
  }
  server.send(400, "text/plain", "Invalid");
}

void addNoteToEvent(time_t timestamp, uint8_t assetIdx, String note, String reason) {
  logEvent(assetIdx, assetStates[assetIdx].lastState, timestamp, (reason.length() ? (reason + " - " + note) : note).c_str());
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}