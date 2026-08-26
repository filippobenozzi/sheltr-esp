// Verifica del dialogo SMTP del gateway, compilata ed eseguita sull'host contro un
// vero server di posta (quello finto usato anche per provare il portale).
//
//   g++ -std=c++17 -O2 -I src test/smtp_session_host_test.cpp src/smtp_session.cpp -o /tmp/smtp_test
//   python3 test/fake_smtp_server.py --auth &   # server di prova su 127.0.0.1:52525
//   /tmp/smtp_test
//
// Senza server in ascolto il test verifica comunque le parti che non richiedono
// rete (base64 dell'autenticazione, elenco destinatari, errori di configurazione).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "smtp_session.h"

namespace {

int failures = 0;

void check(const char *label, bool condition, const std::string &detail = "") {
  std::printf("%s %s%s%s\n", condition ? "OK " : "KO ", label, detail.empty() ? "" : "  -> ",
              detail.c_str());
  if (!condition) failures++;
}

// Trasporto su socket TCP: sull'host non serve TLS, il server di prova parla in chiaro.
class SocketTransport : public smtp::Transport {
 public:
  bool open(const std::string &host, uint16_t port, bool) override {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) return false;
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    return true;
  }

  bool startTls() override {
    tlsRequested_ = true;
    return true;  // il server di prova continua in chiaro
  }

  bool write(const std::string &data) override {
    sent_ += data;
    return ::send(fd_, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size());
  }

  int readReply(std::string &text) override {
    text.clear();
    std::string line;
    while (true) {
      char symbol = 0;
      const ssize_t got = ::recv(fd_, &symbol, 1, 0);
      if (got <= 0) return 0;
      if (symbol == '\n') {
        text += line;
        text += "\n";
        if (line.size() >= 4 && line[3] == '-') {
          line.clear();
          continue;
        }
        return line.size() >= 3 ? std::stoi(line.substr(0, 3)) : 0;
      }
      if (symbol != '\r') line += symbol;
    }
  }

  void close() override {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

  const std::string &sent() const { return sent_; }
  bool tlsRequested() const { return tlsRequested_; }

 private:
  int fd_ = -1;
  std::string sent_;
  bool tlsRequested_ = false;
};

bool serverReachable() {
  SocketTransport probe;
  if (!probe.open("127.0.0.1", 52525, false)) return false;
  probe.close();
  return true;
}

smtp::Config baseConfig() {
  smtp::Config config;
  config.host = "127.0.0.1";
  config.port = 52525;
  config.security = "none";
  config.username = "sheltr";
  config.password = "segreta";
  config.from = "gateway@casa.test";
  config.fromName = "Sheltr ESP";
  return config;
}

}  // namespace

int main() {
  // 1. base64 dell'autenticazione (vettori noti)
  check("base64(\"sheltr\")", smtp::base64("sheltr") == "c2hlbHRy", smtp::base64("sheltr"));
  check("base64 con riempimento", smtp::base64("a") == "YQ==" && smtp::base64("ab") == "YWI=");

  // 2. elenco destinatari
  const auto list = smtp::splitAddresses(" mario@casa.test ,, non-valido; lucia@lavoro.test\nvuoto ");
  check("scarta gli indirizzi non validi", list.size() == 2 && list[0] == "mario@casa.test" &&
                                               list[1] == "lucia@lavoro.test",
        std::to_string(list.size()) + " indirizzi");
  check("accetta le parentesi angolari", smtp::splitAddresses("<a@b.it>").size() == 1);

  // 3. configurazione incompleta: errore chiaro, nessuna connessione
  {
    SocketTransport transport;
    smtp::Config config = baseConfig();
    config.from = "senza-chiocciola";
    std::string error;
    const bool ok = smtp::deliver(transport, config, {"a@b.it"}, "x", "y", error);
    check("mittente non valido -> errore", !ok && !error.empty(), error);
  }
  {
    SocketTransport transport;
    std::string error;
    const bool ok = smtp::deliver(transport, baseConfig(), {}, "x", "y", error);
    check("nessun destinatario -> errore", !ok && !error.empty(), error);
  }

  if (!serverReachable()) {
    std::printf("\nServer di posta di prova non in ascolto: saltate le verifiche di rete.\n");
    std::printf(failures ? "\n%d TEST FALLITI\n" : "\nTUTTI OK\n", failures);
    return failures ? 1 : 0;
  }

  // 4. invio completo con autenticazione e due destinatari
  {
    SocketTransport transport;
    std::string error;
    const bool ok = smtp::deliver(transport, baseConfig(), {"mario@casa.test", "lucia@lavoro.test"},
                                  "Luce 1 (Cucina): ACCESA", "Luce 1 (Cucina): ACCESA\r\n", error);
    const std::string &sent = transport.sent();
    check("invio riuscito", ok, error);
    check("saluto EHLO", sent.find("EHLO sheltr\r\n") != std::string::npos);
    check("autenticazione in base64", sent.find("AUTH LOGIN\r\n") != std::string::npos &&
                                          sent.find("c2hlbHRy\r\n") != std::string::npos);
    check("mittente nella busta", sent.find("MAIL FROM:<gateway@casa.test>\r\n") != std::string::npos);
    check("un RCPT per destinatario",
          sent.find("RCPT TO:<mario@casa.test>\r\n") != std::string::npos &&
              sent.find("RCPT TO:<lucia@lavoro.test>\r\n") != std::string::npos);
    check("intestazioni e chiusura del messaggio",
          sent.find("Subject: Luce 1 (Cucina): ACCESA\r\n") != std::string::npos &&
              sent.find("Content-Type: text/plain; charset=UTF-8") != std::string::npos &&
              sent.find("\r\n.\r\n") != std::string::npos);
    check("i destinatari non compaiono nelle intestazioni",
          sent.find("To: Sheltr ESP <gateway@casa.test>") != std::string::npos &&
              sent.find("To: mario@casa.test") == std::string::npos);
    check("chiusura con QUIT", sent.find("QUIT\r\n") != std::string::npos);
  }

  // 5. STARTTLS: il comando parte e il dialogo riprende con un nuovo EHLO
  {
    SocketTransport transport;
    smtp::Config config = baseConfig();
    config.security = "starttls";
    std::string error;
    const bool ok = smtp::deliver(transport, config, {"mario@casa.test"}, "prova", "corpo", error);
    const std::string &sent = transport.sent();
    size_t first = sent.find("EHLO sheltr\r\n");
    size_t starttls = sent.find("STARTTLS\r\n");
    size_t second = sent.find("EHLO sheltr\r\n", starttls);
    check("STARTTLS poi di nuovo EHLO",
          ok && transport.tlsRequested() && first != std::string::npos &&
              starttls != std::string::npos && second != std::string::npos && second > starttls,
          error);
  }

  // 6. senza credenziali non si autentica
  {
    SocketTransport transport;
    smtp::Config config = baseConfig();
    config.username.clear();
    config.password.clear();
    std::string error;
    smtp::deliver(transport, config, {"mario@casa.test"}, "prova", "corpo", error);
    check("senza utente niente AUTH", transport.sent().find("AUTH") == std::string::npos);
  }

  // 7. una riga di solo punto nel testo non tronca il messaggio
  {
    SocketTransport transport;
    std::string error;
    smtp::deliver(transport, baseConfig(), {"mario@casa.test"}, "prova", "prima\r\n.\r\ndopo\r\n", error);
    check("riga con solo punto protetta", transport.sent().find("\r\n..\r\ndopo") != std::string::npos);
  }

  std::printf(failures ? "\n%d TEST FALLITI\n" : "\nTUTTI OK\n", failures);
  return failures ? 1 : 0;
}
