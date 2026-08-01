#include "kaltura_live/logger.hpp"
#include "kaltura_live/settings_manager.hpp"

#include <obs-module.h>

#include <string>

namespace {
constexpr const char *kLogPrefix = "[kaltura-live] ";
}

namespace kaltura_live {

LoggingLevel Logger::minimumLevel_ = LoggingLevel::Info;
bool Logger::debugEnabled_ = false;

void Logger::configure(LoggingLevel minimumLevel, bool debugEnabled)
{
  minimumLevel_ = minimumLevel;
  debugEnabled_ = debugEnabled;
}

void Logger::write(LogLevel level, std::string_view message)
{
  if (level == LogLevel::Debug && !debugEnabled_) {
    return;
  }

  if (level != LogLevel::Debug) {
    const int levelRank = level == LogLevel::Info ? 0 : level == LogLevel::Warning ? 1 : 2;
    const int minimumRank = minimumLevel_ == LoggingLevel::Info
                              ? 0
                              : minimumLevel_ == LoggingLevel::Warning ? 1 : 2;
    if (levelRank < minimumRank) {
      return;
    }
  }

  int obsLogLevel = LOG_INFO;
  switch (level) {
  case LogLevel::Debug:
    obsLogLevel = LOG_DEBUG;
    break;
  case LogLevel::Info:
    obsLogLevel = LOG_INFO;
    break;
  case LogLevel::Warning:
    obsLogLevel = LOG_WARNING;
    break;
  case LogLevel::Error:
    obsLogLevel = LOG_ERROR;
    break;
  }

  const std::string line = std::string(kLogPrefix) + std::string(message);
  blog(obsLogLevel, "%s", line.c_str());
}

}  // namespace kaltura_live
