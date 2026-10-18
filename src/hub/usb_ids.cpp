#include "hub/usb_ids.h"

#include <initguid.h>  // defines DEVPKEY_Device_BusReportedDeviceDesc in this translation unit
#include <devpkey.h>

#include <cwchar>

bool HasOpenFirmwareId(const std::wstring& device_id, int interface_number) {
  for (const UsbId& id : kOpenFirmwareIds) {
    wchar_t prefix[40];
    if (interface_number < 0) {
      swprintf_s(prefix, L"USB\\VID_%04X&PID_%04X\\", id.vendor, id.product);
    } else {
      swprintf_s(prefix, L"USB\\VID_%04X&PID_%04X&MI_%02X\\", id.vendor, id.product, interface_number);
    }
    if (_wcsnicmp(device_id.c_str(), prefix, wcslen(prefix)) == 0) return true;
  }
  return false;
}

bool IsOpenFirmwareMbox(DEVINST device) {
  wchar_t id[MAX_DEVICE_ID_LEN] = {};
  if (CM_Get_Device_IDW(device, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS || !HasOpenFirmwareId(id)) return false;
  wchar_t name[128] = {};
  DEVPROPTYPE type = 0;
  ULONG size = sizeof name - sizeof(wchar_t);  // keeps the last character a terminator
  return CM_Get_DevNode_PropertyW(device, &DEVPKEY_Device_BusReportedDeviceDesc, &type,
                                  reinterpret_cast<BYTE*>(name), &size, 0) == CR_SUCCESS &&
         type == DEVPROP_TYPE_STRING && _wcsicmp(name, kOpenFirmwareProduct) == 0;
}
