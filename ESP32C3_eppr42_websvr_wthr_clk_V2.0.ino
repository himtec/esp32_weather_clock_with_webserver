#define ENABLE_GxEPD2_GFX 0
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <Adafruit_GFX.h>
#include <SPI.h>
#include <Wire.h>
#include <FS.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Adafruit_Sensor.h>
//#include <Adafruit_BMP280.h>
//#include <Adafruit_AHTX0.h>
#include <ElegantOTA.h>

// Pin definitions for WeActStudio 4.2" e-paper (adjust as needed)
#define EPD_CS   7
#define EPD_DC   1
#define EPD_RST  2
#define EPD_BUSY 3

// ESP32-C3 
//CS(SS)=7,
//SCL(SCK)=4,
//SDA(MOSI)=6,
//BUSY=3,
//RES(RST)=2,
//DC=1

//Pin definitions for BMP and AHT sensors
//#define I2C_SDA 0
//#define I2C_SCL 1

// For WeActStudio 4.2" EPD (400x300, SSD1683)
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(GxEPD2_420_GDEY042T81(/*CS=*/ EPD_CS, /*DC=*/ EPD_DC, /*RST=*/ EPD_RST, /*BUSY=*/ EPD_BUSY));

Preferences preferences;
AsyncWebServer server(80);
//Adafruit_BMP280 bmp;
//Adafruit_AHTX0 aht;

String ssid, password;
String lastWeatherDesc;
String lastIconPath;
float lastOWTemp = 0, lastOWHumidity = 0;
float bmpTemp = 0, bmpPressure = 0;
float ahtTemp = 0, ahtHumidity = 0;
String lastLocation;

// Add these global variables to hold forecast data
String forecastTimes[3];
String forecastTemps[3];
String forecastDescs[3];

unsigned long lastWeatherUpdate = 0;
unsigned long lastTimeSync = 0;
unsigned long lastScreenRefresh = 0;

const unsigned long weatherInterval       = 15 * 60 * 1000;
const unsigned long timeSyncInterval     = 4 * 60 * 60 * 1000;
const unsigned long screenRefreshInterval = 30 * 1000;

bool otaEnabled = false;

void bootMsg(const String &msg) {
  Serial.println(msg);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(10, 50);
    display.print(msg);
  } while (display.nextPage());
  delay(800);
}

uint16_t read16(File &f) {
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  return result;
}

uint32_t read32(File &f) {
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read();
  return result;
}

// Remove draw1BitBMP, use GxEPD's drawBitmap if you have 1-bit BMPs in SPIFFS
void draw1BitBMP(const char *filename, int16_t x, int16_t y) {
  File bmpFile = SPIFFS.open(filename, "r");
  if (!bmpFile) return;

  // BMP Header
  if (bmpFile.read() != 'B' || bmpFile.read() != 'M') {
    bmpFile.close();
    return;
  }
  bmpFile.seek(10);
  uint32_t pixelOffset = read32(bmpFile);
  bmpFile.seek(18);
  int32_t bmpWidth  = read32(bmpFile);
  int32_t bmpHeight = read32(bmpFile);
  bmpFile.seek(28);
  uint16_t bitDepth = read16(bmpFile);

  if (bitDepth != 1) { // Only support 1-bit BMP
    bmpFile.close();
    return;
  }

  // Go to pixel data
  bmpFile.seek(pixelOffset);

  int rowSize = ((bmpWidth + 31) / 32) * 4; // Row size is padded to 4 bytes
  uint8_t line[rowSize];

  for (int16_t row = 0; row < bmpHeight; row++) {
    int16_t drawY = y + bmpHeight - 1 - row; // BMP is bottom-up
    bmpFile.read(line, rowSize);
    for (int16_t col = 0; col < bmpWidth; col++) {
      int byteIdx = col / 8;
      int bitIdx = 7 - (col % 8);
      if (line[byteIdx] & (1 << bitIdx)) {
        display.drawPixel(x + col, drawY, GxEPD_WHITE); // Invert: draw white for set bit
      } else {
        display.drawPixel(x + col, drawY, GxEPD_BLACK); // Invert: draw black for unset bit
      }
    }
  }
  bmpFile.close();
}

String getWeatherIconFilename(const String& desc) {
  String key = desc;
  key.toLowerCase();
  if (key.indexOf("clear") >= 0) return "/icons/clear.bmp";
  if (key.indexOf("cloud") >= 0) return "/icons/cloud.bmp";
  if (key.indexOf("rain") >= 0) return "/icons/rain.bmp";
  if (key.indexOf("shower") >= 0) return "/icons/showers.bmp";
  if (key.indexOf("thunder") >= 0) return "/icons/thunderstorm.bmp";
  if (key.indexOf("snow") >= 0) return "/icons/snow.bmp";
  if (key.indexOf("fog") >= 0) return "/icons/fog.bmp";
  if (key.indexOf("haze") >= 0) return "/icons/haze.bmp";
  if (key.indexOf("sun") >= 0) return "/icons/sunny.bmp";
  if (key.indexOf("wind") >= 0) return "/icons/windy.bmp";
  return "/icons/cloud.bmp";
}

void updateDisplay(const String& loc, const String& weather, float owT, float owH,
                   float bT, float bP, float aT, float aH, const String& timeStr, const String& timeStr2) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(3);
    display.setCursor(0, 50);
    display.println(timeStr2 /*+ " " + timeStr*/);

    display.setTextSize(2);
    display.setCursor(0, 90);
    display.printf("Temp:%.1fc", owT);
    display.setCursor(0, 130);
    display.printf("Hum:%.0f%%", owH);
    //display.setCursor(0, 90);
    //display.printf("I:%.1fc H:%.0f%%", aT, aH);

    display.setTextSize(2);
    display.setCursor(0, 170);
    display.println(weather);

    String iconFile = getWeatherIconFilename(weather);
    lastIconPath = iconFile;
    draw1BitBMP(iconFile.c_str(), 295, 200);

    display.setTextSize(1);
    display.setCursor(0, 210);
    display.println("Next 3 Forecasts:");
    for (int i = 0; i < 3; i++) {
      display.setCursor(0, 235 + i * 20);
      String localTime = forecastTimeToLocal(forecastTimes[i]);
      display.printf("%s  %s, %sC", 
        localTime.c_str(),
        forecastDescs[i].c_str(),
        forecastTemps[i].c_str());
    }
  } while (display.nextPage());
}

String mapToPosixTZ(const String& tzName) {
  if (tzName == "Australia/Sydney") return "AEST-10AEDT,M10.1.0,M4.1.0/3";
  return tzName;
}

// Only set timezone ONCE, globally, in configureTime()
void configureTime(const String& tz) {
  String posixTz = mapToPosixTZ(tz);
  setenv("TZ", posixTz.c_str(), 1);
  tzset();
  configTzTime(posixTz.c_str(), "pool.ntp.org");
  bootMsg("Syncing time...");
  struct tm ti;
  int retry = 0;
  while (!getLocalTime(&ti) && retry++ < 10) delay(500);
  if (retry < 10) {
    char buf[64];
    char buf2[64];
    strftime(buf, sizeof(buf), "%d.%b", &ti);
    strftime(buf2, sizeof(buf2), "%H:%M", &ti);
    Serial.printf("Time: %s %s\n", buf, buf2);
  }
}

void fetchWeather() {
  String key = preferences.getString("apikey", "");
  String units = preferences.getString("units", "metric");
  String lat = preferences.getString("lat", "");
  String lon = preferences.getString("lon", "");

  if (key == "") return;

  String url;
  if (lat != "" && lon != "") {
    url = "http://api.openweathermap.org/data/2.5/weather?lat=" + lat +
          "&lon=" + lon + "&units=" + units + "&appid=" + key;
  } else {
    String loc = preferences.getString("location", "Sydney");
    url = "http://api.openweathermap.org/data/2.5/weather?q=" + loc +
          "&units=" + units + "&appid=" + key;
  }

  HTTPClient http;
  http.begin(url);
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, http.getString());
    lastOWTemp = doc["main"]["temp"];
    lastOWHumidity = doc["main"]["humidity"];
    lastWeatherDesc = doc["weather"][0]["main"].as<String>();
    lastLocation = doc["name"].as<String>();
  }
  http.end();
}

// Additional handler to receive lat/lon and store them
// void setupGeoEndpoint() {
//   server.on("/set-geo", HTTP_POST, [](AsyncWebServerRequest *request) {
//     if (request->hasParam("lat", true) && request->hasParam("lon", true)) {
//       preferences.putString("lat", request->getParam("lat", true)->value());
//       preferences.putString("lon", request->getParam("lon", true)->value());
//       request->send(200, "text/plain", "Coordinates saved");
//     } else {
//       request->send(400, "text/plain", "Missing lat/lon");
//     }
//   });
// }

// Be sure to call setupGeoEndpoint(); inside setup()

void startAPMode() {
  WiFi.softAP("WeatherClock_Config", "weather123");
  bootMsg("AP Mode On");
  display.setTextSize(2);
  display.setCursor(10, 120);
  display.print("Connect to WiFi PS:weather123");
}

void connectToWiFi() {
  WiFi.begin(ssid.c_str(), password.c_str());
  bootMsg("Connecting WiFi...");
  unsigned long st = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - st < 10000) delay(500);
  if (WiFi.status() == WL_CONNECTED) {
    bootMsg("WiFi connected");
    MDNS.begin("weatherstation2");
    bootMsg("mDNS ready");
    otaEnabled = preferences.getBool("ota_enabled", false);
    configureTime(preferences.getString("timezone", "Australia/Sydney"));
    fetchWeather();
    fetchForecast();
  } else {
    startAPMode();
  }
}

void fetchForecast() {
  String key = preferences.getString("apikey", "");
  String units = preferences.getString("units", "metric");
  String lat = preferences.getString("lat", "");
  String lon = preferences.getString("lon", "");
  String url;

  if (lat != "" && lon != "") {
    url = "http://api.openweathermap.org/data/2.5/forecast?lat=" + lat +
          "&lon=" + lon + "&units=" + units + "&appid=" + key;
  } else {
    String loc = preferences.getString("location", "Sydney");
    url = "http://api.openweathermap.org/data/2.5/forecast?q=" + loc +
          "&units=" + units + "&appid=" + key;
  }

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == 200) {
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, http.getString());
    for (int i = 0; i < 3; i++) {
      forecastTimes[i] = doc["list"][i]["dt_txt"].as<String>();
      forecastTemps[i] = String(doc["list"][i]["main"]["temp"].as<float>());
      forecastDescs[i] = doc["list"][i]["weather"][0]["description"].as<String>();
    }
  }
  http.end();
}

// Do NOT set timezone in this function!
String forecastTimeToLocal(const String& utcDtTxt) {
  struct tm tm_utc;
  memset(&tm_utc, 0, sizeof(tm_utc));
  strptime(utcDtTxt.c_str(), "%Y-%m-%d %H:%M:%S", &tm_utc);
  time_t t = mktime(&tm_utc); // Treat as local time, which matches your TZ
  struct tm *tm_local = localtime(&t);
  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", tm_local);
  return String(buf);
}

void setup() {
  Serial.begin(115200);
  //Wire.begin(I2C_SDA, I2C_SCL);
  SPIFFS.begin(true);
  preferences.begin("weather", false);

  display.init(115200, true, 50, false); // Use the same as the example
  display.setFont(&FreeMonoBold9pt7b);
  display.setRotation(4);
  display.setFullWindow();

  // bootMsg("Init BMP280...");
  // if (!(bmp.begin(0x76)||bmp.begin(0x77))) bootMsg("BMP280 Err");
  // else bootMsg("BMP280 OK");

  // bootMsg("Init AHT20...");
  // if (!aht.begin()) bootMsg("AHT20 Err");
  // else bootMsg("AHT20 OK");

  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  if (ssid == "" || password == "") startAPMode();
  else connectToWiFi();

  ElegantOTA.begin(&server);
 
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
  String location = preferences.getString("location", "Sydney");
  String lat = preferences.getString("lat", "");
  String lon = preferences.getString("lon", "");
  String timezone = preferences.getString("timezone", "Australia/Sydney");
  String units = preferences.getString("units", "metric");
  String apiKey = preferences.getString("apikey", "");

  String posixTz = mapToPosixTZ(timezone);

  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>ESP32 Weather Clock</title></head><body>"
                "<h2>Current Settings</h2><ul>"
                "<li><strong>SSID:</strong> " + ssid + "</li>"
                "<li><strong>Location:</strong> " + lastLocation + "</li>"
                "<li><strong>Latitude:</strong> " + lat + "</li>"
                "<li><strong>Longitude:</strong> " + lon + "</li>"
                "<li><strong>Timezone:</strong> " + timezone + "</li>"
                "<li><strong>Units:</strong> " + units + "</li>"
                "<li><strong>API Key:</strong> " + (apiKey.isEmpty() ? "(not set)" : "(set)") + "</li>"
                "<li><strong>Weather:</strong> " + lastWeatherDesc + "</li>"
                "<li><strong>Weather Icon:</strong> " + lastIconPath + "</li>"
                "<li><strong>Temperature:</strong> " + String(lastOWTemp) + " C</li>"
                "<li><strong>Humidity:</strong> " + String(lastOWHumidity) + "%</li>"
                "</ul><hr><h2>Update Settings</h2>"
                "<form action='/update-settings' method='POST'>"
                "WiFi SSID: <input name='ssid' value='" + ssid + "'><br><br>"
                "WiFi Password: <input type='password' name='password' value='" + password + "'><br><br>"
                "Location: <input name='location' value='" + location + "'><br><br>"
                "Latitude: <input name='latitude' value='" + lat + "'><br><br>"
                "Longitude: <input name='longitude' value='" + lon + "'><br><br>"
                "Timezone: <input name='timezone' value='" + timezone + "'><br><br>"
                "Units: <select name='units'>"
                "<option value='metric'" + (units == "metric" ? " selected" : "") + ">Celsius</option>"
                "<option value='imperial'" + (units == "imperial" ? " selected" : "") + ">Fahrenheit</option>"
                "</select><br><br>"
                "OpenWeather API Key: <input name='apikey' value='" + apiKey + "'><br><br>"
                "<button type='submit'>Save</button></form>"

                "<br><form action='/reset-wifi' method='POST'><button>Reset WiFi (AP Mode)</button></form>"
                "<br><form action='/reset-all' method='POST'><button style='color:red'>Reset All Settings</button></form>"
                "<br><form action='/reboot' method='POST'><button type='submit'>Reboot Device</button></form>"
                "<hr><h2>Next 3 Forecasts (Local Time, 3-hour intervals)</h2><ul>";
  for (int i = 0; i < 3; i++) {
    String localTime = forecastTimeToLocal(forecastTimes[i]);
    html += "<li><strong>" + localTime + ":</strong> " + forecastTemps[i] + (units == "imperial" ? "&deg;F" : "&deg;C") + ", " + forecastDescs[i] + "</li>";
  }
  html += "</ul>"
                "</body></html>";
                
  request->send(200, "text/html", html);
});


  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", "<p>Rebooting...</p>");
    delay(500);
    ESP.restart();
  });

  server.on("/update-settings", HTTP_POST, [](AsyncWebServerRequest *request) {
  String newSSID = request->getParam("ssid", true)->value();
  String newPassword = request->getParam("password", true)->value();
  String newLocation = request->getParam("location", true)->value();
  String newLatitude = request->getParam("latitude", true)->value();
  String newLongitude = request->getParam("longitude", true)->value();
  String newTimezone = request->getParam("timezone", true)->value();
  String newUnits = request->getParam("units", true)->value();
  String newApiKey = request->getParam("apikey", true)->value();

  bool wifiChanged = (newSSID != preferences.getString("ssid", "")) ||
                     (newPassword != preferences.getString("password", ""));

  preferences.putString("ssid", newSSID);
  preferences.putString("password", newPassword);
  preferences.putString("location", newLocation);
  preferences.putString("lat", newLatitude);
  preferences.putString("lon", newLongitude);
  preferences.putString("timezone", newTimezone);
  preferences.putString("units", newUnits);
  preferences.putString("apikey", newApiKey);

  if (wifiChanged) {
    request->send(200, "text/html", "<p>WiFi settings changed. Rebooting...</p>");
    delay(1000);
    ESP.restart();
  } else {
    configureTime(newTimezone);
    fetchWeather();
    fetchForecast();
    request->send(200, "text/html", "<p>Settings saved. No reboot needed.</p><p><a href='/'>Back</a></p>");
  }
});

  server.on("/reset-wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    preferences.remove("ssid");
    preferences.remove("password");
    request->send(200, "text/html", "<p>WiFi credentials cleared. Rebooting...</p>");
    delay(1000);
    ESP.restart();
  });

  server.on("/reset-all", HTTP_POST, [](AsyncWebServerRequest *request) {
    preferences.clear();
    request->send(200, "text/html", "<p>All settings cleared. Rebooting...</p>");
    delay(1000);
    ESP.restart();
  });
  
  server.begin();
  //setupGeoEndpoint();

}

void loop() {
  unsigned long now = millis();
  ElegantOTA.loop();

  // if (bmp.begin(0x76)) {
  //   bmpTemp = bmp.readTemperature();
  //   bmpPressure = bmp.readPressure();
  // }

  sensors_event_t humidity, temp;
//  aht.getEvent(&humidity, &temp);
  // ahtTemp = temp.temperature;
  // ahtHumidity = humidity.relative_humidity;
  if (now - lastWeatherUpdate > weatherInterval) {
    lastWeatherUpdate = now;
    fetchWeather();
    fetchForecast();
  }
  if (now - lastTimeSync > timeSyncInterval) {
    lastTimeSync = now;
    configureTime(preferences.getString("timezone","Australia/Sydney"));
  }
  if (now - lastScreenRefresh > screenRefreshInterval) {
    lastScreenRefresh = now;
    struct tm ti;
    if (getLocalTime(&ti)) {
      char buf[64];
      char buf2[64];
      strftime(buf, sizeof(buf), "%d.%b", &ti);
      strftime(buf2, sizeof(buf2), "%H:%M", &ti);
      
      updateDisplay(lastLocation, lastWeatherDesc, lastOWTemp, lastOWHumidity,
                    bmpTemp, bmpPressure, ahtTemp, ahtHumidity, String(buf),String(buf2));
    }
  }
}
