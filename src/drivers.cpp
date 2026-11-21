#include "drivers.h"

#include "estimate.h"
#include "hid.h"

#include <objbase.h>
#include <setupapi.h>
#include <devpkey.h>
#include <initguid.h>
#include <devguid.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <functional>
#include <cwchar>
#include <map>
#include <set>

namespace ch {
namespace {

// ---------------------------------------------------------------- helpers

const wchar_t* Via2G = L"2.4 GHz";

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool Contains(const std::wstring& hay, const wchar_t* needle) {
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

Kind GuessKind(const std::wstring& name) {
    if (Contains(name, L"mouse") || Contains(name, L"aerox") || Contains(name, L"viper") ||
        Contains(name, L"deathadder") || Contains(name, L"orochi") || Contains(name, L"g pro"))
        return Kind::Mouse;
    if (Contains(name, L"keyboard") || Contains(name, L"keychron") || Contains(name, L"apex") ||
        Contains(name, L"huntsman"))
        return Kind::Keyboard;
    if (Contains(name, L"headset") || Contains(name, L"headphone") || Contains(name, L"buds") ||
        Contains(name, L"earbud") || Contains(name, L"airpods") || Contains(name, L"arctis") ||
        Contains(name, L"barracuda") || Contains(name, L"speaker"))
        return Kind::Headset;
    if (Contains(name, L"controller") || Contains(name, L"gamepad") || Contains(name, L"dualsense") ||
        Contains(name, L"dualshock") || Contains(name, L"xbox"))
        return Kind::Gamepad;
    return Kind::Other;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

// ---------------------------------------------------------------- SteelSeries

struct AeroxModel {
    uint16_t pid;
    uint16_t receiverPid;   // one identity for the cable and the 2.4 GHz receiver
    const wchar_t* name;
    bool wired;
    bool plainPercent;      // Gen 2 reports 0..100 directly
};

const AeroxModel kAerox[] = {
    { 0x1838, 0x1838, L"SteelSeries Aerox 3 Wireless", false, false },
    { 0x183A, 0x1838, L"SteelSeries Aerox 3 Wireless", true, false },
    { 0x1878, 0x1878, L"SteelSeries Aerox 3 Wireless CS2 Dragon Lore", false, false },
    { 0x187A, 0x1878, L"SteelSeries Aerox 3 Wireless CS2 Dragon Lore", true, false },
    { 0x1890, 0x1890, L"SteelSeries Aerox 3 Wireless Gen 2", false, true },
    { 0x1892, 0x1890, L"SteelSeries Aerox 3 Wireless Gen 2", true, true },
    { 0x1852, 0x1852, L"SteelSeries Aerox 5 Wireless", false, false },
    { 0x1854, 0x1852, L"SteelSeries Aerox 5 Wireless", true, false },
    { 0x1858, 0x1858, L"SteelSeries Aerox 9 Wireless", false, false },
    { 0x185A, 0x1858, L"SteelSeries Aerox 9 Wireless", true, false },
    { 0x1840, 0x1840, L"SteelSeries Prime Wireless", false, false },
    { 0x1842, 0x1840, L"SteelSeries Prime Wireless", true, false },
    { 0x1860, 0x1860, L"SteelSeries Aerox 5 Wireless Diablo IV", false, false },
};

const AeroxModel* FindAerox(uint16_t pid) {
    for (const auto& m : kAerox)
        if (m.pid == pid) return &m;
    return nullptr;
}

// The receiver forwards one command at a time and SteelSeries GG may be talking to it too, so the
// query is resent until the device echoes the command byte.
int SteelSeriesQuery(Hid& dev, BYTE cmd, std::vector<BYTE>& reply, size_t minLen) {
    // 0 ok, 1 write failed, 2 asleep, 3 timeout
    dev.Flush();
    const ULONGLONG start = GetTickCount64(), end = start + 1000;
    ULONGLONG resend = 0;
    while (GetTickCount64() < end) {
        const ULONGLONG now = GetTickCount64();
        if (now >= resend) {
            if (!dev.WritePadded({ 0x00, cmd })) return 1;
            resend = now + 120;
        }
        ULONGLONG untilResend = resend > end ? end : resend;
        int wait = untilResend > now ? (int)(untilResend - now) : 1;
        if (dev.Read(reply, wait) <= 0) continue;
        // Input reports carry a report ID byte that some receivers send as 0x00: drop it so the
        // command echo is the first byte, like hidapi does.
        if (reply.size() >= 2 && reply[0] == 0x00) reply.erase(reply.begin());
        if (reply.size() >= 2 && reply[0] == 0x40 && reply[1] == 0xFF) return 2;  // mouse off / asleep
        if (reply.size() >= minLen && reply[0] == cmd) return 0;
    }
    return 3;
}

void PollSteelSeries(const std::vector<HidInterface>& hid, std::vector<Reading>& out, std::wstring& log) {
    std::set<uint16_t> done;
    for (const auto& iface : hid) {
        if (iface.vid != 0x1038) continue;
        const AeroxModel* model = FindAerox(iface.pid);
        if (!model || done.count(model->receiverPid)) continue;
        // The battery lives on the vendor collection (usage page FFC0, interface 3).
        if (iface.usagePage != 0xFFC0) continue;
        if (!iface.inLen || !iface.outLen) continue;

        Hid dev;
        if (!dev.Open(iface.path)) { log += L"SteelSeries: cannot open the vendor interface\n"; continue; }
        const BYTE cmd = model->wired ? 0x92 : 0xD2;
        std::vector<BYTE> reply;
        const int query = SteelSeriesQuery(dev, cmd, reply, model->plainPercent ? 3 : 2);
        if (query != 0) {
            if (query == 1) log += L"SteelSeries: write failed\n";
            if (query == 2) log += L"SteelSeries: mouse is off or asleep\n";
            if (query == 3) log += L"SteelSeries: no echo from the receiver within 1 s\n";
            // Off or asleep: keep the device visible with its last known level, dimmed.
            Reading asleep;
            asleep.id = Fmt(L"steelseries:%04X", model->receiverPid);
            asleep.name = model->name;
            asleep.kind = Kind::Mouse;
            asleep.via = model->wired ? L"USB" : Via2G;
            asleep.driver = L"steelseries";
            asleep.asleep = true;
            asleep.stamp = NowUnix();
            done.insert(model->receiverPid);
            out.push_back(std::move(asleep));
            continue;
        }

        Reading r;
        r.id = Fmt(L"steelseries:%04X", model->receiverPid);
        r.name = model->name;
        r.kind = Kind::Mouse;
        r.via = model->wired ? L"USB" : Via2G;
        r.driver = L"steelseries";
        r.stamp = NowUnix();
        if (model->plainPercent) {
            if (reply[1] > 2) continue;
            r.charging = reply[1] != 0;
            if (reply[2] > 100) continue;
            r.percent = reply[2];
            r.full = r.charging && r.percent >= 100;
        } else {
            const BYTE level = reply[1] & 0x7F;
            if (level == 0 || level > 21) continue;
            r.percent = (level - 1) * 5;
            r.charging = (reply[1] & 0x80) != 0;
            r.full = r.charging && r.percent >= 100;
        }
        done.insert(model->receiverPid);
        out.push_back(std::move(r));
    }
}

// ---------------------------------------------------------------- Razer Barracuda X

const uint16_t kRazerVid = 0x1532;
const uint16_t kBarracudaPids[] = { 0x0550, 0x0552 };

// Battery voltage (mV) at 0..100% for the headset's Li-ion cell, from the reverse engineered
// discharge curve of the Razer Barracuda X (checked against the Razer app).
const int kLiIonMv[] = { 3300, 3680, 3750, 3790, 3830, 3870, 3910, 3960, 4020, 4080, 4150 };

uint16_t Crc16Xmodem(const std::vector<BYTE>& data) {
    uint16_t crc = 0;
    for (BYTE b : data) {
        crc ^= (uint16_t)(b << 8);
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

struct Barracuda {
    Hid dev;
    BYTE reportId = 0x02;
    bool ysTech = true;
    BYTE seq = 0;

    // Writes one Airoha RACE frame through the receiver bridge. YS-Tech needs a CRC and a
    // per-report acknowledgement ('O' accepted, 'F' busy); Macronix takes the frame directly.
    bool WriteFrame(const std::vector<BYTE>& race) {
        if (ysTech) {
            std::vector<BYTE> frame(64, 0);
            frame[0] = reportId;
            frame[1] = 0x80;
            frame[2] = (BYTE)race.size();
            std::copy(race.begin(), race.end(), frame.begin() + 5);
            const uint16_t crc = Crc16Xmodem(frame);
            frame[3] = (BYTE)crc;
            frame[4] = (BYTE)(crc >> 8);

            bool accepted = false;
            for (int attempt = 0; attempt < 5 && !accepted; attempt++) {
                if (attempt) Sleep(40);
                if (!dev.WritePadded(frame)) return false;
                std::vector<BYTE> status;
                if (!dev.GetInputReport(reportId, status)) return false;
                if (status.size() > 1 && status[1] == 'O') accepted = true;
                else if (status.size() > 1 && status[1] != 'F') return false;
            }
            return accepted;
        }
        std::vector<BYTE> frame{ reportId, 0x80, (BYTE)race.size() };
        frame.insert(frame.end(), race.begin(), race.end());
        return dev.WritePadded(frame);
    }

    // Walk every 50 49 frame in one report: a report may carry several, plus unsolicited events.
    static bool ScanFrames(const std::vector<BYTE>& payload,
                           const std::function<bool(const std::vector<BYTE>&)>& match,
                           std::vector<BYTE>& out) {
        for (size_t pos = 0; pos + 10 <= payload.size();) {
            if (payload[pos] != 0x50 || payload[pos + 1] != 0x49) return false;
            const size_t len = 10 + (size_t)(payload[pos + 8] | (payload[pos + 9] << 8));
            if (pos + len > payload.size()) return false;
            std::vector<BYTE> frame(payload.begin() + pos, payload.begin() + pos + len);
            if (match(frame)) {
                out = std::move(frame);
                return true;
            }
            pos += len;
        }
        return false;
    }

    bool ReadFrame(int timeoutMs, const std::function<bool(const std::vector<BYTE>&)>& match, std::vector<BYTE>& out) {
        const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutMs;
        while (GetTickCount64() < deadline) {
            std::vector<BYTE> report;
            if (dev.Read(report, (int)(deadline - GetTickCount64())) <= 0) break;
            const int offset = ysTech ? 2 : 3;
            if ((int)report.size() <= offset) continue;
            if (report[0] != reportId) continue;
            if (!ysTech && report[1] != 0x80) continue;
            const int length = report[(size_t)offset - 1];
            if (length <= 0 || (size_t)offset + (size_t)length > report.size()) continue;
            std::vector<BYTE> payload(report.begin() + offset, report.begin() + offset + length);
            if (ScanFrames(payload, match, out)) return true;
        }
        return false;
    }

    // Sends [50 41 group seq body...] and waits for the matching reply.
    bool Command(BYTE group, const std::vector<BYTE>& body, int timeoutMs, std::vector<BYTE>& frame) {
        seq = (BYTE)(seq % 127 + 1);
        const BYTE mySeq = seq;
        std::vector<BYTE> race{ 0x50, 0x41, group, seq };
        race.insert(race.end(), body.begin(), body.end());
        if (!WriteFrame(race)) return false;
        return ReadFrame(timeoutMs, [group, mySeq](const std::vector<BYTE>& f) {
            return f.size() >= 12 && f[10] == group && (f[11] & 0x7F) == mySeq;
        }, frame);
    }

    bool RemoteMode(bool on) {
        std::vector<BYTE> frame;
        if (!Command(0x0E, { 0x02, 0xE1, (BYTE)(on ? 1 : 0) }, 800, frame)) return false;
        return frame.size() >= 13 && frame[12] == 0x00;
    }

    // Airoha customer-data read. 0x21 is the real battery percentage, 0x2A the cable state,
    // so the headset is asked for its own number instead of guessing from the voltage.
    int CustomerGet(BYTE cmd) {
        seq = (BYTE)(seq % 127 + 1);
        std::vector<BYTE> race{ 0x50, 0x41, 0x08, seq, 0x03, cmd, 0x00, 0x00 };
        if (!WriteFrame(race)) return -1;
        std::vector<BYTE> frame;
        const bool ok = ReadFrame(1000, [cmd](const std::vector<BYTE>& f) {
            return f.size() >= 14 && f[10] == cmd && f[11] == 0x01;
        }, frame);
        if (!ok) return -1;
        return frame[13];
    }

    bool EnterAppMode() {
        if (!dev.WritePadded({ reportId, 0x40 })) return false;
        const ULONGLONG deadline = GetTickCount64() + 700;
        while (GetTickCount64() < deadline) {
            std::vector<BYTE> reply;
            if (dev.Read(reply, (int)(deadline - GetTickCount64())) <= 0) break;
            if (reply.size() >= 4 && reply[0] == reportId && reply[1] == 0x40 && reply[2] == 0x01) return true;
        }
        return false;
    }
};

void PollRazer(const std::vector<HidInterface>& hid, std::vector<Reading>& out, std::wstring& log) {
    const HidInterface* best = nullptr;
    for (const auto& iface : hid) {
        if (iface.vid != kRazerVid) continue;
        if (std::find(std::begin(kBarracudaPids), std::end(kBarracudaPids), iface.pid) == std::end(kBarracudaPids)) continue;
        if (iface.usagePage != 0xFF00 || !iface.inLen || !iface.outLen) continue;
        if (!best) best = &iface;
        if (iface.inLen >= 64 && iface.outLen >= 64) { best = &iface; break; }
    }
    if (!best) return;

    Barracuda link;
    if (!link.dev.Open(best->path)) {
        log += L"Razer: dongle found but cannot be opened\n";
        return;
    }
    link.ysTech = best->pid != 0x0552;
    link.reportId = link.ysTech ? 0x02 : 0x01;
    if (!link.ysTech && !link.EnterAppMode()) {
        log += L"Razer: dongle did not enter app mode\n";
        return;
    }

    if (!link.RemoteMode(true)) {
        log += L"Razer: headset does not answer (off, out of range, or in Bluetooth mode)\n";
        Reading asleep;
        asleep.id = Fmt(L"razer:barracuda:%04X", best->pid);
        asleep.name = best->product.empty() ? L"Razer Barracuda X" : best->product;
        asleep.kind = Kind::Headset;
        asleep.via = Via2G;
        asleep.driver = L"razer";
        asleep.asleep = true;
        asleep.stamp = NowUnix();
        out.push_back(std::move(asleep));
        return;
    }

    const int level = link.CustomerGet(0x21);
    const int cable = level >= 0 ? link.CustomerGet(0x2A) : -1;
    link.RemoteMode(false);

    if (level < 0 || level > 100) {
        log += L"Razer: headset did not report a battery level\n";
        return;
    }

    Reading r;
    r.id = Fmt(L"razer:barracuda:%04X", best->pid);
    r.name = best->product.empty() ? L"Razer Barracuda X" : best->product;
    r.kind = Kind::Headset;
    r.via = Via2G;
    r.driver = L"razer";
    r.percent = level;
    // 0x2A is the cable / charging state: 0 means running on battery.
    r.charging = cable > 0;
    r.full = r.charging && r.percent >= 100;
    r.stamp = NowUnix();
    out.push_back(std::move(r));
}

// ---------------------------------------------------------------- Bluetooth (Windows battery)

const DEVPROPKEY kPkeyBtBattery = { { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2 };
const DEVPROPKEY kPkeyIsConnected = { { 0x83DA6326, 0x97A6, 0x4088, { 0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29 } }, 15 };
const DEVPROPKEY kPkeyFriendlyName = { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };
const DEVPROPKEY kPkeyContainerId = { { 0x8C7ED206, 0x3F8A, 0x4827, { 0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C } }, 2 };

std::wstring StripServiceName(std::wstring name) {
    static const wchar_t* suffixes[] = { L" Hands-Free AG", L" Hands-Free", L" Avrcp Transport",
                                         L" Stereo", L" Audio Gateway", L" A2DP" };
    for (const wchar_t* suffix : suffixes) {
        size_t len = wcslen(suffix);
        if (name.size() > len && name.compare(name.size() - len, len, suffix) == 0) {
            name.erase(name.size() - len);
            break;
        }
    }
    return Trim(name);
}

void PollBluetooth(std::vector<Reading>& out) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;

    struct Aggregate {
        std::wstring name;
        int percent = -1;
        bool connected = false;
        bool hasBattery = false;
        bool bluetoothLe = false;
    };
    std::map<std::wstring, Aggregate> byContainer;
    std::wstring containerOrder[64];
    int orderCount = 0;

    SP_DEVINFO_DATA did{};
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &did); i++) {
        wchar_t instance[512];
        if (!SetupDiGetDeviceInstanceIdW(set, &did, instance, 512, nullptr)) continue;
        if (wcsncmp(instance, L"BTHENUM\\", 8) && wcsncmp(instance, L"BTHLE\\", 6) &&
            wcsncmp(instance, L"BTHLEDEVICE\\", 12))
            continue;

        DEVPROPTYPE type = 0;
        GUID container{};
        std::wstring key = instance;
        if (SetupDiGetDevicePropertyW(set, &did, &kPkeyContainerId, &type, (PBYTE)&container,
                                      sizeof(container), nullptr, 0) &&
            type == DEVPROP_TYPE_GUID) {
            wchar_t guid[64];
            StringFromGUID2(container, guid, 64);
            key = guid;
        }

        wchar_t nameBuf[512] = {};
        if (SetupDiGetDevicePropertyW(set, &did, &kPkeyFriendlyName, &type, (PBYTE)nameBuf,
                                      sizeof(nameBuf) - sizeof(wchar_t), nullptr, 0) &&
            type == DEVPROP_TYPE_STRING) {
            std::wstring name = StripServiceName(nameBuf);
            auto& agg = byContainer[key];
            // Prefer the plain device name over a service node's longer name.
            if (agg.name.empty() || (name.size() < agg.name.size() && !name.empty())) agg.name = name;
        }

        DEVPROP_BOOLEAN connected = DEVPROP_FALSE;
        const bool haveConnectedFlag =
            SetupDiGetDevicePropertyW(set, &did, &kPkeyIsConnected, &type, (PBYTE)&connected,
                                      sizeof(connected), nullptr, 0) &&
            type == DEVPROP_TYPE_BOOLEAN;
        if (haveConnectedFlag && connected == DEVPROP_TRUE) byContainer[key].connected = true;
        if (!wcsncmp(instance, L"BTHLE\\", 6) || !wcsncmp(instance, L"BTHLEDEVICE\\", 12))
            byContainer[key].bluetoothLe = true;

        BYTE percent = 0;
        if (SetupDiGetDevicePropertyW(set, &did, &kPkeyBtBattery, &type, &percent, sizeof(percent),
                                      nullptr, 0) &&
            type == DEVPROP_TYPE_BYTE && percent <= 100) {
            auto& agg = byContainer[key];
            agg.percent = percent;
            agg.hasBattery = true;
            // Some LE nodes expose a level but no connected flag at all.
            if (!haveConnectedFlag) agg.connected = true;
        }
        if (byContainer.count(key) && orderCount < 64) {
            bool known = false;
            for (int k = 0; k < orderCount; k++)
                if (containerOrder[k] == key) { known = true; break; }
            if (!known) containerOrder[orderCount++] = key;
        }
    }
    SetupDiDestroyDeviceInfoList(set);

    for (int i = 0; i < orderCount; i++) {
        const auto& key = containerOrder[i];
        auto it = byContainer.find(key);
        if (it == byContainer.end()) continue;
        const Aggregate& agg = it->second;
        if (!agg.hasBattery || agg.percent < 0) continue;
        if (!agg.connected) continue;   // the headset is off or out of range
        Reading r;
        r.id = L"bt:" + key;
        r.name = agg.name.empty() ? L"Bluetooth device" : agg.name;
        r.kind = GuessKind(r.name);
        r.percent = agg.percent;
        r.charging = false;
        r.via = L"Bluetooth";
        r.driver = agg.bluetoothLe ? L"bt-le" : L"bt";
        r.stamp = NowUnix();
        out.push_back(std::move(r));
    }
}

// ---------------------------------------------------------------- XInput controllers

struct XInputBattery {
    BYTE type;
    BYTE level;
};

void PollXInput(std::vector<Reading>& out) {
    using GetBatteryFn = DWORD(WINAPI*)(DWORD, BYTE, XInputBattery*);
    HMODULE dll = LoadLibraryW(L"xinput1_4.dll");
    if (!dll) dll = LoadLibraryW(L"xinput1_3.dll");
    if (!dll) return;
    auto getBattery = (GetBatteryFn)GetProcAddress(dll, "XInputGetBatteryInformation");
    if (!getBattery) {
        FreeLibrary(dll);
        return;
    }
    for (DWORD index = 0; index < 4; index++) {
        XInputBattery battery{};
        if (getBattery(index, 0 /* BATTERY_DEVTYPE_GAMEPAD */, &battery) != ERROR_SUCCESS) continue;
        // 0 disconnected, 1 wired, 2 alkaline, 3 NiMH, 255 unknown.
        if (battery.type == 0 || battery.type == 1 || battery.type == 255) continue;
        int percent = -1;
        switch (battery.level) {
            case 0: percent = 5; break;     // empty
            case 1: percent = 20; break;    // low
            case 2: percent = 60; break;    // medium
            case 3: percent = 100; break;   // full
            default: break;
        }
        if (percent < 0) continue;
        Reading r;
        r.id = Fmt(L"xinput:%u", index);
        r.name = L"Xbox controller";
        r.kind = Kind::Gamepad;
        r.percent = percent;
        r.via = L"Wireless";
        r.driver = L"xinput";
        r.stamp = NowUnix();
        out.push_back(std::move(r));
    }
    FreeLibrary(dll);
}

} // namespace

PollReport PollDevices() {
    PollReport report;
    std::vector<HidInterface> hid = EnumerateHid();
    PollSteelSeries(hid, report.devices, report.log);
    PollRazer(hid, report.devices, report.log);
    PollBluetooth(report.devices);
    PollXInput(report.devices);

    // One entry per physical device, most specific driver first.
    std::vector<Reading> unique;
    std::set<std::wstring> ids;
    for (auto& r : report.devices) {
        if (!ids.insert(r.id).second) continue;
        unique.push_back(std::move(r));
    }
    report.devices = std::move(unique);
    return report;
}

std::wstring DumpHid() {
    std::wstring out = L"HID collections:\n";
    auto list = EnumerateHid(true);
    std::sort(list.begin(), list.end(), [](const HidInterface& a, const HidInterface& b) {
        if (a.vid != b.vid) return a.vid < b.vid;
        if (a.pid != b.pid) return a.pid < b.pid;
        return a.path < b.path;
    });
    for (const auto& i : list) {
        out += Fmt(L"  %04X:%04X  if %2d  page %04X usage %04X  in %3d out %3d feat %3d  %s\n",
                   i.vid, i.pid, i.mi, i.usagePage, i.usage, i.inLen, i.outLen, i.featLen,
                   i.product.c_str());
    }
    return out;
}

} // namespace ch
