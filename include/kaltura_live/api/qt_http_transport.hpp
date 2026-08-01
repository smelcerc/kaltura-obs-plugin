#pragma once

#include "kaltura_live/api/http_transport.hpp"

#include <QNetworkAccessManager>
#include <QObject>

namespace kaltura_live::api {

class QtHttpTransport final : public QObject, public HttpTransport {
public:
  explicit QtHttpTransport(QObject *parent = nullptr);
  void post(HttpRequest request, HttpCompletion completion) override;

private:
  QNetworkAccessManager networkManager_;
};

}  // namespace kaltura_live::api
