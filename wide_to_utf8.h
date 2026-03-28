#pragma once

#include <string>
#include <windows.h>
#include <optional>

inline std::optional<std::string> wide_to_utf8(std::wstring_view ws) noexcept {
    if (ws.empty()) return std::string{};

    int bytes = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS,
            ws.data(), (int)ws.size(),
            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return std::nullopt; // некорректная UTF-16 или другая ошибка [web:108]

    std::string out(bytes, '\0');
    int written = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS,
            ws.data(), (int)ws.size(),
            out.data(), bytes, nullptr, nullptr);
    if (written != bytes) return std::nullopt; //[web:108]

    return out;
}