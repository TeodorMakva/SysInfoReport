#include <winsock2.h>      // Важно: winsock2.h должен быть ДО windows.h
#include <ws2tcpip.h>      // inet_ntop, INET_ADDRSTRLEN и т.п.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <iphlpapi.h>      // GetAdaptersAddresses, структуры IP_ADAPTER_*

#include "NetworkSectionProvider.h"
#include "wide_to_utf8.h"

#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

// Description в IP_ADAPTER_ADDRESSES может быть ANSI (char*) или Wide (wchar_t*)
// в зависимости от SDK/заголовков (в т.ч. MinGW).
// Поэтому делаем две перегрузки и вызываем их одинаково.

static std::string description_to_utf8(const wchar_t* ws) {
    if (!ws || !*ws) return {};

    auto u8 = wide_to_utf8(std::wstring_view(ws, wcslen(ws)));
    return u8 ? *u8 : "Unknown";
}

// ANSI -> UTF-16 (CP_ACP) -> UTF-8, а если не удалось — возвращаем как есть.
static std::string description_to_utf8(const char* s) {
    if (!s || !*s) return {};

    int wlen = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
    if (wlen <= 0) {
        return std::string(s);
    }

    std::wstring w(static_cast<size_t>(wlen), L'\0');
    int written = MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), wlen);
    if (written <= 0) {
        return std::string(s);
    }

    // Убираем завершающий '\0', чтобы wide_to_utf8 не тащил его в результат
    if (!w.empty() && w.back() == L'\0') w.pop_back();

    auto u8 = wide_to_utf8(std::wstring_view(w));
    return u8 ? *u8 : std::string(s);
}

} // namespace

std::vector<NetworkSectionProvider::NetworkAdapterInfo>
NetworkSectionProvider::get_adapters() const {
    std::vector<NetworkAdapterInfo> adapters;

    // Инициализируем Winsock перед использованием inet_ntop и др.
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return adapters; // пусто при ошибке
    }

    // 1) Узнаём, сколько памяти нужно под результат GetAdaptersAddresses.
    ULONG bufferSize = 0;
    DWORD result = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_INCLUDE_PREFIX,
            nullptr,
            nullptr,
            &bufferSize
    );

    // Если не получили ERROR_BUFFER_OVERFLOW, значит размер не выдали как ожидаем.
    if (result != ERROR_BUFFER_OVERFLOW) {
        WSACleanup();
        return adapters;
    }

    // 2) Выделяем буфер нужного размера и вызываем GetAdaptersAddresses снова.
    std::vector<BYTE> buffer(bufferSize);
    PIP_ADAPTER_ADDRESSES pAddresses =
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    result = GetAdaptersAddresses(
            AF_INET,
            // INCLUDE_PREFIX нужен для OnLinkPrefixLength/префиксов
            // SKIP_* — чтобы не тащить лишние части структуры (anycast/multicast/DNS).
            GAA_FLAG_INCLUDE_PREFIX |
            GAA_FLAG_SKIP_ANYCAST |
            GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            pAddresses,
            &bufferSize
    );

    if (result == NO_ERROR) {
        // pAddresses — голова связного списка адаптеров.
        PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses;
        while (pCurrAddresses) {
            // Оставляем только "живые" интерфейсы и выкидываем loopback.
            if (pCurrAddresses->OperStatus == IfOperStatusUp &&
                !(pCurrAddresses->IfType == IF_TYPE_SOFTWARE_LOOPBACK)) {

                NetworkAdapterInfo info;

                // 1) Человеческое имя: FriendlyName предпочтительнее.
                if (pCurrAddresses->FriendlyName) {
                    auto friendly = wide_to_utf8(std::wstring_view(
                            pCurrAddresses->FriendlyName,
                            wcslen(pCurrAddresses->FriendlyName)
                    ));
                    if (friendly) info.name = *friendly;
                }

                // 2) Фолбэк: Description (ANSI или Wide — зависит от заголовков).
                if (info.name.empty() && pCurrAddresses->Description) {
                    info.name = description_to_utf8(pCurrAddresses->Description);
                }

                if (info.name.empty()) {
                    info.name = "Неизвестный адаптер";
                }

                // Ищем первый unicast IPv4 адрес у этого адаптера.
                PIP_ADAPTER_UNICAST_ADDRESS pUnicast =
                        pCurrAddresses->FirstUnicastAddress;

                while (pUnicast) {
                    if (pUnicast->Address.lpSockaddr &&
                        pUnicast->Address.lpSockaddr->sa_family == AF_INET) {

                        auto* ipv4 =
                                reinterpret_cast<sockaddr_in*>(pUnicast->Address.lpSockaddr);

                        // IP в строку
                        char ipStr[INET_ADDRSTRLEN]{};
                        inet_ntop(AF_INET, &ipv4->sin_addr, ipStr, sizeof(ipStr));
                        info.ip = ipStr;

                        // Маска из длины префикса (CIDR)
                        // Важно: prefix==0 надо обработать отдельно, иначе сдвиг на 32 бита.
                        if (pUnicast->OnLinkPrefixLength <= 32) {
                            uint32_t mask = 0;
                            if (pUnicast->OnLinkPrefixLength == 0) {
                                mask = 0;
                            } else {
                                mask = 0xFFFFFFFFu << (32 - pUnicast->OnLinkPrefixLength);
                            }

                            sockaddr_in maskAddr{};
                            maskAddr.sin_addr.s_addr = htonl(mask);

                            char maskStr[INET_ADDRSTRLEN]{};
                            inet_ntop(AF_INET, &maskAddr.sin_addr, maskStr, sizeof(maskStr));
                            info.netmask = maskStr;
                        }
                        break; // берём только первый IPv4
                    }
                    pUnicast = pUnicast->Next;
                }

                // Добавляем только если нашли IPv4 (иначе пропускаем адаптер).
                if (!info.ip.empty()) {
                    adapters.push_back(std::move(info));
                }
            }

            pCurrAddresses = pCurrAddresses->Next;
        }
    }

    // Освобождаем ресурсы Winsock (обязательно на каждый успешный WSAStartup)
    WSACleanup();
    return adapters;
}
