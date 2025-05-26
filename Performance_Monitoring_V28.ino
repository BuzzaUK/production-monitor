#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <time.h>

// USER CONFIGURATION
const char* ssid = "New Router";
const char* password = "Diamond100";
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

WebServer server(80);
Preferences prefs;
AssetState assetStates[MAX_ASSETS];

void loadConfig();
void saveConfig();
void setupWiFi();
void setupTime();
void logEvent(uint8_t assetIdx, bool state, time_t now, const char* note = nullptr);
String htmlDashboard();
String htmlConfig();
String htmlEvents();
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

void setup() {
  Serial.begin(115200);
  if (!SPIFFS.begin(true)) { Serial.println("SPIFFS failed!"); return; }
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
    assetStates[i].lastEventTime = 0;
  }

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
  server.on("/save_config", HTTP_POST, handleConfigPost);
  server.on("/clear_log", HTTP_POST, handleClearLog);
  server.on("/export_log", HTTP_GET, handleExportLog);

  // API
  server.on("/api/summary", HTTP_GET, handleApiSummary);
  server.on("/api/events", HTTP_GET, handleApiEvents);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/note", HTTP_POST, handleApiNote);

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
  time_t now = time(nullptr);
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    bool state = digitalRead(config.assets[i].pin);
    if (state != assetStates[i].lastState) {
      logEvent(i, state, now);
      unsigned long elapsed = now - assetStates[i].lastChangeTime;
      if (state) { assetStates[i].stoppedTime += elapsed; assetStates[i].runCount++; }
      else { assetStates[i].runningTime += elapsed; assetStates[i].stopCount++; }
      assetStates[i].lastState = state;
      assetStates[i].lastChangeTime = now;
      assetStates[i].lastEventTime = now;
    }
  }
  delay(200);
}

// CONFIG LOAD/SAVE
void loadConfig() {
  if (prefs.isKey("cfg")) { prefs.getBytes("cfg", &config, sizeof(config)); }
  else {
    config.assetCount = 2; config.maxEvents = 1000;
    strcpy(config.assets[0].name, "Line 1"); config.assets[0].pin = 4;
    strcpy(config.assets[1].name, "Line 2"); config.assets[1].pin = 12;
    for (int i = 0; i < 5; ++i) strncpy(config.downtimeReasons[i], DEFAULT_DOWNTIME_REASONS[i], 31);
    config.tzOffset = 0; saveConfig();
  }
}
void saveConfig() { prefs.putBytes("cfg", &config, sizeof(config)); }

// WIFI & TIME
void setupWiFi() {
  WiFi.mode(WIFI_STA); WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println(); Serial.print("Connected, IP: "); Serial.println(WiFi.localIP());
}
void setupTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1); tzset();
  Serial.print("Waiting for NTP time sync...");
  time_t now = time(nullptr); int retry = 0;
  while (now < 8 * 3600 * 2 && retry < 30) { delay(200); Serial.print("."); now = time(nullptr); retry++; }
  Serial.println(" done");
}

// LOGGING
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
  mtbf = mtbf / 60.0; mttr = mttr / 60.0;

  struct tm * ti = localtime(&now);
  char datebuf[11], timebuf[9];
  strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", ti);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", ti);

  File f = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
  if (!f) { Serial.println("Failed to open log file for writing!"); return; }
  f.printf("%s,%s,%s,%s,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%s\n",
    datebuf, timebuf, config.assets[assetIdx].name,
    state ? "START" : "STOP",
    state, avail, total_runtime_min, total_downtime_min, mtbf, mttr, as.stopCount, note ? note : ""
  );
  f.close();
  as.lastEventTime = now;
  Serial.println("Wrote event to log.");
}

// DASHBOARD PAGE (with mobile cards)
String htmlDashboard() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;padding:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.3rem 0;text-align:center;box-shadow:0 2px 10px #0001;}";
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;}";
  html += ".nav button{background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;}";
  html += ".nav button:hover{background:#e3f0fc;}";
  html += ".main{max-width:1000px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += "#chart-container{width:100%;overflow:auto;}";
  html += "table{width:100%;border-collapse:collapse;font-size:1em;margin-top:0.8em;}";
  html += "th,td{padding:0.7em 0.5em;text-align:left;border-bottom:1px solid #eee;}";
  html += "th{background:#2196f3;color:#fff;}";
  html += "tr{background:#fcfcfd;} tr:nth-child(even){background:#f3f7fa;}";
  html += ".statrow{display:flex;gap:1rem;flex-wrap:wrap;justify-content:space-between;}";
  html += ".stat{flex:1 1 100px;border-radius:7px;padding:0.9em;text-align:center;font-size:1.1em;margin:0.3em 0;box-shadow:0 2px 8px #0001;font-weight:500;}";
  html += ".stat.running{background:#e6fbe7;border:2px solid #54c27c;}";
  html += ".stat.stopped{background:#ffeaea;border:2px solid #f44336;}";
  html += "@media(max-width:700px){";
  html += "#summaryTable{display:none;}";
  html += "#mobileDashboard .dashCard {border-radius: 10px;box-shadow: 0 2px 10px #0001;margin-bottom: 1.2em;padding: 1em;font-size: 1.05em;}";
  html += "#mobileDashboard .dashCard > div {margin-bottom: 0.3em;}";
  html += "#mobileDashboard .dashCard.running {background:#e6fbe7;border:2px solid #54c27c;}";
  html += "#mobileDashboard .dashCard.stopped {background:#ffeaea;border:2px solid #f44336;}";
  html += "}";
  html += "@media (min-width:701px){#mobileDashboard{display:none;}}";
  html += "</style>";
  html += "</head><body>";
  html += "<header><div style='font-size:1.6em;font-weight:700;'>Dashboard</div></header>";
  html += "<nav class='nav'>";
  html += "<form action='/events'><button type='submit'>Event Log</button></form>";
  html += "<form action='/config'><button type='submit'>Setup</button></form>";
  html += "<form action='/export_log'><button type='submit'>Export CSV</button></form>";
  html += "</nav>";
  html += "<div class='main'>";
  html += "<div class='card'>";
  html += "<div id='chart-container'><canvas id='barChart' height='200'></canvas></div>";
  html += "<div class='statrow' id='statrow'></div>";
  html += "<div style='overflow-x:auto;'><table id='summaryTable'><thead><tr>";
  html += "<th>Name</th><th>State</th><th>Avail (%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th></tr></thead><tbody></tbody></table>";
  html += "<div id='mobileDashboard'></div>";
  html += "</div></div>";
  html += "<script>";
  html += R"rawliteral(
function formatMMSS(val) {
  if (isNaN(val) || val < 0.01) return "00:00";
  let min = Math.floor(val);
  let sec = Math.round((val - min) * 60);
  if (sec == 60) { min++; sec = 0; }
  return (min<10?"0":"")+min+":"+(sec<10?"0":"")+sec;
}
let chartObj=null;
function updateDashboard() {
  fetch('/api/summary').then(r=>r.json()).then(data=>{
    let tbody = document.querySelector('#summaryTable tbody');
    let assets = data.assets;
    let rows = tbody.rows;
    let statrow = document.getElementById('statrow');
    let mobileDiv = document.getElementById('mobileDashboard');
    statrow.innerHTML = '';
    mobileDiv.innerHTML = '';
    let n=assets.length;
    let isMobile = window.innerWidth <= 700;
    for(let i=0;i<n;++i){
      let asset = assets[i];
      let stateClass = asset.state==1 ? "running" : "stopped";
      if (!isMobile) {
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
        for(let j=0;j<8;++j) row.cells[j].innerHTML = vals[j];
        let statHtml = `<div class='stat ${stateClass}'><b>${asset.name}</b><br>Avail: ${asset.availability.toFixed(1)}%<br>Run: ${formatMMSS(asset.total_runtime)}<br>Stops: ${asset.stop_count}</div>`;
        statrow.innerHTML += statHtml;
      } else {
        let card = document.createElement('div');
        card.className = 'dashCard ' + stateClass;
        card.innerHTML =
          "<div><b>Name:</b> " + asset.name + "</div>" +
          "<div><b>State:</b> " + (asset.state==1?'<span style=\"color:#256029;font-weight:bold\">RUNNING</span>':'<span style=\"color:#b71c1c;font-weight:bold\">STOPPED</span>') + "</div>" +
          "<div><b>Avail(%):</b> " + asset.availability.toFixed(2) + "</div>" +
          "<div><b>Runtime:</b> " + formatMMSS(asset.total_runtime) + "</div>" +
          "<div><b>Downtime:</b> " + formatMMSS(asset.total_downtime) + "</div>" +
          "<div><b>MTBF:</b> " + formatMMSS(asset.mtbf) + "</div>" +
          "<div><b>MTTR:</b> " + formatMMSS(asset.mttr) + "</div>" +
          "<div><b>Stops:</b> " + asset.stop_count + "</div>";
        mobileDiv.appendChild(card);
      }
    }
    if (!isMobile) while (rows.length > n) tbody.deleteRow(rows.length-1);
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

// EVENTS PAGE (with inhibit scrolling button and newest at top, mobile scroll fix)
String htmlEvents() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Event Log</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001;}";
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0 0 0;flex-wrap:wrap;}";
  html += ".nav button{background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;transition:.2s;cursor:pointer;margin-bottom:0.5rem;} .nav button:hover{background:#e3f0fc;}";
  html += ".main{max-width:1100px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += ".filterrow{display:flex;gap:1em;align-items:center;margin-bottom:1em;flex-wrap:wrap;}";
  html += ".filterrow label{font-weight:bold;}";
  html += ".scrollToggle{margin-left:auto;font-size:1em;}";
  html += "table{width:100%;border-collapse:collapse;font-size:1em;margin-top:0.8em;}";
  html += "th,td{padding:0.7em 0.5em;text-align:left;border-bottom:1px solid #eee;}";
  html += "th{background:#2196f3;color:#fff;}";
  html += "tr{background:#fcfcfd;} tr:nth-child(even){background:#f3f7fa;}";
  html += ".note{font-style:italic;color:#555;} .notebtn{padding:2px 8px;font-size:1em;border-radius:4px;background:#1976d2;color:#fff;border:none;cursor:pointer;margin-left:0.5em;} .notebtn:hover{background:#0d47a1;}";
  html += ".noteform{display:none;background:#e3f0fc;padding:0.7em;margin:0.5em 0;border-radius:8px;} .noteform select,.noteform input{margin-right:6px;font-size:1em;padding:0.2em 0.5em;} .noteform button{margin:0 4px;}";
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
  // Show newest events at the top
  let displayData = eventData.slice().reverse();
  for (let i=0; i<displayData.length; ++i) {
    let vals = displayData[i].split(',');
    if (vals.length < 12) continue;
    let ldate = vals[0], ltime = vals[1], lasset = vals[2], levent = vals[3], lstate = vals[4];
    let lavail = vals[5], lrun = vals[6], lstop = vals[7], lmtbf = vals[8], lmttr = vals[9], lsc = vals[10], lnote = vals.slice(11).join(',');
    if (filterValue != "ALL" && lasset != filterValue) continue;
    if (!stateMatch(lstate)) continue;
    let noteFormId = 'noteform-' + btoa(ldate + "|" + ltime + "|" + lasset).replace(/[^a-zA-Z0-9]/g, "_");
    let noteFormHtml = `
      <form class='noteform' id='${noteFormId}' onsubmit='return submitNote(event,"${ldate}","${ltime}","${lasset}")' style='display:none;'>
        <input type='hidden' name='date' value='${ldate}'>
        <input type='hidden' name='time' value='${ltime}'>
        <input type='hidden' name='asset' value='${lasset}'>
        <label>Reason: <select name='reason'>
          <option value=''></option>
          ${window.downtimeReasons.map(r =>
            `<option value="${r.replace(/"/g, "&quot;")}">${r}</option>`).join("")}
        </select></label>
        <input type='text' name='note' value='${lnote.replace(/["']/g,"&quot;")}' maxlength='64' placeholder='Add/Edit note'>
        <button type='submit'>Save</button>
        <button type='button' onclick='hideNoteForm("${noteFormId}")'>Cancel</button>
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
      tr.appendChild(td(lavail));
      tr.appendChild(td(lrun));
      tr.appendChild(td(lstop));
      tr.appendChild(td(lmtbf));
      tr.appendChild(td(lmttr));
      tr.appendChild(td(lsc));
      let tdNote = document.createElement('td');
      tdNote.innerHTML = `<span class='note'>${lnote}</span>`;
      if (lstate == "0") {
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
        <div><b>Avail(%):</b> ${lavail}</div>
        <div><b>Runtime:</b> ${lrun}</div>
        <div><b>Downtime:</b> ${lstop}</div>
        <div><b>MTBF:</b> ${lmtbf}</div>
        <div><b>MTTR:</b> ${lmttr}</div>
        <div><b>Stops:</b> ${lsc}</div>
        <div><b>Note:</b> <span class='note'>${lnote}</span>
        ${(lstate == "0" ? ` <button class='notebtn' onclick='showNoteForm("${noteFormId}")'>Edit</button>` : "")}
        ${lstate == "0" ? noteFormHtml : ""}</div>`;
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
  if (form) { form.style.display = 'block'; window.openNoteFormId = noteFormId; }
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
  html += "<label for='stateFilter'>Event State:</label> <select id='stateFilter'><option value='ALL'>All</option><option value='RUNNING'>Running</option><option value='STOPPED'>Stopped</option></select> <span id='eventCount'></span>";
  html += "<button class='scrollToggle' id='scrollBtn' type='button' onclick='toggleScrollInhibit(this)'>Inhibit Auto-Scroll</button></div>";
  html += "<div style='overflow-x:auto;'><table id='eventTable'><thead><tr>";
  html += "<th>Date</th><th>Time</th><th>Asset</th><th>Event</th><th>State</th><th>Avail(%)</th><th>Runtime</th><th>Downtime</th><th>MTBF</th><th>MTTR</th><th>Stops</th><th>Note</th></tr>";
  html += "</thead><tbody id='tbody'></tbody></table>";
  html += "<div id='mobileEvents'></div>";
  html += "</div></div></body></html>";
  return html;
}

// SETUP PAGE
String htmlConfig() {
  String html = "<!DOCTYPE html><html lang='en'><head><title>Setup</title><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<link href='https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap' rel='stylesheet'>";
  html += "<style>";
  html += "body{font-family:Roboto,sans-serif;background:#f3f7fa;margin:0;}";
  html += "header{background:#1976d2;color:#fff;padding:1.2rem 0;text-align:center;box-shadow:0 2px 10px #0001;}";
  html += ".nav{display:flex;justify-content:center;gap:1rem;margin:1rem 0;flex-wrap:wrap;align-items:center;}";
  html += ".nav button{background:#fff;color:#1976d2;border:none;border-radius:6px;padding:0.7em 1.1em;font-size:1.1em;font-weight:700;box-shadow:0 2px 8px #0001;cursor:pointer;}";
  html += ".nav button:hover{background:#e3f0fc;}";
  html += ".nav .right{margin-left:auto;}";
  html += ".main{max-width:700px;margin:1rem auto;padding:1rem;}";
  html += ".card{background:#fff;border-radius:10px;box-shadow:0 2px 16px #0002;margin-bottom:1.3rem;padding:1.3rem;}";
  html += "label{font-weight:500;margin-top:1em;display:block;}input[type=text],input[type=number],select{width:100%;padding:0.6em;margin-top:0.2em;margin-bottom:1em;border:1px solid #ccc;border-radius:6px;font-size:1.1em;}";
  html += "input[type=submit],button{margin-top:1em;padding:0.8em 1.5em;font-size:1.15em;border-radius:8px;border:none;background:#1976d2;color:#fff;font-weight:700;cursor:pointer;}";
  html += "fieldset{border:1px solid #e0e0e0;padding:1em;border-radius:7px;margin-bottom:1em;}";
  html += "legend{font-weight:700;color:#2196f3;font-size:1.1em;}";
  html += ".notice{background:#e6fbe7;color:#256029;font-weight:bold;padding:0.6em 1em;border-radius:7px;margin-bottom:1em;}";
  html += "@media(max-width:700px){.main{padding:0.5rem;} .card{padding:0.7rem;} input[type=submit],button{font-size:1em;} fieldset{padding:0.7em;}}";
  html += "</style>";
  html += "<script>";
  html += "function clearLogDblConfirm(e){";
  html += "if(!confirm('Are you sure you want to CLEAR ALL LOG DATA?')){e.preventDefault();return false;}";
  html += "if(!confirm('Double check: This cannot be undone! Are you REALLY sure you want to clear the log?')){e.preventDefault();return false;}";
  html += "return true;}";
  html += "function showSavedMsg(){";
  html += "document.getElementById('saveNotice').style.display='block';";
  html += "}";
  html += "</script>";
  html += "</head><body>";
  html += "<header><div style='font-size:1.6em;font-weight:700;'>Setup</div></header>";
  html += "<nav class='nav'>";
  html += "<form action='/'><button type='submit'>Dashboard</button></form>";
  html += "<form action='/events'><button type='submit'>Event Log</button></form>";
  html += "<form action='/export_log'><button type='submit'>Export CSV</button></form>";
  html += "<form action='/clear_log' method='POST' style='display:inline;' onsubmit='return clearLogDblConfirm(event);'><button type='submit' style='background:#f44336;color:#fff;' class='right'>Clear Log</button></form>";
  html += "</nav>";
  html += "<div class='main'><div class='card'>";
  html += "<div id='saveNotice' class='notice' style='display:none;'>Save &amp; reboot completed. You can continue setup or return to dashboard.</div>";
  html += "<form method='POST' action='/save_config' id='setupform' onsubmit='setTimeout(showSavedMsg, 1500);'>";
  html += "<label>Asset count (max 10): <input type='number' name='assetCount' min='1' max='" + String(MAX_ASSETS) + "' value='" + String(config.assetCount) + "' required></label>";
  for (uint8_t i = 0; i < config.assetCount; ++i) {
    html += "<fieldset><legend>Asset #" + String(i+1) + "</legend>";
    html += "<label>Name: <input type='text' name='name" + String(i) + "' value='" + String(config.assets[i].name) + "' maxlength='31' required></label>";
    html += "<label>GPIO Pin: <input type='number' name='pin" + String(i) + "' value='" + String(config.assets[i].pin) + "' min='0' max='39' required></label>";
    html += "</fieldset>";
  }
  html += "<label>Max events per asset (log size): <input type='number' name='maxEvents' min='100' max='5000' value='" + String(config.maxEvents) + "' required></label>";
  html += "<label>Timezone offset from UTC (hours): <input type='number' name='tzOffset' min='-12' max='14' step='0.5' value='" + String(config.tzOffset / 3600.0, 1) + "' required></label>";
  html += "<fieldset><legend>Downtime Quick Reasons</legend>";
  for (int i = 0; i < 5; ++i) {
    html += "<label>Reason " + String(i+1) + ": <input type='text' name='reason" + String(i) + "' value='" + String(config.downtimeReasons[i]) + "' maxlength='31'></label>";
  }
  html += "</fieldset>";
  html += "<input type='submit' value='Save & Reboot'>";
  html += "</form>";
  html += "</div></div></body></html>";
  return html;
}

// ASSET DETAIL PAGE (optional simple version)
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
      if (server.hasArg(p)) { config.assets[i].pin = server.arg(p).toInt(); }
    }
    if (server.hasArg("maxEvents")) config.maxEvents = constrain(server.arg("maxEvents").toInt(), 100, 5000);
    if (server.hasArg("tzOffset")) config.tzOffset = constrain(server.arg("tzOffset").toInt() * 60 * 60, -12*3600, 14*3600);
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
void handleClearLog() { SPIFFS.remove(LOG_FILENAME); server.sendHeader("Location", "/config"); server.send(303); }
void handleExportLog() {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f) { server.send(404, "text/plain", "No log file"); return; }
  time_t now = time(nullptr);
  struct tm * ti = localtime(&now);
  char fn[64];
  strftime(fn, sizeof(fn), "log-%Y%m%d-%H%M%S.csv", ti);
  String csv = "Date,Time,Asset,Event,State,Availability (%),Total Runtime (min),Total Downtime (min),MTBF (min),MTTR (min),No. of Stops,Note\n";
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
void updateEventNote(String date, String time, String assetName, String note, String reason) {
  File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!f) return;
  String all = "";
  String newNote = reason.length() > 0 ? (note.length() > 0 ? (reason + " - " + note) : reason) : note;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    String origLine = line;
    line.trim();
    if (line.length()<5) { all += origLine+"\n"; continue; }
    int p[12]; int count=0; p[0]=0;
    for(int i=0;i<11&&count<11;++i) { int idx=line.indexOf(',',p[count]); if(idx<0) break; p[++count]=idx+1; }
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