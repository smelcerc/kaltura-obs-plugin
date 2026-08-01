#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QUrl>

#include <functional>

namespace kaltura_live::api {

struct HttpRequest {
  QUrl url;
  QByteArray body;
  QHash<QByteArray, QByteArray> headers;
  int timeoutMs = 10'000;
};

struct HttpResponse {
  int statusCode = 0;
  QByteArray body;
  QString errorMessage;
  bool timedOut = false;
};

using HttpCompletion = std::function<void(HttpResponse)>;

class HttpTransport {
public:
  virtual ~HttpTransport() = default;
  virtual void post(HttpRequest request, HttpCompletion completion) = 0;
};

}  // namespace kaltura_live::api
