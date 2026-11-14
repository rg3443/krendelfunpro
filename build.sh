#!/usr/bin/env bash
# Сборка на Linux (Ubuntu 24.04 / Debian 12).
#   ./build.sh          — сборка (Release) в ./build
#   ./build.sh deps     — установить пакеты через apt (нужен sudo), затем собрать
#   ./build.sh clean    — сборка с нуля
#   ./build.sh test     — собрать и прогнать тесты (exec_test — против фейкового сервера ботов)
set -euo pipefail
cd "$(dirname "$0")"

install_deps() {
    # под root sudo не нужен (и на минимальных образах VPS его может не быть)
    local sudo=""
    if [[ $EUID -ne 0 ]]; then sudo="sudo"; fi
    $sudo apt-get update
    $sudo apt-get install -y build-essential cmake ninja-build pkg-config git \
        nlohmann-json3-dev libsqlite3-dev libcurl4-openssl-dev libssl-dev python3
    # cpr пакетом в Ubuntu 24.04 / Debian 12 нет — CMake скачает и соберёт его сам (если пакет появится — возьмёт его)
    $sudo apt-get install -y libcpr-dev 2>/dev/null || echo "libcpr-dev недоступен — cpr соберётся из исходников"
}

case "${1:-}" in
    deps) install_deps ;;
    clean) rm -rf build ;;
esac

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
echo "OK: build/rentbot"

if [[ "${1:-}" == "test" ]]; then
    ./build/db_test
    ./build/payments_test
    python3 tests/fake_krendelbot.py 18080 secret >/dev/null 2>&1 &
    fake=$!
    trap 'kill $fake 2>/dev/null || true' EXIT
    sleep 1
    ./build/exec_test
fi
