#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>

#include <functional>

#include "protocol.h"

// Trasporto seriale del protocollo 1.6.
//
// Il bus è half-duplex e condiviso da interfaccia web, MQTT e profili orari:
// ogni transazione (invio + attesa risposta) è protetta da un mutex, esattamente
// come fa il gateway Python su /dev/ttyS0.
class SheltrBus {
 public:
  struct Result {
    bool ok = false;
    bool hasFrame = false;
    protocol::Frame frame;
    String error;
    String requestHex;
    String responseHex;
  };

  using Validator = std::function<bool(const protocol::Frame &)>;

  void begin(int8_t rxPin, int8_t txPin, int8_t dePin, uint32_t baud, uint32_t timeoutMs);
  void reconfigure(int8_t rxPin, int8_t txPin, int8_t dePin, uint32_t baud, uint32_t timeoutMs);

  // Invia un frame costruito da indirizzo/comando/G-bytes.
  Result send(uint8_t address, uint8_t command, const uint8_t *g, size_t gLen,
              bool waitResponse = true, Validator validator = nullptr);

  // Invia un frame già pronto (usato dal bridge Sheltr Cloud, che inoltra i byte verbatim).
  Result sendRaw(const uint8_t *raw, bool waitResponse = true, Validator validator = nullptr);

  // Polling 0x40 di una scheda.
  Result poll(uint8_t address);

  uint32_t sentCount() const { return sent_; }
  uint32_t okCount() const { return ok_; }
  uint32_t errorCount() const { return errors_; }
  uint32_t baud() const { return baud_; }
  int8_t rxPin() const { return rxPin_; }
  int8_t txPin() const { return txPin_; }
  int8_t dePin() const { return dePin_; }
  uint32_t timeoutMs() const { return timeoutMs_; }
  const String &lastError() const { return lastError_; }

 private:
  Result transact(const uint8_t *raw, bool waitResponse, Validator validator);

  HardwareSerial *serial_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  int8_t rxPin_ = -1;
  int8_t txPin_ = -1;
  int8_t dePin_ = -1;
  uint32_t baud_ = 9600;
  uint32_t timeoutMs_ = 1200;
  uint32_t sent_ = 0;
  uint32_t ok_ = 0;
  uint32_t errors_ = 0;
  String lastError_;
};

extern SheltrBus Bus;
