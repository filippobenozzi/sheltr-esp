#include "smtp_session.h"

namespace smtp {

namespace {

const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool codeOk(int code, int wanted) { return code == wanted || (wanted == 250 && code == 251); }

bool step(Transport &transport, const std::string &command, int wanted, std::string &error) {
  if (!command.empty() && !transport.write(command + "\r\n")) {
    error = "scrittura sul server non riuscita";
    return false;
  }
  std::string reply;
  const int code = transport.readReply(reply);
  if (codeOk(code, wanted)) return true;
  while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
  error = reply.empty() ? "nessuna risposta dal server di posta" : reply;
  return false;
}

std::string headerAddress(const Config &config) {
  if (config.fromName.empty()) return config.from;
  return config.fromName + " <" + config.from + ">";
}

}  // namespace

std::string base64(const std::string &value) {
  std::string out;
  const size_t length = value.length();
  const unsigned char *data = reinterpret_cast<const unsigned char *>(value.data());
  for (size_t i = 0; i < length; i += 3) {
    const uint32_t byte0 = data[i];
    const uint32_t byte1 = (i + 1 < length) ? data[i + 1] : 0;
    const uint32_t byte2 = (i + 2 < length) ? data[i + 2] : 0;
    const uint32_t triple = (byte0 << 16) | (byte1 << 8) | byte2;
    out += B64[(triple >> 18) & 0x3F];
    out += B64[(triple >> 12) & 0x3F];
    out += (i + 1 < length) ? B64[(triple >> 6) & 0x3F] : '=';
    out += (i + 2 < length) ? B64[triple & 0x3F] : '=';
  }
  return out;
}

std::vector<std::string> splitAddresses(const std::string &value) {
  std::vector<std::string> out;
  std::string current;
  for (size_t i = 0; i <= value.length(); i++) {
    const char symbol = (i < value.length()) ? value[i] : ',';
    if (symbol == ',' || symbol == ';' || symbol == '\n') {
      // Ripulisce spazi e parentesi angolari intorno all'indirizzo.
      size_t start = current.find_first_not_of(" \t\r<");
      size_t end = current.find_last_not_of(" \t\r>");
      if (start != std::string::npos && end != std::string::npos && end >= start) {
        const std::string address = current.substr(start, end - start + 1);
        const size_t at = address.find('@');
        if (at != std::string::npos && at > 0 && at + 1 < address.length() &&
            address.find(' ') == std::string::npos) {
          out.push_back(address);
        }
      }
      current.clear();
      continue;
    }
    current += symbol;
  }
  return out;
}

bool deliver(Transport &transport, const Config &config, const std::vector<std::string> &recipients,
             const std::string &subject, const std::string &body, std::string &error) {
  if (recipients.empty()) {
    error = "nessun destinatario valido";
    return false;
  }
  if (config.host.empty() || config.from.find('@') == std::string::npos) {
    error = "server di posta o mittente non configurati";
    return false;
  }

  const bool directTls = config.security == "ssl";
  if (!transport.open(config.host, config.port, directTls)) {
    error = "connessione a " + config.host + " non riuscita";
    return false;
  }

  std::string reply;
  if (transport.readReply(reply) != 220) {
    transport.close();
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
    error = reply.empty() ? "il server non ha risposto al saluto" : reply;
    return false;
  }
  if (!step(transport, "EHLO sheltr", 250, error)) {
    transport.close();
    return false;
  }

  if (config.security == "starttls") {
    if (!step(transport, "STARTTLS", 220, error)) {
      transport.close();
      return false;
    }
    if (!transport.startTls()) {
      transport.close();
      error = "passaggio a TLS non riuscito";
      return false;
    }
    // Dopo STARTTLS il dialogo riparte: serve un nuovo EHLO.
    if (!step(transport, "EHLO sheltr", 250, error)) {
      transport.close();
      return false;
    }
  }

  if (!config.username.empty()) {
    if (!step(transport, "AUTH LOGIN", 334, error) ||
        !step(transport, base64(config.username), 334, error) ||
        !step(transport, base64(config.password), 235, error)) {
      transport.close();
      error = "autenticazione rifiutata: " + error;
      return false;
    }
  }

  if (!step(transport, "MAIL FROM:<" + config.from + ">", 250, error)) {
    transport.close();
    return false;
  }
  for (const std::string &address : recipients) {
    if (!step(transport, "RCPT TO:<" + address + ">", 250, error)) {
      transport.close();
      error = "destinatario " + address + " rifiutato: " + error;
      return false;
    }
  }
  if (!step(transport, "DATA", 354, error)) {
    transport.close();
    return false;
  }

  // I destinatari non compaiono nelle intestazioni: non si vedono a vicenda.
  std::string message;
  message += "From: " + headerAddress(config) + "\r\n";
  message += "To: " + headerAddress(config) + "\r\n";
  message += "Subject: " + subject + "\r\n";
  message += "MIME-Version: 1.0\r\n";
  message += "Content-Type: text/plain; charset=UTF-8\r\n\r\n";
  message += body;
  if (message.size() < 2 || message.compare(message.size() - 2, 2, "\r\n") != 0) message += "\r\n";
  // Una riga di solo punto chiude il messaggio: se il testo ne contiene una va
  // protetta, altrimenti il messaggio verrebbe troncato li'.
  std::string escaped;
  escaped.reserve(message.size());
  for (size_t i = 0; i < message.size(); i++) {
    if (message[i] == '.' && (i == 0 || (i >= 2 && message[i - 1] == '\n' && message[i - 2] == '\r'))) {
      escaped += '.';
    }
    escaped += message[i];
  }
  if (!transport.write(escaped) || !transport.write(".\r\n")) {
    transport.close();
    error = "invio del messaggio non riuscito";
    return false;
  }

  std::string finalReply;
  const int code = transport.readReply(finalReply);
  if (code != 250) {
    transport.close();
    while (!finalReply.empty() && (finalReply.back() == '\n' || finalReply.back() == '\r')) finalReply.pop_back();
    error = finalReply.empty() ? "messaggio rifiutato" : finalReply;
    return false;
  }
  transport.write("QUIT\r\n");
  transport.close();
  return true;
}

}  // namespace smtp
