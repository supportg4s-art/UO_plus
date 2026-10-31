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
struct MFile { std::string path, url, sha; long long size = 0; bool only_if_ours = false; };
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
static void play() {
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

enum { ID_PLAY = 1, ID_CHECK, ID_FOLDER };
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
        return 0;
    case WM_CLOSE: if (g_busy && MessageBoxW(h, L"Идёт обновление. Прервать?", TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmdline, int) {
    g_dir = dir_of_self();
    DeleteFileW((self_path() + L".old").c_str());   // прошлая версия лаунчера после самообновления
    if (wcsstr(cmdline, L"/play")) {   // ярлык «сразу играть» без окна (как старый UOPlus_Play)
        g_wnd = nullptr; play(); return 0;
    }
    INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
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
    g_play = CreateWindowW(L"BUTTON", L"Играть", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 16, 264, 170, 40, g_wnd, (HMENU)ID_PLAY, hi, nullptr);
    g_check = CreateWindowW(L"BUTTON", L"Проверить обновления", WS_CHILD | WS_VISIBLE, 196, 264, 190, 40, g_wnd, (HMENU)ID_CHECK, hi, nullptr);
    HWND fb = CreateWindowW(L"BUTTON", L"Папка игры", WS_CHILD | WS_VISIBLE, 396, 264, 132, 40, g_wnd, (HMENU)ID_FOLDER, hi, nullptr);
    for (HWND w : {g_ver, g_log, g_play, g_check, fb}) SendMessageW(w, WM_SETFONT, (WPARAM)font, TRUE);
    ShowWindow(g_wnd, SW_SHOW);
    start_update();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    return 0;
}
