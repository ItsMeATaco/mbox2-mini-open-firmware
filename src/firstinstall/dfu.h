// USB DFU 1.0/1.1 download (the USB Device Firmware Upgrade specification), over any control-transfer transport.
//
// Used to write the open firmware through Avid's own update mode on a stock Mbox 2 Mini: its firmware has a DFU
// interface (runtime 0DBA:3000, interface 0, class FE/01/00, functional descriptor 07 21 05 FF FF 08 00:
// download only, manifestation tolerant, detach timeout 0xFFFF, 8-byte transfers) and re-enumerates as 0DBA:3001
// in update mode. No Windows code here, so dfu_test.cpp can run it against a simulated device.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dfu {

// Class requests (bmRequestType 0x21 out / 0xA1 in, wIndex = interface).
enum Request : uint8_t { kDetach = 0, kDnload = 1, kUpload = 2, kGetStatus = 3, kClrStatus = 4, kGetState = 5,
                         kAbort = 6 };

enum State : uint8_t {
  kAppIdle = 0, kAppDetach = 1, kDfuIdle = 2, kDnloadSync = 3, kDnBusy = 4, kDnloadIdle = 5, kManifestSync = 6,
  kManifest = 7, kManifestWaitReset = 8, kUploadIdle = 9, kDfuError = 10,
};

struct Status {
  uint8_t status = 0;      // 0 = OK, else an error code (StatusName)
  uint32_t poll_ms = 0;    // bwPollTimeout: wait this long before the next GETSTATUS
  uint8_t state = 0;
  uint8_t string = 0;
};

const char* StateName(uint8_t state);
const char* StatusName(uint8_t status);

// The DFU functional descriptor (type 0x21) of the first DFU interface (class FE, subclass 01) in a configuration
// descriptor.
struct Functional {
  bool found = false;
  uint8_t interface_number = 0;
  uint8_t protocol = 0;          // 0 in DFU 1.0; 1 runtime / 2 DFU mode in DFU 1.1
  uint8_t attributes = 0;        // bit 0 download, 1 upload, 2 manifestation tolerant, 3 will detach
  uint16_t detach_timeout_ms = 0;
  uint16_t transfer_size = 0;
  uint16_t dfu_version = 0;      // bcdDFUVersion, 0 when the descriptor is the 7-byte DFU 1.0 one
};
Functional FindFunctional(const uint8_t* config, size_t length);

// What the download needs from the USB side: class requests to the DFU interface. Each returns false and fills
// *error when the transfer fails (a stall, or the device gone).
class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool Out(uint8_t request, uint16_t value, const uint8_t* data, uint16_t length, std::string* error) = 0;
  virtual bool In(uint8_t request, uint16_t value, uint8_t* data, uint16_t length, uint16_t* got,
                  std::string* error) = 0;
  virtual void Sleep(uint32_t ms) = 0;
};

bool GetStatus(Transport& usb, Status* status, std::string* error);

// DFU_DETACH to a runtime interface (wValue = timeout in ms).
bool Detach(Transport& usb, uint16_t timeout_ms, std::string* error);

struct DownloadResult {
  bool ok = false;
  bool left_in_manifest = false;  // the device stopped answering after the last block: it may have reset itself
  uint8_t final_state = 0;
  std::string error;
};

// Writes `image` to a device in DFU mode: brings it to dfuIDLE (CLRSTATUS / ABORT), sends `transfer_size`-byte
// blocks numbered from 0, polls GETSTATUS through dfuDNBUSY after each, then the zero-length block and polls
// through manifestation. Stops at the first error status, unexpected state or failed transfer. `progress` gets
// (bytes sent, total); `log` gets one line per notable event.
DownloadResult Download(Transport& usb, const std::vector<uint8_t>& image, uint16_t transfer_size,
                        const std::function<void(size_t, size_t)>& progress,
                        const std::function<void(const std::string&)>& log);

}  // namespace dfu
