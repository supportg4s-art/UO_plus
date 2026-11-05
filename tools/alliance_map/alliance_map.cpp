// UOPlus_AllianceMap — мост между картой UO+ и сервером карты альянса (EnhancedMap, протокол 2/3).
// Владелец 2026-11-05: «давай подвяжем нашу карту в клиенте к серверу альянса».
//
// Для UO+ мост — сервер карты по протоколу UO+ v1 (HTTP JSON на 127.0.0.1, см. UOPlus_map_protocol.txt):
//   uoplus.ini [mapnet] url=http://127.0.0.1:8090/api/map/v1/update
// С сервером альянса — клиент EnhancedMap (TCP, пакеты [id][длина LE 2 байта][данные], числа LE):
//   0x2B протокол -> 0x1F вход (имя/пароль) -> 0x25 своя позиция; от сервера 0x25 позиции других,
//   0x29 вошёл/вышел, 0x27 сигнал, 0x31 общая метка, 0x23 чат, 0x2F сообщение сервера.
// Настройки — AllianceMap.ini рядом с exe. Пароль никуда, кроме сервера альянса, не уходит.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <ctime>

struct Cfg {
    std::string host, user, pass;
    int port = 8887, listen = 8090;
    std::string color_players = "#40C8FF", color_alerts = "#FF4040", color_labels = "#FFD040";
} cfg;

struct User { int x = 0, y = 0, map = 0, hp = 0, maxhp = 0, flag = 0; bool panic = false; DWORD seen = 0; };
struct Alert { std::string who; int x, y; DWORD at; };
struct Label { std::string who, text; int x, y, map; };

static CRITICAL_SECTION g_cs;
static std::map<std::string, User> g_users;
static std::vector<Alert> g_alerts;
static std::vector<Label> g_labels;
static SOCKET g_em = INVALID_SOCKET;
static bool g_logged = false;
static std::string g_state = "нет связи";
static std::string g_dir;
static FILE *g_log;

static void logf(const char *fmt, ...) {
    char buf[1024];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    SYSTEMTIME t; GetLocalTime(&t);
    char line[1200];
    snprintf(line, sizeof line, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, buf);
    // в консоль — в кодировке консоли (UTF-8 выставлен в main)
    fputs(line, stdout); fflush(stdout);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
}

// ---------------------------------------------------------------- настройки
static std::string ini(const char *sec, const char *key, const char *def) {
    char buf[512];
    GetPrivateProfileStringA(sec, key, def, buf, sizeof buf, (g_dir + "\\AllianceMap.ini").c_str());
    return buf;
}
static void load_cfg() {
    std::string path = g_dir + "\\AllianceMap.ini";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(path.c_str(), "wb");
        if (f) {
            fputs("; UO+ — карта альянса (EnhancedMap). Мост для карты UO+.\r\n"
                  "; В uoplus.ini клиента: [mapnet] url=http://127.0.0.1:8090/api/map/v1/update\r\n"
                  "[server]\r\n; адрес и порт сервера карты альянса\r\nhost=\r\nport=8887\r\n"
                  "; имя аккаунта на сервере карты альянса и пароль\r\nuser=\r\npassword=\r\n"
                  "[bridge]\r\nlisten=8090\r\n", f);
            fclose(f);
        }
    }
    cfg.host = ini("server", "host", "");
    cfg.port = atoi(ini("server", "port", "8887").c_str());
    cfg.user = ini("server", "user", "");
    cfg.pass = ini("server", "password", "");
    cfg.listen = atoi(ini("bridge", "listen", "8090").c_str());
}

// ---------------------------------------------------------------- EnhancedMap
static void put16(std::string &b, int v) { b += (char)(v & 0xFF); b += (char)((v >> 8) & 0xFF); }
static void put32(std::string &b, uint32_t v) { for (int i = 0; i < 4; i++) b += (char)((v >> (8 * i)) & 0xFF); }
static std::string pkt(uint8_t id, const std::string &body) {
    std::string b; b += (char)id; put16(b, (int)body.size() + 3); return b + body;
}
static bool em_send(const std::string &p) {
    EnterCriticalSection(&g_cs);
    SOCKET s = g_em;
    LeaveCriticalSection(&g_cs);
    if (s == INVALID_SOCKET) return false;
    return send(s, p.data(), (int)p.size(), 0) == (int)p.size();
}
static void em_position(int x, int y, int map) {
    if (!g_logged) return;
    std::string b;
    put16(b, x); put16(b, y); b += (char)map;
    put16(b, 100); put16(b, 100); put16(b, 100); put16(b, 100); put16(b, 100); put16(b, 100);   // HP/стамина/мана: UO+ их не шлёт
    b += (char)0; b += (char)0; b += (char)0;          // флаг, смерть, паника
    put32(b, 0xFF40C8FF);                              // цвет подписи
    std::string font = "Arial";
    b += (char)font.size(); b += font;
    float sz = 12.0f; uint32_t u; memcpy(&u, &sz, 4); put32(b, u);
    b += (char)0;                                      // стиль шрифта
    em_send(pkt(0x25, b));
}

struct Rd {
    const uint8_t *p; size_t n, i = 3;
    int u8() { return i < n ? p[i++] : 0; }
    int u16() { int a = u8(); return a | (u8() << 8); }
    uint32_t u32() { uint32_t v = 0; for (int k = 0; k < 4; k++) v |= (uint32_t)u8() << (8 * k); return v; }
    std::string str(size_t len) { std::string s; while (len-- && i < n) s += (char)p[i++]; return s; }
};

static void em_packet(const uint8_t *p, size_t n) {
    Rd r{p, n};
    switch (p[0]) {
    case 0x2B: {   // сервер выбрал протокол -> вход
        int proto = r.u8();
        logf("Сервер альянса: протокол %d, вхожу как %s", proto, cfg.user.c_str());
        std::string b;
        b += (char)cfg.user.size(); b += (char)cfg.pass.size(); b += cfg.user; b += cfg.pass;
        em_send(pkt(0x1F, b));
        break;
    }
    case 0x1F: {
        if (r.u8() == 1) {
            int lvl = r.u8();
            std::string name = r.str(r.u8());
            g_logged = true;
            EnterCriticalSection(&g_cs); g_state = "на связи как " + name; LeaveCriticalSection(&g_cs);
            logf("Вход принят: %s (уровень %d)", name.c_str(), lvl);
        } else {
            EnterCriticalSection(&g_cs); g_state = "вход отклонён — проверьте user/password"; LeaveCriticalSection(&g_cs);
            logf("Вход отклонён сервером — проверьте user и password в AllianceMap.ini");
        }
        break;
    }
    case 0x25: {
        User u;
        u.x = r.u16(); u.y = r.u16(); u.map = r.u8();
        u.hp = r.u16(); r.u16(); r.u16(); u.maxhp = r.u16(); r.u16(); r.u16();
        u.flag = r.u8(); r.u8(); u.panic = r.u8() != 0; r.u32();
        r.str(r.u8()); r.u32(); r.u8();
        std::string name = r.str(r.u8());
        u.seen = GetTickCount();
        EnterCriticalSection(&g_cs);
        bool fresh = !g_users.count(name);
        g_users[name] = u;
        LeaveCriticalSection(&g_cs);
        if (fresh) logf("На карте: %s (%d,%d мир %d)", name.c_str(), u.x, u.y, u.map);
        break;
    }
    case 0x29: {
        bool in = r.u8() != 0;
        std::string name = r.str(r.u8());
        logf("%s %s", name.c_str(), in ? "подключился" : "отключился");
        if (!in) { EnterCriticalSection(&g_cs); g_users.erase(name); LeaveCriticalSection(&g_cs); }
        break;
    }
    case 0x27: {
        int x = r.u16(), y = r.u16();
        std::string name = r.str(r.u8());
        logf("СИГНАЛ от %s: %d,%d", name.c_str(), x, y);
        EnterCriticalSection(&g_cs); g_alerts.push_back({name, x, y, GetTickCount()}); LeaveCriticalSection(&g_cs);
        break;
    }
    case 0x31: {
        bool remove = r.u8() != 0;
        int x = r.u16(), y = r.u16(), map = r.u8();
        EnterCriticalSection(&g_cs);
        if (remove) {
            std::string who = r.str(r.u8());
            for (size_t i = 0; i < g_labels.size(); i++)
                if (g_labels[i].who == who && g_labels[i].x == x && g_labels[i].y == y && g_labels[i].map == map) { g_labels.erase(g_labels.begin() + i); break; }
        } else {
            std::string text = r.str(r.u8());
            std::string who = r.str(r.u8());
            g_labels.push_back({who, text, x, y, map});
            if (g_labels.size() > 200) g_labels.erase(g_labels.begin());
        }
        LeaveCriticalSection(&g_cs);
        break;
    }
    case 0x23: {
        int len = r.u16(); r.u32();
        std::string msg = r.str(len);
        std::string who = r.str(r.u8());
        logf("[чат] %s: %s", who.c_str(), msg.c_str());
        break;
    }
    case 0x2F: {
        std::string msg = r.str(r.u16());
        logf("[сервер карты] %s", msg.c_str());
        break;
    }
    }
}

static DWORD WINAPI em_thread(LPVOID) {
    int backoff = 2000;
    for (;;) {
        if (cfg.host.empty() || cfg.user.empty() || cfg.pass.empty()) {
            EnterCriticalSection(&g_cs); g_state = "не заданы host/user/password в AllianceMap.ini"; LeaveCriticalSection(&g_cs);
            Sleep(5000); load_cfg(); continue;
        }
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char port[16]; snprintf(port, sizeof port, "%d", cfg.port);
        SOCKET s = INVALID_SOCKET;
        if (getaddrinfo(cfg.host.c_str(), port, &hints, &res) == 0) {
            s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) { closesocket(s); s = INVALID_SOCKET; }
            freeaddrinfo(res);
        }
        if (s == INVALID_SOCKET) {
            EnterCriticalSection(&g_cs); g_state = "сервер альянса недоступен, пробую ещё"; LeaveCriticalSection(&g_cs);
            logf("Не удалось подключиться к %s:%d — повтор через %d с", cfg.host.c_str(), cfg.port, backoff / 1000);
            Sleep(backoff); backoff = backoff < 60000 ? backoff * 2 : 60000;
            continue;
        }
        logf("Подключён к серверу альянса %s:%d", cfg.host.c_str(), cfg.port);
        EnterCriticalSection(&g_cs); g_em = s; g_state = "вхожу…"; LeaveCriticalSection(&g_cs);
        em_send(pkt(0x2B, std::string(1, '\0')));
        std::string buf;
        char tmp[8192];
        for (;;) {
            int k = recv(s, tmp, sizeof tmp, 0);
            if (k <= 0) break;
            buf.append(tmp, k);
            while (buf.size() >= 3) {
                size_t len = (uint8_t)buf[1] | ((uint8_t)buf[2] << 8);
                if (len < 3 || len > 65535) { buf.clear(); break; }
                if (buf.size() < len) break;
                em_packet((const uint8_t *)buf.data(), len);
                buf.erase(0, len);
            }
            if (g_logged) backoff = 2000;
        }
        logf("Связь с сервером альянса потеряна — переподключаюсь");
        EnterCriticalSection(&g_cs);
        g_em = INVALID_SOCKET; g_logged = false; g_state = "связь потеряна, переподключаюсь"; g_users.clear();
        LeaveCriticalSection(&g_cs);
        closesocket(s);
        Sleep(backoff); backoff = backoff < 60000 ? backoff * 2 : 60000;
    }
}

// ---------------------------------------------------------------- HTTP для UO+
static bool jnum(const std::string &j, const char *key, int &out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return false;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return false;
    out = atoi(j.c_str() + p + 1);
    return true;
}
static std::string jesc(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) o += ' ';
        else o += (char)c;
    }
    return o;
}
static std::string markers_json() {
    std::string out = "{\"v\":1,\"interval\":2000,\"status\":\"";
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_cs);
    out += jesc("альянс: " + g_state) + "\",\"markers\":[";
    bool first = true;
    auto add = [&](const char *kind, const std::string &name, int map, int x, int y, const std::string &color) {
        char b[600];
        snprintf(b, sizeof b, "%s{\"kind\":\"%s\",\"name\":\"%s\",\"facet\":%d,\"x\":%d,\"y\":%d,\"color\":\"%s\"}",
                 first ? "" : ",", kind, jesc(name).c_str(), map, x, y, color.c_str());
        out += b; first = false;
    };
    for (auto it = g_users.begin(); it != g_users.end();) {
        if (now - it->second.seen > 60000) { it = g_users.erase(it); continue; }   // молчит минуту — убрать
        if (it->first != cfg.user) {
            const User &u = it->second;
            std::string name = it->first;
            if (u.flag == 4) name += " (мёртв)";
            else if (u.panic) name += " (ПОМОГИТЕ!)";
            else if (u.maxhp > 0 && u.hp < u.maxhp) name += " " + std::to_string(u.hp * 100 / u.maxhp) + "%";
            add("player", name, u.map, u.x, u.y, u.panic || u.flag == 4 ? cfg.color_alerts : cfg.color_players);
        }
        ++it;
    }
    for (size_t i = 0; i < g_alerts.size();) {
        if (now - g_alerts[i].at > 30000) { g_alerts.erase(g_alerts.begin() + i); continue; }
        auto w = g_users.find(g_alerts[i].who);   // в сигнале нет мира — берём мир того, кто подал
        int map = w != g_users.end() ? w->second.map : 0;
        add("alert", "СИГНАЛ " + g_alerts[i].who, map, g_alerts[i].x, g_alerts[i].y, cfg.color_alerts);
        i++;
    }
    for (auto &l : g_labels) add("point", l.text + " (" + l.who + ")", l.map, l.x, l.y, cfg.color_labels);
    LeaveCriticalSection(&g_cs);
    out += "]}";
    return out;
}
static void http_reply(SOCKET c, int code, const std::string &body) {
    char h[256];
    snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
             code, code == 200 ? "OK" : "Error", (unsigned)body.size());
    std::string all = std::string(h) + body;
    send(c, all.data(), (int)all.size(), 0);
}
static DWORD WINAPI http_conn(LPVOID arg) {
    SOCKET c = (SOCKET)(uintptr_t)arg;
    DWORD to = 4000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    std::string req;
    char tmp[4096];
    size_t need = 0;
    for (;;) {
        int k = recv(c, tmp, sizeof tmp, 0);
        if (k <= 0) break;
        req.append(tmp, k);
        size_t he = req.find("\r\n\r\n");
        if (he != std::string::npos) {
            if (!need) {
                size_t cl = req.find("Content-Length:");
                if (cl == std::string::npos) cl = req.find("content-length:");
                need = he + 4 + (cl != std::string::npos ? (size_t)atoi(req.c_str() + cl + 15) : 0);
            }
            if (req.size() >= need) break;
        }
        if (req.size() > 65536) break;
    }
    size_t he = req.find("\r\n\r\n");
    std::string body = he != std::string::npos ? req.substr(he + 4) : "";
    if (req.compare(0, 4, "POST") == 0) {
        int x, y, facet = 0;
        if (jnum(body, "x", x) && jnum(body, "y", y)) {
            jnum(body, "facet", facet);
            em_position(x, y, facet);
        }
        http_reply(c, 200, markers_json());
    } else if (req.compare(0, 3, "GET") == 0) {
        http_reply(c, 200, markers_json());
    } else {
        http_reply(c, 400, "{\"error\":\"bad request\"}");
    }
    closesocket(c);
    return 0;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"UO+ — карта альянса");
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_dir = exe;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/"));
    g_log = fopen((g_dir + "\\AllianceMap.log").c_str(), "ab");
    InitializeCriticalSection(&g_cs);
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    load_cfg();
    logf("UO+ карта альянса: сервер %s:%d, мост для UO+ на 127.0.0.1:%d", cfg.host.c_str(), cfg.port, cfg.listen);
    if (cfg.user.empty() || cfg.pass.empty())
        logf("Впишите user и password в %s\\AllianceMap.ini — мост подхватит их сам", g_dir.c_str());

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((u_short)cfg.listen); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
        logf("Порт 127.0.0.1:%d занят — мост уже запущен? Закрываюсь.", cfg.listen);
        Sleep(5000);
        return 1;
    }
    CreateThread(nullptr, 0, em_thread, nullptr, 0, nullptr);
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        HANDLE h = CreateThread(nullptr, 0, http_conn, (LPVOID)(uintptr_t)c, 0, nullptr);
        if (h) CloseHandle(h); else closesocket(c);
    }
}
