// GPUSectionProviderLin.cpp
#include "GPUSectionProvider.h"
#include "LinUtils.h"

#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

/*
 * В /sys/class/drm/ кроме видеокарт есть коннекторы и render-устройства.
 * Нам нужны только "cardN" (без "-DP-1" и т.п.).
 */
static bool is_drm_card_name(const std::string& name) {
    if (name.size() < 5) return false;                  // минимум "card0"
    if (name.find('-') != std::string::npos) return false;
    if (!linutil::starts_with(name, "card")) return false;

    for (size_t i = 4; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isdigit(c)) return false;
    }
    return true;
}

static std::vector<std::string> list_drm_cards() {
    std::vector<std::string> cards;

    DIR* d = ::opendir("/sys/class/drm");
    if (!d) return cards;

    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (!is_drm_card_name(name)) continue;

        // sanity-check: у реальной PCI/SoC GPU обычно есть vendor.
        std::string vendor;
        if (linutil::read_first_line("/sys/class/drm/" + name + "/device/vendor", vendor)) {
            cards.push_back(name);
        }
    }

    ::closedir(d);
    std::sort(cards.begin(), cards.end());
    return cards;
}

// Мини-подсказка производителя по vendor ID без внешних баз.
static const char* vendor_name(std::string_view vendorHex) {
    if (vendorHex == "10de") return "NVIDIA";
    if (vendorHex == "1002" || vendorHex == "1022") return "AMD";
    if (vendorHex == "8086") return "Intel";
    return nullptr;
}

struct CardInfo {
    std::string card;                  // "card0"
    int bootVga = 0;                   // /device/boot_vga (0/1), если нет — 0

    std::string vendorSys;             // /device/vendor (обычно 0x10de)
    std::string deviceSys;             // /device/device (обычно 0x2484)

    std::string pciVendor;             // PCI_ID из uevent (если есть)
    std::string pciDevice;

    std::string driver;                // symlink device/driver basename или uevent DRIVER
    unsigned long long vramBytes = 0;  // mem_info_vram_total (если есть)
};

static CardInfo read_card_info(const std::string& card) {
    CardInfo info;
    info.card = card;

    linutil::read_int_file("/sys/class/drm/" + card + "/device/boot_vga", info.bootVga);

    linutil::read_first_line("/sys/class/drm/" + card + "/device/vendor", info.vendorSys);
    linutil::read_first_line("/sys/class/drm/" + card + "/device/device", info.deviceSys);
    info.vendorSys = linutil::normalize_hex_id(std::move(info.vendorSys));
    info.deviceSys = linutil::normalize_hex_id(std::move(info.deviceSys));

    linutil::read_u64_file("/sys/class/drm/" + card + "/device/mem_info_vram_total", info.vramBytes);

    // 1) Предпочтительно: имя драйвера из symlink /device/driver
    linutil::read_symlink_basename("/sys/class/drm/" + card + "/device/driver", info.driver);

    // 2) Fallback: uevent (DRIVER=..., PCI_ID=...)
    std::string uevent;
    if (linutil::read_text_file("/sys/class/drm/" + card + "/device/uevent", uevent)) {
        if (info.driver.empty()) {
            if (auto drv = linutil::find_env_value(uevent, "DRIVER")) info.driver = *drv;
        }
        if (auto pci = linutil::find_env_value(uevent, "PCI_ID")) {
            linutil::parse_pci_id(*pci, info.pciVendor, info.pciDevice);
        }
    }

    return info;
}

static std::string best_vendor_hex(const CardInfo& info) {
    return !info.pciVendor.empty() ? info.pciVendor : info.vendorSys;
}

static std::string best_device_hex(const CardInfo& info) {
    return !info.pciDevice.empty() ? info.pciDevice : info.deviceSys;
}

/*
 * Эвристика для iGPU/dGPU без внешних утилит:
 * - Intel почти всегда iGPU
 * - Наличие отдельного VRAM в sysfs (mem_info_vram_total) — хороший признак dGPU
 */
static std::string gpu_kind(const CardInfo& info, std::string_view vendorHex) {
    if (vendorHex == "8086") return "iGPU";
    if (info.vramBytes > 0) return "dGPU";
    return "GPU";
}

/*
 * Выбираем “лучшую” карту, если их несколько:
 *  1) boot_vga=1 важнее всего,
 *  2) наличие VRAM важнее отсутствия,
 *  3) больший VRAM лучше,
 *  4) tie-break: меньший cardN (для стабильности).
 */
static CardInfo pick_best_card_info() {
    const auto cards = list_drm_cards();
    if (cards.empty()) return {};

    CardInfo best = read_card_info(cards.front());

    auto better = [](const CardInfo& a, const CardInfo& b) {
        if (a.bootVga != b.bootVga) return a.bootVga > b.bootVga;

        const int aHasVram = (a.vramBytes > 0) ? 1 : 0;
        const int bHasVram = (b.vramBytes > 0) ? 1 : 0;
        if (aHasVram != bHasVram) return aHasVram > bHasVram;

        if (a.vramBytes != b.vramBytes) return a.vramBytes > b.vramBytes;

        return a.card < b.card;
    };

    for (size_t i = 1; i < cards.size(); ++i) {
        CardInfo cur = read_card_info(cards[i]);
        if (better(cur, best)) best = std::move(cur);
    }

    return best;
}

} // namespace

std::string GPUSectionProvider::get_gpu_name() const {
    // На Linux без pci.ids мы не пытаемся “угадывать” маркетинговое имя.
    // Вместо этого печатаем полезную диагностическую строку:
    // Vendor + iGPU/dGPU + (ven:dev) + (driver ...) + (boot_vga).
    const CardInfo info = pick_best_card_info();
    if (info.card.empty()) return "Unknown";

    const std::string ven = best_vendor_hex(info);
    const std::string dev = best_device_hex(info);

    std::ostringstream out;

    if (const char* vname = vendor_name(ven)) out << vname << " ";
    out << gpu_kind(info, ven);

    if (!ven.empty() && !dev.empty()) out << " (" << ven << ":" << dev << ")";
    if (!info.driver.empty()) out << " (driver " << info.driver << ")";
    if (info.bootVga == 1) out << " (boot_vga)";

    const std::string s = out.str();
    return s.empty() ? "Unknown" : s;
}

std::string GPUSectionProvider::get_gpu_driver_version() const {
    const CardInfo info = pick_best_card_info();
    if (info.card.empty()) return "Unknown";
    if (info.driver.empty()) return "Unknown";

    // NVIDIA: часто есть информативная строка.
    if (info.driver == "nvidia") {
        std::ifstream nvf("/proc/driver/nvidia/version");
        if (nvf) {
            std::string line;
            if (std::getline(nvf, line)) {
                line = linutil::trim_copy(line);
                if (!line.empty()) return line;
            }
        }
    }

    // Универсальный вариант: если модуль публикует /sys/module/<driver>/version.
    std::string v;
    if (linutil::read_first_line("/sys/module/" + info.driver + "/version", v) && !v.empty()) {
        return v;
    }

    return "Unknown";
}

std::string GPUSectionProvider::get_gpu_memory() const {
    const CardInfo info = pick_best_card_info();
    if (info.card.empty()) return "Unknown";

    if (info.vramBytes > 0) return linutil::format_mb_from_bytes(info.vramBytes);
    return "Unknown";
}
