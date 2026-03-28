// NetworkSectionProviderLin.cpp
#include "NetworkSectionProvider.h"

#include <ifaddrs.h>     // getifaddrs / freeifaddrs
#include <net/if.h>      // IFF_UP, IFF_LOOPBACK
#include <arpa/inet.h>   // inet_ntop, INET_ADDRSTRLEN, INET6_ADDRSTRLEN
#include <netinet/in.h>  // sockaddr_in, sockaddr_in6, IN6_IS_ADDR_LINKLOCAL

#include <sys/stat.h>    // stat (проверка существования sysfs-пути)

#include <map>           // чтобы вывод был стабильным по имени интерфейса
#include <string>
#include <vector>

namespace {

/*
 * Небольшой RAII-guard под getifaddrs:
 * гарантирует freeifaddrs() при любом выходе из функции.
 */
class IfAddrsGuard {
public:
    IfAddrsGuard() = default;
    IfAddrsGuard(const IfAddrsGuard&) = delete;
    IfAddrsGuard& operator=(const IfAddrsGuard&) = delete;

    ~IfAddrsGuard() {
        if (p_) freeifaddrs(p_);
    }

    ifaddrs** out() { return &p_; }
    ifaddrs* get() const { return p_; }

private:
    ifaddrs* p_ = nullptr;
};

/*
 * Проверка: существует ли директория (для sysfs-проверок типа "wireless").
 * Не используем <filesystem>, чтобы не упираться в нюансы линковки на старых toolchain.
 */
static bool dir_exists(const std::string& path) {
    struct stat st {};
    return (stat(path.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
}

/*
 * Формирование "дружественного" имени интерфейса без изменения структуры отчёта:
 * - "wlan0" -> "wlan0 (Wi-Fi)" если есть /sys/class/net/<if>/wireless
 * При желании можно добавить и другие эвристики (bridge, tun/tap и т.п.).
 */
static std::string pretty_iface_name(const std::string& ifname) {
    const std::string wifiDir = "/sys/class/net/" + ifname + "/wireless";
    if (dir_exists(wifiDir)) {
        return ifname + " (Wi-Fi)";
    }
    return ifname;
}

/*
 * Пытаемся преобразовать sockaddr* в строку IP.
 * Возвращает пустую строку при ошибке/неподдерживаемом семействе.
 */
static std::string sockaddr_to_ip_string(const sockaddr* sa) {
    if (!sa) return {};

    char buf[INET6_ADDRSTRLEN] = {};

    if (sa->sa_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(sa);
        if (inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf))) {
            return std::string(buf);
        }
        return {};
    }

    if (sa->sa_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(sa);
        if (inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf))) {
            return std::string(buf);
        }
        return {};
    }

    return {};
}

/*
 * Проверка: является ли IPv6 адрес link-local (fe80::/10).
 * Такие адреса обычно не хочется показывать как "основной IP".
 */
static bool is_ipv6_link_local(const sockaddr* sa) {
    if (!sa || sa->sa_family != AF_INET6) return false;
    const auto* a6 = reinterpret_cast<const sockaddr_in6*>(sa);
    return IN6_IS_ADDR_LINKLOCAL(&a6->sin6_addr);
}

} // namespace

std::vector<NetworkSectionProvider::NetworkAdapterInfo>
NetworkSectionProvider::get_adapters() const {
    std::vector<NetworkAdapterInfo> out;

    // 1) Получаем список интерфейсов и адресов одним вызовом.
    // getifaddrs отдаёт связный список, где один интерфейс может встречаться много раз
    // (разные адреса, разные семейства AF_*).
    IfAddrsGuard g;
    if (getifaddrs(g.out()) != 0 || !g.get()) {
        return out; // пусто при ошибке
    }

    // 2) Собираем по имени интерфейса, чтобы:
    // - не дублировать "Сеть N"
    // - выбрать "лучший" адрес (IPv4 приоритетнее, иначе IPv6 global).
    std::map<std::string, NetworkAdapterInfo> by_name;

    for (const ifaddrs* ifa = g.get(); ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;

        // Фильтры по состоянию интерфейса:
        // - IFF_UP: интерфейс поднят
        // - IFF_LOOPBACK: loopback не показываем (как и в Win-версии).
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        const char* rawName = ifa->ifa_name ? ifa->ifa_name : "";
        if (!*rawName) continue;
        const std::string ifname(rawName);

        // Нас интересуют только IPv4/IPv6.
        const int fam = ifa->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;

        auto& info = by_name[ifname];

        // Название (можно сделать более "человеческим", но не меняя интерфейс структуры).
        if (info.name.empty()) {
            info.name = pretty_iface_name(ifname);
        }

        // 3) Выбор IP:
        // - Если нашли IPv4 и он ещё не установлен — берём его.
        // - Если IPv4 нет, но есть IPv6 (и он НЕ link-local) — берём его как fallback.
        if (fam == AF_INET) {
            if (info.ip.empty()) {
                info.ip = sockaddr_to_ip_string(ifa->ifa_addr);
            }

            // Маска сети имеет смысл в привычном виде именно для IPv4.
            if (info.netmask.empty() && ifa->ifa_netmask) {
                info.netmask = sockaddr_to_ip_string(ifa->ifa_netmask);
            }
        } else { // AF_INET6
            // Не затираем IPv4 (он приоритетнее), IPv6 используем только если IPv4 нет.
            if (info.ip.empty() && !is_ipv6_link_local(ifa->ifa_addr)) {
                info.ip = sockaddr_to_ip_string(ifa->ifa_addr);
                // netmask для IPv6 в виде "ffff:..." обычно не нужен в таком отчёте,
                // а префикс /64 из ifaddrs тут напрямую не получить — поэтому пропускаем.
            }
        }
    }

    // 4) Собираем результат: добавляем только те адаптеры, где удалось получить IP.
    // Если IP нет — GetNetworkInfo() выведет "Адаптеры не найдены".
    for (auto& kv : by_name) {
        if (!kv.second.ip.empty()) {
            out.push_back(std::move(kv.second));
        }
    }

    return out;
}
