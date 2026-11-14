#include "telegram.hpp"

#include <cpr/cpr.h>

#include <stdexcept>

#include "util.hpp"

TgApi::TgApi(std::string token) : base_("https://api.telegram.org/bot" + std::move(token) + "/") {}

json TgApi::call(const std::string& method, const json& params, int timeoutSec) {
    auto r = cpr::Post(cpr::Url{base_ + method}, cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{params.dump()}, cpr::Timeout{timeoutSec * 1000});
    if (r.error) throw std::runtime_error("Telegram " + method + ": " + r.error.message);
    json j = json::parse(r.text, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error("Telegram " + method + ": bad JSON, HTTP " + std::to_string(r.status_code));
    if (!j.value("ok", false))
        throw std::runtime_error("Telegram " + method + ": " + j.value("description", std::string("unknown error")));
    return j["result"];
}

json TgApi::getMe() { return call("getMe", json::object()); }

std::vector<json> TgApi::getUpdates(int64_t offset, int timeoutSec) {
    json p = {{"offset", offset}, {"timeout", timeoutSec}, {"allowed_updates", {"message", "callback_query"}}};
    json res = call("getUpdates", p, timeoutSec + 10);
    return res.get<std::vector<json>>();
}

json TgApi::sendMessage(int64_t chatId, const std::string& html, const json& replyMarkup) {
    json p = {{"chat_id", chatId}, {"text", html}, {"parse_mode", "HTML"}, {"disable_web_page_preview", true}};
    if (!replyMarkup.is_null()) p["reply_markup"] = replyMarkup;
    return call("sendMessage", p);
}

void TgApi::editMessage(int64_t chatId, int64_t messageId, const std::string& html, const json& replyMarkup) {
    json p = {{"chat_id", chatId}, {"message_id", messageId}, {"text", html}, {"parse_mode", "HTML"},
              {"disable_web_page_preview", true}};
    if (!replyMarkup.is_null()) p["reply_markup"] = replyMarkup;
    try {
        call("editMessageText", p);
    } catch (const std::exception& e) {
        // "message is not modified" — не ошибка
        if (std::string(e.what()).find("not modified") == std::string::npos) throw;
    }
}

void TgApi::answerCallback(const std::string& callbackId, const std::string& text, bool alert) {
    json p = {{"callback_query_id", callbackId}};
    if (!text.empty()) {
        p["text"] = text;
        p["show_alert"] = alert;
    }
    try {
        call("answerCallbackQuery", p);
    } catch (const std::exception& e) {
        util::log(std::string("answerCallback: ") + e.what());
    }
}

json TgApi::button(const std::string& text, const std::string& data) {
    return {{"text", text}, {"callback_data", data}};
}

json TgApi::urlButton(const std::string& text, const std::string& url) { return {{"text", text}, {"url", url}}; }

json TgApi::inlineKeyboard(const std::vector<std::vector<json>>& rows) {
    json kb = json::array();
    for (auto& r : rows) kb.push_back(r);
    return {{"inline_keyboard", kb}};
}
