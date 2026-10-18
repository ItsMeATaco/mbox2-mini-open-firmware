// How the hub recognizes the Mbox with the open firmware on USB.
//
// The firmware's USB ID is pid.codes' shared test ID 1209:0001 (firmware/src/descriptors.c), which other
// people's test gadgets carry too, so a match also needs the product name the firmware reports. A later
// firmware moves to a PID of its own: the list keeps every ID a release has used, so the hub still finds
// the Mbox after its usual USB update switches the firmware to the other one.
#pragma once

#include <windows.h>
#include <cfgmgr32.h>

#include <cstdint>
#include <string>

struct UsbId {
  uint16_t vendor, product;
};

inline constexpr UsbId kOpenFirmwareIds[] = {
    {0x1209, 0x0001},  // pid.codes' test PID: firmware up to v0.26
    // {0x1209, 0x4D32},  // requested from pid.codes for this project; add it once granted
};

// The firmware's product string (iProduct), which Windows keeps as the device's bus-reported description.
inline constexpr wchar_t kOpenFirmwareProduct[] = L"Mbox 2 Mini";

// Whether the device instance ID `device_id` (any case) carries one of kOpenFirmwareIds: the USB device
// itself (USB\VID_1209&PID_0001\...), or with `interface_number` that interface of it (...&MI_00\...).
bool HasOpenFirmwareId(const std::wstring& device_id, int interface_number = -1);

// Whether the USB device node `device`, the composite device rather than one of its interfaces, is the Mbox
// with the open firmware: one of its IDs, and its product name.
bool IsOpenFirmwareMbox(DEVINST device);
