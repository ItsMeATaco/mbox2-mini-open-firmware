// Installs a firmware image in the Mbox's EEPROM over USB, through the running open firmware: a C++ port
// of tools/firmware/usbeeprom.py.
//
// The TUSB3200's own I2C controller (TI SLES018A A.5.5: I2CCTL, I2CDATO, I2CDATI, I2CADR at FFC0h-FFC3h)
// is driven register by register with the firmware's peek and poke requests; the EEPROM is an AT24C64A at
// I2C address 50h with 16-bit addresses and 32-byte pages. Slow (a few USB requests per byte), but needs
// nothing from the firmware beyond peek and poke.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hub/maintenance.h"

// A TUSB3200 EEPROM image (TI SLES018A Table 2-2; firmware/mkimage.py): signature 0x04513200, header
// size 12, version, EEPROM type 0x0A, data type 0x01, payload size, 16-bit sum of the payload.
struct FirmwareImage {
  std::vector<uint8_t> bytes;  // header and payload
  uint8_t version = 0;         // 22 for v0.22 (bcdDevice 0x0022)
  uint16_t size = 0;           // payload bytes
  uint16_t checksum = 0;
};

// Reads and checks an image file. Returns an empty string if it's valid, else what's wrong with it.
std::string LoadFirmwareImage(const std::wstring& path, FirmwareImage* image);

class EepromWriter {
 public:
  // stage: what is happening; done/total: bytes of the whole job.
  using Progress = std::function<void(const char* stage, int done, int total)>;

  explicit EepromWriter(MaintenanceLink& link) : link_(link) {}

  // Writes the image so an interruption can't leave a half-written image that boots: the signature is
  // erased first, the rest is written and verified, and the signature goes back last; then the whole
  // image is read back and its sum checked, and the Mbox reboots into it. Returns an empty string on
  // success, else what went wrong (the Mbox keeps running its old firmware from RAM until it restarts).
  std::string Install(const FirmwareImage& image, const Progress& progress);

 private:
  uint8_t Ctl();
  void Wait(uint8_t bit, const char* what, double timeout_s = 0.2);
  void Send(uint8_t byte, const char* what);
  std::vector<uint8_t> Read(uint16_t offset, int n);
  void Write(uint16_t offset, const uint8_t* data, int n);
  void WaitReady();
  std::vector<uint8_t> ReadRange(uint16_t offset, int n);
  void WriteRange(uint16_t offset, const uint8_t* data, int n);
  void Verify(uint16_t offset, const uint8_t* data, int n, const char* what);
  void Peek(uint16_t address, uint8_t* value);
  void Poke(uint16_t address, uint8_t value);
  void Step(int bytes);

  MaintenanceLink& link_;
  const Progress* progress_ = nullptr;
  const char* stage_ = "";
  int done_ = 0, total_ = 0;
};
