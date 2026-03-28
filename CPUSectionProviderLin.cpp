// CPUSectionProviderLin.cpp
#include "CPUSectionProvider.h"
#include "LinUtils.h"   // linutil::trim_copy, split_kv_colon, read_int_file, looks_like_human_name

#include <unistd.h>     // sysconf()
#include <dirent.h>     // opendir/readdir/closedir

#include <cctype>       // isdigit
#include <fstream>      // ifstream
#include <set>          // set
#include <string>       // string

namespace {

/*
 * is_cpu_dir_name()
 * -----------------
 * В /sys/devices/system/cpu/ есть каталоги cpu0, cpu1, cpu2... и служебные каталоги
 * (cpufreq, cpuidle и т.п.). Нам нужны только cpuN.
 */
static bool is_cpu_dir_name(const std::string& name) {
    if (name.size() < 4) return false;          // минимум "cpu0"
    if (name.rfind("cpu", 0) != 0) return false;

    for (size_t i = 3; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isdigit(c)) return false;
    }
    return true;
}

/*
 * count_physical_cores_sysfs()
 * ---------------------------
 * Считает количество ФИЗИЧЕСКИХ ядер через sysfs topology:
 *   /sys/devices/system/cpu/cpuN/topology/core_id
 *   /sys/devices/system/cpu/cpuN/topology/physical_package_id
 *
 * Идея:
 * - Каждому логическому CPU соответствует каталог cpuN.
 * - Для каждого cpuN читаем core_id (ядро) и physical_package_id (сокет).
 * - Уникальные пары (package_id, core_id) = физические ядра в системе.
 *
 * Возвращает:
 * - >= 1 при успехе
 * - -1 если topology недоступна/непрочиталась
 */
static int count_physical_cores_sysfs() {
    DIR* d = ::opendir("/sys/devices/system/cpu");
    if (!d) return -1;

    std::set<std::pair<int, int>> packageCore; // (physical_package_id, core_id)
    std::set<int> coreOnly;                   // fallback: если package_id не доступен

    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (!is_cpu_dir_name(name)) continue;

        const std::string topo = "/sys/devices/system/cpu/" + name + "/topology/";

        int coreId = -1;
        int pkgId  = -1;

        const bool hasCore = linutil::read_int_file(topo + "core_id", coreId);
        const bool hasPkg  = linutil::read_int_file(topo + "physical_package_id", pkgId);

        if (hasCore && hasPkg && coreId >= 0 && pkgId >= 0) {
            packageCore.emplace(pkgId, coreId);
        } else if (hasCore && coreId >= 0) {
            // Иногда physical_package_id не доступен, но core_id есть.
            coreOnly.emplace(coreId);
        }
    }

    ::closedir(d);

    if (!packageCore.empty()) return static_cast<int>(packageCore.size());
    if (!coreOnly.empty())    return static_cast<int>(coreOnly.size());
    return -1;
}

/*
 * count_physical_cores_proc_cpuinfo()
 * ----------------------------------
 * Старый/fallback способ: /proc/cpuinfo.
 * На x86 часто присутствуют поля:
 *   physical id : 0
 *   core id     : 3
 * Тогда уникальные пары (physical id, core id) = физические ядра.
 *
 * Возвращает >=1 при успехе, иначе -1.
 */
static int count_physical_cores_proc_cpuinfo() {
    std::ifstream f("/proc/cpuinfo");
    if (!f) return -1;

    std::set<std::pair<int,int>> physCores;

    std::string line;
    std::string key, val;

    int physicalId = -1;
    int coreId = -1;

    auto flush = [&]() {
        if (physicalId >= 0 && coreId >= 0) {
            physCores.emplace(physicalId, coreId);
        }
        physicalId = -1;
        coreId = -1;
    };

    while (std::getline(f, line)) {
        if (line.empty()) {   // пустая строка разделяет блоки разных логических CPU
            flush();
            continue;
        }

        if (!linutil::split_kv_colon(line, key, val)) continue;

        if (key == "physical id") {
            try { physicalId = std::stoi(val); } catch (...) {}
        } else if (key == "core id") {
            try { coreId = std::stoi(val); } catch (...) {}
        }
    }
    flush();

    if (physCores.empty()) return -1;
    return static_cast<int>(physCores.size());
}

/*
 * count_logical_cpus_fallback()
 * ----------------------------
 * sysconf(_SC_NPROCESSORS_ONLN) возвращает количество online ЛОГИЧЕСКИХ CPU (потоков).
 * Это НЕ физические ядра, но хороший последний fallback.
 */
static long count_logical_cpus_fallback() {
    return ::sysconf(_SC_NPROCESSORS_ONLN);
}

} // namespace

std::string CPUSectionProvider::get_cpu_name() const {
    // /proc/cpuinfo обычно содержит человекочитаемое имя CPU.
    // Важно: НЕ использовать ключ "model" (это число и идёт раньше "model name" на x86).
    std::ifstream f("/proc/cpuinfo");
    if (!f) return "Unknown";

    std::string line;
    std::string key, val;

    std::string candidate_hardware;   // ARM fallback
    std::string candidate_processor;  // ARM fallback

    while (std::getline(f, line)) {
        if (!linutil::split_kv_colon(line, key, val)) continue;
        if (val.empty()) continue;

        // x86/x86_64: то самое "Intel(R) Core(TM) ..."
        if (key == "model name" && linutil::looks_like_human_name(val)) {
            return val;
        }

        // ARM: часто присутствуют Hardware/Processor
        if (key == "Hardware" && candidate_hardware.empty() && linutil::looks_like_human_name(val)) {
            candidate_hardware = val;
        }
        if (key == "Processor" && candidate_processor.empty() && linutil::looks_like_human_name(val)) {
            candidate_processor = val;
        }
    }

    if (!candidate_hardware.empty()) return candidate_hardware;
    if (!candidate_processor.empty()) return candidate_processor;

    return "Unknown";
}

std::string CPUSectionProvider::get_cpu_core_count() const {
    // Пытаемся получить количество ФИЗИЧЕСКИХ ядер:
    // 1) sysfs topology (лучше всего на Linux)
    // 2) /proc/cpuinfo physical id/core id
    // 3) fallback: логические CPU (потоки)
    if (const int n = count_physical_cores_sysfs(); n > 0) {
        return std::to_string(n);
    }

    if (const int n = count_physical_cores_proc_cpuinfo(); n > 0) {
        return std::to_string(n);
    }

    const long n = count_logical_cpus_fallback();
    return (n > 0) ? std::to_string(n) : "Unknown";
}
