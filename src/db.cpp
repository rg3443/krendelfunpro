#include "db.hpp"

#include <sqlite3.h>

#include <algorithm>

#include "util.hpp"

namespace {

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) throw DbError(sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, int64_t v) {
        check(sqlite3_bind_int64(s_, i, v));
        return *this;
    }
    Stmt& bind(int i, const std::string& v) {
        check(sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
        return *this;
    }
    // true — есть строка, false — конец
    bool step() {
        int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw DbError(sqlite3_errmsg(db_));
    }
    void run() { step(); }
    int64_t i64(int c) { return sqlite3_column_int64(s_, c); }
    std::string str(int c) {
        auto p = sqlite3_column_text(s_, c);
        return p ? reinterpret_cast<const char*>(p) : "";
    }

private:
    void check(int rc) {
        if (rc != SQLITE_OK) throw DbError(sqlite3_errmsg(db_));
    }
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};

class Tx {
public:
    explicit Tx(sqlite3* db) : db_(db) { run("BEGIN IMMEDIATE"); }
    ~Tx() {
        if (!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        run("COMMIT");
        done_ = true;
    }

private:
    void run(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "sqlite error";
            sqlite3_free(err);
            throw DbError(e);
        }
    }
    sqlite3* db_;
    bool done_ = false;
};

const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS users(
  id INTEGER PRIMARY KEY,
  username TEXT,
  balance INTEGER NOT NULL DEFAULT 0 CHECK(balance >= 0),
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS transactions(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES users(id),
  amount INTEGER NOT NULL,
  kind TEXT NOT NULL,
  ref TEXT,
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS payments(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES users(id),
  provider TEXT NOT NULL,
  provider_id TEXT,
  amount INTEGER NOT NULL,
  status TEXT NOT NULL,
  url TEXT,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_payments_status ON payments(status);
CREATE TABLE IF NOT EXISTS orders(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES users(id),
  channel TEXT NOT NULL,
  bots INTEGER NOT NULL,
  hours INTEGER NOT NULL,
  price INTEGER NOT NULL,
  status TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  started_at INTEGER,
  ends_at INTEGER,
  note TEXT
);
CREATE INDEX IF NOT EXISTS idx_orders_status ON orders(status);
CREATE INDEX IF NOT EXISTS idx_orders_user ON orders(user_id);
)SQL";

const char* kPaymentCols = "id,user_id,provider,provider_id,amount,status,url,created_at,updated_at";
const char* kOrderCols =
    "id,user_id,channel,bots,hours,price,status,created_at,started_at,ends_at,note,ext_status,bots_active,missing_bot_sec,"
    "reminded,tier,unit_price";

// Компенсация за недоданные бот-секунды: доля цены, пропорциональная недоданному объёму.
int64_t missingCompensation(const Order& o) {
    int64_t total = o.endsAt > o.startedAt ? o.endsAt - o.startedAt : static_cast<int64_t>(o.hours) * 3600;
    int64_t volume = std::max<int64_t>(1, total * std::max(1, o.bots));
    return std::min(o.price, o.price * std::max<int64_t>(0, o.missingBotSec) / volume);
}

Payment readPayment(Stmt& s) {
    Payment p;
    p.id = s.i64(0);
    p.userId = s.i64(1);
    p.provider = s.str(2);
    p.providerId = s.str(3);
    p.amount = s.i64(4);
    p.status = s.str(5);
    p.url = s.str(6);
    p.createdAt = s.i64(7);
    p.updatedAt = s.i64(8);
    return p;
}

Order readOrder(Stmt& s) {
    Order o;
    o.id = s.i64(0);
    o.userId = s.i64(1);
    o.channel = s.str(2);
    o.bots = static_cast<int>(s.i64(3));
    o.hours = static_cast<int>(s.i64(4));
    o.price = s.i64(5);
    o.status = s.str(6);
    o.createdAt = s.i64(7);
    o.startedAt = s.i64(8);
    o.endsAt = s.i64(9);
    o.note = s.str(10);
    o.extStatus = s.str(11);
    o.botsActive = static_cast<int>(s.i64(12));
    o.missingBotSec = s.i64(13);
    o.reminded = s.i64(14) != 0;
    o.tier = s.str(15);
    o.unitPrice = s.i64(16);
    return o;
}

void addTx(sqlite3* db, int64_t userId, int64_t amount, const std::string& kind, const std::string& ref) {
    Stmt(db, "INSERT INTO transactions(user_id,amount,kind,ref,created_at) VALUES(?,?,?,?,?)")
        .bind(1, userId)
        .bind(2, amount)
        .bind(3, kind)
        .bind(4, ref)
        .bind(5, util::now())
        .run();
}

}  // namespace

Db::Db(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string e = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        sqlite3_close(db_);
        throw DbError(e);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec(kSchema);
    migrate();
}

// Добавляет колонки, появившиеся в новых версиях, в уже существующую базу.
void Db::migrate() {
    auto columns = [&](const char* table) {
        std::vector<std::string> out;
        Stmt s(db_, (std::string("PRAGMA table_info(") + table + ")").c_str());
        while (s.step()) out.push_back(s.str(1));
        return out;
    };
    auto userCols = columns("users");
    if (std::find(userCols.begin(), userCols.end(), "receipt_contact") == userCols.end())
        exec("ALTER TABLE users ADD COLUMN receipt_contact TEXT NOT NULL DEFAULT ''");

    std::vector<std::string> cols = columns("orders");
    auto has = [&](const char* c) { return std::find(cols.begin(), cols.end(), c) != cols.end(); };
    if (!has("ext_status")) exec("ALTER TABLE orders ADD COLUMN ext_status TEXT");
    if (!has("bots_active")) exec("ALTER TABLE orders ADD COLUMN bots_active INTEGER NOT NULL DEFAULT 0");
    if (!has("missing_bot_sec")) exec("ALTER TABLE orders ADD COLUMN missing_bot_sec INTEGER NOT NULL DEFAULT 0");
    if (!has("reminded")) exec("ALTER TABLE orders ADD COLUMN reminded INTEGER NOT NULL DEFAULT 0");
    if (!has("tier")) exec("ALTER TABLE orders ADD COLUMN tier TEXT NOT NULL DEFAULT ''");
    if (!has("unit_price")) {
        exec("ALTER TABLE orders ADD COLUMN unit_price INTEGER NOT NULL DEFAULT 0");
        exec("UPDATE orders SET unit_price = price / (bots * hours) WHERE bots > 0 AND hours > 0");
    }
}

Db::~Db() { sqlite3_close(db_); }

void Db::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "sqlite error";
        sqlite3_free(err);
        throw DbError(e);
    }
}

// ---------------- пользователи ----------------

void Db::ensureUser(int64_t id, const std::string& username) {
    std::lock_guard lk(mu_);
    Stmt(db_,
         "INSERT INTO users(id,username,balance,created_at) VALUES(?,?,0,?) "
         "ON CONFLICT(id) DO UPDATE SET username=excluded.username")
        .bind(1, id)
        .bind(2, username)
        .bind(3, util::now())
        .run();
}

std::optional<User> Db::getUser(int64_t id) {
    std::lock_guard lk(mu_);
    Stmt s(db_, "SELECT id,username,balance,created_at,receipt_contact FROM users WHERE id=?");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return User{s.i64(0), s.str(1), s.i64(2), s.i64(3), s.str(4)};
}

void Db::setReceiptContact(int64_t id, const std::string& contact) {
    std::lock_guard lk(mu_);
    Stmt(db_, "UPDATE users SET receipt_contact=? WHERE id=?").bind(1, contact).bind(2, id).run();
}

bool Db::adjustBalance(int64_t userId, int64_t delta, const std::string& kind, const std::string& ref) {
    std::lock_guard lk(mu_);
    Tx tx(db_);
    Stmt s(db_, "UPDATE users SET balance=balance+? WHERE id=? AND balance+?>=0");
    s.bind(1, delta).bind(2, userId).bind(3, delta).run();
    if (sqlite3_changes(db_) != 1) return false;
    addTx(db_, userId, delta, kind, ref);
    tx.commit();
    return true;
}

// ---------------- платежи ----------------

int64_t Db::createPayment(int64_t userId, const std::string& provider, int64_t amount) {
    std::lock_guard lk(mu_);
    int64_t t = util::now();
    Stmt(db_, "INSERT INTO payments(user_id,provider,amount,status,created_at,updated_at) VALUES(?,?,?,'new',?,?)")
        .bind(1, userId)
        .bind(2, provider)
        .bind(3, amount)
        .bind(4, t)
        .bind(5, t)
        .run();
    return sqlite3_last_insert_rowid(db_);
}

void Db::setPaymentPending(int64_t id, const std::string& providerId, const std::string& url) {
    std::lock_guard lk(mu_);
    Stmt(db_, "UPDATE payments SET status='pending',provider_id=?,url=?,updated_at=? WHERE id=? AND status='new'")
        .bind(1, providerId)
        .bind(2, url)
        .bind(3, util::now())
        .bind(4, id)
        .run();
}

void Db::setPaymentError(int64_t id) {
    std::lock_guard lk(mu_);
    Stmt(db_, "UPDATE payments SET status='error',updated_at=? WHERE id=? AND status='new'")
        .bind(1, util::now())
        .bind(2, id)
        .run();
}

std::optional<Payment> Db::getPayment(int64_t id) {
    std::lock_guard lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kPaymentCols + " FROM payments WHERE id=?").c_str());
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return readPayment(s);
}

std::vector<Payment> Db::pendingPayments() {
    std::lock_guard lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kPaymentCols + " FROM payments WHERE status='pending' ORDER BY id").c_str());
    std::vector<Payment> out;
    while (s.step()) out.push_back(readPayment(s));
    return out;
}

bool Db::completePayment(int64_t id) {
    std::lock_guard lk(mu_);
    Tx tx(db_);
    Stmt(db_, "UPDATE payments SET status='succeeded',updated_at=? WHERE id=? AND status IN ('pending','expired')")
        .bind(1, util::now())
        .bind(2, id)
        .run();
    if (sqlite3_changes(db_) != 1) return false;

    int64_t userId = 0, amount = 0;
    {
        Stmt s(db_, "SELECT user_id,amount FROM payments WHERE id=?");
        s.bind(1, id);
        s.step();
        userId = s.i64(0);
        amount = s.i64(1);
    }
    Stmt(db_, "UPDATE users SET balance=balance+? WHERE id=?").bind(1, amount).bind(2, userId).run();
    addTx(db_, userId, amount, "topup", "payment:" + std::to_string(id));
    tx.commit();
    return true;
}

bool Db::closePayment(int64_t id, const std::string& newStatus) {
    std::lock_guard lk(mu_);
    Stmt(db_, "UPDATE payments SET status=?,updated_at=? WHERE id=? AND status='pending'")
        .bind(1, newStatus)
        .bind(2, util::now())
        .bind(3, id)
        .run();
    return sqlite3_changes(db_) == 1;
}

// ---------------- заказы ----------------

std::optional<int64_t> Db::createOrder(int64_t userId, const std::string& channel, const std::string& tier,
                                       int64_t unitPrice, int bots, int hours) {
    int64_t price = unitPrice * bots * hours;
    std::lock_guard lk(mu_);
    Tx tx(db_);
    Stmt(db_, "UPDATE users SET balance=balance-? WHERE id=? AND balance>=?")
        .bind(1, price)
        .bind(2, userId)
        .bind(3, price)
        .run();
    if (sqlite3_changes(db_) != 1) return std::nullopt;

    Stmt(db_,
         "INSERT INTO orders(user_id,channel,bots,hours,price,status,created_at,tier,unit_price) "
         "VALUES(?,?,?,?,?,'queued',?,?,?)")
        .bind(1, userId)
        .bind(2, channel)
        .bind(3, static_cast<int64_t>(bots))
        .bind(4, static_cast<int64_t>(hours))
        .bind(5, price)
        .bind(6, util::now())
        .bind(7, tier)
        .bind(8, unitPrice)
        .run();
    int64_t orderId = sqlite3_last_insert_rowid(db_);
    addTx(db_, userId, -price, "order", "order:" + std::to_string(orderId));
    tx.commit();
    return orderId;
}

std::optional<Order> Db::getOrder(int64_t id) {
    std::lock_guard lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kOrderCols + " FROM orders WHERE id=?").c_str());
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return readOrder(s);
}

std::vector<Order> Db::userOrders(int64_t userId, int limit) {
    std::lock_guard lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kOrderCols + " FROM orders WHERE user_id=? ORDER BY id DESC LIMIT ?").c_str());
    s.bind(1, userId).bind(2, static_cast<int64_t>(limit));
    std::vector<Order> out;
    while (s.step()) out.push_back(readOrder(s));
    return out;
}

std::vector<Order> Db::ordersByStatus(const std::string& status) {
    std::lock_guard lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kOrderCols + " FROM orders WHERE status=? ORDER BY id").c_str());
    s.bind(1, status);
    std::vector<Order> out;
    while (s.step()) out.push_back(readOrder(s));
    return out;
}

bool Db::startOrder(int64_t id, int64_t startedAt, int64_t endsAt, int botsActive, const std::string& extStatus) {
    std::lock_guard lk(mu_);
    Stmt(db_,
         "UPDATE orders SET status='running',started_at=?,ends_at=?,bots_active=?,ext_status=? "
         "WHERE id=? AND status='queued'")
        .bind(1, startedAt)
        .bind(2, endsAt)
        .bind(3, static_cast<int64_t>(botsActive))
        .bind(4, extStatus)
        .bind(5, id)
        .run();
    return sqlite3_changes(db_) == 1;
}

void Db::updateOrderProgress(int64_t id, const std::string& extStatus, int botsActive, int64_t addMissingBotSec) {
    std::lock_guard lk(mu_);
    Stmt(db_,
         "UPDATE orders SET ext_status=?,bots_active=?,missing_bot_sec=missing_bot_sec+? "
         "WHERE id=? AND status='running'")
        .bind(1, extStatus)
        .bind(2, static_cast<int64_t>(botsActive))
        .bind(3, std::max<int64_t>(0, addMissingBotSec))
        .bind(4, id)
        .run();
}

std::optional<int64_t> Db::finishOrder(int64_t id) {
    std::lock_guard lk(mu_);
    Tx tx(db_);
    Order o;
    {
        Stmt s(db_, (std::string("SELECT ") + kOrderCols + " FROM orders WHERE id=?").c_str());
        s.bind(1, id);
        if (!s.step()) return std::nullopt;
        o = readOrder(s);
    }
    Stmt(db_, "UPDATE orders SET status='completed' WHERE id=? AND status='running'").bind(1, id).run();
    if (sqlite3_changes(db_) != 1) return std::nullopt;

    int64_t comp = missingCompensation(o);
    if (comp > 0) {
        Stmt(db_, "UPDATE users SET balance=balance+? WHERE id=?").bind(1, comp).bind(2, o.userId).run();
        addTx(db_, o.userId, comp, "refund", "order:" + std::to_string(id) + ":missing");
    }
    tx.commit();
    return comp;
}

std::optional<int64_t> Db::cancelOrder(int64_t id, const std::string& newStatus, const std::string& note,
                                       bool withRefund) {
    std::lock_guard lk(mu_);
    Tx tx(db_);
    Order o;
    {
        Stmt s(db_, (std::string("SELECT ") + kOrderCols + " FROM orders WHERE id=?").c_str());
        s.bind(1, id);
        if (!s.step()) return std::nullopt;
        o = readOrder(s);
    }

    if (o.status != "queued" && o.status != "running") return std::nullopt;
    int64_t refund = withRefund ? refundFor(o, util::now()) : 0;

    Stmt(db_, "UPDATE orders SET status=?,note=? WHERE id=? AND status=?")
        .bind(1, newStatus)
        .bind(2, note)
        .bind(3, id)
        .bind(4, o.status)
        .run();
    if (sqlite3_changes(db_) != 1) return std::nullopt;

    if (refund > 0) {
        Stmt(db_, "UPDATE users SET balance=balance+? WHERE id=?").bind(1, refund).bind(2, o.userId).run();
        addTx(db_, o.userId, refund, "refund", "order:" + std::to_string(id));
    }
    tx.commit();
    return refund;
}

int64_t Db::refundFor(const Order& o, int64_t now) {
    if (o.status == "queued") return o.price;
    if (o.status != "running") return 0;
    int64_t total = std::max<int64_t>(1, o.endsAt - o.startedAt);
    int64_t left = std::clamp<int64_t>(o.endsAt - now, 0, total);
    return std::min(o.price, o.price * left / total + missingCompensation(o));
}

ExtendResult Db::extendOrder(int64_t id, int64_t userId, int hours, int64_t price) {
    std::lock_guard lk(mu_);
    Tx tx(db_);
    int64_t add = static_cast<int64_t>(hours) * 3600;
    Stmt(db_,
         "UPDATE orders SET hours=hours+?,price=price+?,ends_at=ends_at+?,reminded=0 "
         "WHERE id=? AND user_id=? AND status='running' AND ends_at>?")
        .bind(1, static_cast<int64_t>(hours))
        .bind(2, price)
        .bind(3, add)
        .bind(4, id)
        .bind(5, userId)
        .bind(6, util::now() + 60)  // в последнюю минуту не продлеваем: сервер уже может закрывать заказ
        .run();
    if (sqlite3_changes(db_) != 1) return ExtendResult::NotRunning;

    Stmt(db_, "UPDATE users SET balance=balance-? WHERE id=? AND balance>=?")
        .bind(1, price)
        .bind(2, userId)
        .bind(3, price)
        .run();
    if (sqlite3_changes(db_) != 1) return ExtendResult::NoFunds;  // Tx откатит и изменение заказа

    addTx(db_, userId, -price, "extend", "order:" + std::to_string(id));
    tx.commit();
    return ExtendResult::Ok;
}

void Db::revertExtension(int64_t id, int hours, int64_t price) {
    std::lock_guard lk(mu_);
    Tx tx(db_);
    int64_t userId = 0;
    {
        Stmt s(db_, "SELECT user_id FROM orders WHERE id=?");
        s.bind(1, id);
        if (!s.step()) return;
        userId = s.i64(0);
    }
    // Если заказ уже закрыт (cancelOrder) — его возврат уже учёл продлённую часть, второй раз не возвращаем.
    Stmt(db_, "UPDATE orders SET hours=hours-?,price=price-?,ends_at=ends_at-? WHERE id=? AND status='running'")
        .bind(1, static_cast<int64_t>(hours))
        .bind(2, price)
        .bind(3, static_cast<int64_t>(hours) * 3600)
        .bind(4, id)
        .run();
    if (sqlite3_changes(db_) != 1) return;
    Stmt(db_, "UPDATE users SET balance=balance+? WHERE id=?").bind(1, price).bind(2, userId).run();
    addTx(db_, userId, price, "refund", "order:" + std::to_string(id) + ":extend");
    tx.commit();
}

bool Db::markReminded(int64_t id) {
    std::lock_guard lk(mu_);
    Stmt(db_, "UPDATE orders SET reminded=1 WHERE id=? AND reminded=0 AND status='running'").bind(1, id).run();
    return sqlite3_changes(db_) == 1;
}

Stats Db::stats() {
    std::lock_guard lk(mu_);
    Stats st;
    {
        Stmt s(db_, "SELECT COUNT(*), COALESCE(SUM(balance),0) FROM users");
        s.step();
        st.users = s.i64(0);
        st.totalBalance = s.i64(1);
    }
    {
        Stmt s(db_, "SELECT COALESCE(SUM(amount),0) FROM payments WHERE status='succeeded'");
        s.step();
        st.paidTotal = s.i64(0);
    }
    {
        Stmt s(db_, "SELECT status, COUNT(*) FROM orders GROUP BY status");
        while (s.step()) {
            auto status = s.str(0);
            if (status == "queued") st.ordersQueued = s.i64(1);
            else if (status == "running") st.ordersRunning = s.i64(1);
            else if (status == "completed") st.ordersCompleted = s.i64(1);
        }
    }
    {
        Stmt s(db_, "SELECT -COALESCE(SUM(amount),0) FROM transactions WHERE kind IN ('order','extend','refund')");
        s.step();
        st.revenue = s.i64(0);
    }
    return st;
}
