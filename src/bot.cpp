#include "bot.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <thread>

#include "util.hpp"

using util::htmlEscape;
using util::money;

namespace {

// Тексты кнопок главного меню (reply keyboard)
const std::string kBtnOrder = "🛒 Заказать ботов";
const std::string kBtnOrders = "📦 Мои заказы";
const std::string kBtnBalance = "💰 Баланс";
const std::string kBtnTopup = "➕ Пополнить";
const std::string kBtnHelp = "ℹ️ Помощь";

// Принимает "name", "@name", "twitch.tv/name", "https://www.twitch.tv/name/..." — возвращает логин или "".
std::string parseTwitchChannel(std::string s) {
    s = util::toLower(util::trim(s));
    static const std::regex urlRe(R"(^(?:https?://)?(?:www\.|m\.)?twitch\.tv/([a-z0-9_]+).*$)");
    std::smatch m;
    if (std::regex_match(s, m, urlRe)) s = m[1];
    if (!s.empty() && s[0] == '@') s.erase(0, 1);
    static const std::regex loginRe(R"(^[a-z0-9_]{3,25}$)");
    return std::regex_match(s, loginRe) ? s : "";
}

std::string hoursWord(int h) {
    int n = h % 100;
    if (n >= 11 && n <= 14) return "часов";
    switch (h % 10) {
        case 1: return "час";
        case 2: case 3: case 4: return "часа";
        default: return "часов";
    }
}

std::string botsWord(int b) {
    int n = b % 100;
    if (n >= 11 && n <= 14) return "ботов";
    switch (b % 10) {
        case 1: return "бот";
        case 2: case 3: case 4: return "бота";
        default: return "ботов";
    }
}

int64_t parseIdSuffix(const std::string& data, const std::string& prefix) {
    int64_t v = 0;
    util::parseInt(data.substr(prefix.size()), v);
    return v;
}

bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

// "prefix<a>:<b>" -> a, b
bool parseTwoIds(const std::string& data, const std::string& prefix, int64_t& a, int64_t& b) {
    std::string rest = data.substr(prefix.size());
    auto c = rest.find(':');
    return c != std::string::npos && util::parseInt(rest.substr(0, c), a) && util::parseInt(rest.substr(c + 1), b);
}

}  // namespace

Bot::Bot(const Config& cfg, Db& db, TgApi& tg, PaymentProvider& pay, OrderExecutor& exec)
    : cfg_(cfg), db_(db), tg_(tg), pay_(pay), exec_(exec) {}

// ============================================================================
// Главный цикл
// ============================================================================

void Bot::run(std::atomic<bool>& stop) {
    auto me = tg_.getMe();
    util::log("Запущен бот @" + me.value("username", std::string("?")) + ", платежи: " + pay_.name() +
              ", исполнитель: " + cfg_.executorType);

    std::thread worker([this, &stop] { workerLoop(stop); });

    int64_t offset = 0;
    while (!stop) {
        try {
            auto updates = tg_.getUpdates(offset, 25);
            for (auto& u : updates) {
                offset = std::max(offset, u.value("update_id", int64_t{0}) + 1);
                try {
                    handleUpdate(u);
                } catch (const std::exception& e) {
                    util::log(std::string("Ошибка обработки апдейта: ") + e.what());
                }
            }
        } catch (const std::exception& e) {
            util::log(std::string("getUpdates: ") + e.what());
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
    worker.join();
}

void Bot::workerLoop(std::atomic<bool>& stop) {
    auto lastPayPoll = std::chrono::steady_clock::time_point{};
    auto lastExecPoll = std::chrono::steady_clock::time_point{};
    while (!stop) {
        try {
            auto nowTp = std::chrono::steady_clock::now();
            if (nowTp - lastPayPoll >= std::chrono::seconds(cfg_.paymentPollSec)) {
                lastPayPoll = nowTp;
                pollPayments();
            }
            if (exec_.automatic() && nowTp - lastExecPoll >= std::chrono::seconds(cfg_.executorPollSec)) {
                lastExecPoll = nowTp;
                pollExecutor();
            }
            processOrders();
        } catch (const std::exception& e) {
            util::log(std::string("worker: ") + e.what());
        }
        for (int i = 0; i < 10 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void Bot::handleUpdate(const json& u) {
    if (u.contains("message")) onMessage(u["message"]);
    else if (u.contains("callback_query")) onCallback(u["callback_query"]);
}

// ============================================================================
// Сообщения
// ============================================================================

void Bot::onMessage(const json& m) {
    if (!m.contains("from") || !m.contains("chat")) return;
    if (m["chat"].value("type", "") != "private") return;  // работаем только в личке
    int64_t uid = m["from"].value("id", int64_t{0});
    db_.ensureUser(uid, m["from"].value("username", ""));

    // Кнопка «Отправить телефон» (request_contact): принимаем только собственный контакт пользователя
    if (m.contains("contact")) {
        auto it = sessions_.find(uid);
        if (it != sessions_.end() && it->second.step == Step::ReceiptContact &&
            m["contact"].value("user_id", int64_t{0}) == uid)
            saveReceiptContact(uid, m["contact"].value("phone_number", ""));
        else
            sendMenu(uid, "Чтобы указать телефон для чеков, нажмите /contact.");
        return;
    }

    std::string text = util::trim(m.value("text", ""));
    if (text.empty()) return;

    if (text[0] == '/') {
        auto sp = text.find(' ');
        std::string cmd = text.substr(0, sp);
        auto at = cmd.find('@');  // /start@botname
        if (at != std::string::npos) cmd = cmd.substr(0, at);
        std::string args = sp == std::string::npos ? "" : util::trim(text.substr(sp + 1));
        onCommand(uid, util::toLower(cmd), args);
        return;
    }

    if (text == kBtnOrder) return onCommand(uid, "/order", "");
    if (text == kBtnOrders) return onCommand(uid, "/orders", "");
    if (text == kBtnBalance) return onCommand(uid, "/balance", "");
    if (text == kBtnTopup) return onCommand(uid, "/topup", "");
    if (text == kBtnHelp) return onCommand(uid, "/help", "");

    onText(uid, text);
}

void Bot::onCommand(int64_t uid, const std::string& cmd, const std::string& args) {
    if (isAdmin(uid) && onAdminCommand(uid, cmd, args)) return;

    // Любая обычная команда сбрасывает незаконченный диалог
    bool hadDialog = sessions_.count(uid) && sessions_[uid].step != Step::None;
    sessions_.erase(uid);

    if (cmd == "/start") {
        sendMenu(uid,
                 "👋 Привет! Здесь можно арендовать ботов для чата вашего Twitch-канала.\n\n"
                 "1. Пополните баланс " + payMethodsText() + "\n"
                 "2. Оформите заказ: канал, тип ботов, их количество и время\n"
                 "3. Получайте уведомления о статусе заказа");
        showTariffs(uid);
    } else if (cmd == "/tariffs") {
        showTariffs(uid);
    } else if (cmd == "/balance") {
        showBalance(uid);
    } else if (cmd == "/topup") {
        int64_t kop = 0;
        if (!args.empty() && util::parseRub(args, kop)) createTopup(uid, kop);
        else askTopup(uid);
    } else if (cmd == "/order") {
        startOrderFlow(uid);
    } else if (cmd == "/orders") {
        showOrders(uid);
    } else if (cmd == "/contact") {
        if (!args.empty()) saveReceiptContact(uid, args);
        else askReceiptContact(uid, 0);
    } else if (cmd == "/cancel") {
        // Во время диалога /cancel прерывает диалог. Иначе — отмена заказа: показываем активные заказы.
        if (hadDialog) sendMenu(uid, "Действие отменено.");
        else showCancelableOrders(uid);
    } else if (cmd == "/help") {
        showHelp(uid);
    } else {
        sendMenu(uid, "Неизвестная команда. /help — справка.");
    }
}

void Bot::onText(int64_t uid, const std::string& text) {
    auto it = sessions_.find(uid);
    if (it == sessions_.end() || it->second.step == Step::None) {
        sendMenu(uid, "Выберите действие в меню 👇");
        return;
    }
    Session& s = it->second;

    switch (s.step) {
        case Step::ReceiptContact:
            saveReceiptContact(uid, text);
            return;
        case Step::TopupAmount: {
            int64_t kop = 0;
            if (!util::parseRub(text, kop)) {
                safeSend(uid, "Введите сумму числом, например <code>500</code>.");
                return;
            }
            sessions_.erase(uid);
            createTopup(uid, kop);
            return;
        }
        case Step::OrderChannel: {
            std::string ch = parseTwitchChannel(text);
            if (ch.empty()) {
                safeSend(uid, "Не похоже на Twitch-канал. Пришлите логин (например <code>streamer_name</code>) "
                              "или ссылку <code>https://twitch.tv/streamer_name</code>.");
                return;
            }
            s.channel = ch;
            if (cfg_.findTier(s.tier)) {  // тариф уже выбран (из описания тарифа)
                s.step = Step::OrderBots;
                askBots(uid);
            } else if (cfg_.tiers.size() > 1) {
                s.step = Step::OrderTier;
                askTier(uid);
            } else {
                s.tier = cfg_.tiers.front().id;
                s.step = Step::OrderBots;
                askBots(uid);
            }
            return;
        }
        case Step::OrderTier:
            safeSend(uid, "Выберите тип ботов кнопкой выше.");
            return;
        case Step::OrderBots: {
            int64_t n = 0;
            if (!util::parseInt(text, n) || n < cfg_.minBots || n > cfg_.maxBots) {
                safeSend(uid, "Введите число от " + std::to_string(cfg_.minBots) + " до " +
                                  std::to_string(cfg_.maxBots) + ".");
                return;
            }
            s.bots = static_cast<int>(n);
            s.step = Step::OrderHours;
            askHours(uid);
            return;
        }
        case Step::OrderHours: {
            int64_t n = 0;
            if (!util::parseInt(text, n) || n < cfg_.minHours || n > cfg_.maxHours) {
                safeSend(uid, "Введите количество часов от " + std::to_string(cfg_.minHours) + " до " +
                                  std::to_string(cfg_.maxHours) + ".");
                return;
            }
            s.hours = static_cast<int>(n);
            s.step = Step::OrderConfirm;
            showOrderSummary(uid);
            return;
        }
        case Step::OrderConfirm:
            safeSend(uid, "Подтвердите или отмените заказ кнопками выше.");
            return;
        default:
            sendMenu(uid, "Выберите действие в меню 👇");
    }
}

// ============================================================================
// Callback-кнопки
// ============================================================================

void Bot::onCallback(const json& cq) {
    std::string cbId = cq.value("id", "");
    std::string data = cq.value("data", "");
    int64_t uid = cq["from"].value("id", int64_t{0});
    db_.ensureUser(uid, cq["from"].value("username", ""));

    auto sit = sessions_.find(uid);
    Session* s = sit == sessions_.end() ? nullptr : &sit->second;

    if (startsWith(data, "topup:")) {
        tg_.answerCallback(cbId);
        std::string v = data.substr(6);
        if (v == "custom") {
            sessions_[uid] = Session{Step::TopupAmount};
            safeSend(uid, "Введите сумму пополнения в рублях (от " + money(cfg_.minTopup) + " до " +
                              money(cfg_.maxTopup) + "):");
        } else {
            int64_t rub = 0;
            if (util::parseInt(v, rub)) createTopup(uid, rub * 100);
        }
    } else if (startsWith(data, "paym:")) {  // paym:<способ>:<сумма в копейках>
        tg_.answerCallback(cbId);
        std::string rest = data.substr(5);
        auto colon = rest.find(':');
        int64_t amount = 0;
        if (colon != std::string::npos && util::parseInt(rest.substr(colon + 1), amount) && amount > 0)
            createTopup(uid, amount, rest.substr(0, colon));
    } else if (startsWith(data, "paycheck:")) {
        checkPaymentByUser(uid, parseIdSuffix(data, "paycheck:"), cbId);
    } else if (startsWith(data, "tier:")) {
        tg_.answerCallback(cbId);
        if (!s || s->step != Step::OrderTier) return;
        std::string tier = data.substr(5);
        if (!cfg_.findTier(tier)) return;
        s->tier = tier;
        s->step = Step::OrderBots;
        askBots(uid);
    } else if (startsWith(data, "tierinfo:")) {
        tg_.answerCallback(cbId);
        showTierDetails(uid, data.substr(9));
    } else if (startsWith(data, "tierorder:")) {
        tg_.answerCallback(cbId);
        std::string tier = data.substr(10);
        if (s && s->step == Step::OrderTier && cfg_.findTier(tier)) {  // уже на шаге выбора тарифа
            s->tier = tier;
            s->step = Step::OrderBots;
            askBots(uid);
        } else {
            startOrderFlow(uid, tier);
        }
    } else if (startsWith(data, "bots:")) {
        tg_.answerCallback(cbId);
        if (!s || s->step != Step::OrderBots) return;
        s->bots = static_cast<int>(parseIdSuffix(data, "bots:"));
        s->step = Step::OrderHours;
        askHours(uid);
    } else if (startsWith(data, "hours:")) {
        tg_.answerCallback(cbId);
        if (!s || s->step != Step::OrderHours) return;
        s->hours = static_cast<int>(parseIdSuffix(data, "hours:"));
        s->step = Step::OrderConfirm;
        showOrderSummary(uid);
    } else if (data == "order:confirm") {
        tg_.answerCallback(cbId);
        if (!s || s->step != Step::OrderConfirm) {
            safeSend(uid, "Этот черновик заказа уже неактуален. Начните заново: /order");
            return;
        }
        confirmOrder(uid);
    } else if (data == "order:abort") {
        tg_.answerCallback(cbId, "Заказ отменён");
        sessions_.erase(uid);
        sendMenu(uid, "Черновик заказа удалён.");
    } else if (data == "rcontact") {
        tg_.answerCallback(cbId);
        askReceiptContact(uid, 0);
    } else if (data == "goto:topup") {
        tg_.answerCallback(cbId);
        // Черновик заказа (если есть) сохраняем — после пополнения можно сразу подтвердить.
        askTopup(uid);
    } else if (data == "goto:order") {
        tg_.answerCallback(cbId);
        if (s && s->step == Step::OrderConfirm) showOrderSummary(uid);
        else startOrderFlow(uid);
    } else if (startsWith(data, "ocancel:")) {
        askCancelOrder(uid, parseIdSuffix(data, "ocancel:"), cbId);
    } else if (startsWith(data, "ocancelok:")) {
        cancelOrderByUser(uid, parseIdSuffix(data, "ocancelok:"), cbId);
    } else if (data == "ocancelno") {
        tg_.answerCallback(cbId, "Заказ остаётся в силе");
    } else if (startsWith(data, "ext:")) {
        askExtendHours(uid, parseIdSuffix(data, "ext:"), cbId);
    } else if (startsWith(data, "exth:") || startsWith(data, "extok:")) {
        bool confirm = startsWith(data, "extok:");
        int64_t oid = 0, h = 0;
        if (!parseTwoIds(data, confirm ? "extok:" : "exth:", oid, h)) return tg_.answerCallback(cbId);
        if (confirm) extendOrderByUser(uid, oid, static_cast<int>(h), cbId);
        else showExtendSummary(uid, oid, static_cast<int>(h), cbId);
    } else if (startsWith(data, "adm_start:") || startsWith(data, "adm_reject:")) {
        if (!isAdmin(uid)) return tg_.answerCallback(cbId, "Нет доступа", true);
        bool start = startsWith(data, "adm_start:");
        int64_t oid = parseIdSuffix(data, start ? "adm_start:" : "adm_reject:");
        auto o = db_.getOrder(oid);
        if (!o || o->status != "queued") return tg_.answerCallback(cbId, "Заказ уже не в очереди", true);
        if (start) {
            std::string err;
            if (launchOrder(*o, err)) tg_.answerCallback(cbId, "Заказ #" + std::to_string(oid) + " запущен");
            else tg_.answerCallback(cbId, "Ошибка: " + err, true);
        } else {
            failOrder(*o, "failed", "отклонён администратором");
            tg_.answerCallback(cbId, "Заказ отклонён, средства возвращены");
        }
    } else {
        tg_.answerCallback(cbId);
    }
}

// ============================================================================
// Экраны
// ============================================================================

json Bot::mainKeyboard() const {
    return {{"keyboard",
             {{{{"text", kBtnOrder}}, {{"text", kBtnOrders}}},
              {{{"text", kBtnBalance}}, {{"text", kBtnTopup}}},
              {{{"text", kBtnHelp}}}}},
            {"resize_keyboard", true},
            {"is_persistent", true}};
}

void Bot::sendMenu(int64_t uid, const std::string& text) { safeSend(uid, text, mainKeyboard()); }

void Bot::showBalance(int64_t uid) {
    auto u = db_.getUser(uid);
    int64_t bal = u ? u->balance : 0;
    std::string t = "💰 Ваш баланс: <b>" + money(bal) + "</b>";
    std::vector<std::vector<json>> rows{{TgApi::button("➕ Пополнить баланс", "goto:topup")}};
    if (cfg_.receiptEnabled) {
        t += "\nЧеки приходят на: " +
             (u && !u->receiptContact.empty() ? "<code>" + htmlEscape(u->receiptContact) + "</code>" : "не указано");
        rows.push_back({TgApi::button("✉️ Контакт для чеков", "rcontact")});
    }
    safeSend(uid, t, TgApi::inlineKeyboard(rows));
}

std::string Bot::payMethodLabel(const std::string& method) {
    if (method == "sbp") return "через СБП";
    if (method == "bank_card") return "картой";
    return "";
}

std::string Bot::payMethodsText() const {
    if (cfg_.paymentMethods.empty()) return "";
    std::string t = cfg_.paymentMethods.size() > 1 ? "по СБП или картой" : payMethodLabel(cfg_.paymentMethods[0]);
    return t;
}

// Выбор способа оплаты для суммы amount (копейки). exclude — способ, который только что не сработал.
void Bot::askPayMethod(int64_t uid, int64_t amount, const std::string& note, const std::string& exclude) {
    std::vector<std::vector<json>> rows;
    for (auto& m : cfg_.paymentMethods) {
        if (m == exclude) continue;
        std::string label = m == "sbp" ? "⚡ СБП — через приложение банка" : "💳 Банковская карта";
        rows.push_back({TgApi::button(label, "paym:" + m + ":" + std::to_string(amount))});
    }
    if (rows.empty()) {
        safeSend(uid, note + "Других способов оплаты сейчас нет. Попробуйте позже или напишите в поддержку.");
        return;
    }
    safeSend(uid, note + "Сумма: <b>" + money(amount) + "</b>\nКак удобнее оплатить?", TgApi::inlineKeyboard(rows));
}

// Запрос email/телефона для чека. pendingTopup > 0 — после ввода сразу продолжить пополнение на эту сумму.
void Bot::askReceiptContact(int64_t uid, int64_t pendingTopup, const std::string& pendingMethod) {
    Session s;
    s.step = Step::ReceiptContact;
    s.pendingTopup = pendingTopup;
    s.pendingMethod = pendingMethod;
    sessions_[uid] = s;
    json kb = {{"keyboard", {{{{"text", "📱 Отправить мой телефон"}, {"request_contact", true}}}, {{{"text", "/cancel"}}}}},
               {"resize_keyboard", true},
               {"one_time_keyboard", true}};
    safeSend(uid,
             "🧾 По закону (54-ФЗ) на каждую оплату выдаётся электронный чек. Куда его присылать?\n\n"
             "Напишите <b>email</b> или <b>номер телефона</b>, либо нажмите кнопку ниже. "
             "Контакт сохранится, изменить его можно командой /contact.",
             kb);
}

void Bot::saveReceiptContact(int64_t uid, const std::string& raw) {
    std::string contact;
    if (!util::normalizeReceiptContact(raw, contact)) {
        safeSend(uid, "Не похоже на email или российский номер телефона. Пример: <code>name@mail.ru</code> или "
                      "<code>+7 900 123-45-67</code>.");
        return;
    }
    int64_t pending = 0;
    std::string method;
    if (auto it = sessions_.find(uid); it != sessions_.end()) {
        pending = it->second.pendingTopup;
        method = it->second.pendingMethod;
    }
    sessions_.erase(uid);
    db_.setReceiptContact(uid, contact);
    sendMenu(uid, "✅ Чеки будут приходить на <code>" + htmlEscape(contact) + "</code>.");
    if (pending > 0) createTopup(uid, pending, method);
}

void Bot::showHelp(int64_t uid) {
    std::string t =
        "<b>Как это работает</b>\n"
        "• <b>Пополнить</b> — оплата " + payMethodsText() + ", деньги зачисляются на внутренний баланс автоматически.\n"
        "• <b>Заказать ботов</b> — укажите Twitch-канал, число ботов (" +
        std::to_string(cfg_.minBots) + "–" + std::to_string(cfg_.maxBots) + ") и время (" +
        std::to_string(cfg_.minHours) + "–" + std::to_string(cfg_.maxHours) +
        " ч). Стоимость списывается с баланса.\n"
        "• <b>Мои заказы</b> — статусы заказов.\n"
        "  – <b>Отмена</b>: боты сразу уходят из чата. " +
        std::string(cfg_.refundOnCancel ? "Возвращается стоимость неиспользованного времени (заказ в очереди — вся сумма).\n"
                                        : "⚠️ Деньги при отмене заказчиком не возвращаются.\n") +
        "  – <b>Продление</b> идущего заказа — кнопка «⏩ Продлить»; за " + std::to_string(cfg_.remindBeforeMin) +
        " мин до конца бот сам напомнит.\n\n"
        "<b>Тарифы</b> (за 1 бота в час):\n" + tariffsText() + "Подробно о тарифах — /tariffs\n\n"
        "Если заказ прерван по нашей вине — неиспользованное время возвращается на баланс.\n\n"
        "Команды: /order — новый заказ, /orders — мои заказы, /cancel — отменить заказ, "
        "/balance, /topup" + std::string(cfg_.receiptEnabled ? ", /contact — контакт для чеков" : "");
    if (!cfg_.supportContact.empty()) t += "\n\nПоддержка: " + htmlEscape(cfg_.supportContact);
    sendMenu(uid, t);
}

std::string Bot::statusLabel(const std::string& st) {
    if (st == "queued") return "⏳ В очереди";
    if (st == "running") return "▶️ Выполняется";
    if (st == "completed") return "✅ Завершён";
    if (st == "cancelled") return "🚫 Отменён";
    if (st == "failed") return "❌ Не выполнен";
    return st;
}

std::string Bot::tierName(const std::string& tierId) const {
    if (auto t = cfg_.findTier(tierId)) return t->name;
    return tierId.empty() ? "🤖 Бот" : tierId;
}

std::string Bot::tariffsText() const {
    std::string t;
    for (auto& tier : cfg_.tiers) {
        t += tier.name + " — <b>" + money(tier.pricePerBotHour) + "</b>";
        if (!tier.description.empty()) t += "\n   " + htmlEscape(tier.description);
        t += "\n";
    }
    return t;
}

std::string Bot::stepLabel(int step) const {
    int total = cfg_.tiers.size() > 1 ? 4 : 3;
    if (total == 3 && step > 2) --step;  // без шага выбора тарифа нумерация сдвигается
    return "Шаг " + std::to_string(step) + "/" + std::to_string(total);
}

std::string Bot::orderCard(const Order& o) const {
    std::ostringstream ss;
    ss << "<b>Заказ #" << o.id << "</b> — " << statusLabel(o.status) << "\n"
       << "Канал: <code>" << htmlEscape(o.channel) << "</code>\n"
       << "Тип: " << tierName(o.tier) << "\n"
       << o.bots << " " << botsWord(o.bots) << " × " << o.hours << " " << hoursWord(o.hours) << " = "
       << money(o.price) << "\n";
    if (o.status == "running") {
        if (o.extStatus == "paused")
            ss << "⏸ Боты на паузе (kill switch в чате)\n";
        else if (!o.extStatus.empty())
            ss << "В чате: " << o.botsActive << " из " << o.bots << "\n";
        ss << "До: " << util::fmtTime(o.endsAt) << "\n";
    }
    if (o.status == "completed") ss << "Завершён: " << util::fmtTime(o.endsAt) << "\n";
    if (!o.note.empty()) ss << "Комментарий: " << htmlEscape(o.note) << "\n";
    return ss.str();
}

void Bot::showOrders(int64_t uid) {
    auto orders = db_.userOrders(uid, 10);
    if (orders.empty()) {
        sendMenu(uid, "У вас пока нет заказов. Нажмите «" + kBtnOrder + "».");
        return;
    }
    std::string t = "📦 <b>Последние заказы</b>\n\n";
    std::vector<std::vector<json>> rows;
    for (auto& o : orders) {
        t += orderCard(o) + "\n";
        if (o.status == "queued")
            rows.push_back({TgApi::button("🚫 Отменить #" + std::to_string(o.id), "ocancel:" + std::to_string(o.id))});
        if (o.status == "running")
            rows.push_back({TgApi::button("⏩ Продлить #" + std::to_string(o.id), "ext:" + std::to_string(o.id)),
                            TgApi::button("🚫 Отменить #" + std::to_string(o.id), "ocancel:" + std::to_string(o.id))});
    }
    safeSend(uid, t, rows.empty() ? json(nullptr) : TgApi::inlineKeyboard(rows));
}

// ============================================================================
// Пополнение баланса (ЮKassa: СБП / карта)
// ============================================================================

void Bot::askTopup(int64_t uid) {
    std::vector<std::vector<json>> rows;
    std::vector<json> row;
    for (int rub : cfg_.topupPresets) {
        row.push_back(TgApi::button(std::to_string(rub) + " ₽", "topup:" + std::to_string(rub)));
        if (row.size() == 3) rows.push_back(std::move(row)), row.clear();
    }
    if (!row.empty()) rows.push_back(row);
    rows.push_back({TgApi::button("✏️ Другая сумма", "topup:custom")});
    std::string how = payMethodsText();
    safeSend(uid, "Выберите сумму пополнения" + (how.empty() ? "" : " (оплата " + how + ")") + ":",
             TgApi::inlineKeyboard(rows));
}

void Bot::createTopup(int64_t uid, int64_t amount, const std::string& requested) {
    if (amount < cfg_.minTopup || amount > cfg_.maxTopup) {
        safeSend(uid, "Сумма должна быть от " + money(cfg_.minTopup) + " до " + money(cfg_.maxTopup) + ".");
        return;
    }
    // Способ оплаты: спросить (если их несколько) или взять единственный
    std::string method = requested;
    const auto& methods = cfg_.paymentMethods;
    if (method.empty()) {
        if (methods.size() > 1) {
            askPayMethod(uid, amount);
            return;
        }
        if (methods.size() == 1) method = methods[0];
    } else if (std::find(methods.begin(), methods.end(), method) == methods.end()) {
        return;  // способ не из списка (старая кнопка или подделанный callback)
    }

    std::string contact;
    if (cfg_.receiptEnabled) {
        auto u = db_.getUser(uid);
        if (!u || u->receiptContact.empty()) {
            askReceiptContact(uid, amount, method);  // после ввода контакта пополнение продолжится само
            return;
        }
        contact = u->receiptContact;
    }
    int64_t pid = db_.createPayment(uid, pay_.name(), amount);
    PaymentInit init;
    try {
        PaymentRequest req;
        req.localId = pid;
        req.userId = uid;
        req.amount = amount;
        req.description = "Пополнение баланса, платёж #" + std::to_string(pid);
        req.customerContact = contact;
        req.method = method;
        init = pay_.create(req);
    } catch (const PaymentMethodUnavailable& e) {
        db_.setPaymentError(pid);
        util::log("Создание платежа #" + std::to_string(pid) + " (" + method + "): " + e.what());
        askPayMethod(uid, amount, "⚠️ Оплата " + payMethodLabel(method) + " сейчас недоступна.\n\n", method);
        return;
    } catch (const std::exception& e) {
        db_.setPaymentError(pid);
        util::log("Создание платежа #" + std::to_string(pid) + ": " + e.what());
        safeSend(uid, "⚠️ Не удалось создать платёж. Попробуйте позже.");
        return;
    }
    db_.setPaymentPending(pid, init.providerId, init.url);
    util::log("Платёж #" + std::to_string(pid) + " создан: user " + std::to_string(uid) + ", " + money(amount) +
              (method.empty() ? "" : ", " + method));

    std::string payBtn = method == "sbp" ? "⚡ Оплатить через СБП" : method == "bank_card" ? "💳 Оплатить картой"
                                                                                          : "💳 Оплатить";
    std::string hint = method == "sbp" ? "Нажмите «Оплатить через СБП», выберите свой банк и подтвердите платёж в его приложении."
                       : method == "bank_card" ? "Нажмите «Оплатить картой» и введите данные карты на защищённой странице ЮKassa."
                                               : "Нажмите «Оплатить» и выберите удобный способ на странице ЮKassa.";
    safeSend(uid,
             "🧾 <b>Счёт #" + std::to_string(pid) + "</b> на " + money(amount) + "\n\n" + hint +
                 " Деньги зачислятся автоматически, обычно в течение минуты.",
             TgApi::inlineKeyboard({{TgApi::urlButton(payBtn, init.url)},
                                    {TgApi::button("🔄 Проверить оплату", "paycheck:" + std::to_string(pid))}}));
}

void Bot::checkPaymentByUser(int64_t uid, int64_t paymentId, const std::string& cbId) {
    auto p = db_.getPayment(paymentId);
    if (!p || p->userId != uid) return tg_.answerCallback(cbId, "Платёж не найден", true);
    if (p->status == "succeeded") return tg_.answerCallback(cbId, "✅ Платёж уже зачислен");
    if (p->status != "pending") return tg_.answerCallback(cbId, "Счёт неактуален, создайте новый", true);
    if (p->provider != pay_.name()) {  // счёт создан другим провайдером (например, в mock-режиме)
        db_.closePayment(p->id, "expired");
        return tg_.answerCallback(cbId, "Счёт устарел — сменился способ оплаты. Создайте новый.", true);
    }

    PayStatus st = pay_.check(p->providerId);
    if (st == PayStatus::Pending || st == PayStatus::Unknown) {
        tg_.answerCallback(cbId, "Оплата ещё не поступила. Если вы уже оплатили — подождите минуту.", true);
        return;
    }
    tg_.answerCallback(cbId);
    applyPaymentStatus(*p, st);
}

// Применяет статус от провайдера. Безопасно вызывать из обоих потоков:
// переход статуса в БД атомарный, уведомление шлёт только тот, кто его выполнил.
void Bot::applyPaymentStatus(const Payment& p, PayStatus st) {
    if (st == PayStatus::Succeeded) {
        if (db_.completePayment(p.id)) {
            auto u = db_.getUser(p.userId);
            util::log("Платёж #" + std::to_string(p.id) + " зачислен: " + money(p.amount));
            safeSend(p.userId, "✅ Оплата получена! На баланс зачислено <b>" + money(p.amount) +
                                   "</b>.\nТекущий баланс: <b>" + money(u ? u->balance : 0) + "</b>",
                     TgApi::inlineKeyboard({{TgApi::button("🛒 Заказать ботов", "goto:order")}}));
        }
    } else if (st == PayStatus::Canceled) {
        if (db_.closePayment(p.id, "canceled"))
            safeSend(p.userId, "❌ Платёж #" + std::to_string(p.id) + " на " + money(p.amount) +
                                   " отменён или отклонён банком. Попробуйте ещё раз: /topup");
    }
}

void Bot::pollPayments() {
    int64_t now = util::now();
    for (auto& p : db_.pendingPayments()) {
        if (p.provider != pay_.name()) {
            // Счёт создан другим провайдером (например, в mock-режиме до переключения на ЮKassa):
            // текущий провайдер о нём не знает. Закрываем; если деньги всё же были — админ зачислит через /paid.
            if (db_.closePayment(p.id, "expired")) {
                util::log("Платёж #" + std::to_string(p.id) + " (" + p.provider + ") закрыт: текущий провайдер " +
                          pay_.name());
                safeSend(p.userId, "⌛ Счёт #" + std::to_string(p.id) + " на " + money(p.amount) +
                                       " устарел — сменился способ оплаты. Создайте новый: /topup");
            }
            continue;
        }
        PayStatus st = pay_.check(p.providerId);
        if (st == PayStatus::Succeeded || st == PayStatus::Canceled) {
            applyPaymentStatus(p, st);
        } else if (now - p.createdAt > cfg_.paymentExpireMin * 60) {
            // Перестаём опрашивать. Если оплата всё же пришла позже — админ подтвердит её через /paid.
            if (db_.closePayment(p.id, "expired"))
                safeSend(p.userId, "⌛ Счёт #" + std::to_string(p.id) + " на " + money(p.amount) +
                                       " истёк. Если вы успели оплатить — напишите в поддержку.");
        }
    }
}

// ============================================================================
// Оформление заказа
// ============================================================================

void Bot::startOrderFlow(int64_t uid, const std::string& presetTier) {
    Session s;
    s.step = Step::OrderChannel;
    if (cfg_.findTier(presetTier)) s.tier = presetTier;
    sessions_[uid] = s;
    std::string t = "🛒 <b>Новый заказ</b>";
    if (!s.tier.empty()) t += " — " + tierName(s.tier);
    safeSend(uid, t + "\n\n" + stepLabel(1) + ". Пришлите название Twitch-канала или ссылку на него.\n"
                      "/cancel — отменить.");
}

void Bot::askTier(int64_t uid) {
    std::vector<std::vector<json>> rows;
    for (auto& t : cfg_.tiers) {
        std::vector<json> row{TgApi::button(t.name + " — " + money(t.pricePerBotHour) + "/ч", "tier:" + t.id)};
        if (!t.details.empty()) row.push_back(TgApi::button("ℹ️ Подробнее", "tierinfo:" + t.id));
        rows.push_back(row);
    }
    safeSend(uid,
             "Канал: <code>" + htmlEscape(sessions_[uid].channel) + "</code>\n\n" + stepLabel(2) +
                 ". Какие боты нужны? Цена — за 1 бота в час.\n\n" + tariffsText(),
             TgApi::inlineKeyboard(rows));
}

void Bot::showTariffs(int64_t uid) {
    std::vector<std::vector<json>> rows;
    for (auto& t : cfg_.tiers) {
        if (!t.details.empty()) rows.push_back({TgApi::button("ℹ️ Подробнее: " + t.name, "tierinfo:" + t.id)});
    }
    safeSend(uid, "<b>Тарифы</b> (за 1 бота в час):\n" + tariffsText(),
             rows.empty() ? json(nullptr) : TgApi::inlineKeyboard(rows));
}

void Bot::showTierDetails(int64_t uid, const std::string& tierId) {
    const Tier* t = cfg_.findTier(tierId);
    if (!t) return;
    std::string text = t->details.empty() ? t->name + "\n" + htmlEscape(t->description) : t->details;
    text += "\n\n💰 <b>" + money(t->pricePerBotHour) + "</b> за 1 бота в час.";
    auto it = sessions_.find(uid);
    bool choosing = it != sessions_.end() && it->second.step == Step::OrderTier;
    std::vector<std::vector<json>> rows{
        {TgApi::button(choosing ? "✅ Выбрать: " + t->name : "🛒 Заказать: " + t->name, "tierorder:" + t->id)}};
    for (auto& other : cfg_.tiers)
        if (other.id != t->id && !other.details.empty())
            rows.push_back({TgApi::button("ℹ️ А что умеет " + other.name + "?", "tierinfo:" + other.id)});
    safeSend(uid, text, TgApi::inlineKeyboard(rows));
}

void Bot::askBots(int64_t uid) {
    std::vector<std::vector<json>> rows;
    std::vector<json> row;
    for (int n : cfg_.botPresets) {
        if (n < cfg_.minBots || n > cfg_.maxBots) continue;
        row.push_back(TgApi::button(std::to_string(n), "bots:" + std::to_string(n)));
        if (row.size() == 5) rows.push_back(std::move(row)), row.clear();
    }
    if (!row.empty()) rows.push_back(row);
    const Session& s = sessions_[uid];

    // Сколько ботов свободно на сервере прямо сейчас (только для автоматического исполнителя)
    std::string avail;
    if (exec_.automatic()) {
        auto cap = exec_.capacity();
        if (cap.free > 0) {
            avail = "\n\n🟢 Сейчас на сервере свободно <b>" + std::to_string(cap.free) + "</b>" +
                    (cap.total > 0 ? " из " + std::to_string(cap.total) : "") + " " + botsWord(cap.free) +
                    ". Можно заказать и больше — заказ стартует со свободными, остальные подключатся, "
                    "как только освободятся, а за время без них вернём деньги.";
        } else if (cap.free == 0) {
            avail = "\n\n🟡 Сейчас все боты заняты. Заказ встанет в очередь и запустится, как только кто-то "
                    "освободится; если за " + std::to_string(cfg_.autoStartTimeoutMin) +
                    " мин никто не освободится — вернём деньги полностью.";
        }
    }
    safeSend(uid,
             "Канал: <code>" + htmlEscape(s.channel) + "</code>\nТип: " + tierName(s.tier) + "\n\n" + stepLabel(3) +
                 ". Сколько ботов нужно? Выберите или введите число (" + std::to_string(cfg_.minBots) + "–" +
                 std::to_string(cfg_.maxBots) + "):" + avail,
             rows.empty() ? json(nullptr) : TgApi::inlineKeyboard(rows));
}

void Bot::askHours(int64_t uid) {
    std::vector<std::vector<json>> rows;
    std::vector<json> row;
    for (int h : cfg_.hourPresets) {
        if (h < cfg_.minHours || h > cfg_.maxHours) continue;
        row.push_back(TgApi::button(std::to_string(h) + " ч", "hours:" + std::to_string(h)));
        if (row.size() == 3) rows.push_back(std::move(row)), row.clear();
    }
    if (!row.empty()) rows.push_back(row);
    int bots = sessions_[uid].bots;
    safeSend(uid,
             "Ботов: <b>" + std::to_string(bots) + "</b>\n\n" + stepLabel(4) +
                 ". На сколько часов? Выберите или введите число (" + std::to_string(cfg_.minHours) + "–" +
                 std::to_string(cfg_.maxHours) + "):",
             rows.empty() ? json(nullptr) : TgApi::inlineKeyboard(rows));
}

void Bot::showOrderSummary(int64_t uid) {
    Session& s = sessions_[uid];
    const Tier* tier = cfg_.findTier(s.tier);
    if (!tier || s.bots < cfg_.minBots || s.bots > cfg_.maxBots || s.hours < cfg_.minHours ||
        s.hours > cfg_.maxHours) {
        sessions_.erase(uid);
        safeSend(uid, "Некорректные параметры заказа, начните заново: /order");
        return;
    }
    int64_t price = tier->pricePerBotHour * s.bots * s.hours;
    auto u = db_.getUser(uid);
    int64_t bal = u ? u->balance : 0;

    std::string t = "📝 <b>Проверьте заказ</b>\n\n"
                    "Канал: <code>" + htmlEscape(s.channel) + "</code>\n"
                    "Тип: " + tier->name + " (" + money(tier->pricePerBotHour) + " за бота в час)\n"
                    "Ботов: <b>" + std::to_string(s.bots) + "</b>\n"
                    "Время: <b>" + std::to_string(s.hours) + " " + hoursWord(s.hours) + "</b>\n"
                    "Стоимость: <b>" + money(price) + "</b>\n"
                    "Ваш баланс: " + money(bal);
    if (exec_.automatic()) {
        auto cap = exec_.capacity();
        if (cap.free >= 0 && cap.free < s.bots)
            t += "\n\nℹ️ Сейчас свободно " + std::to_string(cap.free) + " " + botsWord(cap.free) + " из " +
                 std::to_string(s.bots) + " заказанных: " +
                 (cap.free > 0 ? "заказ стартует с ними, остальные подключатся по мере освобождения"
                               : "заказ запустится, как только боты освободятся") +
                 ". За время без ботов вернём деньги.";
    }
    std::vector<std::vector<json>> rows;
    if (bal >= price) {
        rows.push_back({TgApi::button("✅ Оплатить с баланса", "order:confirm")});
    } else {
        t += "\n\n⚠️ Не хватает <b>" + money(price - bal) + "</b>. Пополните баланс — черновик заказа сохранится.";
        rows.push_back({TgApi::button("➕ Пополнить", "goto:topup")});
        rows.push_back({TgApi::button("✅ Я пополнил, оформить", "order:confirm")});
    }
    rows.push_back({TgApi::button("✖️ Отмена", "order:abort")});
    safeSend(uid, t, TgApi::inlineKeyboard(rows));
}

void Bot::confirmOrder(int64_t uid) {
    Session s = sessions_[uid];
    const Tier* tier = cfg_.findTier(s.tier);
    if (!tier) {
        sessions_.erase(uid);
        safeSend(uid, "Тариф больше недоступен, начните заново: /order");
        return;
    }
    int64_t price = tier->pricePerBotHour * s.bots * s.hours;
    auto oid = db_.createOrder(uid, s.channel, tier->id, tier->pricePerBotHour, s.bots, s.hours);
    if (!oid) {
        auto u = db_.getUser(uid);
        safeSend(uid, "Недостаточно средств: нужно " + money(price) + ", на балансе " + money(u ? u->balance : 0) + ".",
                 TgApi::inlineKeyboard({{TgApi::button("➕ Пополнить", "goto:topup")}}));
        return;
    }
    sessions_.erase(uid);
    auto o = db_.getOrder(*oid);
    util::log("Новый заказ #" + std::to_string(*oid) + ": " + s.channel + ", " + tier->id + " " + std::to_string(s.bots) + "×" +
              std::to_string(s.hours) + "ч, " + money(price));

    std::string t = "🎉 Заказ принят!\n\n" + orderCard(*o) +
                    (exec_.automatic() ? "\nЗапуск произойдёт автоматически в ближайшие минуты."
                                       : "\nЗаказ передан в работу, мы сообщим о запуске.");
    sendMenu(uid, t);
    if (!exec_.automatic()) notifyAdminsNewOrder(*o);
}

// /cancel вне диалога: активные заказы с кнопками отмены.
void Bot::showCancelableOrders(int64_t uid) {
    std::string t;
    std::vector<std::vector<json>> rows;
    for (auto& o : db_.userOrders(uid, 20)) {
        if (o.status != "queued" && o.status != "running") continue;
        t += orderCard(o) + "\n";
        rows.push_back({TgApi::button("🚫 Отменить заказ #" + std::to_string(o.id), "ocancel:" + std::to_string(o.id))});
    }
    if (rows.empty()) {
        sendMenu(uid, "У вас нет активных заказов — отменять нечего.");
        return;
    }
    safeSend(uid, "Какой заказ отменить?\n\n" + t, TgApi::inlineKeyboard(rows));
}

// Шаг 1: подтверждение с предварительной суммой возврата.
void Bot::askCancelOrder(int64_t uid, int64_t orderId, const std::string& cbId) {
    auto o = db_.getOrder(orderId);
    if (!o || o->userId != uid) return tg_.answerCallback(cbId, "Заказ не найден", true);
    if (o->status != "queued" && o->status != "running")
        return tg_.answerCallback(cbId, "Заказ уже закрыт: " + statusLabel(o->status), true);
    tg_.answerCallback(cbId);

    std::string id = std::to_string(orderId);
    std::string t = "🚫 <b>Отменить заказ #" + id + "?</b>\nКанал: <code>" + htmlEscape(o->channel) + "</code>\n\n";
    if (o->status == "running") t += "Боты сразу покинут чат.\n";
    if (!cfg_.refundOnCancel) {
        t += "⚠️ <b>Деньги при отмене не возвращаются</b> (" + money(o->price) + ").";
    } else if (o->status == "queued") {
        t += "Заказ ещё не запущен — вернём всю сумму: <b>" + money(o->price) + "</b>.";
    } else {
        t += "Вернём стоимость неиспользованного времени";
        if (o->missingBotSec > 0) t += " и компенсацию за недоданных ботов";
        t += ": сейчас это <b>" + money(Db::refundFor(*o, util::now())) + "</b> (точная сумма — в момент отмены).";
    }
    safeSend(uid, t,
             TgApi::inlineKeyboard({{TgApi::button("✅ Да, отменить", "ocancelok:" + id),
                                     TgApi::button("↩️ Нет", "ocancelno")}}));
}

// Шаг 2: отмена. Сначала фиксируем отмену и возврат в БД (атомарно), потом снимаем ботов у исполнителя.
void Bot::cancelOrderByUser(int64_t uid, int64_t orderId, const std::string& cbId) {
    auto o = db_.getOrder(orderId);
    if (!o || o->userId != uid) return tg_.answerCallback(cbId, "Заказ не найден", true);
    if (o->status != "queued" && o->status != "running")
        return tg_.answerCallback(cbId, "Заказ уже закрыт: " + statusLabel(o->status), true);
    bool wasRunning = o->status == "running";

    auto refund = db_.cancelOrder(orderId, "cancelled", "отменён заказчиком", cfg_.refundOnCancel);
    if (!refund) return tg_.answerCallback(cbId, "Не удалось отменить — статус заказа уже изменился", true);
    tg_.answerCallback(cbId, "Заказ отменён");
    forgetExecPoll(orderId);
    if (wasRunning && exec_.automatic()) exec_.stop(*o, "отменён заказчиком");

    std::string id = std::to_string(orderId);
    util::log("Заказ #" + id + " отменён заказчиком, возврат " + money(*refund));
    safeSend(uid, "🚫 Заказ #" + id + " отменён." + (wasRunning ? " Боты покидают чат." : "") +
                      (*refund > 0 ? "\nНа баланс возвращено <b>" + money(*refund) + "</b>." : ""));
    if (wasRunning && !exec_.automatic())
        notifyAdmins("🚫 Заказчик отменил идущий заказ #" + id + " (" + htmlEscape(o->channel) + ") — отключите ботов.");
    else
        notifyAdmins("ℹ️ Заказчик отменил заказ #" + id + (*refund > 0 ? ", возврат " + money(*refund) : ", без возврата"));
}

void Bot::forgetExecPoll(int64_t orderId) {
    std::lock_guard lk(execPollMu_);
    lastExecPoll_.erase(orderId);
}

// ============================================================================
// Продление заказа
// ============================================================================

void Bot::askExtendHours(int64_t uid, int64_t orderId, const std::string& cbId) {
    auto o = db_.getOrder(orderId);
    if (!o || o->userId != uid) return tg_.answerCallback(cbId, "Заказ не найден", true);
    if (o->status != "running" || o->endsAt <= util::now() + 60)
        return tg_.answerCallback(cbId, "Продлить можно только идущий заказ", true);
    tg_.answerCallback(cbId);

    std::vector<std::vector<json>> rows;
    std::vector<json> row;
    for (int h : cfg_.hourPresets) {
        if (h < cfg_.minHours || h > cfg_.maxHours) continue;
        row.push_back(TgApi::button("+" + std::to_string(h) + " ч", "exth:" + std::to_string(orderId) + ":" +
                                                                     std::to_string(h)));
        if (row.size() == 3) rows.push_back(std::move(row)), row.clear();
    }
    if (!row.empty()) rows.push_back(row);
    safeSend(uid,
             "⏩ <b>Продление заказа #" + std::to_string(orderId) + "</b>\nКанал: <code>" + htmlEscape(o->channel) +
                 "</code>, " + std::to_string(o->bots) + " " + botsWord(o->bots) + "\nСейчас до: " +
                 util::fmtTime(o->endsAt) + "\n\nНа сколько продлить? " + money(o->unitPrice * o->bots) +
                 " за каждый час.",
             TgApi::inlineKeyboard(rows));
}

void Bot::showExtendSummary(int64_t uid, int64_t orderId, int hours, const std::string& cbId) {
    if (hours < cfg_.minHours || hours > cfg_.maxHours) return tg_.answerCallback(cbId);
    auto o = db_.getOrder(orderId);
    if (!o || o->userId != uid) return tg_.answerCallback(cbId, "Заказ не найден", true);
    if (o->status != "running" || o->endsAt <= util::now() + 60)
        return tg_.answerCallback(cbId, "Продлить можно только идущий заказ", true);
    tg_.answerCallback(cbId);

    int64_t price = o->unitPrice * o->bots * hours;
    auto u = db_.getUser(uid);
    int64_t bal = u ? u->balance : 0;
    std::string id = std::to_string(orderId), h = std::to_string(hours);
    std::string t = "⏩ Продлить заказ #" + id + " на <b>" + h + " " + hoursWord(hours) + "</b>\n" +
                    "Новое окончание: " + util::fmtTime(o->endsAt + static_cast<int64_t>(hours) * 3600) + "\n" +
                    "Стоимость: <b>" + money(price) + "</b>\nВаш баланс: " + money(bal);
    std::vector<std::vector<json>> rows;
    if (bal < price) {
        t += "\n\n⚠️ Не хватает <b>" + money(price - bal) + "</b>.";
        rows.push_back({TgApi::button("➕ Пополнить", "goto:topup")});
        rows.push_back({TgApi::button("✅ Я пополнил, продлить", "extok:" + id + ":" + h)});
    } else {
        rows.push_back({TgApi::button("✅ Продлить с баланса", "extok:" + id + ":" + h)});
    }
    safeSend(uid, t, TgApi::inlineKeyboard(rows));
}

void Bot::extendOrderByUser(int64_t uid, int64_t orderId, int hours, const std::string& cbId) {
    if (hours < cfg_.minHours || hours > cfg_.maxHours) return tg_.answerCallback(cbId);
    auto o = db_.getOrder(orderId);
    if (!o || o->userId != uid) return tg_.answerCallback(cbId, "Заказ не найден", true);
    int64_t price = o->unitPrice * o->bots * hours;

    switch (db_.extendOrder(orderId, uid, hours, price)) {
        case ExtendResult::NotRunning:
            return tg_.answerCallback(cbId, "Заказ уже завершён или заканчивается — продлить нельзя", true);
        case ExtendResult::NoFunds: {
            tg_.answerCallback(cbId);
            auto u = db_.getUser(uid);
            safeSend(uid, "Недостаточно средств: нужно " + money(price) + ", на балансе " + money(u ? u->balance : 0) + ".",
                     TgApi::inlineKeyboard({{TgApi::button("➕ Пополнить", "goto:topup")}}));
            return;
        }
        case ExtendResult::Ok:
            break;
    }
    tg_.answerCallback(cbId, "Продлеваем…");

    // Деньги уже списаны и срок сдвинут в БД; теперь — сервер. Отказал — откатываем.
    ExecState st = exec_.extend(*o, hours);
    std::string id = std::to_string(orderId);
    if (!st.ok) {
        db_.revertExtension(orderId, hours, price);
        util::log("Продление заказа #" + id + " отклонено исполнителем: " + st.error);
        safeSend(uid, "⚠️ Не удалось продлить заказ #" + id + " — сервер ботов отказал" +
                          (st.httpCode == 409 ? " (заказ уже завершился)" : "") + ". Деньги возвращены на баланс.");
        notifyAdmins("⚠️ Продление заказа #" + id + " не прошло: " + htmlEscape(st.error));
        return;
    }

    auto fresh = db_.getOrder(orderId);
    int64_t ends = fresh ? fresh->endsAt : o->endsAt + static_cast<int64_t>(hours) * 3600;
    if (exec_.automatic() && st.endsAt > 0 && std::llabs(st.endsAt - ends) > 120)
        util::log("Заказ #" + id + ": время окончания у сервера (" + util::fmtTime(st.endsAt) + ") и в боте (" +
                  util::fmtTime(ends) + ") расходится");
    util::log("Заказ #" + id + " продлён на " + std::to_string(hours) + " ч, " + money(price));
    safeSend(uid, "⏩ <b>Заказ #" + id + " продлён на " + std::to_string(hours) + " " + hoursWord(hours) +
                      "</b>\nСписано: " + money(price) + "\nНовое окончание: " + util::fmtTime(ends));
    if (!exec_.automatic())
        notifyAdmins("⏩ Заказ #" + id + " (" + htmlEscape(o->channel) + ") продлён на " + std::to_string(hours) +
                     " ч, до " + util::fmtTime(ends) + " — не отключайте ботов.");
}

// ============================================================================
// Жизненный цикл заказа
// ============================================================================

bool Bot::launchOrder(const Order& o, std::string& err) {
    int64_t start = util::now();
    int64_t ends = start + static_cast<int64_t>(o.hours) * 3600;
    ExecState st = exec_.start(o, ends);
    if (!st.ok) {
        err = st.error;
        return false;
    }
    if (st.status == "failed" || st.status == "completed" || st.status == "stopped") {
        err = "исполнитель вернул статус " + st.status + (st.reason.empty() ? "" : ": " + st.reason);
        return false;
    }
    if (st.endsAt > start) ends = st.endsAt;  // при повторном start исполнитель вернёт уже назначенное время
    if (!db_.startOrder(o.id, start, ends, st.assigned, exec_.automatic() ? st.status : "")) {
        err = "статус заказа изменился";
        exec_.stop(o, err);
        return false;
    }
    forgetExecPoll(o.id);
    util::log("Заказ #" + std::to_string(o.id) + " запущен, ботов " + std::to_string(st.assigned) + "/" +
              std::to_string(o.bots));

    std::string t = "▶️ <b>Заказ #" + std::to_string(o.id) + " запущен!</b>\nКанал: <code>" +
                    htmlEscape(o.channel) + "</code>\n";
    if (st.assigned >= o.bots) {
        t += "В чате: " + std::to_string(o.bots) + " " + botsWord(o.bots) + "\n";
    } else {
        t += "В чате сейчас: <b>" + std::to_string(st.assigned) + " из " + std::to_string(o.bots) +
             "</b>. Остальных добавим, как только освободятся, а за недоданное время вернём деньги на баланс.\n";
    }
    t += "Окончание: " + util::fmtTime(ends);
    std::string id = std::to_string(o.id);
    safeSend(o.userId, t,
             TgApi::inlineKeyboard({{TgApi::button("⏩ Продлить", "ext:" + id),
                                     TgApi::button("🚫 Отменить", "ocancel:" + id)}}));
    return true;
}

bool Bot::failOrder(const Order& o, const std::string& status, const std::string& reason) {
    // Сначала закрываем в БД (атомарно): если заказ уже закрыт (например, отменён заказчиком), ничего не делаем.
    auto refund = db_.cancelOrder(o.id, status, reason);
    if (!refund) return false;
    forgetExecPoll(o.id);
    if (o.status == "running" && exec_.automatic()) exec_.stop(o, reason);
    util::log("Заказ #" + std::to_string(o.id) + " -> " + status + " (" + reason + "), возврат " + money(*refund));
    safeSend(o.userId, "❌ Заказ #" + std::to_string(o.id) + " не выполнен: " + htmlEscape(reason) +
                           ".\nНа баланс возвращено <b>" + money(*refund) + "</b>.");
    return true;
}

void Bot::notifyAdminsNewOrder(const Order& o) {
    auto u = db_.getUser(o.userId);
    std::string who = u && !u->username.empty() ? "@" + htmlEscape(u->username) : std::to_string(o.userId);
    notifyAdmins("🆕 Новый заказ от " + who + "\n\n" + orderCard(o),
                 TgApi::inlineKeyboard({{TgApi::button("▶️ Запустить", "adm_start:" + std::to_string(o.id)),
                                         TgApi::button("❌ Отклонить", "adm_reject:" + std::to_string(o.id))}}));
}

void Bot::processOrders() {
    int64_t now = util::now();

    if (exec_.automatic()) {
        auto queued = db_.ordersByStatus("queued");
        // Не запускаем заказ, если у исполнителя нет ни одного свободного бота: иначе время пойдёт впустую.
        int free = queued.empty() ? -1 : exec_.freeBots();
        for (auto& o : queued) {
            std::string err;
            if (free == 0) {
                err = "нет свободных ботов";
            } else if (launchOrder(o, err)) {
                if (free > 0) free = std::max(0, free - o.bots);
                continue;
            }
            util::log("Автозапуск заказа #" + std::to_string(o.id) + " отложен: " + err);
            if (now - o.createdAt > cfg_.autoStartTimeoutMin * 60 &&
                failOrder(o, "failed", free == 0 ? "сейчас нет свободных ботов" : "не удалось запустить ботов"))
                notifyAdmins("⚠️ Заказ #" + std::to_string(o.id) + " не запустился: " + htmlEscape(err));
        }
    }

    for (auto& o : db_.ordersByStatus("running")) {
        if (o.endsAt > now) {
            // Напоминание о скором окончании с кнопкой продления
            if (cfg_.remindBeforeMin > 0 && !o.reminded && o.endsAt - now <= cfg_.remindBeforeMin * 60 &&
                o.endsAt - now > 60 && db_.markReminded(o.id)) {
                std::string id = std::to_string(o.id);
                safeSend(o.userId,
                         "⏰ Заказ #" + id + " (<code>" + htmlEscape(o.channel) + "</code>) закончится в " +
                             util::fmtTime(o.endsAt) + ". Хотите продлить, чтобы боты остались в чате?",
                         TgApi::inlineKeyboard({{TgApi::button("⏩ Продлить", "ext:" + id)}}));
            }
            continue;
        }
        if (exec_.automatic()) exec_.stop(o, "время заказа истекло");
        forgetExecPoll(o.id);
        if (auto comp = db_.finishOrder(o.id)) {
            util::log("Заказ #" + std::to_string(o.id) + " завершён, компенсация " + money(*comp));
            std::string t = "✅ <b>Заказ #" + std::to_string(o.id) + " завершён.</b>\nБоты покинули чат <code>" +
                            htmlEscape(o.channel) + "</code>.";
            if (*comp > 0)
                t += "\nЧасть времени ботов было меньше, чем заказано — на баланс возвращено <b>" + money(*comp) + "</b>.";
            t += "\nСпасибо, что пользуетесь сервисом!";
            safeSend(o.userId, t, TgApi::inlineKeyboard({{TgApi::button("🔁 Заказать ещё", "goto:order")}}));
            if (!exec_.automatic())
                notifyAdmins("⏹ Заказ #" + std::to_string(o.id) + " (" + htmlEscape(o.channel) +
                             ") закончился — отключите ботов.");
        }
    }
}

// Опрос исполнителя по запущенным заказам: учёт недоданных ботов, пауза, падение заказа.
void Bot::pollExecutor() {
    int64_t now = util::now();
    for (auto& listed : db_.ordersByStatus("running")) {
        ExecState st = exec_.query(listed);
        // Пока шёл запрос, заказ могли отменить или продлить в основном потоке — работаем с актуальной версией.
        auto cur = db_.getOrder(listed.id);
        if (!cur || cur->status != "running") continue;
        const Order& o = *cur;
        if (!st.ok && st.httpCode == 404) {
            // Заказ удалён на сервере (DELETE /api/orders/{id}) — ботов в чате больше нет.
            if (failOrder(o, "failed", "заказ снят на сервере ботов"))
                notifyAdmins("⚠️ Заказ #" + std::to_string(o.id) + " не найден на сервере ботов (удалён?) — "
                             "закрыт с возвратом остатка.");
            continue;
        }
        if (!st.ok) {
            util::log("Статус заказа #" + std::to_string(o.id) + " недоступен: " + st.error);
            continue;
        }

        // Недоданные бот-секунды с прошлого опроса. Пауза (kill switch) — решение стримера, её не компенсируем.
        int64_t dt = 0;
        {
            std::lock_guard lk(execPollMu_);
            int64_t& last = lastExecPoll_[o.id];
            if (last == 0) last = std::max(o.startedAt, now - cfg_.executorPollSec);
            dt = std::max<int64_t>(0, std::min(now, o.endsAt) - last);
            last = now;
        }
        bool counts = st.status == "running" || st.status == "waiting";
        int64_t missing = counts ? std::max(0, o.bots - st.assigned) : 0;
        db_.updateOrderProgress(o.id, st.status, st.assigned, missing * dt);

        if (st.status == "failed" || st.status == "completed" || st.status == "stopped") {
            if (st.status != "failed" && now >= o.endsAt - 60) continue;  // штатное окончание — закроет processOrders
            auto fresh = db_.getOrder(o.id);  // с учётом только что добавленных недоданных бот-секунд
            if (!fresh) continue;
            std::string reason = st.reason;
            if (reason.empty()) reason = st.status == "failed" ? "ошибка на сервере ботов" : "остановлен оператором";
            if (failOrder(*fresh, "failed", reason))
                notifyAdmins("⚠️ Заказ #" + std::to_string(o.id) + " закрыт исполнителем (" + st.status + "): " +
                             htmlEscape(reason));
            continue;
        }

        // Уведомления об изменениях
        std::string id = std::to_string(o.id);
        if (st.status != o.extStatus) {
            if (st.status == "paused") {
                safeSend(o.userId, "⏸ Заказ #" + id + ": боты поставлены на паузу командой в чате. "
                                   "Время заказа идёт; вернуть ботов можно командой возврата в чате.");
                continue;
            }
            if (st.status == "waiting") {
                safeSend(o.userId, "⚠️ Заказ #" + id + ": сейчас в чате нет ни одного бота, подбираем замену. "
                                   "За недоданное время вернём деньги на баланс.");
                continue;
            }
            if (st.status == "running" && o.extStatus == "paused") {
                safeSend(o.userId, "▶️ Заказ #" + id + ": боты снова в чате (" + std::to_string(st.assigned) +
                                       " из " + std::to_string(o.bots) + ").");
                continue;
            }
        }
        if (st.status == "running" && st.assigned != o.botsActive) {
            std::string t = "👥 Заказ #" + id + ": в чате сейчас <b>" + std::to_string(st.assigned) + " из " +
                            std::to_string(o.bots) + "</b> " + botsWord(o.bots) + ".";
            if (st.assigned < o.bots) t += " Недостающих добавим, как только освободятся; за недоданное время вернём деньги.";
            safeSend(o.userId, t);
        }
    }
}

// ============================================================================
// Админ-команды
// ============================================================================

bool Bot::onAdminCommand(int64_t uid, const std::string& cmd, const std::string& args) {
    std::istringstream in(args);
    std::string a1, a2;
    in >> a1 >> a2;
    std::string rest;
    std::getline(in, rest);
    rest = util::trim(rest);

    if (cmd == "/admin") {
        safeSend(uid,
                 "<b>Админ-команды</b>\n"
                 "/stats — статистика\n"
                 "/queue — заказы в очереди и в работе\n"
                 "/startorder &lt;id&gt; — запустить заказ\n"
                 "/failorder &lt;id&gt; [причина] — прервать с возвратом остатка\n"
                 "/addbalance &lt;tg_id&gt; &lt;сумма&gt; — начислить (или списать с минусом)\n"
                 "/user &lt;tg_id&gt; — баланс и заказы пользователя\n"
                 "/paid &lt;payment_id&gt; — вручную подтвердить платёж");
        return true;
    }
    if (cmd == "/stats") {
        auto st = db_.stats();
        safeSend(uid, "📊 <b>Статистика</b>\nПользователей: " + std::to_string(st.users) +
                          "\nПоступило оплат: " + money(st.paidTotal) + "\nВыручка по заказам: " + money(st.revenue) +
                          "\nОстатки на балансах: " + money(st.totalBalance) +
                          "\nЗаказов: в очереди " + std::to_string(st.ordersQueued) + ", в работе " +
                          std::to_string(st.ordersRunning) + ", завершено " + std::to_string(st.ordersCompleted));
        return true;
    }
    if (cmd == "/queue") {
        std::string t;
        for (auto st : {"queued", "running"})
            for (auto& o : db_.ordersByStatus(st)) t += orderCard(o) + "\n";
        safeSend(uid, t.empty() ? "Активных заказов нет." : t);
        return true;
    }
    if (cmd == "/startorder" || cmd == "/failorder") {
        int64_t oid = 0;
        if (!util::parseInt(a1, oid)) return safeSend(uid, "Укажите id заказа."), true;
        auto o = db_.getOrder(oid);
        if (!o) return safeSend(uid, "Заказ не найден."), true;
        if (cmd == "/startorder") {
            if (o->status != "queued") return safeSend(uid, "Заказ не в очереди: " + statusLabel(o->status)), true;
            std::string err;
            safeSend(uid, launchOrder(*o, err) ? "Заказ запущен." : "Ошибка: " + htmlEscape(err));
        } else {
            std::string reason = util::trim(a2 + " " + rest);
            if (reason.empty()) reason = "технические проблемы";
            if (o->status != "queued" && o->status != "running")
                return safeSend(uid, "Заказ уже закрыт: " + statusLabel(o->status)), true;
            failOrder(*o, "failed", reason);
            safeSend(uid, "Заказ #" + std::to_string(oid) + " прерван, остаток возвращён.");
        }
        return true;
    }
    if (cmd == "/addbalance") {
        int64_t target = 0, kop = 0;
        bool neg = !a2.empty() && a2[0] == '-';
        if (!util::parseInt(a1, target) || !util::parseRub(neg ? a2.substr(1) : a2, kop))
            return safeSend(uid, "Формат: /addbalance &lt;tg_id&gt; &lt;сумма&gt;"), true;
        if (neg) kop = -kop;
        if (!db_.getUser(target)) return safeSend(uid, "Пользователь не найден (он должен хотя бы раз написать боту)."), true;
        if (!db_.adjustBalance(target, kop, "admin", "admin:" + std::to_string(uid)))
            return safeSend(uid, "Не удалось: баланс ушёл бы в минус."), true;
        auto u = db_.getUser(target);
        safeSend(uid, "Готово. Баланс " + std::to_string(target) + ": " + money(u->balance));
        safeSend(target, (kop >= 0 ? "💰 Вам начислено " : "💸 С баланса списано ") + money(kop >= 0 ? kop : -kop) +
                             ". Баланс: <b>" + money(u->balance) + "</b>");
        return true;
    }
    if (cmd == "/user") {
        int64_t target = 0;
        if (!util::parseInt(a1, target)) return safeSend(uid, "Формат: /user &lt;tg_id&gt;"), true;
        auto u = db_.getUser(target);
        if (!u) return safeSend(uid, "Пользователь не найден."), true;
        std::string t = "👤 " + std::to_string(u->id) + (u->username.empty() ? "" : " @" + htmlEscape(u->username)) +
                        "\nБаланс: " + money(u->balance) + "\n\n";
        for (auto& o : db_.userOrders(target, 5)) t += orderCard(o) + "\n";
        safeSend(uid, t);
        return true;
    }
    if (cmd == "/paid") {
        int64_t pid = 0;
        if (!util::parseInt(a1, pid)) return safeSend(uid, "Формат: /paid &lt;payment_id&gt;"), true;
        auto p = db_.getPayment(pid);
        if (!p) return safeSend(uid, "Платёж не найден."), true;
        if (p->status != "pending" && p->status != "expired")
            return safeSend(uid, "Платёж в статусе " + p->status + ", подтвердить нельзя."), true;
        applyPaymentStatus(*p, PayStatus::Succeeded);
        safeSend(uid, "Платёж #" + std::to_string(pid) + " подтверждён вручную.");
        return true;
    }
    return false;
}

// ============================================================================
// Утилиты
// ============================================================================

bool Bot::isAdmin(int64_t uid) const {
    return std::find(cfg_.adminIds.begin(), cfg_.adminIds.end(), uid) != cfg_.adminIds.end();
}

void Bot::notifyAdmins(const std::string& html, const json& markup) {
    for (auto id : cfg_.adminIds) safeSend(id, html, markup);
}

void Bot::safeSend(int64_t chatId, const std::string& html, const json& markup) {
    try {
        tg_.sendMessage(chatId, html, markup);
    } catch (const std::exception& e) {
        util::log("sendMessage " + std::to_string(chatId) + ": " + e.what());
    }
}
