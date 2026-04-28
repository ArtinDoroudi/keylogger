cmake_minimum_required(VERSION 3.16)
project(ethical-keylogger-demo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/main.cpp
    src/logger.cpp
    # Platform-specific sources (uncomment when implemented):
    # src/platform/windows_hook.cpp   # WIN32 only
    # src/platform/macos_hook.cpp     # APPLE only
)

add_executable(keylogger ${SOURCES})

if(WIN32)
    target_link_libraries(keylogger PRIVATE user32)
elseif(APPLE)
    find_library(APP_SERVICES ApplicationServices REQUIRED)
    find_library(FOUNDATION Foundation REQUIRED)
    target_link_libraries(keylogger PRIVATE ${APP_SERVICES} ${FOUNDATION})
endif()
