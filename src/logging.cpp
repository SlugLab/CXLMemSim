//
// Created by victoryang00 on 1/13/23.
//

#include "logging.h"
#include <cstddef>
#include <iostream>

void LogWriter::operator<(const LogStream &stream) {
    std::ostringstream msg;
    if (log_level_ == TRACE)
        file_ << stream.sstream_->rdbuf();
    else {
        msg << stream.sstream_->rdbuf();
        output_log(msg);
    }
}

void LogWriter::output_log(const std::ostringstream &msg) {
    if (log_level_ >= env_log_level)
        std::cout << fmt::format(fmt::emphasis::bold | fg(level2color(log_level_)), "{}:{} {} ", location_.file_name(),
                                 location_.line(), location_.function_name())
                  << fmt::format(fg(level2color(log_level_)), "{} ", msg.str());
}
std::string level2string(LogLevel level) {
    switch (level) {
    case DEBUG:
        return "DEBUG";
    case INFO:
        return "INFO";
    case WARNING:
        return "WARNING";
    case ERROR:
        return "ERROR";
    default:
        return "";
    }
}
fmt::color level2color(LogLevel level) {
    switch (level) {
    case DEBUG:
        return fmt::color::alice_blue;
    case INFO:
        return fmt::color::magenta;
    case WARNING:
        return fmt::color::yellow;
    case ERROR:
        return fmt::color::red;
    default:
        return fmt::color::white;
    }
}