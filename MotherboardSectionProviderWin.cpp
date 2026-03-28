#include "MotherboardSectionProvider.h"
#include "wide_to_utf8.h"

#define _WIN32_DCOM   // включаем полную поддержку DCOM для WMI
#define NOMINMAX      // отключаем конфликтующие min/max макросы
#include <Windows.h>
#include <Wbemidl.h>     // WMI COM-интерфейсы
#include <comdef.h>      // _bstr_t для строк WMI

#include <string>
#include <mutex>

#pragma comment(lib, "wbemuuid.lib")  // WMI GUID'ы

namespace {  // локальные helper'ы

// WMI-запрос к Win32_BaseBoard.Product (модель материнской платы)
static std::string query_motherboard_series_wmi() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Запоминаем, нужно ли вызывать CoUninitialize в конце
    const bool needUninit = SUCCEEDED(hr);

    // Настройка безопасности COM/WMI
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                         RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

    IWbemLocator* loc = nullptr;
    // WMI Locator: фабрика для создания WMI-сессий
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (void**)&loc);
    if (FAILED(hr) || !loc) {
        if (needUninit) CoUninitialize();
        return "Unknown";
    }

    IWbemServices* svc = nullptr;
    // Подключаемся к системному пространству WMI
    hr = loc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"),
                            nullptr, nullptr, 0, 0, 0, 0, &svc);
    if (FAILED(hr) || !svc) {
        loc->Release();
        if (needUninit) CoUninitialize();
        return "Unknown";
    }

    // Устанавливаем права доступа для WMI-запроса
    CoSetProxyBlanket(svc,
                      RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);

    IEnumWbemClassObject* en = nullptr;
    // Запрос к классу материнских плат: поле Product (модель/серия)
    hr = svc->ExecQuery(_bstr_t(L"WQL"),
                        _bstr_t(L"SELECT Product FROM Win32_BaseBoard"),
                        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        nullptr, &en);
    if (FAILED(hr) || !en) {
        svc->Release(); loc->Release();
        if (needUninit) CoUninitialize();
        return "Unknown";
    }

    std::string result = "Unknown";
    IWbemClassObject* obj = nullptr;
    ULONG ret = 0;
    // Берём первую (и обычно единственную) запись — материнка одна
    hr = en->Next(WBEM_INFINITE, 1, &obj, &ret);

    if (SUCCEEDED(hr) && ret == 1 && obj) {
        VARIANT v;
        VariantInit(&v);
        // Извлекаем поле Product (BSTR = Unicode)
        if (SUCCEEDED(obj->Get(L"Product", 0, &v, nullptr, nullptr)) &&
            v.vt == VT_BSTR && v.bstrVal) {

            // wide_to_utf8(std::wstring_view)
            std::wstring_view ws(reinterpret_cast<const wchar_t*>(v.bstrVal),
                                 SysStringLen(v.bstrVal));
            auto opt = wide_to_utf8(ws);
            if (opt) {
                result = std::move(*opt);  // например "ASUS PRIME Z390-A"
            }
        }
        VariantClear(&v);
        obj->Release();
    }

    en->Release();
    svc->Release();
    loc->Release();
    if (needUninit) CoUninitialize();

    return result.empty() ? "Unknown" : result;
}

// Кэш: WMI вызывается только один раз за программу
static const std::string& motherboard_cache() {
    static std::once_flag once;
    static std::string cached;
    std::call_once(once, [] { cached = query_motherboard_series_wmi(); });
    return cached;
}

}  // namespace

// Публичный метод: использует кэш
std::string MotherboardSectionProvider::get_motherboard_series() const {
    return motherboard_cache();
}
