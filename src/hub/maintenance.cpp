#include "hub/maintenance.h"

#include <cfgmgr32.h>
#include <winioctl.h>
#include <initguid.h>  // defines GUID_DEVINTERFACE_USB_HUB in this translation unit
#include <usbiodef.h>
#undef LowSpeed  // winusbio.h's device-speed constant is a field name in usbioctl.h
#include <usbioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

const GUID MaintenanceLink::kInterfaceGuid = {
    0x74c63992, 0xefbc, 0x429e, {0x96, 0x53, 0xc0, 0xba, 0xa3, 0x3f, 0x1e, 0xbc}};

namespace {

constexpr uint8_t kVendorIn = 0xC0;   // vendor, device recipient, device to host
constexpr uint8_t kVendorOut = 0x40;  // vendor, device recipient, host to device
constexpr USHORT kEnglish = 0x0409;
constexpr ULONG kStringMax = 255;

// The first present interface with this GUID, optionally limited to one device instance.
std::wstring FindInterface(const GUID& guid, const wchar_t* device_instance) {
  GUID copy = guid;
  for (int attempt = 0; attempt < 3; ++attempt) {
    ULONG size = 0;
    if (CM_Get_Device_Interface_List_SizeW(&size, &copy, const_cast<wchar_t*>(device_instance),
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        size <= 1) {
      return {};
    }
    std::wstring list(size, L'\0');
    CONFIGRET cr = CM_Get_Device_Interface_ListW(&copy, const_cast<wchar_t*>(device_instance), list.data(), size,
                                                 CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr == CR_BUFFER_SMALL) continue;  // a device arrived between the two calls
    if (cr != CR_SUCCESS) return {};
    return list.c_str();
  }
  return {};
}

// bLength, bDescriptorType 3, then UTF-16LE text.
std::wstring StringFromDescriptor(const uint8_t* data, ULONG got) {
  if (got < 2 || data[1] != 3) return {};
  ULONG length = std::min<ULONG>(data[0], got);
  std::wstring text;
  for (ULONG i = 2; i + 1 < length; i += 2) text += static_cast<wchar_t>(data[i] | (data[i + 1] << 8));
  return text;
}

}  // namespace

std::string Win32Text(DWORD error) {
  char* buffer = nullptr;
  DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
      0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string text = length ? std::string(buffer, length) : "unknown error";
  if (buffer) LocalFree(buffer);
  while (!text.empty() && strchr("\r\n .", text.back())) text.pop_back();
  return text + " (" + std::to_string(error) + ")";
}

DWORD MaintenanceLink::Open(ULONG timeout_ms) {
  Close();
  std::wstring path = FindInterface(kInterfaceGuid, nullptr);
  if (path.empty()) return ERROR_NOT_FOUND;
  file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (file_ == INVALID_HANDLE_VALUE) return GetLastError();
  if (!WinUsb_Initialize(file_, &usb_)) {
    DWORD error = GetLastError();
    usb_ = nullptr;
    Close();
    return error;
  }
  return SetTimeout(timeout_ms);
}

void MaintenanceLink::Close() {
  if (usb_) {
    WinUsb_Free(usb_);
    usb_ = nullptr;
  }
  if (file_ != INVALID_HANDLE_VALUE) {
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
  }
}

DWORD MaintenanceLink::SetTimeout(ULONG timeout_ms) {
  if (!usb_) return ERROR_INVALID_HANDLE;
  return WinUsb_SetPipePolicy(usb_, 0, PIPE_TRANSFER_TIMEOUT, sizeof timeout_ms, &timeout_ms) ? ERROR_SUCCESS
                                                                                              : GetLastError();
}

DWORD MaintenanceLink::ControlIn(uint8_t request, uint16_t value, uint16_t index, void* data, uint16_t length,
                                 ULONG* got) {
  if (!usb_) return ERROR_INVALID_HANDLE;
  WINUSB_SETUP_PACKET setup{kVendorIn, request, value, index, length};
  ULONG done = 0;
  BOOL ok = WinUsb_ControlTransfer(usb_, setup, static_cast<PUCHAR>(data), length, &done, nullptr);
  if (got) *got = done;
  return ok ? ERROR_SUCCESS : GetLastError();
}

DWORD MaintenanceLink::ControlOut(uint8_t request, uint16_t value, uint16_t index) {
  if (!usb_) return ERROR_INVALID_HANDLE;
  WINUSB_SETUP_PACKET setup{kVendorOut, request, value, index, 0};
  ULONG done = 0;
  return WinUsb_ControlTransfer(usb_, setup, nullptr, 0, &done, nullptr) ? ERROR_SUCCESS : GetLastError();
}

DWORD MaintenanceLink::Peek(uint16_t address, uint8_t* data, uint16_t length) {
  ULONG got = 0;
  DWORD error = ControlIn(kPeek, address, 0, data, length, &got);
  if (!error && got != length) error = ERROR_BAD_LENGTH;
  return error;
}

DWORD MaintenanceLink::Poke(uint16_t address, uint8_t value) { return ControlOut(kPoke, address, value); }

DWORD MaintenanceLink::ReadString(uint8_t index, std::wstring* text) {
  if (!usb_) return ERROR_INVALID_HANDLE;
  uint8_t buffer[kStringMax] = {};
  ULONG got = 0;
  if (!WinUsb_GetDescriptor(usb_, USB_STRING_DESCRIPTOR_TYPE, index, kEnglish, buffer, kStringMax, &got)) {
    return GetLastError();
  }
  *text = StringFromDescriptor(buffer, got);
  return ERROR_SUCCESS;
}

DWORD ReadStringThroughHub(const std::wstring& device_instance, uint8_t index, std::wstring* text) {
  DEVINST device = 0, hub = 0;
  if (CM_Locate_DevNodeW(&device, const_cast<wchar_t*>(device_instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) !=
          CR_SUCCESS ||
      CM_Get_Parent(&hub, device, 0) != CR_SUCCESS) {
    return ERROR_NOT_FOUND;
  }
  DWORD port = 0;
  ULONG size = sizeof port;
  if (CM_Get_DevNode_Registry_PropertyW(device, CM_DRP_ADDRESS, nullptr, &port, &size, 0) != CR_SUCCESS) {
    return ERROR_NOT_FOUND;
  }
  wchar_t hub_id[MAX_DEVICE_ID_LEN] = {};
  if (CM_Get_Device_IDW(hub, hub_id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return ERROR_NOT_FOUND;
  std::wstring hub_path = FindInterface(GUID_DEVINTERFACE_USB_HUB, hub_id);
  if (hub_path.empty()) return ERROR_NOT_FOUND;

  HANDLE file = CreateFileW(hub_path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return GetLastError();
  std::vector<uint8_t> buffer(sizeof(USB_DESCRIPTOR_REQUEST) + kStringMax);
  auto* request = reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(buffer.data());
  request->ConnectionIndex = port;
  request->SetupPacket.bmRequest = 0x80;
  request->SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
  request->SetupPacket.wValue = static_cast<USHORT>((USB_STRING_DESCRIPTOR_TYPE << 8) | index);
  request->SetupPacket.wIndex = kEnglish;
  request->SetupPacket.wLength = static_cast<USHORT>(kStringMax);
  DWORD returned = 0;
  BOOL ok = DeviceIoControl(file, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buffer.data(),
                            static_cast<DWORD>(buffer.size()), buffer.data(), static_cast<DWORD>(buffer.size()),
                            &returned, nullptr);
  DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (error) return error;
  constexpr DWORD kHeader = offsetof(USB_DESCRIPTOR_REQUEST, Data);
  if (returned < kHeader) return ERROR_BAD_LENGTH;
  *text = StringFromDescriptor(request->Data, returned - kHeader);
  return ERROR_SUCCESS;
}
