#include "config.h"     // DEVICE_ID, MQTT_PUB, MQTTMessage, mqttPublishQueue
#include "modbus.h"
#include "timeutil.h"
#include "display.h"

// ============================================================
// Globals
// ============================================================
HardwareSerial RS485(2);
// JsonDocument  config;

// ============================================================
// CRC16 MODBUS
// ============================================================
uint16_t modbusCRC(uint8_t *data, uint16_t length) {
    uint16_t crc = 0xFFFF;
    for (uint16_t pos = 0; pos < length; pos++) {
        crc ^= data[pos];
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else              crc >>= 1;
        }
    }
    return crc;
}

// ============================================================
// RS485 DIRECTION (optional DE/RE pin)
// ============================================================
void rs485TxMode() {
    int pin = config["rs485"]["de_re_pin"] | -1;
    if (pin >= 0) digitalWrite(pin, HIGH);
}

void rs485RxMode() {
    int pin = config["rs485"]["de_re_pin"] | -1;
    if (pin >= 0) digitalWrite(pin, LOW);
}

// ============================================================
// ADDRESS
// ============================================================
uint16_t parseAddress(const char *address) {
    if (address == nullptr) return 0;
    return (uint16_t)strtoul(address, nullptr, 0);
}

// ============================================================
// SERIAL CONFIG
// ============================================================
uint32_t getSerialConfig(const char *parity, int dataBits, int stopBits) {
    char p = 'N';
    if (parity != nullptr) p = toupper(parity[0]);

    if (dataBits == 7) {
        if (stopBits == 2) {
            if (p == 'E') return SERIAL_7E2;
            if (p == 'O') return SERIAL_7O2;
            return SERIAL_7N2;
        }
        if (p == 'E') return SERIAL_7E1;
        if (p == 'O') return SERIAL_7O1;
        return SERIAL_7N1;
    }

    if (stopBits == 2) {
        if (p == 'E') return SERIAL_8E2;
        if (p == 'O') return SERIAL_8O2;
        return SERIAL_8N2;
    }
    if (p == 'E') return SERIAL_8E1;
    if (p == 'O') return SERIAL_8O1;
    return SERIAL_8N1;
}

// ============================================================
// LOAD CONFIG
// ============================================================
bool loadConfig() {
    if (!LittleFS.begin(true)) {
        Serial.println("[CFG] ERROR: LittleFS mount failed");
        return false;
    }
    Serial.println("[CFG] LittleFS mounted");

    // ---- List all files in root ----
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
            Serial.printf("[CFG] file: %s  size=%u\n",
                          f.name(), (unsigned)f.size());
            f = root.openNextFile();
        }
    }

    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("[CFG] ERROR: config.json not found");
        return false;
    }

    File file = LittleFS.open(CONFIG_FILE, "r");
    if (!file) {
        Serial.println("[CFG] ERROR: cannot open config.json");
        return false;
    }

    size_t fileSize = file.size();
    Serial.printf("[CFG] config.json size on disk: %u bytes\n",
                  (unsigned)fileSize);

    // ---- Rewind and parse ----
    file.seek(0);
    DeserializationError error = deserializeJson(config, file);
    file.close();

    if (error) {
        Serial.print("[CFG] ERROR: JSON parse failed: ");
        Serial.println(error.c_str());
        return false;
    }

    // ---- Report parsed size and top-level keys ----
    Serial.printf("[CFG] Parsed: %u bytes, %u top-level entries\n",
                  (unsigned)config.size(),
                  (unsigned)config.as<JsonObject>().size());

    for (JsonPair p : config.as<JsonObject>()) {
        Serial.printf("[CFG] key: '%s'\n", p.key().c_str());
    }

    // ---- Check sensors array specifically ----
    if (!config["sensors"].is<JsonArray>()) {
        Serial.println("[CFG] !!! 'sensors' key missing or not an array !!!");
        return false;
    }

    Serial.printf("[CFG] sensors[] count: %u\n",
                  (unsigned)config["sensors"].as<JsonArray>().size());

    Serial.println("[CFG] Configuration loaded OK");
    return true;
}

void printConfig()
{
    Serial.println("\n========== CONFIGURATION ==========");
    serializeJsonPretty(config, Serial);
    Serial.println("\n===================================");
}
// ============================================================
// CONFIGURE RS485
// ============================================================
void configureRS485(JsonObject modbus) {
    int rxPin   = config["rs485"]["rx_pin"]    | 14;
    int txPin   = config["rs485"]["tx_pin"]    | 13;
    int deRePin = config["rs485"]["de_re_pin"] | -1;

    int baud = modbus["baud"] | 9600;
    const char *parity = modbus["parity"]    | "NONE";
    int dataBits       = modbus["data_bits"] | 8;
    int stopBits       = modbus["stop_bits"] | 1;

    uint32_t serialConfig = getSerialConfig(parity, dataBits, stopBits);

    if (deRePin >= 0) {
        pinMode(deRePin, OUTPUT);
        digitalWrite(deRePin, LOW);
    }

    RS485.end();
    delay(5);
    RS485.begin(baud, serialConfig, rxPin, txPin);
    rs485RxMode();

    Serial.printf("[CFG] RS485: rx=%d tx=%d baud=%d parity=%s\n",
                  rxPin, txPin, baud, parity);
    Serial.flush();
}

// ============================================================
// READ REGISTERS  (with auto-direction timing baked in)
// ============================================================
bool readRegisters(uint8_t slaveId,
                   uint8_t functionCode,
                   uint16_t address,
                   uint16_t quantity,
                   uint16_t *registers,
                   uint16_t timeout) {
    if (quantity == 0 || quantity > 125) return false;

    uint8_t request[8];
    request[0] = slaveId;
    request[1] = functionCode;
    request[2] = (uint8_t)(address >> 8);
    request[3] = (uint8_t)(address & 0xFF);
    request[4] = (uint8_t)(quantity >> 8);
    request[5] = (uint8_t)(quantity & 0xFF);

    uint16_t crc = modbusCRC(request, 6);
    request[6] = crc & 0xFF;
    request[7] = crc >> 8;

    // Flush any stale bytes
    while (RS485.available()) RS485.read();

    // Let auto-direction module settle into RX
    delay(80);

    // Send
    rs485TxMode();
    RS485.write(request, 8);
    RS485.flush();
    delayMicroseconds(100);
    rs485RxMode();

    uint16_t expectedBytes = 3 + (quantity * 2) + 2;

    uint8_t response[260];
    uint16_t index = 0;
    uint32_t start = millis();

    while (index < expectedBytes) {
        if (RS485.available()) {
            response[index++] = RS485.read();
        } else {
            if (millis() - start > timeout) {
                delay(200);
                return false;
            }
            delay(1);
        }
    }

    // Post-read settle
    delay(200);

    // Validate
    if (response[0] != slaveId) return false;

    if (response[1] & 0x80) {
        Serial.printf("[MODBUS] Exception: 0x%02X\n", response[2]);
        return false;
    }
    if (response[1] != functionCode) return false;
    if (response[2] != quantity * 2) return false;

    uint16_t receivedCRC = response[expectedBytes - 2] |
                          ((uint16_t)response[expectedBytes - 1] << 8);
    uint16_t calculatedCRC = modbusCRC(response, expectedBytes - 2);
    if (receivedCRC != calculatedCRC) {
        Serial.println("[MODBUS] CRC error");
        return false;
    }

    for (uint16_t i = 0; i < quantity; i++) {
        registers[i] = ((uint16_t)response[3 + i * 2] << 8) |
                       response[4 + i * 2];
    }
    return true;
}

// ============================================================
// HELPERS: width, combine, byte-swap
// ============================================================
static uint16_t autoWidthForType(const char *dataType) {
    if (strcmp(dataType, "uint32")  == 0 ||
        strcmp(dataType, "int32")   == 0 ||
        strcmp(dataType, "float32") == 0) return 2;
    if (strcmp(dataType, "uint64")  == 0 ||
        strcmp(dataType, "int64")   == 0 ||
        strcmp(dataType, "float64") == 0) return 4;
    return 1;
}

static uint32_t combine32(uint16_t *r, const char *wo) {
    // LITTLE_ENDIAN / LITTLE_ENDIAN_BYTE_SWAP → low word first
    if (wo && (strcasecmp(wo, "low_high") == 0 ||
               strcasecmp(wo, "LITTLE_ENDIAN") == 0 ||
               strcasecmp(wo, "LITTLE_ENDIAN_BYTE_SWAP") == 0))
        return ((uint32_t)r[1] << 16) | r[0];
    return ((uint32_t)r[0] << 16) | r[1];
}

static uint64_t combine64(uint16_t *r, const char *wo) {
    if (wo && (strcasecmp(wo, "low_high") == 0 ||
               strcasecmp(wo, "LITTLE_ENDIAN") == 0 ||
               strcasecmp(wo, "LITTLE_ENDIAN_BYTE_SWAP") == 0))
        return ((uint64_t)r[3] << 48) | ((uint64_t)r[2] << 32) |
               ((uint64_t)r[1] << 16) | r[0];
    return ((uint64_t)r[0] << 48) | ((uint64_t)r[1] << 32) |
            ((uint64_t)r[2] << 16) | r[3];
}

// ============================================================
// DECODE REGISTERS -> FLOAT
// ============================================================
bool decodeRegisters(const uint16_t *src, uint16_t width,
                     const char *dataType, const char *wordOrder,
                        bool byteSwap, float &result) {
    uint16_t regs[4] = {0, 0, 0, 0};
    for (uint16_t i = 0; i < width && i < 4; i++) {
        regs[i] = src[i];
        if (byteSwap) regs[i] = (regs[i] >> 8) | (regs[i] << 8);
    }

    if (strcasecmp(dataType, "uint16") == 0) {
        result = (float)regs[0];
    } else if (strcasecmp(dataType, "int16") == 0) {
        result = (float)(int16_t)regs[0];
    } else if (strcasecmp(dataType, "uint32") == 0) {
        result = (float)combine32(regs, wordOrder);
    } else if (strcasecmp(dataType, "int32") == 0) {
        result = (float)(int32_t)combine32(regs, wordOrder);
    } else if (strcasecmp(dataType, "float32") == 0) {
        uint32_t raw = combine32(regs, wordOrder);
        memcpy(&result, &raw, sizeof(result));
    } else if (strcasecmp(dataType, "uint64") == 0) {
        result = (float)combine64(regs, wordOrder);
    } else if (strcasecmp(dataType, "int64") == 0) {
        result = (float)(int64_t)combine64(regs, wordOrder);
    } else if (strcasecmp(dataType, "float64") == 0) {
        uint64_t raw = combine64(regs, wordOrder);
        double d;
        memcpy(&d, &raw, sizeof(d));
        result = (float)d;
    } else {
        return false;
    }
    return true;
}

// ============================================================
// READ CHANNEL (single-read mode)
// ============================================================
bool readChannel(JsonObject channel, JsonObject modbus, float &value) {
    // new key: "register_address" (was "address")
    const char *addressString = channel["register_address"] | "0x00";
    uint16_t address = parseAddress(addressString);

    float scale  = channel["scale_factor"] | 1.0;
    float offset = channel["offset"]       | 0.0;

    uint8_t  slaveId = modbus["slave_id"] | 1;
    uint16_t timeout = modbus["receive_timeout_ms"] | 300;
    int      retries = modbus["retry_attempts"] | 3;

    // register type: new value is UPPERCASE ("HOLDING", "INPUT", ...)
    const char *registerType = channel["register_type"] | "INPUT";
    uint8_t functionCode = 0x04;
    if (strcasecmp(registerType, "holding") == 0) functionCode = 0x03;

    // data type: new value is UPPERCASE ("INT16", "FLOAT32", ...)
    const char *dataType  = channel["data_type"]  | "UINT16";
    // byte_order replaces word_order + byte_swap
    const char *byteOrder = channel["byte_order"] | "BIG_ENDIAN";
    bool byteSwap = (strcasecmp(byteOrder, "BIG_ENDIAN_BYTE_SWAP") == 0 ||
                        strcasecmp(byteOrder, "LITTLE_ENDIAN_BYTE_SWAP") == 0);

    // UI gives explicit register_width; fall back to auto-width if absent
    uint16_t requiredWidth = autoWidthForType(dataType);
    uint16_t width = channel["register_width"] | requiredWidth;
    if (width < requiredWidth) width = requiredWidth;
    if (width < 1 || width > 4) return false;

    uint16_t registers[4] = {0, 0, 0, 0};

    for (int attempt = 1; attempt <= retries; attempt++) {
        if (readRegisters(slaveId, functionCode, address, width,
                            registers, timeout)) {
            float result = 0;
            if (!decodeRegisters(registers, width, dataType, byteOrder,
                                    byteSwap, result)) {
                Serial.printf("[MODBUS] Unsupported data type: %s\n", dataType);
                return false;
            }
            value = scale * result + offset;
            return true;
        }
        delay(20);
    }
    return false;
}

// ============================================================
// READ MODBUS SENSOR (batch or single-read mode)
// ============================================================
bool readModbusSensor(JsonObject sensor, ChannelWriter &w) {
    JsonObject modbus = sensor["modbus"];

    int slaveId = modbus["slave_id"] | 1;
    int baud = modbus["baud"] | 9600;
    const char *parity = modbus["parity"] | "NONE";

    configureRS485(modbus);

    // ---------------- BATCH MODE ----------------
    bool batchMode = modbus["batch_read"] | false;
    uint16_t batchBuf[64];
    uint16_t batchQty = 0;
    bool batchOk = false;

    if (batchMode) {
        uint16_t batchAddr =
            parseAddress(modbus["batch_start"] | "0x0000");

        batchQty = modbus["batch_qty"] | 0;

        if (batchQty == 0 || batchQty > 64) {
            Serial.println("[MODBUS] Batch: invalid quantity");
            return false;
        }

        const char *batchType =
            modbus["batch_reg_type"] | "HOLDING";

        uint8_t fc = 0x04;
        if (strcasecmp(batchType, "holding") == 0) {
            fc = 0x03;
        }

        uint16_t timeout =
            modbus["receive_timeout_ms"] | 300;
        int retries = modbus["retry_attempts"] | 3;

        for (int attempt = 1;
             attempt <= retries && !batchOk;
             attempt++) {

            batchOk = readRegisters(
                slaveId, fc, batchAddr, batchQty,
                batchBuf, timeout
            );

            if (!batchOk) {
                Serial.printf(
                    "[MODBUS] batch attempt %d failed\n",
                    attempt
                );
                delay(50);
            }
        }

        Serial.printf(
            "[MODBUS] batch: %s\n",
            batchOk ? "OK" : "FAILED"
        );
    }

    // ---------------- CHANNELS ----------------
    JsonArray channels = sensor["channels"];

    for (JsonObject channel : channels) {
        const char *alias = channel["alias"] | "unknown";
        const char *unit = channel["unit"] | "";

        float value = 0;
        bool ok = false;

        if (batchMode) {
            if (batchOk) {
                uint16_t channelAddr = parseAddress(
                    channel["register_address"] | "0x00"
                );

                uint16_t batchStart = parseAddress(
                    modbus["batch_start"] | "0x0000"
                );

                if (channelAddr >= batchStart) {
                    uint16_t regIdx = channelAddr - batchStart;

                    const char *dataType =
                        channel["data_type"] | "UINT16";
                    const char *wordOrder =
                        channel["byte_order"] | "BIG_ENDIAN";

                    float scale =
                        channel["scale_factor"] | 1.0;
                    float offset =
                        channel["offset"] | 0.0;

                    uint16_t width =
                        channel["register_width"] |
                        autoWidthForType(dataType);

                    if (regIdx + width <= batchQty) {
                        float raw = 0;

                        if (decodeRegisters(
                                &batchBuf[regIdx],
                                width, dataType,
                                wordOrder, false, raw)) {
                            value = scale * raw + offset;
                            ok = true;
                        }
                    }
                }
            }
        } else {
            ok = readChannel(channel, modbus, value);
        }

        if (ok) {
            Serial.printf(
                "%-22s : %.3f %s\n", alias, value, unit
            );

            appendChannel(w, alias, value, unit);

            // Update the matching display field.
            if (strcmp(alias, "pressure") == 0) {
                updateAirPressure(value);
            }
            else if (strcmp(alias, "temperature") == 0) {
                updateAirTemperature(value);
            }
            // else if (strcmp(alias, "batteryPercentage") == 0) {
            //     updateBatteryPercentage((int)(value + 0.5f));
            // }
            else if (strcmp(alias, "humidity") == 0) {
                updateAirHumidity(value);
            }
            else if (strcmp(alias, "wind_speed") == 0) {
                updateWindSpeed(value);
            }
            else if (strcmp(alias, "wind_direction") == 0) {
                updateWindDirection(value);
            }
            else if (strcmp(alias, "rain_fall") == 0) {
                updateRainFall(value);
            }
        }
        else {
            Serial.printf("%-22s : ERROR\n", alias);
            appendChannelError(w, alias, unit);

            if (strcmp(alias, "pressure") == 0) {
                updateAirPressure(NAN);
            }
            else if (strcmp(alias, "temperature") == 0) {
                updateAirTemperature(NAN);
            }
            // else if (strcmp(alias, "batteryPercentage") == 0) {
            //     updateBatteryPercentage(-1);
            // }
            else if (strcmp(alias, "humidity") == 0) {
                updateAirHumidity(NAN);
            }
            else if (strcmp(alias, "wind_speed") == 0) {
                updateWindSpeed(NAN);
            }
            else if (strcmp(alias, "wind_direction") == 0) {
                updateWindDirection(NAN);
            }
            else if (strcmp(alias, "rain_fall") == 0) {
                updateRainFall(NAN);
            }
        }
    }

    return w.anyOk;
}

