#include "DiskSectionProvider.h"

#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>  // IOCTL_STORAGE_* для дисков

#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>

// Форматирует байты в человекочитаемый вид: "1.82 TB", "102.34 GB" и т.п.
// Всегда 2 знака после запятой.
static std::string format_bytes(unsigned long long bytes) {
    const double KB = 1024.0;
    const double MB = KB * 1024.0;
    const double GB = MB * 1024.0;
    const double TB = GB * 1024.0;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);

    if (bytes >= static_cast<unsigned long long>(TB)) {
        ss << (bytes / TB) << " TB";
    } else if (bytes >= static_cast<unsigned long long>(GB)) {
        ss << (bytes / GB) << " GB";
    } else if (bytes >= static_cast<unsigned long long>(MB)) {
        ss << (bytes / MB) << " MB";
    } else {
        ss << (bytes / KB) << " KB";
    }
    return ss.str();
}

// Определяет букву диска Windows (обычно C:) через путь к системной папке.
static std::string get_drive_from_path() {
    wchar_t buf[MAX_PATH] = {};
    // GetWindowsDirectoryW возвращает путь "C:\Windows"
    UINT len = GetWindowsDirectoryW(buf, MAX_PATH);
    if (len == 0) return {};

    // Из "C:\Windows\..." берём первые 2 символа "C:"
    if (len >= 2 && buf[1] == L':') {
        char drive[3];
        drive[0] = static_cast<char>(buf[0]);  // 'C'
        drive[1] = ':';
        drive[2] = '\0';
        return std::string(drive);
    }
    return {};
}

// Возвращает общий размер диска по корневому пути ("C:\").
static unsigned long long get_total_size_for_root(const std::wstring& root) {
    ULARGE_INTEGER freeBytesAvailableToCaller{};   // не используется
    ULARGE_INTEGER totalNumberOfBytes{};           // ← нужен нам
    ULARGE_INTEGER totalNumberOfFreeBytes{};       // не используется

    if (GetDiskFreeSpaceExW(root.c_str(),           // "C:\\"
                            &freeBytesAvailableToCaller,
                            &totalNumberOfBytes,       // общий размер диска
                            &totalNumberOfFreeBytes)) {
        return totalNumberOfBytes.QuadPart;  // размер в байтах
    }
    return 0ULL;
}

// Определяет тип диска (SSD или HDD) по seek penalty.
// SSD обычно не имеют задержки на перемещение головки.
static bool is_ssd_for_drive_letter(wchar_t letter) {
    // "\\.\C:" — открываем том напрямую (не раздел!)
    wchar_t volumePath[8] = L"\\\\.\\X:";
    volumePath[4] = letter;

    HANDLE h = CreateFileW(
            volumePath,
            0,                              // доступ только для чтения свойств
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return false;  // не открыли → считаем HDD
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType  = PropertyStandardQuery;

    DEVICE_SEEK_PENALTY_DESCRIPTOR desc{};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
            h,
            IOCTL_STORAGE_QUERY_PROPERTY,    // запрос свойств диска
            &query, sizeof(query),
            &desc, sizeof(desc),
            &bytesReturned,
            nullptr
    );

    CloseHandle(h);

    if (!ok) {
        return false;  // не получили свойства → считаем HDD
    }

    // SSD не имеют seek penalty (IncurSeekPenalty=FALSE)
    return desc.IncursSeekPenalty == FALSE;
}

// Получает модель диска ("WDC WD20EZRX-00D8PB0") по букве тома ('C').
// Использует IOCTL для доступа к физическому диску.
static std::string get_disk_model_for_drive_letter(wchar_t letter) {
    // Шаг 1: открываем том "\\.\C:"
    wchar_t volumePath[8] = L"\\\\.\\X:";
    volumePath[4] = letter;

    HANDLE hVolume = CreateFileW(
            volumePath,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
    );
    if (hVolume == INVALID_HANDLE_VALUE) {
        return "Unknown";
    }

    // Шаг 2: узнаём номер физического диска (PhysicalDrive0, 1...)
    STORAGE_DEVICE_NUMBER devNum{};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
            hVolume,
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr, 0,
            &devNum, sizeof(devNum),
            &bytesReturned,
            nullptr
    );

    CloseHandle(hVolume);

    if (!ok || devNum.DeviceType != FILE_DEVICE_DISK) {
        return "Unknown";
    }

    // Шаг 3: открываем физический диск "\\.\PhysicalDriveN"
    wchar_t physPath[32];
    swprintf(physPath, 32, L"\\\\.\\PhysicalDrive%lu", devNum.DeviceNumber);

    HANDLE hDisk = CreateFileW(
            physPath,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
    );
    if (hDisk == INVALID_HANDLE_VALUE) {
        return "Unknown";
    }

    // Шаг 4: запрашиваем дескриптор устройства
    BYTE buffer[1024] = {};
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;

    ok = DeviceIoControl(
            hDisk,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query, sizeof(query),
            buffer, sizeof(buffer),
            &bytesReturned,
            nullptr
    );

    CloseHandle(hDisk);

    if (!ok || bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return "Unknown";
    }

    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
    if (desc->ProductIdOffset == 0 || desc->ProductIdOffset >= bytesReturned) {
        return "Unknown";
    }

    // ProductId — ASCIIZ строка в смещении ProductIdOffset
    const char* productId = reinterpret_cast<const char*>(buffer + desc->ProductIdOffset);
    std::string model(productId);

    // Обрезаем пробелы и нулевые байты в конце
    while (!model.empty() && (model.back() == ' ' || model.back() == '\0')) {
        model.pop_back();
    }
    return model.empty() ? "Unknown" : model;
}

// ================= Методы класса DiskSectionProvider =================

std::string DiskSectionProvider::get_current_disk_type() const {
    std::string drive = get_drive_from_path();  // "C:"
    if (drive.empty()) {
        return "у текущего диска не определен тип либо у диска не задана буква!";
    }

    wchar_t letter = static_cast<wchar_t>(drive[0]);
    bool ssd = is_ssd_for_drive_letter(letter);

    return ssd ? "SSD" : "HDD";
}

std::string DiskSectionProvider::get_current_disk_size() const {
    std::string drive = get_drive_from_path();  // "C:"
    if (drive.empty()) {
        return "0 GB";
    }

    std::wstring root;
    root.push_back(static_cast<wchar_t>(drive[0]));
    root.push_back(L':');
    root.push_back(L'\\');  // "C:\\"

    unsigned long long bytes = get_total_size_for_root(root);
    if (bytes == 0) return "0 GB";
    return format_bytes(bytes);
}

std::vector<DiskSectionProvider::DiskItem> DiskSectionProvider::get_all_disks() const {
    std::vector<DiskItem> out;

    DWORD mask = GetLogicalDrives();  // битовую маску доступных дисков A-Z
    if (mask == 0) return out;

    // Перебираем буквы A-Z (бит 0 = A:, бит 25 = Z:)
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1 << i))) continue;

        wchar_t letter = L'A' + i;
        std::wstring root;
        root.push_back(letter);
        root.push_back(L':');
        root.push_back(L'\\');  // "A:\\", "C:\\"...

        UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
            // Пропускаем CD/DVD, сетевые, RAM-диски
            continue;
        }

        unsigned long long bytes = get_total_size_for_root(root);
        std::string sizeStr = bytes ? format_bytes(bytes) : "Unknown";

        std::string physType;
        if (type == DRIVE_FIXED) {
            bool ssd = is_ssd_for_drive_letter(letter);
            physType = ssd ? "SSD" : "HDD";
        } else if (type == DRIVE_REMOVABLE) {
            physType = "Removable";
        } else {
            physType = "No disk type";
        }

        std::string model = get_disk_model_for_drive_letter(letter);
        char letter_ascii = static_cast<char>(letter);  // 'C'
        std::string fullName = "диск (";
        fullName += letter_ascii;
        fullName += ":) ";
        if (!model.empty()) {
            fullName += model;
        }

        out.push_back({fullName, physType, sizeStr});
    }

    // Сортируем по имени (теперь по модели+букве)
    std::sort(out.begin(), out.end(),
              [](const DiskItem& a, const DiskItem& b) {
                  return a.name < b.name;
              });

    return out;
}

