#include "kaltura_live/api/kaltura_api_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace {

using namespace kaltura_live::api;

ApiError error(ApiErrorKind kind, std::string message, int status = 0,
               std::string code = {})
{
  return ApiError{kind, std::move(code), std::move(message), status, 0};
}

std::string stringValue(const QJsonObject &object, const char *key)
{
  const QJsonValue value = object.value(QLatin1String(key));
  return value.isString() ? value.toString().toUtf8().toStdString() : std::string{};
}

std::string rtmpStreamName(const QJsonObject &object)
{
  QString streamName = object.value(QLatin1String("streamName")).toString();
  streamName.replace(QLatin1String("%i"), QLatin1String("1"));
  return streamName.toUtf8().toStdString();
}

std::optional<std::int64_t> optionalIntegerValue(const QJsonObject &object, const char *key)
{
  const QJsonValue value = object.value(QLatin1String(key));
  if (value.isString()) {
    bool converted = false;
    const qlonglong result = value.toString().toLongLong(&converted, 10);
    return converted ? std::optional<std::int64_t>(result) : std::nullopt;
  }
  if (value.isDouble()) {
    const double number = value.toDouble();
    constexpr double kMaximumExactJsonInteger = 9'007'199'254'740'991.0;
    if (std::isfinite(number) && std::trunc(number) == number &&
        number >= -kMaximumExactJsonInteger && number <= kMaximumExactJsonInteger) {
      return static_cast<std::int64_t>(number);
    }
  }
  return std::nullopt;
}

std::int64_t integerValue(const QJsonObject &object, const char *key)
{
  return optionalIntegerValue(object, key).value_or(0);
}

QUrl urlValue(const QJsonObject &object, const char *key)
{
  const QJsonValue value = object.value(QLatin1String(key));
  if (!value.isString()) {
    return {};
  }
  const QUrl url(value.toString(), QUrl::StrictMode);
  return url.isValid() && !url.scheme().isEmpty() ? url : QUrl{};
}

bool hasApiError(const QJsonObject &object)
{
  const QString objectType = object.value("objectType").toString();
  return objectType.contains("APIException", Qt::CaseInsensitive) ||
         (!object.value("code").toString().isEmpty() &&
          !object.value("message").toString().isEmpty() && object.size() <= 6);
}

bool isRetryable(const HttpResponse &response)
{
  return response.timedOut || response.statusCode == 0 || response.statusCode == 408 ||
         response.statusCode == 429 || response.statusCode >= 500;
}

bool isValidSession(std::string_view session)
{
  if (session.empty() || session.size() > 4096) {
    return false;
  }
  return std::all_of(session.begin(), session.end(), [](unsigned char character) {
    return character >= 0x21 && character <= 0x7e;
  });
}

bool isValidEntryId(std::string_view entryId)
{
  if (entryId.empty() || entryId.size() > 128) {
    return false;
  }
  return std::all_of(entryId.begin(), entryId.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
  });
}

bool isValidSearchText(std::string_view searchText)
{
  if (searchText.size() > 1024) {
    return false;
  }
  const QByteArray utf8(searchText.data(), static_cast<qsizetype>(searchText.size()));
  const QString decoded = QString::fromUtf8(utf8);
  if (decoded.size() > 256 || decoded.toUtf8() != utf8) {
    return false;
  }
  return std::all_of(decoded.cbegin(), decoded.cend(), [](QChar character) {
    return character.isPrint() || character.isSpace();
  });
}

QJsonObject sessionParameters(std::string session)
{
  QJsonObject parameters;
  parameters.insert("ks", QString::fromUtf8(session));
  return parameters;
}

struct RequestState {
  std::shared_ptr<HttpTransport> transport;
  ClientConfig config;
  HttpRequest request;
  std::function<void(ApiResult<QJsonObject>)> completion;
  int attempt = 0;
};

void dispatch(const std::shared_ptr<RequestState> &state)
{
  ++state->attempt;
  state->transport->post(state->request, [state](HttpResponse response) {
    if (isRetryable(response) && state->attempt <= state->config.maximumRetries) {
      QTimer::singleShot(std::max(0, state->config.retryDelayMs),
                         [state]() { dispatch(state); });
      return;
    }

    if (response.timedOut) {
      ApiError failure = error(ApiErrorKind::Timeout, "Kaltura request timed out");
      failure.attempts = state->attempt;
      state->completion(ApiResult<QJsonObject>::failure(std::move(failure)));
      return;
    }
    if (response.statusCode == 0 || !response.errorMessage.isEmpty()) {
      ApiError failure = error(ApiErrorKind::Network,
                               response.errorMessage.toUtf8().toStdString());
      failure.attempts = state->attempt;
      state->completion(ApiResult<QJsonObject>::failure(std::move(failure)));
      return;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
      ApiError failure = error(ApiErrorKind::Http, "Kaltura returned an HTTP error",
                               response.statusCode);
      failure.attempts = state->attempt;
      state->completion(ApiResult<QJsonObject>::failure(std::move(failure)));
      return;
    }
    if (response.body.size() > state->config.maximumResponseBytes) {
      ApiError failure = error(ApiErrorKind::InvalidResponse, "Kaltura response was too large");
      failure.attempts = state->attempt;
      state->completion(ApiResult<QJsonObject>::failure(std::move(failure)));
      return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      ApiError failure = error(ApiErrorKind::InvalidJson, "Kaltura returned invalid JSON");
      failure.attempts = state->attempt;
      state->completion(ApiResult<QJsonObject>::failure(std::move(failure)));
      return;
    }

    const QJsonObject object = document.object();
    if (hasApiError(object)) {
      ApiError failure = error(ApiErrorKind::Api,
                               stringValue(object, "message"), response.statusCode,
                               stringValue(object, "code"));
      failure.attempts = state->attempt;
      state->completion(ApiResult<QJsonObject>::failure(std::move(failure)));
      return;
    }
    state->completion(
      ApiResult<QJsonObject>::success(object, response.statusCode, state->attempt));
  });
}

}  // namespace

namespace kaltura_live::api {

KalturaApiClient::KalturaApiClient(std::shared_ptr<HttpTransport> transport, ClientConfig config)
  : transport_(std::move(transport)), config_(std::move(config))
{
  config_.timeoutMs = std::clamp(config_.timeoutMs, 1, 600'000);
  config_.maximumRetries = std::clamp(config_.maximumRetries, 0, 10);
  config_.retryDelayMs = std::clamp(config_.retryDelayMs, 0, 60'000);
  config_.maximumResponseBytes = std::clamp(config_.maximumResponseBytes, 1, 64 * 1024 * 1024);
}

void KalturaApiClient::validateSession(std::string session, SessionCompletion completion) const
{
  if (!isValidSession(session)) {
    completion(ApiResult<SessionInfo>::failure(
      error(ApiErrorKind::InvalidRequest, "Kaltura Session is required")));
    return;
  }

  QJsonObject parameters = sessionParameters(session);
  parameters.insert("session", QString::fromUtf8(session));
  call("session", "get", std::move(parameters),
       [completion = std::move(completion)](ApiResult<QJsonObject> response) mutable {
         if (!response.succeeded()) {
           completion(ApiResult<SessionInfo>::failure(std::move(*response.error)));
           return;
         }
         const QJsonObject &object = *response.value;
         const std::optional<std::int64_t> partnerId =
           optionalIntegerValue(object, "partnerId");
         const std::optional<std::int64_t> expiry = optionalIntegerValue(object, "expiry");
         const std::optional<std::int64_t> sessionType =
           optionalIntegerValue(object, "sessionType");
         if (!partnerId || !expiry || !sessionType) {
           ApiError failure =
             error(ApiErrorKind::InvalidResponse,
                   "Session response contains missing or invalid numeric fields",
                   response.httpStatus);
           failure.attempts = response.attempts;
           completion(ApiResult<SessionInfo>::failure(std::move(failure)));
           return;
         }
         SessionInfo info;
         info.partnerId = *partnerId;
         info.userId = stringValue(object, "userId");
         info.expiry = *expiry;
         info.privileges = stringValue(object, "privileges");
         info.type = *sessionType == 0
                       ? SessionType::User
                       : *sessionType == 2 ? SessionType::Admin : SessionType::Unknown;
         completion(ApiResult<SessionInfo>::success(std::move(info), response.httpStatus,
                                                    response.attempts));
       });
}

void KalturaApiClient::listLiveEntries(std::string session, ListLiveEntriesRequest request,
                                       LiveEntriesCompletion completion) const
{
  if (!isValidSession(session) || request.pageIndex < 1 || request.pageSize < 1 ||
      request.pageSize > 500 || !isValidSearchText(request.searchText)) {
    completion(ApiResult<LiveEntryPage>::failure(
      error(ApiErrorKind::InvalidRequest, "Invalid session or paging parameters")));
    return;
  }

  QJsonObject parameters = sessionParameters(std::move(session));
  parameters.insert("filter:objectType", "KalturaLiveStreamEntryFilter");
  parameters.insert("filter:orderBy", "-createdAt");
  parameters.insert("pager:objectType", "KalturaFilterPager");
  parameters.insert("pager:pageIndex", request.pageIndex);
  parameters.insert("pager:pageSize", request.pageSize);
  if (!request.searchText.empty()) {
    parameters.insert("filter:freeText", QString::fromUtf8(request.searchText));
  }
  call("livestream", "list", std::move(parameters),
       [request, completion = std::move(completion)](ApiResult<QJsonObject> response) mutable {
         if (!response.succeeded()) {
           completion(ApiResult<LiveEntryPage>::failure(std::move(*response.error)));
           return;
         }
         const QJsonValue objectsValue = response.value->value("objects");
         const std::optional<std::int64_t> totalCount =
           optionalIntegerValue(*response.value, "totalCount");
         if (!objectsValue.isArray() || !totalCount || *totalCount < 0 ||
             *totalCount > std::numeric_limits<int>::max()) {
           ApiError failure =
             error(ApiErrorKind::InvalidResponse,
                   "Live-entry response is missing required fields", response.httpStatus);
           failure.attempts = response.attempts;
           completion(ApiResult<LiveEntryPage>::failure(std::move(failure)));
           return;
         }

         LiveEntryPage page;
         page.totalCount = static_cast<int>(*totalCount);
         page.pageIndex = request.pageIndex;
         page.pageSize = request.pageSize;
         for (const QJsonValue &value : objectsValue.toArray()) {
           if (!value.isObject()) {
             completion(ApiResult<LiveEntryPage>::failure(
               error(ApiErrorKind::InvalidResponse, "Live-entry response contains an invalid item")));
             return;
           }
           const QJsonObject object = value.toObject();
           const std::string id = stringValue(object, "id");
           if (id.empty()) {
             completion(ApiResult<LiveEntryPage>::failure(
               error(ApiErrorKind::InvalidResponse, "Live entry is missing its ID")));
             return;
           }
           page.entries.push_back(LiveEntry{id, stringValue(object, "name"),
                                            stringValue(object, "description"),
                                            urlValue(object, "thumbnailUrl"),
                                            integerValue(object, "createdAt"),
                                            static_cast<int>(integerValue(object, "status"))});
         }
         completion(ApiResult<LiveEntryPage>::success(std::move(page), response.httpStatus,
                                                      response.attempts));
       });
}

void KalturaApiClient::getStreamUrls(std::string session, std::string entryId,
                                     StreamUrlsCompletion completion) const
{
  if (!isValidSession(session) || !isValidEntryId(entryId)) {
    completion(ApiResult<StreamUrls>::failure(
      error(ApiErrorKind::InvalidRequest, "Session and entry ID are required")));
    return;
  }
  QJsonObject parameters = sessionParameters(std::move(session));
  parameters.insert("entryId", QString::fromUtf8(entryId));
  call("livestream", "get", std::move(parameters),
       [completion = std::move(completion)](ApiResult<QJsonObject> response) mutable {
         if (!response.succeeded()) {
           completion(ApiResult<StreamUrls>::failure(std::move(*response.error)));
           return;
         }
         StreamUrls urls{urlValue(*response.value, "primaryBroadcastingUrl"),
                         urlValue(*response.value, "secondaryBroadcastingUrl"),
                         urlValue(*response.value, "primarySecuredBroadcastingUrl"),
                         urlValue(*response.value, "secondarySecuredBroadcastingUrl"),
                         urlValue(*response.value, "primaryRtspBroadcastingUrl"),
                         urlValue(*response.value, "secondaryRtspBroadcastingUrl"),
                         urlValue(*response.value, "primarySrtBroadcastingUrl"),
                         urlValue(*response.value, "secondarySrtBroadcastingUrl"),
                         urlValue(*response.value, "streamUrl"),
                         urlValue(*response.value, "hlsStreamUrl")};
         if (urls.primary.isEmpty() && urls.backup.isEmpty() && urls.primarySecure.isEmpty() &&
             urls.backupSecure.isEmpty() && urls.primaryRtsp.isEmpty() && urls.backupRtsp.isEmpty() &&
             urls.primarySrt.isEmpty() && urls.backupSrt.isEmpty() && urls.playback.isEmpty() &&
             urls.hlsPlayback.isEmpty()) {
           completion(ApiResult<StreamUrls>::failure(
             error(ApiErrorKind::InvalidResponse, "Live entry contains no valid stream URLs")));
           return;
         }
         completion(ApiResult<StreamUrls>::success(std::move(urls)));
       });
}

void KalturaApiClient::getStreamKeys(std::string session, std::string entryId,
                                     StreamKeysCompletion completion) const
{
  if (!isValidSession(session) || !isValidEntryId(entryId)) {
    completion(ApiResult<StreamKeys>::failure(
      error(ApiErrorKind::InvalidRequest, "Session and entry ID are required")));
    return;
  }
  QJsonObject parameters = sessionParameters(std::move(session));
  parameters.insert("entryId", QString::fromUtf8(entryId));
  call("livestream", "get", std::move(parameters),
       [completion = std::move(completion)](ApiResult<QJsonObject> response) mutable {
         if (!response.succeeded()) {
           completion(ApiResult<StreamKeys>::failure(std::move(*response.error)));
           return;
         }
         StreamKeys keys{rtmpStreamName(*response.value),
                         stringValue(*response.value, "primarySrtStreamId"),
                         stringValue(*response.value, "secondarySrtStreamId"),
                         stringValue(*response.value, "streamUsername"),
                         stringValue(*response.value, "streamPassword")};
         if (keys.rtmp.empty() && keys.primarySrt.empty() && keys.backupSrt.empty()) {
           completion(ApiResult<StreamKeys>::failure(
             error(ApiErrorKind::InvalidResponse, "Live entry contains no stream key")));
           return;
         }
         completion(ApiResult<StreamKeys>::success(std::move(keys)));
       });
}

void KalturaApiClient::getStreamConfiguration(
  std::string session, std::string entryId, StreamConfigurationCompletion completion) const
{
  if (!isValidSession(session) || !isValidEntryId(entryId)) {
    completion(ApiResult<StreamConfiguration>::failure(
      error(ApiErrorKind::InvalidRequest, "Session and entry ID are required")));
    return;
  }
  QJsonObject parameters = sessionParameters(std::move(session));
  parameters.insert("entryId", QString::fromUtf8(entryId));
  call("livestream", "get", std::move(parameters),
       [completion = std::move(completion)](ApiResult<QJsonObject> response) mutable {
         if (!response.succeeded()) {
           completion(ApiResult<StreamConfiguration>::failure(std::move(*response.error)));
           return;
         }
         StreamConfiguration configuration;
         configuration.urls =
           StreamUrls{urlValue(*response.value, "primaryBroadcastingUrl"),
                      urlValue(*response.value, "secondaryBroadcastingUrl"),
                      urlValue(*response.value, "primarySecuredBroadcastingUrl"),
                      urlValue(*response.value, "secondarySecuredBroadcastingUrl"),
                      urlValue(*response.value, "primaryRtspBroadcastingUrl"),
                      urlValue(*response.value, "secondaryRtspBroadcastingUrl"),
                      urlValue(*response.value, "primarySrtBroadcastingUrl"),
                      urlValue(*response.value, "secondarySrtBroadcastingUrl"),
                      urlValue(*response.value, "streamUrl"),
                      urlValue(*response.value, "hlsStreamUrl")};
         configuration.keys =
           StreamKeys{rtmpStreamName(*response.value),
                      stringValue(*response.value, "primarySrtStreamId"),
                      stringValue(*response.value, "secondarySrtStreamId"),
                      stringValue(*response.value, "streamUsername"),
                      stringValue(*response.value, "streamPassword")};
         const bool hasRtmpUrl = !configuration.urls.primary.isEmpty() ||
                                 !configuration.urls.backup.isEmpty() ||
                                 !configuration.urls.primarySecure.isEmpty() ||
                                 !configuration.urls.backupSecure.isEmpty();
         if (!hasRtmpUrl || configuration.keys.rtmp.empty()) {
           ApiError failure =
             error(ApiErrorKind::InvalidResponse,
                   "Live entry is missing its RTMP URL or stream key", response.httpStatus);
           failure.attempts = response.attempts;
           completion(ApiResult<StreamConfiguration>::failure(std::move(failure)));
           return;
         }
         completion(ApiResult<StreamConfiguration>::success(
           std::move(configuration), response.httpStatus, response.attempts));
       });
}

void KalturaApiClient::call(std::string service, std::string action, QJsonObject parameters,
                            JsonCompletion completion) const
{
  if (!transport_ || !config_.serviceUrl.isValid() || config_.serviceUrl.scheme() != "https") {
    completion(ApiResult<QJsonObject>::failure(
      error(ApiErrorKind::InvalidRequest, "A valid HTTPS service URL is required")));
    return;
  }

  QUrl endpoint = config_.serviceUrl;
  QString path = endpoint.path();
  if (!path.endsWith('/')) {
    path += '/';
  }
  path += "service/" + QString::fromUtf8(service) + "/action/" + QString::fromUtf8(action);
  endpoint.setPath(path);

  // Match the documented session/action/get curl semantics. Encode each value
  // exactly once for an application/x-www-form-urlencoded body; in particular,
  // base64 padding and '+' characters must not be interpreted as form syntax.
  QList<QByteArray> formFields;
  formFields.push_back("format=1");
  for (auto item = parameters.constBegin(); item != parameters.constEnd(); ++item) {
    QByteArray value;
    if (item.value().isString()) {
      value = QUrl::toPercentEncoding(item.value().toString());
    } else if (item.value().isDouble()) {
      value = QByteArray::number(item.value().toInt());
    } else {
      continue;
    }
    formFields.push_back(QUrl::toPercentEncoding(item.key()) + '=' + value);
  }

  HttpRequest request;
  request.url = std::move(endpoint);
  request.body = formFields.join('&');
  request.headers.insert("Content-Type", "application/x-www-form-urlencoded");
  request.headers.insert("Accept", "application/json");
  request.timeoutMs = config_.timeoutMs;

  auto state = std::make_shared<RequestState>();
  state->transport = transport_;
  state->config = config_;
  state->request = std::move(request);
  state->completion = std::move(completion);
  dispatch(state);
}

}  // namespace kaltura_live::api
