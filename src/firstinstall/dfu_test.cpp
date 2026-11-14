// Checks dfu::Download against a simulated DFU 1.0 device shaped like Avid's update mode on the Mbox 2 Mini
// (8-byte blocks, manifestation tolerant), and dfu::FindFunctional against the Mini's real runtime descriptors.
// Returns 0 when every check passes.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "firstinstall/dfu.h"

using namespace dfu;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

// A DFU-mode device following the DFU 1.1 state diagram, recording anything the host does out of turn.
class SimDevice : public Transport {
 public:
  uint16_t transfer_size = 8;
  uint8_t state = kDfuIdle;
  uint8_t status = 0;
  uint32_t poll_ms = 5;          // bwPollTimeout reported while busy
  int busy_polls = 2;            // GETSTATUS replies of dfuDNBUSY after each block
  int manifest_polls = 3;
  int fail_at_block = -1;        // reports fail_status at this block's GETSTATUS
  uint8_t fail_status = 7;       // errVERIFY
  bool vanish_in_manifest = false;

  std::vector<uint8_t> received;
  std::vector<uint16_t> block_sizes;
  std::vector<std::string> violations;
  bool got_end = false;
  int requests_after_error = 0;
  uint32_t slept = 0;

  bool Out(uint8_t request, uint16_t value, const uint8_t* data, uint16_t length, std::string* error) override {
    if (errored_) ++requests_after_error;
    switch (request) {
      case kDnload:
        if (state != kDfuIdle && state != kDnloadIdle) return Stall("DNLOAD in " + std::string(StateName(state)), error);
        if (length == 0) {
          if (state == kDfuIdle) violations.push_back("zero-length DNLOAD before any data");
          if (value != next_block_) violations.push_back("end block number " + std::to_string(value));
          got_end = true;
          state = kManifestSync;
          return true;
        }
        if (value != next_block_) violations.push_back("block " + std::to_string(value) + " where " +
                                                       std::to_string(next_block_) + " was due");
        if (length > transfer_size) violations.push_back("block longer than the transfer size");
        received.insert(received.end(), data, data + length);
        block_sizes.push_back(length);
        if (static_cast<int>(next_block_) == fail_at_block) {
          state = kDfuError;
          status = fail_status;
          errored_ = true;
        } else {
          state = kDnloadSync;
          busy_left_ = busy_polls;
        }
        ++next_block_;
        return true;
      case kClrStatus:
        if (state == kDfuError) state = kDfuIdle, status = 0;
        return true;
      case kAbort:
        if (state == kDnloadIdle || state == kDfuIdle || state == kUploadIdle) state = kDfuIdle, next_block_ = 0;
        return true;
      default:
        return Stall("unexpected request " + std::to_string(request), error);
    }
  }

  bool In(uint8_t request, uint16_t, uint8_t* data, uint16_t length, uint16_t* got, std::string* error) override {
    if (request != kGetStatus || length < 6) return Stall("unexpected IN request", error);
    if (vanish_in_manifest && (state == kManifestSync || state == kManifest)) {
      *error = "the device is gone";
      return false;
    }
    uint32_t reported_poll = 0;
    switch (state) {
      case kDnloadSync:
        state = busy_left_ > 0 ? kDnBusy : kDnloadIdle;
        break;
      case kDnBusy:
        if (slept_since_busy_ < poll_ms) violations.push_back("GETSTATUS before bwPollTimeout had passed");
        state = --busy_left_ > 0 ? kDnBusy : kDnloadIdle;
        break;
      case kManifestSync:
        state = kManifest;
        break;
      case kManifest:
        if (--manifest_polls <= 0) state = kDfuIdle;  // manifestation tolerant: back to dfuIDLE
        break;
      default:
        break;
    }
    if (state == kDnBusy || state == kManifest) reported_poll = poll_ms, slept_since_busy_ = 0;
    data[0] = status;
    data[1] = static_cast<uint8_t>(reported_poll);
    data[2] = static_cast<uint8_t>(reported_poll >> 8);
    data[3] = static_cast<uint8_t>(reported_poll >> 16);
    data[4] = state;
    data[5] = 0;
    *got = 6;
    return true;
  }

  void Sleep(uint32_t ms) override {
    slept += ms;
    slept_since_busy_ += ms;
  }

 private:
  bool Stall(const std::string& why, std::string* error) {
    violations.push_back(why);
    *error = "stalled: " + why;
    return false;
  }
  uint16_t next_block_ = 0;
  int busy_left_ = 0;
  uint32_t slept_since_busy_ = 0;
  bool errored_ = false;
};

std::vector<uint8_t> Image(size_t size) {
  std::vector<uint8_t> image(size);
  uint32_t x = 12345;
  for (auto& b : image) b = static_cast<uint8_t>((x = x * 1103515245 + 12345) >> 16);
  return image;
}

DownloadResult Run(SimDevice& dev, const std::vector<uint8_t>& image, std::vector<std::string>* log = nullptr) {
  return Download(dev, image, dev.transfer_size, [](size_t, size_t) {},
                  [log](const std::string& line) { if (log) log->push_back(line); });
}

void Normal() {
  SimDevice dev;
  auto image = Image(7001);  // the size of mbox2-open.bin v0.27
  auto r = Run(dev, image);
  Check(r.ok && r.error.empty(), "7001-byte image downloads (" + r.error + ")");
  Check(dev.received == image, "the device received exactly the image");
  bool sizes = dev.block_sizes.size() == 876;
  for (size_t i = 0; i + 1 < dev.block_sizes.size(); ++i) sizes &= dev.block_sizes[i] == 8;
  sizes &= !dev.block_sizes.empty() && dev.block_sizes.back() == 1;
  Check(sizes, "876 blocks: 875 of 8 bytes and a last one of 1");
  Check(dev.got_end, "a zero-length DNLOAD ends the download");
  Check(r.final_state == kDfuIdle && !r.left_in_manifest, "manifestation ends in dfuIDLE");
  Check(dev.violations.empty(), "no protocol violations" + (dev.violations.empty() ? "" : ": " + dev.violations[0]));
  Check(dev.slept >= 876u * 2 * dev.poll_ms, "waited bwPollTimeout on every busy reply");
}

void ExactMultiple() {
  SimDevice dev;
  auto image = Image(64);
  auto r = Run(dev, image);
  Check(r.ok && dev.received == image && dev.block_sizes.size() == 8 && dev.got_end,
        "an image that's a multiple of the block size: 8 full blocks, then the zero-length one");
}

void StartsInError() {
  SimDevice dev;
  dev.state = kDfuError;
  dev.status = 10;  // errFIRMWARE
  auto image = Image(100);
  auto r = Run(dev, image);
  Check(r.ok && dev.received == image, "starting in dfuERROR: CLRSTATUS, then the download");
}

void StartsMidDownload() {
  SimDevice dev;
  dev.state = kDnloadIdle;
  auto image = Image(100);
  auto r = Run(dev, image);
  Check(r.ok && dev.received == image && dev.violations.empty(), "starting in dfuDNLOAD-IDLE: ABORT, then from block 0");
}

void RuntimeMode() {
  SimDevice dev;
  dev.state = kAppIdle;
  auto r = Run(dev, Image(100));
  Check(!r.ok && dev.received.empty() && r.error.find("normal mode") != std::string::npos,
        "a device in its normal mode gets no data: " + r.error);
}

void ErrorAtBlock() {
  SimDevice dev;
  dev.fail_at_block = 100;
  auto r = Run(dev, Image(7001));
  Check(!r.ok && r.error.find("errVERIFY") != std::string::npos && r.error.find("block 100") != std::string::npos,
        "an error status stops the download and names it: " + r.error);
  Check(dev.requests_after_error == 0 && !dev.got_end, "nothing more is sent after the error");
}

void VanishInManifest() {
  SimDevice dev;
  dev.vanish_in_manifest = true;
  std::vector<std::string> log;
  auto r = Run(dev, Image(500), &log);
  Check(r.ok && r.left_in_manifest, "a device that resets itself during manifestation is reported as such");
}

void StuckBusy() {
  SimDevice dev;
  dev.busy_polls = 1000000;
  dev.poll_ms = 50;
  auto r = Run(dev, Image(100));
  Check(!r.ok && r.error.find("still busy") != std::string::npos, "a block that never finishes times out: " + r.error);
}

void Descriptors() {
  // The Mini's runtime configuration descriptor (Avid firmware 1.40) up to its first audio interface, from
  // docs/history/mbox2-mini-descriptors.bin: configuration, interface 0 (FE/01/00), DFU functional descriptor,
  // interface 1 (audio control).
  const uint8_t avid[] = {0x09, 0x02, 0x86, 0x02, 0x07, 0x01, 0x00, 0x80, 0xFA,
                          0x09, 0x04, 0x00, 0x00, 0x00, 0xFE, 0x01, 0x00, 0x00,
                          0x07, 0x21, 0x05, 0xFF, 0xFF, 0x08, 0x00,
                          0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00};
  Functional f = FindFunctional(avid, sizeof avid);
  Check(f.found && f.interface_number == 0 && f.protocol == 0 && f.attributes == 0x05 &&
            f.detach_timeout_ms == 0xFFFF && f.transfer_size == 8 && f.dfu_version == 0,
        "Avid's runtime descriptor: interface 0, attributes 05, detach 0xFFFF, 8-byte transfers, DFU 1.0");

  // A HID interface (whose descriptor is also type 0x21) before a DFU 1.1 interface.
  const uint8_t mixed[] = {0x09, 0x02, 0x2B, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
                           0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
                           0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x20, 0x00,
                           0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,
                           0x09, 0x04, 0x01, 0x00, 0x00, 0xFE, 0x01, 0x02, 0x00,
                           0x09, 0x21, 0x0B, 0x00, 0x10, 0x00, 0x04, 0x10, 0x01};
  f = FindFunctional(mixed, sizeof mixed);
  Check(f.found && f.interface_number == 1 && f.protocol == 2 && f.transfer_size == 0x400 && f.dfu_version == 0x0110,
        "a HID descriptor isn't taken for DFU; a DFU 1.1 descriptor reads its version");

  const uint8_t none[] = {0x09, 0x02, 0x12, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
                          0x09, 0x04, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00};
  Check(!FindFunctional(none, sizeof none).found, "no DFU interface: not found");
  const uint8_t truncated[] = {0x09, 0x04, 0x00, 0x00, 0x00, 0xFE, 0x01, 0x00, 0x00, 0x07, 0x21, 0x05};
  Check(!FindFunctional(truncated, sizeof truncated).found, "a truncated descriptor isn't read past its end");
}

}  // namespace

int main() {
  Normal();
  ExactMultiple();
  StartsInError();
  StartsMidDownload();
  RuntimeMode();
  ErrorAtBlock();
  VanishInManifest();
  StuckBusy();
  Descriptors();
  std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
  return failures ? 1 : 0;
}
