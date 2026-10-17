// A small append-only log in %LOCALAPPDATA%\Mbox2Asio\driver.log. Not for the audio thread's normal path:
// it writes to disk under a lock.
#pragma once

namespace mbox2::asio {

void Log(const char* format, ...);

}  // namespace mbox2::asio
