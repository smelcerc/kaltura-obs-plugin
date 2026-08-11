#pragma once

#include "kaltura_live/api/models.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace kaltura_live {

enum class OutputProtocol { RTMP, RTMPS, SRT };
enum class SrtMode { Caller, Listener, Rendezvous };

struct ReconnectOptions {
  bool enabled = true;
  int delaySeconds = 2;
  int maxRetries = 20;
};

struct SrtOptions {
  std::string host;
  uint16_t port = 0;
  SrtMode mode = SrtMode::Caller;
  int latencyMs = 3'000;
  std::string passphrase;
  int pbkeylen = 0;
  std::string streamId;
  int timeoutMs = 5'000;
  int packetSize = 1316;
};

struct StreamOutputConfig {
  std::string name;
  bool enabled = true;
  OutputProtocol protocol = OutputProtocol::RTMP;
  std::string endpoint;
  std::string streamKey;
  std::string username;
  std::string password;
  ReconnectOptions reconnect;
  SrtOptions srt;
  bool manualOverride = false;
  std::string kalturaRtmpEndpoint;
  std::string kalturaRtmpsEndpoint;
  std::string kalturaSrtEndpoint;
  std::string kalturaSrtStreamId;
};

enum class OutputRole { Primary, Backup };

struct KalturaIngestEndpoint {
  OutputRole role = OutputRole::Primary;
  OutputProtocol protocol = OutputProtocol::RTMP;
  std::string endpoint;
  std::string streamKey;
  std::string username;
  std::string password;
};

[[nodiscard]] const char *outputProtocolName(OutputProtocol protocol);
[[nodiscard]] const char *obsOutputType(OutputProtocol protocol);
[[nodiscard]] std::string buildSrtUri(const StreamOutputConfig &config);
[[nodiscard]] std::string endpointWithoutSecrets(std::string_view endpoint);
[[nodiscard]] bool validateOutputConfig(const StreamOutputConfig &config,
                                        std::string &failure);
[[nodiscard]] StreamOutputConfig mapKalturaOutput(
  const api::StreamConfiguration &configuration, OutputRole role,
  OutputProtocol preferredProtocol = OutputProtocol::RTMP);

}  // namespace kaltura_live
