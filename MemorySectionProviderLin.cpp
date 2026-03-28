// MemorySectionProviderLin.cpp
#include "MemorySectionProvider.h"

#include <sys/sysinfo.h>

#include <cstdio>      // popen, pclose, fgets
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ----------------- utils -----------------

static std::string trim(std::string s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

static std::string normalize_space(std::string s) {
    s = trim(std::move(s));
    std::string out;
    out.reserve(s.size());

    bool prevSpace = false;
    for (unsigned char c : s) {
        const bool isSpace = (c == ' ' || c == '\t');
        if (isSpace) {
            if (!prevSpace) out.push_back(' ');
        } else {
            out.push_back(static_cast<char>(c));
        }
        prevSpace = isSpace;
    }
    return trim(std::move(out));
}

static bool split_kv_colon(const std::string& line, std::string& key, std::string& val) {
    const auto pos = line.find(':');
    if (pos == std::string::npos) return false;
    key = trim(line.substr(0, pos));
    val = trim(line.substr(pos + 1));
    return true;
}

static std::string to_upper_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

static bool is_unknown_value(std::string_view v) {
    const std::string s = trim(std::string(v));
    return s.empty() || s == "Unknown" || s == "Not Specified" || s == "None";
}

// "3200 MT/s" -> "3200", "2666 MHz" -> "2666"
static std::string extract_first_uint(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] < '0' || s[i] > '9')) ++i;
    if (i == s.size()) return {};

    size_t j = i;
    while (j < s.size() && (s[j] >= '0' && s[j] <= '9')) ++j;
    return std::string(s.substr(i, j - i));
}

// ----------------- total memory -----------------

static std::string format_gb_from_bytes(unsigned long long bytes) {
    const double gb = static_cast<double>(bytes) / 1024.0 / 1024.0 / 1024.0;
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss << std::setprecision(1) << gb; // как в Win-ветке: число без суффикса
    return oss.str();
}

static bool read_total_ram_bytes_sysinfo(unsigned long long& outBytes) {
    struct sysinfo si {};
    if (sysinfo(&si) != 0) return false;

    outBytes = static_cast<unsigned long long>(si.totalram) *
               static_cast<unsigned long long>(si.mem_unit);

    return outBytes != 0;
}

static bool read_total_ram_kb_proc_meminfo(unsigned long long& outKb) {
    std::ifstream f("/proc/meminfo");
    if (!f) return false;

    std::string key;
    unsigned long long kb = 0;
    std::string unit;

    while (f >> key >> kb >> unit) {
        if (key == "MemTotal:" && kb > 0) {
            outKb = kb;
            return true;
        }
        f.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return false;
}

// ----------------- dmidecode -----------------

static bool run_dmidecode_type17(std::string& outText) {
    outText.clear();

    FILE* pipe = popen("dmidecode -t 17 2>/dev/null", "r");
    if (!pipe) return false;

    char buf[4096];
    size_t total = 0;
    const size_t kMax = 512 * 1024;

    while (fgets(buf, sizeof(buf), pipe)) {
        const size_t n = std::char_traits<char>::length(buf);
        if (total + n > kMax) break;
        outText.append(buf, n);
        total += n;
    }

    const int rc = pclose(pipe);
    return (rc == 0) && !outText.empty();
}

/*
 * строго DDR / DDR2 / DDR3 / DDR4 / DDR5, иначе Unknown.
 * LPDDR4/LPDDR5 тоже маппим к DDR4/DDR5 (чтобы было ближе к Windows-выводу).
 */
static std::string canonicalize_mem_type(std::string type, std::string typeDetail) {
    type = normalize_space(std::move(type));
    typeDetail = normalize_space(std::move(typeDetail));

    std::string combined = type;
    if (!typeDetail.empty()) {
        if (!combined.empty()) combined.push_back(' ');
        combined += typeDetail;
    }
    if (combined.empty()) return "Unknown";

    const std::string u = to_upper_ascii(combined);

    if (u.find("DDR5") != std::string::npos) return "DDR5";
    if (u.find("DDR4") != std::string::npos) return "DDR4";
    if (u.find("DDR3") != std::string::npos) return "DDR3";
    if (u.find("DDR2") != std::string::npos) return "DDR2";
    if (u.find("DDR")  != std::string::npos) return "DDR";

    return "Unknown";
}

struct ParsedModule {
    std::string name;
    std::string type;   // canonical (DDR4/DDR5/Unknown)
    std::string speed;  // digits/Unknown
};

static std::vector<ParsedModule> parse_dmidecode_type17(const std::string& text) {
    std::vector<ParsedModule> modules;

    struct Tmp {
        std::string manufacturer;
        std::string partNumber;
        std::string locator;
        std::string bankLocator;

        std::string size;       // "16 GB" / "No Module Installed"
        std::string type;
        std::string typeDetail;
        std::string speed;      // "3200 MT/s" / "Unknown" / "0 MT/s"
        std::string cfgSpeed;   // "Configured Memory Speed"
    } cur;

    auto flush = [&]() {
        if (cur.size == "No Module Installed" || cur.size == "0 MB" || cur.size == "0 GB") {
            cur = {};
            return;
        }

        ParsedModule m;

        const std::string man  = normalize_space(cur.manufacturer);
        const std::string pn   = normalize_space(cur.partNumber);
        const std::string loc  = normalize_space(cur.locator);
        const std::string bank = normalize_space(cur.bankLocator);

        if (!is_unknown_value(man) && !is_unknown_value(pn)) {
            m.name = man + " " + pn;
        } else if (!is_unknown_value(pn)) {
            m.name = pn;
        } else if (!is_unknown_value(man)) {
            m.name = man;
        } else if (!is_unknown_value(loc)) {
            m.name = loc;
        } else if (!is_unknown_value(bank)) {
            m.name = bank;
        } else {
            m.name = "Unknown";
        }

        m.type = canonicalize_mem_type(cur.type, cur.typeDetail);

        std::string sp = normalize_space(cur.speed);
        if (is_unknown_value(sp)) sp = normalize_space(cur.cfgSpeed);

        const std::string digits = extract_first_uint(sp);
        m.speed = (!digits.empty() && digits != "0") ? digits : "Unknown";

        modules.push_back(std::move(m));
        cur = {};
    };

    std::istringstream iss(text);
    std::string line;
    bool inDevice = false;

    while (std::getline(iss, line)) {
        line = trim(line);

        if (line == "Memory Device") {
            if (inDevice) flush();
            inDevice = true;
            continue;
        }

        if (!inDevice || line.empty()) continue;

        std::string key, val;
        if (!split_kv_colon(line, key, val)) continue;

        key = normalize_space(std::move(key));
        val = normalize_space(std::move(val));

        if (key == "Size") cur.size = val;
        else if (key == "Type") cur.type = val;
        else if (key == "Type Detail") cur.typeDetail = val;
        else if (key == "Speed") cur.speed = val;
        else if (key == "Configured Memory Speed") cur.cfgSpeed = val;
        else if (key == "Manufacturer") cur.manufacturer = val;
        else if (key == "Part Number") cur.partNumber = val;
        else if (key == "Locator") cur.locator = val;
        else if (key == "Bank Locator") cur.bankLocator = val;
    }

    if (inDevice) flush();
    return modules;
}

} // namespace

// ----------------- MemorySectionProvider -----------------

std::string MemorySectionProvider::get_total_memory_gb() const {
    unsigned long long bytes = 0;
    if (read_total_ram_bytes_sysinfo(bytes)) {
        return format_gb_from_bytes(bytes);
    }

    unsigned long long kb = 0;
    if (read_total_ram_kb_proc_meminfo(kb) && kb > 0) {
        return format_gb_from_bytes(kb * 1024ULL);
    }

    return "Unknown";
}

auto MemorySectionProvider::get_memory_modules() const -> std::vector<MemoryModule> {
    static std::vector<MemoryModule> cached;
    static bool inited = false;

    if (!inited) {
        cached.clear();

        std::string text;
        if (run_dmidecode_type17(text)) {
            const auto parsed = parse_dmidecode_type17(text);
            cached.reserve(parsed.size());

            for (const auto& p : parsed) {
                MemoryModule m;
                m.name  = p.name.empty()  ? "Unknown" : p.name;
                m.type  = p.type.empty()  ? "Unknown" : p.type;   // DDR4/DDR5/Unknown
                m.speed = p.speed.empty() ? "Unknown" : p.speed;  // digits/Unknown
                cached.push_back(std::move(m));
            }
        }

        // Заглушка “как Windows”: один модуль, все поля Unknown.
        if (cached.empty()) {
            MemoryModule m;
            m.name  = "Unknown";
            m.type  = "Unknown";
            m.speed = "Unknown";
            cached.push_back(std::move(m));
        }

        inited = true;
    }

    return cached; // копия
}
