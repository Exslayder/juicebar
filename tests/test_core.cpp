// Unit tests for the parts that decide what the UI shows: the discharge model, the history file
// format and the voltage curve. No devices are needed.
#include "../src/estimate.h"
#include "../src/history.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ch;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        printf("  FAIL: %s\n", what);
        g_failures++;
    }
}

void CheckNear(double actual, double expected, double tolerance, const char* what) {
    if (std::fabs(actual - expected) > tolerance) {
        printf("  FAIL: %s (got %.2f, expected %.2f +/- %.2f)\n", what, actual, expected, tolerance);
        g_failures++;
    }
}

Sample MakeSample(const wchar_t* id, int64_t t, int percent, bool charging = false) {
    Sample s;
    s.id = id;
    s.t = t;
    s.percent = percent;
    s.charging = charging;
    return s;
}

// A steady discharge: 10% per hour, one sample every five minutes.
std::vector<Sample> DischargeSeries(int64_t start, int hours, int percentPerHour) {
    std::vector<Sample> out;
    const int stepMinutes = 5;
    const int steps = hours * 60 / stepMinutes;
    for (int i = 0; i <= steps; i++) {
        const int64_t t = start + (int64_t)i * stepMinutes * 60;
        const int pct = 100 - (i * stepMinutes * percentPerHour) / 60;
        out.push_back(MakeSample(L"test:1", t, pct));
    }
    return out;
}

void TestVoltageCurve() {
    printf("voltage curve\n");
    Check(VoltageToPercent(3200) == 0, "below the curve clamps to 0");
    Check(VoltageToPercent(3300) == 0, "empty cell is 0%");
    Check(VoltageToPercent(3790) == 30, "3.79 V is 30%");
    Check(VoltageToPercent(3960) == 70, "3.96 V is 70%");
    Check(VoltageToPercent(4150) == 100, "4.15 V is 100%");
    Check(VoltageToPercent(4300) == 100, "above the curve clamps to 100");
    const int mid = VoltageToPercent(3807);
    Check(mid >= 30 && mid <= 40, "3.807 V lands between 30% and 40%");
}

void TestLearning() {
    printf("learning state\n");
    const int64_t now = 1700000000;
    std::vector<Sample> series = DischargeSeries(now - 10 * 60, 0, 10);
    series.resize(3);
    Forecast f = Estimate(series, 99, false, now);
    Check(f.state == ForecastState::Learning, "three samples are not enough for a forecast");
    Check(f.needSec > 0, "learning asks for more observation time");
    Check(f.minutes < 0, "no number is shown while learning");
}

void TestLearningCountdown() {
    printf("learning countdown\n");
    const int64_t now = 1700000000;
    std::vector<Sample> series;
    series.push_back(MakeSample(L"test:2", now - 10 * 60, 90));
    series.push_back(MakeSample(L"test:2", now - 5 * 60, 90));
    series.push_back(MakeSample(L"test:2", now - 60, 90));
    Forecast f = Estimate(series, 90, false, now - 30);
    Check(f.state == ForecastState::Learning, "10 min of data is still learning");
    Check(f.needSec > 0 && f.needSec < 10 * 60, "the countdown reflects the observed span");
    Check(f.needSec <= 15 * 60, "the countdown never exceeds the calibration window");

    // A sample 16 minutes back completes the window; a steady level is then flat, not an endless
    // calibration.
    series.push_back(MakeSample(L"test:2", now - 16 * 60, 90));
    Forecast g = Estimate(series, 90, false, now);
    Check(g.state != ForecastState::Learning, "15 min of data finishes the calibration");
}

void TestDischargeForecast() {
    printf("discharge forecast\n");
    const int64_t now = 1700000000;
    const int64_t start = now - 3 * 3600;
    std::vector<Sample> series = DischargeSeries(start, 3, 6);   // 6% per hour
    Forecast f = Estimate(series, 82, false, now);
    Check(f.state == ForecastState::Ready || f.state == ForecastState::LowConfidence, "3 h of data yield a forecast");
    CheckNear(f.pctPerHour, 6.0, 0.6, "rate matches the simulated drain");
    CheckNear((double)f.minutes, 82.0 / 6.0 * 60.0, 60.0, "remaining time is level / rate");
}

void TestSteepDrain() {
    printf("steep drain\n");
    const int64_t now = 1700000000;
    const int64_t start = now - 2 * 3600;
    std::vector<Sample> series = DischargeSeries(start, 2, 20);
    Forecast f = Estimate(series, 60, false, now);
    CheckNear((double)f.minutes, 60.0 / 20.0 * 60.0, 40.0, "20%/h is converted to minutes");
}

void TestFlat() {
    printf("flat level\n");
    const int64_t now = 1700000000;
    std::vector<Sample> series;
    for (int i = 0; i <= 12; i++)
        series.push_back(MakeSample(L"test:1", now - (12 - i) * 300, 80));
    Forecast f = Estimate(series, 80, false, now);
    Check(f.state == ForecastState::Flat, "unchanged level reports no drain");
}

void TestCharging() {
    printf("charging forecast\n");
    const int64_t now = 1700000000;
    std::vector<Sample> series;
    // 30 minutes charging at 10% per hour-equivalent: +5% every 30 min.
    for (int i = 0; i <= 8; i++)
        series.push_back(MakeSample(L"test:1", now - (8 - i) * 600, 40 + i * 5, true));
    Forecast f = Estimate(series, 80, true, now);
    Check(f.state == ForecastState::Charging, "charging devices report the charging state");
    Check(f.minutes > 0, "charge rate gives time to full");
    CheckNear((double)f.minutes, 20.0 / 30.0 * 60.0, 15.0, "time to full is (100-level)/rate");
}

void TestGapBreaksSegment() {
    printf("gaps\n");
    const int64_t now = 1700000000;
    std::vector<Sample> series;
    // Old block, then a 2 hour gap, then a fresh block.
    for (int i = 0; i < 12; i++) series.push_back(MakeSample(L"test:1", now - 5 * 3600 + i * 300, 98 - i));
    for (int i = 0; i < 12; i++) series.push_back(MakeSample(L"test:1", now - 50 * 60 + i * 300, 80 - i));
    Forecast f = Estimate(series, 69, false, now);
    Check(f.spanSec <= 55 * 60, "the fit stops at the gap, it does not average across it");
    Check(f.minutes > 0, "the fresh block still produces a forecast");
}

void TestHistoryRoundTrip() {
    printf("history storage\n");
    const std::wstring path = L"build/test_history.tsv";
    DeleteFileW(path.c_str());

    History first;
    // Real current time: Load() prunes everything older than the retention window.
    const int64_t now = NowUnix() - 3600;
    for (int i = 0; i < 40; i++) {
        Reading r;
        r.id = L"steelseries:1838";
        r.name = L"Test mouse";
        r.percent = 90 - i / 4;
        r.charging = i > 35;
        first.Add(r, now + i * 60);
    }
    Check(first.size() >= 20, "one sample a minute is enough to keep a history");
    Check(first.Save(path), "history saves atomically");

    History second;
    Check(second.Load(path), "history loads");
    Check(second.size() == first.size(), "sample count survives a round trip");
    auto samples = second.Get(L"steelseries:1838", now - 60);
    Check(!samples.empty(), "samples are found by device id");
    Check(samples.back().percent <= 90 && samples.back().percent >= 70, "levels survive a round trip");
    DeleteFileW(path.c_str());
}

void TestHistoryIgnoresSillyValues() {
    printf("history validation\n");
    History history;
    Reading r;
    r.id = L"x:1";
    r.percent = 120;
    history.Add(r, 1700000000);
    r.percent = -3;
    history.Add(r, 1700000060);
    Check(history.size() == 0, "out of range levels are rejected");

    r.percent = 50;
    r.asleep = true;
    history.Add(r, 1700000120);
    Check(history.size() == 0, "asleep readings are not recorded");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("ChargeHover tests\n");
    TestVoltageCurve();
    TestLearning();
    TestLearningCountdown();
    TestDischargeForecast();
    TestSteepDrain();
    TestFlat();
    TestCharging();
    TestGapBreaksSegment();
    TestHistoryRoundTrip();
    TestHistoryIgnoresSillyValues();

    if (g_failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    printf("\n%d check(s) failed.\n", g_failures);
    return 1;
}
