// должен возвращать ReportSection
#pragma once

#include "ReportSection.h"

class OsSectionProvider {
public:
    inline ReportSection GetOsInfo() const {
        ReportSection result;
        result.title = "ОПЕРАЦИОННАЯ СИСТЕМА:";

        result.lines.push_back({0, "Имя системы", get_system_name()});
        result.lines.push_back({0, "Версия системы", get_system_version()});
        result.lines.push_back({0, "Пользователь",  get_user_name() + "-PC"});
        result.lines.push_back({0, "Имя компьютера",  get_computer_name()});

        return result;
    }

private:
    std::string get_system_name() const;
    std::string get_system_version() const;
    std::string get_user_name() const;
    std::string get_computer_name() const;
};



