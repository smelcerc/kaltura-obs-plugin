#include "kaltura_live/api/qt_http_transport.hpp"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QStringList>
#include <QTimer>

#include <utility>

namespace kaltura_live::api {

QtHttpTransport::QtHttpTransport(QObject *parent) : QObject(parent), networkManager_(this) {}

void QtHttpTransport::post(HttpRequest request, HttpCompletion completion)
{
  QNetworkRequest networkRequest(request.url);
  networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::NoLessSafeRedirectPolicy);
  for (auto header = request.headers.cbegin(); header != request.headers.cend(); ++header) {
    networkRequest.setRawHeader(header.key(), header.value());
  }

  QNetworkReply *reply = networkManager_.post(networkRequest, request.body);
  auto *timeout = new QTimer(reply);
  timeout->setSingleShot(true);
  timeout->start(request.timeoutMs);

  connect(timeout, &QTimer::timeout, reply, [reply]() {
    reply->setProperty("kalturaTimedOut", true);
    reply->abort();
  });

  connect(reply, &QNetworkReply::sslErrors, reply,
          [reply](const QList<QSslError> &errors) {
            QStringList details;
            details.reserve(errors.size());
            for (const QSslError &sslError : errors) {
              details.push_back(sslError.errorString());
            }
            reply->setProperty("kalturaSslErrors", details.join("; "));
          });

  connect(reply, &QNetworkReply::finished, this,
          [reply, completion = std::move(completion)]() mutable {
            HttpResponse response;
            response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            response.body = reply->readAll();
            response.timedOut = reply->property("kalturaTimedOut").toBool();
            if (reply->error() != QNetworkReply::NoError && !response.timedOut) {
              response.errorMessage = reply->errorString();
              const QString sslDetails = reply->property("kalturaSslErrors").toString();
              if (!sslDetails.isEmpty()) {
                response.errorMessage += "; TLS: " + sslDetails;
              }
            }
            reply->deleteLater();
            completion(std::move(response));
          });
}

}  // namespace kaltura_live::api
