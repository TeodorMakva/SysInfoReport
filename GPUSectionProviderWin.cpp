#ifndef NOMINMAX
#define NOMINMAX  // отключаем конфликтующие с std::min/max макросы из windows.h
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN  // убираем редко используемые API из windows.h
#endif

#include "GPUSectionProvider.h"
#include "wide_to_utf8.h"

#include <dxgi.h>        // DXGI для перечисления GPU
#include <Wbemidl.h>     // WMI COM-интерфейсы
#include <comdef.h>      // _bstr_t для WMI

#include <string>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <vector>

#pragma comment(lib, "dxgi.lib")     // DirectX Graphics Infrastructure
#pragma comment(lib, "wbemuuid.lib") // WMI GUID'ы

namespace {  // все helper'ы локальные

struct DxgiGpuInfo {
    std::wstring name;                       // "NVIDIA GeForce RTX 3080"
    SIZE_T dedicatedVideoMemoryBytes = 0;    // выделенная VRAM в байтах
    UINT vendorId = 0;                       // 0x10DE = NVIDIA
    UINT deviceId = 0;                       // 0x2484 = RTX 3080
    bool ok = false;                         // найдена ли GPU
};

// Конвертация wstring → utf8 с fallback'ом
static std::string wstring_to_utf8(const std::wstring& ws) {
    if (ws.empty()) return "Unknown";
    auto s = wide_to_utf8(std::wstring_view{ws});
    return s ? *s : "Unknown";
}

// Перечисляет GPU через DXGI, возвращает "лучшую" дискретную (максимум VRAM)
static DxgiGpuInfo query_best_gpu_dxgi() {
    DxgiGpuInfo best;

    IDXGIFactory1* factory = nullptr;
    // Создаём фабрику для EnumAdapters1 [DXGI 1.1+]
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr) || !factory) return best;

    // Перебираем все адаптеры (iGPU, dGPU, software)
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;  // конец списка
        if (FAILED(hr) || !adapter) continue;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            // Пропускаем Microsoft Basic Display Adapter (software)
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                // Берём с максимальным DedicatedVideoMemory (iGPU обычно меньше)
                if (!best.ok || desc.DedicatedVideoMemory > best.dedicatedVideoMemoryBytes) {
                    best.ok = true;
                    best.name = desc.Description;
                    best.dedicatedVideoMemoryBytes = desc.DedicatedVideoMemory;  // байты
                    best.vendorId = desc.VendorId;
                    best.deviceId = desc.DeviceId;
                }
            }
        }

        adapter->Release();  // обязательно для COM
    }

    factory->Release();  // обязательно для COM
    return best;
}

// Кэш DXGI: выполняется только один раз за программу
static const DxgiGpuInfo& dxgi_cache() {
    static std::once_flag once;
    static DxgiGpuInfo cached;
    std::call_once(once, [] { cached = query_best_gpu_dxgi(); });
    return cached;
}

// Парсит WMI PNPDeviceID: "PCI\\VEN_10DE&DEV_1B80&..." → VendorId=0x10DE, DeviceId=0x1B80
static bool parse_pnp_ven_dev(const std::wstring& pnp, UINT& ven, UINT& dev) {
    const std::wstring venTag = L"VEN_";
    const std::wstring devTag = L"DEV_";

    size_t pVen = pnp.find(venTag);
    size_t pDev = pnp.find(devTag);
    if (pVen == std::wstring::npos || pDev == std::wstring::npos) return false;

    pVen += venTag.size();
    pDev += devTag.size();

    if (pVen + 4 > pnp.size() || pDev + 4 > pnp.size()) return false;

    try {
        ven = static_cast<UINT>(std::stoul(pnp.substr(pVen, 4), nullptr, 16));
        dev = static_cast<UINT>(std::stoul(pnp.substr(pDev, 4), nullptr, 16));
        return true;
    } catch (...) {
        return false;
    }
}

// WMI-запрос: находит DriverVersion для конкретного GPU по VendorId/DeviceId
static std::string query_driver_version_wmi(UINT wantVen, UINT wantDev) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hr);

    // Настройка безопасности COM/WMI
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                         RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

    IWbemLocator* loc = nullptr;
    // WMI Locator для создания WMI-сессии
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (void**)&loc);
    if (FAILED(hr) || !loc) {
        if (needUninit) CoUninitialize();
        return "Unknown";
    }

    IWbemServices* svc = nullptr;
    // Подключаемся к пространству имён WMI
    hr = loc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"),
                            nullptr, nullptr, 0, 0, 0, 0, &svc);
    if (FAILED(hr) || !svc) {
        loc->Release();
        if (needUninit) CoUninitialize();
        return "Unknown";
    }

    // Права доступа для WMI-запроса
    CoSetProxyBlanket(svc,
                      RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);

    IEnumWbemClassObject* en = nullptr;
    // Запрос к Win32_VideoController: PNPDeviceID и DriverVersion всех видеокарт
    hr = svc->ExecQuery(_bstr_t(L"WQL"),
                        _bstr_t(L"SELECT PNPDeviceID, DriverVersion FROM Win32_VideoController"),
                        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        nullptr, &en);
    if (FAILED(hr) || !en) {
        svc->Release(); loc->Release();
        if (needUninit) CoUninitialize();
        return "Unknown";
    }

    std::string result = "Unknown";

    // Перебираем все видеокарты из WMI
    while (true) {
        IWbemClassObject* obj = nullptr;
        ULONG ret = 0;
        hr = en->Next(WBEM_INFINITE, 1, &obj, &ret);
        if (FAILED(hr) || ret == 0) break;

        VARIANT vPnp; VariantInit(&vPnp);
        VARIANT vDrv; VariantInit(&vDrv);

        std::wstring pnp;
        std::wstring drv;

        // Извлекаем PNPDeviceID
        if (SUCCEEDED(obj->Get(L"PNPDeviceID", 0, &vPnp, nullptr, nullptr)) &&
            vPnp.vt == VT_BSTR && vPnp.bstrVal) {
            pnp.assign(vPnp.bstrVal, vPnp.bstrVal + SysStringLen(vPnp.bstrVal));
        }
        // Извлекаем DriverVersion
        if (SUCCEEDED(obj->Get(L"DriverVersion", 0, &vDrv, nullptr, nullptr)) &&
            vDrv.vt == VT_BSTR && vDrv.bstrVal) {
            drv.assign(vDrv.bstrVal, vDrv.bstrVal + SysStringLen(vDrv.bstrVal));
        }

        UINT ven = 0, dev = 0;
        // Ищем совпадение по VendorId/DeviceId из DXGI
        if (!pnp.empty() && !drv.empty() &&
            parse_pnp_ven_dev(pnp, ven, dev) && ven == wantVen && dev == wantDev) {
            result = wstring_to_utf8(drv);
            VariantClear(&vPnp);
            VariantClear(&vDrv);
            obj->Release();
            break;  // нашли нужную GPU
        }

        VariantClear(&vPnp);
        VariantClear(&vDrv);
        obj->Release();
    }

    en->Release();
    svc->Release();
    loc->Release();
    if (needUninit) CoUninitialize();

    return result;
}

// Кэш версии драйвера: выполняется один раз
static const std::string& driver_cache() {
    static std::once_flag once;
    static std::string cached = "Unknown";
    std::call_once(once, [] {
        const auto& g = dxgi_cache();
        if (g.ok) cached = query_driver_version_wmi(g.vendorId, g.deviceId);
    });
    return cached;
}

}  // namespace

// Публичные методы: используют кэш
std::string GPUSectionProvider::get_gpu_name() const {
    const auto& g = dxgi_cache();
    return g.ok ? wstring_to_utf8(g.name) : "Unknown";
}

std::string GPUSectionProvider::get_gpu_memory() const {
    const auto& g = dxgi_cache();
    if (!g.ok || g.dedicatedVideoMemoryBytes == 0) return "Unknown";

    // DedicatedVideoMemory в байтах → MB с 1 знаком после запятой
    const double mb = (double)g.dedicatedVideoMemoryBytes / 1024.0 / 1024.0;
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << mb << " MB";
    return ss.str();
}

std::string GPUSectionProvider::get_gpu_driver_version() const {
    // DXGI не содержит версию драйвера → используем WMI
    return driver_cache();
}
