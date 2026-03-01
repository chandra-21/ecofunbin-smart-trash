// =======================================================
// ESP32 ECOFUN BIN SYSTEM v6.3.1 - CONFIG BUTTON FIX
// CONFIG BUTTON WORKS ANYTIME
// =======================================================

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <SPI.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include <ESP32Servo.h>
#include <DFRobotDFPlayerMini.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ==================== VERSION ====================
#define FIRMWARE_VERSION "6.3.1"

// ---------------- FIREBASE (HARDCODED) ----------------
#define API_KEY "AIzaSyAJ0J1yqm1iELE_L31VZ2WtO_A1mkYyvyY"
#define DATABASE_URL "https://smart-trash-v2-default-rtdb.asia-southeast1.firebasedatabase.app/"

// ---------------- WIFI PORTAL CONFIG ----------------
#define CONFIG_BUTTON   0
const char* AP_SSID = "EcoFunBin-Setup";
const char* AP_PASS = "12345678";
const char* MDNS_HOSTNAME = "ecofunbin";

// ---------------- SERVO ----------------
#define SERVO_ORG_PIN  13
#define SERVO_ANO_PIN  12
#define SERVO_CLOSE    0
#define SERVO_OPEN     160

// ---------------- SENSOR PIN ----------------
#define IR_ORG   34
#define IND_ORG  36
#define CAP_ORG  39
#define IR_ANO   33
#define IND_ANO  35
#define CAP_ANO  32

// ---------------- ULTRASONIC PIN ----------------
#define TRIG_ORG  26
#define ECHO_ORG  25
#define TRIG_ANO  14
#define ECHO_ANO  27

// ---------------- DFPLAYER PIN ----------------
#define RX_PIN   16
#define TX_PIN   17

// ---------------- RELAY PILOT LAMP ----------------
#define RELAY_GREEN  4
#define RELAY_RED    15

// ---------------- TIMING CONSTANTS ----------------
#define SENSOR_READ_TIME 8000
#define SERVO_OPEN_TIME  3000
#define IR_WAIT_TIME     5000
#define LAMP_ON_TIME     2000
#define ULTRASONIC_INTERVAL 500
#define BLINK_INTERVAL   500
#define LCD_UPDATE_INTERVAL 500
#define BUTTON_HOLD_TIME 3000
#define SOUND_TIMEOUT    10000
#define DEBOUNCE_SAMPLES 5
#define DEBOUNCE_DELAY   50

// ---------------- ULTRASONIC CALIBRATION ----------------
#define DISTANCE_EMPTY_ORG   40
#define DISTANCE_EMPTY_ANO   40
#define DISTANCE_FULL_ORG    10
#define DISTANCE_FULL_ANO    10
#define FULL_THRESHOLD       95
#define SMOOTHING_SAMPLES 3

// ---------------- SOUND INDEX ----------------
#define SND_WELCOME       1
#define SND_CORRECT       3
#define SND_WRONG         5
#define SND_MOVE_TRASH    7
#define SND_CARD_ERROR    9
#define SND_THANKYOU      11
#define SND_ORGANIC       13
#define SND_ANORGANIC     15

// ---------------- FreeRTOS TASK PRIORITIES ----------------
#define PRIORITY_ULTRASONIC  1
#define PRIORITY_LCD         2
#define PRIORITY_SOUND       2
#define PRIORITY_LAMP        1
#define PRIORITY_STATE       3
#define PRIORITY_RFID        3
#define PRIORITY_CONFIG      2

// ---------------- FreeRTOS STACK SIZES ----------------
#define STACK_ULTRASONIC  4096
#define STACK_LCD         3072
#define STACK_SOUND       3072
#define STACK_LAMP        2048
#define STACK_STATE       8192
#define STACK_RFID        6144
#define STACK_CONFIG      4096

// ==================== GLOBAL OBJECTS ====================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
LiquidCrystal_I2C lcd(0x27, 20, 4);

MFRC522DriverPinSimple ss_pin(5);
MFRC522DriverSPI driver{ss_pin};
MFRC522 mfrc522{driver};

Servo servoOrg, servoAno;

HardwareSerial dfSerial(2);
DFRobotDFPlayerMini myDFPlayer;

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

// ==================== FREERTOS HANDLES ====================
TaskHandle_t taskUltrasonicHandle = NULL;
TaskHandle_t taskLCDHandle = NULL;
TaskHandle_t taskSoundHandle = NULL;
TaskHandle_t taskLampHandle = NULL;
TaskHandle_t taskStateHandle = NULL;
TaskHandle_t taskRFIDHandle = NULL;
TaskHandle_t taskConfigButtonHandle = NULL;

// ==================== FREERTOS MUTEXES ====================
SemaphoreHandle_t mutexLCD = NULL;
SemaphoreHandle_t mutexServo = NULL;
SemaphoreHandle_t mutexSound = NULL;
SemaphoreHandle_t mutexState = NULL;
SemaphoreHandle_t mutexFirebase = NULL;
SemaphoreHandle_t mutexUserData = NULL;

// ==================== FREERTOS QUEUES ====================
QueueHandle_t queueSound = NULL;
QueueHandle_t queueLCDUpdate = NULL;

// ==================== GLOBAL VARIABLES ====================
String wifiSSID = "";
String wifiPassword = "";
volatile bool configMode = false;
unsigned long buttonPressTime = 0;

String uidCard = "";
String studentName = "";
int studentPoint = 0;
bool loggedIn = false;
bool busyDetect = false;

// Sound Management
volatile bool soundPlaying = false;
unsigned long lastSoundStartTime = 0;
int currentSound = 0;

// Ultrasonic variables
volatile int levelOrg = 0;
volatile int levelAno = 0;
int lastLevelOrg = -1;
int lastLevelAno = -1;
volatile bool binFull = false;
bool blinkState = false;

long distOrgBuffer[SMOOTHING_SAMPLES];
long distAnoBuffer[SMOOTHING_SAMPLES];
int bufferIndex = 0;
bool bufferFilled = false;

enum TrashType { NONE, ORGANIC, PLASTIC, METAL };
enum State { 
  STANDBY, 
  LOGIN_DISPLAY, 
  READY, 
  DETECTING, 
  CORRECT_RESULT, 
  WRONG_RESULT, 
  WAITING_CORRECT_BIN, 
  WAITING_SENSOR, 
  OPENING_SERVO, 
  SERVO_WAIT,
  UPDATING_DB 
};

volatile State currentState = STANDBY;
unsigned long stateTimer = 0;
TrashType detectedType = NONE;
bool isOrganic = false;

// ==================== QUEUE STRUCTURES ====================
struct SoundCommand {
  int soundNumber;
};

struct LCDUpdateRequest {
  bool updateLevels;
};

// =======================================================
// FORWARD DECLARATIONS
// =======================================================
void loadConfig();
void saveConfig();
void connectWiFi();
void firebaseInit();
void enterConfigMode();
void handleRoot();
void handleScan();
void handleSave();

// =======================================================
// LCD FUNCTIONS - CLEAN VERSION WITH LOADING BAR
// =======================================================

void drawLoadingBar(int progress) {
  lcd.setCursor(0, 3);
  
  int filledBlocks = map(progress, 0, 100, 0, 20);
  
  for (int i = 0; i < 20; i++) {
    if (i < filledBlocks) {
      lcd.write(0xFF);
    } else {
      lcd.write('-');
    }
  }
}

void centerText(int row, String text) {
  lcd.setCursor(0, row);
  lcd.print("                    ");
  
  if (text.length() > 20) {
    text = text.substring(0, 20);
  }
  
  int pos = (20 - text.length()) / 2;
  lcd.setCursor(pos, row);
  lcd.print(text);
}

void showHeader() {
  lcd.setCursor(0, 0);
  lcd.print("   --ECOFUN BIN--   ");
}

void showBinLevels() {
  lcd.setCursor(0, 1);
  
  char buffer[21];
  sprintf(buffer, "A:%3d%%  O:%3d%%", levelAno, levelOrg);
  
  int textLen = strlen(buffer);
  int padding = (20 - textLen) / 2;
  
  lcd.print("                    ");
  lcd.setCursor(padding, 1);
  lcd.print(buffer);
  
  if (binFull) {
    lcd.setCursor(16, 1);
    lcd.print("FULL");
  }
}

void showLoadingScreen(String statusText, int progress) {
  if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
    lcd.clear();
    showHeader();
    showBinLevels();
    centerText(2, statusText);
    drawLoadingBar(progress);
    xSemaphoreGive(mutexLCD);
  }
}

void showNameAndPoints(String name, int points) {
  if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
    lcd.clear();
    showHeader();
    showBinLevels();
    centerText(2, name);
    String pointText = "Poin: " + String(points);
    centerText(3, pointText);
    xSemaphoreGive(mutexLCD);
  }
}

void showStandby() {
  if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
    lcd.clear();
    showHeader();
    showBinLevels();
    centerText(2, "TAP KARTU RFID");
    centerText(3, "UNTUK MEMULAI");
    xSemaphoreGive(mutexLCD);
  }
}

void showErrorCard() {
  if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
    lcd.clear();
    showHeader();
    showBinLevels();
    centerText(2, "KARTU BELUM");
    centerText(3, "TERDAFTAR");
    xSemaphoreGive(mutexLCD);
  }
}

void showReady() {
  if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
    lcd.clear();
    showHeader();
    showBinLevels();
    centerText(2, "SILAKAN BUANG");
    centerText(3, "SAMPAHMU");
    xSemaphoreGive(mutexLCD);
  }
}

void showConfigMode() {
  if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(200)) == pdTRUE) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("   CONFIG MODE      ");
    lcd.setCursor(0, 1);
    lcd.print("SSID: " + String(AP_SSID));
    lcd.setCursor(0, 2);
    lcd.print("Pass: " + String(AP_PASS));
    lcd.setCursor(0, 3);
    lcd.print(WiFi.softAPIP().toString());
    xSemaphoreGive(mutexLCD);
  }
}

// =======================================================
// ULTRASONIC FUNCTIONS
// =======================================================
long readUltrasonic(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return 999;
  
  long distance = duration * 0.034 / 2;
  if (distance < 0) distance = 0;
  
  return distance;
}

long getSmoothedDistance(long newValue, long* buffer, int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += buffer[i];
  }
  return sum / samples;
}

int calculateLevel(long distance, int distEmpty, int distFull) {
  if (distance >= distEmpty) return 0;
  if (distance <= distFull) return 100;
  
  int level = map(distance, distEmpty, distFull, 0, 100);
  
  if (level < 0) level = 0;
  if (level > 100) level = 100;
  
  return level;
}

// =======================================================
// SENSOR READING WITH DEBOUNCE
// =======================================================
TrashType readSensorsSingle(bool organicBin) {
  int irPin  = organicBin ? IR_ORG  : IR_ANO;
  int indPin = organicBin ? IND_ORG : IND_ANO;
  int capPin = organicBin ? CAP_ORG : CAP_ANO;

  bool irActive  = (digitalRead(irPin)  == LOW);
  bool indActive = (digitalRead(indPin) == HIGH);
  bool capActive = (digitalRead(capPin) == LOW);

  if (!irActive) return NONE;
  if (indActive) return METAL;
  if (capActive) return ORGANIC;
  return PLASTIC;
}

TrashType readSensors(bool organicBin) {
  int orgCount = 0;
  int plasticCount = 0;
  int metalCount = 0;
  int noneCount = 0;
  
  for (int i = 0; i < DEBOUNCE_SAMPLES; i++) {
    TrashType t = readSensorsSingle(organicBin);
    
    switch(t) {
      case ORGANIC: orgCount++; break;
      case PLASTIC: plasticCount++; break;
      case METAL: metalCount++; break;
      case NONE: noneCount++; break;
    }
    
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_DELAY));
  }
  
  int maxCount = max(max(orgCount, plasticCount), max(metalCount, noneCount));
  
  if (noneCount == maxCount) return NONE;
  if (orgCount == maxCount) return ORGANIC;
  if (metalCount == maxCount) return METAL;
  if (plasticCount == maxCount) return PLASTIC;
  
  return NONE;
}

bool isCorrectBin(TrashType type, bool organicBin) {
  if (type == ORGANIC && organicBin) return true;
  if ((type == PLASTIC || type == METAL) && !organicBin) return true;
  return false;
}

// =======================================================
// RFID FUNCTIONS
// =======================================================
String readUID() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// =======================================================
// CONFIG MODE FUNCTIONS
// =======================================================
void enterConfigMode() {
  Serial.println("\n[CONFIG] Entering config mode...");
  
  configMode = true;
  
  // Stop all tasks except lamp
  if (taskUltrasonicHandle) vTaskSuspend(taskUltrasonicHandle);
  if (taskSoundHandle) vTaskSuspend(taskSoundHandle);
  if (taskStateHandle) vTaskSuspend(taskStateHandle);
  if (taskRFIDHandle) vTaskSuspend(taskRFIDHandle);
  
  WiFi.disconnect(true);
  delay(100);
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  
  IPAddress IP = WiFi.softAPIP();
  
  Serial.println("╔═══════════════════════════╗");
  Serial.println("║   CONFIG MODE ACTIVE      ║");
  Serial.println("╚═══════════════════════════╝");
  Serial.println("AP SSID: " + String(AP_SSID));
  Serial.println("AP Pass: " + String(AP_PASS));
  Serial.println("IP: " + IP.toString());
  
  showConfigMode();
  
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://" + String(MDNS_HOSTNAME) + ".local");
  }
  
  dnsServer.start(53, "*", IP);
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  
  server.begin();
  Serial.println("✓ Config server started");
  
  delay(500);
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  
  Serial.println("🔍 Starting WiFi scan...");
  WiFi.scanNetworks(true);
}

void handleRoot() {
  String html = R"(<!DOCTYPE html>
<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>EcoFunBin Setup</title><style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,sans-serif;background:linear-gradient(135deg,#667eea,#764ba2);min-height:100vh;padding:20px;display:flex;align-items:center;justify-content:center}
.container{max-width:500px;width:100%;background:#fff;border-radius:20px;padding:40px;box-shadow:0 20px 60px rgba(0,0,0,0.3)}
h1{color:#667eea;text-align:center;margin-bottom:10px;font-size:28px}
.version{text-align:center;color:#888;font-size:13px;margin-bottom:30px}
.section{margin-bottom:20px}
.label{font-weight:600;color:#667eea;margin-bottom:8px;display:block;font-size:14px}
select,input{width:100%;padding:12px;margin-bottom:12px;border:2px solid #e0e0e0;border-radius:10px;font-size:16px}
select:focus,input:focus{outline:none;border-color:#667eea}
.hint{font-size:12px;color:#888;margin-top:-8px;margin-bottom:12px}
button{width:100%;padding:16px;background:#667eea;color:#fff;border:none;border-radius:10px;font-size:18px;font-weight:600;cursor:pointer;margin-top:10px}
button:hover{background:#5568d3;transform:translateY(-2px)}
.btn-secondary{background:#6c757d;margin-top:5px}
.btn-secondary:hover{background:#5a6268}
@keyframes spin{to{transform:rotate(360deg)}}
.spinner{display:inline-block;width:16px;height:16px;border:3px solid #ddd;border-top-color:#667eea;border-radius:50%;animation:spin 1s linear infinite;margin-right:8px}
.scan-status{text-align:center;padding:10px;background:#f0f4ff;border-radius:8px;margin-bottom:12px;color:#667eea;font-size:14px}
</style></head><body><div class='container'><h1>♻️ EcoFunBin</h1>
<div class='version'>v)" + String(FIRMWARE_VERSION) + R"(</div>
<form action='/save' method='POST' onsubmit='return validateForm()'>
<div class='section'><div class='label'>📶 WiFi Network</div>
<div id='scanStatus' class='scan-status' style='display:none'>Scanning...</div>
<select id='ssid' name='ssid' required><option value=''>Select WiFi</option></select>
<button type='button' class='btn-secondary' onclick='rescanNetworks()'>🔄 Rescan</button>
<input type='text' id='manualSSID' placeholder='Or enter SSID manually' style='margin-top:12px'>
<input type='password' name='password' placeholder='WiFi Password' required minlength='8'>
<div class='hint'>Min 8 characters</div></div>
<button type='submit' id='submitBtn'><span id='btnText'>💾 Save & Connect</span></button>
</form></div><script>
let scanAttempts = 0;
const maxScanAttempts = 10;

function loadNetworks(){
  scanAttempts++;
  const scanStatus = document.getElementById('scanStatus');
  scanStatus.style.display = 'block';
  scanStatus.textContent = 'Scanning... (' + scanAttempts + ')';
  
  fetch('/scan').then(r=>r.json()).then(data=>{
    const select=document.getElementById('ssid');
    if(data.status==='scanning'){
      if(scanAttempts < maxScanAttempts) {
        setTimeout(loadNetworks,2000);
      } else {
        scanStatus.textContent = 'Scan timeout. Use manual input.';
        scanStatus.style.background = '#ffe0e0';
        scanStatus.style.color = '#cc0000';
      }
      return;
    }
    if(data.networks&&data.networks.length>0){
      scanStatus.style.display = 'none';
      select.innerHTML='<option value="">Select WiFi</option>';
      data.networks.forEach(n=>{
        const opt=document.createElement('option');
        opt.value=n.ssid;
        opt.textContent=n.ssid + ' (' + n.rssi + ' dBm)';
        select.appendChild(opt);
      });
      scanAttempts = 0;
    } else {
      if(scanAttempts < maxScanAttempts) {
        setTimeout(loadNetworks,3000);
      } else {
        scanStatus.textContent = 'No networks. Use manual input.';
        scanStatus.style.background = '#ffe0e0';
        scanStatus.style.color = '#cc0000';
      }
    }
  }).catch(e=>{
    console.error(e);
    if(scanAttempts < maxScanAttempts) {
      setTimeout(loadNetworks,3000);
    }
  });
}

function rescanNetworks() {
  scanAttempts = 0;
  document.getElementById('ssid').innerHTML = '<option value="">Scanning...</option>';
  loadNetworks();
}

function validateForm(){
  let ssid = document.querySelector('[name="ssid"]').value;
  const manualSSID = document.getElementById('manualSSID').value;
  
  if(manualSSID) {
    ssid = manualSSID;
    document.querySelector('[name="ssid"]').value = ssid;
  }
  
  if(!ssid){alert('Enter WiFi SSID');return false;}
  
  const btn=document.getElementById('submitBtn');
  btn.innerHTML='<span class="spinner"></span> Saving...';
  btn.disabled=true;
  return true;
}

setTimeout(loadNetworks, 1000);
</script></body></html>)";
  
  server.send(200, "text/html", html);
}

void handleScan() {
  int n = WiFi.scanComplete();
  
  if (n == WIFI_SCAN_RUNNING) {
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  
  if (n == WIFI_SCAN_FAILED || n == -2) {
    WiFi.scanDelete();
    delay(100);
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  
  if (n == 0) {
    WiFi.scanDelete();
    delay(100);
    WiFi.scanNetworks(true);
    server.send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  
  StaticJsonDocument<2048> doc;
  JsonArray networks = doc.createNestedArray("networks");
  
  for (int i = 0; i < n && i < 20; i++) {
    JsonObject network = networks.createNestedObject();
    network["ssid"] = WiFi.SSID(i);
    network["rssi"] = WiFi.RSSI(i);
  }
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
  
  WiFi.scanDelete();
}

void handleSave() {
  wifiSSID = server.arg("ssid");
  wifiPassword = server.arg("password");
  
  saveConfig();
  
  Serial.println("\n✓ Config saved:");
  Serial.println("  WiFi: " + wifiSSID);
  
  String html = R"(<!DOCTYPE html><html><head><meta charset='UTF-8'>
<meta http-equiv='refresh' content='5;url=/'><style>
body{font-family:sans-serif;text-align:center;padding:50px;background:#667eea;color:#fff}
.box{background:#fff;color:#333;padding:40px;border-radius:20px;max-width:400px;margin:0 auto}
.icon{font-size:64px;margin-bottom:20px}
h1{color:#667eea}
</style></head><body><div class='box'>
<div class='icon'>✓</div><h1>Config Saved!</h1>
<p>Rebooting in 5 seconds...</p>
</div></body></html>)";
  
  server.send(200, "text/html", html);
  
  delay(5000);
  ESP.restart();
}

// =======================================================
// FREERTOS TASK: CONFIG BUTTON MONITOR
// =======================================================
void taskConfigButton(void *parameter) {
  Serial.println("[CONFIG] Button monitor started");
  
  unsigned long pressStart = 0;
  bool buttonPressed = false;
  
  while (true) {
    bool buttonState = (digitalRead(CONFIG_BUTTON) == LOW);
    
    if (buttonState && !buttonPressed) {
      // Button just pressed
      pressStart = millis();
      buttonPressed = true;
      Serial.println("[CONFIG] Button pressed");
      
    } else if (buttonState && buttonPressed) {
      // Button still held
      unsigned long pressDuration = millis() - pressStart;
      
      if (pressDuration >= BUTTON_HOLD_TIME && !configMode) {
        Serial.println("[CONFIG] Button held for 3s - entering config mode");
        enterConfigMode();
        buttonPressed = false;
      }
      
    } else if (!buttonState && buttonPressed) {
      // Button released
      buttonPressed = false;
      Serial.println("[CONFIG] Button released");
    }
    
    // Handle config mode web server
    if (configMode) {
      dnsServer.processNextRequest();
      server.handleClient();
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =======================================================
// FREERTOS TASK: ULTRASONIC MONITORING
// =======================================================
void taskUltrasonic(void *parameter) {
  Serial.println("[ULTRASONIC] Task started");
  
  for (int i = 0; i < SMOOTHING_SAMPLES; i++) {
    distOrgBuffer[i] = readUltrasonic(TRIG_ORG, ECHO_ORG);
    distAnoBuffer[i] = readUltrasonic(TRIG_ANO, ECHO_ANO);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  bufferFilled = true;
  
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  while (true) {
    long distOrg = readUltrasonic(TRIG_ORG, ECHO_ORG);
    long distAno = readUltrasonic(TRIG_ANO, ECHO_ANO);
    
    distOrgBuffer[bufferIndex] = distOrg;
    distAnoBuffer[bufferIndex] = distAno;
    
    bufferIndex++;
    if (bufferIndex >= SMOOTHING_SAMPLES) {
      bufferIndex = 0;
    }
    
    if (bufferFilled) {
      distOrg = getSmoothedDistance(distOrg, distOrgBuffer, SMOOTHING_SAMPLES);
      distAno = getSmoothedDistance(distAno, distAnoBuffer, SMOOTHING_SAMPLES);
    }
    
    levelOrg = calculateLevel(distOrg, DISTANCE_EMPTY_ORG, DISTANCE_FULL_ORG);
    levelAno = calculateLevel(distAno, DISTANCE_EMPTY_ANO, DISTANCE_FULL_ANO);
    
    binFull = (levelOrg >= FULL_THRESHOLD || levelAno >= FULL_THRESHOLD);
    
    if (!configMode) {
      LCDUpdateRequest req = {true};
      xQueueSend(queueLCDUpdate, &req, 0);
    }
    
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(ULTRASONIC_INTERVAL));
  }
}

// =======================================================
// FREERTOS TASK: LCD DISPLAY
// =======================================================
void taskLCD(void *parameter) {
  Serial.println("[LCD] Task started");
  
  LCDUpdateRequest req;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  while (true) {
    if (xQueueReceive(queueLCDUpdate, &req, 0) == pdTRUE) {
      if (req.updateLevels && !configMode) {
        if (currentState == STANDBY || currentState == READY || currentState == LOGIN_DISPLAY) {
          if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(50)) == pdTRUE) {
            showBinLevels();
            xSemaphoreGive(mutexLCD);
          }
        }
      }
    }
    
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
  }
}

// =======================================================
// FREERTOS TASK: SOUND MANAGEMENT
// =======================================================
void taskSound(void *parameter) {
  Serial.println("[SOUND] Task started");
  
  SoundCommand cmd;
  
  while (true) {
    if (myDFPlayer.available()) {
      uint8_t type = myDFPlayer.readType();
      int value = myDFPlayer.read();
      
      if (type == DFPlayerPlayFinished) {
        if (xSemaphoreTake(mutexSound, pdMS_TO_TICKS(10)) == pdTRUE) {
          soundPlaying = false;
          xSemaphoreGive(mutexSound);
        }
      } else if (type == DFPlayerError) {
        if (xSemaphoreTake(mutexSound, pdMS_TO_TICKS(10)) == pdTRUE) {
          soundPlaying = false;
          xSemaphoreGive(mutexSound);
        }
      }
    }
    
    if (soundPlaying && millis() - lastSoundStartTime > SOUND_TIMEOUT) {
      if (xSemaphoreTake(mutexSound, pdMS_TO_TICKS(10)) == pdTRUE) {
        soundPlaying = false;
        xSemaphoreGive(mutexSound);
      }
    }
    
    if (xQueueReceive(queueSound, &cmd, 0) == pdTRUE) {
      if (xSemaphoreTake(mutexSound, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!soundPlaying) {
          myDFPlayer.play(cmd.soundNumber);
          soundPlaying = true;
          currentSound = cmd.soundNumber;
          lastSoundStartTime = millis();
        }
        xSemaphoreGive(mutexSound);
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =======================================================
// FREERTOS TASK: LAMP CONTROL
// =======================================================
void taskLamp(void *parameter) {
  Serial.println("[LAMP] Task started");
  
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  while (true) {
    if (binFull) {
      blinkState = !blinkState;
      
      if (blinkState) {
        digitalWrite(RELAY_RED, LOW);
      } else {
        digitalWrite(RELAY_RED, HIGH);
      }
    } else {
      if (blinkState) {
        blinkState = false;
        digitalWrite(RELAY_RED, HIGH);
      }
    }
    
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(BLINK_INTERVAL));
  }
}

// =======================================================
// FREERTOS TASK: RFID READER
// =======================================================
void taskRFID(void *parameter) {
  Serial.println("[RFID] Task started");
  
  vTaskDelay(pdMS_TO_TICKS(2000));
  
  while (true) {
    if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(50)) == pdTRUE) {
      State state = currentState;
      xSemaphoreGive(mutexState);
      
      if (state == STANDBY) {
        if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
          
          String uid = readUID();
          Serial.println("\n[RFID] Card: " + uid);
          
          for (int i = 0; i <= 100; i += 20) {
            showLoadingScreen("Checking Card", i);
            vTaskDelay(pdMS_TO_TICKS(50));
          }
          
          if (xSemaphoreTake(mutexUserData, pdMS_TO_TICKS(100)) == pdTRUE) {
            uidCard = uid;
            xSemaphoreGive(mutexUserData);
          }
          
          bool userFound = false;
          String userName = "";
          int userPoints = 0;
          
          if (xSemaphoreTake(mutexFirebase, pdMS_TO_TICKS(5000)) == pdTRUE) {
            
            String path = "/user/" + uid + "/nama";
            
            yield();
            
            if (Firebase.RTDB.getString(&fbdo, path)) {
              if (fbdo.dataType() == "string") {
                userName = fbdo.stringData();
                
                yield();
                
                String pointPath = "/user/" + uid + "/poin";
                if (Firebase.RTDB.getInt(&fbdo, pointPath)) {
                  userPoints = fbdo.intData();
                  userFound = true;
                }
              }
            }
            
            xSemaphoreGive(mutexFirebase);
          }
          
          if (userFound) {
            Serial.println("[RFID] User: " + userName);
            
            if (xSemaphoreTake(mutexUserData, pdMS_TO_TICKS(100)) == pdTRUE) {
              studentName = userName;
              studentPoint = userPoints;
              loggedIn = true;
              xSemaphoreGive(mutexUserData);
            }
            
            showNameAndPoints(userName, userPoints);
            
            SoundCommand sndCmd;
            sndCmd.soundNumber = SND_WELCOME;
            xQueueSend(queueSound, &sndCmd, 0);
            
            if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
              currentState = LOGIN_DISPLAY;
              stateTimer = millis();
              xSemaphoreGive(mutexState);
            }
            
          } else {
            Serial.println("[RFID] Card not found");
            
            showErrorCard();
            
            SoundCommand sndCmd;
            sndCmd.soundNumber = SND_CARD_ERROR;
            xQueueSend(queueSound, &sndCmd, 0);
            
            if (!binFull) {
              digitalWrite(RELAY_GREEN, HIGH);
              digitalWrite(RELAY_RED, LOW);
            }
            
            vTaskDelay(pdMS_TO_TICKS(3000));
            
            digitalWrite(RELAY_GREEN, HIGH);
            digitalWrite(RELAY_RED, HIGH);
            
            showStandby();
          }
          
          mfrc522.PICC_HaltA();
          mfrc522.PCD_StopCrypto1();
        }
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// =======================================================
// FREERTOS TASK: STATE MACHINE
// =======================================================
void taskStateMachine(void *parameter) {
  Serial.println("[STATE] Task started");
  
  while (true) {
    if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(50)) == pdTRUE) {
      State state = currentState;
      xSemaphoreGive(mutexState);
      
      unsigned long currentMillis = millis();
      
      switch (state) {
        case LOGIN_DISPLAY:
          if (currentMillis - stateTimer >= 3000) {
            showReady();
            
            if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
              currentState = READY;
              Serial.println("[STATE] Ready");
              xSemaphoreGive(mutexState);
            }
          }
          break;
          
        case READY:
          {
            bool orgTriggered = (digitalRead(IR_ORG) == LOW);
            bool anoTriggered = (digitalRead(IR_ANO) == LOW);
            
            if (orgTriggered || anoTriggered) {
              busyDetect = true;
              isOrganic = orgTriggered;
              
              Serial.println("\n[STATE] Trash detected");
              
              if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
                lcd.clear();
                showHeader();
                showBinLevels();
                centerText(2, "SEDANG DETEKSI");
                centerText(3, isOrganic ? "SAMPAH ORGANIK" : "SAMPAH ANORGANIK");
                xSemaphoreGive(mutexLCD);
              }
              
              vTaskDelay(pdMS_TO_TICKS(2000));
              
              SoundCommand sndCmd;
              sndCmd.soundNumber = isOrganic ? SND_ORGANIC : SND_ANORGANIC;
              xQueueSend(queueSound, &sndCmd, 0);
              
              if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
                currentState = DETECTING;
                stateTimer = currentMillis;
                xSemaphoreGive(mutexState);
              }
            }
          }
          break;
          
        case DETECTING:
          if (currentMillis - stateTimer >= SENSOR_READ_TIME) {
            Serial.println("[STATE] Reading sensors...");
            detectedType = readSensors(isOrganic);
            
            if (detectedType == NONE) {
              Serial.println("[STATE] No trash");
              busyDetect = false;
              showReady();
              
              if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
                currentState = READY;
                xSemaphoreGive(mutexState);
              }
              break;
            }
            
            bool correct = isCorrectBin(detectedType, isOrganic);
            
            SoundCommand sndCmd;
            
            if (correct) {
              Serial.println("[STATE] CORRECT!");
              
              if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
                lcd.clear();
                showHeader();
                showBinLevels();
                centerText(2, "BENAR!");
                centerText(3, "Poin +15");
                xSemaphoreGive(mutexLCD);
              }
              
              if (xSemaphoreTake(mutexUserData, pdMS_TO_TICKS(100)) == pdTRUE) {
                studentPoint += 15;
                xSemaphoreGive(mutexUserData);
              }
              
              if (!binFull) {
                digitalWrite(RELAY_GREEN, LOW);
                digitalWrite(RELAY_RED, HIGH);
              }
              
              sndCmd.soundNumber = SND_CORRECT;
              xQueueSend(queueSound, &sndCmd, 0);
              
              if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
                currentState = CORRECT_RESULT;
                stateTimer = currentMillis;
                xSemaphoreGive(mutexState);
              }
              
            } else {
              Serial.println("[STATE] WRONG!");
              
              if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
                lcd.clear();
                showHeader();
                showBinLevels();
                centerText(2, "SALAH!");
                centerText(3, "Poin -5");
                xSemaphoreGive(mutexLCD);
              }
              
              if (xSemaphoreTake(mutexUserData, pdMS_TO_TICKS(100)) == pdTRUE) {
                studentPoint -= 5;
                xSemaphoreGive(mutexUserData);
              }
              
              if (!binFull) {
                digitalWrite(RELAY_GREEN, HIGH);
                digitalWrite(RELAY_RED, LOW);
              }
              
              sndCmd.soundNumber = SND_WRONG;
              xQueueSend(queueSound, &sndCmd, 0);
              
              if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
                currentState = WRONG_RESULT;
                stateTimer = currentMillis;
                xSemaphoreGive(mutexState);
              }
            }
          }
          break;
          
        case CORRECT_RESULT:
          if (currentMillis - stateTimer >= LAMP_ON_TIME) {
            digitalWrite(RELAY_GREEN, HIGH);
            digitalWrite(RELAY_RED, HIGH);
            
            if (xSemaphoreTake(mutexUserData, pdMS_TO_TICKS(100)) == pdTRUE) {
              showNameAndPoints(studentName, studentPoint);
              xSemaphoreGive(mutexUserData);
            }
            
            Serial.println("[STATE] Opening servo...");
            
            if (xSemaphoreTake(mutexServo, pdMS_TO_TICKS(100)) == pdTRUE) {
              if (isOrganic) servoOrg.write(SERVO_OPEN);
              else servoAno.write(SERVO_OPEN);
              xSemaphoreGive(mutexServo);
            }
            
            if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
              currentState = SERVO_WAIT;
              stateTimer = currentMillis;
              xSemaphoreGive(mutexState);
            }
          }
          break;
          
        case WRONG_RESULT:
          if (currentMillis - stateTimer >= LAMP_ON_TIME) {
            digitalWrite(RELAY_GREEN, HIGH);
            digitalWrite(RELAY_RED, HIGH);
            
            bool waitOrganic = (detectedType == ORGANIC);
            
            if (xSemaphoreTake(mutexLCD, pdMS_TO_TICKS(100)) == pdTRUE) {
              lcd.clear();
              showHeader();
              showBinLevels();
              centerText(2, "PINDAH KE TONG");
              centerText(3, waitOrganic ? "ORGANIK" : "ANORGANIK");
              xSemaphoreGive(mutexLCD);
            }
            
            vTaskDelay(pdMS_TO_TICKS(2000));
            
            SoundCommand sndCmd;
            sndCmd.soundNumber = SND_MOVE_TRASH;
            xQueueSend(queueSound, &sndCmd, 0);
            
            if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
              currentState = WAITING_CORRECT_BIN;
              xSemaphoreGive(mutexState);
            }
          }
          break;
          
        case WAITING_CORRECT_BIN:
          {
            bool waitOrganic = (detectedType == ORGANIC);
            bool correctBinTriggered = false;
            
            if (waitOrganic && digitalRead(IR_ORG) == LOW) correctBinTriggered = true;
            if (!waitOrganic && digitalRead(IR_ANO) == LOW) correctBinTriggered = true;
            
            if (correctBinTriggered) {
              Serial.println("[STATE] Moved to correct bin");
              
              if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
                currentState = WAITING_SENSOR;
                stateTimer = currentMillis;
                xSemaphoreGive(mutexState);
              }
            }
          }
          break;
          
        case WAITING_SENSOR:
          if (currentMillis - stateTimer >= IR_WAIT_TIME) {
            
            if (xSemaphoreTake(mutexUserData, pdMS_TO_TICKS(100)) == pdTRUE) {
              showNameAndPoints(studentName, studentPoint);
              xSemaphoreGive(mutexUserData);
            }
            
            Serial.println("[STATE] Opening servo...");
            
            bool waitOrganic = (detectedType == ORGANIC);
            
            if (xSemaphoreTake(mutexServo, pdMS_TO_TICKS(100)) == pdTRUE) {
              if (waitOrganic) servoOrg.write(SERVO_OPEN);
              else servoAno.write(SERVO_OPEN);
              xSemaphoreGive(mutexServo);
            }
            
            if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
              currentState = SERVO_WAIT;
              stateTimer = currentMillis;
              xSemaphoreGive(mutexState);
            }
          }
          break;
          
        case SERVO_WAIT:
          if (currentMillis - stateTimer >= SERVO_OPEN_TIME) {
            Serial.println("[STATE] Closing servos...");
            
            if (xSemaphoreTake(mutexServo, pdMS_TO_TICKS(100)) == pdTRUE) {
              servoOrg.write(SERVO_CLOSE);
              servoAno.write(SERVO_CLOSE);
              xSemaphoreGive(mutexServo);
            }
            
            if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
              currentState = UPDATING_DB;
              xSemaphoreGive(mutexState);
            }
          }
          break;
          
        case UPDATING_DB:
          {
            Serial.println("[STATE] Updating DB...");
            
            String uid;
            int points;
            
            if (xSemaphoreTake(mutexUserData, pdMS_TO_TICKS(100)) == pdTRUE) {
              uid = uidCard;
              points = studentPoint;
              xSemaphoreGive(mutexUserData);
            }
            
            for (int i = 0; i <= 100; i += 25) {
              showLoadingScreen("Saving Data", i);
              vTaskDelay(pdMS_TO_TICKS(100));
            }
            
            if (xSemaphoreTake(mutexFirebase, pdMS_TO_TICKS(5000)) == pdTRUE) {
              
              String path = "/user/" + uid + "/poin";
              
              yield();
              
              if (Firebase.RTDB.setInt(&fbdo, path, points)) {
                Serial.println("[STATE] Updated");
                showLoadingScreen("Saved!", 100);
                vTaskDelay(pdMS_TO_TICKS(300));
              } else {
                Serial.println("[STATE] Failed: " + fbdo.errorReason());
                showLoadingScreen("Error!", 100);
                vTaskDelay(pdMS_TO_TICKS(500));
              }
              
              xSemaphoreGive(mutexFirebase);
            }
            
            vTaskDelay(pdMS_TO_TICKS(300));
            
            if (xSemaphoreTake(mutexUserData, pdMS_TO_TICKS(100)) == pdTRUE) {
              uidCard = "";
              studentName = "";
              studentPoint = 0;
              loggedIn = false;
              busyDetect = false;
              xSemaphoreGive(mutexUserData);
            }
            
            digitalWrite(RELAY_GREEN, HIGH);
            digitalWrite(RELAY_RED, HIGH);
            
            SoundCommand sndCmd;
            sndCmd.soundNumber = SND_THANKYOU;
            xQueueSend(queueSound, &sndCmd, 0);
            
            vTaskDelay(pdMS_TO_TICKS(2000));
            
            showStandby();
            
            if (xSemaphoreTake(mutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
              currentState = STANDBY;
              Serial.println("[STATE] Logout\n");
              xSemaphoreGive(mutexState);
            }
          }
          break;
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =======================================================
// CONFIG STORAGE
// =======================================================
void loadConfig() {
  prefs.begin("config", true);
  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPassword = prefs.getString("wifi_pass", "");
  prefs.end();
}

void saveConfig() {
  prefs.begin("config", false);
  prefs.putString("wifi_ssid", wifiSSID);
  prefs.putString("wifi_pass", wifiPassword);
  prefs.end();
}

// =======================================================
// WIFI & FIREBASE
// =======================================================
void connectWiFi() {
  if (wifiSSID.length() == 0) return;
  
  Serial.print("Connecting to WiFi: " + wifiSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" ✓");
    Serial.println("IP: " + WiFi.localIP().toString());
    Serial.println("RSSI: " + String(WiFi.RSSI()) + " dBm");
  } else {
    Serial.println(" ✗");
  }
}

void firebaseInit() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = NULL;
  
  Firebase.begin(&config, &auth);
  Firebase.signUp(&config, &auth, "", "");
  Firebase.reconnectWiFi(true);
  
  fbdo.setBSSLBufferSize(4096, 1024);
  fbdo.setResponseSize(2048);
}

// =======================================================
// SETUP
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔═══════════════════════════╗");
  Serial.println("║ EcoFunBin v" + String(FIRMWARE_VERSION) + "        ║");
  Serial.println("║  Smart Trash System       ║");
  Serial.println("╚═══════════════════════════╝");

  pinMode(IR_ORG, INPUT_PULLUP);
  pinMode(IR_ANO, INPUT_PULLUP);
  pinMode(IND_ORG, INPUT_PULLDOWN);
  pinMode(IND_ANO, INPUT_PULLDOWN);
  pinMode(CAP_ORG, INPUT_PULLUP);
  pinMode(CAP_ANO, INPUT_PULLUP);

  pinMode(TRIG_ORG, OUTPUT);
  pinMode(ECHO_ORG, INPUT);
  pinMode(TRIG_ANO, OUTPUT);
  pinMode(ECHO_ANO, INPUT);

  pinMode(RELAY_GREEN, OUTPUT);
  pinMode(RELAY_RED, OUTPUT);
  pinMode(CONFIG_BUTTON, INPUT_PULLUP);
  
  digitalWrite(RELAY_GREEN, HIGH);
  digitalWrite(RELAY_RED, HIGH);

  Serial.println("Initializing servos...");
  servoOrg.attach(SERVO_ORG_PIN);
  servoAno.attach(SERVO_ANO_PIN);
  servoOrg.write(SERVO_CLOSE);
  servoAno.write(SERVO_CLOSE);
  delay(500);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  
  lcd.setCursor(0, 0);
  lcd.print("   --ECOFUN BIN--   ");
  lcd.setCursor(0, 1);
  lcd.print("                    ");
  lcd.setCursor(0, 2);
  lcd.print("   Initializing     ");
  lcd.setCursor(0, 3);
  lcd.print("                    ");
  
  delay(500);
  
  for (int i = 0; i <= 100; i += 5) {
    lcd.setCursor(0, 3);
    int blocks = map(i, 0, 100, 0, 20);
    for (int j = 0; j < 20; j++) {
      if (j < blocks) {
        lcd.write(0xFF);
      } else {
        lcd.write('-');
      }
    }
    delay(30);
  }
  
  delay(500);
  
  mfrc522.PCD_Init();
  Serial.println("✓ RFID initialized");
  
  lcd.setCursor(0, 2);
  lcd.print("    RFID Ready      ");
  delay(500);

  dfSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  Serial.println(F("Initializing DFPlayer..."));
  
  lcd.setCursor(0, 2);
  lcd.print("    Audio Init      ");
  
  delay(1000);
  
  if (!myDFPlayer.begin(dfSerial)) {
    Serial.println(F("⚠️  DFPlayer init failed"));
    lcd.setCursor(0, 2);
    lcd.print("   Audio Failed     ");
  } else {
    Serial.println(F("✓ DFPlayer online"));
    myDFPlayer.volume(30);
    myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
    delay(100);
    
    lcd.setCursor(0, 2);
    lcd.print("    Audio Ready     ");
  }
  
  delay(500);

  loadConfig();
  
  if (wifiSSID.length() > 0) {
    lcd.setCursor(0, 2);
    lcd.print("   WiFi Connect     ");
    
    for (int i = 0; i <= 50; i += 10) {
      lcd.setCursor(0, 3);
      int blocks = map(i, 0, 100, 0, 20);
      for (int j = 0; j < 20; j++) {
        if (j < blocks) lcd.write(0xFF);
        else lcd.write('-');
      }
      delay(100);
    }
    
    connectWiFi();
    
    if (WiFi.status() == WL_CONNECTED) {
      lcd.setCursor(0, 2);
      lcd.print("  WiFi Connected    ");
      
      for (int i = 50; i <= 100; i += 10) {
        lcd.setCursor(0, 3);
        int blocks = map(i, 0, 100, 0, 20);
        for (int j = 0; j < 20; j++) {
          if (j < blocks) lcd.write(0xFF);
          else lcd.write('-');
        }
        delay(50);
      }
      
      delay(300);
      
      if (MDNS.begin(MDNS_HOSTNAME)) {
        Serial.println("✓ mDNS started");
      }
      
      lcd.setCursor(0, 2);
      lcd.print("  Firebase Init     ");
      
      firebaseInit();
      Serial.println("✓ Firebase initialized");
      
      for (int i = 0; i <= 100; i += 20) {
        lcd.setCursor(0, 3);
        int blocks = map(i, 0, 100, 0, 20);
        for (int j = 0; j < 20; j++) {
          if (j < blocks) lcd.write(0xFF);
          else lcd.write('-');
        }
        delay(50);
      }
      
      delay(300);
    } else {
      lcd.setCursor(0, 2);
      lcd.print("   WiFi Failed      ");
      delay(1000);
    }
  }

  delay(500);
  
  lcd.setCursor(0, 2);
  lcd.print("  Starting Tasks    ");
  
  for (int i = 0; i <= 100; i += 10) {
    lcd.setCursor(0, 3);
    int blocks = map(i, 0, 100, 0, 20);
    for (int j = 0; j < 20; j++) {
      if (j < blocks) lcd.write(0xFF);
      else lcd.write('-');
    }
    delay(30);
  }
  
  delay(500);

  Serial.println("\n[RTOS] Creating sync objects...");
  
  mutexLCD = xSemaphoreCreateMutex();
  mutexServo = xSemaphoreCreateMutex();
  mutexSound = xSemaphoreCreateMutex();
  mutexState = xSemaphoreCreateMutex();
  mutexFirebase = xSemaphoreCreateMutex();
  mutexUserData = xSemaphoreCreateMutex();
  
  queueSound = xQueueCreate(5, sizeof(SoundCommand));
  queueLCDUpdate = xQueueCreate(10, sizeof(LCDUpdateRequest));
  
  Serial.println("[RTOS] Creating tasks...");
  
  xTaskCreatePinnedToCore(taskConfigButton, "ConfigButton", STACK_CONFIG, NULL, PRIORITY_CONFIG, &taskConfigButtonHandle, 0);
  xTaskCreatePinnedToCore(taskUltrasonic, "Ultrasonic", STACK_ULTRASONIC, NULL, PRIORITY_ULTRASONIC, &taskUltrasonicHandle, 0);
  xTaskCreatePinnedToCore(taskLCD, "LCD", STACK_LCD, NULL, PRIORITY_LCD, &taskLCDHandle, 0);
  xTaskCreatePinnedToCore(taskSound, "Sound", STACK_SOUND, NULL, PRIORITY_SOUND, &taskSoundHandle, 0);
  xTaskCreatePinnedToCore(taskLamp, "Lamp", STACK_LAMP, NULL, PRIORITY_LAMP, &taskLampHandle, 0);
  
  xTaskCreatePinnedToCore(taskRFID, "RFID", STACK_RFID, NULL, PRIORITY_RFID, &taskRFIDHandle, 1);
  xTaskCreatePinnedToCore(taskStateMachine, "StateMachine", STACK_STATE, NULL, PRIORITY_STATE, &taskStateHandle, 1);
  
  delay(1000);
  showStandby();
  
  Serial.println("\n✓ System ready!");
  Serial.println("✓ FreeRTOS tasks running");
  Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
  Serial.println("✓ Config button monitoring active");
  Serial.println("  Hold BOOT button 3s to enter config mode");
  Serial.println("=================================\n");
}

// =======================================================
// LOOP
// =======================================================
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}