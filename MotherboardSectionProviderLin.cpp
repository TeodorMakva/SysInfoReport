// MotherboardSectionProviderLin.cpp
#include "MotherboardSectionProvider.h"
#include "LinUtils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

/*
 * DMI/SMBIOS поля на Linux доступны через sysfs.
 * Обычно /sys/class/dmi/id/* — symlink на /sys/devices/virtual/dmi/id/*,
 * но в контейнерах/урезанных окружениях может присутствовать только один из путей.
 */
static const char* kDmiBases[] = {
    "/sys/class/dmi/id",
    "/sys/devices/virtual/dmi/id"
};

/*
 * BIOS/UEFI иногда возвращает “заглушки” вместо реальных значений.
 * Такие строки лучше игнорировать, иначе отчёт выглядит как мусор.
 */
static bool is_placeholder_value(std::string_view v) {
    if (v.empty()) return true;

    // Часто встречающиеся заглушки (сравнение строгое, без lower-case).
    static const char* bad[] = {
        "None",
        "Unknown",
        "Not Specified",
        "Default string",
        "System Product Name",
        "System manufacturer",
        "OEM",
        "To be filled by O.E.M.",
        "To Be Filled By O.E.M.",
        "To be filled by OEM",
        "N/A",
        "NA",
        "0"
    };

    for (auto* x : bad) {
        if (v == x) return true;
    }

    return false;
}

/*
 * read_dmi_field()
 * ---------------
 * Читает DMI поле (первую “осмысленную” строку) из одного из базовых путей.
 *
 * Пример: read_dmi_field("board_name") ищет:
 *   /sys/class/dmi/id/board_name
 *   /sys/devices/virtual/dmi/id/board_name
 */
static std::string read_dmi_field(const std::string& leaf) {
    for (auto* base : kDmiBases) {
        std::string value;
        if (!linutil::read_first_line(std::string(base) + "/" + leaf, value)) continue;

        value = linutil::trim_copy(value);
        if (!is_placeholder_value(value)) return value;
    }
    return {};
}

/*
 * push_unique()
 * ------------
 * Добавляет кусок в список, если он не пустой и ещё не был добавлен.
 * Это помогает избежать повторов, например когда sys_vendor == board_vendor.
 */
static void push_unique(std::vector<std::string>& parts, std::string s) {
    s = linutil::trim_copy(s);
    if (s.empty()) return;

    if (std::find(parts.begin(), parts.end(), s) == parts.end()) {
        parts.push_back(std::move(s));
    }
}

/*
 * join_parts()
 * -----------
 * Склеивает части через пробел в одну строку.
 */
static std::string join_parts(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += " ";
        out += parts[i];
    }
    return out;
}

/*
 * build_motherboard_series()
 * -------------------------
 * Пытается собрать “человекочитаемую” строку для "Серия".
 *
 * Приоритет:
 *   1) Baseboard: board_vendor + board_name (+ board_version)
 *   2) Fallback:  sys_vendor + product_name (+ product_version)
 */
static std::string build_motherboard_series() {
    // 1) Baseboard поля (аналог Win32_BaseBoard.Product на Windows по смыслу).
    const std::string board_vendor  = read_dmi_field("board_vendor");
    const std::string board_name    = read_dmi_field("board_name");
    const std::string board_version = read_dmi_field("board_version");

    std::vector<std::string> parts;

    // Важно: иногда vendor пустой, а board_name нормальный — показываем и так.
    push_unique(parts, board_vendor);
    push_unique(parts, board_name);

    // Версию платы добавляем только если уже есть хоть что-то.
    if (!parts.empty() && !board_version.empty()) {
        push_unique(parts, board_version);
    }

    if (!parts.empty()) {
        return join_parts(parts);
    }

    // 2) System/product поля (часто заполняются, даже если baseboard пустой).
    const std::string sys_vendor   = read_dmi_field("sys_vendor");
    const std::string product_name = read_dmi_field("product_name");
    const std::string product_ver  = read_dmi_field("product_version");

    parts.clear();
    push_unique(parts, sys_vendor);
    push_unique(parts, product_name);
    if (!parts.empty() && !product_ver.empty()) push_unique(parts, product_ver);

    if (!parts.empty()) {
        return join_parts(parts);
    }

    return "Unknown";
}

/*
 * motherboard_cache()
 * ------------------
 * Простой ленивый кэш.
 *
 * Важно: вариант с `static bool inited` НЕ thread-safe, но для однопоточного
 * SysInfoReport это нормально и максимально надёжно (без std::call_once).
 */
static const std::string& motherboard_cache() {
    static std::string cached;
    static bool inited = false;

    if (!inited) {
        cached = build_motherboard_series();
        if (cached.empty()) cached = "Unknown";
        inited = true;
    }

    return cached;
}

} // namespace

std::string MotherboardSectionProvider::get_motherboard_series() const {
    return motherboard_cache();
}
