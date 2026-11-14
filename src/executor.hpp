#pragma once
#include <memory>
#include <string>

#include "config.hpp"
#include "db.hpp"

// Состояние заказа на стороне исполнителя.
struct ExecState {
    bool ok = false;        // запрос выполнен и ответ разобран
    std::string error;      // текст ошибки, если !ok
    int httpCode = 0;       // HTTP-код ответа (0 — сеть недоступна)
    std::string status;     // waiting | running | paused | completed | stopped | failed
    int assigned = 0;       // ботов сейчас в чате
    int requested = 0;      // заказано ботов
    int64_t endsAt = 0;     // время окончания по данным исполнителя
    std::string reason;     // причина завершения/ошибки
};

// Исполнитель заказов: то, что реально подключает ботов к Twitch-чату.
// Сам Telegram-бот только принимает оплату/заказы и следит за статусами,
// а запуск делегирует исполнителю.
class OrderExecutor {
public:
    virtual ~OrderExecutor() = default;
    // true — исполнитель запускает заказы сам; false — запускает администратор вручную.
    virtual bool automatic() const = 0;
    struct Capacity {
        int free = -1;   // свободно сейчас; -1 — неизвестно
        int total = -1;  // всего ботов в пуле; -1 — неизвестно
    };
    virtual Capacity capacity() { return {}; }
    // Сколько ботов свободно сейчас; -1 — неизвестно.
    int freeBots() { return capacity().free; }
    virtual ExecState start(const Order& o, int64_t endsAt) = 0;
    // Текущее состояние запущенного заказа. ok=false — исполнитель статусы не отдаёт или недоступен.
    virtual ExecState query(const Order&) { return {}; }
    // Продлить идущий заказ на hours часов (o — заказ до продления).
    virtual ExecState extend(const Order& o, int hours) = 0;
    virtual void stop(const Order& o, const std::string& reason) = 0;
};

// Ручной режим: админ получает уведомление о новом заказе и жмёт «Запустить»/«Отклонить».
class ManualExecutor : public OrderExecutor {
public:
    bool automatic() const override { return false; }
    ExecState start(const Order& o, int64_t endsAt) override {
        ExecState s;
        s.ok = true;
        s.status = "running";
        s.assigned = s.requested = o.bots;
        s.endsAt = endsAt;
        return s;
    }
    ExecState extend(const Order& o, int hours) override {
        ExecState s;
        s.ok = true;
        s.status = "running";
        s.assigned = s.requested = o.bots;
        s.endsAt = o.endsAt + static_cast<int64_t>(hours) * 3600;
        return s;
    }
    void stop(const Order&, const std::string&) override {}
};

// HTTP-режим (совместим с krendelbot):
//   POST {url}/start  {"order_id","channel","bots","hours","ends_at","tier"} -> состояние заказа
//                     tier — id тарифа из pricing.tiers (smart | dumb)
//   POST {url}/stop   {"order_id","reason"}
//   GET  {url}/orders/{order_id}                                       -> состояние заказа (404 — удалён)
//   POST {url}/orders/{order_id}/extend {"hours"}                      -> состояние заказа (409 — уже завершён)
//   GET  {url}/status                                                  -> {"bots_free", ...}
// Номер заказа у исполнителя = order_id_offset + номер в Telegram-боте,
// чтобы не пересекаться с заказами, созданными на сервере вручную.
class HttpExecutor : public OrderExecutor {
public:
    HttpExecutor(std::string url, std::string token, int64_t idOffset)
        : url_(std::move(url)), token_(std::move(token)), offset_(idOffset) {}
    bool automatic() const override { return true; }
    Capacity capacity() override;
    ExecState start(const Order& o, int64_t endsAt) override;
    ExecState query(const Order& o) override;
    ExecState extend(const Order& o, int hours) override;
    void stop(const Order& o, const std::string& reason) override;

private:
    int64_t extId(const Order& o) const { return offset_ + o.id; }
    std::string url_, token_;
    int64_t offset_;
};

std::unique_ptr<OrderExecutor> makeExecutor(const Config& cfg);
