// Identity of the Mbox 2 Mini ASIO driver: its COM class, the names DAWs show and where it logs.
#pragma once

#include <guiddef.h>

namespace mbox2::asio {

// {5FE5CF79-6F37-4AE6-8241-93CD35CD733F}
inline constexpr GUID kClsid = {0x5fe5cf79, 0x6f37, 0x4ae6, {0x82, 0x41, 0x93, 0xcd, 0x35, 0xcd, 0x73, 0x3f}};
inline constexpr wchar_t kClsidString[] = L"{5FE5CF79-6F37-4AE6-8241-93CD35CD733F}";

// The key under HKLM\SOFTWARE\ASIO, which is the name DAWs list. Steinberg's guidelines keep "ASIO" out of
// product names.
inline constexpr wchar_t kRegistryName[] = L"Mbox 2 Mini (open firmware)";
// getDriverName(): at most 31 characters.
inline constexpr char kDriverName[] = "Mbox 2 Mini";
inline constexpr long kDriverVersion = 1;

// Both endpoints carry this in their names ("Speakers (Mbox 2 Mini)", "Microphone (Mbox 2 Mini)").
inline constexpr wchar_t kDeviceName[] = L"Mbox 2 Mini";

// Per-user settings and the log folder (%LOCALAPPDATA%\Mbox2Asio\driver.log).
inline constexpr wchar_t kSettingsKey[] = L"Software\\Mbox2Asio";
inline constexpr wchar_t kLogFolder[] = L"Mbox2Asio";

}  // namespace mbox2::asio
