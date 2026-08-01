#pragma once

#include "kaltura_live/captions/caption_dictionary.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kaltura_live::captions {

struct CaptionProviderConfig {
  std::string languageCode = "en-US";
  int sampleRate = 16'000;
  std::string modelPath;
  std::string modelName;
  std::string initialPrompt;
  std::vector<CaptionDictionaryEntry> dictionary;
};

struct CaptionResult {
  std::string text;
  float confidence = 0.0F;
  bool final = false;
};

class CaptionProvider {
public:
  using ResultCallback = std::function<void(CaptionResult)>;
  using StatusCallback = std::function<void(std::string, bool)>;

  virtual ~CaptionProvider() = default;

  [[nodiscard]] virtual const char *id() const = 0;
  virtual bool start(const CaptionProviderConfig &config,
                     ResultCallback resultCallback,
                     StatusCallback statusCallback,
                     std::string &failure) = 0;
  virtual void submitAudio(const int16_t *samples, size_t sampleCount) noexcept = 0;
  virtual void stop() = 0;
  [[nodiscard]] virtual bool running() const noexcept = 0;
};

}  // namespace kaltura_live::captions
