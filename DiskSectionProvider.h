#pragma once
#include "ReportSection.h"
#include <string>
#include <vector>

class DiskSectionProvider {
public:
    inline ReportSection GetDiskInfo() const {
        ReportSection s;
        s.title = "ЖЁСТКИЙ ДИСК:";

        // Текущий диск (где стоит Windows)
        s.lines.push_back({0, "Тип",   get_current_disk_type()});
        s.lines.push_back({0, "Объём", get_current_disk_size()});

        // Список дисков
        s.lines.push_back({0, "Список дисков", std::nullopt});

        auto disks = get_all_disks();
        for (const auto& d : disks) {
            s.lines.push_back({1, "Название", d.name});
            s.lines.push_back({2, "Тип",      d.type});
            s.lines.push_back({2, "Память",   d.size});
        }

        return s;
    }

private:
    struct DiskItem {
        std::string name;   // "диск (D:) ST2000DM008-2UB102"
        std::string type;   // "HDD"/"SSD"/"No disk type"/"Removable"
        std::string size;   // "1.82 TB"
    };

    // “Текущий диск” — диск, где находится Windows (по пути GetWindowsDirectory)
    std::string get_current_disk_type() const;
    std::string get_current_disk_size() const;

    std::vector<DiskItem> get_all_disks() const;
};
