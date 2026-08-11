#include "kaltura_live/stream_output_config.hpp"

#include <QUrl>
#include <algorithm>

namespace kaltura_live {

const char *outputProtocolName(OutputProtocol protocol)
{
  switch (protocol) {
  case OutputProtocol::RTMP: return "RTMP";
  case OutputProtocol::RTMPS: return "RTMPS";
  case OutputProtocol::SRT: return "SRT";
  }
  return "Unknown";
}

const char *obsOutputType(OutputProtocol protocol)
{
  return protocol == OutputProtocol::SRT ? "ffmpeg_mpegts_muxer" : "rtmp_output";
}

std::string buildSrtUri(const StreamOutputConfig &config)
{
  QUrl url(QString::fromUtf8(config.endpoint));
  if (url.scheme().compare("srt", Qt::CaseInsensitive) != 0) url = QUrl{};
  url.setScheme("srt");
  if (!config.srt.host.empty()) url.setHost(QString::fromUtf8(config.srt.host));
  if (config.srt.port) url.setPort(config.srt.port);
  url.setQuery({});
  url.setFragment({});
  std::string target = url.toString(QUrl::FullyEncoded).toUtf8().toStdString();
  if (!config.srt.streamId.empty()) {
    target += "?streamid=" + config.srt.streamId;
    target += "&latency=" +
      std::to_string(std::clamp(config.srt.latencyMs, 250, 8'000) * 1'000);
  }
  return target;
}

std::string endpointWithoutSecrets(std::string_view endpoint)
{
  if (endpoint.empty()) return {};
  QUrl url(QString::fromUtf8(endpoint));
  if (!url.isValid()) return std::string(endpoint);
  url.setQuery({});
  url.setFragment({});
  return url.toString(QUrl::FullyEncoded).toUtf8().toStdString();
}

bool validateOutputConfig(const StreamOutputConfig &config, std::string &failure)
{
  if (!config.enabled) return true;
  const std::string endpoint = config.protocol == OutputProtocol::SRT
    ? buildSrtUri(config) : config.endpoint;
  const QUrl url(QString::fromUtf8(endpoint));
  const QString expected = QString::fromLatin1(outputProtocolName(config.protocol)).toLower();
  if (!url.isValid() || url.scheme().compare(expected, Qt::CaseInsensitive) != 0 ||
      url.host().isEmpty()) {
    failure = config.name + " needs a valid " + outputProtocolName(config.protocol) +
              " endpoint.";
    return false;
  }
  if (config.protocol != OutputProtocol::SRT && config.streamKey.empty()) {
    failure = config.name + " needs a stream key.";
    return false;
  }
  if (config.protocol == OutputProtocol::SRT && config.srt.pbkeylen != 0 &&
      config.srt.pbkeylen != 16 && config.srt.pbkeylen != 24 && config.srt.pbkeylen != 32) {
    failure = config.name + " SRT PBKEYLEN must be 16, 24, or 32 bytes.";
    return false;
  }
  if (config.protocol == OutputProtocol::SRT && !config.srt.passphrase.empty() &&
      (config.srt.passphrase.size() < 10 || config.srt.passphrase.size() > 79)) {
    failure = config.name + " SRT passphrase must contain 10 to 79 characters.";
    return false;
  }
  return true;
}

StreamOutputConfig mapKalturaOutput(const api::StreamConfiguration &configuration,
                                    OutputRole role, OutputProtocol preferredProtocol)
{
  const bool backup = role == OutputRole::Backup;
  StreamOutputConfig result;
  result.name = backup ? "Backup" : "Primary";
  result.username = configuration.keys.username;
  result.password = configuration.keys.password;
  result.streamKey = configuration.keys.rtmp.empty() ? "1" : configuration.keys.rtmp;
  const QUrl srt = backup ? configuration.urls.backupSrt : configuration.urls.primarySrt;
  const QUrl secure = backup ? configuration.urls.backupSecure : configuration.urls.primarySecure;
  const QUrl rtmp = backup ? configuration.urls.backup : configuration.urls.primary;
  result.kalturaRtmpEndpoint = rtmp.toString(QUrl::FullyEncoded).toUtf8().toStdString();
  result.kalturaRtmpsEndpoint = secure.toString(QUrl::FullyEncoded).toUtf8().toStdString();
  result.kalturaSrtEndpoint = srt.toString(QUrl::FullyEncoded).toUtf8().toStdString();
  result.kalturaSrtStreamId = backup ? configuration.keys.backupSrt
                                     : configuration.keys.primarySrt;
  auto useSrt = [&] {
    result.protocol = OutputProtocol::SRT;
    result.endpoint = srt.toString(QUrl::FullyEncoded).toUtf8().toStdString();
    result.srt.streamId = backup ? configuration.keys.backupSrt : configuration.keys.primarySrt;
  };
  auto useSecure = [&] {
    result.protocol = OutputProtocol::RTMPS;
    result.endpoint = secure.toString(QUrl::FullyEncoded).toUtf8().toStdString();
  };
  auto useRtmp = [&] {
    result.protocol = OutputProtocol::RTMP;
    result.endpoint = rtmp.toString(QUrl::FullyEncoded).toUtf8().toStdString();
  };
  if (preferredProtocol == OutputProtocol::SRT && !srt.isEmpty()) useSrt();
  else if (preferredProtocol == OutputProtocol::RTMPS && !secure.isEmpty()) useSecure();
  else if (preferredProtocol == OutputProtocol::RTMP && !rtmp.isEmpty()) useRtmp();
  else if (!rtmp.isEmpty()) useRtmp();
  else if (!secure.isEmpty()) useSecure();
  else useSrt();
  return result;
}

}  // namespace kaltura_live
