#pragma once
#include "core.h"

#include <cstdint>
#include <vector>

namespace ch {

// One observed battery level. Times are unix seconds (UTC).
struct Sample {
    std::wstring id;
    int64_t t = 0;
    int percent = -1;
    bool charging = false;
};

enum class ForecastState {
    NoData,         // nothing known yet
    Learning,       // real value, but the discharge window is too short
    Ready,          // good forecast
    LowConfidence,  // forecast, marked as preliminary
    Flat,           // level is not moving (device idle/plugged, or too coarse)
    Charging,       // charging; `minutes` is set when time-to-full is known
};

struct Forecast {
    ForecastState state = ForecastState::NoData;
    int minutes = -1;          // remaining (or to full) minutes, -1 unknown
    double pctPerHour = 0;     // discharge rate, positive = losing charge
    int samples = 0;           // samples used by the fit
    int64_t spanSec = 0;       // observed window
    int needSec = 0;           // Learning: how much more observation is needed
    double confidence = 0;     // 0..1 heuristic
};

// A rough, local-only projection from observed discharge samples. Not a device-reported runtime.
Forecast Estimate(const std::vector<Sample>& history, int percentNow, bool charging, int64_t now);

// Li-ion discharge curve used for devices that only report voltage (Razer Barracuda X).
int VoltageToPercent(int millivolts);

} // namespace ch
