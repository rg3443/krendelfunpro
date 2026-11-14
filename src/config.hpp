#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Тариф (тип бота): умный (ИИ) / простой и т.п.
struct Tier {
    std::string id;           // передаётся исполнителю как "tier", латиница: smart, dumb
    std::string name;         // для клиента: "🧠 Умный бот"
    std::string description;  // одна строка пояснения
    std::string details;      // подробное описание (HTML Telegram); пусто — встроенное для smart/dumb
    int64_t pricePerBotHour = 0;  // копейки
};

// Встроенное подробное описание тарифа по id (smart / dumb), иначе "".
std::string defaultTierDetails(const std::string& id);

struct Config {
    std::string telegramToken;
    std::vector<int64_t> adminIds;
    std::string dbPath = "rentbot.db";
    std::string supportContact;  // например "@support"

    // Цены и лимиты заказа
    std::vector<Tier> tiers;
    bool refundOnCancel = false;  // возвращать ли деньги, когда заказчик сам отменяет заказ
    int minBots = 1, maxBots = 500;
    int minHours = 1, maxHours = 72;
    std::vector<int> botPresets{5, 10, 25, 50, 100};
    std::vector<int> hourPresets{1, 2, 3, 6, 12, 24};
    int remindBeforeMin = 15;  // за сколько минут до конца предложить продление (0 — не напоминать)

    // Платежи
    std::string paymentProvider = "yookassa";  // yookassa | mock
    std::string yooShopId, yooSecretKey;
    // Прокси только для запросов к ЮKassa (пусто — напрямую), например "socks5h://127.0.0.1:1080".
    // Нужен, когда IP сервера ЮKassa не пускает.
    std::string paymentProxy;
    // Способы оплаты на выбор клиенту: "sbp", "bank_card". Несколько — бот спрашивает перед оплатой;
    // один — без вопроса; пусто — способ выбирается на странице ЮKassa.
    std::vector<std::string> paymentMethods{"sbp", "bank_card"};
    // Чеки 54-ФЗ через ЮKassa: в каждый платёж добавляется объект receipt
    bool receiptEnabled = false;
    int receiptVatCode = 1;  // 1 — без НДС; см. коды ставок НДС в документации ЮKassa
    std::string receiptItem = "Пополнение баланса сервиса аренды чат-ботов";
    std::string receiptPaymentSubject = "payment";  // предмет расчёта
    std::string receiptPaymentMode = "advance";     // способ расчёта: аванс (баланс, услуга ещё не оказана)
    int receiptTaxSystem = 0;  // tax_system_code, 0 — не передавать
    std::string returnUrl = "https://t.me";
    int64_t minTopup = 10000;     // копейки
    int64_t maxTopup = 10000000;  // копейки
    std::vector<int> topupPresets{100, 300, 500, 1000, 3000};  // рубли
    int paymentPollSec = 15;
    int paymentExpireMin = 1440;  // сколько минут опрашивать неоплаченный счёт

    // Исполнитель заказов
    std::string executorType = "manual";  // manual | http
    std::string executorUrl, executorToken;
    int autoStartTimeoutMin = 10;  // сколько ждать успешного автозапуска до отказа с возвратом
    int executorPollSec = 30;      // как часто спрашивать у исполнителя состояние запущенных заказов
    int64_t executorIdOffset = 100000;  // номер заказа у исполнителя = смещение + номер в боте

    const Tier* findTier(const std::string& id) const {
        for (auto& t : tiers)
            if (t.id == id) return &t;
        return nullptr;
    }

    static Config load(const std::string& path);
};
