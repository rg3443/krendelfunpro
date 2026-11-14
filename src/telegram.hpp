#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// Минимальный клиент Telegram Bot API (long polling, HTML-разметка).
class TgApi {
public:
    explicit TgApi(std::string token);

    json getMe();
    std::vector<json> getUpdates(int64_t offset, int timeoutSec);
    json sendMessage(int64_t chatId, const std::string& html, const json& replyMarkup = nullptr);
    void editMessage(int64_t chatId, int64_t messageId, const std::string& html, const json& replyMarkup = nullptr);
    void answerCallback(const std::string& callbackId, const std::string& text = "", bool alert = false);

    // Помощники для клавиатур
    static json button(const std::string& text, const std::string& data);
    static json urlButton(const std::string& text, const std::string& url);
    static json inlineKeyboard(const std::vector<std::vector<json>>& rows);

private:
    json call(const std::string& method, const json& params, int timeoutSec = 20);
    std::string base_;
};
