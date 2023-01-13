//
// Created by victoryang00 on 1/13/23.
//

#ifndef CXL_MEM_SIMULATOR_LOGGING_H
#define CXL_MEM_SIMULATOR_LOGGING_H

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/os.h>
#include <fmt/ostream.h>
#include <fstream>
#include <iostream>
#include <list>
#include <source_location>
#include <sstream>
#include <string>

enum LogLevel { DEBUG = 0, INFO, WARNING, ERROR };

class LogStream;
class LogWriter;

class LogWriter {
public:
    LogWriter(const std::source_location &location, LogLevel loglevel) : location_(location), log_level_(loglevel) {
        char *logv = std::getenv("LOGV");
        if (logv) {
            env_log_level = std::stoi(logv);
        } else {
            env_log_level = 4;
        }
    };

    void operator<(const LogStream &stream);

private:
    void output_log(const std::ostringstream &g);
    std::source_location location_;
    LogLevel log_level_;
    int env_log_level;
};

class LogStream {
public:
    LogStream() { sstream_ = new std::stringstream(); }
    ~LogStream() = default;

    template <typename T> LogStream &operator<<(const T &val) noexcept {
        (*sstream_) << val;
        return *this;
    }

    friend class LogWriter;

private:
    std::stringstream *sstream_;
};

std::string level2string(LogLevel level);
fmt::color level2color(LogLevel level);

#define LOG_IF(level) LogWriter(std::source_location::current(), level) < LogStream()
#define LOG(level) LOG_##level
#define LOG_DEBUG LOG_IF(DEBUG)
#define LOG_INFO LOG_IF(INFO)
#define LOG_WARNING LOG_IF(WARNING)
#define LOG_ERROR LOG_IF(ERROR)

#endif // CXL_MEM_SIMULATOR_LOGGING_H
