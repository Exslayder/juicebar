#include "hid.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <cwchar>

namespace ch {
namespace {

std::wstring FromWideBuffer(const wchar_t* buf, size_t cap) {
    size_t n = 0;
    while (n < cap && buf[n]) n++;
    return std::wstring(buf, n);
}

int InterfaceNumberFromPath(const std::wstring& path) {
    // ...&mi_03#...   or  ...&col02#...
    const wchar_t* p = wcsstr(path.c_str(), L"&mi_");
    if (!p) return -1;
    return (int)wcstol(p + 4, nullptr, 16);
}

} // namespace

std::vector<HidInterface> EnumerateHid(bool all) {
    std::vector<HidInterface> result;
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return result;

    for (DWORD index = 0;; index++) {
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, index, &ifd)) break;

        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &needed, nullptr);
        if (!needed) continue;
        std::vector<BYTE> storage(needed);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, needed, nullptr, nullptr)) continue;

        HidInterface info;
        info.path = detail->DevicePath;
        if (!all) {
            // HID paths look like \\?\hid#vid_1038&pid_1838&mi_03&col01#...
            std::wstring lower = info.path;
            for (auto& ch2 : lower) ch2 = (wchar_t)towlower(ch2);
            if (lower.find(L"vid_1038") == std::wstring::npos && lower.find(L"vid_1532") == std::wstring::npos)
                continue;
        }
        info.mi = InterfaceNumberFromPath(info.path);

        HANDLE h = CreateFileW(info.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        if (HidD_GetAttributes(h, &attrs)) {
            info.vid = attrs.VendorID;
            info.pid = attrs.ProductID;
        }

        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                info.usagePage = caps.UsagePage;
                info.usage = caps.Usage;
                info.inLen = caps.InputReportByteLength;
                info.outLen = caps.OutputReportByteLength;
                info.featLen = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }

        wchar_t buf[512];
        if (HidD_GetProductString(h, buf, sizeof(buf)) && buf[0]) info.product = FromWideBuffer(buf, 512);
        if (HidD_GetManufacturerString(h, buf, sizeof(buf)) && buf[0]) info.manufacturer = FromWideBuffer(buf, 512);
        if (HidD_GetSerialNumberString(h, buf, sizeof(buf)) && buf[0]) info.serial = FromWideBuffer(buf, 512);
        CloseHandle(h);

        if (!info.vid) continue;
        result.push_back(std::move(info));
    }

    SetupDiDestroyDeviceInfoList(set);
    return result;
}

Hid::~Hid() { Close(); }

bool Hid::Open(const std::wstring& path) {
    Close();
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event_) {
        Close();
        return false;
    }

    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetPreparsedData(handle_, &pp)) {
        HIDP_CAPS caps{};
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
            inLen_ = caps.InputReportByteLength;
            outLen_ = caps.OutputReportByteLength;
        }
        HidD_FreePreparsedData(pp);
    }
    if (!inLen_ || !outLen_) {
        Close();
        return false;
    }
    return true;
}

void Hid::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
    inLen_ = outLen_ = 0;
}

bool Hid::Io(std::vector<BYTE>& buf, DWORD bytes, bool isWrite, int timeoutMs) {
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    ResetEvent(event_);
    OVERLAPPED ov{};
    ov.hEvent = event_;
    DWORD transferred = 0;

    BOOL ok = isWrite ? WriteFile(handle_, buf.data(), bytes, &transferred, &ov)
                      : ReadFile(handle_, buf.data(), bytes, &transferred, &ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(event_, (DWORD)timeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(handle_, &ov);
            WaitForSingleObject(event_, 100);
            return false;
        }
    }
    if (!GetOverlappedResult(handle_, &ov, &transferred, TRUE)) return false;
    if (isWrite) return transferred == bytes;
    buf.resize(transferred);
    return true;
}

bool Hid::WritePadded(const std::vector<BYTE>& data) {
    if (!ok() || !outLen_) return false;
    std::vector<BYTE> buf((size_t)outLen_, 0);
    if (data.size() > buf.size()) return false;
    memcpy(buf.data(), data.data(), data.size());
    return Io(buf, (DWORD)buf.size(), true, 500);
}

int Hid::Read(std::vector<BYTE>& out, int timeoutMs) {
    if (!ok() || !inLen_) return -1;
    std::vector<BYTE> buf((size_t)inLen_, 0);
    if (!Io(buf, (DWORD)buf.size(), false, timeoutMs)) return 0;
    out = std::move(buf);
    return (int)out.size();
}

bool Hid::GetInputReport(BYTE reportId, std::vector<BYTE>& buf) {
    if (!ok() || !inLen_) return false;
    buf.assign((size_t)inLen_, 0);
    buf[0] = reportId;
    return HidD_GetInputReport(handle_, buf.data(), (ULONG)buf.size()) != FALSE;
}

void Hid::Flush() {
    if (ok()) HidD_FlushQueue(handle_);
}

} // namespace ch
