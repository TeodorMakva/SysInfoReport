// OsSectionProviderLin.cpp
#include "OsSectionProvider.h"
#include "LinUtils.h"   // linutil::trim_copy

#include <sys/utsname.h>
#include <unistd.h>
#include <pwd.h>

#include <cstdlib>      // getenv
#include <cstring>      // memset
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/*
 * unescape_basic()
 * ---------------
 * Минимальная обработка escape-последовательностей для значений os-release в двойных кавычках.
 * Достаточно для типичных случаев: \" \\ \n \t \r.
 */
static std::string unescape_basic(std::string s) {
    std::string out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            switch (n) {
                case 'n': out.push_back('\n'); ++i; continue;
                case 't': out.push_back('\t'); ++i; continue;
                case 'r': out.push_back('\r'); ++i; continue;
                case '\\': out.push_back('\\'); ++i; continue;
                case '"': out.push_back('"'); ++i; continue;
                default:
                    break; // неизвестный escape — оставим как есть
            }
        }
        out.push_back(c);
    }
    return out;
}

/*
 * unquote()
 * ---------
 * Убирает внешние кавычки (одинарные/двойные) у значения KEY=VALUE из os-release.
 * Для двойных кавычек применяем unescape_basic().
 */
static std::string unquote(std::string v) {
    v = linutil::trim_copy(v);
    if (v.size() < 2) return v;

    const char q = v.front();
    if ((q == '"' || q == '\'') && v.back() == q) {
        std::string inner = v.substr(1, v.size() - 2);
        if (q == '"') inner = unescape_basic(std::move(inner));
        return inner;
    }
    return v;
}

/*
 * open_os_release()
 * -----------------
 * По стандарту файл os-release может лежать в /etc/os-release (приоритет)
 * или /usr/lib/os-release (fallback).
 */
static bool open_os_release(std::ifstream& f) {
    f.open("/etc/os-release");
    if (f.is_open()) return true;

    f.clear();
    f.open("/usr/lib/os-release");
    return f.is_open();
}

/*
 * os_release_map()
 * ----------------
 * Парсит os-release один раз и кеширует пары KEY=VALUE.
 * Это убирает повторное чтение файла при нескольких вызовах get_system_*.
 */
static const std::unordered_map<std::string, std::string>& os_release_map() {
    static const auto m = [] {
        std::unordered_map<std::string, std::string> map;

        std::ifstream f;
        if (!open_os_release(f)) return map;

        std::string line;
        while (std::getline(f, line)) {
            line = linutil::trim_copy(line);
            if (line.empty()) continue;
            if (!line.empty() && line[0] == '#') continue;

            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = linutil::trim_copy(line.substr(0, eq));
            std::string val = unquote(line.substr(eq + 1));

            if (!key.empty()) map[key] = val;
        }
        return map;
    }();

    return m;
}

static std::string os_release_value(const std::string& key) {
    const auto& m = os_release_map();
    const auto it = m.find(key);
    return (it == m.end()) ? std::string{} : it->second;
}

/*
 * read_uname()
 * ------------
 * Обёртка над uname() с предсказуемой инициализацией структуры.
 */
static bool read_uname(struct utsname& u) {
    std::memset(&u, 0, sizeof(u));
    return ::uname(&u) == 0;
}

} // namespace

std::string OsSectionProvider::get_system_name() const {
    // "Имя системы" стараемся сделать без версии:
    // NAME (Ubuntu/Debian/Arch) -> ID -> uname.sysname -> "Linux".
    std::string name = os_release_value("NAME");
    if (!name.empty()) return name;

    std::string id = os_release_value("ID");
    if (!id.empty()) return id;

    struct utsname u{};
    if (read_uname(u) && u.sysname[0]) return u.sysname;

    return "Linux";
}

std::string OsSectionProvider::get_system_version() const {
    // Формируем “богатую” строку:
    // <NAME + VERSION/VERSION_ID или PRETTY_NAME>; kernel <release>; arch <machine>
    struct utsname u{};
    const bool hasUname = read_uname(u);

    const std::string distroName = os_release_value("NAME");
    std::string distroVer = os_release_value("VERSION");
    if (distroVer.empty()) distroVer = os_release_value("VERSION_ID");

    std::string out;

    if (!distroName.empty() && !distroVer.empty()) {
        out += distroName + " " + distroVer;
    } else {
        const std::string pretty = os_release_value("PRETTY_NAME");
        if (!pretty.empty()) out += pretty;
        else if (!distroVer.empty()) out += distroVer;
        else out += "Unknown";
    }

    if (hasUname && u.release[0]) {
        out += "; kernel ";
        out += u.release;
    }

    if (hasUname && u.machine[0]) {
        out += "; arch ";
        out += u.machine;
    }

    return out.empty() ? "Unknown" : out;
}

std::string OsSectionProvider::get_user_name() const {
    // 1) Быстрый путь: переменная окружения USER.
    if (const char* env = std::getenv("USER"); env && *env) return env;

    // 2) Надёжный путь: getpwuid_r (потокобезопасная версия).
    const uid_t uid = ::getuid();

    long bufSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufSize < 1024) bufSize = 1024; // если sysconf вернул -1, берём разумный минимум

    std::vector<char> buf(static_cast<size_t>(bufSize));

    struct passwd pwd {};
    struct passwd* result = nullptr;

    const int rc = ::getpwuid_r(uid, &pwd, buf.data(), buf.size(), &result);
    if (rc == 0 && result && result->pw_name && result->pw_name[0]) {
        return result->pw_name;
    }

    return "Unknown";
}

std::string OsSectionProvider::get_computer_name() const {
    // gethostname() может не дописать '\0', если имя обрежется,
    // поэтому держим буфер с запасом и гарантируем завершающий ноль.
    long maxLen = ::sysconf(_SC_HOST_NAME_MAX);
    if (maxLen < 64) maxLen = 256;

    const size_t cap = static_cast<size_t>(maxLen) + 1;
    std::vector<char> buf(cap, '\0');

    if (::gethostname(buf.data(), cap - 1) != 0 || buf[0] == '\0') return "Unknown";
    buf.back() = '\0';

    return std::string(buf.data());
}
