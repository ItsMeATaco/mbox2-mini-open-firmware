#include "firstinstall/dfu.h"

#include <algorithm>

namespace dfu {
namespace {

// Limits on how long the device may stay busy, counted in requested poll time plus a cap on GETSTATUS calls (so
// they also hold against a simulated device, where no time passes).
constexpr uint32_t kBlockLimitMs = 10000;
constexpr uint32_t kManifestLimitMs = 60000;
constexpr int kPollLimit = 20000;

std::string Name(const char* (*f)(uint8_t), uint8_t value) { return f(value); }

// After a DNLOAD block: GETSTATUS until dfuDNLOAD-IDLE, waiting bwPollTimeout between polls.
bool WaitBlock(Transport& usb, Status* status, std::string* error) {
  uint32_t waited = 0;
  for (int polls = 0;; ++polls) {
    if (!GetStatus(usb, status, error)) return false;
    if (status->status != 0) {
      *error = Name(StatusName, status->status) + " in " + Name(StateName, status->state);
      return false;
    }
    if (status->state == kDnloadIdle) return true;
    if (status->state != kDnBusy && status->state != kDnloadSync) {
      *error = "unexpected state " + Name(StateName, status->state);
      return false;
    }
    if (waited > kBlockLimitMs || polls > kPollLimit) {
      *error = "still busy after " + std::to_string(waited) + " ms";
      return false;
    }
    uint32_t wait = std::max<uint32_t>(status->poll_ms, 1);
    usb.Sleep(wait);
    waited += wait;
  }
}

}  // namespace

const char* StateName(uint8_t state) {
  static const char* const names[] = {"appIDLE",         "appDETACH",   "dfuIDLE",
                                      "dfuDNLOAD-SYNC",  "dfuDNBUSY",   "dfuDNLOAD-IDLE",
                                      "dfuMANIFEST-SYNC", "dfuMANIFEST", "dfuMANIFEST-WAIT-RESET",
                                      "dfuUPLOAD-IDLE",  "dfuERROR"};
  return state < std::size(names) ? names[state] : "unknown state";
}

const char* StatusName(uint8_t status) {
  static const char* const names[] = {"OK",          "errTARGET",   "errFILE",   "errWRITE",
                                      "errERASE",    "errCHECK_ERASED", "errPROG", "errVERIFY",
                                      "errADDRESS",  "errNOTDONE",  "errFIRMWARE", "errVENDOR",
                                      "errUSBR",     "errPOR",      "errUNKNOWN", "errSTALLEDPKT"};
  return status < std::size(names) ? names[status] : "unknown status";
}

Functional FindFunctional(const uint8_t* config, size_t length) {
  Functional f;
  bool in_dfu = false;
  uint8_t number = 0, protocol = 0;
  for (size_t i = 0; i + 2 <= length;) {
    const uint8_t size = config[i], type = config[i + 1];
    if (size < 2 || i + size > length) break;
    if (type == 4 && size >= 9) {  // interface: class FE (application specific), subclass 01 (DFU)
      in_dfu = config[i + 5] == 0xFE && config[i + 6] == 0x01;
      number = config[i + 2];
      protocol = config[i + 7];
    } else if (type == 0x21 && in_dfu && size >= 7) {  // 0x21 is also HID's type: only inside a DFU interface
      f.found = true;
      f.interface_number = number;
      f.protocol = protocol;
      f.attributes = config[i + 2];
      f.detach_timeout_ms = static_cast<uint16_t>(config[i + 3] | config[i + 4] << 8);
      f.transfer_size = static_cast<uint16_t>(config[i + 5] | config[i + 6] << 8);
      f.dfu_version = size >= 9 ? static_cast<uint16_t>(config[i + 7] | config[i + 8] << 8) : 0;
      return f;
    }
    i += size;
  }
  return f;
}

bool GetStatus(Transport& usb, Status* status, std::string* error) {
  uint8_t data[6] = {};
  uint16_t got = 0;
  if (!usb.In(kGetStatus, 0, data, sizeof data, &got, error)) return false;
  if (got < 6) {
    *error = "GETSTATUS returned " + std::to_string(got) + " bytes";
    return false;
  }
  status->status = data[0];
  status->poll_ms = data[1] | data[2] << 8 | data[3] << 16;
  status->state = data[4];
  status->string = data[5];
  return true;
}

bool Detach(Transport& usb, uint16_t timeout_ms, std::string* error) {
  return usb.Out(kDetach, timeout_ms, nullptr, 0, error);
}

DownloadResult Download(Transport& usb, const std::vector<uint8_t>& image, uint16_t transfer_size,
                        const std::function<void(size_t, size_t)>& progress,
                        const std::function<void(const std::string&)>& log) {
  DownloadResult r;
  auto fail = [&r](std::string error) {
    r.error = std::move(error);
    return r;
  };
  if (transfer_size == 0) return fail("the device reports a transfer size of 0");
  if (image.empty()) return fail("empty image");

  // Start from dfuIDLE.
  Status s;
  std::string error;
  if (!GetStatus(usb, &s, &error)) return fail("GETSTATUS: " + error);
  log(std::string("update mode reports ") + StateName(s.state) + ", status " + StatusName(s.status));
  if (s.state == kAppIdle || s.state == kAppDetach) {
    return fail(std::string("the device is in its normal mode (") + StateName(s.state) + "), not in update mode");
  }
  if (s.state == kDfuError) {
    if (!usb.Out(kClrStatus, 0, nullptr, 0, &error)) return fail("CLRSTATUS: " + error);
    if (!GetStatus(usb, &s, &error)) return fail("GETSTATUS after CLRSTATUS: " + error);
    log(std::string("after CLRSTATUS: ") + StateName(s.state));
  }
  if (s.state != kDfuIdle) {
    if (!usb.Out(kAbort, 0, nullptr, 0, &error)) return fail("ABORT: " + error);
    if (!GetStatus(usb, &s, &error)) return fail("GETSTATUS after ABORT: " + error);
    log(std::string("after ABORT: ") + StateName(s.state));
  }
  if (s.state != kDfuIdle) return fail(std::string("the device didn't reach dfuIDLE (") + StateName(s.state) + ")");

  // The blocks.
  uint16_t block = 0;
  for (size_t offset = 0; offset < image.size(); ++block) {
    const uint16_t n = static_cast<uint16_t>(std::min<size_t>(transfer_size, image.size() - offset));
    if (!usb.Out(kDnload, block, image.data() + offset, n, &error)) {
      return fail("DNLOAD of block " + std::to_string(block) + " (byte " + std::to_string(offset) + "): " + error);
    }
    if (!WaitBlock(usb, &s, &error)) {
      return fail("block " + std::to_string(block) + " (byte " + std::to_string(offset) + "): " + error);
    }
    offset += n;
    progress(offset, image.size());
  }
  log("sent " + std::to_string(image.size()) + " bytes in " + std::to_string(block) + " blocks");

  // End of the download, then manifestation.
  if (!usb.Out(kDnload, block, nullptr, 0, &error)) return fail("final zero-length DNLOAD: " + error);
  uint32_t waited = 0;
  for (int polls = 0;; ++polls) {
    if (!GetStatus(usb, &s, &error)) {
      // A device that isn't manifestation tolerant may reset itself here; this one says it is, so note it.
      r.ok = true;
      r.left_in_manifest = true;
      log("the device stopped answering during manifestation (" + error + "): it may have restarted by itself");
      return r;
    }
    if (s.status != 0) {
      return fail(std::string("manifestation: ") + StatusName(s.status) + " in " + StateName(s.state));
    }
    if (s.state == kDfuIdle || s.state == kManifestWaitReset) {
      r.ok = true;
      r.final_state = s.state;
      log(std::string("manifestation done: ") + StateName(s.state));
      return r;
    }
    if (s.state != kManifestSync && s.state != kManifest) {
      return fail(std::string("unexpected state after the last block: ") + StateName(s.state));
    }
    if (waited > kManifestLimitMs || polls > kPollLimit) {
      return fail("manifestation still running after " + std::to_string(waited / 1000) + " s");
    }
    const uint32_t wait = std::max<uint32_t>(s.poll_ms, 1);
    usb.Sleep(wait);
    waited += wait;
  }
}

}  // namespace dfu
