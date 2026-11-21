#pragma once
#include "core.h"

namespace ch {

struct PollReport {
    std::vector<Reading> devices;
    std::wstring log;   // errors worth showing in --once output
};

// Queries every supported device. Blocking (can take a couple of seconds): call from a worker thread.
PollReport PollDevices();

// Human readable list of HID collections, for --dump (helps adding new devices).
std::wstring DumpHid();

} // namespace ch
