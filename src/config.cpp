#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

std::string appDataDir() {
    wchar_t* base = nullptr;
    std::string dir;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) {
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, base, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            dir.resize(static_cast<size_t>(n - 1));
            ::WideCharToMultiByte(CP_UTF8, 0, base, -1, dir.data(), n, nullptr, nullptr);
        }
        ::CoTaskMemFree(base);
    }
    if (dir.empty()) {
        const char* env = std::getenv("APPDATA");
        if (env) dir = env;
    }
    return dir;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring exePathQuoted() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(L"\"") + buf + L"\" --tray";
}

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"openmix";

}  // namespace

std::string Config::path() {
    const std::string dir = appDataDir();
    if (dir.empty()) return {};
    return dir + "\\openmix\\config.ini";
}

void Config::load() {
    const std::string p = path();
    if (p.empty()) return;
    std::ifstream f(p);
    if (!f) return;
    existed_ = true;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        values_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

bool Config::save() const {
    const std::string p = path();
    if (p.empty()) return false;

    // Create %APPDATA%\openmix on first save.
    const size_t slash = p.find_last_of('\\');
    if (slash != std::string::npos) {
        const std::string dir = p.substr(0, slash);
        ::CreateDirectoryA(dir.c_str(), nullptr);
    }

    std::ofstream f(p, std::ios::trunc);
    if (!f) return false;
    f << "# openmix settings\n";
    for (const auto& [k, v] : values_) f << k << " = " << v << "\n";
    return f.good();
}

std::string Config::get(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

float Config::getFloat(const std::string& key, float fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    try {
        return std::stof(it->second);
    } catch (...) {
        return fallback;
    }
}

bool Config::getBool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    return it->second == "1" || it->second == "true" || it->second == "yes";
}

void Config::set(const std::string& key, const std::string& value) {
    values_[key] = value;
}

void Config::setFloat(const std::string& key, float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", value);
    values_[key] = buf;
}

void Config::setBool(const std::string& key, bool value) {
    values_[key] = value ? "1" : "0";
}

// ---- autostart ---------------------------------------------------------

bool autostartEnabled() {
    HKEY key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LSTATUS st = ::RegQueryValueExW(key, kRunValue, nullptr, &type,
                                          reinterpret_cast<BYTE*>(buf), &size);
    ::RegCloseKey(key);
    return st == ERROR_SUCCESS && type == REG_SZ;
}

bool setAutostart(bool enable) {
    HKEY key{};
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                          KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS st;
    if (enable) {
        const std::wstring cmd = exePathQuoted();
        if (cmd.empty()) {
            ::RegCloseKey(key);
            return false;
        }
        st = ::RegSetValueExW(key, kRunValue, 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(cmd.c_str()),
                              static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = ::RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    ::RegCloseKey(key);
    return st == ERROR_SUCCESS;
}
