cmake_minimum_required(VERSION 3.16)
project(infinity_router C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

set(ROUTER_SOURCES
  src/platform.c src/strbuf.c src/json.c src/log.c src/config.c
  src/http_client.c src/quota.c src/router.c src/server.c src/settings.c src/auth.c src/stats.c src/crypto.c
  src/providers/provider.c src/providers/gemini.c src/providers/openai.c
  src/providers/anthropic.c src/providers/mock.c src/providers/media.c)

# Ядро — отдельной библиотекой, чтобы его использовали и сервер, и тесты
add_library(router_core STATIC ${ROUTER_SOURCES})
target_include_directories(router_core PUBLIC src)
target_link_libraries(router_core PUBLIC CURL::libcurl SQLite::SQLite3 OpenSSL::Crypto Threads::Threads)
if(WIN32)
  target_link_libraries(router_core PUBLIC ws2_32 crypt32)
  # inet_pton нужен Windows 7+; корректные %zu/%lld в printf у MinGW
  target_compile_definitions(router_core PUBLIC _WIN32_WINNT=0x0601 __USE_MINGW_ANSI_STDIO=1)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(router_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(infinity-router src/main.c)
target_link_libraries(infinity-router PRIVATE router_core)
# каталог моделей для автоматического каскада кладём рядом с конфигом
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/catalog.json ${CMAKE_CURRENT_BINARY_DIR}/catalog.json COPYONLY)

# Тесты: ctest (или ./build/router-tests)
enable_testing()
add_executable(router-tests tests/test_main.c)
target_link_libraries(router-tests PRIVATE router_core)
add_test(NAME router-tests COMMAND router-tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests)
