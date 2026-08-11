#pragma once

#include "kaltura_live/api/models.hpp"
#include "kaltura_live/settings_manager.hpp"
#include "kaltura_live/stream_output_config.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace kaltura_live {

enum class OutputHealthState {
  Disabled,
  Idle,
  Starting,
  Live,
  Reconnecting,
  Stopping,
  Error,
};

struct OutputHealth {
  OutputHealthState state = OutputHealthState::Disabled;
  bool connected = false;
  uint64_t bitrateKbps = 0;
  uint64_t bytesSent = 0;
  int droppedFrames = 0;
  int totalFrames = 0;
  uint64_t reconnectAttempts = 0;
  int latencyMs = 0;
  float congestion = 0.0F;
  uint64_t elapsedSeconds = 0;
  OutputProtocol protocol = OutputProtocol::RTMPS;
  std::string endpoint;
  std::string lastError;
};

struct StreamingHealth {
  OutputHealth primary;
  OutputHealth backup;
};

struct CaptionDeliveryResult {
  int outputsQueued = 0;
  std::string error;
  bool primaryQueued = false;
  bool backupQueued = false;
  uint32_t packetCount = 0;
  bool nativeTimed = false;

  [[nodiscard]] bool succeeded() const { return outputsQueued > 0 && error.empty(); }
};

class StreamingManager {
public:
  StreamingManager();
  ~StreamingManager();

  StreamingManager(const StreamingManager &) = delete;
  StreamingManager &operator=(const StreamingManager &) = delete;

  bool configure(const api::StreamConfiguration &configuration,
                 StreamingEndpoint mode, std::string &failure);
  bool configureOutput(OutputRole role, const StreamOutputConfig &configuration,
                       std::string &failure);
  [[nodiscard]] StreamOutputConfig outputConfiguration(OutputRole role) const;
  void clearConfiguration();
  void setProgramDelay(int delayMs);

  void onStreamingStarting();
  void onStreamingStarted();
  void onStreamingStopping();
  void onStreamingStopped();
  void shutdown();
  bool startPrimary(std::string &failure);
  bool stopPrimary(std::string &failure);
  bool startBackup(std::string &failure);
  bool stopBackup(std::string &failure);
  [[nodiscard]] bool anyOutputActive() const;
  [[nodiscard]] bool anyOutputRequested() const;

  [[nodiscard]] StreamingHealth health();
  [[nodiscard]] CaptionDeliveryResult sendCaption(const std::string &text,
                                                  double displayDurationSeconds,
                                                  CaptionPlacement placement,
                                                  CaptionAlignment alignment);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kaltura_live
