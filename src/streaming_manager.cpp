#include "kaltura_live/streaming_manager.hpp"

#include "kaltura_live/captions/native_caption_text.hpp"
#include "kaltura_live/logger.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QUrl>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string_view>
#include <utility>

namespace {

QUrl endpointUrl(const kaltura_live::api::StreamConfiguration &configuration,
                 bool backup)
{
  const QUrl secure = backup ? configuration.urls.backupSecure
                             : configuration.urls.primarySecure;
  if (!secure.isEmpty()) {
    return secure;
  }
  return backup ? configuration.urls.backup : configuration.urls.primary;
}

bool validRtmpUrl(const QUrl &url)
{
  return url.isValid() && (url.scheme() == "rtmp" || url.scheme() == "rtmps") &&
         !url.host().isEmpty();
}

}  // namespace

namespace kaltura_live {

class StreamingManager::Impl {
public:
  Impl() = default;
  ~Impl() = default;
  bool configure(const api::StreamConfiguration &configuration,
                 StreamingEndpoint newMode, std::string &failure)
  {
    if (anyOutputActive()) {
      failure = "Stop Primary and Backup before changing the output mode.";
      return false;
    }

    const QUrl primary = endpointUrl(configuration, false);
    const QUrl backup = endpointUrl(configuration, true);
    if ((newMode == StreamingEndpoint::Primary || newMode == StreamingEndpoint::Both) &&
        !validRtmpUrl(primary)) {
      failure = "Kaltura did not return a valid primary RTMP endpoint.";
      return false;
    }
    if ((newMode == StreamingEndpoint::Backup || newMode == StreamingEndpoint::Both) &&
        !validRtmpUrl(backup)) {
      failure = "Kaltura did not return a valid backup RTMP endpoint.";
      return false;
    }
    if (configuration.keys.rtmp.empty()) {
      failure = "Kaltura did not return a stream key.";
      return false;
    }

    releaseAuxiliaryOutput();
    frontendOutputRequested = false;
    auxiliaryOutputRequested = false;
    mode = newMode;
    configured = true;
    auxiliaryServer = backup.toString(QUrl::FullyEncoded).toUtf8().toStdString();
    auxiliaryKey = configuration.keys.rtmp;
    auxiliaryUsername = configuration.keys.username;
    auxiliaryPassword = configuration.keys.password;
    auxiliaryState = OutputHealthState::Idle;
    auxiliaryError.clear();
    resetTelemetry();
    return true;
  }

  void clearConfiguration()
  {
    frontendOutputRequested = false;
    auxiliaryOutputRequested = false;
    stopAuxiliaryOutput(true);
    releaseAuxiliaryOutput();
    configured = false;
    mode = StreamingEndpoint::Primary;
    auxiliaryServer.clear();
    auxiliaryKey.clear();
    auxiliaryUsername.clear();
    auxiliaryPassword.clear();
    auxiliaryError.clear();
    auxiliaryState = OutputHealthState::Disabled;
    endPrimaryMonitoring();
    resetTelemetry();
  }

  void setProgramDelay(int delayMs)
  {
    programDelaySeconds = static_cast<uint32_t>(
      std::clamp((delayMs + 999) / 1000, 0, 10));

    config_t *profile = obs_frontend_get_profile_config();
    if (profile) {
      const bool enabled = programDelaySeconds > 0;
      const bool changed = config_get_bool(profile, "Output", "DelayEnable") != enabled ||
        config_get_uint(profile, "Output", "DelaySec") != programDelaySeconds ||
        (enabled && !config_get_bool(profile, "Output", "DelayPreserve"));
      if (changed) {
        config_set_bool(profile, "Output", "DelayEnable", enabled);
        config_set_uint(profile, "Output", "DelaySec", programDelaySeconds);
        config_set_bool(profile, "Output", "DelayPreserve", true);
        config_save_safe(profile, "tmp", nullptr);
      }
    }

    if (!anyOutputActive()) {
      obs_output_t *frontendOutput = obs_frontend_get_streaming_output();
      if (frontendOutput) {
        obs_output_set_delay(frontendOutput, programDelaySeconds,
                             OBS_OUTPUT_DELAY_PRESERVE);
        obs_output_release(frontendOutput);
      }
      if (auxiliaryOutput) {
        obs_output_set_delay(auxiliaryOutput, programDelaySeconds,
                             OBS_OUTPUT_DELAY_PRESERVE);
      }
    }
    Logger::write(LogLevel::Info,
                  "Program output delay configured: " +
                    std::to_string(programDelaySeconds) + " second(s)");
  }

  void onStreamingStarting()
  {
    frontendOutputRequested = true;
    resetTelemetry();
    beginPrimaryMonitoring();
  }

  void onStreamingStarted()
  {
    if (!monitoredPrimaryOutput) {
      beginPrimaryMonitoring();
    }
  }

  void onStreamingStopping() {}

  void onStreamingStopped()
  {
    frontendOutputRequested = false;
    endPrimaryMonitoring();
  }

  void shutdown()
  {
    clearConfiguration();
  }

  bool startPrimary(std::string &failure)
  {
    if (!configured || mode == StreamingEndpoint::Backup) {
      failure = "Primary is not enabled by the selected endpoint mode.";
      return false;
    }
    if (obs_frontend_streaming_active()) {
      frontendOutputRequested = true;
      return true;
    }
    frontendOutputRequested = true;
    obs_frontend_streaming_start();
    Logger::write(LogLevel::Info, "Primary RTMP output start requested");
    return true;
  }

  bool stopPrimary(std::string &failure)
  {
    if (!configured || mode == StreamingEndpoint::Backup) {
      failure = "Primary is not enabled by the selected endpoint mode.";
      return false;
    }
    frontendOutputRequested = false;
    if (!obs_frontend_streaming_active()) {
      return true;
    }
    obs_frontend_streaming_stop();
    Logger::write(LogLevel::Info, "Primary RTMP output stop requested");
    return true;
  }

  bool startBackup(std::string &failure)
  {
    if (!configured || mode == StreamingEndpoint::Primary) {
      failure = "Backup is not enabled by the selected endpoint mode.";
      return false;
    }
    if (mode == StreamingEndpoint::Backup) {
      frontendOutputRequested = true;
      if (!obs_frontend_streaming_active()) {
        obs_frontend_streaming_start();
        Logger::write(LogLevel::Info, "Backup RTMP output start requested");
      }
      return true;
    }
    if (!startAuxiliaryOutput()) {
      auxiliaryOutputRequested = false;
      failure = auxiliaryError.empty() ? "The Backup output could not start."
                                       : auxiliaryError;
      return false;
    }
    return true;
  }

  bool stopBackup(std::string &failure)
  {
    if (!configured || mode == StreamingEndpoint::Primary) {
      failure = "Backup is not enabled by the selected endpoint mode.";
      return false;
    }
    if (mode == StreamingEndpoint::Backup) {
      frontendOutputRequested = false;
      if (obs_frontend_streaming_active()) {
        obs_frontend_streaming_stop();
        Logger::write(LogLevel::Info, "Backup RTMP output stop requested");
      }
      return true;
    }
    auxiliaryOutputRequested = false;
    if (!auxiliaryOutput || !obs_output_active(auxiliaryOutput)) {
      return true;
    }
    stopAuxiliaryOutput(false);
    return true;
  }

  [[nodiscard]] bool anyOutputActive() const
  {
    return obs_frontend_streaming_active() ||
           (auxiliaryOutput && obs_output_active(auxiliaryOutput));
  }

  [[nodiscard]] bool anyOutputRequested() const
  {
    return frontendOutputRequested || auxiliaryOutputRequested;
  }

  StreamingHealth health()
  {
    StreamingHealth result;
    if (!configured) {
      return result;
    }

    if (auxiliaryOutput && auxiliaryState == OutputHealthState::Stopping &&
        !obs_output_active(auxiliaryOutput)) {
      releaseAuxiliaryOutput();
      auxiliaryState = OutputHealthState::Idle;
    }

    obs_output_t *frontendOutput = obs_frontend_get_streaming_output();
    if (mode == StreamingEndpoint::Primary) {
      result.primary = readOutputHealth(frontendOutput, OutputHealthState::Idle, {},
                                        primaryTelemetry, primaryReconnectAttempts.load());
    } else if (mode == StreamingEndpoint::Backup) {
      result.backup = readOutputHealth(frontendOutput, OutputHealthState::Idle, {},
                                       primaryTelemetry, primaryReconnectAttempts.load());
    } else {
      result.primary = readOutputHealth(frontendOutput, OutputHealthState::Idle, {},
                                        primaryTelemetry, primaryReconnectAttempts.load());
      result.backup = readOutputHealth(auxiliaryOutput, auxiliaryState, auxiliaryError,
                                       auxiliaryTelemetry, auxiliaryReconnectAttempts.load());
    }
    if (frontendOutput) {
      obs_output_release(frontendOutput);
    }
    return result;
  }

  CaptionDeliveryResult sendCaption(const std::string &text, double displayDurationSeconds,
                                    CaptionPlacement placement, CaptionAlignment alignment)
  {
    if (text.empty() || text.size() > 2'048) {
      CaptionDeliveryResult result;
      result.error = "Caption text was empty or exceeded the safe size limit.";
      return result;
    }
    CaptionDeliveryResult result;
    result.nativeTimed = true;
    const double duration = std::clamp(displayDurationSeconds, 0.5, 10.0);
    const captions::NativeCaptionText prepared =
      captions::prepareNativeCaptionText(text, placement);
    (void)alignment;

    const auto queueForOutput = [&result, &prepared, duration](obs_output_t *output,
                                                               bool primary) {
      if (!output || !obs_output_active(output)) {
        return;
      }
      obs_encoder_t *encoder = obs_output_get_video_encoder(output);
      const char *codec = encoder ? obs_encoder_get_codec(encoder) : nullptr;
      if (!codec || (std::string_view(codec) != "h264" &&
                     std::string_view(codec) != "hevc" &&
                     std::string_view(codec) != "av1")) {
        return;
      }
      // OBS/libcaption emits a complete pop-on CEA-608 screen update on one
      // encoded video frame. Repeat the same screen on nearby frames so a
      // transient RTMP loss or an ingest segment boundary cannot remove the
      // entire cue. Repainting identical text is visually idempotent.
      constexpr int kCaptionScreenCopies = 3;
      constexpr double kRepeatIntervalSeconds = 0.10;
      for (int copy = 1; copy < kCaptionScreenCopies; ++copy) {
        obs_output_output_caption_text2(
          output, prepared.text.c_str(), kRepeatIntervalSeconds);
      }
      obs_output_output_caption_text2(output, prepared.text.c_str(), duration);
      ++result.outputsQueued;
      result.packetCount += kCaptionScreenCopies;
      if (primary) {
        result.primaryQueued = true;
      } else {
        result.backupQueued = true;
      }
    };

    obs_output_t *frontendOutput = obs_frontend_get_streaming_output();
    queueForOutput(frontendOutput, mode != StreamingEndpoint::Backup);
    if (frontendOutput) {
      obs_output_release(frontendOutput);
    }
    queueForOutput(auxiliaryOutput, false);
    if (result.outputsQueued == 0) {
      result.error =
        "No active OBS output with a supported video encoder was available for captions.";
    } else {
      Logger::write(
        LogLevel::Info,
        "CEA-608 caption screen queued in three consecutive encoder updates for " +
          std::to_string(result.outputsQueued) + " active output(s)");
    }
    return result;
  }

private:
  struct TelemetrySample {
    obs_output_t *outputIdentity = nullptr;
    uint64_t bytes = 0;
    std::chrono::steady_clock::time_point sampledAt{};
    uint64_t bitrateKbps = 0;
  };

  static OutputHealth readOutputHealth(obs_output_t *output,
                                       OutputHealthState fallbackState,
                                       const std::string &fallbackError,
                                       TelemetrySample &sample,
                                       uint64_t reconnectAttempts)
  {
    OutputHealth health;
    health.state = fallbackState;
    health.lastError = fallbackError;
    if (!output) {
      return health;
    }

    health.bytesSent = obs_output_get_total_bytes(output);
    const auto now = std::chrono::steady_clock::now();
    if (sample.outputIdentity == output && sample.sampledAt.time_since_epoch().count() != 0 &&
        health.bytesSent >= sample.bytes) {
      const double elapsedSeconds =
        std::chrono::duration<double>(now - sample.sampledAt).count();
      if (elapsedSeconds > 0.0) {
        sample.bitrateKbps = static_cast<uint64_t>(
          ((health.bytesSent - sample.bytes) * 8.0) / elapsedSeconds / 1000.0);
      }
    } else {
      sample.bitrateKbps = 0;
    }
    sample.outputIdentity = output;
    sample.bytes = health.bytesSent;
    sample.sampledAt = now;
    health.bitrateKbps = sample.bitrateKbps;
    health.droppedFrames = obs_output_get_frames_dropped(output);
    health.totalFrames = obs_output_get_total_frames(output);
    health.reconnectAttempts = reconnectAttempts;
    health.latencyMs = std::max(0, obs_output_get_connect_time_ms(output));
    health.congestion = std::clamp(obs_output_get_congestion(output), 0.0F, 1.0F);
    const bool active = obs_output_active(output);
    if (obs_output_reconnecting(output)) {
      health.state = OutputHealthState::Reconnecting;
    } else if (active) {
      health.state = OutputHealthState::Live;
    }
    health.connected = active && !obs_output_reconnecting(output);
    const char *lastError = obs_output_get_last_error(output);
    if (lastError && *lastError) {
      health.lastError = lastError;
      if (!active) {
        health.state = OutputHealthState::Error;
      }
    }
    return health;
  }

  static void onPrimaryReconnect(void *data, calldata_t *)
  {
    auto *self = static_cast<Impl *>(data);
    if (self) {
      ++self->primaryReconnectAttempts;
    }
  }

  static void onAuxiliaryReconnect(void *data, calldata_t *)
  {
    auto *self = static_cast<Impl *>(data);
    if (self) {
      ++self->auxiliaryReconnectAttempts;
    }
  }

  void beginPrimaryMonitoring()
  {
    endPrimaryMonitoring();
    monitoredPrimaryOutput = obs_frontend_get_streaming_output();
    if (!monitoredPrimaryOutput) {
      return;
    }
    signal_handler_connect(obs_output_get_signal_handler(monitoredPrimaryOutput), "reconnect",
                           &Impl::onPrimaryReconnect, this);
  }

  void endPrimaryMonitoring()
  {
    if (!monitoredPrimaryOutput) {
      return;
    }
    signal_handler_disconnect(obs_output_get_signal_handler(monitoredPrimaryOutput), "reconnect",
                              &Impl::onPrimaryReconnect, this);
    obs_output_release(monitoredPrimaryOutput);
    monitoredPrimaryOutput = nullptr;
  }

  void resetTelemetry()
  {
    primaryTelemetry = {};
    auxiliaryTelemetry = {};
    primaryReconnectAttempts = 0;
    auxiliaryReconnectAttempts = 0;
  }

  bool startAuxiliaryOutput()
  {
    if (auxiliaryOutput && obs_output_active(auxiliaryOutput)) {
      auxiliaryOutputRequested = true;
      return true;
    }
    releaseAuxiliaryOutput();

    obs_output_t *primaryOutput = obs_frontend_get_streaming_output();
    if (!primaryOutput) {
      setAuxiliaryError("OBS primary streaming output is unavailable.");
      return false;
    }

    obs_encoder_t *videoEncoder = obs_output_get_video_encoder(primaryOutput);
    obs_encoder_t *audioEncoder = obs_output_get_audio_encoder(primaryOutput, 0);
    if (!videoEncoder || !audioEncoder) {
      obs_output_release(primaryOutput);
      auxiliaryState = OutputHealthState::Starting;
      auxiliaryError = "Waiting for OBS streaming encoders.";
      return false;
    }

    obs_data_t *outputSettings = obs_output_get_settings(primaryOutput);
    auxiliaryOutput = obs_output_create("rtmp_output", "kaltura_backup_output",
                                        outputSettings, nullptr);
    if (outputSettings) {
      obs_data_release(outputSettings);
    }
    if (!auxiliaryOutput) {
      obs_output_release(primaryOutput);
      setAuxiliaryError("OBS could not create the backup RTMP output.");
      return false;
    }
    signal_handler_connect(obs_output_get_signal_handler(auxiliaryOutput), "reconnect",
                           &Impl::onAuxiliaryReconnect, this);
    auxiliarySignalConnected = true;

    obs_data_t *serviceSettings = obs_data_create();
    obs_data_set_string(serviceSettings, "server", auxiliaryServer.c_str());
    obs_data_set_string(serviceSettings, "key", auxiliaryKey.c_str());
    const bool useAuthentication = !auxiliaryUsername.empty() && !auxiliaryPassword.empty();
    obs_data_set_bool(serviceSettings, "use_auth", useAuthentication);
    obs_data_set_bool(serviceSettings, "bwtest", false);
    if (useAuthentication) {
      obs_data_set_string(serviceSettings, "username", auxiliaryUsername.c_str());
      obs_data_set_string(serviceSettings, "password", auxiliaryPassword.c_str());
    }
    auxiliaryService = obs_service_create("rtmp_custom", "kaltura_backup_service",
                                          serviceSettings, nullptr);
    obs_data_release(serviceSettings);
    if (!auxiliaryService) {
      obs_output_release(primaryOutput);
      setAuxiliaryError("OBS could not create the backup RTMP service.");
      releaseAuxiliaryOutput();
      return false;
    }

    obs_output_set_service(auxiliaryOutput, auxiliaryService);
    obs_output_set_video_encoder(auxiliaryOutput, videoEncoder);
    obs_output_set_audio_encoder(auxiliaryOutput, audioEncoder, 0);
    obs_output_set_delay(auxiliaryOutput, programDelaySeconds,
                         OBS_OUTPUT_DELAY_PRESERVE);
    obs_output_release(primaryOutput);

    auxiliaryState = OutputHealthState::Starting;
    auxiliaryError.clear();
    if (!obs_output_start(auxiliaryOutput)) {
      const char *error = obs_output_get_last_error(auxiliaryOutput);
      setAuxiliaryError(error && *error ? error : "The backup RTMP output could not start.");
      return false;
    }

    auxiliaryOutputRequested = true;
    Logger::write(LogLevel::Info, "Backup RTMP output started");
    return true;
  }

  void stopAuxiliaryOutput(bool force)
  {
    auxiliaryOutputRequested = false;
    if (!auxiliaryOutput || !obs_output_active(auxiliaryOutput)) {
      return;
    }
    auxiliaryState = OutputHealthState::Stopping;
    if (force) {
      obs_output_force_stop(auxiliaryOutput);
    } else {
      obs_output_stop(auxiliaryOutput);
    }
    Logger::write(LogLevel::Info, "Backup RTMP output stopped");
  }

  void releaseAuxiliaryOutput()
  {
    if (auxiliaryOutput) {
      if (auxiliarySignalConnected) {
        signal_handler_disconnect(obs_output_get_signal_handler(auxiliaryOutput), "reconnect",
                                  &Impl::onAuxiliaryReconnect, this);
        auxiliarySignalConnected = false;
      }
      obs_output_release(auxiliaryOutput);
      auxiliaryOutput = nullptr;
    }
    if (auxiliaryService) {
      obs_service_release(auxiliaryService);
      auxiliaryService = nullptr;
    }
  }

  void setAuxiliaryError(std::string message)
  {
    auxiliaryState = OutputHealthState::Error;
    auxiliaryError = std::move(message);
    Logger::write(LogLevel::Error, "Backup RTMP output failed: " + auxiliaryError);
  }

  StreamingEndpoint mode = StreamingEndpoint::Primary;
  bool configured = false;
  std::string auxiliaryServer;
  std::string auxiliaryKey;
  std::string auxiliaryUsername;
  std::string auxiliaryPassword;
  obs_output_t *auxiliaryOutput = nullptr;
  obs_service_t *auxiliaryService = nullptr;
  obs_output_t *monitoredPrimaryOutput = nullptr;
  bool auxiliarySignalConnected = false;
  TelemetrySample primaryTelemetry;
  TelemetrySample auxiliaryTelemetry;
  std::atomic<uint64_t> primaryReconnectAttempts{0};
  std::atomic<uint64_t> auxiliaryReconnectAttempts{0};
  OutputHealthState auxiliaryState = OutputHealthState::Disabled;
  std::string auxiliaryError;
  uint32_t programDelaySeconds = 0;
  bool frontendOutputRequested = false;
  bool auxiliaryOutputRequested = false;
};

StreamingManager::StreamingManager() : impl_(std::make_unique<Impl>()) {}
StreamingManager::~StreamingManager()
{
  if (impl_) {
    impl_->shutdown();
  }
}

bool StreamingManager::configure(const api::StreamConfiguration &configuration,
                                 StreamingEndpoint mode, std::string &failure)
{
  return impl_->configure(configuration, mode, failure);
}

void StreamingManager::clearConfiguration() { impl_->clearConfiguration(); }
void StreamingManager::setProgramDelay(int delayMs) { impl_->setProgramDelay(delayMs); }
void StreamingManager::onStreamingStarting() { impl_->onStreamingStarting(); }
void StreamingManager::onStreamingStarted() { impl_->onStreamingStarted(); }
void StreamingManager::onStreamingStopping() { impl_->onStreamingStopping(); }
void StreamingManager::onStreamingStopped() { impl_->onStreamingStopped(); }
void StreamingManager::shutdown() { impl_->shutdown(); }
bool StreamingManager::startPrimary(std::string &failure)
{
  return impl_->startPrimary(failure);
}
bool StreamingManager::stopPrimary(std::string &failure)
{
  return impl_->stopPrimary(failure);
}
bool StreamingManager::startBackup(std::string &failure)
{
  return impl_->startBackup(failure);
}
bool StreamingManager::stopBackup(std::string &failure)
{
  return impl_->stopBackup(failure);
}
bool StreamingManager::anyOutputActive() const { return impl_->anyOutputActive(); }
bool StreamingManager::anyOutputRequested() const { return impl_->anyOutputRequested(); }
StreamingHealth StreamingManager::health() { return impl_->health(); }
CaptionDeliveryResult StreamingManager::sendCaption(const std::string &text,
                                                     double displayDurationSeconds,
                                                     CaptionPlacement placement,
                                                     CaptionAlignment alignment)
{
  return impl_->sendCaption(text, displayDurationSeconds, placement, alignment);
}

}  // namespace kaltura_live
