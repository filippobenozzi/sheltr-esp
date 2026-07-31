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

namespace {

// "AA01", "AA1", "AA12,altri,parametri" -> numero della lista scenari.
// Il numero è decimale, a una o più cifre, e sta all'inizio del messaggio;
// quello che segue (azione, parametri) per ora non viene interpretato.
bool parseAsciiScenario(const uint8_t *data, size_t length, uint16_t &scenario) {
  size_t index = 0;
  while (index < length && (data[index] == ' ' || data[index] == '\t' || data[index] < 0x20)) {
    index++;
  }
  if (index + 2 >= length) return false;
  if (toupper(data[index]) != 'A' || toupper(data[index + 1]) != 'A') return false;
  index += 2;

  uint32_t value = 0;
  size_t digits = 0;
  while (index < length && isdigit(data[index]) && digits < 3) {
    value = value * 10 + (data[index] - '0');
    index++;
    digits++;
  }
  if (digits == 0 || value == 0 || value > 255) return false;
  scenario = static_cast<uint16_t>(value);
  return true;
}

}  // namespace

void SheltrBus::listen() {
  if (serial_ == nullptr || trigger_ == nullptr) return;
  // Solo quando il bus è libero: durante una transazione i byte appartengono alla risposta.
  if (mutex_ != nullptr && xSemaphoreTake(mutex_, 0) != pdTRUE) return;

  const uint32_t now = millis();
  while (serial_->available() > 0) {
    const int value = serial_->read();
    if (value < 0) break;
    if (listenUsed_ >= sizeof(listenBuffer_)) {
      memmove(listenBuffer_, listenBuffer_ + 16, listenUsed_ - 16);
      listenUsed_ -= 16;
    }
    listenBuffer_[listenUsed_++] = static_cast<uint8_t>(value);
    listenLastByteAt_ = now;
  }

  uint16_t triggers[4];
  size_t triggerCount = 0;

  // 1) Frame protocollo 0xAA "Attivazione liste Scenari": G1 = numero lista.
  uint8_t candidate[protocol::FRAME_LEN];
  while (triggerCount < 4 && protocol::extractBinary(listenBuffer_, listenUsed_, candidate)) {
    protocol::Frame frame;
    if (protocol::parse(candidate, frame) && frame.command == 0xAA && frame.g[0] > 0) {
      triggers[triggerCount++] = frame.g[0];
    }
    listenUsed_ = 0;  // il frame è stato consumato
  }

  // 2) Messaggi testuali: si elaborano a fine riga oppure dopo una pausa sul bus,
  //    perché "AA1" non ha una lunghezza fissa.
  while (triggerCount < 4 && listenUsed_ > 0) {
    size_t terminator = listenUsed_;
    for (size_t i = 0; i < listenUsed_; i++) {
      if (listenBuffer_[i] == '\r' || listenBuffer_[i] == '\n') {
        terminator = i;
        break;
      }
    }

    const bool hasLine = terminator < listenUsed_;
    const bool idle = (now - listenLastByteAt_) > 150;
    if (!hasLine && !idle) break;  // messaggio ancora in arrivo

    uint16_t scenario = 0;
    if (parseAsciiScenario(listenBuffer_, terminator, scenario)) {
      triggers[triggerCount++] = scenario;
    }

    if (hasLine) {
      size_t consumed = terminator + 1;
      while (consumed < listenUsed_ &&
             (listenBuffer_[consumed] == '\r' || listenBuffer_[consumed] == '\n')) {
        consumed++;
      }
      memmove(listenBuffer_, listenBuffer_ + consumed, listenUsed_ - consumed);
      listenUsed_ -= consumed;
    } else {
      listenUsed_ = 0;
    }
  }

  if (mutex_ != nullptr) xSemaphoreGive(mutex_);

  for (size_t i = 0; i < triggerCount; i++) {
    triggers_++;
    lastTrigger_ = triggers[i];
    log_i("Comando scenario dal bus: AA%02u", triggers[i]);
    trigger_(triggers[i]);
  }
}

SheltrBus::Result SheltrBus::poll(uint8_t address) {
  return send(address, protocol::CMD_POLL, nullptr, 0, true,
              [address](const protocol::Frame &frame) {
                return frame.address == address && frame.command == protocol::CMD_POLL;
              });
}
