#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

struct WiFiNet { String ssid; String pass; };
WiFiNet myNets[10] = {
  {"TP-Link_zhenja_4G", "5840902a"},
  {"TP-Link_BA1C", "t19610313"},
  {"Redmi Note 12", "123321123321"}
};
int netsCount = 3;

WebServer server(80);
File uploadFile;
bool isEN = false; 
bool sdStarted = false;
String currentPath = "/";

int pin_SCK = 18, pin_MISO = 19, pin_MOSI = 23, pin_CS = 5;
SemaphoreHandle_t fsMutex;

const int pin_FAN = 25;
const int fan_FREQ = 5000;
const int fan_RES = 8; 
const float TEMP_THROTTLE_ON = 65.0;
const float TEMP_THROTTLE_OFF = 55.0;
bool isThrottled = false;

String msg(String ru, String en) { return isEN ? en : ru; }

String formatSize(uint64_t bytes) {
  if (bytes < 1024) return String(bytes) + " B";
  else if (bytes < (1024 * 1024)) return String(bytes / 1024.0, 2) + " KB";
  else if (bytes < (1024 * 1024 * 1024)) return String(bytes / 1024.0 / 1024.0, 2) + " MB";
  else return String(bytes / 1024.0 / 1024.0 / 1024.0, 2) + " GB";
}

void initSD() {
  SPI.begin(pin_SCK, pin_MISO, pin_MOSI, pin_CS);
  if (SD.begin(pin_CS)) {
    sdStarted = true;
    Serial.println("SD Card: OK");
  } else {
    sdStarted = false;
    Serial.println("SD Card: NOT FOUND");
  }
}

void initCooling() { ledcAttach(pin_FAN, fan_FREQ, fan_RES); }

void updateCooling() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 2000) return; 
  lastCheck = millis();
  float temp = temperatureRead();
  if (isnan(temp)) return;
  if (!isThrottled && temp > TEMP_THROTTLE_ON) { setCpuFrequencyMhz(80); isThrottled = true; } 
  else if (isThrottled && temp < TEMP_THROTTLE_OFF) { setCpuFrequencyMhz(240); isThrottled = false; }
  int speed = (temp > 35.0) ? map((int)temp, 35, 65, 120, 255) : 0;
  ledcWrite(pin_FAN, constrain(speed, 0, 255));
}

bool handleFileRead(String path) {
  if (path.endsWith("/")) path += "index.html";
  String contentType = "text/plain";
  if (path.endsWith(".html")) contentType = "text/html";
  else if (path.endsWith(".js")) contentType = "application/javascript";
  else if (path.endsWith(".css")) contentType = "text/css";
  else if (path.endsWith(".png")) contentType = "image/png";
  
  if (xSemaphoreTake(fsMutex, portMAX_DELAY)) {
    if (path.startsWith("/sd/") && sdStarted) {
      String p = path.substring(3);
      if (SD.exists(p)) {
        File f = SD.open(p, "r");
        server.streamFile(f, contentType);
        f.close(); xSemaphoreGive(fsMutex); return true;
      }
    }
    if (LittleFS.exists(path)) {
      File f = LittleFS.open(path, "r");
      server.streamFile(f, contentType);
      f.close(); xSemaphoreGive(fsMutex); return true;
    }
    xSemaphoreGive(fsMutex);
  }
  return false;
}

const char STYLE[] PROGMEM = R"rawliteral(
<style>
body{background:#0d1117;color:#c9d1d9;font-family:sans-serif;margin:0;display:flex;}
.nav{width:260px;background:#161b22;height:100vh;padding:20px;border-right:1px solid #30363d;position:fixed;}
.nav a{display:block;color:#8b949e;padding:12px;text-decoration:none;border-radius:6px;margin-bottom:8px;}
.nav a.active{background:#21262d;color:#58a6ff;}
.content{margin-left:300px;padding:30px;width:calc(100% - 300px);}
.card{background:#161b22;padding:20px;border-radius:10px;border:1px solid #30363d;margin-bottom:20px;}
.path-bar{background:#0d1117;padding:10px;border-radius:6px;border:1px solid #30363d;margin-bottom:15px;font-family:monospace;}
button{background:#238636;border:none;color:white;padding:10px 18px;border-radius:6px;cursor:pointer;}
button.red{background:#da3633;}
table{width:100%;border-collapse:collapse;}
th, td{text-align:left;padding:12px;border-bottom:1px solid #30363d;}
.folder-link{color:#d29922;font-weight:bold;text-decoration:none;}
.file-link{color:#58a6ff;text-decoration:none;}
.sd-status{padding:5px 10px;border-radius:4px;font-size:0.8rem;font-weight:bold;}
.sd-ok{background:#238636;color:white;} .sd-fail{background:#da3633;color:white;}
.ip-info{color:#00ff00;font-family:monospace;font-size:1.1rem;}
</style>
<script src="https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js"></script>
)rawliteral";

String getNav(int active) {
  String n = String(STYLE);
  n += "<div class='nav'><h2>ESP-OS PRO</h2>";
  n += "<div style='margin-bottom:15px'>";
  n += sdStarted ? String("<span class='sd-status sd-ok'>SD: OK</span>") : String("<span class='sd-status sd-fail'>SD: NONE</span>");
  n += "</div>";
  n += "<a href='/lang'>🌐 " + msg("РУССКИЙ", "ENGLISH") + "</a>";
  n += "<a href='/' class='" + String(active == 0 ? "active" : "") + "'>🏠 " + msg("Главная", "Home") + "</a>";
  n += "<a href='/files_page' class='" + String(active == 1 ? "active" : "") + "'>📂 " + msg("Файлы", "Files") + "</a>";
  n += "<a href='/sys_page' class='" + String(active == 3 ? "active" : "") + "'>⚙️ " + msg("Система", "System") + "</a></div>";
  return n;
}

void handleFilesPage() {
  if (server.hasArg("path")) currentPath = server.arg("path");
  if (!currentPath.endsWith("/")) currentPath += "/";

  if (xSemaphoreTake(fsMutex, portMAX_DELAY)) {
    String s = getNav(1) + "<div class='content'><h1>" + msg("Проводник", "Explorer") + "</h1>";
    s += "<div class='path-bar'><b>📍 Location:</b> " + currentPath + "</div>";
    s += "<div class='card'><button onclick='newDir()'>📁 " + msg("Папка", "Folder") + "</button> ";
    s += "<button onclick=\"document.getElementById('u').click()\">⬆ " + msg("Загрузить", "Upload") + "</button>";
    s += "<input type='file' id='u' style='display:none' onchange='processUpload()' multiple></div>";
    
    s += "<div class='card'><table><tr><th>" + msg("Имя", "Name") + "</th><th>" + msg("Размер", "Size") + "</th><th>" + msg("Удалить", "Del") + "</th></tr>";
    
    if (currentPath == "/") {
       if (sdStarted) s += "<tr><td><a href='/files_page?path=/sd/' class='folder-link'>💾 [ MICRO SD CARD ]</a></td><td>-</td><td>-</td></tr>";
    } else {
       String up = currentPath.substring(0, currentPath.lastIndexOf('/', currentPath.length() - 2) + 1);
       s += "<tr><td><a href='/files_page?path=" + up + "' class='folder-link'>⬅ [ .. Назад ]</a></td><td>-</td><td>-</td></tr>";
    }

    File root = (currentPath.startsWith("/sd/")) ? SD.open(currentPath.substring(3)) : LittleFS.open(currentPath);
    if (root) {
      File f = root.openNextFile();
      while (f) {
        String n = f.name();
        String fullP = currentPath + n;
        s += "<tr><td>";
        if (f.isDirectory()) s += "<a href='/files_page?path="+fullP+"/' class='folder-link'>📁 "+n+"</a>";
        else s += "<a href='"+fullP+"' target='_blank' class='file-link'>📄 "+n+"</a>";
        s += "</td><td>" + (f.isDirectory() ? "-" : formatSize(f.size())) + "</td>";
        s += "<td><button class='red' onclick=\"if(confirm('Del?'))location.href='/delete?f="+fullP+"'\">X</button></td></tr>";
        f = root.openNextFile();
      }
    }
    s += "</table></div>";
    s += "<script>function newDir(){let n=prompt('Name:');if(n)location.href='/mkdir?n='+n+'&p="+currentPath+"';}";
    s += "async function processUpload(){const files=document.getElementById('u').files;for(let i=0;i<files.length;i++){let fd=new FormData();fd.append('f',files[i],files[i].name);await fetch('/upload?path="+currentPath+"',{method:'POST',body:fd});}location.reload();}</script></div>";
    xSemaphoreGive(fsMutex);
    server.send(200, "text/html; charset=utf-8", s);
  }
}

void setup() {
  Serial.begin(115200);
  fsMutex = xSemaphoreCreateMutex();
  LittleFS.begin(true);
  initSD();
  initCooling();

  WiFi.mode(WIFI_AP_STA);
  
  // Настройка точки доступа (AP)
  WiFi.softAP("ESP32_SYSTEM", "12345678");

  bool connected = false;
  for (int i = 0; i < netsCount; i++) {
    WiFi.begin(myNets[i].ssid.c_str(), myNets[i].pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(500);
    if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
  }

  if (!connected) {
    Serial.println("WiFi STA Connection Failed.");
  }

  Serial.println("\n--- ИНФОРМАЦИЯ О ПОДКЛЮЧЕНИИ ---");
  Serial.printf("1. Локальная сеть (STA): http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("2. Прямая точка (AP): http://%s/ (SSID: ESP32_SYSTEM)\n", WiFi.softAPIP().toString().c_str());
  Serial.println("--------------------------------\n");

  server.on("/", []() { if (!handleFileRead("/")) handleFilesPage(); });
  server.on("/files_page", handleFilesPage);
  
  server.on("/mkdir", []() {
    String p = server.arg("p") + server.arg("n");
    if (xSemaphoreTake(fsMutex, portMAX_DELAY)) {
      if (p.startsWith("/sd/")) SD.mkdir(p.substring(3));
      else LittleFS.mkdir(p);
      xSemaphoreGive(fsMutex);
    }
    server.sendHeader("Location", "/files_page?path=" + server.arg("p"));
    server.send(303);
  });

  server.on("/upload", HTTP_POST, [](){ server.send(200); }, [](){
    HTTPUpload& u = server.upload();
    String path = server.arg("path") + u.filename;
    if (u.status == UPLOAD_FILE_START && xSemaphoreTake(fsMutex, portMAX_DELAY)) {
      uploadFile = (path.startsWith("/sd/")) ? SD.open(path.substring(3), "w") : LittleFS.open(path, "w");
    } else if (u.status == UPLOAD_FILE_WRITE && uploadFile) {
      uploadFile.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END && uploadFile) {
      uploadFile.close(); xSemaphoreGive(fsMutex);
    }
  });

  server.on("/delete", []() {
    String f = server.arg("f");
    if (xSemaphoreTake(fsMutex, portMAX_DELAY)) {
      if (f.startsWith("/sd/")) { if(!SD.remove(f.substring(3))) SD.rmdir(f.substring(3)); }
      else { if(!LittleFS.remove(f)) LittleFS.rmdir(f); }
      xSemaphoreGive(fsMutex);
    }
    server.sendHeader("Location", "/files_page?path=" + currentPath);
    server.send(303);
  });

  server.on("/sys_page", []() {
    String s = getNav(3) + "<div class='content'><h1>" + msg("Статус Системы", "System Status") + "</h1>";
    
    // БЛОК С АДРЕСАМИ
    s += "<div class='card'><h3>🌐 " + msg("Сетевые адреса", "Network Addresses") + "</h3>";
    s += "<p>" + msg("Прямое подключение (AP): ", "Direct Access (AP): ") + "<b class='ip-info'>http://" + WiFi.softAPIP().toString() + "/</b></p>";
    if(WiFi.status() == WL_CONNECTED) {
       s += "<p>" + msg("Локальная сеть (STA): ", "Local Network (STA): ") + "<b class='ip-info'>http://" + WiFi.localIP().toString() + "/</b></p>";
    } else {
       s += "<p style='color:red'>" + msg("STA не подключен", "STA Disconnected") + "</p>";
    }
    s += "</div>";

    s += "<div class='card'><h3>🌡️ CPU: " + String(temperatureRead(), 1) + "°C</h3></div>";
    s += "<div class='card'><h3>💾 LittleFS (Flash)</h3><p>Used: " + formatSize(LittleFS.usedBytes()) + " / " + formatSize(LittleFS.totalBytes()) + "</p></div>";
    if (sdStarted) {
      s += "<div class='card'><h3>💾 MicroSD Card</h3><p>Size: " + formatSize(SD.cardSize()) + "</p>";
      s += "<p>Type: " + String(SD.cardType() == CARD_SDHC ? "SDHC" : "SD") + "</p></div>";
    }
    s += "<div class='card'><h3>🔗 WiFi Networks</h3>";
    s += "<form action='/add_wifi' method='POST'>";
    s += "<input name='s' placeholder='SSID'> <input name='p' placeholder='PASS'> <button type='submit'>Add</button></form><hr>";
    for(int i=0; i<netsCount; i++) s += "<div>• " + myNets[i].ssid + "</div>";
    s += "</div><button class='red' onclick=\"location.href='/restart'\">RESTART</button></div>";
    server.send(200, "text/html; charset=utf-8", s);
  });

  server.on("/add_wifi", HTTP_POST, []() {
    if (netsCount < 10) {
      myNets[netsCount].ssid = server.arg("s");
      myNets[netsCount].pass = server.arg("p");
      netsCount++;
    }
    server.sendHeader("Location", "/sys_page"); server.send(303);
  });

  server.on("/restart", []() { ESP.restart(); });
  server.on("/lang", []() { isEN = !isEN; server.sendHeader("Location", "/sys_page"); server.send(303); });
  server.onNotFound([]() { if (!handleFileRead(server.uri())) server.send(404); });

  server.begin();
  xTaskCreatePinnedToCore([](void*p){for(;;){server.handleClient();vTaskDelay(2);}}, "Srv", 10000, NULL, 1, NULL, 1);
}

void loop() { updateCooling(); vTaskDelay(1000 / portTICK_PERIOD_MS); }