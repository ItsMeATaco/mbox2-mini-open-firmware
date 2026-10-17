#include "asio/log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include "asio/names.h"

namespace mbox2::asio {
namespace {

std::mutex g_mutex;

std::wstring LogPath() {
  PWSTR local = nullptr;
  std::wstring path;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
    path = std::wstring(local) + L"\\" + kLogFolder;
    CreateDirectoryW(path.c_str(), nullptr);
    path += L"\\driver.log";
  }
  CoTaskMemFree(local);
  return path;
}

}  // namespace

void Log(const char* format, ...) {
  char message[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof message, format, args);
  va_end(args);

  SYSTEMTIME now;
  GetLocalTime(&now);
  char exe[MAX_PATH] = "";
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  const char* exe_name = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;

  std::lock_guard lock(g_mutex);
  static const std::wstring path = LogPath();
  if (path.empty()) return;
  FILE* file = _wfopen(path.c_str(), L"a");
  if (!file) return;
  fprintf(file, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s %lu] %s\n", now.wYear, now.wMonth, now.wDay, now.wHour,
          now.wMinute, now.wSecond, now.wMilliseconds, exe_name, GetCurrentProcessId(), message);
  fclose(file);
}

}  // namespace mbox2::asio
