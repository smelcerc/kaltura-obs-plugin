#include "kaltura_live/captions/whisper_provider.hpp"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
  using kaltura_live::captions::CaptionDictionaryEntry;
  const std::vector<CaptionDictionaryEntry> dictionary{
    {"cal torah", "Kaltura"}, {"kay ess", "KS"}, {"", "RTMP"}};
  const std::string corrected = kaltura_live::captions::applyCaptionDictionary(
    "A cal torah kay ess is active, but concatenate is unchanged.", dictionary);
  if (corrected != "A Kaltura KS is active, but concatenate is unchanged.") {
    std::cerr << "caption dictionary replacements were not applied safely\n";
    return 1;
  }

  kaltura_live::captions::WhisperProvider provider;
  if (std::string(provider.id()) != "local-whisper") {
    std::cerr << "unexpected provider identifier\n";
    return 1;
  }

  kaltura_live::captions::CaptionProviderConfig invalidConfig;
  invalidConfig.sampleRate = 44'100;
  std::string failure;
  if (provider.start(invalidConfig, {}, {}, failure) || failure.empty() || provider.running()) {
    std::cerr << "invalid local Whisper configuration was accepted\n";
    return 1;
  }

  provider.stop();

  if (argc == 3) {
    std::ifstream audio(argv[2], std::ios::binary);
    if (!audio) {
      std::cerr << "could not open Whisper smoke-test audio\n";
      return 1;
    }
    audio.seekg(0, std::ios::end);
    const std::streamoff byteCount = audio.tellg() - std::streamoff(44);
    if (byteCount <= 0 || byteCount % sizeof(int16_t) != 0) {
      std::cerr << "invalid Whisper smoke-test WAV data\n";
      return 1;
    }
    std::vector<int16_t> samples(static_cast<size_t>(byteCount) / sizeof(int16_t));
    audio.seekg(44);
    audio.read(reinterpret_cast<char *>(samples.data()),
               static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));

    kaltura_live::captions::CaptionProviderConfig config;
    config.modelPath = argv[1];
    config.modelName = "Smoke Test";
    std::mutex mutex;
    std::condition_variable resultReady;
    std::string transcript;
    failure.clear();
    if (!provider.start(
          config,
          [&](kaltura_live::captions::CaptionResult result) {
            std::lock_guard lock(mutex);
            transcript += result.text;
            resultReady.notify_all();
          },
          [](std::string status, bool error) {
            std::cout << (error ? "error: " : "status: ") << status << '\n';
          },
          failure)) {
      std::cerr << failure << '\n';
      return 1;
    }
    for (size_t offset = 0; offset < samples.size(); offset += 1'600) {
      const size_t count = std::min<size_t>(1'600, samples.size() - offset);
      provider.submitAudio(samples.data() + offset, count);
    }
    const std::vector<int16_t> silence(16'000, 0);
    provider.submitAudio(silence.data(), silence.size());
    {
      std::unique_lock lock(mutex);
      resultReady.wait_for(lock, std::chrono::seconds(120), [&]() {
        return !transcript.empty();
      });
    }
    provider.stop();
    if (transcript.empty()) {
      std::cerr << "Local Whisper produced no smoke-test transcript\n";
      return 1;
    }
    std::cout << "transcript: " << transcript << '\n';
  }
  return 0;
}
