#include "bus.h"

SheltrBus Bus;

void SheltrBus::begin(int8_t rxPin, int8_t txPin, int8_t dePin, uint32_t baud, uint32_t timeoutMs) {
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
  if (serial_ == nullptr) serial_ = new HardwareSerial(1);
  reconfigure(rxPin, txPin, dePin, baud, timeoutMs);
}

void SheltrBus::reconfigure(int8_t rxPin, int8_t txPin, int8_t dePin, uint32_t baud,
                            uint32_t timeoutMs) {
  if (serial_ == nullptr) return;
  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);

  if (dePin_ >= 0 && dePin_ != dePin) {
    pinMode(dePin_, INPUT);
  }
  rxPin_ = rxPin;
  txPin_ = txPin;
  dePin_ = dePin;
  baud_ = baud;
  timeoutMs_ = timeoutMs;

  serial_->end();
  serial_->begin(baud_, SERIAL_8N1, rxPin_, txPin_);
  serial_->setRxBufferSize(512);
  if (dePin_ >= 0) {
    pinMode(dePin_, OUTPUT);
    digitalWrite(dePin_, LOW);  // riposo: ricezione
  }

  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
  log_i("Bus seriale: RX=%d TX=%d DE=%d baud=%u timeout=%ums", rxPin_, txPin_, dePin_, baud_,
        timeoutMs_);
}

SheltrBus::Result SheltrBus::transact(const uint8_t *raw, bool waitResponse, Validator validator) {
  Result result;
  result.requestHex = protocol::toHex(raw);

  if (serial_ == nullptr) {
    result.error = F("Bus seriale non inizializzato");
    lastError_ = result.error;
    errors_++;
    return result;
  }

  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);

  // Scarta eventuali byte rimasti da transazioni precedenti.
  while (serial_->available()) serial_->read();

  if (dePin_ >= 0) {
    digitalWrite(dePin_, HIGH);  // abilita il driver RS485 in trasmissione
    delayMicroseconds(50);
  }
  serial_->write(raw, protocol::FRAME_LEN);
  serial_->flush();
  if (dePin_ >= 0) {
    delayMicroseconds(50);
    digitalWrite(dePin_, LOW);
  }
  sent_++;

  if (!waitResponse) {
    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
    result.ok = true;
    ok_++;
    return result;
  }

  uint8_t buffer[96];
  size_t used = 0;
  const uint32_t deadline = millis() + timeoutMs_;
  bool sawInvalid = false;
  protocol::Frame lastSeen;

  while (static_cast<int32_t>(deadline - millis()) > 0) {
    while (serial_->available() > 0) {
      const int value = serial_->read();
      if (value < 0) break;
      if (used >= sizeof(buffer)) {
        // Buffer pieno: mantiene la coda, il frame utile è sempre in fondo.
        memmove(buffer, buffer + protocol::FRAME_LEN, used - protocol::FRAME_LEN);
        used -= protocol::FRAME_LEN;
      }
      buffer[used++] = static_cast<uint8_t>(value);

      uint8_t candidate[protocol::FRAME_LEN];
      if (!protocol::extractBinary(buffer, used, candidate)) continue;

      protocol::Frame frame;
      if (!protocol::parse(candidate, frame)) continue;

      if (validator == nullptr || validator(frame)) {
        result.ok = true;
        result.hasFrame = true;
        result.frame = frame;
        result.responseHex = protocol::toHex(candidate);
        ok_++;
        if (mutex_ != nullptr) xSemaphoreGive(mutex_);
        return result;
      }
      sawInvalid = true;
      lastSeen = frame;
      used = 0;  // scarta e resta in ascolto
    }
    delay(2);
  }

  if (mutex_ != nullptr) xSemaphoreGive(mutex_);

  errors_++;
  if (sawInvalid) {
    result.hasFrame = true;
    result.frame = lastSeen;
    result.responseHex = protocol::toHex(lastSeen.raw);
    result.error = String(F("Risposta protocollo inattesa: ")) + result.responseHex;
  } else if (used > 0) {
    result.error = F("Risposta protocollo non valida");
  } else {
    result.error = F("Nessuna risposta dal bus");
  }
  lastError_ = result.error;
  return result;
}

SheltrBus::Result SheltrBus::send(uint8_t address, uint8_t command, const uint8_t *g, size_t gLen,
                                  bool waitResponse, Validator validator) {
  uint8_t raw[protocol::FRAME_LEN];
  protocol::build(address, command, g, gLen, raw);
  if (validator == nullptr) {
    validator = [address, command](const protocol::Frame &frame) {
      return frame.address == address && frame.command == command;
    };
  }
  return transact(raw, waitResponse, validator);
}

SheltrBus::Result SheltrBus::sendRaw(const uint8_t *raw, bool waitResponse, Validator validator) {
  if (validator == nullptr) {
    const uint8_t address = raw[1];
    validator = [address](const protocol::Frame &frame) { return frame.address == address; };
  }
  return transact(raw, waitResponse, validator);
}

SheltrBus::Result SheltrBus::poll(uint8_t address) {
  return send(address, protocol::CMD_POLL, nullptr, 0, true,
              [address](const protocol::Frame &frame) {
                return frame.address == address && frame.command == protocol::CMD_POLL;
              });
}
