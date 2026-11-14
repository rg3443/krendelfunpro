#pragma once
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "config.hpp"

enum class PayStatus { Pending, Succeeded, Canceled, Unknown };

struct PaymentInit {
    std::string providerId;
    std::string url;  // ссылка на оплату
};

// Данные запроса на создание платежа.
struct PaymentRequest {
    int64_t localId = 0;      // номер платежа в БД бота
    int64_t userId = 0;       // Telegram id
    int64_t amount = 0;       // копейки
    std::string description;  // описание платежа (до 128 символов)
    std::string customerContact;  // email или телефон 7XXXXXXXXXX для чека (если чеки включены)
    std::string method;           // sbp | bank_card | "" — способ выбирается на странице ЮKassa
};

// Ошибка «этот способ оплаты недоступен в магазине» — можно предложить клиенту другой способ.
struct PaymentMethodUnavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class PaymentProvider {
public:
    virtual ~PaymentProvider() = default;
    virtual std::string name() const = 0;
    // Создаёт платёж у провайдера. Бросает исключение при ошибке.
    virtual PaymentInit create(const PaymentRequest& req) = 0;
    virtual PayStatus check(const std::string& providerId) = 0;
};

// ЮKassa. Способ оплаты — из payments.payment_method (по умолчанию СБП),
// при payments.receipt.enabled в платёж добавляется чек 54-ФЗ.
// Документация: https://yookassa.ru/developers/api#create_payment
class YooKassaProvider : public PaymentProvider {
public:
    explicit YooKassaProvider(const Config& cfg) : cfg_(cfg) {}
    std::string name() const override { return "yookassa"; }
    PaymentInit create(const PaymentRequest& req) override;
    PayStatus check(const std::string& providerId) override;

    // Тело POST /v3/payments (вынесено для тестов).
    static nlohmann::json buildCreateBody(const Config& cfg, const PaymentRequest& req);

private:
    const Config& cfg_;
};

// Тестовый провайдер: платёж «висит», пока админ не подтвердит его командой /paid <id>.
class MockProvider : public PaymentProvider {
public:
    std::string name() const override { return "mock"; }
    PaymentInit create(const PaymentRequest& req) override {
        return {"mock-" + std::to_string(req.localId), "https://example.com/mock-pay/" + std::to_string(req.localId)};
    }
    PayStatus check(const std::string&) override { return PayStatus::Pending; }
};

std::unique_ptr<PaymentProvider> makePaymentProvider(const Config& cfg);
