#pragma once
#include <cstdint>
#include <string>

namespace util {

// Текущее время, unix-секунды.
int64_t now();

// Деньги хранятся в копейках. "1234.50"
std::string moneyPlain(int64_t kop);
// "1 234.50 ₽"
std::string money(int64_t kop);
// Разбор суммы в рублях: "500", "500.5", "500,50" -> копейки.
bool parseRub(const std::string& s, int64_t& kop);

// "12.11.2026 14:05" (локальное время)
std::string fmtTime(int64_t ts);

std::string uuid4();
std::string htmlEscape(const std::string& s);
std::string trim(const std::string& s);
std::string toLower(std::string s);
bool parseInt(const std::string& s, int64_t& out);

// Обрезает UTF-8 строку до maxChars символов, не разрывая многобайтные символы.
std::string utf8Truncate(const std::string& s, size_t maxChars);

// Контакт для чека: email (приводится к нижнему регистру) или телефон РФ -> "7XXXXXXXXXX".
// true — если распознан; out — нормализованное значение.
bool normalizeReceiptContact(const std::string& in, std::string& out);

void log(const std::string& msg);

}  // namespace util
