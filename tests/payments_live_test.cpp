// Живая проверка против ТЕСТОВОГО магазина ЮKassa: build\payments_live_test.exe config.json
// Создаёт платёж с каждым способом из payments.payment_methods и печатает результат.
// Деньги не списываются: платежи остаются неоплаченными и ЮKassa их сама отменяет.
// Отказывается работать с боевым ключом (live_...).
#include <iostream>
#include <random>

#include "../src/config.hpp"
#include "../src/payments.hpp"
#include "../src/util.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: payments_live_test <config.json>\n";
        return 2;
    }
    Config cfg = Config::load(argv[1]);
    if (cfg.paymentProvider != "yookassa" || cfg.yooSecretKey.rfind("test_", 0) != 0) {
        std::cerr << "Нужен provider=yookassa и тестовый ключ test_...\n";
        return 2;
    }
    YooKassaProvider yk(cfg);
    std::mt19937_64 rng{std::random_device{}()};
    for (const std::string& m : {std::string("sbp"), std::string("bank_card")}) {
        PaymentRequest req;
        req.localId = 900000000 + static_cast<int64_t>(rng() % 1000000);  // не пересекается с номерами бота
        req.userId = 0;
        req.amount = 10000;
        req.description = "rentbot: проверка способа " + m;
        req.method = m;
        req.customerContact = "test@example.com";
        try {
            auto init = yk.create(req);
            std::cout << m << ": OK, платёж " << init.providerId << " создан\n";
        } catch (const PaymentMethodUnavailable& e) {
            std::cout << m << ": PaymentMethodUnavailable (" << e.what() << ")\n";
        } catch (const std::exception& e) {
            std::cout << m << ": другая ошибка (" << e.what() << ")\n";
        }
    }
    return 0;
}
