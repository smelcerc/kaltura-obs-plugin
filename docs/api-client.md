# Kaltura API Client

`kaltura_live::api::KalturaApiClient` is a reusable, UI-independent asynchronous client for
Kaltura's API v3. It supports session validation, paged live-entry listing, and retrieval of
RTMP/RTMPS/RTSP/SRT/playback URLs and RTMP/SRT stream keys.

## Construction

Create one `QtHttpTransport` on a thread with a running Qt event loop and inject it into the
client. The transport owns `QNetworkAccessManager`; the client owns no UI or OBS objects.

```cpp
auto transport = std::make_shared<kaltura_live::api::QtHttpTransport>();

kaltura_live::api::ClientConfig config;
config.timeoutMs = 10'000;
config.maximumRetries = 2;
config.retryDelayMs = 250;

kaltura_live::api::KalturaApiClient client(transport, config);
client.validateSession(session, [](auto result) {
  if (!result.succeeded()) {
    // Handle result.error without logging the session or stream keys.
    return;
  }
  const auto &sessionInfo = *result.value;
});
```

Each operation completes through a typed callback with either a model or `ApiError`. HTTP 408,
429, 5xx, network failures, and timeouts are retried up to `maximumRetries`. JSON type checks,
response-size limits, HTTPS-only service URLs, paging bounds, KS bounds, and entry-ID validation
are applied before returning data.

## Tests

`tests/mock_http_transport.hpp` provides an injectable response queue. JSON fixtures under
`tests/fixtures` cover successful session, live-list, URL/key parsing, and Kaltura API errors.

```bash
ctest --test-dir build --output-on-failure
```
