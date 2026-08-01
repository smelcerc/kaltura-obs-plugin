#include "mock_http_transport.hpp"

#include "kaltura_live/api/kaltura_api_client.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>

#include <functional>
#include <iostream>
#include <memory>
#include <optional>

namespace {

using namespace kaltura_live::api;
using kaltura_live::api::test::MockHttpTransport;

#define REQUIRE(condition)                                                                        \
  do {                                                                                            \
    if (!(condition)) {                                                                           \
      std::cerr << __func__ << ": requirement failed: " #condition << '\n';                       \
      return false;                                                                               \
    }                                                                                             \
  } while (false)

QByteArray fixture(const char *name)
{
  QFile file(QString::fromUtf8(KALTURA_TEST_FIXTURES_DIR) + "/" + name);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll();
}

bool waitFor(const std::function<bool()> &condition)
{
  QElapsedTimer timer;
  timer.start();
  while (!condition() && timer.elapsed() < 2'000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::yieldCurrentThread();
  }
  return condition();
}

ClientConfig testConfig()
{
  ClientConfig config;
  config.retryDelayMs = 0;
  return config;
}

bool parsesSession()
{
  auto transport = std::make_shared<MockHttpTransport>();
  transport->enqueue({200, fixture("session_valid.json"), {}, false});
  KalturaApiClient client(transport, testConfig());
  std::optional<ApiResult<SessionInfo>> result;
  client.validateSession("test+session-token==", [&](auto response) { result = std::move(response); });
  REQUIRE(waitFor([&]() { return result.has_value(); }));
  REQUIRE(result->succeeded());
  REQUIRE(result->value->partnerId == 12345);
  REQUIRE(result->value->type == SessionType::Admin);
  REQUIRE(result->httpStatus == 200);
  REQUIRE(result->attempts == 1);
  REQUIRE(transport->requests().front().url.path().endsWith("/service/session/action/get"));
  REQUIRE(transport->requests().front().headers.value("Content-Type") ==
          "application/x-www-form-urlencoded");
  REQUIRE(transport->requests().front().body ==
          "format=1&ks=test%2Bsession-token%3D%3D&session=test%2Bsession-token%3D%3D");
  return true;
}

bool parsesLiveEntries()
{
  auto transport = std::make_shared<MockHttpTransport>();
  transport->enqueue({200, fixture("live_entries.json"), {}, false});
  KalturaApiClient client(transport, testConfig());
  std::optional<ApiResult<LiveEntryPage>> result;
  client.listLiveEntries("test-session-token", {2, 25, "town hall"},
                         [&](auto response) { result = std::move(response); });
  REQUIRE(waitFor([&]() { return result.has_value(); }));
  REQUIRE(result->succeeded());
  REQUIRE(result->value->entries.size() == 2);
  REQUIRE(result->value->entries.front().id == "1_livealpha");
  REQUIRE(result->value->entries.front().thumbnailUrl.host() == "example.test");
  REQUIRE(result->value->pageIndex == 2);
  REQUIRE(transport->requests().front().body.contains("filter%3AfreeText=town%20hall"));
  REQUIRE(transport->requests().front().body.contains("filter%3AorderBy=-createdAt"));
  return true;
}

bool parsesUrlsAndKeys()
{
  auto transport = std::make_shared<MockHttpTransport>();
  transport->enqueue({200, fixture("live_entry.json"), {}, false});
  transport->enqueue({200, fixture("live_entry.json"), {}, false});
  transport->enqueue({200, fixture("live_entry.json"), {}, false});
  KalturaApiClient client(transport, testConfig());
  std::optional<ApiResult<StreamUrls>> urls;
  std::optional<ApiResult<StreamKeys>> keys;
  std::optional<ApiResult<StreamConfiguration>> configuration;
  client.getStreamUrls("test-session-token", "1_livealpha",
                       [&](auto response) { urls = std::move(response); });
  client.getStreamKeys("test-session-token", "1_livealpha",
                       [&](auto response) { keys = std::move(response); });
  client.getStreamConfiguration("test-session-token", "1_livealpha",
                                [&](auto response) { configuration = std::move(response); });
  REQUIRE(waitFor([&]() {
    return urls.has_value() && keys.has_value() && configuration.has_value();
  }));
  REQUIRE(urls->succeeded());
  REQUIRE(urls->value->primary.scheme() == "rtmp");
  REQUIRE(urls->value->hlsPlayback.scheme() == "https");
  REQUIRE(keys->succeeded());
  REQUIRE(keys->value->rtmp == "rtmp-stream-1");
  REQUIRE(keys->value->backupSrt == "#!::r=backup-key");
  REQUIRE(configuration->succeeded());
  REQUIRE(configuration->value->urls.primarySecure.scheme() == "rtmps");
  REQUIRE(configuration->value->keys.rtmp == "rtmp-stream-1");
  REQUIRE(configuration->value->keys.username == "publisher-user");
  return true;
}

bool retriesTransientFailure()
{
  auto transport = std::make_shared<MockHttpTransport>();
  transport->enqueue({503, "{}", {}, false});
  transport->enqueue({200, fixture("session_valid.json"), {}, false});
  KalturaApiClient client(transport, testConfig());
  std::optional<ApiResult<SessionInfo>> result;
  client.validateSession("test-session-token", [&](auto response) { result = std::move(response); });
  REQUIRE(waitFor([&]() { return result.has_value(); }));
  REQUIRE(result->succeeded());
  REQUIRE(transport->requests().size() == 2);
  return true;
}

bool reportsTimeoutAfterRetries()
{
  auto transport = std::make_shared<MockHttpTransport>();
  transport->enqueue({0, {}, {}, true});
  transport->enqueue({0, {}, {}, true});
  transport->enqueue({0, {}, {}, true});
  KalturaApiClient client(transport, testConfig());
  std::optional<ApiResult<SessionInfo>> result;
  client.validateSession("test-session-token", [&](auto response) { result = std::move(response); });
  REQUIRE(waitFor([&]() { return result.has_value(); }));
  REQUIRE(!result->succeeded());
  REQUIRE(result->error->kind == ApiErrorKind::Timeout);
  REQUIRE(result->error->attempts == 3);
  return true;
}

bool rejectsMalformedAndApiErrorResponses()
{
  auto transport = std::make_shared<MockHttpTransport>();
  transport->enqueue({200, "not-json", {}, false});
  transport->enqueue({200, fixture("api_error.json"), {}, false});
  KalturaApiClient client(transport, testConfig());
  std::optional<ApiResult<SessionInfo>> malformed;
  std::optional<ApiResult<SessionInfo>> apiFailure;
  client.validateSession("test-session-token",
                         [&](auto response) { malformed = std::move(response); });
  client.validateSession("test-session-token",
                         [&](auto response) { apiFailure = std::move(response); });
  REQUIRE(waitFor([&]() { return malformed.has_value() && apiFailure.has_value(); }));
  REQUIRE(malformed->error->kind == ApiErrorKind::InvalidJson);
  REQUIRE(apiFailure->error->kind == ApiErrorKind::Api);
  REQUIRE(apiFailure->error->code == "INVALID_KS");
  return true;
}

}  // namespace

int main(int argc, char **argv)
{
  QCoreApplication application(argc, argv);
  const bool passed = parsesSession() && parsesLiveEntries() && parsesUrlsAndKeys() &&
                      retriesTransientFailure() && reportsTimeoutAfterRetries() &&
                      rejectsMalformedAndApiErrorResponses();
  return passed ? 0 : 1;
}
