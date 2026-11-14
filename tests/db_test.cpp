// Тест денежной логики: зачисление, двойное зачисление, списание за заказ, возвраты.
#include <cstdio>
#include <filesystem>
#include <iostream>

#include "../src/db.hpp"
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
    auto path = (std::filesystem::temp_directory_path() / "rentbot_test.db").string();
    std::filesystem::remove(path);
    {
        Db db(path);
        db.ensureUser(42, "tester");
        CHECK(db.getUser(42)->balance == 0);
        CHECK(db.getUser(42)->receiptContact.empty());
        db.setReceiptContact(42, "name@mail.ru");
        db.ensureUser(42, "tester2");  // повторный ensureUser не стирает контакт
        CHECK(db.getUser(42)->receiptContact == "name@mail.ru" && db.getUser(42)->username == "tester2");

        // Платёж: зачисляется ровно один раз
        int64_t pid = db.createPayment(42, "mock", 50000);
        CHECK(!db.completePayment(pid));  // ещё 'new'
        db.setPaymentPending(pid, "mock-1", "https://x");
        CHECK(db.pendingPayments().size() == 1);
        CHECK(db.completePayment(pid));
        CHECK(!db.completePayment(pid));
        CHECK(db.getUser(42)->balance == 50000);
        CHECK(!db.closePayment(pid, "canceled"));

        // Заказ: недостаточно средств
        CHECK(!db.createOrder(42, "chan", "smart", 200, 100, 10).has_value());
        CHECK(db.getUser(42)->balance == 50000);

        // Заказ в очереди -> отмена с полным возвратом
        auto o1 = db.createOrder(42, "chan", "smart", 200, 10, 2);
        CHECK(o1.has_value());
        CHECK(db.getUser(42)->balance == 46000);
        auto r1 = db.cancelOrder(*o1, "cancelled", "test");
        CHECK(r1 && *r1 == 4000);
        CHECK(db.getUser(42)->balance == 50000);
        CHECK(!db.cancelOrder(*o1, "cancelled", "again").has_value());

        // Запущенный заказ, прерван на середине -> возврат ~половины
        auto o2 = db.createOrder(42, "chan", "smart", 360, 10, 2);
        int64_t now = util::now();
        CHECK(db.startOrder(*o2, now - 3600, now + 3600, 10, "running"));
        CHECK(!db.startOrder(*o2, now, now + 1, 10, "running"));
        auto r2 = db.cancelOrder(*o2, "failed", "test");
        CHECK(r2 && *r2 >= 3590 && *r2 <= 3600);

        // Прерван на середине, причём половину прошедшего времени было 5 ботов из 10:
        // возврат = половина цены за остаток + четверть за недоданное в прошедшей половине
        auto o2b = db.createOrder(42, "chan", "smart", 400, 10, 2);
        CHECK(db.startOrder(*o2b, now - 3600, now + 3600, 5, "running"));
        db.updateOrderProgress(*o2b, "running", 5, 5 * 3600);
        auto r2b = db.cancelOrder(*o2b, "failed", "test");
        CHECK(r2b && *r2b >= 5990 && *r2b <= 6000);

        // Завершение по времени без возврата
        auto o3 = db.createOrder(42, "chan", "smart", 200, 1, 1);
        CHECK(db.startOrder(*o3, now, now + 3600, 1, "running"));
        auto c3 = db.finishOrder(*o3);
        CHECK(c3 && *c3 == 0);
        CHECK(!db.finishOrder(*o3).has_value());
        CHECK(!db.cancelOrder(*o3, "failed", "late").has_value());
        CHECK(db.getOrder(*o3)->status == "completed");

        // Завершение с компенсацией: 2 бота из 4 весь заказ -> вернуть половину
        int64_t before = db.getUser(42)->balance;
        auto o4 = db.createOrder(42, "chan", "smart", 200, 4, 1);
        CHECK(db.startOrder(*o4, now - 3600, now, 2, "running"));
        db.updateOrderProgress(*o4, "running", 2, 2 * 3600);
        CHECK(db.getOrder(*o4)->botsActive == 2);
        auto c4 = db.finishOrder(*o4);
        CHECK(c4 && *c4 == 400);
        CHECK(db.getUser(42)->balance == before - 800 + 400);

        // Продление
        {
            int64_t b0 = db.getUser(42)->balance;
            auto oe = db.createOrder(42, "chan", "smart", 200, 2, 1);
            CHECK(db.startOrder(*oe, now, now + 3600, 2, "running"));
            CHECK(db.markReminded(*oe));
            CHECK(!db.markReminded(*oe));
            CHECK(db.extendOrder(*oe, 42, 2, 800) == ExtendResult::Ok);
            auto e = db.getOrder(*oe);
            CHECK(e->hours == 3 && e->price == 1200 && e->endsAt == now + 3 * 3600 && !e->reminded);
            CHECK(db.getUser(42)->balance == b0 - 1200);
            CHECK(db.extendOrder(*oe, 43, 1, 400) == ExtendResult::NotRunning);  // чужой заказ
            CHECK(db.extendOrder(*oe, 42, 1, 100000000) == ExtendResult::NoFunds);
            CHECK(db.getOrder(*oe)->hours == 3);  // при нехватке денег заказ не изменился
            db.revertExtension(*oe, 2, 800);       // сервер отказал
            e = db.getOrder(*oe);
            CHECK(e->hours == 1 && e->price == 400 && e->endsAt == now + 3600);
            CHECK(db.getUser(42)->balance == b0 - 400);
            // закрытый заказ: продлить нельзя, откат не возвращает деньги второй раз
            CHECK(db.extendOrder(*oe, 42, 1, 400) == ExtendResult::Ok);
            auto rr = db.cancelOrder(*oe, "failed", "t");
            CHECK(rr.has_value());
            int64_t b1 = db.getUser(42)->balance;
            db.revertExtension(*oe, 1, 400);
            CHECK(db.getUser(42)->balance == b1);
            CHECK(db.extendOrder(*oe, 42, 1, 400) == ExtendResult::NotRunning);
            // в последнюю минуту продлевать нельзя
            auto ol = db.createOrder(42, "chan", "smart", 200, 1, 1);
            CHECK(db.startOrder(*ol, now - 3570, now + 30, 1, "running"));
            CHECK(db.extendOrder(*ol, 42, 1, 200) == ExtendResult::NotRunning);
            db.cancelOrder(*ol, "failed", "t");
        }

        // Оценка возврата совпадает с фактическим возвратом при отмене
        {
            auto oq = db.createOrder(42, "chan", "smart", 200, 2, 1);
            CHECK(Db::refundFor(*db.getOrder(*oq), now) == 400);
            auto orun = db.createOrder(42, "chan", "smart", 200, 4, 2);
            CHECK(db.startOrder(*orun, now - 1800, now + 5400, 2, "running"));
            db.updateOrderProgress(*orun, "running", 2, 2 * 1800);
            auto est = Db::refundFor(*db.getOrder(*orun), util::now());
            // 3/4 времени не использовано (1200) + 2 бота из 4 недоданы 1/4 времени (200)
            CHECK(est >= 1395 && est <= 1400);
            auto real = db.cancelOrder(*orun, "cancelled", "t");
            CHECK(real && *real <= est && est - *real <= 2);
            CHECK(db.getOrder(*orun)->status == "cancelled");
            CHECK(Db::refundFor(*db.getOrder(*orun), now) == 0);
            db.cancelOrder(*oq, "cancelled", "t");
        }

        // Отмена заказчиком без возврата + тариф и цена за бот-час сохраняются в заказе
        {
            int64_t b0 = db.getUser(42)->balance;
            auto od = db.createOrder(42, "chan", "dumb", 1000, 3, 2);  // 10 ₽ × 3 бота × 2 ч
            auto d = db.getOrder(*od);
            CHECK(d->tier == "dumb" && d->unitPrice == 1000 && d->price == 6000);
            CHECK(db.getUser(42)->balance == b0 - 6000);
            auto r = db.cancelOrder(*od, "cancelled", "t", /*withRefund*/ false);
            CHECK(r && *r == 0);
            CHECK(db.getUser(42)->balance == b0 - 6000);
            CHECK(db.getOrder(*od)->status == "cancelled");

            auto orun = db.createOrder(42, "chan", "smart", 10000, 1, 1);
            CHECK(db.startOrder(*orun, now, now + 3600, 1, "running"));
            int64_t b1 = db.getUser(42)->balance;
            auto rn = db.cancelOrder(*orun, "cancelled", "t", false);
            CHECK(rn && *rn == 0 && db.getUser(42)->balance == b1);
        }

        // Ручная корректировка не уводит баланс в минус
        int64_t bal = db.getUser(42)->balance;
        CHECK(!db.adjustBalance(42, -(bal + 1), "admin", "t"));
        CHECK(db.adjustBalance(42, -bal, "admin", "t"));
        CHECK(db.getUser(42)->balance == 0);

        auto st = db.stats();
        CHECK(st.paidTotal == 50000);
        CHECK(st.ordersCompleted == 2);
    }
    std::filesystem::remove(path);
    std::cout << (failures ? "TESTS FAILED" : "ALL TESTS PASSED") << "\n";
    return failures ? 1 : 0;
}
