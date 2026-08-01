#pragma once

#include "kaltura_live/captions/caption_provider.hpp"

#include <memory>

namespace kaltura_live::captions {

class WhisperProvider final : public CaptionProvider {
public:
  WhisperProvider();
  ~WhisperProvider() override;

  WhisperProvider(const WhisperProvider &) = delete;
  WhisperProvider &operator=(const WhisperProvider &) = delete;

  [[nodiscard]] const char *id() const override;
  bool start(const CaptionProviderConfig &config,
             ResultCallback resultCallback,
             StatusCallback statusCallback,
             std::string &failure) override;
  void submitAudio(const int16_t *samples, size_t sampleCount) noexcept override;
  void stop() override;
  [[nodiscard]] bool running() const noexcept override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kaltura_live::captions
