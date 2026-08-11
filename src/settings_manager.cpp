#include "kaltura_live/settings_manager.hpp"
#include "kaltura_live/logger.hpp"
#include "kaltura_live/platform/platform.hpp"

#include <obs-module.h>

#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace {
constexpr const char *kSettingsRootKey = "kaltura_live";
constexpr const char *kKalturaSessionKey = "kaltura_session";
constexpr const char *kCredentialIdKey = "credential_id";
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
constexpr const char *kPrimaryOutputKey = "primary_output";
constexpr const char *kBackupOutputKey = "backup_output";
constexpr const char *kOutputCredentialIdKey = "credential_id";

bool validCredentialId(std::string_view value);

const char *protocolValue(kaltura_live::OutputProtocol protocol)
{
  switch (protocol) {
  case kaltura_live::OutputProtocol::RTMP: return "rtmp";
  case kaltura_live::OutputProtocol::RTMPS: return "rtmps";
  case kaltura_live::OutputProtocol::SRT: return "srt";
  }
  return "rtmps";
}

const char *srtModeValue(kaltura_live::SrtMode mode)
{
  switch (mode) {
  case kaltura_live::SrtMode::Caller: return "caller";
  case kaltura_live::SrtMode::Listener: return "listener";
  case kaltura_live::SrtMode::Rendezvous: return "rendezvous";
  }
  return "caller";
}

std::string serializeSecrets(const kaltura_live::StreamOutputConfig &config)
{
  QJsonObject object;
  object["endpoint"] = QString::fromUtf8(config.endpoint);
  object["kaltura_rtmp_endpoint"] = QString::fromUtf8(config.kalturaRtmpEndpoint);
  object["kaltura_rtmps_endpoint"] = QString::fromUtf8(config.kalturaRtmpsEndpoint);
  object["kaltura_srt_endpoint"] = QString::fromUtf8(config.kalturaSrtEndpoint);
  object["stream_key"] = QString::fromUtf8(config.streamKey);
  object["username"] = QString::fromUtf8(config.username);
  object["password"] = QString::fromUtf8(config.password);
  object["srt_passphrase"] = QString::fromUtf8(config.srt.passphrase);
  object["srt_stream_id"] = QString::fromUtf8(config.srt.streamId);
  object["kaltura_srt_stream_id"] = QString::fromUtf8(config.kalturaSrtStreamId);
  return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

void deserializeSecrets(const std::string &value, kaltura_live::StreamOutputConfig &config)
{
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(value), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return;
  const QJsonObject object = document.object();
  const auto restoreString = [&object](const char *key, std::string &destination) {
    const QString value = object[key].toString();
    if (!value.isNull()) destination = value.toUtf8().toStdString();
  };
  restoreString("endpoint", config.endpoint);
  restoreString("kaltura_rtmp_endpoint", config.kalturaRtmpEndpoint);
  restoreString("kaltura_rtmps_endpoint", config.kalturaRtmpsEndpoint);
  restoreString("kaltura_srt_endpoint", config.kalturaSrtEndpoint);
  config.streamKey = object["stream_key"].toString().toUtf8().toStdString();
  config.username = object["username"].toString().toUtf8().toStdString();
  config.password = object["password"].toString().toUtf8().toStdString();
  config.srt.passphrase = object["srt_passphrase"].toString().toUtf8().toStdString();
  config.srt.streamId = object["srt_stream_id"].toString().toUtf8().toStdString();
  config.kalturaSrtStreamId = object["kaltura_srt_stream_id"].toString().toUtf8().toStdString();
}

void loadOutput(obs_data_t *parent, const char *key, const char *defaultName,
                kaltura_live::StreamOutputConfig &config, std::string &credentialId,
                std::string &persistedSecrets,
                kaltura_live::platform::CredentialStore *credentialStore)
{
  config = kaltura_live::StreamOutputConfig{};
  config.name = defaultName;
  config.enabled = false;
  obs_data_t *data = obs_data_get_obj(parent, key);
  if (!data) return;
  config.enabled = obs_data_get_bool(data, "enabled");
  const std::string protocol = obs_data_get_string(data, "protocol");
  config.protocol = protocol == "srt" ? kaltura_live::OutputProtocol::SRT
    : protocol == "rtmp" ? kaltura_live::OutputProtocol::RTMP
                          : kaltura_live::OutputProtocol::RTMPS;
  config.endpoint = obs_data_get_string(data, "endpoint");
  config.manualOverride = obs_data_get_bool(data, "manual_override");
  config.kalturaRtmpEndpoint = obs_data_get_string(data, "kaltura_rtmp_endpoint");
  config.kalturaRtmpsEndpoint = obs_data_get_string(data, "kaltura_rtmps_endpoint");
  config.kalturaSrtEndpoint = obs_data_get_string(data, "kaltura_srt_endpoint");
  config.reconnect.enabled = obs_data_get_bool(data, "reconnect_enabled");
  config.reconnect.delaySeconds = std::clamp(static_cast<int>(obs_data_get_int(data, "reconnect_delay")), 1, 60);
  config.reconnect.maxRetries = std::clamp(static_cast<int>(obs_data_get_int(data, "reconnect_retries")), 0, 10'000);
  config.srt.host = obs_data_get_string(data, "srt_host");
  config.srt.port = static_cast<uint16_t>(std::clamp<int64_t>(obs_data_get_int(data, "srt_port"), 0, 65535));
  const std::string mode = obs_data_get_string(data, "srt_mode");
  config.srt.mode = mode == "listener" ? kaltura_live::SrtMode::Listener
    : mode == "rendezvous" ? kaltura_live::SrtMode::Rendezvous
                            : kaltura_live::SrtMode::Caller;
  const int storedLatency = static_cast<int>(obs_data_get_int(data, "srt_latency_ms"));
  config.srt.latencyMs = storedLatency < 250 ? 3'000
                                             : std::clamp(storedLatency, 250, 8'000);
  config.srt.pbkeylen = static_cast<int>(obs_data_get_int(data, "srt_pbkeylen"));
  config.srt.streamId = obs_data_get_string(data, "srt_stream_id"); // legacy migration
  config.srt.timeoutMs = std::clamp(static_cast<int>(obs_data_get_int(data, "srt_timeout_ms")), 0, 120'000);
  config.srt.packetSize = std::clamp(static_cast<int>(obs_data_get_int(data, "srt_packet_size")), 188, 65'536);
  credentialId = obs_data_get_string(data, kOutputCredentialIdKey);
  if (credentialStore && validCredentialId(credentialId)) {
    if (const auto stored = credentialStore->load(credentialId)) {
      persistedSecrets = *stored;
      deserializeSecrets(*stored, config);
    }
  }
  obs_data_release(data);
}

void saveOutput(obs_data_t *parent, const char *key,
                const kaltura_live::StreamOutputConfig &config,
                const std::string &credentialId)
{
  obs_data_t *data = obs_data_create();
  obs_data_set_bool(data, "enabled", config.enabled);
  obs_data_set_string(data, "protocol", protocolValue(config.protocol));
  const std::string safeEndpoint = kaltura_live::endpointWithoutSecrets(config.endpoint);
  const std::string safeRtmpEndpoint =
    kaltura_live::endpointWithoutSecrets(config.kalturaRtmpEndpoint);
  const std::string safeRtmpsEndpoint =
    kaltura_live::endpointWithoutSecrets(config.kalturaRtmpsEndpoint);
  const std::string safeSrtEndpoint =
    kaltura_live::endpointWithoutSecrets(config.kalturaSrtEndpoint);
  obs_data_set_string(data, "endpoint", safeEndpoint.c_str());
  obs_data_set_bool(data, "manual_override", config.manualOverride);
  obs_data_set_string(data, "kaltura_rtmp_endpoint", safeRtmpEndpoint.c_str());
  obs_data_set_string(data, "kaltura_rtmps_endpoint", safeRtmpsEndpoint.c_str());
  obs_data_set_string(data, "kaltura_srt_endpoint", safeSrtEndpoint.c_str());
  obs_data_set_bool(data, "reconnect_enabled", config.reconnect.enabled);
  obs_data_set_int(data, "reconnect_delay", config.reconnect.delaySeconds);
  obs_data_set_int(data, "reconnect_retries", config.reconnect.maxRetries);
  obs_data_set_string(data, "srt_host", config.srt.host.c_str());
  obs_data_set_int(data, "srt_port", config.srt.port);
  obs_data_set_string(data, "srt_mode", srtModeValue(config.srt.mode));
  obs_data_set_int(data, "srt_latency_ms", config.srt.latencyMs);
  obs_data_set_int(data, "srt_pbkeylen", config.srt.pbkeylen);
  obs_data_set_int(data, "srt_timeout_ms", config.srt.timeoutMs);
  obs_data_set_int(data, "srt_packet_size", config.srt.packetSize);
  if (!credentialId.empty()) obs_data_set_string(data, kOutputCredentialIdKey, credentialId.c_str());
  obs_data_set_obj(parent, key, data);
  obs_data_release(data);
}

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

bool validCredentialId(std::string_view value)
{
  return value.size() == 36 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= 'a' && character <= 'f') ||
                  (character >= '0' && character <= '9') || character == '-';
         });
}
}

namespace kaltura_live {

SettingsManager::SettingsManager()
  : credentialStore_(platform::createCredentialStore())
{
}

SettingsManager::~SettingsManager() = default;

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
  settings_.kalturaSession.clear();
  credentialId_.clear();
  persistedSession_.clear();
  primaryCredentialId_.clear();
  backupCredentialId_.clear();
  persistedPrimarySecrets_.clear();
  persistedBackupSecrets_.clear();
  if (!rootData) {
    return;
  }

  obs_data_t *pluginData = obs_data_get_obj(rootData, kSettingsRootKey);
  if (!pluginData) {
    return;
  }

  const std::string credentialId = obs_data_get_string(pluginData, kCredentialIdKey);
  if (validCredentialId(credentialId)) {
    credentialId_ = credentialId;
    if (credentialStore_) {
      const std::optional<std::string> stored = credentialStore_->load(credentialId_);
      if (stored && isValidKalturaSession(*stored)) {
        settings_.kalturaSession = *stored;
        persistedSession_ = *stored;
      }
    }
  }

  // Migrate legacy profile data once. The next OBS save omits this field even if
  // secure storage is unavailable, so a KS is never left in plaintext.
  const char *legacySession = obs_data_get_string(pluginData, kKalturaSessionKey);
  if (settings_.kalturaSession.empty() && legacySession &&
      isValidKalturaSession(legacySession)) {
    settings_.kalturaSession = legacySession;
    persistSession();
  }
  settings_.selectedEntryId = obs_data_get_string(pluginData, kSelectedEntryIdKey);
  settings_.selectedEntryName = obs_data_get_string(pluginData, kSelectedEntryNameKey);
  settings_.selectedEntryDescription =
    obs_data_get_string(pluginData, kSelectedEntryDescriptionKey);
  settings_.selectedEntryThumbnailUrl =
    obs_data_get_string(pluginData, kSelectedEntryThumbnailUrlKey);
  settings_.selectedEntryStatus =
    static_cast<int>(obs_data_get_int(pluginData, kSelectedEntryStatusKey));
  loadOutput(pluginData, kPrimaryOutputKey, "Primary", settings_.primaryOutput,
             primaryCredentialId_, persistedPrimarySecrets_, credentialStore_.get());
  loadOutput(pluginData, kBackupOutputKey, "Backup", settings_.backupOutput,
             backupCredentialId_, persistedBackupSecrets_, credentialStore_.get());
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
  // Migrate destination query tokens from legacy OBS project JSON into the
  // per-output secure credential record before the next project save strips
  // those query strings from plaintext persistence.
  persistOutputSecrets();
  obs_data_release(pluginData);
}

void SettingsManager::save(obs_data_t *rootData) const
{
  if (!rootData) {
    return;
  }

  obs_data_t *pluginData = obs_data_create();
  if (!credentialId_.empty()) {
    obs_data_set_string(pluginData, kCredentialIdKey, credentialId_.c_str());
  }
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
  saveOutput(pluginData, kPrimaryOutputKey, settings_.primaryOutput, primaryCredentialId_);
  saveOutput(pluginData, kBackupOutputKey, settings_.backupOutput, backupCredentialId_);
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
  const auto extractSrtSecrets = [](StreamOutputConfig &config) {
    if (config.protocol != OutputProtocol::SRT || config.endpoint.empty()) return;
    QUrl url(QString::fromUtf8(config.endpoint));
    QUrlQuery query(url);
    if (config.srt.passphrase.empty())
      config.srt.passphrase = query.queryItemValue("passphrase").toUtf8().toStdString();
    if (config.srt.streamId.empty())
      config.srt.streamId = query.queryItemValue("streamid").toUtf8().toStdString();
    query.removeAllQueryItems("passphrase");
    query.removeAllQueryItems("streamid");
    url.setQuery(query);
    config.endpoint = url.toString(QUrl::FullyEncoded).toUtf8().toStdString();
  };
  extractSrtSecrets(settings_.primaryOutput);
  extractSrtSecrets(settings_.backupOutput);
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
  persistSession();
  persistOutputSecrets();
  if (!settings_.selectedEntryId.empty() && !validEntryId(settings_.selectedEntryId)) {
    settings_.selectedEntryId.clear();
    settings_.selectedEntryName.clear();
    settings_.selectedEntryDescription.clear();
    settings_.selectedEntryThumbnailUrl.clear();
    settings_.selectedEntryStatus = 0;
  }
}

void SettingsManager::persistOutputSecrets()
{
  if (!credentialStore_) return;
  const auto persist = [this](const StreamOutputConfig &config, std::string &id,
                              std::string &persisted) {
    const std::string secrets = serializeSecrets(config);
    if (secrets == persisted) return;
    if (id.empty()) id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    if (credentialStore_->save(id, secrets)) persisted = secrets;
    else Logger::write(LogLevel::Warning,
      config.name + " output credentials could not be persisted securely.");
  };
  persist(settings_.primaryOutput, primaryCredentialId_, persistedPrimarySecrets_);
  persist(settings_.backupOutput, backupCredentialId_, persistedBackupSecrets_);
}

void SettingsManager::persistSession()
{
  if (!credentialStore_ || settings_.kalturaSession == persistedSession_) {
    return;
  }
  if (credentialId_.empty() && !settings_.kalturaSession.empty()) {
    credentialId_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  }
  const bool stored = settings_.kalturaSession.empty()
                        ? credentialId_.empty() || credentialStore_->remove(credentialId_)
                        : credentialStore_->save(credentialId_, settings_.kalturaSession);
  if (stored) {
    persistedSession_ = settings_.kalturaSession;
    if (settings_.kalturaSession.empty()) {
      credentialId_.clear();
    }
    Logger::write(LogLevel::Info,
                  std::string("Kaltura Session updated in ") +
                    credentialStore_->backendName());
  } else {
    Logger::write(LogLevel::Warning,
                  std::string("Kaltura Session could not be persisted securely using ") +
                    credentialStore_->backendName() +
                    "; it will remain available only for this OBS process");
  }
}

}  // namespace kaltura_live
