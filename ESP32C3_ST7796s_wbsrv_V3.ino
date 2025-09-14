#include <WiFi.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7796S.h>
#include <FS.h>
#include <SPIFFS.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_AHTX0.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <memory>

// ======================= Pins & Globals =======================
#define TFT_CS        7
#define TFT_DC        9
#define TFT_RST       10
#define TFT_BACKLIGHT 3
#define I2C_SDA       0
#define I2C_SCL       1

Adafruit_ST7796S tft(TFT_CS, TFT_DC, TFT_RST);
Preferences preferences;
AsyncWebServer server(80);
Adafruit_AHTX0 aht;

String ssid, password;
String lastWeatherDesc;
String lastIconPath;
String lastIconCode;
float lastOWTemp = 0, lastOWHumidity = 0;
float ahtTemp = NAN, ahtHumidity = NAN;
String lastLocation;

// Forecast data (3 next 3-hour slices)
String forecastTemps[3];
String forecastDescs[3];

// Timers
unsigned long lastWeatherUpdate   = 0;
unsigned long lastTimeSync        = 0;
unsigned long lastScreenRefresh   = 0;

// Intervals
const unsigned long weatherInterval        = 15UL * 60UL * 1000UL; // 15 min
const unsigned long timeSyncInterval       = 4UL  * 60UL * 60UL * 1000UL; // 4 hours
const unsigned long screenRefreshInterval  = 15UL * 1000UL; // 15 sec

// ======================= Helpers =======================
static inline void bootMsg(const char *msg) {
  Serial.println(msg);
  tft.fillScreen(ST77XX_WHITE);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(5);
  tft.setCursor(8, 40);
  tft.print(msg);
}

uint16_t read16(File &f) {
  uint16_t r;
  ((uint8_t *)&r)[0] = f.read();
  ((uint8_t *)&r)[1] = f.read();
  return r;
}
uint32_t read32(File &f) {
  uint32_t r;
  ((uint8_t *)&r)[0] = f.read();
  ((uint8_t *)&r)[1] = f.read();
  ((uint8_t *)&r)[2] = f.read();
  ((uint8_t *)&r)[3] = f.read();
  return r;
}

void draw1BitBMP(const char *filename, int x, int y, uint16_t fgColor, uint16_t bgColor) {
  File f = SPIFFS.open(filename);
  if (!f) return;
  if (read16(f) != 0x4D42) { f.close(); return; }
  (void)read32(f); (void)read32(f);
  uint32_t offset = read32(f);
  (void)read32(f);
  int32_t width  = (int32_t)read32(f);
  int32_t height = (int32_t)read32(f);
  if (read16(f) != 1 || read16(f) != 1) { f.close(); return; }
  (void)read32(f);
  f.seek(offset);
  int rowSize = ((width + 31) / 32) * 4;
  std::unique_ptr<uint8_t[]> rowBuf(new uint8_t[rowSize]);
  if (!rowBuf) { f.close(); return; }
  for (int row = 0; row < height; row++) {
    f.read(rowBuf.get(), rowSize);
    for (int col = 0; col < width; col++) {
      int byteIndex = col / 8;
      int bitIndex  = 7 - (col % 8);
      bool pixelOn  = rowBuf[byteIndex] & (1 << bitIndex);
      tft.drawPixel(x + col, y + (height - row - 1), pixelOn ? fgColor : bgColor);
    }
  }
  f.close();
}

String iconFromCodeToFilename(const String& code) {
  // OWM icon codes: 01d/n, 02d/n, 03d/n, 04d/n, 09d/n, 10d/n, 11d/n, 13d/n, 50d/n
  if (code.startsWith("01")) return "/icons/clear.bmp";
  if (code.startsWith("02")) return "/icons/sunny.bmp";
  if (code.startsWith("03")) return "/icons/cloud.bmp";
  if (code.startsWith("04")) return "/icons/cloud.bmp";
  if (code.startsWith("09")) return "/icons/showers.bmp";
  if (code.startsWith("10")) return "/icons/rain.bmp";
  if (code.startsWith("11")) return "/icons/thunderstorm.bmp";
  if (code.startsWith("13")) return "/icons/snow.bmp";
  if (code.startsWith("50")) return "/icons/fog.bmp";
  return "/icons/cloud.bmp";
}

String getWeatherIconFilename() {
  if (lastIconCode.length()) return iconFromCodeToFilename(lastIconCode);
  return "/icons/cloud.bmp";
}

// Minimal TZ mapping if you want to pass city names and not POSIX strings
String mapToPosixTZ(const String& tzName) {
  if (tzName == "Australia/Sydney" || tzName == "Australia/Melbourne")
    return "AEST-10AEDT,M10.1.0,M4.1.0/3";
  return tzName;
}

void configureTimeOnce(const String& tz) {
  String posixTz = mapToPosixTZ(tz);
  setenv("TZ", posixTz.c_str(), 1);
  tzset();
  configTzTime(posixTz.c_str(), "pool.ntp.org");
  struct tm ti;
  for (int i = 0; i < 10; i++) {
    if (getLocalTime(&ti)) break;
    delay(200);
  }
}

// ======================= Weather & Forecast =======================
void fetchWeather() {
  String key   = preferences.getString("apikey", "");
  String units = preferences.getString("units", "metric");
  String lat   = preferences.getString("lat", "");
  String lon   = preferences.getString("lon", "");
  if (key == "") return;

  String url;
  if (lat.length() && lon.length()) {
    url = "http://api.openweathermap.org/data/2.5/weather?lat=" + lat +
          "&lon=" + lon + "&units=" + units + "&appid=" + key;
  } else {
    String loc = preferences.getString("location", "Sydney");
    url = "http://api.openweathermap.org/data/2.5/weather?q=" + loc +
          "&units=" + units + "&appid=" + key;
  }

  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin(url)) return;
  int code = http.GET();
  if (code == 200) {
    // Filter only required fields
    StaticJsonDocument<512> filter;
    filter["name"] = true;
    filter["weather"][0]["main"] = true;
    filter["weather"][0]["icon"] = true;
    filter["main"]["temp"] = true;
    filter["main"]["humidity"] = true;

    StaticJsonDocument<768> doc;
    if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) {
      lastOWTemp      = doc["main"]["temp"] | lastOWTemp;
      lastOWHumidity  = doc["main"]["humidity"] | lastOWHumidity;
      lastWeatherDesc = doc["weather"][0]["main"].as<String>();
      lastIconCode    = doc["weather"][0]["icon"].as<String>();
      lastLocation    = doc["name"].as<String>();
    }
  }
  http.end();
}

void fetchForecast() {
  String key   = preferences.getString("apikey", "");
  String units = preferences.getString("units", "metric");
  String lat   = preferences.getString("lat", "");
  String lon   = preferences.getString("lon", "");
  if (key == "") return;

  String url;
  if (lat.length() && lon.length()) {
    url = "http://api.openweathermap.org/data/2.5/forecast?lat=" + lat +
          "&lon=" + lon + "&units=" + units + "&appid=" + key;
  } else {
    String loc = preferences.getString("location", "Sydney");
    url = "http://api.openweathermap.org/data/2.5/forecast?q=" + loc +
          "&units=" + units + "&appid=" + key;
  }

  HTTPClient http;
  http.setTimeout(7000);
  if (!http.begin(url)) return;
  int code = http.GET();
  if (code == 200) {
    StaticJsonDocument<512> filter;
    for (int i = 0; i < 3; i++) {
      filter["list"][i]["main"]["temp"] = true;
      filter["list"][i]["weather"][0]["description"] = true;
    }
    StaticJsonDocument<2048> doc;
    if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) {
      for (int i = 0; i < 3; i++) {
        forecastTemps[i] = String(doc["list"][i]["main"]["temp"].as<float>(), 1);
        forecastDescs[i] = doc["list"][i]["weather"][0]["description"].as<String>();
      }
    }
  }
  http.end();
}

// ======================= Display =======================
void updateDisplay(const String& loc, const String& weather,
                   float owT, float owH,
                   float aT, float aH,
                   const String& dateStr, const String& timeStr) {
  tft.fillScreen(ST77XX_WHITE);
  tft.setTextColor(ST77XX_BLACK);
  tft.setTextSize(5);
  tft.setCursor(0, 5);
  tft.print(dateStr); 
  tft.print("  "); 
  tft.setTextSize(7);
  tft.print(timeStr);

  tft.setTextSize(5);
  tft.setCursor(0, 65);
  tft.setTextColor(ST77XX_BLUE);
  tft.printf("O:%.1fc", owT);
  tft.print(" "); 
  tft.setTextSize(4);
  tft.printf("%.0f%%\n", owH);
  
  tft.setTextSize(5);
  tft.setCursor(0, 110);
  tft.printf("I:%.1fc", aT );
  tft.print(" "); 
  tft.setTextSize(4);
  tft.printf("%.0f%%\n", aH);

  tft.setTextSize(7);
  tft.setCursor(0, 170);
  tft.setTextColor(ST77XX_RED);
  tft.println(weather);

  lastIconPath = getWeatherIconFilename();
  draw1BitBMP(lastIconPath.c_str(), 350, 150, ST77XX_WHITE, ST77XX_BLACK);

  tft.setTextColor(ST77XX_BLUE);
  tft.setTextSize(2);
  tft.setCursor(0, 240);
  tft.println("Next Forecasts:");
  const char* labels[3] = {"3 Hour", "6 Hour", "9 Hour"};
  for (int i = 0; i < 3; i++) {
    tft.setCursor(0, 260 + i * 20);
    tft.printf("%s  %s, %sC\n", labels[i], forecastDescs[i].c_str(), forecastTemps[i].c_str());
  }
}

// ======================= WiFi & AP =======================
void startAPMode() {
  WiFi.softAP("WeatherClock_Setup", "");
  bootMsg("AP Mode On");
  tft.setTextSize(5);
  tft.setCursor(0, 100);
  tft.print("Connect: WeatherClock_Setup\nPassword: (blank)\nOpen: 192.168.4.1 or\nweatherclock.local");
}

void connectToWiFi() {
  WiFi.begin(ssid.c_str(), password.c_str());
  bootMsg("Connecting WiFi...");
  unsigned long st = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - st) < 10000) delay(300);

  if (WiFi.status() == WL_CONNECTED) {
    bootMsg("WiFi connected");
    configureTimeOnce(preferences.getString("timezone", "Australia/Sydney"));
    fetchWeather();
    fetchForecast();

    if (MDNS.begin("weatherclock")) {
      Serial.println("mDNS: http://weatherclock.local");
    } else {
      Serial.println("mDNS start failed");
    }
  } else {
    startAPMode();
  }
}

// ======================= Web UI + OTA =======================
void handleRoot(AsyncWebServerRequest *request) {
  String units = preferences.getString("units", "metric");

  AsyncResponseStream *response = request->beginResponseStream("text/html");
  response->print(F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
                    "<title>ESP32 Weather Clock</title><style>body{font-family:sans-serif}label{display:block;margin:6px 0}</style></head><body>"));

  response->print(F("<h2>Current Status</h2><ul>"));
  response->printf("<li><strong>SSID:</strong> %s</li>", ssid.c_str());
  response->printf("<li><strong>Location:</strong> %s</li>", lastLocation.c_str());
  response->printf("<li><strong>Timezone:</strong> %s</li>", preferences.getString("timezone","Australia/Sydney").c_str());
  response->printf("<li><strong>Units:</strong> %s</li>", units.c_str());
  response->printf("<li><strong>API Key:</strong> %s</li>", preferences.getString("apikey","").isEmpty() ? "(not set)" : "(set)");
  response->printf("<li><strong>Weather:</strong> %s</li>", lastWeatherDesc.c_str());
  response->printf("<li><strong>Temp:</strong> %.1f %s</li>", lastOWTemp, (units=="imperial")?"F":"C");
  response->printf("<li><strong>Humidity:</strong> %.0f%%</li>", lastOWHumidity);
  response->print(F("</ul><hr>"));

  response->print(F("<h2>OTA Update</h2><a href='/update'>Open Firmware Updater</a><hr>"));

  response->print(F("<h2>Update Settings</h2><form action='/update-settings' method='POST'>"));
  response->printf(" <label>WiFi SSID: <input name='ssid' value='%s'></label>", preferences.getString("ssid","").c_str());
  response->print(" <label>WiFi Password: <input type='password' name='password' value=''></label>");
  response->printf(" <label>Location: <input name='location' value='%s'></label>", preferences.getString("location","Sydney").c_str());
  response->printf(" <label>Latitude: <input name='latitude' value='%s'></label>", preferences.getString("lat","").c_str());
  response->printf(" <label>Longitude: <input name='longitude' value='%s'></label>", preferences.getString("lon","").c_str());
  response->printf(" <label>Timezone: <input name='timezone' value='%s'></label>", preferences.getString("timezone","Australia/Sydney").c_str());
  response->print( " <label>Units: <select name='units'>");
  response->print( (units=="metric") ? "<option value='metric' selected>Celsius</option><option value='imperial'>Fahrenheit</option>"
                                     : "<option value='metric'>Celsius</option><option value='imperial' selected>Fahrenheit</option>" );
  response->printf(" </select></label> <label>OpenWeather API Key: <input name='apikey' value='%s'></label>", preferences.getString("apikey","").c_str());
  response->print(F(" <button type='submit'>Save</button></form>"));

  response->print(F("<br><form action='/reset-wifi' method='POST'><button>Reset WiFi (AP Mode)</button></form>"
                    "<br><form action='/reset-all' method='POST'><button style='color:red'>Reset All Settings</button></form>"
                    "<br><form action='/reboot' method='POST'><button type='submit'>Reboot Device</button></form>"));

  const char* labels[3] = {"3 Hour", "6 Hour", "9 Hour"};
  response->print(F("<hr><h2>Next 3 Forecasts</h2><ul>"));
  for (int i = 0; i < 3; i++) {
    response->printf("<li><strong>%s:</strong> %s%s, %s</li>",
      labels[i],
      forecastTemps[i].c_str(),
      (units == "imperial") ? "&deg;F" : "&deg;C",
      forecastDescs[i].c_str()
    );
  }
  response->print(F("</ul></body></html>"));
  request->send(response);
}

void handleUpdateSettings(AsyncWebServerRequest *request) {
  String newSSID     = request->getParam("ssid", true)->value();
  String newPassword = request->getParam("password", true)->value();
  String newLocation = request->getParam("location", true)->value();
  String newLatitude = request->getParam("latitude", true)->value();
  String newLongitude= request->getParam("longitude", true)->value();
  String newTimezone = request->getParam("timezone", true)->value();
  String newUnits    = request->getParam("units", true)->value();
  String newApiKey   = request->getParam("apikey", true)->value();

  bool wifiChanged = (newSSID != preferences.getString("ssid", "")) ||
                     (newPassword.length() > 0);

  preferences.putString("ssid", newSSID);
  if (newPassword.length()) preferences.putString("password", newPassword);
  preferences.putString("location", newLocation);
  preferences.putString("lat", newLatitude);
  preferences.putString("lon", newLongitude);
  preferences.putString("timezone", newTimezone);
  preferences.putString("units", newUnits);
  preferences.putString("apikey", newApiKey);

  if (wifiChanged) {
    request->send(200, "text/html", "<p>WiFi settings changed. Rebooting...</p>");
    delay(500);
    ESP.restart();
  } else {
    configureTimeOnce(newTimezone);
    fetchWeather();
    fetchForecast();
    request->send(200, "text/html", "<p>Settings saved. <a href=\"/\">Back</a></p>");
  }
}

// ======================= Setup / Loop =======================
void setup() {
  Serial.begin(115200);
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);

  Wire.begin(I2C_SDA, I2C_SCL);
  tft.init(320, 480, 0, 0, ST7796S_RGB);
  tft.setRotation(1);

  SPIFFS.begin(true);
  preferences.begin("weather", false);

  bootMsg("Init AHT20...");
  bool ahtOK = aht.begin();
  bootMsg(ahtOK ? "AHT20 OK" : "AHT20 Err");

  ssid     = preferences.getString("ssid", "");
  password = preferences.getString("password", "");

  if (ssid.length() == 0 || password.length() == 0) {
    startAPMode();
  } else {
    connectToWiFi();
  }

  // Routes
  server.on("/", HTTP_GET, handleRoot);

  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", "<p>Rebooting...</p>");
    delay(300);
    ESP.restart();
  });

  server.on("/update-settings", HTTP_POST, handleUpdateSettings);

  server.on("/reset-wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    preferences.remove("ssid");
    preferences.remove("password");
    request->send(200, "text/html", "<p>WiFi cleared. Rebooting...</p>");
    delay(300);
    ESP.restart();
  });

  server.on("/reset-all", HTTP_POST, [](AsyncWebServerRequest *request) {
    preferences.clear();
    request->send(200, "text/html", "<p>All settings cleared. Rebooting...</p>");
    delay(300);
    ESP.restart();
  });

  ElegantOTA.begin(&server);  // /update page
  server.begin();
}

void loop() {
  unsigned long now = millis();

  ElegantOTA.loop();  // keep ElegantOTA responsive

  // Read AHT20 about once per second
  static unsigned long lastAht = 0;
  if (now - lastAht > 1000) {
    lastAht = now;
    sensors_event_t humidity, temp;
    if (aht.getEvent(&humidity, &temp)) {
      ahtTemp     = temp.temperature;
      ahtHumidity = humidity.relative_humidity;
    }
  }

  if (now - lastWeatherUpdate > weatherInterval) {
    lastWeatherUpdate = now;
    fetchWeather();
    fetchForecast();
  }

  if (now - lastTimeSync > timeSyncInterval) {
    lastTimeSync = now;
    configureTimeOnce(preferences.getString("timezone","Australia/Sydney"));
  }

  if (now - lastScreenRefresh > screenRefreshInterval) {
    lastScreenRefresh = now;
    struct tm ti;
    if (getLocalTime(&ti)) {
      char bufDate[16];
      char bufTime[16];
      strftime(bufDate, sizeof(bufDate), "%d.%b", &ti);
      strftime(bufTime, sizeof(bufTime), "%H:%M", &ti);

      updateDisplay(lastLocation, lastWeatherDesc, lastOWTemp, lastOWHumidity,
                    ahtTemp, ahtHumidity,
                    String(bufDate), String(bufTime));
    }
  }
}
