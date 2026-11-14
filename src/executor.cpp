#include "executor.hpp"

#include <cpr/cpr.h>

#include <nlohmann/json.hpp>

#include "util.hpp"

using json = nlohmann::json;

namespace {

cpr::Header headers(const std::string& token) {
    cpr::Header h{{"Content-Type", "application/json"}};
    if (!token.empty()) h["Authorization"] = "Bearer " + token;
    return h;
}

// Разбирает ответ сервера в ExecState (формат заказа krendelbot).
ExecState parseState(const cpr::Response& r) {
    ExecState s;
    if (r.error) {
        s.error = r.error.message;
        return s;
    }
    s.httpCode = static_cast<int>(r.status_code);
    json j = json::parse(r.text, nullptr, false);
    if (r.status_code < 200 || r.status_code >= 300) {
        std::string msg = !j.is_discarded() && j.is_object() ? j.value("error", r.text) : r.text;
        s.error = "HTTP " + std::to_string(r.status_code) + ": " + msg.substr(0, 300);
        return s;
    }
    if (j.is_discarded() || !j.is_object()) {
        s.error = "ответ не JSON-объект";
        return s;
    }
    s.ok = true;
    s.status = j.value("status", "");
    s.assigned = j.value("bots_assigned", 0);
    s.requested = j.value("bots_requested", 0);
    s.endsAt = j.value("ends_at", int64_t{0});
    s.reason = j.value("reason", "");
    return s;
}

}  // namespace

OrderExecutor::Capacity HttpExecutor::capacity() {
    Capacity c;
    // Короткий таймаут: вызывается и из диалога заказа, клиент не должен ждать.
    auto r = cpr::Get(cpr::Url{url_ + "/status"}, headers(token_), cpr::Timeout{5000});
    if (r.error || r.status_code != 200) return c;
    json j = json::parse(r.text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return c;
    if (j.contains("bots_free") && j["bots_free"].is_number_integer()) c.free = j["bots_free"].get<int>();
    if (j.contains("bots_total") && j["bots_total"].is_number_integer()) c.total = j["bots_total"].get<int>();
    return c;
}

ExecState HttpExecutor::start(const Order& o, int64_t endsAt) {
    json body = {{"order_id", extId(o)}, {"channel", o.channel}, {"bots", o.bots},
                 {"hours", o.hours},     {"ends_at", endsAt},     {"tier", o.tier}};
    auto s = parseState(cpr::Post(cpr::Url{url_ + "/start"}, headers(token_), cpr::Body{body.dump()},
                                  cpr::Timeout{20000}));
    if (s.ok && s.requested == 0) s.requested = o.bots;
    if (s.ok && s.endsAt == 0) s.endsAt = endsAt;
    return s;
}

ExecState HttpExecutor::query(const Order& o) {
    return parseState(cpr::Get(cpr::Url{url_ + "/orders/" + std::to_string(extId(o))}, headers(token_),
                               cpr::Timeout{10000}));
}

ExecState HttpExecutor::extend(const Order& o, int hours) {
    json body = {{"hours", hours}};
    return parseState(cpr::Post(cpr::Url{url_ + "/orders/" + std::to_string(extId(o)) + "/extend"}, headers(token_),
                                cpr::Body{body.dump()}, cpr::Timeout{15000}));
}

void HttpExecutor::stop(const Order& o, const std::string& reason) {
    json body = {{"order_id", extId(o)}, {"reason", reason}};
    auto s = parseState(cpr::Post(cpr::Url{url_ + "/stop"}, headers(token_), cpr::Body{body.dump()},
                                  cpr::Timeout{15000}));
    if (!s.ok) util::log("executor stop #" + std::to_string(o.id) + ": " + s.error);
}

std::unique_ptr<OrderExecutor> makeExecutor(const Config& cfg) {
    if (cfg.executorType == "http")
        return std::make_unique<HttpExecutor>(cfg.executorUrl, cfg.executorToken, cfg.executorIdOffset);
    return std::make_unique<ManualExecutor>();
}
