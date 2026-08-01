#include "kaltura_live/captions/caption_manager.hpp"

#include <obs-module.h>

#include <utility>

namespace kaltura_live::captions {

CaptionManager::CaptionManager(std::unique_ptr<CaptionProvider> provider,
                               CaptionCallback captionCallback,
                               StatusCallback statusCallback)
  : provider_(std::move(provider)),
    captionCallback_(std::move(captionCallback)),
    statusCallback_(std::move(statusCallback))
{
}

CaptionManager::~CaptionManager() { stop(); }

bool CaptionManager::start(const CaptionProviderConfig &config, std::string &failure)
{
  if (!provider_) {
    failure = "No caption provider is available.";
    return false;
  }
  if (provider_->running()) {
    return true;
  }

  if (!provider_->start(
        config,
        [this](CaptionResult result) {
          if (result.final && !result.text.empty() && captionCallback_) {
            captionCallback_(std::move(result.text));
          }
        },
        statusCallback_, failure)) {
    return false;
  }

  const audio_convert_info conversion{
    16'000,
    AUDIO_FORMAT_16BIT,
    SPEAKERS_MONO,
    false,
  };
  obs_add_raw_audio_callback(0, &conversion, &CaptionManager::onProgramAudio, this);
  audioCaptureRegistered_ = true;
  return true;
}

void CaptionManager::stop()
{
  if (audioCaptureRegistered_) {
    obs_remove_raw_audio_callback(0, &CaptionManager::onProgramAudio, this);
    audioCaptureRegistered_ = false;
  }
  if (provider_) {
    provider_->stop();
  }
}

bool CaptionManager::running() const
{
  return provider_ && provider_->running();
}

void CaptionManager::onProgramAudio(void *privateData, size_t, ::audio_data *audio)
{
  auto *manager = static_cast<CaptionManager *>(privateData);
  if (!manager || !manager->provider_ || !audio || !audio->data[0] || audio->frames == 0) {
    return;
  }
  manager->provider_->submitAudio(reinterpret_cast<const int16_t *>(audio->data[0]),
                                  audio->frames);
}

}  // namespace kaltura_live::captions
