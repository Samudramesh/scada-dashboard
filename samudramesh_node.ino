/*
  SamudraMesh node firmware, ESP32-S3 DevKitC (Arduino-ESP32 core 3.x)
  SIM_MODE 1 = fake sensor data, no hardware needed.
  SIM_MODE 0 = real sensors (install Adafruit VL53L0X + Adafruit MPU6050 libraries).
  Posts JSON to the dashboard server once per second and reads manual overrides from the reply.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

#define SIM_MODE 1

#if !SIM_MODE
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include <Adafruit_MPU6050.h>
Adafruit_VL53L0X tof;
Adafruit_MPU6050 mpu;
#endif

// ---- Network: use the PC IP printed by server.js ----
const char* WIFI_SSID  = "YOUR_WIFI";
const char* WIFI_PASS  = "YOUR_PASSWORD";
const char* SERVER_URL = "http://192.168.1.50:3000/api/telemetry";
const char* NODE_ID    = "SM-01";

// ---- Pins (safe GPIOs on ESP32-S3; AJ-SR04T ECHO is 5 V, use a divider to 3.3 V) ----
const int US_UP_TRIG = 4,  US_UP_ECHO = 5;     // upstream (drain side) level
const int US_DN_TRIG = 6,  US_DN_ECHO = 7;     // downstream (creek side) level
const int G1_OPEN = 8,  G1_CLOSE = 9;          // bypass gate 1 actuator driver (relay / H-bridge)
const int G2_OPEN = 10, G2_CLOSE = 11;         // bypass gate 2
const int I2C_SDA = 12, I2C_SCL = 13;          // VL53L0X + MPU6050
const int FLOW_PIN = 14;                       // flow module signal
const int M_RPWM = 15, M_REN = 16, M_LEN = 17; // BTS7960 conveyor motor
const int BAT_ADC = 2;                         // battery voltage divider

// ---- Calibration: measure these on your flume / site ----
const float MOUNT_UP_M = 3.0f, MOUNT_DN_M = 3.0f;   // sensor height above channel bed
const float BIN_EMPTY_MM = 400, BIN_FULL_MM = 60;   // ToF distance when bin is empty / full
const float KG_PER_PERCENT = 0.2f;                  // replace with measured value
const float BAT_DIV = 5.7f;                         // divider ratio, e.g. 47k + 10k

// ---- Control thresholds ----
const float BYPASS_OPEN_LEVEL = 2.0f, BYPASS_CLOSE_LEVEL = 1.8f;
const float HEAD_HIGH = 0.15f, HEAD_CLOG = 0.45f, HEAD_CLOSE = 0.20f;
const float BIN_START = 70, BIN_STOP = 15;

struct Sensors { float levelUp, levelDown, dH, flow, binFill, tilt, battV; } S;
float gate1 = 0, gate2 = 0, kg = 0, lastFill = 0, simBin = 10;
bool conv = false, bypass = false;
int bypassVotes = 0;
String cmdGate = "auto", cmdConv = "auto";

float noise(float a) { return (random(-100, 101) / 100.0f) * a; }

#if !SIM_MODE
float readUS(int trig, int echo) {                  // metres, NAN if no echo (blind zone ~20 cm)
  digitalWrite(trig, LOW); delayMicroseconds(4);
  digitalWrite(trig, HIGH); delayMicroseconds(12); digitalWrite(trig, LOW);
  long us = pulseIn(echo, HIGH, 30000);
  return us ? us * 0.000343f / 2.0f : NAN;
}
float readFlow() {
  // TODO: depends on the module you buy. Return signed m/s, + = toward the creek (ebb).
  // Optical/pulse type: count pulses with an interrupt and multiply by k. Doppler type: read its UART/analog output.
  return 0;
}
#endif

void readSensors() {
#if SIM_MODE
  float t = millis() / 1000.0f;
  float tide = sinf(2 * PI * t / 120.0f);
  float ph = fmodf(t, 90.0f);
  float rain = ph > 60 ? sinf(PI * (ph - 60) / 30.0f) : 0;
  float down = 1.4f + 0.6f * tide + noise(0.01f);
  float up = max(down + 0.05f, 1.2f + 1.1f * rain) + 0.30f * (simBin / 100.0f) * (conv ? 0 : 1) - 0.4f * (gate1 / 100.0f);
  float dH = up - down;
  float flow = 0.6f * (dH > 0 ? 1 : -1) * sqrtf(fabsf(dH)) * (1 + rain) - 0.8f * cosf(2 * PI * t / 120.0f);
  simBin = min(100.0f, simBin + 3 * fabsf(flow) * (0.4f + rain));
  if (conv) simBin = max(0.0f, simBin - 2.5f);
  S = { up, down, dH, flow, simBin, 2 + 7 * fabsf(flow) + noise(0.3f), 12.9f - (conv ? 0.15f : 0) + noise(0.03f) };
#else
  float u = readUS(US_UP_TRIG, US_UP_ECHO), d = readUS(US_DN_TRIG, US_DN_ECHO);
  if (!isnan(u)) S.levelUp = MOUNT_UP_M - u;
  if (!isnan(d)) S.levelDown = MOUNT_DN_M - d;
  S.dH = S.levelUp - S.levelDown;
  S.flow = readFlow();
  VL53L0X_RangingMeasurementData_t m; tof.rangingTest(&m, false);
  if (m.RangeStatus != 4) S.binFill = constrain((BIN_EMPTY_MM - m.RangeMilliMeter) / (BIN_EMPTY_MM - BIN_FULL_MM) * 100, 0, 100);
  sensors_event_t a, g, tmp; mpu.getEvent(&a, &g, &tmp);
  S.tilt = atan2f(a.acceleration.x, a.acceleration.z) * 180.0f / PI;
  S.battV = analogReadMilliVolts(BAT_ADC) / 1000.0f * BAT_DIV;
#endif
}

void control() {
  // Bypass needs HIGH water AND a head difference, or a clog signature. Rain alone never opens it.
  bool highWater = S.levelUp > BYPASS_OPEN_LEVEL && S.dH > HEAD_HIGH;
  bool clog = S.dH > HEAD_CLOG;
  bypassVotes = (highWater || clog) ? min(bypassVotes + 1, 5) : max(bypassVotes - 1, 0);
  if (bypassVotes >= 3) bypass = true;                       // must persist ~3 s
  else if (S.levelUp < BYPASS_CLOSE_LEVEL && S.dH < HEAD_CLOSE && bypassVotes == 0) bypass = false;
  if (cmdGate == "open") bypass = true; else if (cmdGate == "close") bypass = false;

  if (cmdConv == "on") conv = true;
  else if (cmdConv == "off") conv = false;
  else { if (S.binFill >= BIN_START) conv = true; if (S.binFill <= BIN_STOP) conv = false; }

  if (S.binFill < lastFill) kg += (lastFill - S.binFill) * KG_PER_PERCENT;   // kg removed
  lastFill = S.binFill;
}

void drive() {
  float target = bypass ? 100 : 0;                           // actuator travel estimated by time (~10 s full stroke)
  gate1 += constrain(target - gate1, -10, 10);
  gate2 = gate1;
  bool opening = gate1 < target - 1, closing = gate1 > target + 1;
  digitalWrite(G1_OPEN, opening); digitalWrite(G1_CLOSE, closing);
  digitalWrite(G2_OPEN, opening); digitalWrite(G2_CLOSE, closing);
  ledcWrite(M_RPWM, conv ? 200 : 0);                         // BTS7960 forward only
}

void post() {
  if (WiFi.status() != WL_CONNECTED) return;
  const char* mode = bypass ? "BYPASS" : S.flow > 0.05f ? "EBB_CAPTURE" : S.flow < -0.05f ? "FLOOD_CAPTURE" : "SLACK";
  char body[400];
  snprintf(body, sizeof(body),
    "{\"node\":\"%s\",\"mode\":\"%s\",\"kg\":%.2f,\"s\":{\"levelUp\":%.2f,\"levelDown\":%.2f,\"dH\":%.2f,"
    "\"flow\":%.2f,\"binFill\":%.1f,\"tilt\":%.1f,\"battV\":%.2f},\"a\":{\"convRpm\":%d,\"gate1\":%.0f,\"gate2\":%.0f}}",
    NODE_ID, mode, kg, S.levelUp, S.levelDown, S.dH, S.flow, S.binFill, S.tilt, S.battV, conv ? 60 : 0, gate1, gate2);
  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  if (http.POST(body) == 200) {                              // reply carries manual overrides
    String r = http.getString();
    cmdGate = r.indexOf("\"gate\":\"open\"") >= 0 ? "open" : r.indexOf("\"gate\":\"close\"") >= 0 ? "close" : "auto";
    cmdConv = r.indexOf("\"conveyor\":\"on\"") >= 0 ? "on" : r.indexOf("\"conveyor\":\"off\"") >= 0 ? "off" : "auto";
  }
  http.end();
  Serial.println(body);
}

void setup() {
  Serial.begin(115200);
  int outs[] = { G1_OPEN, G1_CLOSE, G2_OPEN, G2_CLOSE, M_REN, M_LEN, US_UP_TRIG, US_DN_TRIG };
  for (int p : outs) pinMode(p, OUTPUT);
  pinMode(US_UP_ECHO, INPUT); pinMode(US_DN_ECHO, INPUT);
  digitalWrite(M_REN, HIGH); digitalWrite(M_LEN, HIGH);
  ledcAttach(M_RPWM, 20000, 8);
#if !SIM_MODE
  Wire.begin(I2C_SDA, I2C_SCL);
  tof.begin(); mpu.begin();
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println(WiFi.localIP());
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  readSensors();
  control();
  drive();
  post();
}
