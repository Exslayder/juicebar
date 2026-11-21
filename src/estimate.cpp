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
    if (points.size() < 4) return fit;

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
    if (n < 4) return fit;
    const int64_t span = points.back().t - points[start].t;
    if (span < 15 * 60) return fit;

    // Least squares on hours/percent; the intercept absorbs the level at window start.
    const double x0 = (double)points[start].t;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = start; i < points.size(); i++) {
        const double x = ((double)points[i].t - x0) / 3600.0;
        const double y = (double)points[i].percent;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double count = (double)n;
    const double denom = count * sxx - sx * sx;
    if (std::fabs(denom) < 1e-9) return fit;

    fit.ok = true;
    fit.slope = (count * sxy - sx * sy) / denom;
    fit.n = (int)n;
    fit.span = span;
    fit.drop = points[start].percent - points.back().percent;
    return fit;
}

double Confidence(int64_t spanSec, int drop) {
    if (spanSec >= 2 * 3600 && drop >= 10) return 0.92;
    if (spanSec >= 3600 && drop >= 5) return 0.8;
    if (spanSec >= 30 * 60 && drop >= 3) return 0.65;
    if (spanSec >= 15 * 60) return 0.45;
    return 0.3;
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
            double minutes = hours * 60.0;
            if (minutes > 0 && minutes < 8 * 60) {
                f.minutes = (int)std::lround(minutes);
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
        f.needSec = (int)std::max<int64_t>(0, 15 * 60 - fit.span);
        return f;
    }

    f.pctPerHour = -fit.slope;
    // A device that idles at a fraction of a percent per hour would produce a fantasy
    // forecast of several days. Show "steady" until the drain is actually measurable.
    if (f.pctPerHour < 0.8) {
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
    f.minutes = (int)std::lround(minutes);
    f.confidence = Confidence(fit.span, std::max(0, fit.drop));
    f.state = f.confidence >= 0.6 ? ForecastState::Ready : ForecastState::LowConfidence;
    return f;
}

} // namespace ch
