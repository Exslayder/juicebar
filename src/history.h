#pragma once
#include "core.h"
#include "estimate.h"

#include <map>
#include <string>
#include <vector>

namespace ch {

// Battery history: one row per device reading, persisted as plain TSV (id hex, time, level, charging).
// Bounded: 30 days and 50 000 rows, so the file stays small and parsing stays fast.
class History {
public:
    void Add(const Reading& r, int64_t now);
    std::vector<Sample> Get(const std::wstring& id, int64_t since) const;
    int64_t LastSample(const std::wstring& id) const;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    void Prune(int64_t now);

    size_t size() const { return samples_.size(); }

private:
    std::vector<Sample> samples_;                  // sorted by time
    std::map<std::wstring, int64_t> lastByDevice_; // last recorded time per device
    mutable bool dirty_ = true;
};

} // namespace ch
