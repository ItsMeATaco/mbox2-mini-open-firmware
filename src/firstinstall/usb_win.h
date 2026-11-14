// The Windows side of the first install: finding the Mbox in each of its states, getting Microsoft's WinUSB
// driver onto its DFU interfaces, DFU requests through WinUSB, and the hub requests (descriptors, port cycle).
//
// WinUSB goes on as Microsoft documents for a device with no driver of its own (learn.microsoft.com, "WinUSB
// (Winusb.sys) Installation for Developers"): the user picks "Universal Serial Bus devices > WinUsb Device" in
// Device Manager, and a DeviceInterfaceGUIDs value under the device's Device Parameters key gives programs a
// device interface to open. This writes that value first, so WinUSB registers the interface as soon as it loads;
// no self-signed certificate or custom INF is involved.
#pragma once

#include <windows.h>
#include <winusb.h>

#include <cstdint>
#include <string>
#include <vector>

#include "firstinstall/dfu.h"

namespace fi {

extern const GUID kDfuInterfaceGuid;  // {CF5D7360-E4F5-49E8-88C3-B6C105DC7328}

struct UsbNode {
  std::wstring instance;     // device instance ID
  std::wstring service;      // the driver bound to it ("WINUSB", "usbaudio", ...), empty if none
  ULONG problem = 0;         // CM_PROB_* (28 = no driver), 0 if none
  uint16_t revision = 0;     // REV_xxxx from its hardware IDs (bcdDevice)
  bool has_interface = false;  // WinUSB has registered kDfuInterfaceGuid on it
};

// Present devnodes with this exact hardware ID (e.g. L"USB\\VID_0DBA&PID_3000&MI_00"), case-insensitive.
std::vector<UsbNode> FindNodes(const std::wstring& hardware_id);
std::wstring Parent(const std::wstring& instance);

// Hub requests for a USB device devnode (not an interface child): no driver needed on the device.
bool ConfigDescriptorThroughHub(const std::wstring& device, std::vector<uint8_t>* config, std::string* error);
bool DeviceDescriptorThroughHub(const std::wstring& device, std::vector<uint8_t>* descriptor, std::string* error);
bool CyclePort(const std::wstring& device, std::string* error);  // IOCTL_USB_HUB_CYCLE_PORT (administrator)

// DeviceInterfaceGUIDs = kDfuInterfaceGuid under the devnode's Device Parameters key (administrator).
bool SetInterfaceGuid(const std::wstring& instance, std::string* error);
// Stops and starts the devnode's driver stack, so a WinUSB already bound re-reads that value.
bool RestartNode(const std::wstring& instance, std::string* error);
// Device Manager's properties dialog for the devnode (its Driver tab has "Update driver").
void OpenProperties(const std::wstring& instance);

// Driver packages that claim Avid's update interface or update mode (USB\VID_0DBA&PID_3000&MI_00, PID_3001) with
// a driver other than WinUSB: by default the driver store's copies, %windir%\INF\oem*.inf. Avid's dgmbx2fu.inf
// (service MBX2DFU) installs as one. With it there, Windows puts it on update mode by itself, and Device
// Manager's "Let me pick" then lists only drivers of its class (Universal Serial Bus controllers), not WinUSB
// (Universal Serial Bus devices), so it has to be uninstalled first. A WinUSB package (Zadig's) is fine.
std::vector<std::string> UpdateModeDriverPackages(const std::wstring& folder = {}, const wchar_t* pattern = L"oem*.inf");

class WinUsbDfu : public dfu::Transport {
 public:
  WinUsbDfu() = default;
  WinUsbDfu(const WinUsbDfu&) = delete;
  WinUsbDfu& operator=(const WinUsbDfu&) = delete;
  ~WinUsbDfu() override { Close(); }

  bool Open(const std::wstring& instance, uint8_t interface_number, std::string* error);
  void Close();
  bool ConfigDescriptor(std::vector<uint8_t>* config, std::string* error);

  bool Out(uint8_t request, uint16_t value, const uint8_t* data, uint16_t length, std::string* error) override;
  bool In(uint8_t request, uint16_t value, uint8_t* data, uint16_t length, uint16_t* got,
          std::string* error) override;
  void Sleep(uint32_t ms) override { ::Sleep(ms); }

 private:
  HANDLE file_ = INVALID_HANDLE_VALUE;
  WINUSB_INTERFACE_HANDLE usb_ = nullptr;
  uint8_t interface_ = 0;
};

std::string Win32Text(DWORD error);
std::string Narrow(const std::wstring& text);

}  // namespace fi
