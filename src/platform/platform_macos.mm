#include "kaltura_live/platform/platform.hpp"

#include <QDir>
#include <QFileInfo>

#include <Security/Security.h>

#include <string>

namespace {
CFMutableDictionaryRef credentialQuery(std::string_view key)
{
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(
    kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
    &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, CFSTR("com.kaltura.obs.kaltura-live"));
  CFStringRef account = CFStringCreateWithBytes(
    kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(key.data()),
    static_cast<CFIndex>(key.size()), kCFStringEncodingUTF8, false);
  CFDictionarySetValue(query, kSecAttrAccount, account);
  CFRelease(account);
  return query;
}

class KeychainCredentialStore final : public kaltura_live::platform::CredentialStore {
public:
  std::optional<std::string> load(std::string_view key) override
  {
    CFMutableDictionaryRef query = credentialQuery(key);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status == errSecItemNotFound) {
      return std::nullopt;
    }
    if (status != errSecSuccess || !result || CFGetTypeID(result) != CFDataGetTypeID()) {
      if (result) {
        CFRelease(result);
      }
      return std::nullopt;
    }
    const auto data = static_cast<CFDataRef>(result);
    std::string value(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                      static_cast<size_t>(CFDataGetLength(data)));
    CFRelease(result);
    return value;
  }

  bool save(std::string_view key, std::string_view secret) override
  {
    CFDataRef value = CFDataCreate(
      kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(secret.data()),
      static_cast<CFIndex>(secret.size()));
    CFMutableDictionaryRef query = credentialQuery(key);
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kSecValueData, value);
    OSStatus status = SecItemUpdate(query, attributes);
    if (status == errSecItemNotFound) {
      CFDictionarySetValue(query, kSecValueData, value);
      status = SecItemAdd(query, nullptr);
    }
    CFRelease(attributes);
    CFRelease(query);
    CFRelease(value);
    return status == errSecSuccess;
  }

  bool remove(std::string_view key) override
  {
    CFMutableDictionaryRef query = credentialQuery(key);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    return status == errSecSuccess || status == errSecItemNotFound;
  }

  bool available() const override { return true; }
  const char *backendName() const override { return "macOS Keychain"; }
};
}

namespace kaltura_live::platform {

RuntimePaths runtimePaths(const char *moduleBinaryPath, const char *moduleDataPath)
{
  RuntimePaths result;
  if (moduleBinaryPath && *moduleBinaryPath) {
    QDir contents = QFileInfo(QString::fromUtf8(moduleBinaryPath)).absoluteDir();
    if (contents.cdUp()) {
      result.qtPluginDirectory = contents.filePath("PlugIns").toUtf8().toStdString();
      result.modelDirectory = contents.filePath("Resources/models").toUtf8().toStdString();
    }
  }
  if (result.modelDirectory.empty() && moduleDataPath && *moduleDataPath) {
    result.modelDirectory =
      QDir(QString::fromUtf8(moduleDataPath)).filePath("models").toUtf8().toStdString();
  }
  return result;
}

std::unique_ptr<CredentialStore> createCredentialStore()
{
  return std::make_unique<KeychainCredentialStore>();
}

}  // namespace kaltura_live::platform
