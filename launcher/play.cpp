// UOPlus_Play.exe — лаунчер UO+: проверяет обновления (manifest.json на GitHub), докачивает изменённое и запускает игру.
// Лежит в папке клиента. Пути в манифесте — от папки клиента. Ничего личного не читает и не отправляет.
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>

static const wchar_t *MANIFEST_URL = L"https://raw.githubusercontent.com/supportg4s-art/UO_plus/main/manifest.json";
static const wchar_t *TITLE = L"UO+ — Age of Power";
static const wchar_t *VERSION_FILE = L"uoplus_version.txt";
static const UINT WM_APP_LOG = WM_APP + 1, WM_APP_DONE = WM_APP + 2, WM_APP_PROG = WM_APP + 3;

static HWND g_wnd, g_log, g_prog, g_play, g_check, g_ver;
static std::wstring g_dir;
static volatile bool g_busy = false;

static std::wstring dir_of_self() {
    wchar_t b[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    std::wstring s = b;
    return s.substr(0, s.find_last_of(L'\\'));
}
static std::wstring self_path() { wchar_t b[MAX_PATH * 2]; GetModuleFileNameW(nullptr, b, MAX_PATH * 2); return b; }
static std::wstring u2w(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static void log(const std::wstring &s) {
    // и в файл uoplus_launcher.log рядом — чтобы было видно, что случилось, если окно уже закрыли
    FILE *f = _wfopen((dir_of_self() + L"\\uoplus_launcher.log").c_str(), L"a, ccs=UTF-8");
    if (f) { SYSTEMTIME t; GetLocalTime(&t); fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, s.c_str()); fclose(f); }
    if (g_wnd) PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(s));
}
static bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---------------------------------------------------------------- сеть
static bool http_get(const std::wstring &url, std::vector<char> *mem, HANDLE file, long long expect) {
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    HINTERNET s = WinHttpOpen(L"UOPlus-Launcher/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) s = WinHttpOpen(L"UOPlus-Launcher/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!s) return false;
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, nullptr, nullptr,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
        if (code == 200) {
            ok = true;
            long long got = 0;
            std::vector<char> buf(1 << 16);
            for (;;) {
                DWORD n = 0;
                if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) { ok = false; break; }
                if (!n) break;
                if (mem) mem->insert(mem->end(), buf.data(), buf.data() + n);
                if (file) { DWORD w; if (!WriteFile(file, buf.data(), n, &w, nullptr) || w != n) { ok = false; break; } }
                got += n;
                if (expect > 0) PostMessageW(g_wnd, WM_APP_PROG, (WPARAM)(got * 1000 / expect), 0);
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// ---------------------------------------------------------------- sha256
static std::string sha256_file(const std::wstring &p) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            std::vector<unsigned char> buf(1 << 20);
            DWORD n;
            while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) BCryptHashData(h, buf.data(), n, 0);
            unsigned char d[32];
            BCryptFinishHash(h, d, 32, 0);
            char hex[65];
            for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]);
            out = hex;
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(f);
    return out;
}

// ---------------------------------------------------------------- манифест (наш формат, простой разбор)
struct MFile { std::string path, url, sha, option; long long size = 0; bool only_if_ours = false; };
struct Manifest { int version = 0; std::string notes; std::vector<MFile> files; };
static std::string jstr(const std::string &s, size_t &i) {   // s[i] == '"'
    std::string r;
    for (i++; i < s.size() && s[i] != '"'; i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char e = s[++i];
            if (e == 'n') r += '\n'; else if (e == 't') r += '\t';
            else if (e == 'u' && i + 4 < s.size()) {
                unsigned cp = strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16); i += 4;
                if (cp < 0x80) r += (char)cp;
                else if (cp < 0x800) { r += (char)(0xC0 | cp >> 6); r += (char)(0x80 | (cp & 63)); }
                else { r += (char)(0xE0 | cp >> 12); r += (char)(0x80 | ((cp >> 6) & 63)); r += (char)(0x80 | (cp & 63)); }
            } else r += e;
        } else r += s[i];
    }
    i++;
    return r;
}
static bool parse_manifest(const std::string &s, Manifest &m) {
    // ключи верхнего уровня и объекты в "files": [ {...}, ... ]
    size_t i = 0;
    int depth = 0;
    MFile cur;
    bool in_files = false;
    std::string key;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            std::string t = jstr(s, i);
            while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
            if (i < s.size() && s[i] == ':') { key = t; i++; continue; }
            if (depth == 1 && key == "notes") m.notes = t;
            if (in_files && depth == 3) {
                if (key == "path") cur.path = t; else if (key == "url") cur.url = t; else if (key == "sha256") cur.sha = t;
                else if (key == "option") cur.option = t;
            }
            continue;
        }
        if (c == '{') { depth++; if (in_files && depth == 3) cur = MFile(); }
        else if (c == '}') { if (in_files && depth == 3 && !cur.path.empty()) m.files.push_back(cur); depth--; }
        else if (c == '[') { if (depth == 1 && key == "files") in_files = true; depth++; }
        else if (c == ']') { depth--; if (depth == 1) in_files = false; }
        else if ((c >= '0' && c <= '9') || c == '-') {
            long long v = strtoll(s.c_str() + i, nullptr, 10);
            if (depth == 1 && key == "version") m.version = (int)v;
            if (in_files && depth == 3 && key == "size") cur.size = v;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '.')) i++;
            continue;
        } else if (s.compare(i, 4, "true") == 0) { if (in_files && depth == 3 && key == "only_if_ours") cur.only_if_ours = true; i += 4; continue; }
        i++;
    }
    return m.version > 0 && !m.files.empty();
}

// ---------------------------------------------------------------- игра
static bool game_running() {
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e = {sizeof e};
    bool found = false;
    if (Process32FirstW(sn, &e)) do { if (_wcsnicmp(e.szExeFile, L"Client+inj", 10) == 0) found = true; } while (Process32NextW(sn, &e));
    CloseHandle(sn);
    return found;
}
static int local_version() {
    FILE *f = _wfopen((g_dir + L"\\" + VERSION_FILE).c_str(), L"r");
    int v = 0;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); }
    return v;
}
// куда класть файл: uoplus.ini [files] перенаправляет verdata.mul и т.п. — обновляем то, что игра реально читает
static std::wstring target_of(const std::string &rel) {
    std::wstring w = u2w(rel);
    for (auto &c : w) if (c == L'/') c = L'\\';
    std::wstring name = w.substr(w.find_last_of(L'\\') + 1);
    if (w.find(L'\\') == std::wstring::npos) {
        wchar_t b[MAX_PATH * 2];
        GetPrivateProfileStringW(L"files", name.c_str(), L"", b, MAX_PATH * 2, (g_dir + L"\\uoplus.ini").c_str());
        if (b[0]) return b;
    }
    return g_dir + L"\\" + w;
}
static bool is_ours(const std::wstring &p) {   // наш UOPlus (а не оригинальный zlib1.dll Injection)
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(f, nullptr), n;
    std::vector<char> d(sz);
    ReadFile(f, d.data(), sz, &n, nullptr);
    CloseHandle(f);
    static const char mark[] = "UOPlus";
    return std::search(d.begin(), d.end(), mark, mark + 6) != d.end() && sz > 500000;
}

// ---------------------------------------------------------------- настройки (uoplus.ini и ddraw.ini)
static std::wstring ini_path() { return g_dir + L"\\uoplus.ini"; }
static int ini_int(const wchar_t *sec, const wchar_t *key, int def) { return GetPrivateProfileIntW(sec, key, def, ini_path().c_str()); }
static void ini_set(const wchar_t *sec, const wchar_t *key, const std::wstring &v) { WritePrivateProfileStringW(sec, key, v.c_str(), ini_path().c_str()); }
static std::wstring ini_str(const wchar_t *sec, const wchar_t *key, const wchar_t *def) {
    wchar_t b[1024]; GetPrivateProfileStringW(sec, key, def, b, 1024, ini_path().c_str()); return b;
}
static const wchar_t *NOPENKI = L"verdata_nopenki.mul";
static bool option_on(const std::string &o) {
    if (o == "nopenki") return ini_int(L"launch", L"penki", 1) == 0;
    return false;
}
// [files] verdata.mul — DLL берёт verdata отсюда. Лаунчер управляет строкой, только если её нет или она наша (без пеньков);
// если игрок прописал свою verdata — не трогаем, галочка «пеньки» тогда недоступна.
static bool verdata_custom() {
    std::wstring v = ini_str(L"files", L"verdata.mul", L"");
    return !v.empty() && lstrcmpiW(v.c_str(), (g_dir + L"\\" + NOPENKI).c_str()) != 0;
}
static void apply_penki() {
    if (verdata_custom()) return;
    bool off = ini_int(L"launch", L"penki", 1) == 0 && exists(g_dir + L"\\" + NOPENKI);
    WritePrivateProfileStringW(L"files", L"verdata.mul", off ? (g_dir + L"\\" + NOPENKI).c_str() : nullptr, ini_path().c_str());
}

static DWORD WINAPI update_thread(LPVOID) {
    std::vector<char> raw;
    log(L"Проверяю обновления…");
    wchar_t mu[1024];   // [update] manifest = … в uoplus.ini — свой адрес списка (для проверки); обычно не задан
    GetPrivateProfileStringW(L"update", L"manifest", MANIFEST_URL, mu, 1024, (g_dir + L"\\uoplus.ini").c_str());
    if (!http_get(mu, &raw, nullptr, 0)) { log(L"Нет связи с сервером обновлений — играем на том, что есть."); PostMessageW(g_wnd, WM_APP_DONE, 0, 0); return 0; }
    Manifest m;
    if (!parse_manifest(std::string(raw.begin(), raw.end()), m)) { log(L"Список обновлений повреждён — пропускаю."); PostMessageW(g_wnd, WM_APP_DONE, 0, 0); return 0; }
    std::vector<MFile> need;
    for (auto &f : m.files) {
        std::wstring t = target_of(f.path);
        if (f.only_if_ours && !(exists(t) && is_ours(t))) continue;   // zlib1.dll: только если там уже наш, Injection-овский не трогаем
        if (!f.option.empty() && !option_on(f.option)) continue;       // нужно только при такой настройке (verdata без пеньков)
        if (sha256_file(t) != f.sha) need.push_back(f);
    }
    if (need.empty()) {
        FILE *vf = _wfopen((g_dir + L"\\" + VERSION_FILE).c_str(), L"w"); if (vf) { fprintf(vf, "%d\n", m.version); fclose(vf); }
        log(L"Версия " + std::to_wstring(m.version) + L" — всё обновлено.");
        PostMessageW(g_wnd, WM_APP_DONE, 1, 0);
        return 0;
    }
    if (game_running()) { log(L"Есть обновление до версии " + std::to_wstring(m.version) + L". Закройте игру и нажмите «Проверить обновления»."); PostMessageW(g_wnd, WM_APP_DONE, 0, 0); return 0; }
    log(L"Обновление до версии " + std::to_wstring(m.version) + L": файлов " + std::to_wstring(need.size()));
    if (!m.notes.empty()) log(L"Что нового: " + u2w(m.notes));
    int ok = 0;
    std::wstring me = self_path();
    for (auto &f : need) {
        std::wstring t = target_of(f.path), tmp = t + L".download";
        CreateDirectoryW(t.substr(0, t.find_last_of(L'\\')).c_str(), nullptr);
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { log(L"Не могу записать " + t + L" (нет прав на папку?)"); continue; }
        log(L"Скачиваю " + u2w(f.path) + L" (" + std::to_wstring(f.size / 1024) + L" КБ)…");
        bool got = http_get(u2w(f.url), nullptr, h, f.size);
        CloseHandle(h);
        if (!got || sha256_file(tmp) != f.sha) { DeleteFileW(tmp.c_str()); log(L"  не скачалось целиком — попробуйте ещё раз позже"); continue; }
        std::wstring old = t + L".old";
        DeleteFileW(old.c_str());
        if (exists(t) && !MoveFileW(t.c_str(), old.c_str())) { DeleteFileW(tmp.c_str()); log(L"  файл занят — закройте игру"); continue; }
        if (!MoveFileW(tmp.c_str(), t.c_str())) { MoveFileW(old.c_str(), t.c_str()); log(L"  не удалось заменить"); continue; }
        if (_wcsicmp(t.c_str(), me.c_str()) == 0) log(L"  лаунчер обновлён — новая версия со следующего запуска");
        ok++;
    }
    if (ok == (int)need.size()) {
        FILE *vf = _wfopen((g_dir + L"\\" + VERSION_FILE).c_str(), L"w"); if (vf) { fprintf(vf, "%d\n", m.version); fclose(vf); }
        log(L"Готово: версия " + std::to_wstring(m.version) + L". Прошлые файлы сохранены рядом с расширением .old");
    } else log(L"Обновлено " + std::to_wstring(ok) + L" из " + std::to_wstring(need.size()) + L".");
    PostMessageW(g_wnd, WM_APP_DONE, ok == (int)need.size(), 0);
    return 0;
}

static std::wstring pick_client() {   // как раньше в UOPlus_Play: размер окна из [launch] size или самый большой, что влезает
    std::vector<std::pair<int, int>> sizes;
    bool has_std = false;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((g_dir + L"\\Client+inj_multi_3.0.0c*_uoplus.exe").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            std::wstring mid = n.substr(23, n.size() - 23 - 11);
            int w = 0, h = 0;
            if (mid.empty()) has_std = true;
            else if (swscanf(mid.c_str(), L"_%dx%d", &w, &h) == 2 && mid.find_first_not_of(L"_0123456789x") == std::wstring::npos)
                sizes.push_back({w, h});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(sizes.begin(), sizes.end());
    wchar_t want[32];
    GetPrivateProfileStringW(L"launch", L"size", L"auto", want, 32, (g_dir + L"\\uoplus.ini").c_str());
    std::wstring exe;
    if (lstrcmpiW(want, L"std") == 0 && has_std) exe = L"Client+inj_multi_3.0.0c_uoplus.exe";
    else if (lstrcmpiW(want, L"auto") != 0) {
        std::wstring e = std::wstring(L"Client+inj_multi_3.0.0c_") + want + L"_uoplus.exe";
        if (exists(g_dir + L"\\" + e)) exe = e;
    }
    if (exe.empty()) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int W = wa.right - wa.left, H = wa.bottom - wa.top;
        std::pair<int, int> best = {0, 0};
        for (auto &s : sizes) if (s.first + 16 <= W && s.second + 40 <= H) best = s;
        if (!best.first && !sizes.empty()) best = sizes.front();
        if (best.first) exe = L"Client+inj_multi_3.0.0c_" + std::to_wstring(best.first) + L"x" + std::to_wstring(best.second) + L"_uoplus.exe";
        else if (has_std) exe = L"Client+inj_multi_3.0.0c_uoplus.exe";
    }
    return exe;
}
// ---------------------------------------------------------------- реестр: где клиент ищет файлы игры
// Клиент 3.0.0c берёт папку с *.mul/*.def не из своей папки, а из реестра (ExePath). Если там чужая папка без файлов,
// он молча выходит с кодом 1 (2026-11-17, тестер на Маке: «ярлык не стартует»). Рабочую запись не трогаем —
// у игрока может стоять ещё и обычный клиент AoP; переписываем, только если там файлов игры нет.
static const wchar_t *UO_KEY = L"SOFTWARE\\Origin Worlds Online\\Ultima Online\\1.0";
static bool has_game_files(const std::wstring &d) {
    static const wchar_t *need[] = {L"anim1.def", L"anim2.def", L"anim3.def", L"anim4.def", L"art.def", L"body.def",
                                    L"corpse.def", L"music.def", L"sound.def", L"art.mul", L"tiledata.mul"};
    if (d.empty()) return false;
    for (auto n : need) if (!exists(d + L"\\" + n)) return false;
    return true;
}
static std::wstring reg_game_dir() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UO_KEY, 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) return L"";
    wchar_t b[MAX_PATH * 2] = {};
    DWORD sz = sizeof b - sizeof(wchar_t), type = 0;
    LONG r = RegQueryValueExW(k, L"ExePath", nullptr, &type, (BYTE *)b, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ) return L"";
    std::wstring s = b;
    size_t i = s.find_last_of(L'\\');
    return i == std::wstring::npos ? L"" : s.substr(0, i);
}
static bool write_reg() {
    std::wstring exe = pick_client();
    if (exe.empty()) return false;
    std::wstring full = g_dir + L"\\" + exe, cd = g_dir + L"\\";
    bool ok32 = false;
    for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, UO_KEY, 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &k, nullptr) != ERROR_SUCCESS)
            continue;
        auto put = [&](const wchar_t *n, const std::wstring &v) {
            return RegSetValueExW(k, n, 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)((v.size() + 1) * 2)) == ERROR_SUCCESS;
        };
        bool ok = put(L"ExePath", full) && put(L"InstCDPath", cd) && put(L"StartExePath", full) && put(L"PatchExePath", full)
                  && put(L"Upgraded", L"Yes");
        RegCloseKey(k);
        if (view == KEY_WOW64_32KEY) ok32 = ok;
    }
    return ok32;
}
static void fix_registry() {
    std::wstring cur = reg_game_dir();
    if (has_game_files(cur)) return;   // клиент найдёт свои файлы — не трогаем
    if (!has_game_files(g_dir)) {
        log(L"В реестре папка игры не та (" + (cur.empty() ? std::wstring(L"пусто") : cur) + L"), а в папке UO+ нет всех файлов игры — реестр не трогаю");
        return;
    }
    log(L"Папка игры в реестре: " + (cur.empty() ? std::wstring(L"не записана") : cur) + L" — прописываю " + g_dir);
    if (write_reg()) { log(L"Реестр: готово"); return; }
    // в HKLM без прав администратора не записать — один раз спросить Windows (окно «Разрешить изменения?»)
    log(L"Реестр: нужны права администратора — Windows спросит разрешение");
    SHELLEXECUTEINFOW se = {sizeof se};
    std::wstring me = self_path();
    se.fMask = SEE_MASK_NOCLOSEPROCESS; se.hwnd = g_wnd; se.lpVerb = L"runas"; se.lpFile = me.c_str();
    se.lpParameters = L"/fixreg"; se.lpDirectory = g_dir.c_str(); se.nShow = SW_HIDE;
    if (ShellExecuteExW(&se) && se.hProcess) {
        WaitForSingleObject(se.hProcess, 60000);
        CloseHandle(se.hProcess);
    }
    log(has_game_files(reg_game_dir()) ? L"Реестр: готово" : L"Реестр: записать не удалось — игра может не запуститься");
}

static void play() {
    fix_registry();
    apply_penki();
    std::wstring exe = pick_client();
    if (exe.empty() || !exists(g_dir + L"\\UOPlus.dll")) {
        MessageBoxW(g_wnd, L"UO+ не установлен в этой папке. Запустите установщик ещё раз.", TITLE, MB_ICONWARNING);
        return;
    }
    std::wstring full = g_dir + L"\\" + exe;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        std::wstring v = L"~ HIGHDPIAWARE";
        RegSetValueExW(k, full.c_str(), 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)((v.size() + 1) * 2));
        RegCloseKey(k);
    }
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    std::wstring cmd = L"\"" + full + L"\"";
    if (!CreateProcessW(full.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) {
        MessageBoxW(g_wnd, (L"Не удалось запустить " + exe).c_str(), TITLE, MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}
static void start_update() {
    if (g_busy) return;
    g_busy = true;
    EnableWindow(g_play, FALSE); EnableWindow(g_check, FALSE);
    SendMessageW(g_prog, PBM_SETPOS, 0, 0);
    CloseHandle(CreateThread(nullptr, 0, update_thread, nullptr, 0, nullptr));
}

// ---------------------------------------------------------------- окно «Настройки» (вкладки; всё применяется при следующем запуске игры)
// Владелец 2026-11-17: «сделать меню настроек более систематезированым и стандартным... подобные функции вынести в отдельное
// меню... туда же включить еще и пеньки... разрешения экрана в меню видео».
struct Opt { int tab; const wchar_t *sec, *key; int def; const wchar_t *label; HWND h; };
static Opt g_opts[] = {
    {1, L"patches", L"smoothwalk", 1, L"Плавная ходьба (в виртуальной машине лучше выключить)", nullptr},
    {1, L"patches", L"light", 1, L"Свет", nullptr},
    {1, L"patches", L"undead", 1, L"Undead (нежить)", nullptr},
    {1, L"journal", L"enabled", 1, L"Журнал UO+", nullptr},
    {2, L"bar", L"show", 1, L"Панель запасов (золото, реагенты, банки)", nullptr},
    {2, L"map", L"show", 0, L"Карта мира открыта при входе", nullptr},
    {2, L"map", L"dock", 1, L"Карта встроена в окно игры", nullptr},
    {2, L"map", L"mobiles", 1, L"Существа на карте", nullptr},
    {2, L"map", L"rotate", 1, L"Карта повёрнута как в игре", nullptr},
    {2, L"grid", L"button", 1, L"Значок # (сетка) в сумках", nullptr},
    {2, L"ui", L"options_button", 1, L"Кнопка «Опции» открывает окно UO+", nullptr},
    {3, L"timers", L"bandage", 0, L"Полоска бинта", nullptr},
    {3, L"timers", L"potion", 0, L"Полоска банки", nullptr},
    {3, L"timers", L"cast", 0, L"Полоска своего каста", nullptr},
    {3, L"timers", L"others", 0, L"Каст других игроков над головой", nullptr},
    {3, L"timers", L"target", 1, L"Строка цели с HP и метка над целью", nullptr},
    {3, L"timers", L"stealth", 1, L"Счётчик шагов в стелсе", nullptr},
    {3, L"timers", L"doors", 0, L"Открывать двери перед собой", nullptr},
    {4, L"loot", L"carve", 0, L"Сначала резать трупы (крис/нож)", nullptr},
    {4, L"loot", L"auto_open", 0, L"Сам открывать трупы рядом", nullptr},
    {4, L"loot", L"delay_orion", 1, L"Задержки как в Orion (повтор через 5 с, до 3 раз)", nullptr},
};
static const wchar_t *TABS[] = {L"Видео", L"Игра", L"Интерфейс", L"Полоски", L"Лут"};
enum { IDS_TAB = 100, IDS_OK, IDS_CANCEL, IDS_SIZE, IDS_DDRAW, IDS_RENDER, IDS_VSYNC, IDS_FPS, IDS_PENKI };
static HWND g_set, g_tab, s_size, s_ddraw, s_render, s_vsync, s_fps, s_penki;
static std::vector<std::pair<HWND, int>> g_tabctl;   // контрол -> вкладка
static std::vector<std::wstring> g_sizes;             // значения [launch] size по пунктам списка
static const wchar_t *RENDERERS[] = {L"auto", L"opengl", L"direct3d9", L"gdi"};
static const wchar_t *RENDER_NAMES[] = {L"Автоматически", L"OpenGL", L"Direct3D 9", L"GDI (самый совместимый)"};
static const int FPS[] = {-1, 60, 30};

static std::wstring ddraw_ini() { return g_dir + (exists(g_dir + L"\\ddraw.ini") ? L"\\ddraw.ini" : L"\\ddraw.ini.off"); }
static bool ddraw_on() { return exists(g_dir + L"\\ddraw.dll"); }
static bool ddraw_have() { return ddraw_on() || exists(g_dir + L"\\ddraw.dll.off"); }

static void set_tab(int t) {
    for (auto &c : g_tabctl) ShowWindow(c.first, c.second == t ? SW_SHOW : SW_HIDE);
}
static void sync_ddraw_ui() {
    BOOL on = SendMessageW(s_ddraw, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(s_render, on); EnableWindow(s_vsync, on); EnableWindow(s_fps, on);
}
static HWND s_add(HINSTANCE hi, HFONT f, int tab, const wchar_t *cls, const wchar_t *txt, DWORD st, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowW(cls, txt, WS_CHILD | st, x, y, w, h, g_set, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    g_tabctl.push_back({c, tab});
    return c;
}
static void settings_build(HINSTANCE hi) {
    HFONT f = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_tabctl.clear();
    g_tab = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 10, 10, 500, 330, g_set, (HMENU)IDS_TAB, hi, nullptr);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)f, TRUE);
    for (int i = 0; i < 5; i++) { TCITEMW it = {TCIF_TEXT}; it.pszText = (LPWSTR)TABS[i]; SendMessageW(g_tab, TCM_INSERTITEMW, i, (LPARAM)&it); }
    // Видео
    s_add(hi, f, 0, L"STATIC", L"Размер окна игры:", 0, 26, 50, 200, 20, 0);
    s_size = s_add(hi, f, 0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 230, 46, 260, 300, IDS_SIZE);
    g_sizes.clear();
    g_sizes.push_back(L"auto"); SendMessageW(s_size, CB_ADDSTRING, 0, (LPARAM)L"Авто: самый большой, что влезает");
    if (exists(g_dir + L"\\Client+inj_multi_3.0.0c_uoplus.exe")) { g_sizes.push_back(L"std"); SendMessageW(s_size, CB_ADDSTRING, 0, (LPARAM)L"Стандартный (маленький)"); }
    {
        std::vector<std::pair<int, int>> v;
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW((g_dir + L"\\Client+inj_multi_3.0.0c_*_uoplus.exe").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do { int w = 0, h = 0; std::wstring n = fd.cFileName;
                 if (swscanf(n.c_str() + 24, L"%dx%d", &w, &h) == 2
                     && lstrcmpiW(n.c_str(), (L"Client+inj_multi_3.0.0c_" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L"_uoplus.exe").c_str()) == 0)
                     v.push_back({w, h}); }
            while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
        std::sort(v.begin(), v.end());
        for (auto &p : v) {
            g_sizes.push_back(std::to_wstring(p.first) + L"x" + std::to_wstring(p.second));
            SendMessageW(s_size, CB_ADDSTRING, 0, (LPARAM)(std::to_wstring(p.first) + L" × " + std::to_wstring(p.second)).c_str());
        }
    }
    std::wstring cur = ini_str(L"launch", L"size", L"auto");
    int sel = 0;
    for (size_t i = 0; i < g_sizes.size(); i++) if (lstrcmpiW(g_sizes[i].c_str(), cur.c_str()) == 0) sel = (int)i;
    SendMessageW(s_size, CB_SETCURSEL, sel, 0);
    s_ddraw = s_add(hi, f, 0, L"BUTTON", L"Замена DirectDraw (cnc-ddraw) — для Мака, виртуальных машин и при рывках",
                    BS_AUTOCHECKBOX | WS_TABSTOP | BS_MULTILINE, 26, 88, 470, 40, IDS_DDRAW);
    SendMessageW(s_ddraw, BM_SETCHECK, ddraw_on() ? BST_CHECKED : BST_UNCHECKED, 0);
    if (!ddraw_have()) EnableWindow(s_ddraw, FALSE);
    s_add(hi, f, 0, L"STATIC", L"Отрисовка:", 0, 46, 138, 180, 20, 0);
    s_render = s_add(hi, f, 0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 230, 134, 260, 200, IDS_RENDER);
    for (auto n : RENDER_NAMES) SendMessageW(s_render, CB_ADDSTRING, 0, (LPARAM)n);
    wchar_t r[32]; GetPrivateProfileStringW(L"ddraw", L"renderer", L"auto", r, 32, ddraw_ini().c_str());
    int rs = 0; for (int i = 0; i < 4; i++) if (lstrcmpiW(r, RENDERERS[i]) == 0) rs = i;
    SendMessageW(s_render, CB_SETCURSEL, rs, 0);
    s_vsync = s_add(hi, f, 0, L"BUTTON", L"Вертикальная синхронизация (меньше рывков картинки)", BS_AUTOCHECKBOX | WS_TABSTOP, 46, 172, 440, 24, IDS_VSYNC);
    GetPrivateProfileStringW(L"ddraw", L"vsync", L"false", r, 32, ddraw_ini().c_str());
    SendMessageW(s_vsync, BM_SETCHECK, lstrcmpiW(r, L"true") == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    s_add(hi, f, 0, L"STATIC", L"Ограничение кадров:", 0, 46, 210, 180, 20, 0);
    s_fps = s_add(hi, f, 0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 230, 206, 260, 200, IDS_FPS);
    for (auto n : {L"Без ограничения", L"60 кадров в секунду", L"30 кадров в секунду"}) SendMessageW(s_fps, CB_ADDSTRING, 0, (LPARAM)n);
    int fps = GetPrivateProfileIntW(L"ddraw", L"maxfps", -1, ddraw_ini().c_str()), fs = 0;
    for (int i = 0; i < 3; i++) if (FPS[i] == fps) fs = i;
    SendMessageW(s_fps, CB_SETCURSEL, fs, 0);
    sync_ddraw_ui();
    // Игра: пеньки + галочки
    s_penki = s_add(hi, f, 1, L"BUTTON", L"Пеньки вместо деревьев", BS_AUTOCHECKBOX | WS_TABSTOP, 26, 50, 460, 24, IDS_PENKI);
    SendMessageW(s_penki, BM_SETCHECK, ini_int(L"launch", L"penki", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    if (verdata_custom()) { EnableWindow(s_penki, FALSE); SetWindowTextW(s_penki, L"Пеньки: verdata задана вручную в uoplus.ini [files]"); }
    int y[5] = {50, 80, 50, 50, 50};
    for (auto &o : g_opts) {
        int &yy = y[o.tab];
        o.h = s_add(hi, f, o.tab, L"BUTTON", o.label, BS_AUTOCHECKBOX | WS_TABSTOP, 26, yy, 470, 24, 0);
        SendMessageW(o.h, BM_SETCHECK, ini_int(o.sec, o.key, o.def) ? BST_CHECKED : BST_UNCHECKED, 0);
        yy += 30;
    }
    HWND n = CreateWindowW(L"STATIC", L"Применится при следующем запуске игры.", WS_CHILD | WS_VISIBLE, 14, 352, 300, 20, g_set, nullptr, hi, nullptr);
    SendMessageW(n, WM_SETFONT, (WPARAM)f, TRUE);
    HWND ok = CreateWindowW(L"BUTTON", L"Сохранить", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 318, 348, 100, 30, g_set, (HMENU)IDS_OK, hi, nullptr);
    HWND cn = CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 424, 348, 86, 30, g_set, (HMENU)IDS_CANCEL, hi, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)f, TRUE); SendMessageW(cn, WM_SETFONT, (WPARAM)f, TRUE);
    set_tab(0);
}
static void settings_save() {
    int sel = (int)SendMessageW(s_size, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)g_sizes.size()) ini_set(L"launch", L"size", g_sizes[sel]);
    for (auto &o : g_opts) ini_set(o.sec, o.key, SendMessageW(o.h, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    if (IsWindowEnabled(s_penki)) {
        bool on = SendMessageW(s_penki, BM_GETCHECK, 0, 0) == BST_CHECKED;
        ini_set(L"launch", L"penki", on ? L"1" : L"0");
        if (!on && !exists(g_dir + L"\\" + NOPENKI)) { log(L"Без пеньков: скачиваю нужный файл"); if (!g_busy) start_update(); }
    }
    if (ddraw_have()) {
        bool want = SendMessageW(s_ddraw, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (want != ddraw_on()) {
            const wchar_t *a = want ? L".off" : L"", *b = want ? L"" : L".off";
            for (const wchar_t *n : {L"\\ddraw.dll", L"\\ddraw.ini"})
                if (exists(g_dir + n + a)) MoveFileExW((g_dir + n + a).c_str(), (g_dir + n + b).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        std::wstring di = ddraw_ini();
        int r = (int)SendMessageW(s_render, CB_GETCURSEL, 0, 0), fs = (int)SendMessageW(s_fps, CB_GETCURSEL, 0, 0);
        WritePrivateProfileStringW(L"ddraw", L"renderer", RENDERERS[r < 0 ? 0 : r], di.c_str());
        WritePrivateProfileStringW(L"ddraw", L"vsync", SendMessageW(s_vsync, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"true" : L"false", di.c_str());
        WritePrivateProfileStringW(L"ddraw", L"maxfps", std::to_wstring(FPS[fs < 0 ? 0 : fs]).c_str(), di.c_str());
    }
    apply_penki();
    log(L"Настройки сохранены — применятся при следующем запуске игры");
}
static LRESULT CALLBACK setproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == IDS_TAB && ((NMHDR *)lp)->code == TCN_SELCHANGE) set_tab((int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0));
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDS_DDRAW) sync_ddraw_ui();
        else if (LOWORD(wp) == IDS_OK) { settings_save(); DestroyWindow(h); }
        else if (LOWORD(wp) == IDS_CANCEL) DestroyWindow(h);
        return 0;
    case WM_CTLCOLORSTATIC: {   // подписи и галочки — на белом, как страница вкладки
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        if (!IsWindowEnabled((HWND)lp)) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g_set = nullptr; EnableWindow(g_wnd, TRUE); SetForegroundWindow(g_wnd); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void settings_open() {
    if (g_set) { SetForegroundWindow(g_set); return; }
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = setproc; wc.hInstance = hi; wc.lpszClassName = L"UOPlusSettings";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc); reg = true;
    }
    RECT r; GetWindowRect(g_wnd, &r);
    RECT want = {0, 0, 520, 388};
    AdjustWindowRect(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    g_set = CreateWindowW(L"UOPlusSettings", L"UO+ — Настройки", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                          r.left + 20, r.top + 20, want.right - want.left, want.bottom - want.top, g_wnd, nullptr, hi, nullptr);
    settings_build(hi);
    EnableWindow(g_wnd, FALSE);
    ShowWindow(g_set, SW_SHOW);
}

enum { ID_PLAY = 1, ID_CHECK, ID_FOLDER, ID_SETTINGS };
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_LOG: {
        auto s = (std::wstring *)lp;
        int n = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, n, n);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)(*s + L"\r\n").c_str());
        delete s;
        return 0;
    }
    case WM_APP_PROG: SendMessageW(g_prog, PBM_SETPOS, wp, 0); return 0;
    case WM_APP_DONE: {
        g_busy = false;
        EnableWindow(g_play, TRUE); EnableWindow(g_check, TRUE);
        SendMessageW(g_prog, PBM_SETPOS, 1000, 0);
        int v = local_version();
        SetWindowTextW(g_ver, v ? (L"Установлена версия UO+: " + std::to_wstring(v)).c_str() : L"Версия UO+: неизвестна");
        SetFocus(g_play);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_PLAY) play();
        else if (LOWORD(wp) == ID_CHECK) start_update();
        else if (LOWORD(wp) == ID_FOLDER) ShellExecuteW(h, L"open", g_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (LOWORD(wp) == ID_SETTINGS) settings_open();
        return 0;
    case WM_CLOSE: if (g_busy && MessageBoxW(h, L"Идёт обновление. Прервать?", TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmdline, int) {
    g_dir = dir_of_self();
    DeleteFileW((self_path() + L".old").c_str());   // прошлая версия лаунчера после самообновления
    if (wcsstr(cmdline, L"/fixreg")) return write_reg() ? 0 : 1;   // вызов из fix_registry() с правами администратора
    if (wcsstr(cmdline, L"/play")) {   // ярлык «сразу играть» без окна (как старый UOPlus_Play)
        g_wnd = nullptr; play(); return 0;
    }
    INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&ic);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = L"UOPlusPlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(L"UOPlusPlay", TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                          560, 360, nullptr, nullptr, hi, nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT big = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HWND t = CreateWindowW(L"STATIC", L"UO+ для Age of Power", WS_CHILD | WS_VISIBLE, 16, 12, 400, 30, g_wnd, nullptr, hi, nullptr);
    SendMessageW(t, WM_SETFONT, (WPARAM)big, TRUE);
    int v = local_version();
    g_ver = CreateWindowW(L"STATIC", v ? (L"Установлена версия UO+: " + std::to_wstring(v)).c_str() : L"Версия UO+: неизвестна",
                          WS_CHILD | WS_VISIBLE, 16, 44, 400, 20, g_wnd, nullptr, hi, nullptr);
    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                            16, 70, 512, 160, g_wnd, nullptr, hi, nullptr);
    g_prog = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 16, 238, 512, 14, g_wnd, nullptr, hi, nullptr);
    SendMessageW(g_prog, PBM_SETRANGE32, 0, 1000);
    g_play = CreateWindowW(L"BUTTON", L"Играть", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 16, 264, 140, 40, g_wnd, (HMENU)ID_PLAY, hi, nullptr);
    g_check = CreateWindowW(L"BUTTON", L"Проверить обновления", WS_CHILD | WS_VISIBLE, 162, 264, 176, 40, g_wnd, (HMENU)ID_CHECK, hi, nullptr);
    HWND sb = CreateWindowW(L"BUTTON", L"Настройки", WS_CHILD | WS_VISIBLE, 344, 264, 92, 40, g_wnd, (HMENU)ID_SETTINGS, hi, nullptr);
    HWND fb = CreateWindowW(L"BUTTON", L"Папка игры", WS_CHILD | WS_VISIBLE, 442, 264, 86, 40, g_wnd, (HMENU)ID_FOLDER, hi, nullptr);
    for (HWND w : {g_ver, g_log, g_play, g_check, sb, fb}) SendMessageW(w, WM_SETFONT, (WPARAM)font, TRUE);
    ShowWindow(g_wnd, SW_SHOW);
    start_update();
    if (const wchar_t *st = wcsstr(cmdline, L"/settings")) {   // сразу окно настроек; /settings2 — с третьей вкладки
        settings_open();
        int t = (st[9] >= L'0' && st[9] <= L'4') ? st[9] - L'0' : 0;
        SendMessageW(g_tab, TCM_SETCURSEL, t, 0); set_tab(t);
    }
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    return 0;
}
