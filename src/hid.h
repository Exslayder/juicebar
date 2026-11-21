#pragma once
#include "core.h"

namespace ch {

struct HidInterface {
    std::wstring path;
    uint16_t vid = 0, pid = 0;
    int mi = -1;              // interface number from the device path, -1 when unknown
    uint16_t usagePage = 0, usage = 0;
    int inLen = 0, outLen = 0, featLen = 0;
    std::wstring product, manufacturer, serial;
};

// Present HID collections (one entry per interface path). By default only the vendor IDs the
// app knows are opened: opening every HID device on the system can block for a long time on a
// device that is asleep or being removed, which used to leave the pill empty for a minute after
// a restart. Pass all=true for --dump.
std::vector<HidInterface> EnumerateHid(bool all = false);

class Hid {
public:
    Hid() = default;
    ~Hid();
    Hid(const Hid&) = delete;
    Hid& operator=(const Hid&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    bool ok() const { return handle_ != INVALID_HANDLE_VALUE; }

    // Pads `data` to the output report length and writes it.
    bool WritePadded(const std::vector<BYTE>& data);
    // Returns bytes read (>0), 0 on timeout, -1 on error. Also returns the timeout error as 0.
    int Read(std::vector<BYTE>& out, int timeoutMs);
    bool GetInputReport(BYTE reportId, std::vector<BYTE>& buf);
    void Flush();

    int inLen() const { return inLen_; }
    int outLen() const { return outLen_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    int inLen_ = 0, outLen_ = 0;
    bool Io(std::vector<BYTE>& buf, DWORD bytes, bool isWrite, int timeoutMs);
};

} // namespace ch
