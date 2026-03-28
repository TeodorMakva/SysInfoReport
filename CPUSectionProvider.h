#pragma once

#include "ReportSection.h"

class CPUSectionProvider {
public:
    inline ReportSection GetCPUInfo() const {
        ReportSection result;
        result.title = "ПРОЦЕССОР:";

        result.lines.push_back({0, "Процессор", get_cpu_name()});
        result.lines.push_back({0, "Количество ядер процессора", get_cpu_core_count()});

        return result;
    }

private:
    std::string get_cpu_name() const;
    std::string get_cpu_core_count() const;
};
