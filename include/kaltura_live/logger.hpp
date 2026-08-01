#pragma once

#include <string_view>

namespace kaltura_live {

enum class LoggingLevel;

enum class LogLevel {
  Debug,
  Info,
  Warning,
  Error,
};

class Logger {
public:
  static void configure(LoggingLevel minimumLevel, bool debugEnabled);
  static void write(LogLevel level, std::string_view message);

private:
  static LoggingLevel minimumLevel_;
  static bool debugEnabled_;
};

}  // namespace kaltura_live
