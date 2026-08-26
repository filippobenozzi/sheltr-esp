"""Server SMTP finto per provare l'invio delle notifiche, senza toccare un server vero.

    python3 test/fake_smtp_server.py --auth &
    g++ -std=c++17 -O2 -I src test/smtp_session_host_test.cpp src/smtp_session.cpp -o /tmp/smtp_test
    /tmp/smtp_test

Accetta un messaggio, stampa mittente, destinatari e oggetto, e risponde come un
server vero ai comandi che il gateway usa (EHLO, STARTTLS, AUTH LOGIN, MAIL, RCPT,
DATA). Con --auth rifiuta il MAIL FROM finche' non e' stata fatta l'autenticazione.
"""
import json, socket, sys, threading

HOST, PORT = "127.0.0.1", 52525
REQUIRE_AUTH = "--auth" in sys.argv

def handle(conn):
    f = conn.makefile("rwb")
    def send(line):
        f.write(line.encode() + b"\r\n"); f.flush()
    send("220 finto.smtp pronto")
    sender, rcpts, data_mode, lines, authed = "", [], False, [], not REQUIRE_AUTH
    while True:
        raw = f.readline()
        if not raw:
            break
        line = raw.decode("utf-8", "ignore").rstrip("\r\n")
        if data_mode:
            if line == ".":
                data_mode = False
                text = "\n".join(lines)
                subject = next((l for l in lines if l.lower().startswith("subject:")), "(nessun oggetto)")
                to_header = next((l for l in lines if l.lower().startswith("to:")), "")
                print(json.dumps({"mittente": sender, "destinatari": rcpts, "oggetto": subject,
                                  "intestazione_to": to_header, "corpo_righe": len(lines),
                                  "autenticato": authed}, ensure_ascii=False), flush=True)
                lines = []
                send("250 Ok: messaggio accettato")
            else:
                lines.append(line)
            continue
        upper = line.upper()
        if upper.startswith("EHLO") or upper.startswith("HELO"):
            send("250-finto.smtp"); send("250-AUTH PLAIN LOGIN"); send("250 SIZE 10485760")
        elif upper.startswith("STARTTLS"):
            # Risponde come un server vero; la prova sull'host prosegue in chiaro.
            send("220 Pronto per TLS")
        elif upper.startswith("AUTH"):
            authed = True
            if "LOGIN" in upper and len(line.split()) == 2:
                send("334 VXNlcm5hbWU6")
                f.readline(); send("334 UGFzc3dvcmQ6"); f.readline()
            send("235 Autenticazione riuscita")
        elif upper.startswith("MAIL FROM"):
            sender = line.split(":", 1)[1].strip()
            send("250 Ok" if authed else "530 Autenticazione richiesta")
        elif upper.startswith("RCPT TO"):
            rcpts.append(line.split(":", 1)[1].strip()); send("250 Ok")
        elif upper.startswith("DATA"):
            data_mode = True; send("354 Manda pure, chiudi con un punto")
        elif upper.startswith("QUIT"):
            send("221 Arrivederci"); break
        elif upper.startswith("RSET"):
            sender, rcpts = "", []; send("250 Ok")
        else:
            send("250 Ok")
    conn.close()

srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT)); srv.listen(5)
print(f"[smtp] in ascolto su {HOST}:{PORT} (auth {'richiesta' if REQUIRE_AUTH else 'facoltativa'})", flush=True)
while True:
    conn, _ = srv.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
