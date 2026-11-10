
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <Update.h>
#include "display.h"
#include "storage.h"
#include "radar.h"

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

// bool saveConfig()
// {
//     File file = LittleFS.open(CONFIG_FILE, "w");
//     if (!file) {
//         Serial.println("ERROR: Cannot open config.json for writing.");
//         return false;
//     }
//     serializeJsonPretty(config, file);
//     file.close();
//     Serial.println("Configuration saved.");
//     Serial.println("Device Restarting ...");
//     vTaskDelay(pdMS_TO_TICKS(1200));
//     ESP.restart();
//     return true;
// }

bool writeConfigFile() {          // write only, no restart
    File file = LittleFS.open(CONFIG_FILE, "w");
    if (!file) {
        Serial.println("ERROR: Cannot open config.json for writing.");
        return false;
    }
    serializeJsonPretty(config, file);
    file.close();
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


// ---------- Date helpers ----------

bool parseDate(const String& s, int& y, int& m, int& d) {
    if (s.length() != 10 || s[4] != '-' || s[7] != '-') return false;
    y = s.substring(0, 4).toInt();
    m = s.substring(5, 7).toInt();
    d = s.substring(8, 10).toInt();
    return (y >= 2000 && m >= 1 && m <= 12 && d >= 1 && d <= 31);
}

// ---------- Fast SD range scan ----------
// Log rows look like "YYMMDDHHMMSS,device,...". Timestamps are fixed-width
// digits, so they compare correctly as plain strings (no mktime / String).
// Month files (/YYYYMM.csv) are time-ordered, so a scan stops at the first
// row past the end of the range.

#define TS_LEN 12

// "YYYY-MM-DD" -> "YYMMDD" + suffix ("000000" / "235959")
static bool dateToCompact(const String& d, const char* suffix, char* out) {
    int y, m, dd;
    if (!parseDate(d, y, m, dd) || y < 2000 || y > 2099) return false;
    snprintf(out, TS_LEN + 1, "%02d%02d%02d%s", y - 2000, m, dd, suffix);
    return true;
}

static bool validTs(const char* l, size_t n) {
    if (n < TS_LEN) return false;
    for (int i = 0; i < TS_LEN; i++)
        if (l[i] < '0' || l[i] > '9') return false;
    return true;
}

// Block reader: one SD read per 2 KB, lines split with memchr.
static char g_blk[2048];
struct LineReader {
    File& f;
    size_t pos = 0, len = 0;
    explicit LineReader(File& file) : f(file) {}
    bool next(char* line, size_t cap, size_t& n) {
        n = 0;
        bool got = false;
        for (;;) {
            if (pos >= len) {
                len = f.read((uint8_t*)g_blk, sizeof(g_blk));
                pos = 0;
                if (len == 0) break;                 // EOF
            }
            char* start = g_blk + pos;
            char* nl = (char*)memchr(start, '\n', len - pos);
            size_t take = nl ? (size_t)(nl - start) : (len - pos);
            size_t room = cap - 1 - n;
            size_t cp = take < room ? take : room;
            memcpy(line + n, start, cp);
            n += cp;
            pos += take;
            if (take) got = true;
            if (nl) { pos++; got = true; break; }
        }
        line[n] = 0;
        if (n && line[n - 1] == '\r') line[--n] = 0;
        return got;
    }
};

// Is column 11 (sensor_name) equal to `want`?  Handles ',' or '\t' rows.
static bool sensorMatches(const char* line, const String& want) {
    char d = strchr(line, '\t') ? '\t' : ',';
    int col = 0;
    const char* p = line;
    while (*p && col < 11) { if (*p == d) col++; p++; }
    if (col != 11) return false;
    size_t wl = want.length();
    return strncmp(p, want.c_str(), wl) == 0 && (p[wl] == d || p[wl] == 0);
}

// Walks every record in [lo, hi] across the month files.
// onHeader(line) is called once for the first header found,
// onRow(line, len) for each match; return false from onRow to stop.
template <typename FH, typename FR>
static void scanRange(const char* lo, const char* hi, const String& sensor,
                      FH onHeader, FR onRow) {
    int y  = 2000 + (lo[0] - '0') * 10 + (lo[1] - '0');
    int m  = (lo[2] - '0') * 10 + (lo[3] - '0');
    int yE = 2000 + (hi[0] - '0') * 10 + (hi[1] - '0');
    int mE = (hi[2] - '0') * 10 + (hi[3] - '0');

    static char line[512];
    bool headerDone = false, stop = false;

    for (; !stop && (y < yE || (y == yE && m <= mE)); ) {
        char path[16];
        snprintf(path, sizeof(path), "/%04d%02d.csv", y, m);
        if (SD.exists(path)) {
            File f = SD.open(path, FILE_READ);
            if (!f) {
                Serial.printf("[DATA] open failed: %s\n", path);
            } else {
                LineReader rd(f);
                size_t n;
                while (rd.next(line, sizeof(line), n)) {
                    if (n == 0) continue;
                    if (!validTs(line, n)) {
                        if (!headerDone && strncmp(line, "timestamp", 9) == 0) {
                            onHeader(line);
                            headerDone = true;
                        }
                        continue;
                    }
                    if (strncmp(line, lo, TS_LEN) < 0) continue;
                    if (strncmp(line, hi, TS_LEN) > 0) break;     // ordered file
                    if (sensor.length() && !sensorMatches(line, sensor)) continue;
                    if (!onRow(line, n)) { stop = true; break; }
                }
                f.close();
            }
        }
        if (++m > 12) { m = 1; y++; }
        yield();
    }
}

// Row -> JSON array; delimiter is ',' (logger) or '\t'.
static void appendJsonRow(String& out, const char* line) {
    char d = strchr(line, '\t') ? '\t' : ',';
    out += '[';
    out += '"';
    for (const char* p = line; *p; p++) {
        if (*p == d)            { out += "\",\""; }
        else if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
        else                    out += *p;
    }
    out += "\"]";
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

    normalizeSensors(incoming);

    // Force device.id to stay unchanged (read-only in UI).
    if (config["device"]["id"].is<const char*>()) {
        incoming["device"]["id"] = config["device"]["id"];
    }
    incoming["config_version"] = DEFAULT_CONFIG_VERSION;

    config.clear();
    config.set(incoming);

    // if (!saveConfig()) {
    //     sendMessage(500, false, "Failed to save configuration");
    //     return;
    // }
    if (!writeConfigFile()) {
        sendMessage(500, false, "Failed to save configuration");
        return;
    }
    printConfig();
    sendMessage(200, true, "Configuration saved - rebooting");
    delay(300);                       // flush the response
    ESP.restart();

    printConfig();
    sendMessage(200, true, "Configuration saved");
}

void handleResetConfig()
{
    createDefaultConfig();
    if (writeConfigFile()) {
        printConfig();
        sendMessage(200, true, "Configuration reset");
    } else {
        sendMessage(500, false, "Failed to reset configuration");
    }
}

void handleFiles() {
    if (!storage_sd_ready()) {
        server.send(503, "application/json", "{\"error\":\"SD not mounted\"}");
        return;
    }
    String json = "{\"files\":[";
    File root = SD.open("/");
    bool first = true;
    if (root) {
        File f = root.openNextFile();
        while (f) {
            String name = f.name();
            if (name.startsWith("/")) name = name.substring(1);
            if (name.endsWith(".csv") && !name.startsWith("retrieve")) {
                if (!first) json += ",";
                json += "{\"name\":\"" + name + "\",\"size\":" +
                        String((unsigned long)f.size()) + "}";
                first = false;
            }
            f.close();
            f = root.openNextFile();
        }
        root.close();
    }
    json += "]}";
    server.send(200, "application/json", json);
}

static const char* CSV_HEADER =
    "timestamp,device_id,AOS,LOS,csq,fwv,hwv,heap,uptime,"
    "solar_voltage,battery_voltage,sensor_name,raw_data";

// Parses from/to/sensor args. Returns false (after replying) on error.
static bool parseRangeArgs(char* lo, char* hi, String& sensor) {
    if (!storage_sd_ready()) {
        server.send(503, "application/json", "{\"error\":\"SD not mounted\"}");
        return false;
    }
    if (!server.hasArg("from") || !server.hasArg("to") ||
        !dateToCompact(server.arg("from"), "000000", lo) ||
        !dateToCompact(server.arg("to"),   "235959", hi) ||
        strncmp(lo, hi, TS_LEN) > 0) {
        server.send(400, "application/json", "{\"error\":\"bad date range\"}");
        return false;
    }
    sensor = server.arg("sensor");
    return true;
}

// Preview: small, capped, sent in one response (known length, no chunking).
void handleData() {
    char lo[TS_LEN + 1], hi[TS_LEN + 1];
    String sensor;
    if (!parseRangeArgs(lo, hi, sensor)) return;

    int limit = server.hasArg("limit") ? server.arg("limit").toInt() : 200;
    if (limit < 1 || limit > 500) limit = 500;
    bool asJson = (server.arg("format") != "csv");

    String out;
    out.reserve(limit * 110 + 300);
    int rows = 0;
    String header;

    if (asJson) {
        out = "{\"headers\":[\"timestamp\",\"device_id\",\"AOS\",\"LOS\",\"csq\","
              "\"fwv\",\"hwv\",\"heap\",\"uptime\",\"solar_voltage\","
              "\"battery_voltage\",\"sensor_name\",\"raw_data\"],\"rows\":[";
    }
    scanRange(lo, hi, sensor,
        [&](const char* h) { header = h; },
        [&](const char* l, size_t) {
            if (asJson) { if (rows) out += ','; appendJsonRow(out, l); }
            else        { out += l; out += "\r\n"; }
            return ++rows < limit;
        });

    if (asJson) out += "]}";
    else        out = (header.length() ? header : String(CSV_HEADER)) + "\r\n" + out;

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, asJson ? "application/json" : "text/csv", out);
}

// ---- Download: filter on-device into a temp file, then stream the file ----
static const char* RETRIEVE_PATH = "/retrieve.csv";
static String retrieveName;

void handleRetrieve() {
    char lo[TS_LEN + 1], hi[TS_LEN + 1];
    String sensor;
    if (!parseRangeArgs(lo, hi, sensor)) return;

    if (SD.exists(RETRIEVE_PATH)) SD.remove(RETRIEVE_PATH);
    File out = SD.open(RETRIEVE_PATH, FILE_WRITE);
    if (!out) {
        server.send(500, "application/json", "{\"ok\":false,\"error\":\"Cannot create temp file\"}");
        return;
    }

    static char obuf[4096];
    size_t olen = 0;
    uint32_t records = 0, bytes = 0;
    bool werr = false;
    auto put = [&](const char* d, size_t n) {
        if (olen + n > sizeof(obuf)) {
            if (out.write((const uint8_t*)obuf, olen) != olen) werr = true;
            olen = 0;
        }
        if (n > sizeof(obuf)) { if (out.write((const uint8_t*)d, n) != n) werr = true; return; }
        memcpy(obuf + olen, d, n);
        olen += n;
    };

    uint32_t t0 = millis();
    String header;
    scanRange(lo, hi, sensor,
        [&](const char* h) { header = h; },
        [&](const char* l, size_t n) {
            if (!records && !bytes) {   // write header before first row
                const char* h = header.length() ? header.c_str() : CSV_HEADER;
                put(h, strlen(h)); put("\r\n", 2);
                bytes += strlen(h) + 2;
            }
            put(l, n); put("\r\n", 2);
            bytes += n + 2;
            records++;
            return !werr;
        });
    if (olen) out.write((const uint8_t*)obuf, olen);
    out.close();
    Serial.printf("[DATA] retrieve: %u rows, %u bytes, %u ms\n",
                  (unsigned)records, (unsigned)bytes, (unsigned)(millis() - t0));

    if (werr) {
        SD.remove(RETRIEVE_PATH);
        server.send(500, "application/json", "{\"ok\":false,\"error\":\"SD write failed\"}");
        return;
    }
    if (records == 0) {
        SD.remove(RETRIEVE_PATH);
        server.send(200, "application/json",
                    "{\"ok\":true,\"records\":0,\"bytes\":0}");
        return;
    }
    retrieveName = "data_" + server.arg("from") + "_to_" + server.arg("to") + ".csv";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json",
        "{\"ok\":true,\"records\":" + String(records) + ",\"bytes\":" + String(bytes) +
        ",\"filename\":\"" + retrieveName + "\",\"downloadUrl\":\"/api/retrieve/download\"}");
}

void handleRetrieveDownload() {
    if (!storage_sd_ready() || !SD.exists(RETRIEVE_PATH)) {
        server.send(404, "text/plain", "No retrieve file ready. Run Retrieve first.");
        return;
    }
    File f = SD.open(RETRIEVE_PATH, FILE_READ);
    if (!f) {
        server.send(500, "text/plain", "Cannot open retrieve file");
        return;
    }
    server.sendHeader("Content-Disposition",
        "attachment; filename=\"" + (retrieveName.length() ? retrieveName : String("data.csv")) + "\"");
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(f, "text/csv");      // known length, fast
    f.close();
    SD.remove(RETRIEVE_PATH);
}



// ---------- Stage height (stored in the radar sensor) ----------

void handleGetStage() {
    JsonDocument doc;
    bool ok = radarRefreshStageHeight();
    float v = radarStageHeight();
    doc["ok"] = ok;
    if (!isnan(v)) doc["stage"] = v;
    else           doc["message"] = "Could not read stage height from sensor";
    sendJson(ok ? 200 : 503, doc);
}

void handleSetStage() {
    JsonDocument in;
    if (deserializeJson(in, server.arg("plain")) || !in["stage"].is<float>()) {
        sendMessage(400, false, "Invalid request");
        return;
    }
    float h = in["stage"].as<float>();
    if (h < 1.0f || h > 1000.0f) {
        sendMessage(400, false, "Stage height must be 1 - 1000");
        return;
    }
    if (!setStageHeight(h)) {
        sendMessage(502, false, "Sensor did not accept the stage height");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["message"] = "Stage height updated";
    doc["stage"] = radarStageHeight();      // value read back from the sensor
    sendJson(200, doc);
}


// ---------- Firmware upload (full image, from the portal) ----------
// Upload build output "firmware.bin" (app image). Device reboots when done.

static String fwErr;
static bool   fwOk = false;
static bool   fwFirst = true;
static int    fwLastPct = -1;

void handleFirmwareUpload() {
    HTTPUpload &up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
        fwErr = ""; fwOk = false; fwFirst = true; fwLastPct = -1;
        Serial.printf("[FW] upload start: %s\n", up.filename.c_str());
        if (!up.filename.endsWith(".bin")) { fwErr = "Select a .bin firmware file"; return; }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { fwErr = Update.errorString(); return; }
        updateOtaProgress(0);

    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (fwErr.length()) return;
        if (fwFirst) {
            fwFirst = false;
            if (up.buf[0] != 0xE9) {                 // ESP32 image magic byte
                fwErr = "Not an ESP32 firmware image";
                Update.abort();
                return;
            }
        }
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            fwErr = Update.errorString();
            Update.abort();
            return;
        }
        int total = server.clientContentLength();
        if (total > 0) {
            int pct = (int)((100ULL * (up.totalSize + up.currentSize)) / total);
            if (pct > 99) pct = 99;
            if (pct != fwLastPct) {
                fwLastPct = pct;
                updateOtaProgress(pct);
                if (pct % 10 == 0) Serial.printf("[FW] %d%%\n", pct);
            }
        }

    } else if (up.status == UPLOAD_FILE_END) {
        if (fwErr.length()) return;
        if (Update.end(true)) { fwOk = true; updateOtaProgress(100); Serial.printf("[FW] done, %u bytes\n", (unsigned)up.totalSize); }
        else                  fwErr = Update.errorString();

    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        fwErr = "Upload aborted";
    }
}

void handleFirmwareDone() {
    if (fwOk) {
        sendMessage(200, true, "Firmware installed - rebooting");
        delay(800);
        ESP.restart();
    } else {
        updateOtaProgress(-1);
        sendMessage(500, false, fwErr.length() ? fwErr.c_str() : "Upload failed");
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

// ---------- SD card handlers ----------

void handleSDFiles() {
    if (!storage_sd_ready()) {
        sendMessage(503, false, "SD card not ready");
        return;
    }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    File root = SD.open("/");
    if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
            if (!f.isDirectory()) {
                JsonObject o = arr.add<JsonObject>();
                o["name"] = String(f.name());
                o["size"] = f.size();
            }
            f.close();
            f = root.openNextFile();
        }
    }
    if (root) root.close();
    sendJson(200, doc);
}

void handleSDDownload() {
    if (!server.hasArg("file")) {
        sendMessage(400, false, "Missing file parameter");
        return;
    }
    if (!storage_sd_ready()) {
        sendMessage(503, false, "SD card not ready");
        return;
    }
    String name = server.arg("file");
    if (!name.startsWith("/")) name = "/" + name;

    if (!SD.exists(name.c_str())) {
        sendMessage(404, false, "File not found");
        return;
    }
    File f = SD.open(name.c_str(), FILE_READ);
    if (!f) {
        sendMessage(500, false, "Cannot open file");
        return;
    }

    const char *mime = "application/octet-stream";
    if (name.endsWith(".csv") || name.endsWith(".txt")) mime = "text/csv";
    else if (name.endsWith(".json")) mime = "application/json";
    else if (name.endsWith(".html")) mime = "text/html";

    server.sendHeader("Content-Disposition", "attachment; filename=\"" + name.substring(name.lastIndexOf('/') + 1) + "\"");
    server.streamFile(f, mime);
    f.close();
}

void handleSDDelete() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendMessage(400, false, "Empty request body");
        return;
    }
    JsonDocument incoming;
    DeserializationError err = deserializeJson(incoming, body);
    if (err) {
        sendMessage(400, false, "Invalid JSON");
        return;
    }
    const char *fname = incoming["file"] | "";
    if (strlen(fname) == 0) {
        sendMessage(400, false, "Missing file");
        return;
    }
    if (!storage_sd_ready()) {
        sendMessage(503, false, "SD card not ready");
        return;
    }
    String name = String(fname);
    if (!name.startsWith("/")) name = "/" + name;

    if (!SD.exists(name.c_str())) {
        sendMessage(404, false, "File not found");
        return;
    }
    if (SD.remove(name.c_str())) {
        sendMessage(200, true, "File deleted");
    } else {
        sendMessage(500, false, "Delete failed");
    }
}

void handleSDStatus() {
    JsonDocument doc;
    doc["ready"] = storage_sd_ready();
    sendJson(200, doc);
}
void setupWebServer()
{
    server.on("/",          HTTP_GET, [](){ serveFile("/index.html", "text/html"); });
    server.on("/style.css", HTTP_GET, [](){ serveFile("/style.css",  "text/css"); });
    server.on("/script.js", HTTP_GET, [](){ serveFile("/script.js",  "application/javascript"); });

    server.on("/api/config",       HTTP_GET,  handleGetConfig);
    server.on("/api/config",       HTTP_POST, handleSetConfig);
    server.on("/api/config/reset", HTTP_POST, handleResetConfig);
    server.on("/api/update",       HTTP_POST, handleFirmwareDone, handleFirmwareUpload);
    server.on("/api/stage",        HTTP_GET,  handleGetStage);
    server.on("/api/stage",        HTTP_POST, handleSetStage);
    server.on("/api/files",        HTTP_GET,  handleFiles);
    server.on("/api/data",         HTTP_GET,  handleData);
    server.on("/api/retrieve",          HTTP_GET, handleRetrieve);
    server.on("/api/retrieve/download", HTTP_GET, handleRetrieveDownload);

    server.on("/api/sd/files",   HTTP_GET,  handleSDFiles);
    server.on("/api/sd/download", HTTP_GET, handleSDDownload);
    server.on("/api/sd/delete",   HTTP_POST, handleSDDelete);
    server.on("/api/sd/status",   HTTP_GET,  handleSDStatus);

    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("HTTP server started.");
}

