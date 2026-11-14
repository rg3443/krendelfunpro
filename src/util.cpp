#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <random>
#include <regex>

namespace util {

int64_t now() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string moneyPlain(int64_t kop) {
    bool neg = kop < 0;
    if (neg) kop = -kop;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%lld.%02lld", neg ? "-" : "", static_cast<long long>(kop / 100),
                  static_cast<long long>(kop % 100));
    return buf;
}

std::string money(int64_t kop) {
    bool neg = kop < 0;
    if (neg) kop = -kop;
    std::string rub = std::to_string(kop / 100);
    std::string grouped;
    int cnt = 0;
    for (auto it = rub.rbegin(); it != rub.rend(); ++it) {
        if (cnt && cnt % 3 == 0) grouped.insert(grouped.begin(), ' ');
        grouped.insert(grouped.begin(), *it);
        ++cnt;
    }
    char frac[8];
    std::snprintf(frac, sizeof(frac), "%02lld", static_cast<long long>(kop % 100));
    return (neg ? "-" : "") + grouped + "." + frac + " ₽";
}

bool parseRub(const std::string& raw, int64_t& kop) {
    std::string s = trim(raw);
    std::replace(s.begin(), s.end(), ',', '.');
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    if (s.empty() || s.size() > 12) return false;
    auto dot = s.find('.');
    std::string ip = s.substr(0, dot);
    std::string fp = dot == std::string::npos ? "" : s.substr(dot + 1);
    if (ip.empty() || fp.size() > 2) return false;
    for (char c : ip + fp)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    while (fp.size() < 2) fp += '0';
    kop = std::stoll(ip) * 100 + std::stoll(fp);
    return true;
}

std::string fmtTime(int64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", &tm);
    return buf;
}

std::string uuid4() {
    static thread_local std::mt19937_64 rng{std::random_device{}() ^
                                            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08llx-%04llx-%04llx-%04llx-%012llx",
                  static_cast<unsigned long long>(a >> 32), static_cast<unsigned long long>((a >> 16) & 0xFFFF),
                  static_cast<unsigned long long>(a & 0xFFFF), static_cast<unsigned long long>(b >> 48),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return buf;
}

std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseInt(const std::string& raw, int64_t& out) {
    std::string s = trim(raw);
    if (s.empty()) return false;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

std::string utf8Truncate(const std::string& s, size_t maxChars) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        if (chars == maxChars) return s.substr(0, i);
        unsigned char c = static_cast<unsigned char>(s[i]);
        i += c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        ++chars;
    }
    return s;
}

bool normalizeReceiptContact(const std::string& raw, std::string& out) {
    std::string s = trim(raw);
    if (s.find('@') != std::string::npos) {
        s = toLower(s);
        static const std::regex emailRe(R"(^[a-z0-9._%+\-]+@[a-z0-9.\-]+\.[a-z]{2,}$)");
        if (s.size() > 254 || !std::regex_match(s, emailRe)) return false;
        out = s;
        return true;
    }
    std::string digits;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
        else if (c != '+' && c != ' ' && c != '-' && c != '(' && c != ')') return false;
    }
    if (digits.size() == 11 && (digits[0] == '8' || digits[0] == '7')) digits[0] = '7';
    else if (digits.size() == 10 && digits[0] == '9') digits = "7" + digits;
    else return false;
    out = digits;
    return true;
}

void log(const std::string& msg) {
    static std::mutex m;
    std::lock_guard lk(m);
    std::cerr << "[" << fmtTime(now()) << "] " << msg << std::endl;
}

}  // namespace util
