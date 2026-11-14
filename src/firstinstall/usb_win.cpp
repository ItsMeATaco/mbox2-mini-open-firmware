#include "firstinstall/usb_win.h"

#include <cfgmgr32.h>
#include <setupapi.h>
#include <winioctl.h>
#include <initguid.h>  // defines GUID_DEVINTERFACE_USB_HUB in this translation unit
#include <usbiodef.h>
#undef LowSpeed  // winusbio.h's device-speed constant is a field name in usbioctl.h
#include <usbioctl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace fi {

const GUID kDfuInterfaceGuid = {0xcf5d7360, 0xe4f5, 0x49e8, {0x88, 0xc3, 0xb6, 0xc1, 0x05, 0xdc, 0x73, 0x28}};

namespace {

constexpr uint8_t kClassOut = 0x21;  // class request, interface recipient, host to device
constexpr uint8_t kClassIn = 0xA1;
constexpr ULONG kTimeoutMs = 5000;

std::wstring FindInterface(const GUID& guid, const std::wstring& instance) {
  GUID copy = guid;
  for (int attempt = 0; attempt < 3; ++attempt) {
    ULONG size = 0;
    if (CM_Get_Device_Interface_List_SizeW(&size, &copy, const_cast<wchar_t*>(instance.c_str()),
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        size <= 1) {
      return {};
    }
    std::wstring list(size, L'\0');
    CONFIGRET cr = CM_Get_Device_Interface_ListW(&copy, const_cast<wchar_t*>(instance.c_str()), list.data(), size,
                                                 CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr == CR_BUFFER_SMALL) continue;
    if (cr != CR_SUCCESS) return {};
    return list.c_str();
  }
  return {};
}

std::wstring MultiSz(HDEVINFO set, SP_DEVINFO_DATA* dev, DWORD property) {
  wchar_t buffer[2048] = {};
  if (!SetupDiGetDeviceRegistryPropertyW(set, dev, property, nullptr, reinterpret_cast<BYTE*>(buffer),
                                         sizeof buffer - 2 * sizeof(wchar_t), nullptr)) {
    return {};
  }
  return std::wstring(buffer, sizeof buffer / sizeof buffer[0]);
}

// Opens the parent hub of a USB device devnode; *port is the device's port on it.
HANDLE OpenHub(const std::wstring& device, ULONG* port, std::string* error) {
  DEVINST node = 0, hub = 0;
  if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(device.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS ||
      CM_Get_Parent(&hub, node, 0) != CR_SUCCESS) {
    *error = "device or its hub not found";
    return INVALID_HANDLE_VALUE;
  }
  ULONG size = sizeof *port;
  if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_ADDRESS, nullptr, port, &size, 0) != CR_SUCCESS) {
    *error = "no port number for the device";
    return INVALID_HANDLE_VALUE;
  }
  wchar_t hub_id[MAX_DEVICE_ID_LEN] = {};
  if (CM_Get_Device_IDW(hub, hub_id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
    *error = "hub ID unreadable";
    return INVALID_HANDLE_VALUE;
  }
  std::wstring path = FindInterface(GUID_DEVINTERFACE_USB_HUB, hub_id);
  if (path.empty()) {
    *error = "the hub has no interface to open";
    return INVALID_HANDLE_VALUE;
  }
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) *error = "opening the hub: " + Win32Text(GetLastError());
  return file;
}

bool DescriptorThroughHub(const std::wstring& device, uint8_t type, std::vector<uint8_t>* out, std::string* error) {
  ULONG port = 0;
  HANDLE hub = OpenHub(device, &port, error);
  if (hub == INVALID_HANDLE_VALUE) return false;
  auto read = [&](uint16_t length, std::vector<uint8_t>* data) {
    std::vector<uint8_t> buffer(sizeof(USB_DESCRIPTOR_REQUEST) + length);
    auto* request = reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(buffer.data());
    request->ConnectionIndex = port;
    request->SetupPacket.bmRequest = 0x80;
    request->SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    request->SetupPacket.wValue = static_cast<USHORT>(type << 8);
    request->SetupPacket.wLength = length;
    DWORD returned = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buffer.data(),
                         static_cast<DWORD>(buffer.size()), buffer.data(), static_cast<DWORD>(buffer.size()),
                         &returned, nullptr)) {
      *error = "descriptor request: " + Win32Text(GetLastError());
      return false;
    }
    size_t got = returned > sizeof(USB_DESCRIPTOR_REQUEST) ? returned - sizeof(USB_DESCRIPTOR_REQUEST) : 0;
    data->assign(buffer.begin() + sizeof(USB_DESCRIPTOR_REQUEST), buffer.begin() + sizeof(USB_DESCRIPTOR_REQUEST) + got);
    return true;
  };
  bool ok;
  if (type == USB_CONFIGURATION_DESCRIPTOR_TYPE) {
    ok = read(9, out) && out->size() >= 4;
    if (ok) {
      const uint16_t total = static_cast<uint16_t>((*out)[2] | (*out)[3] << 8);
      ok = read(std::min<uint16_t>(total, 4096), out);
    }
  } else {
    ok = read(18, out);
  }
  CloseHandle(hub);
  return ok;
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

std::string Narrow(const std::wstring& text) {
  std::string out;
  for (wchar_t c : text) out += c < 128 ? static_cast<char>(c) : '?';
  return out;
}

std::vector<UsbNode> FindNodes(const std::wstring& hardware_id) {
  std::vector<UsbNode> nodes;
  HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE) return nodes;
  SP_DEVINFO_DATA dev{sizeof dev};
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
    std::wstring ids = MultiSz(set, &dev, SPDRP_HARDWAREID);
    bool match = false;
    uint16_t revision = 0;
    for (const wchar_t* id = ids.c_str(); *id; id += wcslen(id) + 1) {
      if (_wcsicmp(id, hardware_id.c_str()) == 0) match = true;
      if (const wchar_t* rev = wcsstr(id, L"&REV_")) revision = static_cast<uint16_t>(wcstoul(rev + 5, nullptr, 16));
    }
    if (!match) continue;
    UsbNode node;
    wchar_t instance[MAX_DEVICE_ID_LEN] = {};
    SetupDiGetDeviceInstanceIdW(set, &dev, instance, MAX_DEVICE_ID_LEN, nullptr);
    node.instance = instance;
    node.service = MultiSz(set, &dev, SPDRP_SERVICE).c_str();
    node.revision = revision;
    ULONG status = 0;
    if (CM_Get_DevNode_Status(&status, &node.problem, dev.DevInst, 0) != CR_SUCCESS) node.problem = 0;
    node.has_interface = !FindInterface(kDfuInterfaceGuid, node.instance).empty();
    nodes.push_back(node);
  }
  SetupDiDestroyDeviceInfoList(set);
  return nodes;
}

std::wstring Parent(const std::wstring& instance) {
  DEVINST node = 0, parent = 0;
  wchar_t id[MAX_DEVICE_ID_LEN] = {};
  if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS ||
      CM_Get_Parent(&parent, node, 0) != CR_SUCCESS || CM_Get_Device_IDW(parent, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
    return {};
  }
  return id;
}

bool ConfigDescriptorThroughHub(const std::wstring& device, std::vector<uint8_t>* config, std::string* error) {
  return DescriptorThroughHub(device, USB_CONFIGURATION_DESCRIPTOR_TYPE, config, error);
}

bool DeviceDescriptorThroughHub(const std::wstring& device, std::vector<uint8_t>* descriptor, std::string* error) {
  return DescriptorThroughHub(device, USB_DEVICE_DESCRIPTOR_TYPE, descriptor, error);
}

bool CyclePort(const std::wstring& device, std::string* error) {
  ULONG port = 0;
  HANDLE hub = OpenHub(device, &port, error);
  if (hub == INVALID_HANDLE_VALUE) return false;
  USB_CYCLE_PORT_PARAMS params{};
  params.ConnectionIndex = port;
  DWORD returned = 0;
  BOOL ok = DeviceIoControl(hub, IOCTL_USB_HUB_CYCLE_PORT, &params, sizeof params, &params, sizeof params, &returned,
                            nullptr);
  if (!ok) *error = "IOCTL_USB_HUB_CYCLE_PORT on port " + std::to_string(port) + ": " + Win32Text(GetLastError());
  CloseHandle(hub);
  return ok != FALSE;
}

bool SetInterfaceGuid(const std::wstring& instance, std::string* error) {
  HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
  if (set == INVALID_HANDLE_VALUE) {
    *error = Win32Text(GetLastError());
    return false;
  }
  SP_DEVINFO_DATA dev{sizeof dev};
  bool ok = false;
  if (!SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &dev)) {
    *error = "opening the device: " + Win32Text(GetLastError());
  } else {
    HKEY key = SetupDiCreateDevRegKeyW(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
    if (key == INVALID_HANDLE_VALUE) {
      *error = "opening its Device Parameters key: " + Win32Text(GetLastError());
    } else {
      const GUID& g = kDfuInterfaceGuid;
      wchar_t value[64] = {};
      swprintf(value, 64, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3, g.Data4[0],
               g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
      const DWORD bytes = static_cast<DWORD>((wcslen(value) + 2) * sizeof(wchar_t));  // REG_MULTI_SZ: two NULs
      LSTATUS status = RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ,
                                      reinterpret_cast<const BYTE*>(value), bytes);
      ok = status == ERROR_SUCCESS;
      if (!ok) *error = "writing DeviceInterfaceGUIDs: " + Win32Text(status);
      RegCloseKey(key);
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  return ok;
}

bool RestartNode(const std::wstring& instance, std::string* error) {
  HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
  if (set == INVALID_HANDLE_VALUE) {
    *error = Win32Text(GetLastError());
    return false;
  }
  SP_DEVINFO_DATA dev{sizeof dev};
  SP_PROPCHANGE_PARAMS params{};
  params.ClassInstallHeader.cbSize = sizeof params.ClassInstallHeader;
  params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
  params.StateChange = DICS_PROPCHANGE;
  params.Scope = DICS_FLAG_CONFIGSPECIFIC;
  bool ok = SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &dev) &&
            SetupDiSetClassInstallParamsW(set, &dev, &params.ClassInstallHeader, sizeof params) &&
            SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev);
  if (!ok) *error = Win32Text(GetLastError());
  SetupDiDestroyDeviceInfoList(set);
  return ok;
}

void OpenProperties(const std::wstring& instance) {
  wchar_t system[MAX_PATH] = {};
  GetSystemDirectoryW(system, MAX_PATH);
  std::wstring command = std::wstring(L"\"") + system + L"\\rundll32.exe\" devmgr.dll,DeviceProperties_RunDLL /DeviceID \"" +
                         instance + L"\"";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

std::vector<std::string> UpdateModeDriverPackages(const std::wstring& folder_in, const wchar_t* pattern) {
  std::vector<std::string> found;
  std::wstring folder = folder_in;
  if (folder.empty()) {
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    folder = std::wstring(windows) + L"\\INF";
  }
  folder += L"\\";
  WIN32_FIND_DATAW entry;
  HANDLE search = FindFirstFileW((folder + pattern).c_str(), &entry);
  if (search == INVALID_HANDLE_VALUE) return found;
  do {
    HANDLE file = CreateFileW((folder + entry.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    std::vector<uint8_t> bytes(std::min<DWORD>(entry.nFileSizeLow, 4u << 20));
    DWORD got = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
    CloseHandle(file);
    bytes.resize(got);
    const bool utf16 = got >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE;  // else ANSI or UTF-8
    std::string text;
    for (size_t i = utf16 ? 2 : 0; i < bytes.size(); i += utf16 ? 2 : 1) {
      text += static_cast<char>(toupper(bytes[i]));
    }
    const bool claims = text.find("VID_0DBA&PID_3001") != std::string::npos ||
                        text.find("VID_0DBA&PID_3000&MI_00") != std::string::npos;
    if (claims && text.find("WINUSB.INF") == std::string::npos) found.push_back(Narrow(entry.cFileName));
  } while (FindNextFileW(search, &entry));
  FindClose(search);
  return found;
}

bool WinUsbDfu::Open(const std::wstring& instance, uint8_t interface_number, std::string* error) {
  Close();
  std::wstring path = FindInterface(kDfuInterfaceGuid, instance);
  if (path.empty()) {
    *error = "WinUSB hasn't registered the device interface";
    return false;
  }
  file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (file_ == INVALID_HANDLE_VALUE) {
    *error = "opening the device: " + Win32Text(GetLastError());
    return false;
  }
  if (!WinUsb_Initialize(file_, &usb_)) {
    *error = "WinUsb_Initialize: " + Win32Text(GetLastError());
    usb_ = nullptr;
    Close();
    return false;
  }
  ULONG timeout = kTimeoutMs;
  WinUsb_SetPipePolicy(usb_, 0, PIPE_TRANSFER_TIMEOUT, sizeof timeout, &timeout);
  interface_ = interface_number;
  return true;
}

void WinUsbDfu::Close() {
  if (usb_) {
    WinUsb_Free(usb_);
    usb_ = nullptr;
  }
  if (file_ != INVALID_HANDLE_VALUE) {
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
  }
}

bool WinUsbDfu::ConfigDescriptor(std::vector<uint8_t>* config, std::string* error) {
  uint8_t buffer[4096] = {};
  ULONG got = 0;
  if (!WinUsb_GetDescriptor(usb_, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0, buffer, sizeof buffer, &got)) {
    *error = Win32Text(GetLastError());
    return false;
  }
  config->assign(buffer, buffer + got);
  return true;
}

bool WinUsbDfu::Out(uint8_t request, uint16_t value, const uint8_t* data, uint16_t length, std::string* error) {
  if (!usb_) {
    *error = "not open";
    return false;
  }
  WINUSB_SETUP_PACKET setup{kClassOut, request, value, interface_, length};
  ULONG done = 0;
  if (!WinUsb_ControlTransfer(usb_, setup, const_cast<uint8_t*>(data), length, &done, nullptr)) {
    *error = Win32Text(GetLastError());
    return false;
  }
  if (done != length) {
    *error = "sent " + std::to_string(done) + " of " + std::to_string(length) + " bytes";
    return false;
  }
  return true;
}

bool WinUsbDfu::In(uint8_t request, uint16_t value, uint8_t* data, uint16_t length, uint16_t* got,
                   std::string* error) {
  if (!usb_) {
    *error = "not open";
    return false;
  }
  WINUSB_SETUP_PACKET setup{kClassIn, request, value, interface_, length};
  ULONG done = 0;
  if (!WinUsb_ControlTransfer(usb_, setup, data, length, &done, nullptr)) {
    *error = Win32Text(GetLastError());
    return false;
  }
  *got = static_cast<uint16_t>(done);
  return true;
}

}  // namespace fi
