#include "hub/eeprom.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace {

constexpr uint16_t kI2cCtl = 0xFFC0, kI2cDatO = 0xFFC1, kI2cDatI = 0xFFC2, kI2cAdr = 0xFFC3;
constexpr uint8_t kRxf = 0x80, kErr = 0x20, kTxe = 0x08, kStopRead = 0x02, kStopWrite = 0x01;
constexpr uint8_t kEeprom = 0x50;
constexpr int kEepromSize = 8192;
constexpr int kPage = 32;  // a page write must not cross a page boundary
constexpr uint32_t kSignature = 0x04513200;
constexpr int kHeaderSize = 12;
constexpr uint8_t kRebootString = 0x4F;  // diag.c: pulls P1.2 low, which resets the TUSB3200

struct I2cError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

using Clock = std::chrono::steady_clock;

Clock::time_point After(double seconds) {
  return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

std::string Format(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof buffer, format, args);
  va_end(args);
  return buffer;
}

uint16_t Sum(const uint8_t* data, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; ++i) sum += data[i];
  return static_cast<uint16_t>(sum);
}

struct Header {
  uint32_t signature;
  uint8_t header_size, version, eeprom_type, data_type;
  uint16_t size, checksum;
};

Header ParseHeader(const uint8_t* h) {
  return {static_cast<uint32_t>(h[0] | h[1] << 8 | h[2] << 16 | h[3] << 24),
          h[4],
          h[5],
          h[6],
          h[7],
          static_cast<uint16_t>(h[8] | h[9] << 8),
          static_cast<uint16_t>(h[10] | h[11] << 8)};
}

}  // namespace

std::string LoadFirmwareImage(const std::wstring& path, FirmwareImage* image) {
  FILE* file = _wfopen(path.c_str(), L"rb");
  if (!file) return "can't open the file";
  std::vector<uint8_t> bytes(kEepromSize + 1);
  size_t n = fread(bytes.data(), 1, bytes.size(), file);
  fclose(file);
  if (n > static_cast<size_t>(kEepromSize)) return "larger than the 8 KB EEPROM";
  if (n < static_cast<size_t>(kHeaderSize) + 1) return "too short for a TUSB3200 image";
  bytes.resize(n);
  Header h = ParseHeader(bytes.data());
  if (h.signature != kSignature) return Format("no TUSB3200 signature (found %08X)", h.signature);
  if (h.header_size != kHeaderSize) return Format("header size is %u, not 12", h.header_size);
  if (h.eeprom_type != 0x0A || h.data_type != 0x01) {
    return Format("not an application image for a 24C64 (types %02X %02X)", h.eeprom_type, h.data_type);
  }
  if (h.size == 0 || kHeaderSize + h.size != static_cast<int>(n)) {
    return Format("the header says %u bytes of code, the file has %u", h.size, static_cast<unsigned>(n - kHeaderSize));
  }
  uint16_t sum = Sum(bytes.data() + kHeaderSize, h.size);
  if (sum != h.checksum) return Format("checksum mismatch (header %04X, data %04X)", h.checksum, sum);
  image->bytes = std::move(bytes);
  image->version = h.version;
  image->size = h.size;
  image->checksum = h.checksum;
  return {};
}

void EepromWriter::Peek(uint16_t address, uint8_t* value) {
  if (DWORD error = link_.Peek(address, value, 1)) throw std::runtime_error("USB read failed: " + Win32Text(error));
}

void EepromWriter::Poke(uint16_t address, uint8_t value) {
  if (DWORD error = link_.Poke(address, value)) throw std::runtime_error("USB write failed: " + Win32Text(error));
}

uint8_t EepromWriter::Ctl() {
  uint8_t c = 0;
  Peek(kI2cCtl, &c);
  return c;
}

void EepromWriter::Wait(uint8_t bit, const char* what, double timeout_s) {
  auto end = After(timeout_s);
  for (;;) {
    uint8_t c = Ctl();
    if (c & kErr) {
      Poke(kI2cCtl, 0);
      throw I2cError(Format("no acknowledge while %s (I2CCTL %02X)", what, c));
    }
    if (c & bit) return;
    if (Clock::now() > end) throw I2cError(Format("timed out while %s (I2CCTL %02X)", what, c));
  }
}

void EepromWriter::Send(uint8_t byte, const char* what) {
  Poke(kI2cDatO, byte);
  Wait(kTxe, what);
}

// Random read of n bytes: a write of the address, then a read. The read phase starts with a dummy write
// to I2CDATO after I2CADR gets the read address (found 2026-10-16, usbeeprom.py).
std::vector<uint8_t> EepromWriter::Read(uint16_t offset, int n) {
  Poke(kI2cCtl, 0);  // 100 kHz, no stop pending, clear ERR
  Poke(kI2cAdr, kEeprom << 1);
  Send(static_cast<uint8_t>(offset >> 8), "sending the address high byte");
  Send(static_cast<uint8_t>(offset & 0xFF), "sending the address low byte");
  if (n == 1) Poke(kI2cCtl, kStopRead);
  Poke(kI2cAdr, kEeprom << 1 | 1);
  Poke(kI2cDatO, 0x00);
  Wait(kRxf, "waiting for the first byte after a dummy write");
  std::vector<uint8_t> out(n);
  for (int i = 0; i < n; ++i) {
    if (i) Wait(kRxf, "receiving a byte");
    if (i == n - 2) Poke(kI2cCtl, kStopRead);  // stop after the next (last) byte
    Peek(kI2cDatI, &out[i]);                    // reading I2CDATI frees the receiver for the next byte
  }
  Poke(kI2cCtl, 0);
  return out;
}

// Page write: up to 32 bytes inside one page; waits until the chip has finished writing.
void EepromWriter::Write(uint16_t offset, const uint8_t* data, int n) {
  Poke(kI2cCtl, 0);
  Poke(kI2cAdr, kEeprom << 1);
  Send(static_cast<uint8_t>(offset >> 8), "sending the address high byte");
  Send(static_cast<uint8_t>(offset & 0xFF), "sending the address low byte");
  for (int i = 0; i < n; ++i) {
    if (i == n - 1) Poke(kI2cCtl, kStopWrite);  // stop after this byte
    Send(data[i], "writing a byte");
  }
  Poke(kI2cCtl, 0);
  WaitReady();
}

// The chip ignores its address for up to 5 ms while it programs a page: poll until it answers.
void EepromWriter::WaitReady() {
  auto end = After(0.1);
  for (;;) {
    try {
      Read(0, 1);
      return;
    } catch (const I2cError&) {
      if (Clock::now() > end) throw;
    }
  }
}

void EepromWriter::Step(int bytes) {
  done_ += bytes;
  if (progress_ && *progress_) (*progress_)(stage_, done_, total_);
}

std::vector<uint8_t> EepromWriter::ReadRange(uint16_t offset, int n) {
  std::vector<uint8_t> data;
  while (static_cast<int>(data.size()) < n) {
    int k = std::min(kPage, n - static_cast<int>(data.size()));
    std::vector<uint8_t> chunk = Read(static_cast<uint16_t>(offset + data.size()), k);
    data.insert(data.end(), chunk.begin(), chunk.end());
    Step(k);
  }
  return data;
}

void EepromWriter::WriteRange(uint16_t offset, const uint8_t* data, int n) {
  int pos = 0;
  while (pos < n) {
    int here = offset + pos;
    int k = std::min(kPage - here % kPage, n - pos);
    Write(static_cast<uint16_t>(here), data + pos, k);
    pos += k;
    Step(k);
  }
}

void EepromWriter::Verify(uint16_t offset, const uint8_t* data, int n, const char* what) {
  std::vector<uint8_t> back = ReadRange(offset, n);
  int bad = 0, first = -1;
  for (int i = 0; i < n; ++i) {
    if (back[i] != data[i]) {
      if (first < 0) first = offset + i;
      ++bad;
    }
  }
  if (bad) throw I2cError(Format("%s: %d byte(s) differ, first at %04X", what, bad, first));
}

std::string EepromWriter::Install(const FirmwareImage& image, const Progress& progress) {
  const uint8_t* bytes = image.bytes.data();
  const int n = static_cast<int>(image.bytes.size());
  static const uint8_t kErased[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  progress_ = &progress;
  done_ = 0;
  // header read, erase + verify, image write + verify, signature write + verify, final check
  total_ = kHeaderSize + 8 + 2 * (n - 4) + 8 + n;
  auto set_stage = [&](const char* s) {
    stage_ = s;
    if (progress) progress(stage_, done_, total_);
  };
  try {
    // Proves the I2C path works before anything is changed.
    set_stage("Reading the installed header");
    ReadRange(0, kHeaderSize);

    set_stage("Erasing the signature");
    WriteRange(0, kErased, 4);
    Verify(0, kErased, 4, "erasing the signature");

    set_stage("Writing");
    WriteRange(4, bytes + 4, n - 4);
    set_stage("Verifying");
    Verify(4, bytes + 4, n - 4, "verifying the image");

    set_stage("Restoring the signature");
    WriteRange(0, bytes, 4);
    Verify(0, bytes, 4, "verifying the signature");

    set_stage("Checking the installed image");
    std::vector<uint8_t> header = ReadRange(0, kHeaderSize);
    Header h = ParseHeader(header.data());
    if (h.signature != kSignature || h.header_size != kHeaderSize || h.size != image.size) {
      throw I2cError("the installed header doesn't match the image");
    }
    std::vector<uint8_t> payload = ReadRange(kHeaderSize, h.size);
    uint16_t sum = Sum(payload.data(), payload.size());
    if (sum != h.checksum) throw I2cError(Format("installed image sum %04X, header says %04X", sum, h.checksum));
  } catch (const std::exception& e) {
    progress_ = nullptr;
    return std::string(stage_) + ": " + e.what();
  }
  progress_ = nullptr;

  // The reboot request never completes: the chip resets while answering it.
  if (progress) progress("Rebooting the Mbox", total_, total_);
  link_.SetTimeout(500);
  std::wstring ignored;
  link_.ReadString(kRebootString, &ignored);
  return {};
}
