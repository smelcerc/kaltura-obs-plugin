#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kaltura_live::platform {

struct RuntimePaths {
  std::string modelDirectory;
  std::string qtPluginDirectory;
};

[[nodiscard]] RuntimePaths runtimePaths(const char *moduleBinaryPath,
                                        const char *moduleDataPath);

class CredentialStore {
public:
  virtual ~CredentialStore() = default;
  [[nodiscard]] virtual std::optional<std::string> load(std::string_view key) = 0;
  virtual bool save(std::string_view key, std::string_view secret) = 0;
  virtual bool remove(std::string_view key) = 0;
  [[nodiscard]] virtual bool available() const = 0;
  [[nodiscard]] virtual const char *backendName() const = 0;
};

[[nodiscard]] std::unique_ptr<CredentialStore> createCredentialStore();

}  // namespace kaltura_live::platform
