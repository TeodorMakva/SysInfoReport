#include "MemorySectionProvider.h"
#include "wide_to_utf8.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <WbemIdl.h>

#include <string>
#include <vector>
#include <optional>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "wbemuuid.lib")

namespace {

// Преобразование SMBIOS-кода типа памяти в строку для UI.
std::string memory_type_from_code(long code) {
    switch (code) {
        case 20: return "DDR";
        case 21: return "DDR2";
        case 24: return "DDR3";
        case 26: return "DDR4";
        case 34: return "DDR5";
        default: return "Unknown";
    }
}

// GetPhysicallyInstalledSystemMemory возвращает объём в KB.
// Переводим KB -> GB и форматируем с 1 знаком после запятой.
std::string format_gb_from_kb(ULONGLONG kb) {
    double gb = static_cast<double>(kb) / (1024.0 * 1024.0);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << gb << " GB";
    return oss.str();
}

// Минимальный RAII-умный указатель под COM-интерфейсы:
// хранит сырой T*, а в деструкторе вызывает Release().
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ~ComPtr() {
        if (p_) p_->Release();
    }

    // Доступ к сырому указателю (для чтения).
    T* get() const { return p_; }

    // "Выходной параметр" для функций COM вида Foo(..., T** out).
    // Важно: предполагается, что p_ сейчас nullptr.
    T** out() { return &p_; }

    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

// RAII-обёртка над BSTR: выделили в конструкторе, освободили в деструкторе.
class ScopedBstr {
public:
    explicit ScopedBstr(const wchar_t* s) : bstr_(SysAllocString(s)) {}
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    ~ScopedBstr() {
        if (bstr_) SysFreeString(bstr_);
    }

    // Неявно приводим к BSTR, чтобы передавать в COM-функции.
    operator BSTR() const { return bstr_; }
    BSTR get() const { return bstr_; }

private:
    BSTR bstr_ = nullptr;
};

// RAII-обёртка над VARIANT: VariantInit + VariantClear.
class ScopedVariant {
public:
    ScopedVariant() { VariantInit(&v_); }
    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;

    ~ScopedVariant() { VariantClear(&v_); }

    VARIANT* out() { return &v_; }
    const VARIANT& get() const { return v_; }

private:
    VARIANT v_{};
};

// Читаем строковое свойство WMI (ожидаем VT_BSTR) и переводим в UTF-8.
std::optional<std::string> wmi_get_string(IWbemClassObject* obj, const wchar_t* prop) {
    if (!obj) return std::nullopt;

    ScopedVariant vt;
    if (FAILED(obj->Get(prop, 0, vt.out(), nullptr, nullptr))) return std::nullopt;

    const auto& v = vt.get();
    if (v.vt != VT_BSTR || !v.bstrVal) return std::nullopt;

    auto s = wide_to_utf8(std::wstring_view(v.bstrVal, SysStringLen(v.bstrVal)));
    if (!s) return std::nullopt;

    return *s;
}

// Читаем целочисленное свойство WMI и возвращаем long.
// Замечание: для UI-типа здесь всё равно берётся lVal, т.е. поведение упрощено.
std::optional<long> wmi_get_long(IWbemClassObject* obj, const wchar_t* prop) {
    if (!obj) return std::nullopt;

    ScopedVariant vt;
    if (FAILED(obj->Get(prop, 0, vt.out(), nullptr, nullptr))) return std::nullopt;

    const auto& v = vt.get();
    if (v.vt == VT_I4 || v.vt == VT_UI4 || v.vt == VT_I2 || v.vt == VT_UI2) {
        return v.lVal;
    }

    return std::nullopt;
}

// Собираем “название модуля” из Manufacturer+PartNumber.
// Если их нет — используем Name, иначе "Неизвестно".
std::string build_module_name(const std::optional<std::string>& name,
                              const std::optional<std::string>& manufacturer,
                              const std::optional<std::string>& part_number) {
    const bool has_man = manufacturer && !manufacturer->empty();
    const bool has_part = part_number && !part_number->empty();

    if (has_man || has_part) {
        std::string result;
        if (has_man) result += *manufacturer;
        if (!result.empty() && has_part) result += " ";
        if (has_part) result += *part_number;
        return result;
    }

    if (name && !name->empty()) return *name;

    return "Unknown";
}

}  // namespace

// Общий установленный объём RAM: WinAPI отдаёт KB -> форматируем в GB.
std::string MemorySectionProvider::get_total_memory_gb() const {
    ULONGLONG mem_kb = 0;
    if (!GetPhysicallyInstalledSystemMemory(&mem_kb) || mem_kb == 0) {
        return "Не удалось определить";
    }
    return format_gb_from_kb(mem_kb);
}

// Список модулей RAM: через WMI класс Win32_PhysicalMemory.
std::vector<MemorySectionProvider::MemoryModule> MemorySectionProvider::get_memory_modules() const {
    std::vector<MemoryModule> modules;

    // Инициализация COM для текущего потока.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(hr);

    // Используем do/while(false) как единый блок с ранними break при ошибках.
    do {
        // Настройка безопасности COM (может вернуть RPC_E_TOO_LATE — это допустимо).
        hr = CoInitializeSecurity(
                nullptr, -1, nullptr, nullptr,
                RPC_C_AUTHN_LEVEL_DEFAULT,
                RPC_C_IMP_LEVEL_IDENTIFY,
                nullptr, EOAC_NONE, nullptr
        );
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
            break;
        }

        // Создаём WMI-локатор (точка входа в WMI).
        ComPtr<IWbemLocator> locator;
        hr = CoCreateInstance(
                CLSID_WbemLocator,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_IWbemLocator,
                reinterpret_cast<LPVOID*>(locator.out())
        );
        if (FAILED(hr) || !locator) {
            break;
        }

        // Подключаемся к пространству имён ROOT\CIMV2 и получаем IWbemServices.
        ComPtr<IWbemServices> services;
        {
            ScopedBstr ns(L"ROOT\\CIMV2");
            hr = locator->ConnectServer(
                    ns,
                    nullptr,
                    nullptr,
                    nullptr,
                    0,
                    nullptr,
                    nullptr,
                    services.out()
            );
        }
        if (FAILED(hr) || !services) {
            break;
        }

        // Разрешаем WMI делать вызовы от имени текущего пользователя (impersonation).
        hr = CoSetProxyBlanket(
                services.get(),
                RPC_C_AUTHN_WINNT,
                RPC_C_AUTHZ_NONE,
                nullptr,
                RPC_C_AUTHN_LEVEL_CALL,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                nullptr,
                EOAC_NONE
        );
        if (FAILED(hr)) {
            break;
        }

        // Выполняем WQL-запрос к Win32_PhysicalMemory и получаем перечислитель результатов.
        ComPtr<IEnumWbemClassObject> enumerator;
        {
            ScopedBstr wql(L"WQL");
            ScopedBstr query(
                    L"SELECT Name, Manufacturer, PartNumber, "
                    L"SMBIOSMemoryType, MemoryType, Speed "
                    L"FROM Win32_PhysicalMemory"
            );

            hr = services->ExecQuery(
                    wql,
                    query,
                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                    nullptr,
                    enumerator.out()
            );
        }
        if (FAILED(hr) || !enumerator) {
            break;
        }

        // Читаем результаты по одному объекту.
        while (true) {
            ComPtr<IWbemClassObject> obj;
            ULONG returned = 0;

            hr = enumerator->Next(WBEM_INFINITE, 1, obj.out(), &returned);
            if (FAILED(hr) || returned == 0 || !obj) {
                break;
            }

            MemoryModule module;

            // Имя модуля: предпочтительно Manufacturer+PartNumber, иначе Name.
            const auto name = wmi_get_string(obj.get(), L"Name");
            const auto manufacturer = wmi_get_string(obj.get(), L"Manufacturer");
            const auto part_number = wmi_get_string(obj.get(), L"PartNumber");
            module.name = build_module_name(name, manufacturer, part_number);

            // Тип памяти: сначала SMBIOSMemoryType, если 0 — пробуем MemoryType.
            long type_code = wmi_get_long(obj.get(), L"SMBIOSMemoryType").value_or(0);
            if (type_code == 0) {
                type_code = wmi_get_long(obj.get(), L"MemoryType").value_or(0);
            }
            module.type = memory_type_from_code(type_code);

            // Скорость: если свойство есть и > 0 — показываем, иначе "Неизвестно".
            const auto speed = wmi_get_long(obj.get(), L"Speed");
            if (speed && *speed > 0) {
                module.speed = std::to_string(*speed);
            } else {
                module.speed = "Unknown";
            }

            modules.push_back(std::move(module));
        }

    } while (false);

    // Освобождаем COM только если инициализировали его в этом методе.
    if (com_initialized) {
        CoUninitialize();
    }

    return modules;
}
