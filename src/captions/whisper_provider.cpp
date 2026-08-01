#include "kaltura_live/captions/whisper_provider.hpp"

#include <whisper.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kaltura_live::captions {
namespace {
constexpr size_t kAudioSlots = 256;
constexpr size_t kSamplesPerSlot = 2'048;
constexpr float kSpeechRmsThreshold = 0.012F;
constexpr int kPreRollMs = 300;
constexpr int kTrailingSilenceMs = 650;
constexpr int kMinimumUtteranceMs = 700;
constexpr int kMaximumUtteranceMs = 10'000;
constexpr size_t kMaximumPhraseQueue = 4;

char asciiLower(char value)
{
  return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool isWordCharacter(char value)
{
  const unsigned char character = static_cast<unsigned char>(value);
  return std::isalnum(character) || value == '_';
}

void replaceDictionaryPhrase(std::string &text, const CaptionDictionaryEntry &entry)
{
  if (entry.spokenForm.empty()) {
    return;
  }
  std::string loweredText(text.size(), '\0');
  std::transform(text.begin(), text.end(), loweredText.begin(), asciiLower);
  std::string loweredNeedle(entry.spokenForm.size(), '\0');
  std::transform(entry.spokenForm.begin(), entry.spokenForm.end(),
                 loweredNeedle.begin(), asciiLower);
  size_t offset = 0;
  while ((offset = loweredText.find(loweredNeedle, offset)) != std::string::npos) {
    const size_t end = offset + loweredNeedle.size();
    const bool leftBoundary = offset == 0 || !isWordCharacter(loweredText[offset - 1]);
    const bool rightBoundary = end == loweredText.size() ||
                               !isWordCharacter(loweredText[end]);
    if (!leftBoundary || !rightBoundary) {
      offset = end;
      continue;
    }
    text.replace(offset, entry.spokenForm.size(), entry.preferredText);
    loweredText.replace(offset, loweredNeedle.size(), entry.preferredText);
    std::transform(loweredText.begin() + static_cast<std::ptrdiff_t>(offset),
                   loweredText.begin() + static_cast<std::ptrdiff_t>(offset + entry.preferredText.size()),
                   loweredText.begin() + static_cast<std::ptrdiff_t>(offset), asciiLower);
    offset += entry.preferredText.size();
  }
}

std::string trimTranscript(std::string value)
{
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character);
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
    return std::isspace(character);
  }).base();
  return first < last ? std::string(first, last) : std::string{};
}

}  // namespace

std::string applyCaptionDictionary(
  std::string text, const std::vector<CaptionDictionaryEntry> &dictionary)
{
  std::vector<CaptionDictionaryEntry> ordered = dictionary;
  std::stable_sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
    return left.spokenForm.size() > right.spokenForm.size();
  });
  for (const CaptionDictionaryEntry &entry : ordered) {
    replaceDictionaryPhrase(text, entry);
  }
  return text;
}

namespace {

bool isUsefulTranscript(const std::string &value)
{
  return !value.empty() && value != "[BLANK_AUDIO]" && value != "(silence)" &&
         value != "[Silence]";
}
}  // namespace

class WhisperProvider::Impl {
public:
  ~Impl() { stop(); }

  bool start(const CaptionProviderConfig &newConfig,
             ResultCallback newResultCallback,
             StatusCallback newStatusCallback,
             std::string &failure)
  {
    if (running.load()) {
      failure = "Local Whisper captions are already running.";
      return false;
    }
    if (newConfig.sampleRate != 16'000) {
      failure = "Local Whisper requires 16 kHz program audio.";
      return false;
    }
    if (newConfig.modelPath.empty() ||
        !std::filesystem::is_regular_file(newConfig.modelPath)) {
      failure = "The Local Whisper model is missing. Redeploy the plugin to install models.";
      return false;
    }

    config = newConfig;
    resultCallback = std::move(newResultCallback);
    statusCallback = std::move(newStatusCallback);
    readSequence.store(0);
    writeSequence.store(0);
    droppedAudioChunks.store(0);
    phraseQueue.clear();
    running.store(true);
    analysisWorker = std::thread([this]() { analyzeAudio(); });
    inferenceWorker = std::thread([this]() { runInference(); });
    return true;
  }

  void submitAudio(const int16_t *samples, size_t sampleCount) noexcept
  {
    if (!running.load(std::memory_order_relaxed) || !samples || sampleCount == 0) {
      return;
    }
    while (sampleCount > 0) {
      const uint64_t write = writeSequence.load(std::memory_order_relaxed);
      const uint64_t read = readSequence.load(std::memory_order_acquire);
      if (write - read >= kAudioSlots) {
        ++droppedAudioChunks;
        return;
      }
      AudioSlot &slot = audioSlots[write % kAudioSlots];
      slot.sampleCount = std::min(sampleCount, kSamplesPerSlot);
      std::copy_n(samples, slot.sampleCount, slot.samples.begin());
      writeSequence.store(write + 1, std::memory_order_release);
      samples += slot.sampleCount;
      sampleCount -= slot.sampleCount;
    }
  }

  void stop()
  {
    const bool hadSession = running.exchange(false) || analysisWorker.joinable() ||
                            inferenceWorker.joinable();
    phraseReady.notify_all();
    if (analysisWorker.joinable()) {
      analysisWorker.join();
    }
    if (inferenceWorker.joinable()) {
      inferenceWorker.join();
    }
    {
      std::lock_guard lock(phraseMutex);
      phraseQueue.clear();
    }
    if (hadSession) {
      emitStatus("Local Whisper ready", false);
    }
  }

  [[nodiscard]] bool isRunning() const noexcept { return running.load(); }

private:
  struct AudioSlot {
    std::array<int16_t, kSamplesPerSlot> samples{};
    size_t sampleCount = 0;
  };

  bool popAudio(std::vector<float> &samples)
  {
    const uint64_t read = readSequence.load(std::memory_order_relaxed);
    if (read == writeSequence.load(std::memory_order_acquire)) {
      return false;
    }
    const AudioSlot &slot = audioSlots[read % kAudioSlots];
    samples.resize(slot.sampleCount);
    std::transform(slot.samples.begin(), slot.samples.begin() + slot.sampleCount,
                   samples.begin(), [](int16_t sample) {
                     return static_cast<float>(sample) / 32'768.0F;
                   });
    readSequence.store(read + 1, std::memory_order_release);
    return true;
  }

  void queuePhrase(std::vector<float> phrase)
  {
    const size_t minimumSamples =
      static_cast<size_t>(config.sampleRate * kMinimumUtteranceMs / 1'000);
    if (phrase.size() < minimumSamples) {
      return;
    }
    {
      std::lock_guard lock(phraseMutex);
      if (phraseQueue.size() >= kMaximumPhraseQueue) {
        phraseQueue.pop_front();
        emitStatus("Local Whisper is behind; an old caption phrase was dropped.", true);
      }
      phraseQueue.push_back(std::move(phrase));
    }
    phraseReady.notify_one();
  }

  void analyzeAudio()
  {
    const size_t preRollSamples =
      static_cast<size_t>(config.sampleRate * kPreRollMs / 1'000);
    const size_t trailingSilenceSamples =
      static_cast<size_t>(config.sampleRate * kTrailingSilenceMs / 1'000);
    const size_t maximumSamples =
      static_cast<size_t>(config.sampleRate * kMaximumUtteranceMs / 1'000);
    std::deque<float> preRoll;
    std::vector<float> utterance;
    size_t silenceSamples = 0;
    bool speaking = false;

    while (running.load()) {
      std::vector<float> chunk;
      if (!popAudio(chunk)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }
      float energy = 0.0F;
      for (float sample : chunk) {
        energy += sample * sample;
      }
      const float rms = chunk.empty() ? 0.0F : std::sqrt(energy / chunk.size());
      const bool speech = rms >= kSpeechRmsThreshold;

      if (!speaking) {
        for (float sample : chunk) {
          preRoll.push_back(sample);
        }
        while (preRoll.size() > preRollSamples) {
          preRoll.pop_front();
        }
        if (!speech) {
          continue;
        }
        utterance.assign(preRoll.begin(), preRoll.end());
        preRoll.clear();
        speaking = true;
        silenceSamples = 0;
      } else {
        utterance.insert(utterance.end(), chunk.begin(), chunk.end());
      }

      silenceSamples = speech ? 0 : silenceSamples + chunk.size();
      if (silenceSamples >= trailingSilenceSamples || utterance.size() >= maximumSamples) {
        queuePhrase(std::move(utterance));
        utterance.clear();
        speaking = false;
        silenceSamples = 0;
      }
    }
    if (!utterance.empty()) {
      queuePhrase(std::move(utterance));
    }
  }

  void runInference()
  {
    emitStatus("Loading Local Whisper · " + config.modelName + "…", false);
    whisper_context_params contextParams = whisper_context_default_params();
    contextParams.use_gpu = false;
    whisper_context *context =
      whisper_init_from_file_with_params(config.modelPath.c_str(), contextParams);
    if (!context) {
      emitStatus("Local Whisper could not load the " + config.modelName + " model.", true);
      running.store(false);
      phraseReady.notify_all();
      return;
    }
    emitStatus("Local Whisper active · " + config.modelName, false);

    while (true) {
      std::vector<float> phrase;
      {
        std::unique_lock lock(phraseMutex);
        phraseReady.wait(lock, [this]() {
          return !running.load() || !phraseQueue.empty();
        });
        if (phraseQueue.empty()) {
          if (!running.load()) {
            break;
          }
          continue;
        }
        phrase = std::move(phraseQueue.front());
        phraseQueue.pop_front();
      }

      whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
      params.n_threads = std::max(1U, std::min(4U, std::thread::hardware_concurrency()));
      params.print_realtime = false;
      params.print_progress = false;
      params.print_timestamps = false;
      params.print_special = false;
      params.translate = false;
      params.no_context = true;
      params.no_timestamps = true;
      params.single_segment = true;
      params.suppress_blank = true;
      params.suppress_nst = true;
      params.language = "en";
      params.initial_prompt = config.initialPrompt.empty() ? nullptr
                                                           : config.initialPrompt.c_str();

      if (whisper_full(context, params, phrase.data(),
                       static_cast<int>(phrase.size())) != 0) {
        emitStatus("Local Whisper failed to transcribe an audio phrase.", true);
        continue;
      }
      std::string transcript;
      const int segmentCount = whisper_full_n_segments(context);
      for (int index = 0; index < segmentCount; ++index) {
        if (const char *segment = whisper_full_get_segment_text(context, index)) {
          transcript += segment;
        }
      }
      transcript = trimTranscript(std::move(transcript));
      transcript = applyCaptionDictionary(std::move(transcript), config.dictionary);
      if (isUsefulTranscript(transcript) && transcript.size() <= 2'048 && resultCallback) {
        resultCallback({std::move(transcript), 0.0F, true});
      }
    }
    whisper_free(context);
  }

  void emitStatus(std::string message, bool error)
  {
    if (statusCallback) {
      statusCallback(std::move(message), error);
    }
  }

  CaptionProviderConfig config;
  ResultCallback resultCallback;
  StatusCallback statusCallback;
  std::array<AudioSlot, kAudioSlots> audioSlots{};
  std::atomic<uint64_t> readSequence{0};
  std::atomic<uint64_t> writeSequence{0};
  std::atomic<uint64_t> droppedAudioChunks{0};
  std::atomic<bool> running{false};
  std::thread analysisWorker;
  std::thread inferenceWorker;
  std::mutex phraseMutex;
  std::condition_variable phraseReady;
  std::deque<std::vector<float>> phraseQueue;
};

WhisperProvider::WhisperProvider() : impl_(std::make_unique<Impl>()) {}
WhisperProvider::~WhisperProvider() = default;
const char *WhisperProvider::id() const { return "local-whisper"; }

bool WhisperProvider::start(const CaptionProviderConfig &config,
                            ResultCallback resultCallback,
                            StatusCallback statusCallback,
                            std::string &failure)
{
  return impl_->start(config, std::move(resultCallback), std::move(statusCallback), failure);
}

void WhisperProvider::submitAudio(const int16_t *samples, size_t sampleCount) noexcept
{
  impl_->submitAudio(samples, sampleCount);
}

void WhisperProvider::stop() { impl_->stop(); }
bool WhisperProvider::running() const noexcept { return impl_->isRunning(); }

}  // namespace kaltura_live::captions
