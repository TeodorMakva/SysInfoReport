#pragma once
#include "ReportSection.h"
#include <string>
#include <vector>

class MemorySectionProvider {
public:
    inline ReportSection GetMemoryInfo() const {
        ReportSection s;
        s.title = "ОПЕРАТИВНАЯ ПАМЯТЬ:";

        // Общее количество установленной ОЗУ
        s.lines.push_back({0, "Установленное", get_total_memory_gb()});

        // Модули памяти
        auto modules = get_memory_modules();
        int index = 1;
        for (const auto& m : modules) {
            std::string key = "Оперативная память " + std::to_string(index);
            s.lines.push_back({0, key, std::nullopt});

            s.lines.push_back({1, "Название", m.name});
            s.lines.push_back({1, "Тип памяти", m.type});
            s.lines.push_back({1, "Скорость", m.speed});
            ++index;
        }

        return s;
    }

private:
    struct MemoryModule {
        std::string name;
        std::string type;
        std::string speed;
    };

    std::string get_total_memory_gb() const;
    std::vector<MemoryModule> get_memory_modules() const;
};
