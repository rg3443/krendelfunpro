#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;

struct DbError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct User {
    int64_t id = 0;  // Telegram user id
    std::string username;
    int64_t balance = 0;  // копейки
    int64_t createdAt = 0;
    std::string receiptContact;  // email или телефон 7XXXXXXXXXX для чеков 54-ФЗ
};

struct Payment {
    int64_t id = 0;
    int64_t userId = 0;
    std::string provider, providerId;
    int64_t amount = 0;  // копейки
    std::string status;  // new | pending | succeeded | canceled | expired | error
    std::string url;
    int64_t createdAt = 0, updatedAt = 0;
};

struct Order {
    int64_t id = 0;
    int64_t userId = 0;
    std::string channel;
    std::string tier;        // id тарифа (smart/dumb)
    int64_t unitPrice = 0;   // цена за 1 бота в час на момент заказа, копейки
    int bots = 0, hours = 0;
    int64_t price = 0;   // копейки (с учётом продлений)
    std::string status;  // queued | running | completed | cancelled | failed
    int64_t createdAt = 0, startedAt = 0, endsAt = 0;
    std::string note;
    // Данные от исполнителя
    std::string extStatus;      // waiting | running | paused ... (пусто — неизвестно)
    int botsActive = 0;         // ботов сейчас в чате
    int64_t missingBotSec = 0;  // накоплено «недоданных» бот-секунд (за них — компенсация)
    bool reminded = false;      // напоминание о скором окончании уже отправлено
};

enum class ExtendResult { Ok, NoFunds, NotRunning };

struct Stats {
    int64_t users = 0, totalBalance = 0, paidTotal = 0;
    int64_t ordersQueued = 0, ordersRunning = 0, ordersCompleted = 0, revenue = 0;
};

// Потокобезопасная обёртка над SQLite. Все суммы — в копейках.
class Db {
public:
    explicit Db(const std::string& path);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // --- пользователи и баланс ---
    void ensureUser(int64_t id, const std::string& username);
    std::optional<User> getUser(int64_t id);
    void setReceiptContact(int64_t id, const std::string& contact);
    // Ручное изменение баланса (админ). false — если баланс ушёл бы в минус.
    bool adjustBalance(int64_t userId, int64_t delta, const std::string& kind, const std::string& ref);

    // --- платежи ---
    int64_t createPayment(int64_t userId, const std::string& provider, int64_t amount);
    void setPaymentPending(int64_t id, const std::string& providerId, const std::string& url);
    void setPaymentError(int64_t id);
    std::optional<Payment> getPayment(int64_t id);
    std::vector<Payment> pendingPayments();
    // Атомарно: pending|expired -> succeeded и зачисление на баланс. true — если переход выполнен именно этим вызовом.
    bool completePayment(int64_t id);
    // Атомарно: pending -> newStatus (canceled/expired). true — если переход выполнен.
    bool closePayment(int64_t id, const std::string& newStatus);

    // --- заказы ---
    // Списывает unitPrice*bots*hours с баланса и создаёт заказ в статусе queued. nullopt — недостаточно средств.
    std::optional<int64_t> createOrder(int64_t userId, const std::string& channel, const std::string& tier,
                                       int64_t unitPrice, int bots, int hours);
    std::optional<Order> getOrder(int64_t id);
    std::vector<Order> userOrders(int64_t userId, int limit);
    std::vector<Order> ordersByStatus(const std::string& status);
    // queued -> running
    bool startOrder(int64_t id, int64_t startedAt, int64_t endsAt, int botsActive, const std::string& extStatus);
    // Обновить данные исполнителя для running-заказа и добавить недоданные бот-секунды.
    void updateOrderProgress(int64_t id, const std::string& extStatus, int botsActive, int64_t addMissingBotSec);
    // running -> completed. Возвращает компенсацию за недоданных ботов (уже зачислена) или nullopt.
    std::optional<int64_t> finishOrder(int64_t id);
    // queued|running -> newStatus (cancelled|failed). При withRefund: полный возврат для queued; для running —
    // за оставшееся время плюс компенсация за недоданных ботов. Без withRefund деньги не возвращаются.
    // Возвращает сумму возврата (0 без возврата) или nullopt, если переход невозможен.
    std::optional<int64_t> cancelOrder(int64_t id, const std::string& newStatus, const std::string& note,
                                       bool withRefund = true);

    // Продление running-заказа (ещё не истёкшего): списывает price, увеличивает hours/price/ends_at,
    // сбрасывает флаг напоминания. Всё в одной транзакции.
    ExtendResult extendOrder(int64_t id, int64_t userId, int hours, int64_t price);
    // Откат продления, если исполнитель отказал: возвращает деньги и сдвигает срок обратно
    // (только если заказ ещё running — иначе возврат уже сделан при закрытии).
    void revertExtension(int64_t id, int hours, int64_t price);
    // Отметить, что напоминание отправлено. true — если отметил этот вызов.
    bool markReminded(int64_t id);

    Stats stats();

    // Сколько вернётся при отмене заказа в момент now: queued — вся цена; running — неиспользованное время
    // плюс компенсация за недоданных ботов; иначе 0. Ту же формулу использует cancelOrder.
    static int64_t refundFor(const Order& o, int64_t now);

private:
    void exec(const char* sql);
    void migrate();
    sqlite3* db_ = nullptr;
    std::mutex mu_;
};
