// SPDX-License-Identifier: GPL-3.0-or-later
//
// Logging for the VR host layer, on the port's own PortLog so it lands on
// stderr on the desktop and in logcat on Android. PORTVR_LOG() is a drop-in
// for Wiicompiled's `RT_LOG(RT_TAG_RUNTIME) << ... << std::endl;` lines: the
// stream is flushed as one line when the temporary dies.

#pragma once

#include "port_log.h"

#include <sstream>
#include <string>

namespace PortVr {

class LogStream {
public:
    LogStream() = default;
    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;
    ~LogStream() {
        std::string line = m_stream.str();
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        PortLog::Write("[vr] %s\n", line.c_str());
    }
    template <typename T>
    LogStream& operator<<(const T& value) {
        m_stream << value;
        return *this;
    }
    // std::endl and friends: swallowed, the destructor ends the line.
    LogStream& operator<<(std::ostream& (*)(std::ostream&)) { return *this; }

private:
    std::ostringstream m_stream;
};

} // namespace PortVr

#define PORTVR_LOG() ::PortVr::LogStream()
