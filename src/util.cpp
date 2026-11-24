#include "core.h"

#include <atomic>
#include <cstdarg>
#include <cwchar>
#include <ctime>

namespace ch {

namespace {
std::atomic<int> gUiLanguage{ 0 };   // 0 system, 1 Russian, 2 English
}

int64_t NowUnix() { return (int64_t)time(nullptr); }

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return buf;
}

void SetUiLanguage(int language) { gUiLanguage.store(language); }

bool RussianUi() {
    const int language = gUiLanguage.load();
    if (language == 1) return true;
    if (language == 2) return false;
    // Auto: Russian Windows gets Russian, everything else English.
    LANGID lang = GetUserDefaultUILanguage();
    return (lang & 0x3FF) == LANG_RUSSIAN;
}

std::wstring FormatSpan(int minutes) {
    if (minutes < 0) minutes = 0;
    const bool ru = RussianUi();
    if (minutes < 1) return ru ? L"меньше минуты" : L"less than a minute";
    if (minutes >= 24 * 60) return ru ? L"больше суток" : L"more than a day";
    int h = minutes / 60, m = minutes % 60;
    if (h == 0) return ru ? Fmt(L"%d мин", m) : Fmt(L"%d min", m);
    if (m == 0) return ru ? Fmt(L"%d ч", h) : Fmt(L"%d h", h);
    return ru ? Fmt(L"%d ч %02d мин", h, m) : Fmt(L"%d h %02d min", h, m);
}

std::wstring FormatAgo(int64_t seconds) {
    const bool ru = RussianUi();
    if (seconds < 5) return ru ? L"только что" : L"just now";
    if (seconds < 60) return ru ? Fmt(L"%lld с назад", (long long)seconds) : Fmt(L"%lld s ago", (long long)seconds);
    int64_t m = seconds / 60;
    if (m < 60) return ru ? Fmt(L"%lld мин назад", (long long)m) : Fmt(L"%lld min ago", (long long)m);
    int64_t h = m / 60;
    return ru ? Fmt(L"%lld ч назад", (long long)h) : Fmt(L"%lld h ago", (long long)h);
}

} // namespace ch
