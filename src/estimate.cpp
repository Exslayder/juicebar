#include "estimate.h"

#include <algorithm>
#include <cmath>

namespace ch {
namespace {

struct Fit {
    bool ok = false;
    double slope = 0;   // percentage points per hour
    int n = 0;
    int64_t span = 0;
    int drop = 0;       // positive while discharging, negative while charging
};

Fit FitTrailing(const std::vector<Sample>& points, bool charging) {
    Fit fit;
    if (points.empty()) return fit;

    const int64_t kMaxGap = 25 * 60;  // a longer gap means the device (or the PC) was off
    size_t start = points.size() - 1;
    while (start > 0) {
        const Sample& prev = points[start - 1];
        const Sample& cur = points[start];
        const int64_t gap = cur.t - prev.t;
        if (gap <= 0 || gap > kMaxGap) break;
        // Small upward blips are integer rounding, not charge.
        const bool monotonic = charging ? (cur.percent >= prev.percent - 1) : (cur.percent <= prev.percent + 1);
        if (!monotonic) break;
        start--;
    }

    const size_t n = points.size() - start;
    fit.n = (int)n;
    fit.span = points.back().t - points[start].t;
    if (n < 3 || fit.span < 10 * 60) return fit;

    // Theil-Sen: the median of all pairwise slopes is far less sensitive to a single level jump
    // (a 5 % firmware step or a reconnection) than a least-squares fit.
    std::vector<double> slopes;
    slopes.reserve(n * (n - 1) / 2);
    for (size_t i = start; i < points.size(); i++) {
        for (size_t j = i + 1; j < points.size(); j++) {
            const int64_t dt = points[j].t - points[i].t;
            if (dt < 5 * 60) continue;
            const double dp = (double)points[j].percent - (double)points[i].percent;
            slopes.push_back(dp / ((double)dt / 3600.0));
        }
    }
    if (slopes.size() < 3) return fit;

    const size_t mid = slopes.size() / 2;
    std::nth_element(slopes.begin(), slopes.begin() + mid, slopes.end());
    fit.ok = true;
    fit.slope = slopes[mid];   // points per hour; negative while discharging
    fit.drop = points[start].percent - points.back().percent;
    return fit;
}

double Confidence(int64_t spanSec, int drop) {
    if (spanSec >= 2 * 3600 && drop >= 8) return 0.9;
    if (spanSec >= 3600 && drop >= 4) return 0.75;
    if (spanSec >= 30 * 60 && drop >= 2) return 0.6;
    if (spanSec >= 15 * 60 && drop >= 1) return 0.45;
    return 0.3;
}

int RoundForecastMinutes(double minutes) {
    if (!std::isfinite(minutes) || minutes <= 0) return -1;
    int value;
    if (minutes >= 120) value = (int)std::lround(minutes / 15.0) * 15;
    else if (minutes >= 30) value = (int)std::lround(minutes / 10.0) * 10;
    else value = (int)std::lround(minutes / 5.0) * 5;
    return std::max(5, value);
}

} // namespace

int VoltageToPercent(int millivolts) {
    const int curve[] = { 3300, 3680, 3750, 3790, 3830, 3870, 3910, 3960, 4020, 4080, 4150 };
    const int steps = (int)(sizeof(curve) / sizeof(curve[0]));
    if (millivolts <= curve[0]) return 0;
    for (int i = 1; i < steps; i++) {
        if (millivolts < curve[i]) {
            const int span = curve[i] - curve[i - 1];
            const int part = millivolts - curve[i - 1];
            return (i - 1) * 10 + part * 10 / span;
        }
    }
    return 100;
}

Forecast Estimate(const std::vector<Sample>& history, int percentNow, bool charging, int64_t now) {
    Forecast f;
    if (percentNow < 0) return f;
    percentNow = std::clamp(percentNow, 0, 100);

    const int64_t horizon = now - 6 * 3600;
    std::vector<Sample> points;
    points.reserve(history.size() + 1);
    for (const auto& s : history) {
        if (s.percent < 0 || s.percent > 100) continue;
        if (s.t < horizon || s.t > now + 60) continue;
        points.push_back(s);
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const Sample& a, const Sample& b) { return a.t < b.t; });

    // The poll that just finished is the most recent truth; replace an older sample with it.
    if (!points.empty() && points.back().t >= now - 5) {
        points.back().percent = percentNow;
        points.back().charging = charging;
    } else {
        points.push_back({ L"", now, percentNow, charging });
    }

    if (charging) {
        // Charging samples are not part of the discharge fit: keep only the trailing charging run.
        size_t end = points.size();
        size_t start = end - 1;
        while (start > 0 && points[start - 1].charging && points[end - 1].t - points[start - 1].t <= 6 * 3600) start--;
        std::vector<Sample> chargeRun(points.begin() + start, points.begin() + end);
        Fit fit = FitTrailing(chargeRun, true);
        f.state = ForecastState::Charging;
        f.samples = fit.n;
        f.spanSec = fit.span;
        if (fit.ok && fit.slope > 0.5) {
            const double hours = (100.0 - percentNow) / fit.slope;
            const int minutes = RoundForecastMinutes(hours * 60.0);
            if (minutes > 0 && minutes < 8 * 60) {
                f.minutes = minutes;
                f.pctPerHour = -fit.slope;
                f.confidence = Confidence(fit.span, -fit.drop);
            }
        }
        return f;
    }

    // Drop trailing charging samples so the fit only sees discharge.
    while (!points.empty() && points.back().charging) points.pop_back();
    if (points.empty()) return f;

    // Never rewrite the level of an older sample: append the current reading instead, so a device
    // that was charging in between starts a fresh segment (and a fresh "learning" window).
    if (points.back().t >= now - 180) points.back().percent = percentNow;
    else points.push_back({ L"", now, percentNow, false });

    Fit fit = FitTrailing(points, false);
    f.samples = fit.n;
    f.spanSec = fit.span;
    if (!fit.ok) {
        f.state = ForecastState::Learning;
        f.needSec = (int)std::max<int64_t>(5 * 60, 10 * 60 - fit.span);
        return f;
    }

    f.pctPerHour = -fit.slope;
    // A device that idles at a fraction of a percent per hour would produce a fantasy
    // forecast of several days. Show "steady" until the drain is actually measurable.
    if (f.pctPerHour < 0.5 || fit.drop < 1) {
        f.state = ForecastState::Flat;
        return f;
    }
    if (percentNow == 0) {
        f.state = ForecastState::Ready;
        f.minutes = 0;
        return f;
    }

    const double minutes = (double)percentNow / f.pctPerHour * 60.0;
    if (!std::isfinite(minutes) || minutes > 7 * 24 * 60) {
        f.state = ForecastState::Flat;
        return f;
    }
    f.minutes = RoundForecastMinutes(minutes);
    f.confidence = Confidence(fit.span, std::max(0, fit.drop));
    f.state = f.confidence >= 0.6 ? ForecastState::Ready : ForecastState::LowConfidence;
    return f;
}

} // namespace ch
