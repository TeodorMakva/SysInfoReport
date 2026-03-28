#pragma once

#include <string>
#include <vector>
#include <optional>

struct ReportLine {
    int indent = 0;                 // 0 = без \t, 1 = \t, 2 = \t\t ...
    std::string key;                // "Тип", "Список дисков", "Название"
    std::optional<std::string> value; // если пусто => печатаем как "key:" (заголовок подпункта)
};

struct ReportSection {
    std::string title;              // "ЖЁСТКИЙ ДИСК:" и т.п.
    std::vector<ReportLine> lines;  // набор строк разной вложенности
};

