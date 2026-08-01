#pragma once

#include "kaltura_live/captions/caption_dictionary.hpp"

#include <obs-data.h>

#include <string>
#include <string_view>
#include <vector>

namespace kaltura_live {

enum class StreamingEndpoint {
  Primary,
  Backup,
  Both,
};

enum class LoggingLevel {
  Info,
  Warning,
  Error,
};

enum class Theme {
  System,
  Light,
  Dark,
};

enum class CaptionStyle {
  Standard,
  Compact,
  Uppercase,
};

enum class CaptionPlacement {
  Bottom,
  Top,
};

enum class CaptionAlignment {
  Center,
  Left,
  Right,
};

enum class WhisperModel {
  Tiny,
  Base,
};

struct PluginSettings {
  std::string kalturaSession;
  std::string selectedEntryId;
  std::string selectedEntryName;
  std::string selectedEntryDescription;
  std::string selectedEntryThumbnailUrl;
  int selectedEntryStatus = 0;
  StreamingEndpoint preferredEndpoint = StreamingEndpoint::Primary;
  LoggingLevel loggingLevel = LoggingLevel::Info;
  bool debugLogging = false;
  bool captionsEnabled = false;
  int captionDelayMs = 0;
  CaptionStyle captionStyle = CaptionStyle::Standard;
  CaptionPlacement captionPlacement = CaptionPlacement::Bottom;
  CaptionAlignment captionAlignment = CaptionAlignment::Center;
  WhisperModel whisperModel = WhisperModel::Tiny;
  std::vector<captions::CaptionDictionaryEntry> captionDictionary;
  Theme theme = Theme::System;
};

[[nodiscard]] bool isValidKalturaSession(std::string_view value);

class SettingsManager {
public:
  void load(obs_data_t *rootData);
  void save(obs_data_t *rootData) const;

  [[nodiscard]] const PluginSettings &settings() const;
  void update(const PluginSettings &updated);

private:
  PluginSettings settings_{};
};

}  // namespace kaltura_live
