// ============================================================
//  Septic Tank Gas Monitoring Probe (History Log Edition)
//  Feature  : Live Warmup, Session History, Soft Reset
//  Hardware : ESP32 DevKit V1
//  Sensors  : MQ-2 (Flammable), MQ-4 (Methane), MQ-7 (CO), MQ-135 (NH3)
//             DHT22 (Temp/Hum), HC-SR04 (Ultrasonic)
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <math.h>

// ── Wi-Fi Credentials ───────────────────────────────────────
const char* ssid     = "GasProbe_AP";      // Connect to this Wi-Fi network
const char* password = "password123";      // Password for the network

WebServer server(80);

// ── States & Hazards ────────────────────────────────────────
enum SystemState { STATE_WARMUP, STATE_LOWERING, STATE_MEASURING, STATE_DONE };
enum HazardLevel { SAFE, WARNING, DANGER };

SystemState currentState = STATE_WARMUP;

// ── History Data Structure ──────────────────────────────────
struct SensorData {
  float flam;
  float ch4;
  float co;
  float nh3;
  float temp;
  float humi;
  HazardLevel hazard;
};

#define MAX_HISTORY 10 // Saves the last 10 readings
SensorData readingHistory[MAX_HISTORY];
int historyCount = 0;

// ── Pin Definitions ─────────────────────────────────────────
#define MQ2_PIN       34  
#define MQ4_PIN       35  
#define MQ7_PIN       32  
#define MQ135_PIN     33  

#define DHT_PIN        4
#define DHT_TYPE   DHT22

#define TRIG_PIN      18
#define ECHO_PIN      19

#define BUZZER_PIN    15

// ── Target Distance & Timing ────────────────────────────────
const float OPTIMAL_DIST_MIN = 15.0f; 
const float OPTIMAL_DIST_MAX = 30.0f; 

const unsigned long MEASURE_DURATION = 10000; // 10 seconds
const unsigned long READ_INTERVAL    = 500;   // Read every 0.5s

unsigned long stateStartTime         = 0;
unsigned long warmupStartTime        = 0;
unsigned long lastReadTime           = 0;

// ── Safety Thresholds (ILO / OSHA) ──────────────────────────
#define FLAM_WARN   1000.0f
#define FLAM_DANGER 5000.0f
#define CH4_WARN    1000.0f
#define CH4_DANGER  5000.0f
#define CO_WARN       25.0f
#define CO_DANGER     50.0f
#define NH3_WARN      20.0f
#define NH3_DANGER    25.0f

// ── Sensor Constants & Calibration ──────────────────────────
const float RL = 10.0f; 
float Ro_MQ2 = 10.0f, Ro_MQ4 = 10.0f, Ro_MQ7 = 10.0f, Ro_MQ135 = 10.0f;

const float MQ2_a   = 574.25f,  MQ2_b   = -2.222f; 
const float MQ4_a   = 1012.7f,  MQ4_b   = -2.786f; 
const float MQ7_a   = 99.042f,  MQ7_b   = -1.518f; 
const float MQ135_a = 102.2f,   MQ135_b = -2.473f; 

// ── Global Objects & Accumulators ───────────────────────────
DHT dht(DHT_PIN, DHT_TYPE);

int readCount = 0;
float sumTemp = 0.0f, sumHumi = 0.0f;
float sumFlam = 0.0f, sumCH4 = 0.0f, sumCO = 0.0f, sumNH3 = 0.0f;
int validTempHumiCount = 0;

// Web Server Display Variables
int warmupTimeRemaining = 60;
float currentDist = 0.0f;
int timeRemaining = 10;
float finalTemp = 0.0f, finalHumi = 0.0f;
float finalFlam = 0.0f, finalCH4 = 0.0f, finalCO = 0.0f, finalNH3 = 0.0f;
HazardLevel finalHazard = SAFE;

// ── Buzzer state ─────────────────────────────────────────────
int           buzzerPattern = 0;
bool          beepState     = false;
unsigned long lastBeep      = 0;
unsigned long beepOn        = 300;
unsigned long beepOff       = 700;

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
float       readUltrasonic();
float       readGasPPM(int pin, float Ro, float a, float b);
float       tempHumidityCorrection(float temp, float humi);
void        calibrateAllSensors();
HazardLevel evaluateGasHazard(float flam, float ch4, float co, float nh3);
void        handleBuzzer();
void        handleRoot();
void        handleReset();
String      getHTML();

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT); 
  pinMode(ECHO_PIN, INPUT);
  
  digitalWrite(BUZZER_PIN, LOW);
  dht.begin();

  Serial.println(F("\n>> Starting Wi-Fi AP..."));
  WiFi.softAP(ssid, password);
  Serial.print(F(">> AP IP address: "));
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/reset", HTTP_POST, handleReset); 
  server.begin();
  Serial.println(F(">> Web Server Started."));

  warmupStartTime = millis();
  currentState = STATE_WARMUP;
}

// ============================================================
//  MAIN LOOP (STATE MACHINE)
// ============================================================
void loop() {
  server.handleClient(); 
  
  unsigned long now = millis();

  // ==========================================
  // STATE 0: WARMING UP (60 Seconds)
  // ==========================================
  if (currentState == STATE_WARMUP) {
    warmupTimeRemaining = 60 - ((now - warmupStartTime) / 1000);
    
    if (warmupTimeRemaining <= 0) {
      Serial.println(F(">> Calibrating..."));
      calibrateAllSensors(); 
      
      digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW); delay(100);
      digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW);

      currentState = STATE_LOWERING;
    }
  }
  
  // ==========================================
  // STATE 1: LOWERING THE PROBE
  // ==========================================
  else if (currentState == STATE_LOWERING) {
    if (now - lastReadTime >= 1000) { 
      lastReadTime = now;
      currentDist = readUltrasonic();
      
      if (currentDist >= OPTIMAL_DIST_MIN && currentDist <= OPTIMAL_DIST_MAX) {
        currentState = STATE_MEASURING;
        stateStartTime = millis();
        readCount = 0;
        timeRemaining = 10;
        
        buzzerPattern = 2; beepOn = 100; beepOff = 100;
      }
    }
  }
  
  // ==========================================
  // STATE 2: MEASURING (10 Seconds)
  // ==========================================
  else if (currentState == STATE_MEASURING) {
    if (now - lastReadTime >= READ_INTERVAL) {
      lastReadTime = now;
      
      float temp = dht.readTemperature();
      float humi = dht.readHumidity();
      
      if (!isnan(temp) && !isnan(humi)) {
        sumTemp += temp; sumHumi += humi; validTempHumiCount++;
      }
      sumFlam += readGasPPM(MQ2_PIN,   Ro_MQ2,   MQ2_a,   MQ2_b);
      sumCH4  += readGasPPM(MQ4_PIN,   Ro_MQ4,   MQ4_a,   MQ4_b);
      sumCO   += readGasPPM(MQ7_PIN,   Ro_MQ7,   MQ7_a,   MQ7_b);
      sumNH3  += readGasPPM(MQ135_PIN, Ro_MQ135, MQ135_a, MQ135_b);
      readCount++;

      if (now - stateStartTime > 2000) { buzzerPattern = 1; beepOn = 50; beepOff = 950; }
      
      timeRemaining = (MEASURE_DURATION - (now - stateStartTime)) / 1000;
    }
    
    // Time is up, calculate final data
    if (now - stateStartTime >= MEASURE_DURATION) {
      finalTemp = (validTempHumiCount > 0) ? (sumTemp / validTempHumiCount) : NAN;
      finalHumi = (validTempHumiCount > 0) ? (sumHumi / validTempHumiCount) : NAN;
      
      float avgFlam = sumFlam / readCount;
      float avgCH4  = sumCH4  / readCount;
      float avgCO   = sumCO   / readCount;
      float avgNH3  = sumNH3  / readCount;
      
      float corr = tempHumidityCorrection(finalTemp, finalHumi);
      finalFlam = avgFlam * corr;
      finalCH4  = avgCH4  * corr;
      finalCO   = avgCO   * corr;
      finalNH3  = avgNH3  * corr;
      
      finalHazard = evaluateGasHazard(finalFlam, finalCH4, finalCO, finalNH3);
      
      // Save reading to history array (Explicit Cast added here)
      if (historyCount < MAX_HISTORY) {
        readingHistory[historyCount] = SensorData{finalFlam, finalCH4, finalCO, finalNH3, finalTemp, finalHumi, finalHazard};
        historyCount++;
      } else {
        for(int i = 1; i < MAX_HISTORY; i++){
          readingHistory[i-1] = readingHistory[i];
        }
        readingHistory[MAX_HISTORY-1] = SensorData{finalFlam, finalCH4, finalCO, finalNH3, finalTemp, finalHumi, finalHazard};
      }

      currentState = STATE_DONE;
      stateStartTime = millis(); 
      buzzerPattern = 3; 
    }
  }
  
  // ==========================================
  // STATE 3: DONE 
  // ==========================================
  else if (currentState == STATE_DONE) {
    if (now - stateStartTime > 3000 && buzzerPattern == 3) {
      buzzerPattern = 0; 
    }
  }

  handleBuzzer();
}

// ============================================================
//  WEB SERVER ROUTES
// ============================================================

void handleRoot() {
  server.send(200, "text/html", getHTML());
}

// Soft Reset: Clears math, keeps history, skips 60s warmup
void handleReset() {
  readCount = 0;
  sumTemp = 0.0f; sumHumi = 0.0f;
  sumFlam = 0.0f; sumCH4 = 0.0f; sumCO = 0.0f; sumNH3 = 0.0f;
  validTempHumiCount = 0;
  buzzerPattern = 0;
  
  currentState = STATE_LOWERING; 
  
  // Redirect browser instantly back to dashboard (Strict 3-argument format added here)
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", ""); 
}

String getHTML() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  
  if (currentState != STATE_DONE) {
    html += "<meta http-equiv='refresh' content='2'>";
  }
  
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; text-align: center; background-color: #f4f4f9; color: #333; margin: 0; padding: 20px;}";
  html += ".card { background: white; padding: 20px; border-radius: 10px; box-shadow: 0px 4px 8px rgba(0,0,0,0.1); max-width: 400px; margin: auto;}";
  html += ".data-row { display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #ddd;}";
  html += ".safe { color: #28a745; font-weight: bold; }";
  html += ".warn { color: #ffc107; font-weight: bold; }";
  html += ".danger { color: #dc3545; font-weight: bold; }";
  html += ".btn { display: inline-block; padding: 15px; margin-top: 20px; background: #007bff; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 1.1em; width: 100%; font-weight: bold;}";
  html += "table { width: 100%; border-collapse: collapse; margin-top: 20px; font-size: 0.85em; }";
  html += "th, td { border: 1px solid #ddd; padding: 8px; text-align: center; }";
  html += "th { background-color: #333; color: white; }";
  html += "</style></head><body>";
  
  html += "<div class='card'>";
  html += "<h2>Septic Gas Probe</h2>";

  if (currentState == STATE_WARMUP) {
    html += "<h3 style='color: #ff9800;'>Status: Warming Up</h3>";
    html += "<p style='color:#666;'>Heating sensors to clear condensation...</p>";
    if (warmupTimeRemaining > 0) {
      html += "<h1 style='font-size:3.5em; margin:20px 0; color: #ff9800;'>" + String(warmupTimeRemaining) + "s</h1>";
    } else {
      html += "<h1 style='font-size:2em; margin:20px 0; color: #ff9800;'>Calibrating...</h1>";
    }
  }
  else if (currentState == STATE_LOWERING) {
    html += "<h3 style='color: #007bff;'>Status: Lowering...</h3>";
    html += "<p>Current Depth: <b style='font-size:1.2em;'>" + String(currentDist > 0 ? String(currentDist, 1) + " cm" : "Out of range") + "</b></p>";
    html += "<p>Target Zone: 15 cm - 30 cm</p>";
    html += "<p style='color:#888; font-size:0.9em;'><i>(Auto-refreshing live)</i></p>";
  } 
  else if (currentState == STATE_MEASURING) {
    html += "<h3 style='color: #ffc107;'>Status: Measuring...</h3>";
    html += "<p>Hold probe steady!</p>";
    html += "<h1 style='font-size:3.5em; margin:20px 0;'>" + String(timeRemaining) + "s</h1>";
  } 
  else if (currentState == STATE_DONE) {
    String statColor = (finalHazard == DANGER) ? "danger" : (finalHazard == WARNING) ? "warn" : "safe";
    String statText = (finalHazard == DANGER) ? "DANGER" : (finalHazard == WARNING) ? "WARNING" : "SAFE";
    
    html += "<h3 class='" + statColor + "'>Status: " + statText + "</h3>";
    
    html += "<div class='data-row'><span>Flammable Gas:</span> <b class='" + String((finalFlam >= FLAM_DANGER) ? "danger" : (finalFlam >= FLAM_WARN) ? "warn" : "safe") + "'>" + String(finalFlam, 1) + " ppm</b></div>";
    html += "<div class='data-row'><span>Methane (CH4):</span> <b class='" + String((finalCH4 >= CH4_DANGER) ? "danger" : (finalCH4 >= CH4_WARN) ? "warn" : "safe") + "'>" + String(finalCH4, 1) + " ppm</b></div>";
    html += "<div class='data-row'><span>Carbon Monoxide:</span> <b class='" + String((finalCO >= CO_DANGER) ? "danger" : (finalCO >= CO_WARN) ? "warn" : "safe") + "'>" + String(finalCO, 1) + " ppm</b></div>";
    html += "<div class='data-row'><span>Ammonia (NH3):</span> <b class='" + String((finalNH3 >= NH3_DANGER) ? "danger" : (finalNH3 >= NH3_WARN) ? "warn" : "safe") + "'>" + String(finalNH3, 1) + " ppm</b></div>";
    html += "<div class='data-row'><span>Temperature:</span> <b>" + String(finalTemp, 1) + " &deg;C</b></div>";
    html += "<div class='data-row'><span>Humidity:</span> <b>" + String(finalHumi, 1) + " %</b></div>";
    
    html += "<form action='/reset' method='POST'>";
    html += "<button type='submit' class='btn' style='background-color: #28a745;'>&#10133; Take New Reading</button>";
    html += "</form>";
  }

  // --- Display History Table ---
  if (historyCount > 0 && currentState != STATE_WARMUP && currentState != STATE_MEASURING) {
    html += "<h3>Session History</h3>";
    html += "<table>";
    html += "<tr><th>#</th><th>Flam</th><th>CH4</th><th>CO</th><th>NH3</th><th>Status</th></tr>";
    
    for(int i = 0; i < historyCount; i++) {
      String histColor = (readingHistory[i].hazard == DANGER) ? "danger" : (readingHistory[i].hazard == WARNING) ? "warn" : "safe";
      String histText = (readingHistory[i].hazard == DANGER) ? "DNG" : (readingHistory[i].hazard == WARNING) ? "WRN" : "OK";
      
      html += "<tr>";
      html += "<td>" + String(i + 1) + "</td>";
      html += "<td>" + String(readingHistory[i].flam, 0) + "</td>";
      html += "<td>" + String(readingHistory[i].ch4, 0) + "</td>";
      html += "<td>" + String(readingHistory[i].co, 0) + "</td>";
      html += "<td>" + String(readingHistory[i].nh3, 0) + "</td>";
      html += "<td class='" + histColor + "'>" + histText + "</td>";
      html += "</tr>";
    }
    html += "</table>";
  }

  html += "</div></body></html>";
  return html;
}

// ============================================================
//  HELPER FUNCTIONS (Hardware & Math)
// ============================================================

float readUltrasonic() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return -1.0f;
  return (duration * 0.0343f) / 2.0f;
}

float readGasPPM(int pin, float Ro, float a, float b) {
  float volt = constrain(analogRead(pin) * (3.3f / 4095.0f), 0.01f, 3.28f);
  float ratio = (((5.0f - volt) / volt) * RL) / Ro;
  return max(0.0f, (float)(a * pow(ratio, b)));
}

float tempHumidityCorrection(float temp, float humi) {
  if (isnan(temp) || isnan(humi)) return 1.0f;
  return (1.0f + 0.006f * (temp - 20.0f)) * (1.0f + 0.003f * (humi - 65.0f));
}

void calibrateAllSensors() {
  long s2 = 0, s4 = 0, s7 = 0, s135 = 0;
  for (int i = 0; i < 50; i++) {
    server.handleClient(); 
    s2   += analogRead(MQ2_PIN); 
    s4   += analogRead(MQ4_PIN); 
    s7   += analogRead(MQ7_PIN); 
    s135 += analogRead(MQ135_PIN);
    delay(100);
  }
  
  auto calcRo = [&](long sum) -> float {
    float volt = constrain((sum / 50.0f) * (3.3f / 4095.0f), 0.01f, 3.28f);
    return (((5.0f - volt) / volt) * RL) / 9.8f;
  };
  
  Ro_MQ2 = calcRo(s2); Ro_MQ4 = calcRo(s4); Ro_MQ7 = calcRo(s7); Ro_MQ135 = calcRo(s135);
}

HazardLevel evaluateGasHazard(float flam, float ch4, float co, float nh3) {
  if (flam >= FLAM_DANGER || ch4 >= CH4_DANGER || co >= CO_DANGER || nh3 >= NH3_DANGER) return DANGER;
  if (flam >= FLAM_WARN   || ch4 >= CH4_WARN   || co >= CO_WARN   || nh3 >= NH3_WARN)   return WARNING;
  return SAFE;
}

void handleBuzzer() {
  unsigned long now = millis();
  if (buzzerPattern == 0) { digitalWrite(BUZZER_PIN, LOW); return; }
  if (buzzerPattern == 3) { digitalWrite(BUZZER_PIN, HIGH); return; } 
  
  if (beepState) {
    if (now - lastBeep >= beepOn) { beepState = false; lastBeep = now; digitalWrite(BUZZER_PIN, LOW); }
  } else {
    if (now - lastBeep >= beepOff) { beepState = true; lastBeep = now; digitalWrite(BUZZER_PIN, HIGH); }
  }
}