#include "mailer.h"

#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "net_manager.h"
#include "settings.h"
#include "smtp_session.h"

namespace mailer {

namespace {

struct Message {
  char subject[120];
  char body[400];
  char to[240];
};

QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
uint32_t g_sent = 0;
uint32_t g_failed = 0;
uint32_t g_dropped = 0;
String g_lastError;
uint32_t g_lastSentAt = 0;

constexpr size_t QUEUE_LENGTH = 6;
constexpr uint32_t SMTP_TIMEOUT_MS = 15000;

// Il canale verso il server di posta: qui c'e' solo il trasporto, la sequenza dei
// comandi SMTP sta in smtp_session (che si prova anche sull'host).
class ClientTransport : public smtp::Transport {
 public:
  bool open(const std::string &host, uint16_t port, bool tls) override {
    // Il certificato del server non viene verificato: sul dispositivo non c'e' un
    // archivio di autorita' da tenere aggiornato. Il traffico e' comunque cifrato.
    secure_.setInsecure();
    secure_.setTimeout(SMTP_TIMEOUT_MS / 1000);
    plain_.setTimeout(SMTP_TIMEOUT_MS / 1000);
    client_ = tls ? static_cast<WiFiClient *>(&secure_) : static_cast<WiFiClient *>(&plain_);
    return client_->connect(host.c_str(), port, SMTP_TIMEOUT_MS);
  }

  // STARTTLS vorrebbe dire cifrare una connessione gia' aperta: WiFiClientSecure
  // non sa farlo (non esiste un modo, nell'API standard, di prendere in carico un
  // socket esistente). Meglio dirlo chiaramente che fingere: sul dispositivo si usa
  // SSL/TLS diretto, che i server di posta espongono di solito sulla porta 465.
  bool startTls() override { return false; }

  bool write(const std::string &data) override {
    if (client_ == nullptr) return false;
    return client_->write(reinterpret_cast<const uint8_t *>(data.data()), data.size()) == data.size();
  }

  int readReply(std::string &text) override {
    text.clear();
    if (client_ == nullptr) return 0;
    const uint32_t deadline = millis() + SMTP_TIMEOUT_MS;
    while (millis() < deadline) {
      if (!client_->available()) {
        if (!client_->connected()) return 0;
        delay(10);
        continue;
      }
      const String line = client_->readStringUntil('\n');
      text += line.c_str();
      text += "\n";
      if (line.length() >= 4 && line[3] == '-') continue;  // risposta non finita
      if (line.length() >= 3) return String(line.substring(0, 3)).toInt();
    }
    return 0;
  }

  void close() override {
    if (client_ != nullptr) client_->stop();
    client_ = nullptr;
  }

 private:
  WiFiClient plain_;
  WiFiClientSecure secure_;
  WiFiClient *client_ = nullptr;
};

smtp::Config toSmtpConfig(const cfg::EmailCfg &settings) {
  smtp::Config config;
  config.host = settings.host.c_str();
  config.port = settings.port;
  config.security = settings.security.c_str();
  config.username = settings.username.c_str();
  config.password = settings.password.c_str();
  config.from = settings.from.c_str();
  config.fromName = settings.fromName.c_str();
  return config;
}

bool deliver(const cfg::EmailCfg &settings, const String &subject, const String &body,
             const String &to, String &error) {
  const std::string list = (to.length() ? to : settings.recipients).c_str();
  ClientTransport transport;
  std::string failure;
  const bool ok = smtp::deliver(transport, toSmtpConfig(settings), smtp::splitAddresses(list),
                                std::string(subject.c_str()), std::string(body.c_str()), failure);
  if (!ok) error = failure.c_str();
  return ok;
}

void task(void *) {
  Message message;
  for (;;) {
    if (xQueueReceive(g_queue, &message, portMAX_DELAY) != pdTRUE) continue;
    if (!net::online()) {
      g_failed++;
      g_lastError = F("nessuna connessione a internet");
      continue;
    }
    String error;
    const cfg::EmailCfg settings = cfg::config().email;  // copia: la config puo' cambiare
    if (deliver(settings, String(message.subject), String(message.body), String(message.to), error)) {
      g_sent++;
      g_lastError = "";
      g_lastSentAt = millis();
      log_i("Email inviata: %s", message.subject);
    } else {
      g_failed++;
      g_lastError = error;
      log_w("Invio email fallito: %s", error.c_str());
    }
  }
}

void fill(Message &message, const String &subject, const String &body, const String &to) {
  strncpy(message.subject, subject.c_str(), sizeof(message.subject) - 1);
  message.subject[sizeof(message.subject) - 1] = '\0';
  strncpy(message.body, body.c_str(), sizeof(message.body) - 1);
  message.body[sizeof(message.body) - 1] = '\0';
  strncpy(message.to, to.c_str(), sizeof(message.to) - 1);
  message.to[sizeof(message.to) - 1] = '\0';
}

}  // namespace

void begin() {
  if (g_queue == nullptr) g_queue = xQueueCreate(QUEUE_LENGTH, sizeof(Message));
  if (g_task == nullptr && g_queue != nullptr) {
    // Stack generoso: la sessione TLS di mbedTLS ne consuma parecchio.
    xTaskCreatePinnedToCore(task, "sheltr-mail", 8192, nullptr, 1, &g_task, 0);
  }
}

void reload() { begin(); }

bool configured() {
  const cfg::EmailCfg &settings = cfg::config().email;
  return settings.enabled && settings.host.length() > 0 && settings.from.indexOf('@') > 0 &&
         !smtp::splitAddresses(settings.recipients.c_str()).empty();
}

bool queue(const String &subject, const String &body, const String &to) {
  if (!configured() && !to.length()) return false;
  if (g_queue == nullptr) begin();
  if (g_queue == nullptr) return false;
  Message message;
  fill(message, subject, body, to);
  if (xQueueSend(g_queue, &message, 0) != pdTRUE) {
    // Coda piena: meglio perdere una notifica che bloccare il gateway.
    g_dropped++;
    log_w("Coda email piena: notifica '%s' scartata", subject.c_str());
    return false;
  }
  return true;
}

bool sendNow(const String &subject, const String &body, const String &to, String &error) {
  if (!net::online()) {
    error = F("nessuna connessione a internet");
    return false;
  }
  const cfg::EmailCfg settings = cfg::config().email;
  const bool ok = deliver(settings, subject, body, to, error);
  if (ok) {
    g_sent++;
    g_lastError = "";
    g_lastSentAt = millis();
  } else {
    g_failed++;
    g_lastError = error;
  }
  return ok;
}

void statusJson(JsonObject out) {
  const cfg::EmailCfg &settings = cfg::config().email;
  out["enabled"] = settings.enabled;
  out["configured"] = configured();
  out["host"] = settings.host;
  out["port"] = settings.port;
  out["security"] = settings.security;
  out["from"] = settings.from;
  out["recipients"] = settings.recipients;
  out["sent"] = g_sent;
  out["failed"] = g_failed;
  out["dropped"] = g_dropped;
  out["lastError"] = g_lastError;
  out["lastSentAgoMs"] = g_lastSentAt ? (millis() - g_lastSentAt) : 0;
}

}  // namespace mailer
