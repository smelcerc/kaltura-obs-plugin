#include "kaltura_live/streaming_manager.hpp"

#include "kaltura_live/captions/native_caption_text.hpp"
#include "kaltura_live/logger.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string_view>
#include <utility>

namespace kaltura_live {

namespace {
constexpr int kLiveKeyframeIntervalSeconds = 2;

void enforceLiveEncoderSettings(obs_output_t *output)
{
  if (!output) return;
  obs_encoder_t *videoEncoder = obs_output_get_video_encoder(output);
  if (!videoEncoder) {
    Logger::write(LogLevel::Warning,
                  "Could not enforce the live keyframe interval: OBS video encoder unavailable");
    return;
  }

  obs_data_t *settings = obs_encoder_get_settings(videoEncoder);
  if (!settings) {
    Logger::write(LogLevel::Warning,
                  "Could not enforce the live keyframe interval: encoder settings unavailable");
    return;
  }

  const int previousInterval = static_cast<int>(obs_data_get_int(settings, "keyint_sec"));
  obs_data_set_int(settings, "keyint_sec", kLiveKeyframeIntervalSeconds);
  obs_encoder_update(videoEncoder, settings);
  obs_data_release(settings);

  Logger::write(LogLevel::Info,
                "OBS live video encoder keyframe interval set to 2 seconds" +
                  (previousInterval > 0
                     ? " (was " + std::to_string(previousInterval) + " seconds)"
                     : std::string{}));
}
}  // namespace

class StreamingManager::Impl {
public:
  struct Slot {
    StreamOutputConfig config;
    obs_output_t *output = nullptr;
    obs_service_t *service = nullptr;
    obs_encoder_t *videoEncoder = nullptr;
    obs_encoder_t *audioEncoder = nullptr;
    bool signalConnected = false;
    std::atomic<uint64_t> reconnectAttempts{0};
    OutputHealthState state = OutputHealthState::Disabled;
    std::string error;
    bool requested = false;
    uint64_t sampledBytes = 0;
    uint64_t sampledBitrate = 0;
    std::chrono::steady_clock::time_point sampledAt{};
    std::chrono::steady_clock::time_point startedAt{};
  };

  Impl()
  {
    primary.config.name = "Primary";
    backup.config.name = "Backup";
  }

  ~Impl() { shutdown(); }

  bool configure(const api::StreamConfiguration &source, StreamingEndpoint endpoint,
                 std::string &failure)
  {
    StreamOutputConfig primaryConfig = mapKalturaOutput(source, OutputRole::Primary);
    StreamOutputConfig backupConfig = mapKalturaOutput(source, OutputRole::Backup);
    primaryConfig.enabled = endpoint != StreamingEndpoint::Backup;
    backupConfig.enabled = endpoint != StreamingEndpoint::Primary;
    return configureSlot(primary, primaryConfig, failure) &&
           configureSlot(backup, backupConfig, failure);
  }

  bool configureOutput(OutputRole role, const StreamOutputConfig &config,
                       std::string &failure)
  {
    return configureSlot(slot(role), config, failure);
  }

  StreamOutputConfig outputConfiguration(OutputRole role) const
  {
    return role == OutputRole::Primary ? primary.config : backup.config;
  }

  void clearConfiguration()
  {
    if (hasFrontendRole && obs_frontend_streaming_active())
      obs_frontend_streaming_stop();
    releaseAuxiliary(true);
    releaseFrontendTracking();
    hasFrontendRole = false;
    pendingBackup = false;
    primary.config = StreamOutputConfig{.name = "Primary", .enabled = false};
    backup.config = StreamOutputConfig{.name = "Backup", .enabled = false};
    primary.state = OutputHealthState::Disabled;
    backup.state = OutputHealthState::Disabled;
    primary.error.clear();
    backup.error.clear();
  }

  void setProgramDelay(int delayMs)
  {
    delaySeconds = static_cast<uint32_t>(std::clamp((delayMs + 999) / 1000, 0, 10));
    if (!obs_frontend_streaming_active()) {
      if (obs_output_t *frontend = obs_frontend_get_streaming_output()) {
        obs_output_set_delay(frontend, delaySeconds, OBS_OUTPUT_DELAY_PRESERVE);
        obs_output_release(frontend);
      }
    }
    if (backup.output && !obs_output_active(backup.output))
      obs_output_set_delay(backup.output, delaySeconds, OBS_OUTPUT_DELAY_PRESERVE);
  }

  void onStreamingStarting()
  {
    if (!hasFrontendRole) {
      frontendRole = OutputRole::Primary;
      hasFrontendRole = true;
    }
    Slot &item = slot(frontendRole);
    item.requested = true;
    item.state = OutputHealthState::Starting;
    item.error.clear();
    item.startedAt = std::chrono::steady_clock::now();

    // OBS has finished creating/configuring the frontend encoders by the
    // STREAMING_STARTING event, but has not started them yet. Enforce a
    // broadcast-safe GOP here so it applies to the built-in stream and is
    // inherited by a subsequently cloned SRT backup encoder.
    releaseFrontendTracking();
    if (obs_output_t *frontend = obs_frontend_get_streaming_output()) {
      enforceLiveEncoderSettings(frontend);
      signal_handler_connect(obs_output_get_signal_handler(frontend), "reconnect",
                             &Impl::onReconnect, &item);
      trackedFrontendOutput = frontend;
      trackedFrontendSlot = &item;
      frontendSignalConnected = true;
    }
  }

  void onStreamingStarted()
  {
    if (!hasFrontendRole) return;
    slot(frontendRole).state = OutputHealthState::Live;
    if (pendingBackup && frontendRole == OutputRole::Primary) {
      pendingBackup = false;
      std::string failure;
      if (!startAuxiliary(failure)) {
        backup.requested = false;
        backup.state = OutputHealthState::Error;
        backup.error = failure;
        Logger::write(LogLevel::Error, "Backup output failed after Primary started: " + failure);
      }
    }
  }

  void onStreamingStopping()
  {
    if (hasFrontendRole) slot(frontendRole).state = OutputHealthState::Stopping;
  }

  void onStreamingStopped()
  {
    if (!hasFrontendRole) return;
    Slot &item = slot(frontendRole);
    item.requested = false;
    item.state = item.config.enabled ? OutputHealthState::Idle : OutputHealthState::Disabled;
    pendingBackup = false;
    releaseFrontendTracking();
  }

  void shutdown()
  {
    pendingBackup = false;
    primary.requested = false;
    backup.requested = false;
    releaseAuxiliary(true);
    if (hasFrontendRole && obs_frontend_streaming_active())
      obs_frontend_streaming_stop();
    releaseFrontendTracking();
    hasFrontendRole = false;
  }

  bool startPrimary(std::string &failure)
  {
    if (!primary.config.enabled) {
      failure = "Primary is disabled.";
      return false;
    }
    if (obs_frontend_streaming_active() || primary.requested) {
      if (hasFrontendRole && frontendRole == OutputRole::Primary) return true;
      failure = "Backup currently owns the OBS streaming output.";
      return false;
    }
    frontendRole = OutputRole::Primary;
    hasFrontendRole = true;
    primary.requested = true;
    primary.state = OutputHealthState::Starting;
    primary.error.clear();
    primary.startedAt = std::chrono::steady_clock::now();
    obs_frontend_streaming_start();
    Logger::write(LogLevel::Info, "Primary start requested through OBS streaming");
    return true;
  }

  bool startBackup(std::string &failure)
  {
    if (!backup.config.enabled) {
      failure = "Backup is disabled.";
      return false;
    }
    if (backup.output && obs_output_active(backup.output)) {
      backup.requested = true;
      return true;
    }
    if (hasFrontendRole && frontendRole == OutputRole::Primary) {
      backup.requested = true;
      backup.state = OutputHealthState::Starting;
      if (!obs_frontend_streaming_active()) {
        pendingBackup = true;
        Logger::write(LogLevel::Info, "Backup is waiting for Primary OBS encoders");
        return true;
      }
      return startAuxiliary(failure);
    }
    if (obs_frontend_streaming_active()) {
      failure = "The OBS streaming output is already in use.";
      return false;
    }
    frontendRole = OutputRole::Backup;
    hasFrontendRole = true;
    backup.requested = true;
    backup.state = OutputHealthState::Starting;
    backup.error.clear();
    backup.startedAt = std::chrono::steady_clock::now();
    obs_frontend_streaming_start();
    Logger::write(LogLevel::Info, "Backup start requested through OBS streaming");
    return true;
  }

  bool stopPrimary(std::string &failure)
  {
    (void)failure;
    primary.requested = false;
    if (hasFrontendRole && frontendRole == OutputRole::Primary) {
      pendingBackup = false;
      if (obs_frontend_streaming_active()) {
        primary.state = OutputHealthState::Stopping;
        obs_frontend_streaming_stop();
      } else {
        primary.state = OutputHealthState::Idle;
      }
    }
    return true;
  }

  bool stopBackup(std::string &failure)
  {
    (void)failure;
    pendingBackup = false;
    backup.requested = false;
    if (hasFrontendRole && frontendRole == OutputRole::Backup) {
      if (obs_frontend_streaming_active()) {
        backup.state = OutputHealthState::Stopping;
        obs_frontend_streaming_stop();
      } else {
        backup.state = OutputHealthState::Idle;
      }
      return true;
    }
    if (backup.output && obs_output_active(backup.output)) {
      backup.state = OutputHealthState::Stopping;
      obs_output_stop(backup.output);
    } else {
      backup.state = OutputHealthState::Idle;
    }
    return true;
  }

  bool anyOutputActive() const
  {
    return (hasFrontendRole && obs_frontend_streaming_active()) ||
           (backup.output && obs_output_active(backup.output));
  }

  bool anyOutputRequested() const { return primary.requested || backup.requested; }

  StreamingHealth health()
  {
    obs_output_t *frontend = hasFrontendRole ? obs_frontend_get_streaming_output() : nullptr;
    StreamingHealth result;
    result.primary = readHealth(primary,
      hasFrontendRole && frontendRole == OutputRole::Primary ? frontend : nullptr);
    result.backup = readHealth(backup,
      hasFrontendRole && frontendRole == OutputRole::Backup ? frontend : backup.output);
    if (frontend) obs_output_release(frontend);
    return result;
  }

  CaptionDeliveryResult sendCaption(const std::string &text, double displayDurationSeconds,
                                    CaptionPlacement placement, CaptionAlignment alignment)
  {
    CaptionDeliveryResult result;
    if (text.empty() || text.size() > 2'048) {
      result.error = "Caption text was empty or exceeded the safe size limit.";
      return result;
    }
    result.nativeTimed = true;
    const auto prepared = captions::prepareNativeCaptionText(text, placement);
    const double duration = std::clamp(displayDurationSeconds, 0.5, 10.0);
    (void)alignment;
    auto queue = [&](obs_output_t *output, bool isPrimary) {
      if (!output || !obs_output_active(output)) return;
      obs_encoder_t *encoder = obs_output_get_video_encoder(output);
      const char *codec = encoder ? obs_encoder_get_codec(encoder) : nullptr;
      if (!codec || (std::string_view(codec) != "h264" &&
                     std::string_view(codec) != "hevc" &&
                     std::string_view(codec) != "av1")) return;
      for (int copy = 1; copy < 3; ++copy)
        obs_output_output_caption_text2(output, prepared.text.c_str(), 0.10);
      obs_output_output_caption_text2(output, prepared.text.c_str(), duration);
      ++result.outputsQueued;
      result.packetCount += 3;
      if (isPrimary) result.primaryQueued = true;
      else result.backupQueued = true;
    };
    obs_output_t *frontend = hasFrontendRole ? obs_frontend_get_streaming_output() : nullptr;
    queue(frontend, frontendRole == OutputRole::Primary);
    queue(backup.output, false);
    if (frontend) obs_output_release(frontend);
    if (!result.outputsQueued)
      result.error = "No active OBS output with a supported video encoder was available for captions.";
    return result;
  }

private:
  Slot &slot(OutputRole role) { return role == OutputRole::Primary ? primary : backup; }

  bool configureSlot(Slot &item, StreamOutputConfig config, std::string &failure)
  {
    const bool ownsFrontend = hasFrontendRole && &slot(frontendRole) == &item;
    if ((ownsFrontend && obs_frontend_streaming_active()) ||
        (item.output && obs_output_active(item.output))) {
      failure = "Stop " + item.config.name + " before changing its protocol.";
      return false;
    }
    if (config.name.empty()) config.name = item.config.name;
    if (config.protocol != OutputProtocol::SRT && config.streamKey.empty())
      config.streamKey = "1";
    if (!validateOutputConfig(config, failure)) return false;
    if (&item == &backup) releaseAuxiliary(false);
    item.config = std::move(config);
    item.state = item.config.enabled ? OutputHealthState::Idle : OutputHealthState::Disabled;
    item.error.clear();
    return true;
  }

  bool startAuxiliary(std::string &failure)
  {
    if (backup.output && obs_output_active(backup.output)) return true;
    releaseAuxiliary(false);
    obs_output_t *frontend = obs_frontend_get_streaming_output();
    if (!frontend || !obs_output_active(frontend)) {
      if (frontend) obs_output_release(frontend);
      failure = "Backup is waiting for the Primary OBS stream to start.";
      return false;
    }
    obs_encoder_t *video = obs_output_get_video_encoder(frontend);
    obs_encoder_t *audio = obs_output_get_audio_encoder(frontend, 0);
    if (!video || !audio) {
      obs_output_release(frontend);
      failure = "Primary OBS streaming encoders are unavailable.";
      return false;
    }
    if (backup.config.protocol == OutputProtocol::SRT) {
      obs_data_t *videoSettings = obs_encoder_get_settings(video);
      obs_data_t *audioSettings = obs_encoder_get_settings(audio);
#if LIBOBS_API_MAJOR_VER >= 32
      const size_t audioMixerIndex = obs_encoder_get_mixer_index(audio);
#else
      const size_t audioMixerIndex = obs_output_get_mixer(frontend);
#endif
      backup.videoEncoder = obs_video_encoder_create(
        obs_encoder_get_id(video), "kaltura_backup_video", videoSettings, nullptr);
      backup.audioEncoder = obs_audio_encoder_create(
        obs_encoder_get_id(audio), "kaltura_backup_audio", audioSettings,
        audioMixerIndex, nullptr);
      if (videoSettings) obs_data_release(videoSettings);
      if (audioSettings) obs_data_release(audioSettings);
      if (!backup.videoEncoder || !backup.audioEncoder) {
        obs_output_release(frontend);
        failure = "OBS could not create dedicated SRT encoders for Backup.";
        releaseAuxiliary(false);
        return false;
      }
      obs_encoder_set_video(backup.videoEncoder, obs_encoder_video(video));
      obs_encoder_set_audio(backup.audioEncoder, obs_encoder_audio(audio));
      obs_encoder_set_scaled_size(backup.videoEncoder, obs_encoder_get_width(video),
                                  obs_encoder_get_height(video));
      video = backup.videoEncoder;
      audio = backup.audioEncoder;
    }
    obs_data_t *outputSettings = obs_output_get_settings(frontend);
    backup.output = obs_output_create(obsOutputType(backup.config.protocol),
      "kaltura_backup_output", outputSettings, nullptr);
    if (outputSettings) obs_data_release(outputSettings);
    if (!backup.output) {
      obs_output_release(frontend);
      failure = "OBS could not create the native Backup output.";
      return false;
    }
    obs_data_t *serviceSettings = obs_data_create();
    const std::string server = backup.config.protocol == OutputProtocol::SRT
      ? buildSrtUri(backup.config) : backup.config.endpoint;
    obs_data_set_string(serviceSettings, "server", server.c_str());
    obs_data_set_string(serviceSettings, "key",
      backup.config.protocol == OutputProtocol::SRT ? ""
                                                    : backup.config.streamKey.c_str());
    const bool auth = backup.config.protocol == OutputProtocol::SRT
      ? !backup.config.srt.passphrase.empty()
      : (!backup.config.username.empty() && !backup.config.password.empty());
    obs_data_set_bool(serviceSettings, "use_auth", auth);
    obs_data_set_bool(serviceSettings, "bwtest", false);
    obs_data_set_string(serviceSettings, "username", backup.config.username.c_str());
    obs_data_set_string(serviceSettings, "password",
      backup.config.protocol == OutputProtocol::SRT ? backup.config.srt.passphrase.c_str()
                                                    : backup.config.password.c_str());
    backup.service = obs_service_create("rtmp_custom", "kaltura_backup_service",
                                        serviceSettings, nullptr);
    obs_data_release(serviceSettings);
    if (!backup.service) {
      obs_output_release(frontend);
      failure = "OBS could not create the Backup streaming service.";
      releaseAuxiliary(false);
      return false;
    }
    obs_output_set_service(backup.output, backup.service);
    obs_output_set_video_encoder(backup.output, video);
    obs_output_set_audio_encoder(backup.output, audio, 0);
    obs_output_set_reconnect_settings(backup.output,
      backup.config.reconnect.enabled ? backup.config.reconnect.maxRetries : 0,
      backup.config.reconnect.delaySeconds);
    obs_output_set_delay(backup.output, delaySeconds, OBS_OUTPUT_DELAY_PRESERVE);
    signal_handler_connect(obs_output_get_signal_handler(backup.output), "reconnect",
                           &Impl::onReconnect, &backup);
    backup.signalConnected = true;
    obs_output_release(frontend);
    backup.state = OutputHealthState::Starting;
    backup.error.clear();
    backup.startedAt = std::chrono::steady_clock::now();
    if (!obs_output_start(backup.output)) {
      const char *last = obs_output_get_last_error(backup.output);
      failure = last && *last ? last : "Backup could not start.";
      backup.state = OutputHealthState::Error;
      backup.error = failure;
      backup.requested = false;
      return false;
    }
    backup.requested = true;
    Logger::write(LogLevel::Info, "Backup auxiliary " +
                  std::string(outputProtocolName(backup.config.protocol)) + " output started");
    return true;
  }

  static void onReconnect(void *data, calldata_t *)
  {
    if (auto *item = static_cast<Slot *>(data)) {
      const uint64_t attempt = ++item->reconnectAttempts;
      Logger::write(LogLevel::Warning,
                    item->config.name + " stream reconnect attempt " +
                      std::to_string(attempt) + " (" +
                      outputProtocolName(item->config.protocol) + ")");
    }
  }

  OutputHealth readHealth(Slot &item, obs_output_t *output)
  {
    OutputHealth result;
    result.state = item.state;
    result.protocol = item.config.protocol;
    result.endpoint = item.config.protocol == OutputProtocol::SRT
      ? buildSrtUri(item.config) : item.config.endpoint;
    result.lastError = item.error;
    if (!output) return result;
    const auto now = std::chrono::steady_clock::now();
    result.bytesSent = obs_output_get_total_bytes(output);
    if (item.sampledAt.time_since_epoch().count() && result.bytesSent >= item.sampledBytes) {
      const double seconds = std::chrono::duration<double>(now - item.sampledAt).count();
      if (seconds > 0) item.sampledBitrate = static_cast<uint64_t>(
        ((result.bytesSent - item.sampledBytes) * 8.0) / seconds / 1000.0);
    }
    item.sampledBytes = result.bytesSent;
    item.sampledAt = now;
    result.bitrateKbps = item.sampledBitrate;
    result.droppedFrames = obs_output_get_frames_dropped(output);
    result.totalFrames = obs_output_get_total_frames(output);
    result.reconnectAttempts = item.reconnectAttempts.load();
    result.latencyMs = std::max(0, obs_output_get_connect_time_ms(output));
    result.congestion = std::clamp(obs_output_get_congestion(output), 0.0F, 1.0F);
    const bool active = obs_output_active(output);
    if (active && item.startedAt.time_since_epoch().count())
      result.elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - item.startedAt).count();
    if (obs_output_reconnecting(output)) result.state = OutputHealthState::Reconnecting;
    else if (active) result.state = OutputHealthState::Live;
    else if (item.state == OutputHealthState::Stopping) {
      item.state = item.config.enabled ? OutputHealthState::Idle : OutputHealthState::Disabled;
      result.state = item.state;
    }
    result.connected = active && !obs_output_reconnecting(output);
    if (const char *last = obs_output_get_last_error(output); last && *last) {
      result.lastError = last;
      if (!active && item.requested) result.state = OutputHealthState::Error;
    }
    return result;
  }

  void releaseAuxiliary(bool force)
  {
    pendingBackup = false;
    if (backup.output) {
      if (obs_output_active(backup.output)) {
        if (force) obs_output_force_stop(backup.output);
        else obs_output_stop(backup.output);
      }
      if (backup.signalConnected) {
        signal_handler_disconnect(obs_output_get_signal_handler(backup.output), "reconnect",
                                  &Impl::onReconnect, &backup);
        backup.signalConnected = false;
      }
      obs_output_release(backup.output);
      backup.output = nullptr;
    }
    if (backup.service) {
      obs_service_release(backup.service);
      backup.service = nullptr;
    }
    if (backup.videoEncoder) {
      obs_encoder_release(backup.videoEncoder);
      backup.videoEncoder = nullptr;
    }
    if (backup.audioEncoder) {
      obs_encoder_release(backup.audioEncoder);
      backup.audioEncoder = nullptr;
    }
    backup.sampledBytes = 0;
    backup.sampledBitrate = 0;
    backup.sampledAt = {};
    backup.reconnectAttempts = 0;
  }

  void releaseFrontendTracking()
  {
    if (!trackedFrontendOutput) return;
    if (frontendSignalConnected) {
      signal_handler_disconnect(obs_output_get_signal_handler(trackedFrontendOutput), "reconnect",
                                &Impl::onReconnect,
                                trackedFrontendSlot ? trackedFrontendSlot : &primary);
      frontendSignalConnected = false;
    }
    obs_output_release(trackedFrontendOutput);
    trackedFrontendOutput = nullptr;
    trackedFrontendSlot = nullptr;
  }

  Slot primary;
  Slot backup;
  OutputRole frontendRole = OutputRole::Primary;
  bool hasFrontendRole = false;
  bool pendingBackup = false;
  uint32_t delaySeconds = 0;
  obs_output_t *trackedFrontendOutput = nullptr;
  Slot *trackedFrontendSlot = nullptr;
  bool frontendSignalConnected = false;
};

StreamingManager::StreamingManager() : impl_(std::make_unique<Impl>()) {}
StreamingManager::~StreamingManager() = default;
bool StreamingManager::configure(const api::StreamConfiguration &c, StreamingEndpoint m,
                                 std::string &f) { return impl_->configure(c, m, f); }
bool StreamingManager::configureOutput(OutputRole r, const StreamOutputConfig &c,
                                       std::string &f) { return impl_->configureOutput(r, c, f); }
StreamOutputConfig StreamingManager::outputConfiguration(OutputRole r) const
{ return impl_->outputConfiguration(r); }
void StreamingManager::clearConfiguration() { impl_->clearConfiguration(); }
void StreamingManager::setProgramDelay(int v) { impl_->setProgramDelay(v); }
void StreamingManager::onStreamingStarting() { impl_->onStreamingStarting(); }
void StreamingManager::onStreamingStarted() { impl_->onStreamingStarted(); }
void StreamingManager::onStreamingStopping() { impl_->onStreamingStopping(); }
void StreamingManager::onStreamingStopped() { impl_->onStreamingStopped(); }
void StreamingManager::shutdown() { impl_->shutdown(); }
bool StreamingManager::startPrimary(std::string &f) { return impl_->startPrimary(f); }
bool StreamingManager::stopPrimary(std::string &f) { return impl_->stopPrimary(f); }
bool StreamingManager::startBackup(std::string &f) { return impl_->startBackup(f); }
bool StreamingManager::stopBackup(std::string &f) { return impl_->stopBackup(f); }
bool StreamingManager::anyOutputActive() const { return impl_->anyOutputActive(); }
bool StreamingManager::anyOutputRequested() const { return impl_->anyOutputRequested(); }
StreamingHealth StreamingManager::health() { return impl_->health(); }
CaptionDeliveryResult StreamingManager::sendCaption(const std::string &t, double d,
  CaptionPlacement p, CaptionAlignment a) { return impl_->sendCaption(t, d, p, a); }

}  // namespace kaltura_live
