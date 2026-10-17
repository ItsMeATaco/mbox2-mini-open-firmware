// The open firmware's maintenance channel (firmware/src/main.c, diag.c): vendor requests and diagnostic
// string descriptors.
//
// Interface 3 of the Mbox is a vendor interface with Microsoft OS descriptors, so Windows binds its own
// WinUSB driver to it with no INF (tools/firmware/winusb.py does the same from Python). String descriptors
// can also be read through the USB hub the Mbox is plugged into, which needs no driver on the Mbox at all:
// that keeps the counters readable when Windows didn't load the maintenance interface.
#pragma once

#include <windows.h>
#include <winusb.h>

#include <cstdint>
#include <string>

class MaintenanceLink {
 public:
  static const GUID kInterfaceGuid;  // {74C63992-EFBC-429E-9653-C0BAA33F1EBC}, firmware/src/descriptors.c

  // Firmware vendor requests (device recipient).
  static constexpr uint8_t kPeek = 0x01;      // IN: wLength bytes of external data memory from wValue
  static constexpr uint8_t kPoke = 0x02;      // no data: low byte of wIndex to external data address wValue
  static constexpr uint8_t kClockStat = 0x09; // IN, 12 bytes: frame number, MCLK capture, N trim, lock phase,
                                               // sample rate (24 bits), streams; all LE (up to v0.23: the first 8)

  MaintenanceLink() = default;
  MaintenanceLink(const MaintenanceLink&) = delete;
  MaintenanceLink& operator=(const MaintenanceLink&) = delete;
  ~MaintenanceLink() { Close(); }

  // ERROR_NOT_FOUND when Windows has no maintenance interface for a present Mbox; ERROR_ACCESS_DENIED or
  // ERROR_SHARING_VIOLATION when another program (such as usbeeprom.py) has it open.
  DWORD Open(ULONG timeout_ms = 1000);
  void Close();
  bool IsOpen() const { return usb_ != nullptr; }

  DWORD ControlIn(uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t length, ULONG* got);
  DWORD ControlOut(uint8_t request, uint16_t value, uint16_t index);  // no data stage
  DWORD Peek(uint16_t address, uint8_t* data, uint16_t length = 1);
  DWORD Poke(uint16_t address, uint8_t value);
  DWORD ReadString(uint8_t index, std::wstring* text);  // US English string descriptor
  DWORD SetTimeout(ULONG timeout_ms);

 private:
  HANDLE file_ = INVALID_HANDLE_VALUE;
  WINUSB_INTERFACE_HANDLE usb_ = nullptr;
};

// Reads a string descriptor of the USB device `device_instance` (e.g. USB\VID_1209&PID_0001\5&...)
// through its parent hub, as USBView does.
DWORD ReadStringThroughHub(const std::wstring& device_instance, uint8_t index, std::wstring* text);

std::string Win32Text(DWORD error);
