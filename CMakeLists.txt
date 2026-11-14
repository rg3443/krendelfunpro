cmake_minimum_required(VERSION 3.20)
project(twitch_rent_bot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json CONFIG REQUIRED)

# cpr: vcpkg на Windows; на Linux — системный пакет, если есть. Если нет (Ubuntu 24.04, Debian 12) —
# собираем cpr из исходников поверх системного libcurl (нужен libcurl4-openssl-dev).
find_package(cpr CONFIG QUIET)
if(NOT cpr_FOUND)
    message(STATUS "cpr не найден в системе — скачиваю и собираю cpr 1.10.5")
    include(FetchContent)
    set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
    set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git GIT_TAG 1.10.5)
    FetchContent_MakeAvailable(cpr)
endif()

# SQLite: в vcpkg — unofficial::sqlite3::sqlite3, в системе (libsqlite3-dev) — SQLite::SQLite3
find_package(unofficial-sqlite3 CONFIG QUIET)
if(unofficial-sqlite3_FOUND)
    set(SQLITE_TARGET unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 REQUIRED)
    set(SQLITE_TARGET SQLite::SQLite3)
endif()

find_package(Threads REQUIRED)

add_executable(rentbot
    src/main.cpp
    src/bot.cpp
    src/config.cpp
    src/db.cpp
    src/executor.cpp
    src/payments.cpp
    src/telegram.cpp
    src/util.cpp
)

target_link_libraries(rentbot PRIVATE cpr::cpr nlohmann_json::nlohmann_json ${SQLITE_TARGET} Threads::Threads)

add_executable(db_test tests/db_test.cpp src/db.cpp src/util.cpp)
target_link_libraries(db_test PRIVATE ${SQLITE_TARGET})

add_executable(exec_test tests/exec_test.cpp src/executor.cpp src/util.cpp)
target_link_libraries(exec_test PRIVATE cpr::cpr nlohmann_json::nlohmann_json)

# С русскоязычным MSVC генератор NMake не распознаёт вывод /showIncludes и не отслеживает изменения
# заголовков — после их правки остаются устаревшие .obj, и программа падает. Поэтому явно:
# любой .cpp пересобирается при изменении любого заголовка из src/ (проект маленький, это быстро).
file(GLOB PROJECT_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.hpp)
file(GLOB PROJECT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/tests/*.cpp)
set_source_files_properties(${PROJECT_SOURCES} PROPERTIES OBJECT_DEPENDS "${PROJECT_HEADERS}")

add_executable(payments_test tests/payments_test.cpp src/payments.cpp src/util.cpp)
target_link_libraries(payments_test PRIVATE cpr::cpr nlohmann_json::nlohmann_json)

add_executable(payments_live_test tests/payments_live_test.cpp src/payments.cpp src/config.cpp src/util.cpp)
target_link_libraries(payments_live_test PRIVATE cpr::cpr nlohmann_json::nlohmann_json)

foreach(t rentbot db_test exec_test payments_test payments_live_test)
    if(MSVC)
        target_compile_options(${t} PRIVATE /utf-8 /W4 /permissive-)
        target_compile_definitions(${t} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra)
    endif()
endforeach()
