#include "config.h"

#include <windows.h>
#include <dpapi.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "net.h"
#include "nlohmann/json.hpp"

namespace disky {

namespace {

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"disky";

std::wstring configPath(bool create) {
    PWSTR appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) return {};
    std::wstring dir = std::wstring(appData) + L"\\disky";
    CoTaskMemFree(appData);
    if (create) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.json";
}

std::string protect(const std::string& plain) {
    DATA_BLOB in{DWORD(plain.size()), (BYTE*)plain.data()}, out{};
    if (!CryptProtectData(&in, L"disky token", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    DWORD n = 0;
    CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string b64(n, '\0');
    CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &n);
    b64.resize(n);
    LocalFree(out.pbData);
    return b64;
}

std::string unprotect(const std::string& b64) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(b64.c_str(), DWORD(b64.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr))
        return {};
    std::string raw(n, '\0');
    CryptStringToBinaryA(b64.c_str(), DWORD(b64.size()), CRYPT_STRING_BASE64, (BYTE*)raw.data(), &n, nullptr, nullptr);
    DATA_BLOB in{n, (BYTE*)raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string plain((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

}  // namespace

Config LoadConfig() {
    Config c;
    std::ifstream f(configPath(false), std::ios::binary);
    if (!f) return c;
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) return c;
    c.token = unprotect(j.value("token", ""));
    c.buildNumber = j.value("buildNumber", 0);
    c.buildDay = j.value("buildDay", int64_t(0));
    c.x = j.value("x", 0);
    c.y = j.value("y", 0);
    c.w = j.value("w", 0);
    c.h = j.value("h", 0);
    c.maximised = j.value("maximised", false);
    c.guild = std::strtoull(j.value("guild", "0").c_str(), nullptr, 10);
    c.channel = std::strtoull(j.value("channel", "0").c_str(), nullptr, 10);
    c.notify = j.value("notify", true);
    c.members = j.value("members", true);
    c.volume = j.value("volume", 0.5f);
    if (auto it = j.find("recentEmoji"); it != j.end() && it->is_array())
        for (const auto& e : *it)
            if (e.is_string()) c.recentEmoji.push_back(e.get<std::string>());
    return c;
}

bool SaveConfig(const Config& c) {
    nlohmann::json j = {
        {"token", c.token.empty() ? std::string() : protect(c.token)},
        {"buildNumber", c.buildNumber},
        {"buildDay", c.buildDay},
        {"x", c.x},
        {"y", c.y},
        {"w", c.w},
        {"h", c.h},
        {"maximised", c.maximised},
        {"guild", std::to_string(c.guild)},
        {"channel", std::to_string(c.channel)},
        {"notify", c.notify},
        {"members", c.members},
        {"volume", c.volume},
        {"recentEmoji", c.recentEmoji},
    };
    std::wstring path = configPath(true);
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(2);
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool StartsWithWindows() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return on;
}

void SetStartWithWindows(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

}  // namespace disky
