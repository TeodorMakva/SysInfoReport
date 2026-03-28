// “форматирование”: преобразует набор секций в текст с табуляциями/переносами по шаблону из примера.

#pragma once

#include "ReportSection.h"
#include <string>
#include <vector>

class ReportFormatter {
public:
    // Преобразует секции в итоговый текст отчёта
    std::string Format(const std::vector<ReportSection>& sections) const;

private:
    static void appendTabs(std::string& out, int count);
};