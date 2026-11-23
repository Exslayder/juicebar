#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ch {

enum class Kind : int { Other = 0, Mouse, Keyboard, Headset, Gamepad, Earbuds, Speaker, Phone };

// One battery-powered device as seen during a single poll.
struct Reading {
    std::wstring id;          // stable identity across polls
    std::wstring name;
    Kind kind = Kind::Other;
    int percent = -1;         // 0..100, -1 when unknown
    bool charging = false;
    bool full = false;        // on power and fully charged
    bool asleep = false;      // last known value, device not answering
    bool connected = true;
    std::wstring via;         // "2.4 GHz", "USB", "Bluetooth"
    std::wstring driver;      // which driver produced the reading
};

int64_t NowUnix();
std::wstring Wide(const std::string& utf8);
std::string Utf8(const std::wstring& s);
std::wstring Fmt(const wchar_t* fmt, ...);
std::wstring FormatSpan(int minutes);   // "3 h 20 min" style, localized later in the UI
std::wstring FormatAgo(int64_t seconds);
bool RussianUi();

} // namespace ch
