#pragma once
#include "ReportSection.h"
#include <string>
#include <vector>

class NetworkSectionProvider {
public:
    inline ReportSection GetNetworkInfo() const {
        ReportSection s;
        s.title = "СЕТЕВЫЕ НАСТРОЙКИ:";

        auto adapters = get_adapters();
        int index = 1;

        for (const auto& adapter : adapters) {
            std::string key = "Сеть " + std::to_string(index);
            s.lines.push_back({0, key, std::nullopt});

            s.lines.push_back({1, "Название", adapter.name});
            if (!adapter.ip.empty()) {
                s.lines.push_back({1, "IP адрес", adapter.ip});
            }
            if (!adapter.netmask.empty()) {
                s.lines.push_back({1, "Маска сети", adapter.netmask});
            }
            ++index;
        }

        if (adapters.empty()) {
            s.lines.push_back({0, "Адаптеры не найдены", std::nullopt});
        }

        return s;
    }

private:
    struct NetworkAdapterInfo {
        std::string name;
        std::string ip;
        std::string netmask;
    };

    std::vector<NetworkAdapterInfo> get_adapters() const;
};
