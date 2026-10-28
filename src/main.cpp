/*
 * ESP32 - Do nhiet do (DS18B20) + do duc, gui len Firebase Realtime Database
 * Hieu chuan nuoc sach bang nut tren trang web
 *
 * So do noi day:
 *   DS18B20 DATA  -> GPIO21  (tro 4.7k keo len 3.3V)
 *   Turbidity OUT -> cau chia ap -> GPIO34
 *                    OUT ---[R1 = 10k]--- GPIO34 ---[R2 = 15k]--- GND
 *
 * Cau truc du lieu tren Firebase:
 *   /sensors      : so do moi nhat (ESP32 ghi moi 2 giay)
 *   /calibration  : thong so & ket qua hieu chuan (ESP32 ghi)
 *   /control/calibrate : lenh hieu chuan (web ghi, ESP32 doc roi xoa)
 *
 * Thu vien can cai: OneWire, DallasTemperature
 * (WiFi, HTTPClient, Preferences co san trong core ESP32)
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include "secret.h"
// =================================================================

// ---------------- Chan ket noi ----------------
const int PIN_DS18B20   = 21;
const int PIN_TURBIDITY = 34;   // ADC1 -> doc duoc khi bat WiFi (ADC2 thi khong)

// ---------------- Thong so cam bien do duc ----------------
const float DIVIDER_RATIO = 25.0f / 15.0f;  // (R1 + R2) / R2
const int   ADC_SAMPLES   = 64;    // so mau ADC moi lan doc (bo 1/4 thap nhat va 1/4 cao nhat)
const float FILTER_ALPHA  = 0.3f;  // he so loc lam muot giua cac lan do (0..1, nho = muot hon)
const float ZERO_BAND_MV  = 20.0f; // trong khoang nay duoi U_clean coi la nuoc trong (0 NTU)

// Hieu chuan 2 diem (U_REF va NTU_REF phai do thuc te roi thay so)
const float U_CLEAN_DEFAULT_MV = 4200.0f;   // bi ghi de khi hieu chuan tu web
const float U_REF_MV           = 2500.0f;
const float NTU_REF            = 1000.0f;
const float NTU_MAX            = 3000.0f;

// Hieu chuan: do 5 giay, dao dong phai nho hon nguong nay moi chap nhan
const int   CALIB_SAMPLES      = 25;        // 25 mau x 200 ms = 5 giay
const float CALIB_MAX_RANGE_MV = 40.0f;

const unsigned long UPDATE_INTERVAL_MS = 2000;

// ---------------- Doi tuong ----------------
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
Preferences prefs;
WiFiClientSecure secureClient;

float uCleanMv = U_CLEAN_DEFAULT_MV;
unsigned long lastUpdate = 0;

// ================== FIREBASE (REST API) ==================

// Gui 1 yeu cau toi Firebase. method: GET / PUT / PATCH / DELETE
bool firebaseRequest(const char *method, const String &path,
                     const String &body = "", String *response = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = String("https://") + FIREBASE_HOST + path + ".json?auth=" + FIREBASE_SECRET;
  if (!http.begin(secureClient, url)) return false;

  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, body);

  if (response && code > 0) *response = http.getString();
  http.end();
  return code == 200;
}

// ================== CAM BIEN ==================

float readTemperatureC() {
  float t = sensors.getTempCByIndex(0);  // ket qua lan truoc
  sensors.requestTemperatures();         // ra lenh do lan moi (khong cho)
  return t;
}

// Doc 64 mau, sap xep, bo 1/4 thap nhat va 1/4 cao nhat (loai gai nhieu do WiFi phat song)
// roi lay trung binh phan con lai
float readTurbidityMv() {
  uint16_t buf[ADC_SAMPLES];
  for (int i = 0; i < ADC_SAMPLES; i++) {
    buf[i] = analogReadMilliVolts(PIN_TURBIDITY);
    delayMicroseconds(200);
  }
  for (int i = 1; i < ADC_SAMPLES; i++) {          // sap xep chen (insertion sort)
    uint16_t v = buf[i];
    int j = i - 1;
    while (j >= 0 && buf[j] > v) { buf[j + 1] = buf[j]; j--; }
    buf[j + 1] = v;
  }
  uint32_t sum = 0;
  for (int i = ADC_SAMPLES / 4; i < ADC_SAMPLES * 3 / 4; i++) sum += buf[i];
  return (sum / (float)(ADC_SAMPLES / 2)) * DIVIDER_RATIO;
}

float voltageToNTU(float mv) {
  if (mv >= uCleanMv - ZERO_BAND_MV) return 0.0f;  // nam trong vung nhieu quanh U_clean
  float ntu = NTU_REF * (uCleanMv - mv) / (uCleanMv - U_REF_MV);
  return constrain(ntu, 0.0f, NTU_MAX);
}

// ================== HIEU CHUAN ==================

void runCalibration() {
  firebaseRequest("PATCH", "/calibration", "{\"status\":\"running\"}");

  float sum = 0, minMv = 1e9, maxMv = 0;
  for (int i = 0; i < CALIB_SAMPLES; i++) {
    float mv = readTurbidityMv();
    sum += mv;
    if (mv < minMv) minMv = mv;
    if (mv > maxMv) maxMv = mv;
    delay(200);
  }
  float meanMv  = sum / CALIB_SAMPLES;
  float rangeMv = maxMv - minMv;

  const char *status;
  if (rangeMv > CALIB_MAX_RANGE_MV) {
    status = "unstable";                       // dao dong lon: bot khi, nuoc dang khuay...
  } else if (meanMv <= U_REF_MV + 300.0f) {
    status = "too_low";                        // dien ap qua thap: nuoc chua sach
  } else {
    status = "ok";
    uCleanMv = meanMv;
    prefs.putFloat("u_clean", uCleanMv);
  }

  char body[256];
  snprintf(body, sizeof(body),
           "{\"status\":\"%s\",\"measured_mv\":%.0f,\"range_mv\":%.0f,"
           "\"u_clean_mv\":%.0f,\"at\":{\".sv\":\"timestamp\"}}",
           status, meanMv, rangeMv, uCleanMv);
  firebaseRequest("PATCH", "/calibration", body);
}

// Kiem tra web co gui lenh hieu chuan khong
void checkCalibrateCommand() {
  String resp;
  if (!firebaseRequest("GET", "/control/calibrate", "", &resp)) return;
  resp.trim();
  if (resp.length() > 0 && resp != "null") {
    runCalibration();
    firebaseRequest("DELETE", "/control/calibrate");   // xoa lenh da xu ly
  }
}

// ================== DO & GUI DU LIEU ==================

void updateReadings() {
  float tempC  = readTemperatureC();
  static float umFilt = 0;
  float raw = readTurbidityMv();
  umFilt = (umFilt == 0) ? raw : umFilt + FILTER_ALPHA * (raw - umFilt);  // loc lam muot
  float um     = umFilt;
  float ntu    = voltageToNTU(um);
  bool  tempOk = (tempC != DEVICE_DISCONNECTED_C);

  // Gui len Firebase
  char tempStr[16];
  if (tempOk) snprintf(tempStr, sizeof(tempStr), "%.2f", tempC);
  else        strcpy(tempStr, "null");

  char body[256];
  snprintf(body, sizeof(body),
           "{\"temperature\":%s,\"temp_ok\":%s,\"voltage_mv\":%.0f,"
           "\"ntu\":%.1f,\"updated\":{\".sv\":\"timestamp\"}}",
           tempStr, tempOk ? "true" : "false", um, ntu);
  firebaseRequest("PUT", "/sensors", body);
}

// ================== CHUONG TRINH CHINH ==================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);   // cho toi da 15 giay; khong duoc thi van chay tiep, ESP32 tu ket noi lai sau
  }
}

void setup() {
  prefs.begin("turbidity", false);
  uCleanMv = prefs.getFloat("u_clean", U_CLEAN_DEFAULT_MV);

  sensors.begin();
  sensors.requestTemperatures();         // lan dau: cho de co so dung
  sensors.setWaitForConversion(false);   // tu day: khong chan chuong trinh

  connectWiFi();
  secureClient.setInsecure();            // bo qua kiem tra chung chi SSL (don gian hoa)

  // Xoa lenh hieu chuan cu con sot lai (tranh tu hieu chuan khi vua bat may)
  firebaseRequest("DELETE", "/control/calibrate");

  // Bao cho web biet thong so hieu chuan hien tai
  char body[192];
  snprintf(body, sizeof(body),
           "{\"status\":\"idle\",\"u_clean_mv\":%.0f,\"u_ref_mv\":%.0f,\"ntu_ref\":%.0f}",
           uCleanMv, U_REF_MV, NTU_REF);
  firebaseRequest("PATCH", "/calibration", body);
}

void loop() {
  if (millis() - lastUpdate >= UPDATE_INTERVAL_MS) {
    lastUpdate = millis();
    updateReadings();
    checkCalibrateCommand();
  }
}