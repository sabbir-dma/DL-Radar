#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "sensors.h"

#define CONFIG_FILE "/config.json"

// ============================================================
// Shared globals
// ============================================================
extern HardwareSerial RS485;
extern JsonDocument  config;

// ============================================================
// Low-level helpers (unchanged)
// ============================================================
uint16_t modbusCRC(uint8_t *data, uint16_t length);
uint16_t parseAddress(const char *address);
uint32_t getSerialConfig(const char *parity, int dataBits, int stopBits);

void rs485TxMode();
void rs485RxMode();

bool loadConfig();
void printConfig();
void configureRS485(JsonObject modbus);

bool readRegisters(uint8_t slaveId,
                   uint8_t functionCode,
                   uint16_t address,
                   uint16_t quantity,
                   uint16_t *registers,
                   uint16_t timeout);

bool decodeRegisters(const uint16_t *src,
                     uint16_t width,
                     const char *dataType,
                     const char *wordOrder,
                     bool byteSwap,
                     float &result);

bool readChannel(JsonObject channel, JsonObject modbus, float &value);

// ============================================================
// Protocol entrypoint — called by dispatcher
// ============================================================
bool readModbusSensor(JsonObject sensor, ChannelWriter &w);