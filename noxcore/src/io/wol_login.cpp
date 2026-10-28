// Reading and writing the saved Westwood Online logins in the registry (see wol_login.hpp).
#include "noxcore/io/wol_login.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <string_view>

namespace nox::io {

namespace {

#ifdef _WIN32
// A REG_SZ value as ASCII; empty when missing.
std::string registryText(HKEY root, const std::wstring& key, const wchar_t* value)
{
    DWORD size = 0;
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring w(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ, nullptr, w.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    w.resize(wcsnlen(w.c_str(), w.size()));
    std::string out;
    for (const wchar_t c : w) {
        out += c < 0x80 ? static_cast<char>(c) : '?'; // (nicks are ASCII)
    }
    return out;
}

/// Wchat\Nick<n>\<what>'s default value under HKEY_CLASSES_ROOT.
std::string wchatText(int n, const wchar_t* what)
{
    const std::wstring key = L"Wchat\\Nick" + std::to_wstring(n) + L"\\" + what;
    return registryText(HKEY_CLASSES_ROOT, key, nullptr);
}

bool setDefault(const std::wstring& key, const std::string& text)
{
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    // (ASCII, as WOLAPI writes it with RegSetValueExA)
    const LONG r = RegSetValueExA(h, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
                                  static_cast<DWORD>(text.size() + 1));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}
#endif

} // namespace

std::vector<SavedLogin> savedLogins()
{
    std::vector<SavedLogin> out;
#ifdef _WIN32
    // Slots 1 to 129, the last being the unsaved-login slot.
    for (int n = 1; n <= kUnsavedLoginSlot + 1; ++n) {
        out.push_back({wchatText(n, L"Nick"), wchatText(n, L"Pass")});
    }
#endif
    return out;
}

bool saveLogin(int index, const SavedLogin& login)
{
#ifdef _WIN32
    if (index < 0 || index > kUnsavedLoginSlot) {
        return false;
    }
    const std::wstring base = L"Software\\Classes\\Wchat\\Nick" + std::to_wstring(index + 1) + L"\\";
    return setDefault(base + L"Nick", login.nick) && setDefault(base + L"Pass", login.password);
#else
    (void)index;
    (void)login;
    return false;
#endif
}

std::vector<std::string> savedOnlineNicks()
{
    std::vector<std::string> out;
#ifdef _WIN32
    // Where WOLAPI keeps them: the login list's slots.
    for (int n = 1; n <= kSavedLoginSlots; ++n) {
        if (std::string nick = wchatText(n, L"Nick"); !nick.empty() && std::ranges::find(out, nick) == out.end()) {
            out.push_back(std::move(nick));
        }
    }
    struct Place {
        HKEY root;
        const wchar_t* path;
    };
    static const Place kPlaces[] = { // (the HKEYs are casts: not constexpr)
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Westwood\\WOLAPI"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Westwood\\WOLAPI"},
        {HKEY_CURRENT_USER, L"Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node\\Westwood\\WOLAPI"},
        {HKEY_CURRENT_USER, L"Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\Westwood\\WOLAPI"},
        {HKEY_CURRENT_USER, L"Software\\Westwood\\WOLAPI"},
    };
    for (const auto& place : kPlaces) {
        for (int n = 0; n <= 9; ++n) {
            const std::wstring base = std::wstring(place.path) + L"\\Wchat\\Nick" + std::to_wstring(n);
            std::string nick = registryText(place.root, base, L"Nick");
            if (nick.empty()) {
                // (the names may be values of the WOLAPI key itself)
                const std::wstring value = L"Wchat\\Nick" + std::to_wstring(n) + L"\\Nick";
                nick = registryText(place.root, place.path, value.c_str());
            }
            if (!nick.empty() && std::ranges::find(out, nick) == out.end()) {
                out.push_back(nick);
            }
        }
    }
#endif
    return out;
}

} // namespace nox::io
