"""Control requests to the Mbox open firmware from Windows, through its maintenance interface.

Firmware v0.19 and later have a vendor interface (3) with Microsoft OS 1.0 descriptors, so Windows binds
its own WinUSB driver to it: no INF, no driver install, nothing to sign, and usbaudio.sys keeps the audio
interfaces. Its device interface GUID (descriptors.c) is how this module finds it. Standard library only
(ctypes on setupapi.dll and winusb.dll); fwdebug.Mbox uses it when it runs on Windows.
"""
import ctypes
import uuid
from ctypes import wintypes

MBOX_GUID = "{74C63992-EFBC-429E-9653-C0BAA33F1EBC}"

DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x02, 0x10
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL, FILE_FLAG_OVERLAPPED = 0x80, 0x40000000
PIPE_TRANSFER_TIMEOUT = 0x03
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID), ("Flags", wintypes.DWORD),
                ("Reserved", ctypes.c_size_t)]


class WINUSB_SETUP_PACKET(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("RequestType", ctypes.c_ubyte), ("Request", ctypes.c_ubyte), ("Value", ctypes.c_ushort),
                ("Index", ctypes.c_ushort), ("Length", ctypes.c_ushort)]


setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
winusb = ctypes.WinDLL("winusb", use_last_error=True)

setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID),
                                                 wintypes.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [ctypes.c_void_p, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
                                                      ctypes.c_void_p, wintypes.DWORD,
                                                      ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
kernel32.CreateFileW.restype = ctypes.c_void_p
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
winusb.WinUsb_Initialize.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
winusb.WinUsb_Free.argtypes = [ctypes.c_void_p]
winusb.WinUsb_SetPipePolicy.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, wintypes.ULONG, wintypes.ULONG,
                                        ctypes.c_void_p]
winusb.WinUsb_ControlTransfer.argtypes = [ctypes.c_void_p, WINUSB_SETUP_PACKET, ctypes.c_void_p, wintypes.ULONG,
                                          ctypes.POINTER(wintypes.ULONG), ctypes.c_void_p]
winusb.WinUsb_GetDescriptor.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_ubyte, ctypes.c_ushort,
                                        ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG)]


def _error(what):
    code = ctypes.get_last_error()
    return OSError(code, f"{what}: {ctypes.FormatError(code).strip()} (error {code})")


def device_paths(guid=MBOX_GUID):
    g = GUID.from_buffer_copy(uuid.UUID(guid).bytes_le)
    info = setupapi.SetupDiGetClassDevsW(ctypes.byref(g), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if info in (None, INVALID_HANDLE_VALUE):
        raise _error("SetupDiGetClassDevs")
    paths = []
    try:
        i = 0
        while True:
            data = SP_DEVICE_INTERFACE_DATA(cbSize=ctypes.sizeof(SP_DEVICE_INTERFACE_DATA))
            if not setupapi.SetupDiEnumDeviceInterfaces(info, None, ctypes.byref(g), i, ctypes.byref(data)):
                break
            i += 1
            size = wintypes.DWORD()
            setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(data), None, 0, ctypes.byref(size), None)
            buf = ctypes.create_string_buffer(size.value)
            # SP_DEVICE_INTERFACE_DETAIL_DATA_W: DWORD cbSize, then the path; cbSize is 8 on 64-bit Windows
            ctypes.c_uint32.from_buffer(buf).value = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            if not setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(data), buf, size, None, None):
                raise _error("SetupDiGetDeviceInterfaceDetail")
            paths.append(ctypes.wstring_at(ctypes.addressof(buf) + 4))
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(info)
    return paths


class Device:
    """One WinUSB interface; control() has the same shape as fwdebug.Mbox.control() on Linux."""

    def __init__(self, path, timeout_ms=1000):
        self.file = kernel32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, None)
        if self.file in (None, INVALID_HANDLE_VALUE):
            raise _error(f"opening {path}")
        self.handle = ctypes.c_void_p()
        if not winusb.WinUsb_Initialize(self.file, ctypes.byref(self.handle)):
            err = _error("WinUsb_Initialize")
            kernel32.CloseHandle(self.file)
            raise err
        t = wintypes.ULONG(timeout_ms)
        winusb.WinUsb_SetPipePolicy(self.handle, 0, PIPE_TRANSFER_TIMEOUT, ctypes.sizeof(t), ctypes.byref(t))

    def close(self):
        if self.handle:
            winusb.WinUsb_Free(self.handle)
            kernel32.CloseHandle(self.file)
            self.handle = None

    def control(self, request_type, request, value=0, index=0, length=0, data=b""):
        if data:
            length = len(data)
        buf = (ctypes.c_ubyte * max(length, 1))(*data)
        got = wintypes.ULONG()
        if request_type == 0x80 and request == 6:   # GET_DESCRIPTOR goes through WinUSB's own call
            ok = winusb.WinUsb_GetDescriptor(self.handle, value >> 8, value & 0xFF, index, buf, length,
                                             ctypes.byref(got))
        else:
            setup = WINUSB_SETUP_PACKET(request_type, request, value, index, length)
            ok = winusb.WinUsb_ControlTransfer(self.handle, setup, buf, length, ctypes.byref(got), None)
        if not ok:
            raise _error(f"control request {request_type:02X} {request:02X}")
        return bytes(buf[:got.value]) if request_type & 0x80 else b""


def open_mbox():
    paths = device_paths()
    if not paths:
        raise SystemExit("no Mbox maintenance interface: needs the open firmware v0.19 or later on this PC's USB "
                         "(Device Manager shows it as \"Mbox 2 Mini maintenance\" under Universal Serial Bus devices)")
    return Device(paths[0])
