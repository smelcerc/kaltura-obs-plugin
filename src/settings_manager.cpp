#include "kaltura_live/settings_manager.hpp"

#include <obs-module.h>

#include <algorithm>

namespace {
constexpr const char *kSettingsRootKey = "kaltura_live";
constexpr const char *kKalturaSessionKey = "kaltura_session";
constexpr const char *kSelectedEntryIdKey = "selected_entry_id";
constexpr const char *kSelectedEntryNameKey = "selected_entry_name";
constexpr const char *kSelectedEntryDescriptionKey = "selected_entry_description";
constexpr const char *kSelectedEntryThumbnailUrlKey = "selected_entry_thumbnail_url";
constexpr const char *kSelectedEntryStatusKey = "selected_entry_status";
constexpr const char *kPreferredEndpointKey = "preferred_endpoint";
constexpr const char *kLoggingLevelKey = "logging_level";
constexpr const char *kDebugLoggingKey = "debug_logging";
constexpr const char *kCaptionsEnabledKey = "captions_enabled";
constexpr const char *kCaptionDelayMsKey = "caption_delay_ms";
constexpr const char *kCaptionStyleKey = "caption_style";
constexpr const char *kCaptionPlacementKey = "caption_placement";
constexpr const char *kCaptionAlignmentKey = "caption_alignment";
constexpr const char *kWhisperModelKey = "whisper_model";
constexpr const char *kCaptionDictionaryKey = "caption_dictionary";
constexpr const char *kDictionarySpokenFormKey = "spoken_form";
constexpr const char *kDictionaryPreferredTextKey = "preferred_text";
constexpr const char *kThemeKey = "theme";
constexpr const char *kVerboseLoggingKey = "verbose_logging";

const char *endpointValue(kaltura_live::StreamingEndpoint endpoint)
{
  switch (endpoint) {
  case kaltura_live::StreamingEndpoint::Primary:
    return "primary";
  case kaltura_live::StreamingEndpoint::Backup:
    return "backup";
  case kaltura_live::StreamingEndpoint::Both:
    return "both";
  }
  return "primary";
}

const char *loggingLevelValue(kaltura_live::LoggingLevel level)
{
  switch (level) {
  case kaltura_live::LoggingLevel::Info:
    return "info";
  case kaltura_live::LoggingLevel::Warning:
    return "warning";
  case kaltura_live::LoggingLevel::Error:
    return "error";
  }
  return "info";
}

const char *themeValue(kaltura_live::Theme theme)
{
  switch (theme) {
  case kaltura_live::Theme::System:
    return "system";
  case kaltura_live::Theme::Light:
    return "light";
  case kaltura_live::Theme::Dark:
    return "dark";
  }
  return "system";
}

const char *captionStyleValue(kaltura_live::CaptionStyle style)
{
  switch (style) {
  case kaltura_live::CaptionStyle::Standard: return "standard";
  case kaltura_live::CaptionStyle::Compact: return "compact";
  case kaltura_live::CaptionStyle::Uppercase: return "uppercase";
  }
  return "standard";
}

const char *captionPlacementValue(kaltura_live::CaptionPlacement placement)
{
  return placement == kaltura_live::CaptionPlacement::Top ? "top" : "bottom";
}

const char *captionAlignmentValue(kaltura_live::CaptionAlignment alignment)
{
  switch (alignment) {
  case kaltura_live::CaptionAlignment::Left: return "left";
  case kaltura_live::CaptionAlignment::Right: return "right";
  case kaltura_live::CaptionAlignment::Center: return "center";
  }
  return "center";
}

bool validEntryId(std::string_view value)
{
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '_' ||
                  character == '-';
         });
}

bool validDictionaryField(std::string_view value, bool allowEmpty = false)
{
  return (allowEmpty || !value.empty()) && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return character >= 0x20 && character != 0x7f;
         });
}
}

namespace kaltura_live {

bool isValidKalturaSession(std::string_view value)
{
  if (value.size() < 10 || value.size() > 4096) {
    return false;
  }

  for (const unsigned char character : value) {
    if (character < 0x21 || character > 0x7e) {
      return false;
    }
  }
  return true;
}

void SettingsManager::load(obs_data_t *rootData)
{
  if (!rootData) {
    return;
  }

  obs_data_t *pluginData = obs_data_get_obj(rootData, kSettingsRootKey);
  if (!pluginData) {
    return;
  }

  const char *session = obs_data_get_string(pluginData, kKalturaSessionKey);
  settings_.kalturaSession = session && isValidKalturaSession(session) ? session : "";
  settings_.selectedEntryId = obs_data_get_string(pluginData, kSelectedEntryIdKey);
  settings_.selectedEntryName = obs_data_get_string(pluginData, kSelectedEntryNameKey);
  settings_.selectedEntryDescription =
    obs_data_get_string(pluginData, kSelectedEntryDescriptionKey);
  settings_.selectedEntryThumbnailUrl =
    obs_data_get_string(pluginData, kSelectedEntryThumbnailUrlKey);
  settings_.selectedEntryStatus =
    static_cast<int>(obs_data_get_int(pluginData, kSelectedEntryStatusKey));
  if (!settings_.selectedEntryId.empty() && !validEntryId(settings_.selectedEntryId)) {
    settings_.selectedEntryId.clear();
    settings_.selectedEntryName.clear();
    settings_.selectedEntryDescription.clear();
    settings_.selectedEntryThumbnailUrl.clear();
    settings_.selectedEntryStatus = 0;
  }

  const std::string endpoint = obs_data_get_string(pluginData, kPreferredEndpointKey);
  if (endpoint == "backup") {
    settings_.preferredEndpoint = StreamingEndpoint::Backup;
  } else if (endpoint == "both") {
    settings_.preferredEndpoint = StreamingEndpoint::Both;
  } else {
    settings_.preferredEndpoint = StreamingEndpoint::Primary;
  }

  const std::string loggingLevel = obs_data_get_string(pluginData, kLoggingLevelKey);
  if (loggingLevel == "warning") {
    settings_.loggingLevel = LoggingLevel::Warning;
  } else if (loggingLevel == "error") {
    settings_.loggingLevel = LoggingLevel::Error;
  } else {
    settings_.loggingLevel = LoggingLevel::Info;
  }

  settings_.debugLogging = obs_data_has_user_value(pluginData, kDebugLoggingKey)
                             ? obs_data_get_bool(pluginData, kDebugLoggingKey)
                             : obs_data_get_bool(pluginData, kVerboseLoggingKey);
  settings_.captionsEnabled = obs_data_get_bool(pluginData, kCaptionsEnabledKey);
  settings_.captionDelayMs = std::clamp(
    static_cast<int>(obs_data_get_int(pluginData, kCaptionDelayMsKey)), 0, 10'000);
  const std::string captionStyle = obs_data_get_string(pluginData, kCaptionStyleKey);
  if (captionStyle == "compact") {
    settings_.captionStyle = CaptionStyle::Compact;
  } else if (captionStyle == "uppercase") {
    settings_.captionStyle = CaptionStyle::Uppercase;
  } else {
    settings_.captionStyle = CaptionStyle::Standard;
  }
  settings_.captionPlacement =
    std::string(obs_data_get_string(pluginData, kCaptionPlacementKey)) == "top"
      ? CaptionPlacement::Top : CaptionPlacement::Bottom;
  const std::string captionAlignment =
    obs_data_get_string(pluginData, kCaptionAlignmentKey);
  if (captionAlignment == "left") {
    settings_.captionAlignment = CaptionAlignment::Left;
  } else if (captionAlignment == "right") {
    settings_.captionAlignment = CaptionAlignment::Right;
  } else {
    settings_.captionAlignment = CaptionAlignment::Center;
  }
  const std::string whisperModel = obs_data_get_string(pluginData, kWhisperModelKey);
  settings_.whisperModel = whisperModel == "base" ? WhisperModel::Base
                                                   : WhisperModel::Tiny;
  settings_.captionDictionary.clear();
  obs_data_array_t *dictionary = obs_data_get_array(pluginData, kCaptionDictionaryKey);
  if (dictionary) {
    const size_t count = std::min<size_t>(obs_data_array_count(dictionary), 250);
    for (size_t index = 0; index < count; ++index) {
      obs_data_t *item = obs_data_array_item(dictionary, index);
      if (!item) {
        continue;
      }
      captions::CaptionDictionaryEntry entry{
        obs_data_get_string(item, kDictionarySpokenFormKey),
        obs_data_get_string(item, kDictionaryPreferredTextKey)};
      if (validDictionaryField(entry.spokenForm, true) &&
          validDictionaryField(entry.preferredText)) {
        settings_.captionDictionary.push_back(std::move(entry));
      }
      obs_data_release(item);
    }
    obs_data_array_release(dictionary);
  }

  const std::string theme = obs_data_get_string(pluginData, kThemeKey);
  if (theme == "light") {
    settings_.theme = Theme::Light;
  } else if (theme == "dark") {
    settings_.theme = Theme::Dark;
  } else {
    settings_.theme = Theme::System;
  }
  obs_data_release(pluginData);
}

void SettingsManager::save(obs_data_t *rootData) const
{
  if (!rootData) {
    return;
  }

  obs_data_t *pluginData = obs_data_create();
  obs_data_set_string(pluginData, kKalturaSessionKey, settings_.kalturaSession.c_str());
  obs_data_set_string(pluginData, kSelectedEntryIdKey, settings_.selectedEntryId.c_str());
  obs_data_set_string(pluginData, kSelectedEntryNameKey, settings_.selectedEntryName.c_str());
  obs_data_set_string(pluginData, kSelectedEntryDescriptionKey,
                      settings_.selectedEntryDescription.c_str());
  obs_data_set_string(pluginData, kSelectedEntryThumbnailUrlKey,
                      settings_.selectedEntryThumbnailUrl.c_str());
  obs_data_set_int(pluginData, kSelectedEntryStatusKey, settings_.selectedEntryStatus);
  obs_data_set_string(pluginData, kPreferredEndpointKey,
                      endpointValue(settings_.preferredEndpoint));
  obs_data_set_string(pluginData, kLoggingLevelKey,
                      loggingLevelValue(settings_.loggingLevel));
  obs_data_set_bool(pluginData, kDebugLoggingKey, settings_.debugLogging);
  obs_data_set_bool(pluginData, kCaptionsEnabledKey, settings_.captionsEnabled);
  obs_data_set_int(pluginData, kCaptionDelayMsKey, settings_.captionDelayMs);
  obs_data_set_string(pluginData, kCaptionStyleKey,
                      captionStyleValue(settings_.captionStyle));
  obs_data_set_string(pluginData, kCaptionPlacementKey,
                      captionPlacementValue(settings_.captionPlacement));
  obs_data_set_string(pluginData, kCaptionAlignmentKey,
                      captionAlignmentValue(settings_.captionAlignment));
  obs_data_set_string(pluginData, kWhisperModelKey,
                      settings_.whisperModel == WhisperModel::Base ? "base" : "tiny");
  obs_data_array_t *dictionary = obs_data_array_create();
  for (const captions::CaptionDictionaryEntry &entry : settings_.captionDictionary) {
    if (!validDictionaryField(entry.spokenForm, true) ||
        !validDictionaryField(entry.preferredText)) {
      continue;
    }
    obs_data_t *item = obs_data_create();
    obs_data_set_string(item, kDictionarySpokenFormKey, entry.spokenForm.c_str());
    obs_data_set_string(item, kDictionaryPreferredTextKey, entry.preferredText.c_str());
    obs_data_array_push_back(dictionary, item);
    obs_data_release(item);
  }
  obs_data_set_array(pluginData, kCaptionDictionaryKey, dictionary);
  obs_data_array_release(dictionary);
  obs_data_set_string(pluginData, kThemeKey, themeValue(settings_.theme));
  obs_data_set_obj(rootData, kSettingsRootKey, pluginData);
  obs_data_release(pluginData);
}

const PluginSettings &SettingsManager::settings() const
{
  return settings_;
}

void SettingsManager::update(const PluginSettings &updated)
{
  settings_ = updated;
  settings_.captionDelayMs = std::clamp(settings_.captionDelayMs, 0, 10'000);
  if (settings_.captionDictionary.size() > 250) {
    settings_.captionDictionary.resize(250);
  }
  std::erase_if(settings_.captionDictionary, [](const auto &entry) {
    return !validDictionaryField(entry.spokenForm, true) ||
           !validDictionaryField(entry.preferredText);
  });
  if (!settings_.kalturaSession.empty() && !isValidKalturaSession(settings_.kalturaSession)) {
    settings_.kalturaSession.clear();
  }
  if (!settings_.selectedEntryId.empty() && !validEntryId(settings_.selectedEntryId)) {
    settings_.selectedEntryId.clear();
    settings_.selectedEntryName.clear();
    settings_.selectedEntryDescription.clear();
    settings_.selectedEntryThumbnailUrl.clear();
    settings_.selectedEntryStatus = 0;
  }
}

}  // namespace kaltura_live
