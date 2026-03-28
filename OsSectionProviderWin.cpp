#include <windows.h>
#include <lmcons.h>

#include "OsSectionProvider.h"
#include "wide_to_utf8.h"

std::string OsSectionProvider::get_system_name() const {
    return "Windows";
}

std::string OsSectionProvider::get_system_version() const {
    // Совместимый минимум структуры OSVERSIONINFOEXW / RTL_OSVERSIONINFOEXW.
    // https://learn.microsoft.com/ru-ru/windows-hardware/drivers/ddi/wdm/ns-wdm-_osversioninfoexw
    struct RTL_OSVERSIONINFOEXW_Compat {
        ULONG dwOSVersionInfoSize;
        ULONG dwMajorVersion;
        ULONG dwMinorVersion;
        ULONG dwBuildNumber;
        ULONG dwPlatformId;
        WCHAR szCSDVersion[128];
        USHORT wServicePackMajor;
        USHORT wServicePackMinor;
        USHORT wSuiteMask;
        UCHAR  wProductType;
        UCHAR  wReserved;
    };

    using RtlGetVersionFn = LONG (WINAPI*)(RTL_OSVERSIONINFOEXW_Compat*);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return "Unknown";

    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return "Unknown";

    RTL_OSVERSIONINFOEXW_Compat v{};
    // Обязательное поле: сообщает функции размер структуры.
    v.dwOSVersionInfoSize = sizeof(v);

    // 0 = STATUS_SUCCESS, любое другое значение считаем ошибкой.
    if (fn(&v) != 0) return "Unknown";

    // Строка вида "10.0.19045".
    return std::to_string(v.dwMajorVersion) + "." +
           std::to_string(v.dwMinorVersion) + "." +
           std::to_string(v.dwBuildNumber);
}

std::string OsSectionProvider::get_user_name() const {
    // Максимальная длина имени пользователя, включая завершающий '\0'.
    DWORD size = UNLEN + 1;
    std::wstring buf(size, L'\0');

    if (!GetUserNameW(buf.data(), &size) || size == 0)  // size incl '\0'
        return "Unknown";

    // size включает '\0', для std::wstring храним только сам текст.
    buf.resize(size - 1);

    auto s = wide_to_utf8(buf);
    return s ? *s : "Unknown";
}

std::string OsSectionProvider::get_computer_name() const {
    // Буфер для имени компьютера (hostname) в Unicode.
    std::wstring w(MAX_COMPUTERNAME_LENGTH + 1, L'\0');
    DWORD n = (DWORD)w.size();                 // размер буфера, включая '\0'

    // GetComputerNameW на выходе записывает в n длину БЕЗ завершающего '\0'.
    if (!GetComputerNameW(w.data(), &n))
        return "Unknown";

    w.resize(n);                            // Обрезаем до фактической длины имени.

    auto s = wide_to_utf8(w);
    return s ? *s : "Unknown";
}
