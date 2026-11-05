#!/usr/bin/env python3
"""Тестовый сервер EnhancedMap (протокол 3) — по исходникам EnhancedMapServerNetCore. Только для проверки моста."""
import socket, struct, threading, time, sys

USERS = {"Elisa": "test", "Sylthar": "test"}
clients = []
lock = threading.Lock()


def pkt(pid, body):
    return bytes([pid]) + struct.pack("<H", len(body) + 3) + body


def s8(s):
    b = s.encode("ascii")
    return bytes([len(b)]) + b


def player_data(x, y, m, name, hp=80, maxhp=100, flag=0, panic=0):
    b = struct.pack("<HHB", x, y, m) + struct.pack("<6H", hp, 50, 50, maxhp, 50, 50)
    b += bytes([flag, 0, panic]) + struct.pack("<I", 0xFFFFFFFF) + s8("Arial") + struct.pack("<f", 12.0) + bytes([0]) + s8(name)
    return pkt(0x25, b)


def handle(c, addr):
    buf = b""; name = None
    while True:
        d = c.recv(4096)
        if not d:
            break
        buf += d
        while len(buf) >= 3:
            n = struct.unpack_from("<H", buf, 1)[0]
            if len(buf) < n:
                break
            p, buf = buf[:n], buf[n:]
            if p[0] == 0x2B:
                c.sendall(pkt(0x2B, bytes([3])))
            elif p[0] == 0x1F:
                ul, pl = p[3], p[4]
                u = p[5:5 + ul].decode(); pw = p[5 + ul:5 + ul + pl].decode()
                ok = USERS.get(u) == pw
                print("login", u, "ok" if ok else "REFUSED", flush=True)
                if not ok:
                    c.sendall(pkt(0x1F, bytes([0]))); c.close(); return
                name = u
                c.sendall(pkt(0x1F, bytes([1, 0]) + s8(u) + bytes([3])))
                c.sendall(pkt(0x29, bytes([1]) + s8("Sylthar")))
                c.sendall(player_data(1500, 1600, 0, "Sylthar"))
                c.sendall(player_data(800, 700, 2, "Bruke", flag=4))
                c.sendall(pkt(0x27, struct.pack("<HH", 1505, 1605) + s8("Sylthar")))
                c.sendall(pkt(0x31, bytes([0]) + struct.pack("<HHB", 1400, 1400, 0) + s8("Lair") + s8("Sylthar")))
            elif p[0] == 0x25:
                x, y, m = struct.unpack_from("<HHB", p, 3)
                print("pos from", name, x, y, m, "font", p[3 + 5 + 12 + 3 + 4 + 1:3 + 5 + 12 + 3 + 4 + 1 + p[3 + 5 + 12 + 3 + 4]], flush=True)
    c.close()


s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 18887)); s.listen()
while True:
    c, a = s.accept()
    threading.Thread(target=handle, args=(c, a), daemon=True).start()
