
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define AP_SSID     "DL301-26"
#define AP_PASSWORD "12345678"

#define MAX_SENSORS     12
#define MAX_CHANNELS    12
#define DEFAULT_CONFIG_VERSION 3

bool softAP_started = false;


WebServer server(80);
JsonDocument config;



void createDefaultConfig()
{
    config.clear();

    config["config_version"] = DEFAULT_CONFIG_VERSION;

    JsonObject device = config["device"].to<JsonObject>();
    device["id"]   = "DL002";
    device["name"] = "Industrial Data Logger";

    JsonObject net = config["network"].to<JsonObject>();
    net["apn"]         = "";
    net["apn_user"]    = "";
    net["apn_pass"]    = "";
    net["broker"]      = "";
    net["broker_port"] = 1883;
    net["mqtt_user"]   = "";
    net["mqtt_pass"]   = "";
    net["pub_topic"]   = "";
    net["sub_topic"]   = "";
    net["ack_topic"]   = "";

    config["sensors"].to<JsonArray>();
}

// ---------- Persistence ----------

bool saveConfig()
{
    File file = LittleFS.open(CONFIG_FILE, "w");
    if (!file) {
        Serial.println("ERROR: Cannot open config.json for writing.");
        return false;
    }
    serializeJsonPretty(config, file);
    file.close();
    Serial.println("Configuration saved.");
    Serial.println("Device Restarting ...");
    vTaskDelay(pdMS_TO_TICKS(1200));
    ESP.restart();
    return true;
}


// ---------- HTTP helpers ----------

void sendJson(int code, JsonDocument &doc)
{
    String out;
    serializeJson(doc, out);
    server.send(code, "application/json", out);
}

void sendMessage(int code, bool ok, const char *message)
{
    JsonDocument r;
    r["ok"] = ok;
    r["message"] = message;
    sendJson(code, r);
}

// ---------- Validation ----------

bool validateConfig(JsonDocument &doc, String &error)
{
    if (!doc["device"].is<JsonObject>())  { error = "Missing device object";  return false; }
    if (!doc["network"].is<JsonObject>()) { error = "Missing network object"; return false; }
    if (!doc["sensors"].is<JsonArray>())  { error = "Missing sensors array";  return false; }

    JsonArray sensors = doc["sensors"].as<JsonArray>();
    if (sensors.size() > MAX_SENSORS) {
        error = "Maximum 12 sensors allowed";
        return false;
    }

    for (JsonObject s : sensors) {
        if (!s["channels"].is<JsonArray>()) {
            error = "Each sensor must contain channels[]";
            return false;
        }
        if (s["channels"].as<JsonArray>().size() > MAX_CHANNELS) {
            error = "Maximum 12 channels per sensor";
            return false;
        }
    }
    return true;
}

// ---------- API handlers ----------

void handleGetConfig()
{
    sendJson(200, config);
}

void handleSetConfig()
{
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendMessage(400, false, "Empty request body");
        return;
    }

    JsonDocument incoming;
    DeserializationError err = deserializeJson(incoming, body);
    if (err) {
        Serial.print("Invalid JSON: ");
        Serial.println(err.c_str());
        sendMessage(400, false, "Invalid JSON");
        return;
    }

    String error;
    if (!validateConfig(incoming, error)) {
        sendMessage(400, false, error.c_str());
        return;
    }

    // Force device.id to stay unchanged (read-only in UI).
    if (config["device"]["id"].is<const char*>()) {
        incoming["device"]["id"] = config["device"]["id"];
    }
    incoming["config_version"] = DEFAULT_CONFIG_VERSION;

    config.clear();
    config.set(incoming);

    if (!saveConfig()) {
        sendMessage(500, false, "Failed to save configuration");
        return;
    }

    printConfig();
    sendMessage(200, true, "Configuration saved");
}

void handleResetConfig()
{
    createDefaultConfig();
    if (saveConfig()) {
        printConfig();
        sendMessage(200, true, "Configuration reset");
    } else {
        sendMessage(500, false, "Failed to reset configuration");
    }
}

void handleNotFound()
{
    server.send(404, "text/plain", "Not found");
}

// ---------- Static files ----------

void serveFile(const char *path, const char *mime)
{
    File file = LittleFS.open(path, "r");
    if (!file) {
        server.send(404, "text/plain", String(path) + " not found");
        return;
    }
    server.streamFile(file, mime);
    file.close();
}

void setupWebServer()
{
    server.on("/",          HTTP_GET, [](){ serveFile("/index.html", "text/html"); });
    server.on("/style.css", HTTP_GET, [](){ serveFile("/style.css",  "text/css"); });
    server.on("/script.js", HTTP_GET, [](){ serveFile("/script.js",  "application/javascript"); });

    server.on("/api/config",       HTTP_GET,  handleGetConfig);
    server.on("/api/config",       HTTP_POST, handleSetConfig);
    server.on("/api/config/reset", HTTP_POST, handleResetConfig);

    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("HTTP server started.");
}

