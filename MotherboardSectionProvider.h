#pragma once
#include "ReportSection.h"

class MotherboardSectionProvider {
public:
    inline ReportSection GetMotherboardInfo() const {
        ReportSection s;
        s.title = "МАТЕРИНСКАЯ ПЛАТА:";
        s.lines.push_back({0, "Серия", get_motherboard_series()});
        return s;
    }

private:
    std::string get_motherboard_series() const;
};
