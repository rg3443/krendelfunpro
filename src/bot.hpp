#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "config.hpp"
#include "db.hpp"
#include "executor.hpp"
#include "payments.hpp"
#include "telegram.hpp"

class Bot {
public:
    Bot(const Config& cfg, Db& db, TgApi& tg, PaymentProvider& pay, OrderExecutor& exec);
    // Блокирует до stop == true. Внутри запускает фоновый поток проверки платежей и заказов.
    void run(std::atomic<bool>& stop);

private:
    // Диалоговое состояние пользователя (живёт только в потоке обработки апдейтов)
    enum class Step { None, TopupAmount, ReceiptContact, OrderChannel, OrderTier, OrderBots, OrderHours, OrderConfirm };
    struct Session {
        Step step = Step::None;
        int64_t pendingTopup = 0;      // сумма пополнения, ожидающая ввода контакта для чека
        std::string pendingMethod;     // и выбранный для неё способ оплаты
        std::string channel;
        std::string tier;
        int bots = 0, hours = 0;
    };

    // --- входящие апдейты ---
    void handleUpdate(const json& u);
    void onMessage(const json& m);
    void onCallback(const json& cq);
    void onCommand(int64_t uid, const std::string& cmd, const std::string& args);
    bool onAdminCommand(int64_t uid, const std::string& cmd, const std::string& args);
    void onText(int64_t uid, const std::string& text);

    // --- экраны ---
    void sendMenu(int64_t uid, const std::string& text);
    void showBalance(int64_t uid);
    void showHelp(int64_t uid);
    void showOrders(int64_t uid);

    // --- пополнение ---
    void askTopup(int64_t uid);
    // method пусто: при нескольких способах оплаты бот спросит, при одном — возьмёт его.
    void createTopup(int64_t uid, int64_t amount, const std::string& method = "");
    void askPayMethod(int64_t uid, int64_t amount, const std::string& note = "",
                      const std::string& exclude = "");
    void checkPaymentByUser(int64_t uid, int64_t paymentId, const std::string& cbId);
    void applyPaymentStatus(const Payment& p, PayStatus st);
    void askReceiptContact(int64_t uid, int64_t pendingTopup, const std::string& pendingMethod = "");
    void saveReceiptContact(int64_t uid, const std::string& raw);
    static std::string payMethodLabel(const std::string& method);  // "через СБП" / "картой" / ""
    std::string payMethodsText() const;  // все доступные способы: "по СБП или картой"

    // --- заказ ---
    void startOrderFlow(int64_t uid, const std::string& presetTier = "");
    void askTier(int64_t uid);
    void showTariffs(int64_t uid);                         // тарифы кратко + кнопки «Подробнее»
    void showTierDetails(int64_t uid, const std::string& tierId);
    void askBots(int64_t uid);
    void askHours(int64_t uid);
    void showOrderSummary(int64_t uid);
    void confirmOrder(int64_t uid);
    void showCancelableOrders(int64_t uid);
    void askCancelOrder(int64_t uid, int64_t orderId, const std::string& cbId);
    void cancelOrderByUser(int64_t uid, int64_t orderId, const std::string& cbId);

    // --- продление ---
    void askExtendHours(int64_t uid, int64_t orderId, const std::string& cbId);
    void showExtendSummary(int64_t uid, int64_t orderId, int hours, const std::string& cbId);
    void extendOrderByUser(int64_t uid, int64_t orderId, int hours, const std::string& cbId);

    // --- жизненный цикл заказа ---
    bool launchOrder(const Order& o, std::string& err);
    // Закрывает заказ с возвратом. false — заказ уже был закрыт (ничего не сделано, уведомлений нет).
    bool failOrder(const Order& o, const std::string& status, const std::string& reason);
    void notifyAdminsNewOrder(const Order& o);

    // --- фоновые задачи ---
    void workerLoop(std::atomic<bool>& stop);
    void pollPayments();
    void processOrders();
    void pollExecutor();

    // --- утилиты ---
    bool isAdmin(int64_t uid) const;
    void notifyAdmins(const std::string& html, const json& markup = nullptr);
    void safeSend(int64_t chatId, const std::string& html, const json& markup = nullptr);
    std::string orderCard(const Order& o) const;
    static std::string statusLabel(const std::string& status);
    std::string tierName(const std::string& tierId) const;
    std::string tariffsText() const;     // "🧠 Умный бот — 100.00 ₽ за бота в час\n..."
    std::string stepLabel(int step) const;  // "Шаг 2/4"
    json mainKeyboard() const;

    const Config& cfg_;
    Db& db_;
    TgApi& tg_;
    PaymentProvider& pay_;
    OrderExecutor& exec_;
    std::unordered_map<int64_t, Session> sessions_;
    // заказ -> время прошлого опроса исполнителя; трогают оба потока, поэтому под мьютексом
    std::unordered_map<int64_t, int64_t> lastExecPoll_;
    std::mutex execPollMu_;
    void forgetExecPoll(int64_t orderId);
};
