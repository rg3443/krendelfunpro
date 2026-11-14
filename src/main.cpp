#include <atomic>
#include <csignal>
#include <iostream>

#include "bot.hpp"
#include "config.hpp"
#include "db.hpp"
#include "executor.hpp"
#include "payments.hpp"
#include "telegram.hpp"
#include "util.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

static std::atomic<bool> g_stop{false};

static void onSignal(int) { g_stop = true; }

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::string cfgPath = argc > 1 ? argv[1] : "config.json";
    try {
        Config cfg = Config::load(cfgPath);
        Db db(cfg.dbPath);
        TgApi tg(cfg.telegramToken);
        auto pay = makePaymentProvider(cfg);
        auto exec = makeExecutor(cfg);
        if (cfg.adminIds.empty()) util::log("ВНИМАНИЕ: admin_ids пуст — некому подтверждать заказы в ручном режиме");

        Bot bot(cfg, db, tg, *pay, *exec);
        bot.run(g_stop);
        util::log("Остановлен");
    } catch (const std::exception& e) {
        util::log(std::string("Фатальная ошибка: ") + e.what());
        return 1;
    }
    return 0;
}
