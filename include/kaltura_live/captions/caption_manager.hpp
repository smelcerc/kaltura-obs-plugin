#pragma once

#include "kaltura_live/captions/caption_provider.hpp"

#include <functional>
#include <memory>
#include <string>

struct audio_data;

namespace kaltura_live::captions {

class CaptionManager {
public:
  using CaptionCallback = std::function<void(std::string)>;
  using StatusCallback = CaptionProvider::StatusCallback;

  CaptionManager(std::unique_ptr<CaptionProvider> provider,
                 CaptionCallback captionCallback,
                 StatusCallback statusCallback);
  ~CaptionManager();

  CaptionManager(const CaptionManager &) = delete;
  CaptionManager &operator=(const CaptionManager &) = delete;

  bool start(const CaptionProviderConfig &config, std::string &failure);
  void stop();
  [[nodiscard]] bool running() const;

private:
  static void onProgramAudio(void *privateData, size_t mixIndex,
                             ::audio_data *audio);

  std::unique_ptr<CaptionProvider> provider_;
  CaptionCallback captionCallback_;
  StatusCallback statusCallback_;
  bool audioCaptureRegistered_ = false;
};

}  // namespace kaltura_live::captions
