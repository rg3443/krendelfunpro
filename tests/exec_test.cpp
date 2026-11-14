// Проверка HttpExecutor против tests/fake_krendelbot.py (запускается заранее на порту 18080, токен "secret").
#include <cpr/cpr.h>

#include <iostream>

#include "../src/executor.hpp"
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
    const std::string url = "http://127.0.0.1:18080/api";
    HttpExecutor ex(url, "secret", 100000);
    HttpExecutor bad(url, "wrong", 100000);

    CHECK(ex.freeBots() == 3);
    CHECK(ex.capacity().total == 3);
    CHECK(bad.freeBots() == -1 && bad.capacity().total == -1);

    Order o;
    o.id = 7;
    o.channel = "some_channel";
    o.tier = "smart";
    o.bots = 5;
    o.hours = 2;
    int64_t ends = util::now() + 7200;

    auto b = bad.start(o, ends);
    CHECK(!b.ok && b.error.find("401") != std::string::npos);

    auto s = ex.start(o, ends);
    CHECK(s.ok);
    CHECK(s.status == "running");
    CHECK(s.assigned == 3 && s.requested == 5);
    CHECK(s.endsAt == ends);

    auto again = ex.start(o, ends + 999);  // повтор — то же состояние, не второй заказ
    CHECK(again.ok && again.endsAt == ends && again.assigned == 3);
    CHECK(ex.freeBots() == 0);

    // У сервера заказ лежит под номером 100007
    auto raw = cpr::Get(cpr::Url{url + "/orders/100007"}, cpr::Header{{"Authorization", "Bearer secret"}});
    CHECK(raw.status_code == 200);
    CHECK(raw.text.find("\"tier\": \"smart\"") != std::string::npos);  // тариф дошёл до сервера

    auto q = ex.query(o);
    CHECK(q.ok && q.status == "running" && q.assigned == 3);

    auto ext = ex.extend(o, 3);  // продление идущего
    CHECK(ext.ok && ext.endsAt == ends + 3 * 3600);
    CHECK(ex.query(o).endsAt == ends + 3 * 3600);

    Order o2 = o;
    o2.id = 8;
    auto w = ex.start(o2, ends);  // свободных нет -> waiting
    CHECK(w.ok && w.status == "waiting" && w.assigned == 0);

    cpr::Post(cpr::Url{url + "/_fail"}, cpr::Header{{"Authorization", "Bearer secret"}},
              cpr::Body{R"({"order_id":100008})"});
    auto f = ex.query(o2);
    CHECK(f.ok && f.status == "failed" && f.reason == "канал не найден");

    ex.stop(o, "test");
    auto st = ex.query(o);
    CHECK(st.ok && st.status == "stopped" && st.reason == "test");

    auto ext2 = ex.extend(o, 1);  // завершённый продлить нельзя
    CHECK(!ext2.ok && ext2.httpCode == 409);

    // Удалённый на сервере заказ -> 404
    cpr::Delete(cpr::Url{url + "/orders/100008"}, cpr::Header{{"Authorization", "Bearer secret"}});
    auto del = ex.query(o2);
    CHECK(!del.ok && del.httpCode == 404);
    CHECK(!ex.extend(o2, 1).ok);

    Order none = o;
    none.id = 999;
    auto nq = ex.query(none);
    CHECK(!nq.ok && nq.error.find("404") != std::string::npos);

    std::cout << (failures ? "TESTS FAILED" : "ALL TESTS PASSED") << "\n";
    return failures ? 1 : 0;
}
