#pragma once

#include "kaltura_live/captions/caption_dictionary.hpp"
#include "kaltura_live/stream_output_config.hpp"

#include <obs-data.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kaltura_live {

namespace platform {
class CredentialStore;
}

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
  std::int64_t partnerId = 0;
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
  StreamOutputConfig primaryOutput{.name = "Primary", .enabled = false};
  StreamOutputConfig backupOutput{.name = "Backup", .enabled = false};
};

[[nodiscard]] bool isValidKalturaSession(std::string_view value);

class SettingsManager {
public:
  SettingsManager();
  ~SettingsManager();
  SettingsManager(const SettingsManager &) = delete;
  SettingsManager &operator=(const SettingsManager &) = delete;

  void load(obs_data_t *rootData);
  void save(obs_data_t *rootData) const;

  [[nodiscard]] const PluginSettings &settings() const;
  void update(const PluginSettings &updated);

private:
  void persistSession();
  void persistOutputSecrets();

  PluginSettings settings_{};
  std::unique_ptr<platform::CredentialStore> credentialStore_;
  std::string credentialId_;
  std::string persistedSession_;
  std::string primaryCredentialId_;
  std::string backupCredentialId_;
  std::string persistedPrimarySecrets_;
  std::string persistedBackupSecrets_;
};

}  // namespace kaltura_live
