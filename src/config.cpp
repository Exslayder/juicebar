#include "config.h"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace ch {
namespace {

std::wstring ExeDir() {
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof(path) / sizeof(path[0])));
    std::wstring full(path, n);
    size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(0, slash);
}

std::wstring Environment(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return n && n < sizeof(buf) / sizeof(buf[0]) ? std::wstring(buf, n) : L"";
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Copy the JuiceBar settings/history once, when WattPill starts for the first time.
void MigrateLegacy(const Paths& p) {
    const std::wstring oldBase = Environment(L"APPDATA") + L"\\JuiceBar";
    const std::wstring oldData = Environment(L"LOCALAPPDATA") + L"\\JuiceBar";
    const std::wstring oldSettings = oldBase + L"\\settings.ini";
    const std::wstring oldHistory = oldData + L"\\history.tsv";
    if (!FileExists(p.settingsFile) && FileExists(oldSettings)) {
        EnsureDirectory(p.baseDir);
        CopyFileW(oldSettings.c_str(), p.settingsFile.c_str(), FALSE);
    }
    if (!FileExists(p.historyFile) && FileExists(oldHistory)) {
        EnsureDirectory(p.dataDir);
        CopyFileW(oldHistory.c_str(), p.historyFile.c_str(), FALSE);
    }
}

std::string ReadAll(const std::wstring& file) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

int ToInt(const std::wstring& s, int fallback) {
    if (s.empty()) return fallback;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    return end && *end == 0 ? (int)v : fallback;
}

} // namespace

bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::wstring current;
    for (size_t i = 0; i < dir.size(); i++) {
        current.push_back(dir[i]);
        if (dir[i] == L'\\' || dir[i] == L'/' || i + 1 == dir.size()) {
            if (current.size() <= 3) continue;   // "C:\"
            if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                if (GetLastError() != ERROR_PATH_NOT_FOUND) return false;
            }
        }
    }
    return true;
}

Paths ResolvePaths(const std::wstring& overrideDir) {
    Paths p;
    const std::wstring exeDir = ExeDir();
    const bool portable = FileExists(exeDir + L"\\portable.flag");

    if (!overrideDir.empty()) {
        p.baseDir = p.dataDir = overrideDir;
    } else if (portable) {
        p.baseDir = p.dataDir = exeDir;
    } else {
        p.baseDir = Environment(L"APPDATA") + L"\\WattPill";
        p.dataDir = Environment(L"LOCALAPPDATA") + L"\\WattPill";
    }
    EnsureDirectory(p.baseDir);
    EnsureDirectory(p.dataDir);
    p.settingsFile = p.baseDir + L"\\settings.ini";
    p.historyFile = p.dataDir + L"\\history.tsv";
    if (overrideDir.empty() && !portable) MigrateLegacy(p);
    return p;
}

Settings LoadSettings(const std::wstring& file) {
    Settings s;
    const std::string raw = ReadAll(file);
    if (raw.empty()) return s;

    // UTF-8 without BOM; keys are ASCII.
    std::wstring text = Wide(raw);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == L'[' || line[0] == L';' || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq));
        std::wstring value = Trim(line.substr(eq + 1));

        if (key == L"poll_seconds") s.pollSeconds = ToInt(value, s.pollSeconds);
        else if (key == L"show_pill") s.showPill = ToInt(value, 1) != 0;
        else if (key == L"dim_sleeping") s.dimSleeping = ToInt(value, 1) != 0;
        else if (key == L"autostart") s.autoStart = ToInt(value, 1) != 0;
        else if (key == L"pill_placement") s.pillPlacement = ToInt(value, 0);
        else if (key == L"language") s.language = ToInt(value, 0);
    }

    s.pollSeconds = std::clamp(s.pollSeconds, 15, 900);
    // The app follows the Windows theme at runtime; only the language is user-selectable.
    s.language = std::clamp(s.language, 0, 2);
    s.pillPlacement = std::clamp(s.pillPlacement, 0, 1);
    return s;
}

bool SaveSettings(const std::wstring& file, const Settings& s) {
    std::string text;
    text += "# WattPill settings\n";
    text += "[general]\n";
    text += "poll_seconds=" + std::to_string(std::clamp(s.pollSeconds, 15, 900)) + "\n";
    text += std::string("show_pill=") + (s.showPill ? "1" : "0") + "\n";
    text += std::string("dim_sleeping=") + (s.dimSleeping ? "1" : "0") + "\n";
    text += std::string("autostart=") + (s.autoStart ? "1" : "0") + "\n";
    text += "pill_placement=" + std::to_string(std::clamp(s.pillPlacement, 0, 1)) + "\n";
    text += "language=" + std::to_string(s.language) + "\n";

    const std::wstring tmp = file + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool TaskbarIsLight() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 0, size = sizeof(value), type = 0;
    bool light = false;
    if (RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
        light = value != 0;
    RegCloseKey(key);
    return light;
}

} // namespace ch
