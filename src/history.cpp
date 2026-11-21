#include "history.h"

#include <algorithm>
#include <cstdio>

namespace ch {
namespace {

constexpr int64_t kRetentionSec = 30LL * 24 * 3600;
constexpr size_t kMaxSamples = 50000;
constexpr int64_t kMinIntervalSec = 60;

std::string HexId(const std::wstring& id) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(id.size() * 8);
    for (wchar_t wc : id) {
        uint32_t c = (uint32_t)wc;
        for (int shift = 28; shift >= 0; shift -= 4) out.push_back(digits[(c >> shift) & 15]);
    }
    return out;
}

bool UnhexId(const std::string& text, std::wstring& out) {
    if (text.empty() || text.size() % 8 || text.size() > 256 * 8) return false;
    out.clear();
    for (size_t i = 0; i < text.size(); i += 8) {
        uint32_t value = 0;
        for (size_t j = i; j < i + 8; j++) {
            char c = text[j];
            int digit = c >= '0' && c <= '9' ? c - '0'
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0) return false;
            value = (value << 4) | (uint32_t)digit;
        }
        if (!value || value > 0xFFFD) return false;
        out.push_back((wchar_t)value);
    }
    return !out.empty();
}

std::vector<std::string> Split(const std::string& row, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t end = row.find(sep, start);
        if (end == std::string::npos) {
            parts.push_back(row.substr(start));
            return parts;
        }
        parts.push_back(row.substr(start, end - start));
        start = end + 1;
    }
}

} // namespace

void History::Add(const Reading& r, int64_t now) {
    if (r.percent < 0 || r.percent > 100 || r.id.empty()) return;
    if (!r.connected || r.asleep) return;

    auto it = lastByDevice_.find(r.id);
    if (it != lastByDevice_.end()) {
        const int64_t elapsed = now - it->second;
        if (elapsed < 0) return;
        // Keep one sample a minute; a charging state change is worth recording right away.
        if (elapsed < kMinIntervalSec) {
            // Refresh the last row if the level moved since it was stored.
            for (auto s = samples_.rbegin(); s != samples_.rend(); ++s) {
                if (s->id != r.id) continue;
                if (s->percent != r.percent || s->charging != r.charging) {
                    s->percent = r.percent;
                    s->charging = r.charging;
                    s->t = now;
                    it->second = now;
                    dirty_ = true;
                }
                break;
            }
            return;
        }
    }

    samples_.push_back({ r.id, now, r.percent, r.charging });
    lastByDevice_[r.id] = now;
    dirty_ = true;
    Prune(now);
}

std::vector<Sample> History::Get(const std::wstring& id, int64_t since) const {
    std::vector<Sample> out;
    for (const auto& s : samples_)
        if (s.id == id && s.t >= since) out.push_back(s);
    return out;
}

int64_t History::LastSample(const std::wstring& id) const {
    auto it = lastByDevice_.find(id);
    return it == lastByDevice_.end() ? 0 : it->second;
}

void History::Prune(int64_t now) {
    const int64_t oldest = now - kRetentionSec;
    const size_t before = samples_.size();
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                  [oldest, now](const Sample& s) { return s.t < oldest || s.t > now + 3600; }),
                   samples_.end());
    if (samples_.size() > kMaxSamples)
        samples_.erase(samples_.begin(), samples_.begin() + (samples_.size() - kMaxSamples));
    if (samples_.size() != before) dirty_ = true;

    for (auto it = lastByDevice_.begin(); it != lastByDevice_.end();) {
        if (it->second < oldest) it = lastByDevice_.erase(it);
        else ++it;
    }
}

bool History::Load(const std::wstring& path) {
    samples_.clear();
    lastByDevice_.clear();
    dirty_ = false;

    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) return false;

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        std::string row(line);
        while (!row.empty() && (row.back() == '\n' || row.back() == '\r')) row.pop_back();
        if (row.empty()) continue;
        auto parts = Split(row, '\t');
        if (parts.size() != 4) continue;

        Sample s;
        if (!UnhexId(parts[0], s.id)) continue;
        char* end = nullptr;
        s.t = (int64_t)_strtoi64(parts[1].c_str(), &end, 10);
        if (!end || *end || s.t <= 0) continue;
        s.percent = atoi(parts[2].c_str());
        s.charging = parts[3] == "1";
        if (s.percent < 0 || s.percent > 100) continue;
        samples_.push_back(std::move(s));
    }
    fclose(file);

    std::stable_sort(samples_.begin(), samples_.end(),
                     [](const Sample& a, const Sample& b) { return a.t < b.t; });
    Prune(NowUnix());
    for (const auto& s : samples_) lastByDevice_[s.id] = std::max(lastByDevice_[s.id], s.t);
    return true;
}

bool History::Save(const std::wstring& path) const {
    if (!dirty_) return true;
    std::wstring tmp = path + L".tmp";
    FILE* file = _wfopen(tmp.c_str(), L"wb");
    if (!file) return false;

    std::string buffer;
    buffer.reserve(samples_.size() * 48);
    for (const auto& s : samples_) {
        buffer += HexId(s.id);
        buffer += '\t';
        buffer += std::to_string((long long)s.t);
        buffer += '\t';
        buffer += std::to_string(s.percent);
        buffer += '\t';
        buffer += s.charging ? '1' : '0';
        buffer += '\n';
    }
    const bool ok = fwrite(buffer.data(), 1, buffer.size(), file) == buffer.size();
    fclose(file);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    dirty_ = false;
    return true;
}

} // namespace ch
