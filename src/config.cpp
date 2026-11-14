#include "config.hpp"

#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

using json = nlohmann::json;

static int64_t rubToKop(double rub) { return static_cast<int64_t>(std::llround(rub * 100.0)); }

// Встроенные подробные описания тарифов. Описывают то, что реально делает сервер krendelbot
// (prompts/tier_*.txt, правила policy/guard). Если поменяли поведение ботов — поправьте текст.
std::string defaultTierDetails(const std::string& id) {
    if (id == "smart")
        return "🧠 <b>Умный бот — живой собеседник на Claude Opus</b>\n\n"
               "Внутри — <b>Claude Opus</b> от Anthropic, одна из самых сильных языковых моделей в мире. "
               "Это не скрипт с набором заготовок: бот по-настоящему читает чат и понимает, о чём идёт речь.\n\n"
               "<b>Что он умеет</b>\n"
               "• Следит за чатом в реальном времени и помнит последние сообщения — отвечает по смыслу, "
               "а не общими фразами\n"
               "• Откликается, когда к нему обращаются по нику или прозвищу: подхватывает мысль, "
               "задаёт встречные вопросы, шутит к месту\n"
               "• Отвечает со знанием дела — игры, механики, названия, цифры — коротко, в одну-две фразы\n"
               "• Когда чат затихает, предлагает тему и разгоняет обсуждение\n"
               "• У каждого бота свой характер, поэтому несколько ботов не звучат одинаково\n\n"
               "<b>Ведёт себя прилично</b>\n"
               "• Пишет в темпе живого зрителя — примерно раз в минуту, без флуда\n"
               "• Несколько ботов говорят по очереди и не устраивают бесконечный диалог между собой\n"
               "• Никаких ссылок, команд и слов из стоп-листа — каждое сообщение проходит фильтр\n"
               "• Стример и модераторы в любой момент могут поставить ботов на паузу командой в чате\n\n"
               "<b>Честно</b>: в профиле бота написано «🤖 ИИ чат-бот», а заходя в чат, он представляется "
               "ботом. Зрители видят умного ИИ-собеседника, а не подделку под человека.\n\n"
               "<b>Кому подходит</b>: тем, кому нужен живой, умный и вовлекающий чат — на старте канала, "
               "в тихие часы или чтобы новым зрителям было с кем поговорить.";
    if (id == "dumb")
        return "🤖 <b>Простой бот — живой фон чата</b>\n\n"
               "Пишет как обычный зритель, который смотрит стрим вполуха: короткие реакции в 1–5 слов — "
               "«ахах», «го», «ору», «гг», эмоуты. Не рассуждает и не даёт развёрнутых ответов.\n\n"
               "• Оживляет чат, чтобы он не выглядел пустым\n"
               "• Работает на облегчённой ИИ-модели — поэтому в 10 раз дешевле умного\n"
               "• Так же, как умный, пишет без флуда, без ссылок и представляется ботом\n\n"
               "<b>Кому подходит</b>: когда нужна массовка и фон, а не собеседник. "
               "Хотите, чтобы с ботами можно было поговорить по-настоящему, — берите 🧠 умных.";
    return "";
}

Config Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Не удалось открыть конфиг: " + path);
    json j = json::parse(f, nullptr, true, /*ignore_comments*/ true);

    Config c;
    c.telegramToken = j.at("telegram_token").get<std::string>();
    c.adminIds = j.value("admin_ids", std::vector<int64_t>{});
    c.dbPath = j.value("db_path", c.dbPath);
    c.supportContact = j.value("support_contact", "");

    if (j.contains("pricing")) {
        auto& p = j["pricing"];
        if (p.contains("tiers")) {
            for (auto& t : p["tiers"]) {
                Tier tier;
                tier.id = t.at("id").get<std::string>();
                tier.name = t.value("name", tier.id);
                tier.description = t.value("description", "");
                if (t.contains("details")) {  // строка или массив строк (по строке на абзац/пункт)
                    if (t["details"].is_array()) {
                        for (auto& line : t["details"]) tier.details += line.get<std::string>() + "\n";
                    } else {
                        tier.details = t["details"].get<std::string>();
                    }
                }
                tier.pricePerBotHour = rubToKop(t.at("price_per_bot_hour_rub").get<double>());
                c.tiers.push_back(tier);
            }
        } else if (p.contains("price_per_bot_hour_rub")) {  // старый формат: один тариф
            c.tiers.push_back({"standard", "🤖 Бот", "", "", rubToKop(p["price_per_bot_hour_rub"].get<double>())});
        }
        c.refundOnCancel = p.value("refund_on_cancel", c.refundOnCancel);
        c.minBots = p.value("min_bots", c.minBots);
        c.maxBots = p.value("max_bots", c.maxBots);
        c.minHours = p.value("min_hours", c.minHours);
        c.maxHours = p.value("max_hours", c.maxHours);
        c.botPresets = p.value("bot_presets", c.botPresets);
        c.hourPresets = p.value("hour_presets", c.hourPresets);
        c.remindBeforeMin = p.value("remind_before_min", c.remindBeforeMin);
    }
    if (j.contains("payments")) {
        auto& p = j["payments"];
        c.paymentProvider = p.value("provider", c.paymentProvider);
        c.yooShopId = p.value("shop_id", "");
        c.yooSecretKey = p.value("secret_key", "");
        c.paymentProxy = p.value("proxy", "");
        if (p.contains("payment_methods")) {
            c.paymentMethods = p["payment_methods"].get<std::vector<std::string>>();
        } else if (p.contains("payment_method")) {  // старый формат: один способ ("" — выбор на странице ЮKassa)
            std::string m = p["payment_method"].get<std::string>();
            c.paymentMethods = m.empty() ? std::vector<std::string>{} : std::vector<std::string>{m};
        }
        if (p.contains("receipt")) {
            auto& r = p["receipt"];
            c.receiptEnabled = r.value("enabled", c.receiptEnabled);
            c.receiptVatCode = r.value("vat_code", c.receiptVatCode);
            c.receiptItem = r.value("item_description", c.receiptItem);
            c.receiptPaymentSubject = r.value("payment_subject", c.receiptPaymentSubject);
            c.receiptPaymentMode = r.value("payment_mode", c.receiptPaymentMode);
            c.receiptTaxSystem = r.value("tax_system_code", c.receiptTaxSystem);
        }
        c.returnUrl = p.value("return_url", c.returnUrl);
        c.minTopup = rubToKop(p.value("min_topup_rub", 100.0));
        c.maxTopup = rubToKop(p.value("max_topup_rub", 100000.0));
        c.topupPresets = p.value("presets_rub", c.topupPresets);
        c.paymentPollSec = p.value("poll_interval_sec", c.paymentPollSec);
        c.paymentExpireMin = p.value("expire_minutes", c.paymentExpireMin);
    }
    if (j.contains("executor")) {
        auto& e = j["executor"];
        c.executorType = e.value("type", c.executorType);
        c.executorUrl = e.value("url", "");
        c.executorToken = e.value("token", "");
        c.autoStartTimeoutMin = e.value("auto_start_timeout_min", c.autoStartTimeoutMin);
        c.executorPollSec = e.value("poll_interval_sec", c.executorPollSec);
        c.executorIdOffset = e.value("order_id_offset", c.executorIdOffset);
    }

    if (c.telegramToken.empty()) throw std::runtime_error("telegram_token не задан");
    if (c.paymentProvider == "yookassa" && (c.yooShopId.empty() || c.yooSecretKey.empty()))
        throw std::runtime_error("Для provider=yookassa нужны payments.shop_id и payments.secret_key");
    for (size_t i = 0; i < c.paymentMethods.size(); ++i) {
        const auto& m = c.paymentMethods[i];
        if (m != "sbp" && m != "bank_card")
            throw std::runtime_error("payments.payment_methods: допустимы sbp и bank_card, а не '" + m + "'");
        for (size_t k = 0; k < i; ++k)
            if (c.paymentMethods[k] == m) throw std::runtime_error("payments.payment_methods: повторяется " + m);
    }
    if (c.receiptEnabled) {
        if (c.receiptVatCode < 1 || c.receiptVatCode > 12)
            throw std::runtime_error("payments.receipt.vat_code: код ставки НДС ЮKassa (1–12)");
        if (c.receiptItem.empty() || c.receiptItem.size() > 128 * 4)
            throw std::runtime_error("payments.receipt.item_description: от 1 до 128 символов");
        if (c.receiptTaxSystem < 0 || c.receiptTaxSystem > 6)
            throw std::runtime_error("payments.receipt.tax_system_code: 1–6 или 0 (не передавать)");
    }
    if (c.executorType == "http" && c.executorUrl.empty())
        throw std::runtime_error("Для executor.type=http нужен executor.url");
    if (c.tiers.empty()) {
        c.tiers = {{"smart", "🧠 Умный бот", "Claude Opus: читает чат и отвечает по смыслу", "", 10000},
                   {"dumb", "🤖 Простой бот", "короткие реакции, как обычный зритель", "", 1000}};
    }
    for (auto& t : c.tiers)
        if (t.details.empty()) t.details = defaultTierDetails(t.id);
    for (size_t i = 0; i < c.tiers.size(); ++i) {
        auto& t = c.tiers[i];
        if (t.id.empty() || t.id.size() > 20 ||
            t.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
            throw std::runtime_error("pricing.tiers: id тарифа — латиница/цифры/_, до 20 символов: '" + t.id + "'");
        if (t.pricePerBotHour <= 0) throw std::runtime_error("pricing.tiers: цена тарифа " + t.id + " должна быть > 0");
        for (size_t k = 0; k < i; ++k)
            if (c.tiers[k].id == t.id) throw std::runtime_error("pricing.tiers: повторяется id " + t.id);
    }
    if (c.minBots < 1 || c.maxBots < c.minBots || c.minHours < 1 || c.maxHours < c.minHours)
        throw std::runtime_error("Некорректные лимиты в pricing");
    return c;
}
