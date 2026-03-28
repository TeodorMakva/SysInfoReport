#pragma once

#include "ReportSection.h"

class GPUSectionProvider {
public:
    inline ReportSection GetGPUInfo() const {
        ReportSection result;
        result.title = "ВИДЕОКАРТА:";

        result.lines.push_back({0, "Название видеокарты", get_gpu_name()});
        result.lines.push_back({0, "Версия видеодрайвера", get_gpu_driver_version()});
        result.lines.push_back({0, "Память видеокарты", get_gpu_memory()});

        return result;
    }

private:
    std::string get_gpu_name() const;
    std::string get_gpu_driver_version() const;
    std::string get_gpu_memory() const;
};