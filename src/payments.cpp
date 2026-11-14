#include "payments.hpp"

#include <cpr/cpr.h>

#include <stdexcept>

#include "util.hpp"

using json = nlohmann::json;

namespace {
const char* kApi = "https://api.yookassa.ru/v3/payments";

// payments.proxy из конфига: пусто — без прокси (cpr::Proxies{} ничего не меняет).
cpr::Proxies yooProxies(const Config& cfg) {
    if (cfg.paymentProxy.empty()) return cpr::Proxies{};
    return cpr::Proxies{{"https", cfg.paymentProxy}};
}
}

json YooKassaProvider::buildCreateBody(const Config& cfg, const PaymentRequest& req) {
    json amount = {{"value", util::moneyPlain(req.amount)}, {"currency", "RUB"}};
    json body = {
        {"amount", amount},
        {"confirmation", {{"type", "redirect"}, {"return_url", cfg.returnUrl}}},
        {"capture", true},
        {"description", util::utf8Truncate(req.description, 128)},
        {"metadata", {{"local_payment_id", std::to_string(req.localId)}, {"tg_user_id", std::to_string(req.userId)}}},
    };
    if (!req.method.empty()) body["payment_method_data"] = {{"type", req.method}};

    if (cfg.receiptEnabled) {
        if (req.customerContact.empty()) throw std::runtime_error("для чека нужен email или телефон покупателя");
        json customer;
        if (req.customerContact.find('@') != std::string::npos) customer["email"] = req.customerContact;
        else customer["phone"] = req.customerContact;
        json receipt = {
            {"customer", customer},
            {"items",
             {{{"description", util::utf8Truncate(cfg.receiptItem, 128)},
               {"quantity", "1.00"},
               {"amount", amount},
               {"vat_code", cfg.receiptVatCode},
               {"payment_subject", cfg.receiptPaymentSubject},
               {"payment_mode", cfg.receiptPaymentMode}}}},
        };
        if (cfg.receiptTaxSystem > 0) receipt["tax_system_code"] = cfg.receiptTaxSystem;
        body["receipt"] = receipt;
    }
    return body;
}

PaymentInit YooKassaProvider::create(const PaymentRequest& req) {
    json body = buildCreateBody(cfg_, req);
    // Idempotence-Key — случайный UUID на каждый счёт. Нельзя строить его только из номера платежа в БД:
    // ЮKassa помнит ключи 24 ч на весь магазин, и вторая копия бота (другой компьютер, новая БД) с тем же
    // магазином получала бы на свои #1, #2… ошибку idempotence_key_duplicated или чужой старый платёж.
    auto r = cpr::Post(cpr::Url{kApi}, cpr::Authentication{cfg_.yooShopId, cfg_.yooSecretKey, cpr::AuthMode::BASIC},
                       cpr::Header{{"Content-Type", "application/json"}, {"Idempotence-Key", util::uuid4()}},
                       cpr::Body{body.dump()}, cpr::Timeout{20000}, yooProxies(cfg_));
    if (r.error) throw std::runtime_error("ЮKassa: " + r.error.message);
    json j = json::parse(r.text, nullptr, false);
    if (j.is_discarded() || r.status_code != 200) {
        std::string msg = r.text.substr(0, 300);
        std::string description;
        if (!j.is_discarded() && j.is_object()) {  // {"type":"error","code":"invalid_request","description":"...","parameter":"receipt"}
            description = j.value("description", "");
            msg = j.value("code", "") + ": " + description +
                  (j.contains("parameter") ? " (" + j.value("parameter", "") + ")" : "");
        }
        std::string full = "ЮKassa HTTP " + std::to_string(r.status_code) + ": " + msg;
        // Способ оплаты не подключён в магазине (например, СБП в тестовом магазине)
        if (r.status_code == 400 && description.find("Payment method is not available") != std::string::npos)
            throw PaymentMethodUnavailable(full);
        throw std::runtime_error(full);
    }

    PaymentInit init;
    init.providerId = j.value("id", "");
    if (j.contains("confirmation")) init.url = j["confirmation"].value("confirmation_url", "");
    if (init.providerId.empty() || init.url.empty())
        throw std::runtime_error("ЮKassa: в ответе нет id/confirmation_url: " + r.text.substr(0, 300));
    return init;
}

PayStatus YooKassaProvider::check(const std::string& providerId) {
    auto r = cpr::Get(cpr::Url{std::string(kApi) + "/" + providerId},
                      cpr::Authentication{cfg_.yooShopId, cfg_.yooSecretKey, cpr::AuthMode::BASIC}, cpr::Timeout{15000},
                      yooProxies(cfg_));
    if (r.error || r.status_code != 200) {
        util::log("ЮKassa check " + providerId + ": " + r.error.message + " HTTP " + std::to_string(r.status_code));
        return PayStatus::Unknown;
    }
    json j = json::parse(r.text, nullptr, false);
    if (j.is_discarded()) return PayStatus::Unknown;
    std::string st = j.value("status", "");
    if (st == "succeeded") return PayStatus::Succeeded;
    if (st == "canceled") return PayStatus::Canceled;
    if (st == "pending" || st == "waiting_for_capture") return PayStatus::Pending;
    return PayStatus::Unknown;
}

std::unique_ptr<PaymentProvider> makePaymentProvider(const Config& cfg) {
    if (cfg.paymentProvider == "mock") return std::make_unique<MockProvider>();
    if (cfg.paymentProvider == "yookassa") return std::make_unique<YooKassaProvider>(cfg);
    throw std::runtime_error("Неизвестный payments.provider: " + cfg.paymentProvider);
}
