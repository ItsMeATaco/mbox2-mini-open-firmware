#include "hub/update.h"

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <iterator>

#include "hub/eeprom.h"

namespace {

constexpr size_t kReleaseLimit = 1 << 20;     // the API's JSON for one release
constexpr size_t kFileLimit = 32 << 20;       // any release file
constexpr int kMaxDepth = 32;

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(length, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

std::string WinError(const char* what, DWORD code) {
  char text[96];
  snprintf(text, sizeof text, "%s failed (error %lu)", what, code);
  return text;
}

// Just enough JSON for GitHub's release objects: values keep strings (as UTF-8), arrays and objects.
struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  std::string text;  // a string's value, or a number as written
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;

  const Json* Get(const char* key) const {
    for (const auto& [name, value] : members) {
      if (name == key) return &value;
    }
    return nullptr;
  }
  std::string String(const char* key) const {
    const Json* value = Get(key);
    return value && value->type == Type::String ? value->text : std::string();
  }
};

class JsonParser {
 public:
  JsonParser(const std::string& text) : p_(text.data()), end_(text.data() + text.size()) {}
  bool Parse(Json* out) {
    if (!Value(out, 0)) return false;
    Space();
    return p_ == end_;
  }

 private:
  void Space() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }
  bool Literal(const char* word) {
    for (const char* w = word; *w; ++w, ++p_) {
      if (p_ >= end_ || *p_ != *w) return false;
    }
    return true;
  }
  bool Hex4(unsigned* value) {
    if (end_ - p_ < 4) return false;
    *value = 0;
    for (int i = 0; i < 4; ++i, ++p_) {
      char c = *p_;
      int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0) return false;
      *value = *value << 4 | digit;
    }
    return true;
  }
  static void Utf8(unsigned code, std::string* out) {
    if (code < 0x80) {
      *out += static_cast<char>(code);
    } else if (code < 0x800) {
      *out += static_cast<char>(0xC0 | code >> 6);
      *out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      *out += static_cast<char>(0xE0 | code >> 12);
      *out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      *out += static_cast<char>(0xF0 | code >> 18);
      *out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
      *out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
      *out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }
  bool String(std::string* out) {
    if (p_ >= end_ || *p_ != '"') return false;
    ++p_;
    while (p_ < end_ && *p_ != '"') {
      if (*p_ != '\\') {
        *out += *p_++;
        continue;
      }
      if (++p_ >= end_) return false;
      char c = *p_++;
      switch (c) {
        case '"': case '\\': case '/': *out += c; break;
        case 'b': *out += '\b'; break;
        case 'f': *out += '\f'; break;
        case 'n': *out += '\n'; break;
        case 'r': *out += '\r'; break;
        case 't': *out += '\t'; break;
        case 'u': {
          unsigned code = 0;
          if (!Hex4(&code)) return false;
          if (code >= 0xD800 && code < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            p_ += 2;
            unsigned low = 0;
            if (!Hex4(&low) || low < 0xDC00 || low > 0xDFFF) return false;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
          Utf8(code, out);
          break;
        }
        default: return false;
      }
    }
    if (p_ >= end_) return false;
    ++p_;
    return true;
  }
  bool Value(Json* out, int depth) {
    if (depth > kMaxDepth) return false;
    Space();
    if (p_ >= end_) return false;
    switch (*p_) {
      case '{': {
        out->type = Json::Type::Object;
        ++p_;
        Space();
        if (p_ < end_ && *p_ == '}') return ++p_, true;
        for (;;) {
          std::pair<std::string, Json> member;
          Space();
          if (!String(&member.first)) return false;
          Space();
          if (p_ >= end_ || *p_++ != ':') return false;
          if (!Value(&member.second, depth + 1)) return false;
          out->members.push_back(std::move(member));
          Space();
          if (p_ < end_ && *p_ == ',') { ++p_; continue; }
          return p_ < end_ && *p_++ == '}';
        }
      }
      case '[': {
        out->type = Json::Type::Array;
        ++p_;
        Space();
        if (p_ < end_ && *p_ == ']') return ++p_, true;
        for (;;) {
          out->items.emplace_back();
          if (!Value(&out->items.back(), depth + 1)) return false;
          Space();
          if (p_ < end_ && *p_ == ',') { ++p_; continue; }
          return p_ < end_ && *p_++ == ']';
        }
      }
      case '"':
        out->type = Json::Type::String;
        return String(&out->text);
      case 't': out->type = Json::Type::Bool; out->text = "true"; return Literal("true");
      case 'f': out->type = Json::Type::Bool; out->text = "false"; return Literal("false");
      case 'n': out->type = Json::Type::Null; return Literal("null");
      default: {
        out->type = Json::Type::Number;
        const char* start = p_;
        while (p_ < end_ && (isdigit(static_cast<unsigned char>(*p_)) || *p_ == '-' || *p_ == '+' || *p_ == '.' ||
                             *p_ == 'e' || *p_ == 'E')) {
          ++p_;
        }
        out->text.assign(start, p_);
        return p_ > start;
      }
    }
  }

  const char* p_;
  const char* end_;
};

bool ParseRelease(const std::string& text, Release* release, std::string* error) {
  Json root;
  if (!JsonParser(text).Parse(&root) || root.type != Json::Type::Object) {
    *error = "GitHub's answer isn't valid JSON";
    return false;
  }
  release->tag = Widen(root.String("tag_name"));
  release->title = Widen(root.String("name"));
  release->notes = Widen(root.String("body"));
  release->page = Widen(root.String("html_url"));
  if (release->tag.empty()) {
    *error = "GitHub's answer has no release tag";
    return false;
  }
  if (const Json* assets = root.Get("assets"); assets && assets->type == Json::Type::Array) {
    for (const Json& item : assets->items) {
      ReleaseAsset asset;
      asset.name = Widen(item.String("name"));
      asset.url = Widen(item.String("browser_download_url"));
      std::string digest = item.String("digest");
      if (digest.rfind("sha256:", 0) == 0) asset.sha256 = digest.substr(7);
      if (!asset.name.empty() && !asset.url.empty()) release->assets.push_back(std::move(asset));
    }
  }
  return true;
}

std::string Sha256(const std::string& data) {
  unsigned char hash[32] = {};
  if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                 static_cast<ULONG>(data.size()), hash, sizeof hash) != 0) {
    return {};
  }
  std::string hex;
  char pair[3];
  for (unsigned char byte : hash) {
    snprintf(pair, sizeof pair, "%02x", byte);
    hex += pair;
  }
  return hex;
}

// Writes next to `path` first and then replaces it, so a reader never sees half a file.
bool WriteFile(const std::wstring& path, const std::string& data, std::string* error) {
  const std::wstring temporary = path + L".part";
  FILE* file = _wfopen(temporary.c_str(), L"wb");
  if (!file) {
    *error = "can't save the download";
    return false;
  }
  bool ok = fwrite(data.data(), 1, data.size(), file) == data.size();
  ok = fclose(file) == 0 && ok;
  if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(temporary.c_str());
    *error = WinError("saving the download", GetLastError());
    return false;
  }
  return true;
}

void CreateFolders(const std::wstring& folder) {
  for (size_t at = folder.find(L'\\', 3); at != std::wstring::npos; at = folder.find(L'\\', at + 1)) {
    CreateDirectoryW(folder.substr(0, at).c_str(), nullptr);
  }
  CreateDirectoryW(folder.c_str(), nullptr);
}

}  // namespace

const ReleaseAsset* Release::Asset(const wchar_t* name) const {
  for (const ReleaseAsset& asset : assets) {
    if (_wcsicmp(asset.name.c_str(), name) == 0) return &asset;
  }
  return nullptr;
}

int CompareVersions(const std::wstring& a, const std::wstring& b) {
  auto numbers = [](const std::wstring& text) {
    std::vector<unsigned long> parts;
    for (size_t i = 0; i < text.size();) {
      if (iswdigit(text[i])) {
        size_t end = i;
        while (end < text.size() && iswdigit(text[end])) ++end;
        parts.push_back(wcstoul(text.substr(i, end - i).c_str(), nullptr, 10));
        i = end;
      } else {
        ++i;
      }
    }
    return parts;
  };
  std::vector<unsigned long> x = numbers(a), y = numbers(b);
  for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
    unsigned long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
    if (p != q) return p < q ? -1 : 1;
  }
  return 0;
}

Updater::Updater(const std::wstring& user_agent, std::function<void()> changed) : changed_(std::move(changed)) {
  session_ = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0);
  if (session_) WinHttpSetTimeouts(session_, 10000, 10000, 15000, 30000);
}

Updater::~Updater() {
  stopping_ = true;
  {
    std::lock_guard lock(mutex_);
    if (request_) WinHttpCloseHandle(request_);  // makes the worker's pending call fail at once
    request_ = nullptr;
  }
  if (worker_.joinable()) worker_.join();
  if (session_) WinHttpCloseHandle(session_);
}

Updater::Status Updater::GetStatus() const {
  std::lock_guard lock(mutex_);
  return status_;
}

void Updater::Set(const std::function<void(Status&)>& change) {
  {
    std::lock_guard lock(mutex_);
    change(status_);
  }
  if (!stopping_ && changed_) changed_();
}

void Updater::Start(std::function<void()> work) {
  if (busy_.exchange(true)) return;
  if (worker_.joinable()) worker_.join();
  worker_ = std::thread([this, work = std::move(work)] {
    work();
    busy_ = false;
    if (!stopping_ && changed_) changed_();
  });
}

bool Updater::Get(const std::wstring& url, const wchar_t* accept, size_t limit, std::string* body, std::string* error) {
  if (!session_) {
    *error = WinError("starting WinHTTP", GetLastError());
    return false;
  }
  wchar_t host[256] = {}, path[2048] = {}, extra[1024] = {};
  URL_COMPONENTS parts{sizeof parts};
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
    *error = "bad download address";
    return false;
  }
  HINTERNET connection = WinHttpConnect(session_, host, parts.nPort, 0);
  if (!connection) {
    *error = WinError("connecting", GetLastError());
    return false;
  }
  HINTERNET request = WinHttpOpenRequest(connection, L"GET", (std::wstring(path) + extra).c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  bool ok = false;
  if (request) {
    {
      std::lock_guard lock(mutex_);
      request_ = stopping_ ? nullptr : request;
    }
    const std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    DWORD code = 0, size = sizeof code;
    if (stopping_) {
      *error = "stopped";
    } else if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
               !WinHttpReceiveResponse(request, nullptr)) {
      *error = GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT || GetLastError() == ERROR_WINHTTP_NAME_NOT_RESOLVED
                   ? "no connection to GitHub"
                   : WinError("the request", GetLastError());
    } else if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX) ||
               code != 200) {
      char text[64];
      snprintf(text, sizeof text, code == 404 ? "not found on GitHub (HTTP %lu)" : "GitHub answered HTTP %lu", code);
      *error = text;
    } else {
      ok = true;
      body->clear();
      for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
          ok = false;
          *error = WinError("downloading", GetLastError());
          break;
        }
        if (available == 0) break;
        if (body->size() + available > limit) {
          ok = false;
          *error = "the download is larger than expected";
          break;
        }
        const size_t at = body->size();
        body->resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body->data() + at, available, &read)) {
          ok = false;
          *error = WinError("downloading", GetLastError());
          break;
        }
        body->resize(at + read);
      }
    }
    std::lock_guard lock(mutex_);
    if (request_ == request) {
      WinHttpCloseHandle(request);
      request_ = nullptr;
    }
  } else {
    *error = WinError("opening the request", GetLastError());
  }
  WinHttpCloseHandle(connection);
  return ok;
}

bool Updater::Fetch(const ReleaseAsset& asset, const std::wstring& path, std::string* error) {
  std::string data;
  if (!Get(asset.url, L"application/octet-stream", kFileLimit, &data, error)) return false;
  if (!asset.sha256.empty() && Sha256(data) != asset.sha256) {
    *error = "a download didn't match GitHub's checksum";
    return false;
  }
  return WriteFile(path, data, error);
}

void Updater::Check(const std::wstring& firmware_path, bool manual) {
  if (busy_) return;
  Set([manual](Status& s) {
    s.state = State::Checking;
    s.manual = manual;
    s.error.clear();
  });
  Start([this, firmware_path] {
    Release release;
    std::string body, error;
    const std::wstring url = std::wstring(L"https://api.github.com/repos/") + kRepository + L"/releases/latest";
    if (!Get(url, L"application/vnd.github+json", kReleaseLimit, &body, &error) ||
        !ParseRelease(body, &release, &error)) {
      Set([&](Status& s) {
        s.state = State::Failed;
        s.error = error;
      });
      return;
    }
    // The firmware image is small: fetch it now, so the Firmware card can offer it. Keep the last good one if
    // this one doesn't check out.
    std::wstring firmware;
    if (const ReleaseAsset* asset = release.Asset(kFirmwareFile)) {
      const std::wstring folder = firmware_path.substr(0, firmware_path.find_last_of(L'\\'));
      CreateFolders(folder);
      const std::wstring fresh = firmware_path + L".new";
      FirmwareImage image;
      if (Fetch(*asset, fresh, &error) && LoadFirmwareImage(fresh, &image).empty() && !image.avid &&
          MoveFileExW(fresh.c_str(), firmware_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        firmware = firmware_path;
      } else {
        DeleteFileW(fresh.c_str());
      }
    }
    Set([&](Status& s) {
      s.state = State::Checked;
      s.release = std::move(release);
      s.firmware = firmware;
    });
  });
}

void Updater::Download(const std::wstring& folder) {
  if (busy_) return;
  Set([](Status& s) {
    s.state = State::Downloading;
    s.error.clear();
  });
  Start([this, folder] {
    const Release release = GetStatus().release;
    std::string error;
    CreateFolders(folder);
    bool ok = release.Asset(kHubFile) != nullptr;
    if (!ok) error = "the release has no mbox2hub.exe";
    for (const wchar_t* name : {kHubFile, kDriverFile, kFirmwareFile}) {
      const ReleaseAsset* asset = release.Asset(name);
      if (!ok || !asset) continue;
      ok = Fetch(*asset, folder + L"\\" + name, &error);
    }
    Set([&](Status& s) {
      s.state = ok ? State::Downloaded : State::Failed;
      s.folder = ok ? folder : L"";
      s.error = error;
    });
  });
}
