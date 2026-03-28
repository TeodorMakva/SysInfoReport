// LinUtils.h
#pragma once

#include <cctype>
#include <cerrno>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef __linux__
#include <unistd.h> // readlink
#endif

namespace linutil {

/*
 * trim_copy()
 * ----------
 * Убирает пробельные символы по краям (space/tab/CR/LF).
 */
inline std::string trim_copy(std::string_view s) {
    const auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };

    size_t b = 0;
    while (b < s.size() && is_ws(static_cast<unsigned char>(s[b]))) ++b;
    if (b == s.size()) return {};

    size_t e = s.size();
    while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;

    return std::string(s.substr(b, e - b));
}

/*
 * starts_with()
 * ------------
 * C++17-совместимая проверка префикса.
 */
inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

/*
 * split_kv_colon()
 * ----------------
 * Парсит строки формата "key: value" (как в /proc/cpuinfo).
 */
inline bool split_kv_colon(std::string_view line, std::string& key, std::string& val) {
    const size_t pos = line.find(':');
    if (pos == std::string_view::npos) return false;

    key = trim_copy(line.substr(0, pos));
    val = trim_copy(line.substr(pos + 1));
    return true;
}

/*
 * read_first_line()
 * -----------------
 * Читает первую строку файла (и trim'ит).
 */
inline bool read_first_line(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    if (!std::getline(f, line)) return false;

    out = trim_copy(line);
    return true;
}

/*
 * read_text_file()
 * ---------------
 * Читает файл целиком в строку.
 */
inline bool read_text_file(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;

    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

/*
 * read_int_file()
 * --------------
 * Читает int из файла (типичный sysfs: "0\n", "3\n"...).
 */
inline bool read_int_file(const std::string& path, int& out) {
    std::string s;
    if (!read_first_line(path, s) || s.empty()) return false;

    try {
        out = std::stoi(s);
        return true;
    } catch (...) {
        return false;
    }
}

/*
 * read_u64_file()
 * --------------
 * Читает unsigned long long из файла (типичный sysfs: большие числа в байтах).
 */
inline bool read_u64_file(const std::string& path, unsigned long long& out) {
    std::ifstream f(path);
    if (!f) return false;

    unsigned long long v = 0;
    f >> v;
    if (!f) return false;

    out = v;
    return true;
}

/*
 * looks_like_human_name()
 * -----------------------
 * Мини-защита от чисто числовых значений: "имя" обычно содержит буквы.
 */
inline bool looks_like_human_name(std::string_view s) {
    for (unsigned char c : s) {
        if (std::isalpha(c)) return true;
    }
    return false;
}

/*
 * find_env_value()
 * ---------------
 * Ищет в многострочном тексте строку вида KEY=VALUE и возвращает VALUE (trim).
 * Удобно для sysfs uevent.
 */
inline std::optional<std::string> find_env_value(std::string_view text, std::string_view key) {
    const std::string prefix = std::string(key) + "=";

    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string_view::npos) j = text.size();

        std::string_view line = text.substr(i, j - i);
        if (starts_with(line, prefix)) {
            auto v = trim_copy(line.substr(prefix.size()));
            return v;
        }

        i = (j < text.size()) ? (j + 1) : j;
    }

    return std::nullopt;
}

/*
 * normalize_hex_id()
 * -----------------
 * Приводит идентификатор к виду "10de" (strip 0x, lower-case, trim).
 */
inline std::string normalize_hex_id(std::string s) {
    s = trim_copy(s);
    if (starts_with(s, "0x") || starts_with(s, "0X")) {
        s = s.substr(2);
    }

    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/*
 * parse_pci_id()
 * -------------
 * Парсит PCI_ID вида "10DE:2484" или "10DE/2484".
 */
inline bool parse_pci_id(std::string_view s, std::string& vendor, std::string& device) {
    s = trim_copy(s);

    const size_t pos = s.find(':') != std::string::npos ? s.find(':') : s.find('/');
    if (pos == std::string::npos) return false;

    std::string v = std::string(s.substr(0, pos));
    std::string d = std::string(s.substr(pos + 1));

    v = normalize_hex_id(std::move(v));
    d = normalize_hex_id(std::move(d));

    if (v.empty() || d.empty()) return false;

    vendor = std::move(v);
    device = std::move(d);
    return true;
}

/*
 * read_symlink_basename()
 * ----------------------
 * Читает symlink и возвращает basename target'а (после последнего '/').
 * Например: /sys/.../device/driver -> /sys/bus/pci/drivers/nvidia => "nvidia"
 */
inline bool read_symlink_basename(const std::string& path, std::string& out) {
#ifdef __linux__
    std::vector<char> buf(4096, '\0');
    const ssize_t n = ::readlink(path.c_str(), buf.data(), buf.size() - 1);
    if (n <= 0) return false;

    buf[static_cast<size_t>(n)] = '\0';
    std::string target(buf.data());

    const size_t slash = target.find_last_of('/');
    out = (slash == std::string::npos) ? target : target.substr(slash + 1);
    out = trim_copy(out);
    return !out.empty();
#else
    (void)path;
    (void)out;
    return false;
#endif
}

/*
 * format_mb_from_bytes()
 * ---------------------
 * Утилита форматирования (MB с 1 знаком после запятой).
 */
inline std::string format_mb_from_bytes(unsigned long long bytes) {
    const double mb = static_cast<double>(bytes) / 1024.0 / 1024.0;
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(1);
    ss << mb << " MB";
    return ss.str();
}

inline std::string format_bytes_2dp(unsigned long long bytes) {
    const double KB = 1024.0;
    const double MB = KB * 1024.0;
    const double GB = MB * 1024.0;
    const double TB = GB * 1024.0;

    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(2);

    if (bytes >= (unsigned long long)TB) ss << (bytes / TB) << " TB";
    else if (bytes >= (unsigned long long)GB) ss << (bytes / GB) << " GB";
    else if (bytes >= (unsigned long long)MB) ss << (bytes / MB) << " MB";
    else ss << (bytes / KB) << " KB";

    return ss.str();
}

} // namespace linutil
