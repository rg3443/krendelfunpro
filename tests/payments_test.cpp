// Тело запроса к ЮKassa (способ оплаты, чек 54-ФЗ) и разбор контакта для чека. Сеть не нужна.
#include <iostream>

#include "../src/payments.hpp"
#include "../src/util.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond << "\n"; \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

int main() {
    // --- контакт для чека ---
    std::string c;
    CHECK(util::normalizeReceiptContact(" Name.Surname@Mail.RU ", c) && c == "name.surname@mail.ru");
    CHECK(util::normalizeReceiptContact("+7 (900) 123-45-67", c) && c == "79001234567");
    CHECK(util::normalizeReceiptContact("89001234567", c) && c == "79001234567");
    CHECK(util::normalizeReceiptContact("9001234567", c) && c == "79001234567");
    CHECK(util::normalizeReceiptContact("79001234567", c) && c == "79001234567");
    CHECK(!util::normalizeReceiptContact("12345", c));
    CHECK(!util::normalizeReceiptContact("name@", c));
    CHECK(!util::normalizeReceiptContact("привет", c));
    CHECK(!util::normalizeReceiptContact("+1 202 555 0100", c));

    CHECK(util::utf8Truncate("абвгд", 3) == "абв");
    CHECK(util::utf8Truncate("abc", 10) == "abc");

    // --- тело запроса ---
    Config cfg;
    cfg.returnUrl = "https://t.me/test_bot";
    PaymentRequest req;
    req.localId = 42;
    req.userId = 777;
    req.amount = 50050;  // 500.50 ₽
    req.description = "Пополнение баланса, платёж #42";
    req.method = "sbp";

    // СБП, без чека
    auto b = YooKassaProvider::buildCreateBody(cfg, req);
    CHECK(b["amount"]["value"] == "500.50" && b["amount"]["currency"] == "RUB");
    CHECK(b["payment_method_data"]["type"] == "sbp");
    CHECK(b["capture"] == true);
    CHECK(b["confirmation"]["return_url"] == "https://t.me/test_bot");
    CHECK(b["metadata"]["local_payment_id"] == "42");
    CHECK(!b.contains("receipt"));

    // Карта и «способ выбирает клиент на странице ЮKassa» — способ берётся из запроса, а не из конфига
    req.method = "bank_card";
    CHECK(YooKassaProvider::buildCreateBody(cfg, req)["payment_method_data"]["type"] == "bank_card");
    req.method = "";
    CHECK(!YooKassaProvider::buildCreateBody(cfg, req).contains("payment_method_data"));
    req.method = "sbp";

    // Чек: email
    cfg.receiptEnabled = true;
    cfg.receiptVatCode = 1;
    req.customerContact = "name@mail.ru";
    b = YooKassaProvider::buildCreateBody(cfg, req);
    CHECK(b.contains("receipt"));
    CHECK(b["receipt"]["customer"]["email"] == "name@mail.ru");
    CHECK(!b["receipt"]["customer"].contains("phone"));
    auto& item = b["receipt"]["items"][0];
    CHECK(b["receipt"]["items"].size() == 1);
    CHECK(item["amount"]["value"] == "500.50");  // сумма позиции = сумме платежа
    CHECK(item["quantity"] == "1.00");
    CHECK(item["vat_code"] == 1);
    CHECK(item["payment_subject"] == "payment" && item["payment_mode"] == "advance");
    CHECK(!b["receipt"].contains("tax_system_code"));

    // Чек: телефон + система налогообложения
    cfg.receiptTaxSystem = 2;
    req.customerContact = "79001234567";
    b = YooKassaProvider::buildCreateBody(cfg, req);
    CHECK(b["receipt"]["customer"]["phone"] == "79001234567");
    CHECK(b["receipt"]["tax_system_code"] == 2);

    // Чек включён, а контакта нет — ошибка, платёж не создаётся
    req.customerContact.clear();
    bool threw = false;
    try {
        YooKassaProvider::buildCreateBody(cfg, req);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    std::cout << (failures ? "TESTS FAILED" : "ALL TESTS PASSED") << "\n";
    return failures ? 1 : 0;
}
