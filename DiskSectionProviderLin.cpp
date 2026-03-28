// DiskSectionProviderLin.cpp
#include "DiskSectionProvider.h"
#include "LinUtils.h"

#include <sys/statvfs.h>
#include <dirent.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

static std::string format_bytes_2dp(unsigned long long bytes) {
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

static bool statvfs_size_bytes(const std::string& path, unsigned long long& bytes_out) {
    struct statvfs v {};
    if (statvfs(path.c_str(), &v) != 0) return false;
    bytes_out = (unsigned long long)v.f_blocks * (unsigned long long)v.f_frsize;
    return true;
}

static bool is_pseudo_fs(const std::string& fstype) {
    static const char* bad[] = {
        "proc","sysfs","tmpfs","devtmpfs","devpts",
        "cgroup","cgroup2",
        "overlay","squashfs","autofs","mqueue",
        "debugfs","tracefs","fusectl","securityfs",
        "pstore","efivarfs","ramfs","nsfs",
        "bpf","configfs"
    };

    for (auto* x : bad) {
        if (fstype == x) return true;
    }

    // Многие “технические” маунты приходят как fuse.something
    if (linutil::starts_with(fstype, "fuse.")) return true;

    return false;
}

static bool csv_opts_has_token(const std::string& opts, const std::string& token) {
    // opts из /proc/self/mounts: "rw,relatime,bind" и т.п.
    // Ищем token как отдельный элемент CSV.
    size_t i = 0;
    while (i < opts.size()) {
        size_t j = opts.find(',', i);
        if (j == std::string::npos) j = opts.size();
        const std::string_view part(opts.data() + i, j - i);
        if (part == token) return true;
        i = (j < opts.size()) ? (j + 1) : j;
    }
    return false;
}

static std::vector<std::string> list_dir_names(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = ::opendir(path.c_str());
    if (!d) return out;

    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(name);
    }

    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

static std::string strip_partition_suffix(std::string dev) {
    // Вход: "sda1", "nvme0n1p2", "mmcblk0p1", "vda2", "dm-0"
    // Выход: "sda", "nvme0n1", "mmcblk0", "vda", "dm-0"

    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };

    if (linutil::starts_with(dev, "nvme") || linutil::starts_with(dev, "mmcblk")) {
        // nvme0n1p2 -> nvme0n1, mmcblk0p1 -> mmcblk0
        const size_t p = dev.rfind('p');
        if (p != std::string::npos && p + 1 < dev.size() && is_digit(dev[p + 1])) {
            return dev.substr(0, p);
        }
        return dev;
    }

    // sda1 -> sda, vda2 -> vda
    size_t i = dev.size();
    while (i > 0 && is_digit(dev[i - 1])) --i;
    if (i == dev.size()) return dev;
    return dev.substr(0, i);
}

static std::string resolve_mapper_to_dm(const std::string& mapperName) {
    // mapperName: что после "/dev/mapper/"
    // Ищем dm-* у которого /sys/class/block/dm-*/dm/name == mapperName
    for (const auto& dev : list_dir_names("/sys/class/block")) {
        if (!linutil::starts_with(dev, "dm-")) continue;

        std::string name;
        if (!linutil::read_first_line("/sys/class/block/" + dev + "/dm/name", name)) continue;
        if (name == mapperName) return dev;
    }
    return {};
}

static std::string base_block_device_from_devpath(std::string devPath) {
    // devPath может быть: /dev/sda1, /dev/nvme0n1p2, /dev/mmcblk0p1, /dev/dm-0, /dev/mapper/xxx
    if (!linutil::starts_with(devPath, "/dev/")) return {};

    devPath = devPath.substr(5); // remove "/dev/"

    if (linutil::starts_with(devPath, "mapper/")) {
        const std::string mapperName = devPath.substr(std::string("mapper/").size());
        std::string dm = resolve_mapper_to_dm(mapperName);
        return dm; // может быть пусто
    }

    // Уже dm-0 / md0 / sda1 / nvme0n1p2 ...
    return strip_partition_suffix(devPath);
}

static std::string read_rotational_type(const std::string& base) {
    if (base.empty()) return "Unknown";

    std::string rot;
    if (!linutil::read_first_line("/sys/class/block/" + base + "/queue/rotational", rot)) return "Unknown";

    if (rot == "0") return "SSD";
    if (rot == "1") return "HDD";
    return "Unknown";
}

static std::string read_device_model(const std::string& base) {
    if (base.empty()) return {};

    std::string vendor, model;

    // SATA/SCSI обычно: vendor + model
    linutil::read_first_line("/sys/class/block/" + base + "/device/vendor", vendor);
    linutil::read_first_line("/sys/class/block/" + base + "/device/model", model);

    vendor = linutil::trim_copy(vendor);
    model  = linutil::trim_copy(model);

    std::string out;
    if (!vendor.empty() && linutil::looks_like_human_name(vendor)) out += vendor;
    if (!out.empty() && !model.empty()) out += " ";
    if (!model.empty()) out += model;

    out = linutil::trim_copy(out);
    if (!out.empty()) return out;

    // NVMe/virtio иногда имеют только model
    if (!model.empty()) return model;

    return {};
}

static bool read_removable_flag(const std::string& base, int& removable) {
    removable = 0;
    if (base.empty()) return false;
    return linutil::read_int_file("/sys/class/block/" + base + "/removable", removable);
}

static std::string resolve_to_physical_like(std::string base) {
    // Пытаемся для dm-/md* спуститься к нижележащему устройству по slaves/*
    // (делаем пару шагов и защищаемся от циклов).
    std::unordered_set<std::string> seen;

    for (int steps = 0; steps < 8; ++steps) {
        if (base.empty()) return {};
        if (!seen.insert(base).second) return base; // цикл

        const std::string slavesDir = "/sys/class/block/" + base + "/slaves";
        const auto slaves = list_dir_names(slavesDir);
        if (slaves.empty()) return base;

        // Берём первого "slave" — этого хватает, чтобы определить rotational/model.
        std::string child = strip_partition_suffix(slaves.front());
        if (child == base) return base;

        base = std::move(child);
    }

    return base;
}

static std::string root_device_from_mounts() {
    std::ifstream f("/proc/self/mounts");
    if (!f) return {};

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string dev, mnt, fstype, opts;
        if (!(iss >> dev >> mnt >> fstype >> opts)) continue;
        if (mnt == "/") return dev;
    }
    return {};
}

} // namespace

std::string DiskSectionProvider::get_current_disk_type() const {
    const std::string dev = root_device_from_mounts();
    const std::string base = base_block_device_from_devpath(dev);
    const std::string phys = resolve_to_physical_like(base);

    int removable = 0;
    if (read_removable_flag(phys, removable) && removable == 1) return "Removable";

    const std::string t = read_rotational_type(phys);
    return t.empty() ? "Unknown" : t;
}

std::string DiskSectionProvider::get_current_disk_size() const {
    // Размер текущего "тома" (как было у тебя) — ёмкость FS для "/"
    unsigned long long bytes = 0;
    if (!statvfs_size_bytes("/", bytes) || bytes == 0) return "0 GB";
    return format_bytes_2dp(bytes);
}

std::vector<DiskSectionProvider::DiskItem> DiskSectionProvider::get_all_disks() const {
    std::vector<DiskItem> out;

    std::ifstream f("/proc/self/mounts");
    if (!f) return out;

    std::unordered_set<std::string> seen; // dev|mnt чтобы не дублировать строки

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string dev, mnt, fstype, opts;
        if (!(iss >> dev >> mnt >> fstype >> opts)) continue;

        if (is_pseudo_fs(fstype)) continue;
        if (csv_opts_has_token(opts, "bind") || csv_opts_has_token(opts, "rbind")) continue;

        const std::string key = dev + "|" + mnt;
        if (!seen.insert(key).second) continue;

        unsigned long long bytes = 0;
        if (!statvfs_size_bytes(mnt, bytes) || bytes == 0) continue;

        std::string base = base_block_device_from_devpath(dev);
        std::string phys = resolve_to_physical_like(base);

        DiskItem item;

        // Имя: модель (если есть) + исходный dev и mountpoint
        std::string model = read_device_model(phys);
        if (!model.empty()) {
            item.name = model + " [" + dev + " (" + mnt + ")]";
        } else {
            item.name = dev + " (" + mnt + ")";
        }

        int removable = 0;
        if (read_removable_flag(phys, removable) && removable == 1) {
            item.type = "Removable";
        } else {
            item.type = read_rotational_type(phys);
        }

        item.size = format_bytes_2dp(bytes);
        out.push_back(std::move(item));
    }

    std::sort(out.begin(), out.end(), [](const DiskItem& a, const DiskItem& b) {
        return a.name < b.name;
    });

    return out;
}
