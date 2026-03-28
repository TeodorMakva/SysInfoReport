#include "ReportFormatter.h"

void ReportFormatter::appendTabs(std::string& out, int count) {
    for (int i = 0; i < count; ++i) out.push_back('\t');
}

std::string ReportFormatter::Format(const std::vector<ReportSection>& sections) const
{
    std::string out;
    out.reserve(4096);

    for (const auto& sec : sections) {
        // Секция: без табуляции
        out += sec.title;
        out += "\n";

        // Внутри секции: базово 1 таб + line.indent
        for (const auto& line : sec.lines) {
            appendTabs(out, 1 + line.indent);
            out += line.key;
            out += ":";

            if (line.value) {
                out += " ";
                out += *line.value;
            }
            out += "\n";
        }

        out += "\n";
    }

    return out;
}
