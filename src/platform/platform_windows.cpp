#include "kaltura_live/platform/platform.hpp"

#include <QDir>

#include <windows.h>
#include <wincred.h>

#include <string>

namespace {
std::wstring credentialTarget(std::string_view key)
{
  return L"Kaltura Live OBS/" + std::wstring(key.begin(), key.end());
}

class WindowsCredentialStore final : public kaltura_live::platform::CredentialStore {
public:
  std::optional<std::string> load(std::string_view key) override
  {
    const std::wstring target = credentialTarget(key);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
      return std::nullopt;
    }
    std::string value(reinterpret_cast<const char *>(credential->CredentialBlob),
                      credential->CredentialBlobSize);
    CredFree(credential);
    return value;
  }

  bool save(std::string_view key, std::string_view secret) override
  {
    if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
      return false;
    }
    const std::wstring target = credentialTarget(key);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t *>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(secret.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t *>(L"Kaltura Session");
    return CredWriteW(&credential, 0) == TRUE;
  }

  bool remove(std::string_view key) override
  {
    const std::wstring target = credentialTarget(key);
    return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) == TRUE ||
           GetLastError() == ERROR_NOT_FOUND;
  }

  bool available() const override { return true; }
  const char *backendName() const override { return "Windows Credential Manager"; }
};
}

namespace kaltura_live::platform {

RuntimePaths runtimePaths(const char *, const char *moduleDataPath)
{
  RuntimePaths result;
  if (moduleDataPath && *moduleDataPath) {
    result.modelDirectory =
      QDir(QString::fromUtf8(moduleDataPath)).filePath("models").toUtf8().toStdString();
  }
  return result;
}

std::unique_ptr<CredentialStore> createCredentialStore()
{
  return std::make_unique<WindowsCredentialStore>();
}

}  // namespace kaltura_live::platform
