#!/usr/bin/env python3
"""UO+ радар-сервер для доработанного EnhancedMap.

Каждый клиент раз в секунду шлёт POST с теми, кого видит его Орион (зелёные и рыжие),
и получает в ответ всех, кого за последние TTL секунд видел кто-либо из альянса.

Запрос  (text/plain, UTF-8, заголовок X-Radar-Key):
    #uoplus-radar 1|<кто прислал>
    serial|x|y|map|noto|hp|name
Ответ:
    #uoplus-radar 1
    serial|x|y|map|noto|hp|age_ms|reporter|name

Запуск: python3 radar_server.py --port 8899 --key СЕКРЕТ
Только стандартная библиотека Python 3.7+.
"""
import argparse
import http.server
import threading
import time

TTL = 10.0
MAX_BODY = 64 * 1024
MAX_CONTACTS = 5000

lock = threading.Lock()
contacts = {}   # serial -> (seen, x, y, map, noto, hp, reporter, name)
reporters = {}  # reporter -> last time
KEY = ""


def clean(s, n=40):
    return s.replace("|", "/").replace("\n", " ").replace("\r", " ").strip()[:n]


def parse_body(text, now):
    lines = text.split("\n")
    if not lines or not lines[0].startswith("#uoplus-radar 1"):
        return None
    head = lines[0].split("|", 1)
    reporter = clean(head[1]) if len(head) > 1 else "?"
    got = []
    for line in lines[1:]:
        line = line.strip("\r \t")
        if not line or line[0] == "#":
            continue
        f = line.split("|", 6)
        if len(f) < 7:
            continue
        try:
            serial = int(f[0], 16) if f[0].lower().startswith("0x") else int(f[0])
            x, y, m, noto, hp = int(f[1]), int(f[2]), int(f[3]), int(f[4]), int(f[5])
        except ValueError:
            continue
        if noto not in (2, 5) or not (0 < x <= 7168 and 0 < y <= 4096) or not (0 <= m < 256):
            continue
        got.append((serial, (now, x, y, m, noto, max(-1, min(hp, 100)), reporter, clean(f[6]))))
    return reporter, got


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "UOPlusRadar/1"

    def log_message(self, fmt, *args):
        pass

    def reply(self, code, body):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/health"):
            now = time.time()
            with lock:
                live = sum(1 for c in contacts.values() if now - c[0] < TTL)
                who = sorted(r for r, t in reporters.items() if now - t < 30)
            self.reply(200, "ok contacts=%d reporters=%d %s\n" % (live, len(who), ",".join(who)))
        else:
            self.reply(404, "not found\n")

    def do_POST(self):
        if self.path.rstrip("/") != "/radar":
            return self.reply(404, "not found\n")
        if KEY and self.headers.get("X-Radar-Key", "") != KEY:
            return self.reply(403, "bad key\n")
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n < 0 or n > MAX_BODY:
            return self.reply(413, "too big\n")
        text = self.rfile.read(n).decode("utf-8", "replace")
        now = time.time()
        parsed = parse_body(text, now)
        if parsed is None:
            return self.reply(400, "bad request\n")
        reporter, got = parsed
        out = ["#uoplus-radar 1"]
        with lock:
            reporters[reporter] = now
            for serial, c in got:
                old = contacts.get(serial)
                if old is None or old[0] <= c[0]:
                    contacts[serial] = c
            for serial in [s for s, c in contacts.items() if now - c[0] >= TTL]:
                del contacts[serial]
            while len(contacts) > MAX_CONTACTS:
                del contacts[min(contacts, key=lambda s: contacts[s][0])]
            for serial, (seen, x, y, m, noto, hp, rep, name) in contacts.items():
                out.append("0x%08X|%d|%d|%d|%d|%d|%d|%s|%s" % (serial, x, y, m, noto, hp, int((now - seen) * 1000), rep, name))
        self.reply(200, "\n".join(out) + "\n")


def main():
    global KEY
    ap = argparse.ArgumentParser(description="UO+ радар-сервер")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--key", default="", help="общий ключ альянса (обязательно задать)")
    a = ap.parse_args()
    KEY = a.key
    if not KEY:
        print("ВНИМАНИЕ: ключ не задан, радар открыт для всех")
    srv = http.server.ThreadingHTTPServer((a.host, a.port), Handler)
    srv.daemon_threads = True
    print("UO+ радар-сервер слушает %s:%d" % (a.host, a.port), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
