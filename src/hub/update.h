// Follows the project's GitHub releases: asks for the latest one, downloads its firmware image right away (a
// few KB, so the Firmware card can offer it), and on request the hub's and the ASIO driver's new files, each
// checked against the SHA-256 digest GitHub lists for it. The hub updates itself by swapping those files in
// next to its exe (app.cpp); firmware goes in over USB like any other image.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The repository whose releases the hub follows (CMake option MBOX2_REPOSITORY, for forks), and the release
// files it uses.
inline constexpr wchar_t kRepository[] = L"" MBOX2_REPOSITORY;
inline constexpr wchar_t kHubFile[] = L"mbox2hub.exe";
inline constexpr wchar_t kDriverFile[] = L"mbox2asio.dll";
inline constexpr wchar_t kFirmwareFile[] = L"mbox2-open.bin";

struct ReleaseAsset {
  std::wstring name;
  std::wstring url;    // browser_download_url
  std::string sha256;  // lowercase hex, from the API's "digest" ("sha256:..."); empty if it lists none
};

struct Release {
  std::wstring tag;    // "v1.1.0"
  std::wstring title;
  std::wstring notes;  // the release text (Markdown)
  std::wstring page;   // html_url
  std::vector<ReleaseAsset> assets;
  const ReleaseAsset* Asset(const wchar_t* name) const;
};

// <0, 0 or >0 as a is older, the same or newer, comparing the dotted numbers in "v1.2.3" or "1.2".
int CompareVersions(const std::wstring& a, const std::wstring& b);

// A release's notes (GitHub Markdown) as plain text for a message box: headings, emphasis, code marks, links,
// quotes, rules and table pipes reduced to their words, list items to bullets.
std::wstring PlainText(const std::wstring& markdown);

class Updater {
 public:
  enum class State { Idle, Checking, Checked, Downloading, Downloaded, Failed };
  struct Status {
    State state = State::Idle;
    bool manual = false;    // the last check was asked for from the menu
    Release release;        // the latest release, once a check succeeded
    std::wstring firmware;  // its firmware image, downloaded and valid; empty if it has none
    std::wstring folder;    // where Download() put the new files
    std::string error;      // why the last check or download failed
  };

  // `changed` runs on the worker thread whenever the status changes.
  Updater(const std::wstring& user_agent, std::function<void()> changed);
  ~Updater();

  // Asks GitHub for the latest release, and saves its firmware image as `firmware_path` once it's checked.
  void Check(const std::wstring& firmware_path, bool manual);
  // Downloads the latest release's hub, driver and firmware files into `folder`.
  void Download(const std::wstring& folder);
  Status GetStatus() const;
  bool Busy() const { return busy_; }

 private:
  void Start(std::function<void()> work);
  void Set(const std::function<void(Status&)>& change);
  bool Get(const std::wstring& url, const wchar_t* accept, size_t limit, std::string* body, std::string* error);
  bool Fetch(const ReleaseAsset& asset, const std::wstring& path, std::string* error);

  std::function<void()> changed_;
  HINTERNET session_ = nullptr;
  mutable std::mutex mutex_;
  Status status_;
  std::thread worker_;
  std::atomic<bool> busy_{false};
  std::atomic<bool> stopping_{false};
  HINTERNET request_ = nullptr;  // the one in flight, closed to cancel it on exit (under mutex_)
};
