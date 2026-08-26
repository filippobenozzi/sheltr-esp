#pragma once

// Dialogo SMTP, separato da come viaggiano i byte.
//
// Sul gateway il trasporto e' WiFiClient/WiFiClientSecure, ma qui dentro non
// compare: cosi' la parte che puo' davvero sbagliare (sequenza dei comandi,
// autenticazione, piu' destinatari, gestione degli errori del server) si compila
// anche sull'host e si prova contro un vero server di posta.
// Vedi test/smtp_session_host_test.cpp.

#include <stdint.h>

#include <string>
#include <vector>

namespace smtp {

struct Config {
  std::string host;
  uint16_t port = 587;
  std::string security = "starttls";  // starttls | ssl | none
  std::string username;
  std::string password;
  std::string from;
  std::string fromName;
};

// Il canale verso il server: aprire, scrivere, leggere una risposta.
struct Transport {
  virtual ~Transport() = default;
  // `tls` = la connessione parte gia' cifrata (SSL diretto).
  virtual bool open(const std::string &host, uint16_t port, bool tls) = 0;
  // Passaggio a TLS dopo il comando STARTTLS.
  virtual bool startTls() = 0;
  virtual bool write(const std::string &data) = 0;
  // Ritorna il codice SMTP (0 se il server non risponde). Le risposte su piu'
  // righe vanno lette per intero prima di tornare.
  virtual int readReply(std::string &text) = 0;
  virtual void close() = 0;
};

std::string base64(const std::string &value);
std::vector<std::string> splitAddresses(const std::string &value);

// Consegna un messaggio. `error` viene riempito con la risposta del server, che
// e' quasi sempre la spiegazione piu' utile ("relay denied", "auth failed"...).
bool deliver(Transport &transport, const Config &config, const std::vector<std::string> &recipients,
             const std::string &subject, const std::string &body, std::string &error);

}  // namespace smtp
