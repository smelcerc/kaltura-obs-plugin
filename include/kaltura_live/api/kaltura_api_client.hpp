#pragma once

#include "kaltura_live/api/http_transport.hpp"
#include "kaltura_live/api/models.hpp"

#include <QJsonObject>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace kaltura_live::api {

enum class ApiErrorKind {
  InvalidRequest,
  Network,
  Timeout,
  Http,
  InvalidJson,
  InvalidResponse,
  Api,
};

struct ApiError {
  ApiErrorKind kind = ApiErrorKind::InvalidResponse;
  std::string code;
  std::string message;
  int httpStatus = 0;
  int attempts = 0;
};

template<typename T> struct ApiResult {
  std::optional<T> value;
  std::optional<ApiError> error;
  int httpStatus = 0;
  int attempts = 0;

  [[nodiscard]] bool succeeded() const { return value.has_value() && !error.has_value(); }

  static ApiResult success(T result, int status = 0, int requestAttempts = 0)
  {
    ApiResult response;
    response.value = std::move(result);
    response.httpStatus = status;
    response.attempts = requestAttempts;
    return response;
  }

  static ApiResult failure(ApiError failure)
  {
    ApiResult response;
    response.httpStatus = failure.httpStatus;
    response.attempts = failure.attempts;
    response.error = std::move(failure);
    return response;
  }
};

struct ClientConfig {
  QUrl serviceUrl = QUrl("https://www.kaltura.com/api_v3");
  int timeoutMs = 10'000;
  int maximumRetries = 2;
  int retryDelayMs = 250;
  int maximumResponseBytes = 2 * 1024 * 1024;
};

struct ListLiveEntriesRequest {
  int pageIndex = 1;
  int pageSize = 20;
  std::string searchText;
};

class KalturaApiClient {
public:
  using SessionCompletion = std::function<void(ApiResult<SessionInfo>)>;
  using LiveEntriesCompletion = std::function<void(ApiResult<LiveEntryPage>)>;
  using StreamUrlsCompletion = std::function<void(ApiResult<StreamUrls>)>;
  using StreamKeysCompletion = std::function<void(ApiResult<StreamKeys>)>;
  using StreamConfigurationCompletion =
    std::function<void(ApiResult<StreamConfiguration>)>;

  explicit KalturaApiClient(std::shared_ptr<HttpTransport> transport,
                            ClientConfig config = {});

  void validateSession(std::string session, SessionCompletion completion) const;
  void listLiveEntries(std::string session, ListLiveEntriesRequest request,
                       LiveEntriesCompletion completion) const;
  void getStreamUrls(std::string session, std::string entryId,
                     StreamUrlsCompletion completion) const;
  void getStreamKeys(std::string session, std::string entryId,
                     StreamKeysCompletion completion) const;
  void getStreamConfiguration(std::string session, std::string entryId,
                              StreamConfigurationCompletion completion) const;

private:
  using JsonCompletion = std::function<void(ApiResult<QJsonObject>)>;

  void call(std::string service, std::string action, QJsonObject parameters,
            JsonCompletion completion) const;

  std::shared_ptr<HttpTransport> transport_;
  ClientConfig config_;
};

}  // namespace kaltura_live::api
