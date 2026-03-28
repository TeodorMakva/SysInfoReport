#include "OsSectionProvider.h"
#include "CPUSectionProvider.h"
#include "GPUSectionProvider.h"
#include "DiskSectionProvider.h"
#include "MotherboardSectionProvider.h"
#include "MemorySectionProvider.h"
#include "NetworkSectionProvider.h"


#include "ReportFormatter.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static std::string parseOutputFile(int argc, char** argv)
{
    std::string out = "report.txt";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-f") {
            if (i + 1 < argc) {
                out = argv[i + 1];
                ++i;
            } else {
                std::cerr << "Error: -f requires a filename\n";
                return {}; // ошибка
            }
        }
    }
    return out;
}

int main(int argc, char** argv)
{
    const std::string outFile = parseOutputFile(argc, argv);
    if (outFile.empty()) return 1;

    // 1) Собираем секции
    OsSectionProvider os;
    CPUSectionProvider cpu;
    GPUSectionProvider gpu;
    DiskSectionProvider disk;
    MotherboardSectionProvider mother_board;
    MemorySectionProvider memory;
    NetworkSectionProvider network;

    std::vector<ReportSection> sections;

    sections.push_back(os.GetOsInfo());
    sections.push_back(cpu.GetCPUInfo());
    sections.push_back(gpu.GetGPUInfo());
    sections.push_back(disk.GetDiskInfo());
    sections.push_back(mother_board.GetMotherboardInfo());
    sections.push_back(memory.GetMemoryInfo());
    sections.push_back(network.GetNetworkInfo());

    // 2) Форматируем
    ReportFormatter formatter;
    const std::string text = formatter.Format(sections);

    // 3) Пишем в файл
    std::ofstream f(outFile, std::ios::out | std::ios::trunc);
    if (!f.is_open()) {
        std::cerr << "Error: cannot open output file: " << outFile << "\n";
        return 2;
    }
    f << text;

    return 0;
}
