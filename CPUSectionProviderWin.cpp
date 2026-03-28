#include "CPUSectionProvider.h"
#include "wide_to_utf8.h"

// Читает имя процессора из реестра: HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0
std::string CPUSectionProvider::get_cpu_name() const {
    // Путь к первому процессору (индекс 0). Последующие (1, 2...) — это те же ядра.
    const wchar_t* subKey = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    const wchar_t* value  = L"ProcessorNameString";

    DWORD type = 0;     // uint32_t: тип значения (REG_SZ)
    DWORD bytes = 0;    // uint32_t: размер значения в байтах

    // Шаг 1: узнаём размер значения (nullptr = не заполнять буфер)
    LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, subKey, value,
                              RRF_RT_REG_SZ, &type, nullptr, &bytes);
    if (st != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return "Unknown";

    // Шаг 2: выделяем буфер и читаем значение
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t));  // количество символов
    st = RegGetValueW(HKEY_LOCAL_MACHINE, subKey, value,
                      RRF_RT_REG_SZ, &type, buf.data(), &bytes);
    if (st != ERROR_SUCCESS)
        return "Unknown";

    // wcsnlen: длина без '\0', на случай если буфер содержит мусор после строки
    size_t wlen = wcsnlen(buf.data(), buf.size());
    auto s = wide_to_utf8(std::wstring_view{buf.data(), wlen});
    return s ? *s : "Unknown";
}

// Считает физические ядра процессора (не логические потоки/threads).
// Источник: GetLogicalProcessorInformationEx(RelationProcessorCore)
std::string CPUSectionProvider::get_cpu_core_count() const {
    DWORD len = 0;  // uint32_t: размер буфера в байтах
    // Первый вызов: узнаём размер, ожидаем ERROR_INSUFFICIENT_BUFFER
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0)
        return "Unknown";

    // Второй вызов: заполняем буфер
    std::vector<unsigned char> buffer(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                                          &len)) {
        return "Unknown";
    }

    int cores = 0;
    size_t offset = 0;
    // Парсим массив структур переменной длины
    while (offset < len) {
        auto p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
        // Каждая запись RelationProcessorCore = одно физическое ядро
        if (p->Relationship == RelationProcessorCore)
            ++cores;
        offset += p->Size;  // переход к следующей записи
    }

    return std::to_string(cores);
}