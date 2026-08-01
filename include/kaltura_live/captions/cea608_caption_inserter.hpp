#pragma once

#include "kaltura_live/settings_manager.hpp"
#include "kaltura_live/streaming_manager.hpp"

#include <QObject>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kaltura_live::captions {

enum class CaptionHealthState {
  Disabled,
  Ready,
  Starting,
  Healthy,
  Degraded,
  Error,
};

struct CaptionSegment {
  uint64_t sequence = 0;
  std::string timestamp;
  std::string text;
  int outputsQueued = 0;
  bool primaryQueued = false;
  bool backupQueued = false;
  uint32_t packetCount = 0;
  bool nativeTimed = false;
  bool delivered = false;
  std::string error;
};

struct CaptionHealth {
  CaptionHealthState state = CaptionHealthState::Disabled;
  uint64_t received = 0;
  uint64_t inserted = 0;
  uint64_t dropped = 0;
  size_t queued = 0;
  int delayMs = 0;
  int lastOutputCount = 0;
  std::string providerStatus;
  std::string lastError;
  std::vector<CaptionSegment> recentSegments;
};

class Cea608CaptionInserter final : public QObject {
public:
  using DeliveryCallback =
    std::function<CaptionDeliveryResult(const std::string &, double,
                                        CaptionPlacement, CaptionAlignment)>;
  using DiagnosticCallback = std::function<void(bool, std::string)>;

  explicit Cea608CaptionInserter(DeliveryCallback deliveryCallback,
                                QObject *parent = nullptr);
  ~Cea608CaptionInserter() override;

  Cea608CaptionInserter(const Cea608CaptionInserter &) = delete;
  Cea608CaptionInserter &operator=(const Cea608CaptionInserter &) = delete;

  void configure(bool enabled, int delayMs, CaptionStyle style,
                 CaptionPlacement placement, CaptionAlignment alignment);
  void start();
  void stop();
  [[nodiscard]] bool submit(std::string text);
  void setProviderStatus(std::string status, bool error);
  void setDiagnosticCallback(DiagnosticCallback callback);
  [[nodiscard]] CaptionHealth health() const;

  [[nodiscard]] static std::string formatText(const std::string &text,
                                              CaptionStyle style);
  [[nodiscard]] static std::vector<std::string> formatSegments(
    const std::string &text, CaptionStyle style);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kaltura_live::captions
