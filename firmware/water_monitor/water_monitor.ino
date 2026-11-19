/*
  ESP32 Water & Air Monitor -> Firebase Realtime Database -> web dashboard
  Pins: pH 34 | TDS 35 | Turbidity 32 (divider) | MQ135 33 | DS18B20 4 | DHT22 27
        Ultrasonic TRIG 5, ECHO 18 (divider) | LCD SDA 21, SCL 22
*/
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- WIFI & FIREBASE ----------
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* DB_URL    = "https://water-monitor-3ae76-default-rtdb.asia-southeast1.firebasedatabase.app";
// -------------------------------------

const unsigned long SEND_MS = 2000;      // live data every 2 sec
const unsigned long HIST_MS = 300000UL;  // history point every 5 min
const unsigned long LCD_MS  = 4000;      // LCD page switch

const int PIN_PH = 34, PIN_TDS = 35, PIN_NTU = 32, PIN_MQ = 33;
const int PIN_DS = 4, PIN_DHT = 27, PIN_TRIG = 5, PIN_ECHO = 18;

// Divider factor 2.0 only for turbidity (2.2k + 2.2k divider)
const float DIV_PH = 1.0, DIV_NTU = 2.0, DIV_MQ = 1.0, DIV_TDS = 1.0;
float PH7_V = 2.50;               // pH 7 buffer-e je volt ashe, ekhane boshao
const float PH_SLOPE = 0.18;
const float NO_SIGNAL_V = 0.20;   // er niche hole sensor-e signal nei -> "--" dekhabe

OneWire ow(PIN_DS);
DallasTemperature ds(&ow);
DHT dht(PIN_DHT, DHT22);
LiquidCrystal_I2C lcd(0x27, 16, 2);   // kaj na korle 0x3F dao

WiFiClientSecure sc;
HTTPClient hc;

unsigned long lastSend = 0, lastHist = 0, lastLcd = 0;
float at = NAN, ah = NAN;
bool page = false;

void firebase(const char* method, String path, String body) {
  if (WiFi.status() != WL_CONNECTED) return;
  hc.setReuse(true);
  hc.setTimeout(4000);
  hc.begin(sc, String(DB_URL) + path);
  hc.addHeader("Content-Type", "application/json");
  int code = hc.sendRequest(method, body);
  Serial.println(String(method) + " " + path + " -> " + code);
  hc.end();
}

float readRaw(int pin) {                       // median of 9 samples, volts at the ESP32 pin
  const int n = 9; float v[n];
  for (int i = 0; i < n; i++) { v[i] = analogReadMilliVolts(pin) / 1000.0; delay(2); }
  for (int i = 0; i < n - 1; i++) for (int j = i + 1; j < n; j++) if (v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }
  return v[n / 2];
}

float readDistanceCm() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_TRIG, LOW); delayMicroseconds(4);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(12);
    digitalWrite(PIN_TRIG, LOW);
    long d = pulseIn(PIN_ECHO, HIGH, 30000);
    if (d > 0) return d * 0.0343 / 2.0;
    delay(30);
  }
  return NAN;
}

void wifiConnect() {
  if (WiFi.status() == WL_CONNECTED) return;
  static unsigned long lastTry = 0;
  if (lastTry != 0 && millis() - lastTry < 10000) return;
  lastTry = millis();
  Serial.println("WiFi connecting...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

String num(float v, int d) { return isnan(v) ? "null" : String(v, d); }
String show(float v, int d) { return isnan(v) ? "--" : String(v, d); }

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  ds.begin();
  Serial.println("DS18B20 sensor count: " + String(ds.getDeviceCount()));
  ds.setResolution(10);
  ds.setWaitForConversion(false);
  ds.requestTemperatures();
  dht.begin();
  sc.setInsecure();
  Wire.begin(21, 22);
  lcd.init(); lcd.backlight(); lcd.print("Connecting WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  Serial.println(WiFi.status() == WL_CONNECTED ? "WiFi OK: " + WiFi.localIP().toString() : "WiFi FAILED");
  lcd.clear();
}

void loop() {
  if (millis() - lastSend < SEND_MS) return;
  lastSend = millis();
  wifiConnect();

  // Water temperature
  float wt = ds.getTempCByIndex(0);
  ds.requestTemperatures();
  if (wt == DEVICE_DISCONNECTED_C) wt = NAN;

  // Air temperature & humidity (keep last good value)
  float t = dht.readTemperature(), h = dht.readHumidity();
  if (!isnan(t)) at = t;
  if (!isnan(h)) ah = h;

  // pH
  float rph = readRaw(PIN_PH);
  float ph = (rph < NO_SIGNAL_V) ? NAN : 7.0 + (PH7_V - rph * DIV_PH) / PH_SLOPE;

  // TDS
  float rtd = readRaw(PIN_TDS);
  float tds = NAN;
  if (rtd >= NO_SIGNAL_V) {
    float k = 1.0 + 0.02 * ((isnan(wt) ? 25 : wt) - 25.0);
    float c = rtd * DIV_TDS / k;
    tds = (133.42 * c * c * c - 255.86 * c * c + 857.39 * c) * 0.5;
  }

  // Turbidity
  float rn = readRaw(PIN_NTU);
  float vn = rn * DIV_NTU;
  float ntu = NAN;
  if (rn >= NO_SIGNAL_V) {
    ntu = vn < 2.5 ? 3000 : (-1120.4 * vn * vn + 5742.3 * vn - 4352.9);
    if (ntu < 0) ntu = 0;
  }

  // Air quality
  float rmq = readRaw(PIN_MQ);
  float aq = (rmq < NO_SIGNAL_V) ? NAN : rmq * DIV_MQ * 1000.0 / 3.3 * 0.82;

  // Water level (distance to water)
  float lvl = readDistanceCm();

  Serial.println("Ultrasonic: " + show(lvl, 1) + " cm | DS18B20: " + show(wt, 1) + " C");
  Serial.printf("Pin volts  pH=%.2f  TDS=%.2f  NTU=%.2f  MQ=%.2f\n", rph, rtd, rn, rmq);

  String j = "{\"ph\":" + num(ph, 2) + ",\"tds\":" + num(tds, 0) + ",\"ntu\":" + num(ntu, 1) +
             ",\"wt\":" + num(wt, 1) + ",\"at\":" + num(at, 1) + ",\"ah\":" + num(ah, 0) +
             ",\"aq\":" + num(aq, 0) + ",\"lvl\":" + num(lvl, 1) + ",\"ts\":{\".sv\":\"timestamp\"}}";
  Serial.println(j);
  firebase("PUT", "/sensors/latest.json", j);

  if (lastHist == 0 || millis() - lastHist > HIST_MS) {
    lastHist = millis();
    firebase("POST", "/history.json", j);
  }

  // LCD
  if (millis() - lastLcd > LCD_MS) {
    lastLcd = millis(); page = !page; lcd.clear();
    if (page) {
      lcd.print("pH " + show(ph, 1) + " T" + show(wt, 1));
      lcd.setCursor(0, 1);
      lcd.print("TDS " + show(tds, 0) + " NTU" + show(ntu, 0));
    } else {
      lcd.print("Air " + show(at, 1) + "C " + show(ah, 0) + "%");
      lcd.setCursor(0, 1);
      lcd.print("Lvl " + show(lvl, 0) + "cm AQ" + show(aq, 0));
    }
  }
}
