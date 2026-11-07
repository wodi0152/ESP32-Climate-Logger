#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Adafruit_AHTX0.h>
#include <time.h>
#include <math.h>
#include <string.h>

// ===== ESP32 + AHT10 + BUTTON =====
constexpr uint8_t SDA_PIN = 21;
constexpr uint8_t SCL_PIN = 22;
constexpr uint8_t BUTTON_PIN = 27;

const char *AP_SSID = "ESP32-Climate-Setup";
const char *AP_PASS = "ClimateSet_7284";
const char *DEVICE_ID = "ESP32_001";

// FINAL: 15 minutes between independent measurements.
constexpr uint32_t MEASURE_INTERVAL_MS = 900000UL;
constexpr uint32_t FIRST_MEASURE_DELAY_MS = 15000UL;
constexpr uint32_t WIFI_RETRY_MS = 15000UL;
constexpr uint32_t HTTP_RETRY_MS = 60000UL;
constexpr uint32_t HTTP_DRAIN_MS = 2000UL;
constexpr uint32_t BUTTON_HOLD_MS = 3000UL;
constexpr uint32_t DEBOUNCE_MS = 40UL;

// 96 readings at 15-minute intervals: approximately 24 hours of backlog.
constexpr uint16_t MAX_SAMPLES = 96;
constexpr uint32_t QUEUE_VERSION = 1;
constexpr time_t MIN_VALID_TIME = 1700000000; // NTP not yet synchronized before this

struct Sample
{
    uint32_t sampleId;
    uint32_t measuredAtUtc; // Unix UTC seconds; 0 = clock not synchronized
    float temperature;
    float humidity;
};

struct QueueState
{
    uint32_t version;
    uint32_t nextSampleId;
    uint16_t count;
    uint16_t reserved;
    Sample samples[MAX_SAMPLES];
};

static_assert(sizeof(Sample) == 16, "Unexpected sample format");

Adafruit_AHTX0 aht;
WebServer web(80);
Preferences prefs;
QueueState queueState = {};

String wifiSSID;
String wifiPassword;
String serverIP;
uint16_t serverPort = 8080;

bool sensorReady = false;
bool configMode = false;
bool restartPending = false;
bool rawPressedLast = false;
bool stablePressed = false;
bool holdHandled = false;
bool timeSyncStarted = false;
bool timeSyncLogged = false;
bool firstMeasureDone = false;

uint32_t startedAt = 0;
uint32_t lastMeasurementAt = 0;
uint32_t lastWiFiRetryAt = 0;
uint32_t lastHttpTryAt = 0;
uint32_t buttonChangedAt = 0;
uint32_t buttonDownAt = 0;
uint32_t restartAt = 0;
int previousWiFiStatus = -1;

// Forward declarations needed for PlatformIO's .cpp compilation.
void startConfigMode();

// ===== PERSISTENT SERVER SETTINGS =====
void loadConfig()
{
    prefs.begin("climate", true);
    wifiSSID = prefs.getString("ssid", "");
    wifiPassword = prefs.getString("password", "");
    serverIP = prefs.getString("server_ip", "");
    serverPort = prefs.getUShort("port", 8080);
    prefs.end();

    Serial.printf("Saved Wi-Fi SSID: %s\n", wifiSSID.c_str());
    Serial.printf("Saved server: %s:%u\n", serverIP.c_str(), serverPort);
}

// ===== PERSISTENT OUTBOX =====
bool saveQueue()
{
    if (!prefs.begin("outbox", false))
    {
        Serial.println("[QUEUE] ERROR opening NVS");
        return false;
    }
    size_t written = prefs.putBytes("state", &queueState, sizeof(queueState));
    prefs.end();
    if (written != sizeof(queueState))
    {
        Serial.println("[QUEUE] ERROR writing NVS; data might be lost on power-off");
        return false;
    }
    return true;
}

void loadQueue()
{
    bool loaded = false;
    if (prefs.begin("outbox", true))
    {
        if (prefs.getBytesLength("state") == sizeof(queueState))
        {
            loaded = (prefs.getBytes("state", &queueState, sizeof(queueState)) == sizeof(queueState));
        }
        prefs.end();
    }

    if (!loaded || queueState.version != QUEUE_VERSION ||
        queueState.count > MAX_SAMPLES || queueState.nextSampleId == 0)
    {
        memset(&queueState, 0, sizeof(queueState));
        queueState.version = QUEUE_VERSION;
        queueState.nextSampleId = 1;
        // Only initialize the persistent queue when no valid saved queue exists.
        saveQueue();
        Serial.println("[QUEUE] Initialized empty queue");
    }
    else
    {
        Serial.printf("[QUEUE] Restored %u pending measurements\n", queueState.count);
    }
}

void enqueueSample(float temperature, float humidity)
{
    if (queueState.count == MAX_SAMPLES)
    {
        // Fixed-size storage: discard oldest only after 24 hours of outages.
        memmove(queueState.samples, queueState.samples + 1,
                (MAX_SAMPLES - 1) * sizeof(Sample));
        queueState.count = MAX_SAMPLES - 1;
        Serial.println("[QUEUE] FULL: oldest measurement discarded");
    }

    time_t nowUtc = time(nullptr);
    uint32_t timestamp = nowUtc >= MIN_VALID_TIME
                             ? static_cast<uint32_t>(nowUtc)
                             : 0;

    Sample &sample = queueState.samples[queueState.count++];
    sample.sampleId = queueState.nextSampleId++;
    if (queueState.nextSampleId == 0)
        queueState.nextSampleId = 1;
    sample.measuredAtUtc = timestamp;
    sample.temperature = temperature;
    sample.humidity = humidity;

    bool saved = saveQueue();
    Serial.printf("[QUEUE] Added sample #%lu, pending=%u, UTC=%lu, saved=%s\n",
                  (unsigned long)sample.sampleId, queueState.count,
                  (unsigned long)timestamp, saved ? "YES" : "NO");
    if (timestamp == 0)
    {
        Serial.println("[TIME] Clock not synchronized: timestamp will be unknown");
    }
}

bool removeOldestSample()
{
    if (queueState.count == 0)
        return true;
    if (queueState.count > 1)
    {
        memmove(queueState.samples, queueState.samples + 1,
                (queueState.count - 1) * sizeof(Sample));
    }
    queueState.count--;
    bool saved = saveQueue();
    Serial.printf("[QUEUE] Delivered. Pending=%u\n", queueState.count);
    return saved;
}

// ===== WEB CONFIGURATION =====
String configPage()
{
    String page = R"HTML(<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Climate Logger</title>
<style>body{font-family:Arial,sans-serif;background:#101827;color:white;padding:24px}
main{max-width:440px;margin:auto}input,button{box-sizing:border-box;width:100%;padding:13px;
margin:9px 0 20px;border-radius:8px;border:0;font-size:16px}
button{background:#16a085;color:white;cursor:pointer}label{display:block}</style>
</head><body><main><h2>ESP32 Climate Logger</h2>
<p>Wi-Fi and destination server settings</p>
<form method="POST" action="/save">
<label>Wi-Fi SSID</label><input name="ssid" placeholder="Leave empty to keep saved network">
<label>Wi-Fi password</label><input name="password" type="password" placeholder="Leave empty to keep saved password">
<label>Server IPv4 address</label><input name="ip" value="{{IP}}" required>
<label>Server port</label><input name="port" type="number" min="1" max="65535" value="{{PORT}}" required>
<button type="submit">Save and restart</button></form></main></body></html>)HTML";
    page.replace("{{IP}}", serverIP);
    page.replace("{{PORT}}", String(serverPort));
    return page;
}

void setupWebServer()
{
    web.on("/", HTTP_GET, []()
           { web.send(200, "text/html; charset=utf-8", configPage()); });
    web.on("/save", HTTP_POST, []()
           {
        String newIP = web.arg("ip");
        String newSSID = web.arg("ssid");
        String newPassword = web.arg("password");
        newIP.trim();
        newSSID.trim();
        int newPort = web.arg("port").toInt();
        IPAddress address;
        if (!address.fromString(newIP) || newPort < 1 || newPort > 65535) {
            web.send(400, "text/plain", "Invalid server IP or port");
            return;
        }
        if (newSSID.length() > 32 || newPassword.length() > 63) {
            web.send(400, "text/plain", "Invalid Wi-Fi SSID or password length");
            return;
        }
        if (newSSID.length()) {
            if (newSSID != wifiSSID) {
                wifiPassword = newPassword;
            } else if (newPassword.length()) {
                wifiPassword = newPassword;
            }
            wifiSSID = newSSID;
        } else if (newPassword.length()) {
            wifiPassword = newPassword;
        }
        serverIP = newIP;
        serverPort = static_cast<uint16_t>(newPort);

        if (!prefs.begin("climate", false)) {
            web.send(500, "text/plain", "NVS unavailable");
            return;
        }
        size_t saved = 0;
        saved += prefs.putString("ssid", wifiSSID);
        saved += prefs.putString("password", wifiPassword);
        saved += prefs.putString("server_ip", serverIP);
        saved += prefs.putUShort("port", serverPort);
        prefs.end();
        if (!saved) {
            web.send(500, "text/plain", "Could not save settings");
            return;
        }
        Serial.printf("[CONFIG] Server set to %s:%u\n", serverIP.c_str(), serverPort);
        web.send(200, "text/html; charset=utf-8",
                 "<h2>Settings saved</h2><p>ESP32 restarting...</p>");
        restartAt = millis();
        restartPending = true; });
}

void startConfigMode()
{
    if (configMode)
        return;
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(AP_SSID, AP_PASS))
    {
        Serial.println("[CONFIG] ERROR: AP did not start");
        return;
    }
    web.begin();
    configMode = true;
    Serial.printf("[CONFIG] Wi-Fi AP: %s\n", AP_SSID);
    Serial.printf("[CONFIG] Open http://%s\n", WiFi.softAPIP().toString().c_str());
}

void connectWiFi()
{
    if (wifiSSID.isEmpty())
    {
        startConfigMode();
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    lastWiFiRetryAt = millis();
    Serial.printf("[WiFi] Connecting: %s\n", wifiSSID.c_str());
}

// ===== SENSOR / BUTTON =====
void checkButton(uint32_t now)
{
    bool rawPressed = digitalRead(BUTTON_PIN) == LOW;
    if (rawPressed != rawPressedLast)
    {
        rawPressedLast = rawPressed;
        buttonChangedAt = now;
    }
    if (now - buttonChangedAt >= DEBOUNCE_MS && rawPressed != stablePressed)
    {
        stablePressed = rawPressed;
        if (stablePressed)
        {
            buttonDownAt = now;
            holdHandled = false;
            Serial.println("[BUTTON] Pressed");
        }
        else
        {
            Serial.println("[BUTTON] Released");
        }
    }
    if (stablePressed && !holdHandled && now - buttonDownAt >= BUTTON_HOLD_MS)
    {
        holdHandled = true;
        Serial.println("[BUTTON] Long press -> setup portal");
        startConfigMode();
    }
}

void takeMeasurement()
{
    if (!sensorReady)
    {
        Serial.println("[SENSOR] Not initialized");
        return;
    }
    sensors_event_t humidity;
    sensors_event_t temperature;
    if (!aht.getEvent(&humidity, &temperature) ||
        !isfinite(temperature.temperature) ||
        !isfinite(humidity.relative_humidity) ||
        humidity.relative_humidity < 0 || humidity.relative_humidity > 100)
    {
        Serial.println("[SENSOR] Invalid reading; not queued");
        return;
    }
    Serial.printf("[SENSOR] %.2f C | %.2f %%\n",
                  temperature.temperature, humidity.relative_humidity);
    enqueueSample(temperature.temperature, humidity.relative_humidity);
}

// ===== TIME AND NETWORK =====
void checkWiFi()
{
    int status = (int)WiFi.status();
    if (status != previousWiFiStatus)
    {
        previousWiFiStatus = status;
        if (status == WL_CONNECTED)
        {
            Serial.print("[WiFi] CONNECTED. IP: ");
            Serial.println(WiFi.localIP());
            Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
            if (!timeSyncStarted)
            {
                configTime(0, 0, "pool.ntp.org", "time.google.com");
                timeSyncStarted = true;
                Serial.println("[TIME] Waiting for NTP UTC synchronization...");
            }
        }
        else
        {
            Serial.printf("[WiFi] Status: %d\n", status);
        }
    }
    if (WiFi.status() == WL_CONNECTED && !timeSyncLogged &&
        time(nullptr) >= MIN_VALID_TIME)
    {
        timeSyncLogged = true;
        Serial.println("[TIME] UTC clock synchronized");
    }
}

void sendOldestMeasurement()
{
    if (queueState.count == 0 || WiFi.status() != WL_CONNECTED ||
        serverIP.isEmpty())
        return;

    Sample sample = queueState.samples[0];
    String json = String("{\"device_id\":\"") + DEVICE_ID +
                  "\",\"sample_id\":" + String(sample.sampleId) +
                  ",\"temperature\":" + String(sample.temperature, 2) +
                  ",\"humidity\":" + String(sample.humidity, 2) +
                  ",\"measured_at_utc\":" +
                  (sample.measuredAtUtc ? String(sample.measuredAtUtc) : String("null")) +
                  "}";
    String url = "http://" + serverIP + ":" + String(serverPort) +
                 "/api/measurements";

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(4000);
    Serial.printf("[HTTP] POST sample #%lu -> %s\n",
                  (unsigned long)sample.sampleId, url.c_str());
    Serial.println("[HTTP] " + json);
    if (!http.begin(client, url))
    {
        Serial.println("[HTTP] Could not initialize HTTP client");
        return;
    }
    http.addHeader("Content-Type", "application/json");
    int status = http.POST(json);
    if (status == 200)
    {
        Serial.println("[HTTP] SUCCESS 200");
        // Remove only on confirmed server acknowledgement.
        if (!removeOldestSample())
        {
            Serial.println("[QUEUE] WARNING: delivery acknowledged, NVS update failed");
        }
    }
    else if (status > 0)
    {
        Serial.printf("[HTTP] Server status: %d, queue retained\n", status);
        Serial.println(http.getString());
    }
    else
    {
        Serial.printf("[HTTP] Network error %d: %s; queue retained\n",
                      status, http.errorToString(status).c_str());
    }
    http.end();
}

void setup()
{
    Serial.begin(115200);
    delay(700);
    Serial.println("\n=== ESP32 CLIMATE LOGGER / PERSISTENT OUTBOX ===");
    startedAt = millis();
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Wire.begin(SDA_PIN, SCL_PIN);
    sensorReady = aht.begin();
    Serial.println(sensorReady ? "AHT10 initialized OK" : "ERROR: AHT10 not found");

    loadConfig();
    loadQueue();
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info)
                 { Serial.printf("[WiFi] Disconnect reason: %u\n",
                                 info.wifi_sta_disconnected.reason); }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    setupWebServer();
    connectWiFi();
}

void loop()
{
    uint32_t now = millis();
    checkButton(now);
    if (configMode)
        web.handleClient();
    checkWiFi();

    // One initial reading after ~15 seconds; then every 15 minutes.
    if (!firstMeasureDone)
    {
        if (now - startedAt >= FIRST_MEASURE_DELAY_MS)
        {
            firstMeasureDone = true;
            lastMeasurementAt = now;
            takeMeasurement();
        }
    }
    else if (now - lastMeasurementAt >= MEASURE_INTERVAL_MS)
    {
        lastMeasurementAt += MEASURE_INTERVAL_MS;
        takeMeasurement();
    }

    if (!configMode && wifiSSID.length() && WiFi.status() != WL_CONNECTED &&
        now - lastWiFiRetryAt >= WIFI_RETRY_MS)
    {
        Serial.println("[WiFi] Reconnecting...");
        WiFi.reconnect();
        lastWiFiRetryAt = now;
    }

    // Queue is preserved during setup and while network/server is unavailable.
    if (!configMode && WiFi.status() == WL_CONNECTED &&
        queueState.count > 0 && serverIP.length() > 0)
    {
        uint32_t retryInterval = (lastHttpTryAt == 0) ? 0 : HTTP_RETRY_MS;
        if (now - lastHttpTryAt >= retryInterval)
        {
            uint16_t oldCount = queueState.count;
            sendOldestMeasurement();
            lastHttpTryAt = millis();
            if (queueState.count < oldCount)
            {
                // Next queued item can be sent soon, avoiding 60 s per backlog item.
                lastHttpTryAt = millis() - (HTTP_RETRY_MS - HTTP_DRAIN_MS);
            }
        }
    }

    if (restartPending && now - restartAt >= 1500UL)
    {
        Serial.flush();
        ESP.restart();
    }
    delay(10);
}
