#include "kaltura_live/platform/platform.hpp"

#include <QDir>

#ifdef KALTURA_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

namespace {
#ifdef KALTURA_HAVE_LIBSECRET
const SecretSchema kCredentialSchema = {
  "com.kaltura.obs.kaltura-live", SECRET_SCHEMA_NONE,
  {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};

class LibsecretCredentialStore final : public kaltura_live::platform::CredentialStore {
public:
  std::optional<std::string> load(std::string_view key) override
  {
    const std::string account(key);
    GError *error = nullptr;
    gchar *password = secret_password_lookup_sync(
      &kCredentialSchema, nullptr, &error, "account", account.c_str(), nullptr);
    if (error) {
      g_error_free(error);
      return std::nullopt;
    }
    if (!password) {
      return std::nullopt;
    }
    std::string value(password);
    secret_password_free(password);
    return value;
  }

  bool save(std::string_view key, std::string_view secret) override
  {
    const std::string account(key);
    const std::string value(secret);
    GError *error = nullptr;
    const gboolean stored = secret_password_store_sync(
      &kCredentialSchema, SECRET_COLLECTION_DEFAULT, "Kaltura Live OBS Session",
      value.c_str(), nullptr, &error, "account", account.c_str(), nullptr);
    if (error) {
      g_error_free(error);
    }
    return stored == TRUE;
  }

  bool remove(std::string_view key) override
  {
    const std::string account(key);
    GError *error = nullptr;
    const gboolean cleared = secret_password_clear_sync(
      &kCredentialSchema, nullptr, &error, "account", account.c_str(), nullptr);
    if (error) {
      g_error_free(error);
    }
    return cleared == TRUE;
  }

  bool available() const override { return true; }
  const char *backendName() const override { return "Secret Service (libsecret)"; }
};
#else
class UnavailableCredentialStore final : public kaltura_live::platform::CredentialStore {
public:
  std::optional<std::string> load(std::string_view) override { return std::nullopt; }
  bool save(std::string_view, std::string_view) override { return false; }
  bool remove(std::string_view) override { return true; }
  bool available() const override { return false; }
  const char *backendName() const override { return "unavailable Secret Service"; }
};
#endif
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
#ifdef KALTURA_HAVE_LIBSECRET
  return std::make_unique<LibsecretCredentialStore>();
#else
  return std::make_unique<UnavailableCredentialStore>();
#endif
}

}  // namespace kaltura_live::platform
