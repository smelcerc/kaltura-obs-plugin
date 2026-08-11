#include "kaltura_live/stream_output_config.hpp"

#include <cassert>
#include <string>

#include <QUrl>

using namespace kaltura_live;

int main()
{
  for (const OutputProtocol primary : {OutputProtocol::RTMP, OutputProtocol::RTMPS,
                                       OutputProtocol::SRT}) {
    for (const OutputProtocol backup : {OutputProtocol::RTMP, OutputProtocol::RTMPS,
                                        OutputProtocol::SRT}) {
      assert(obsOutputType(primary));
      assert(obsOutputType(backup));
      assert(std::string(obsOutputType(primary)) ==
             (primary == OutputProtocol::SRT ? "ffmpeg_mpegts_muxer" : "rtmp_output"));
    }
  }
  StreamOutputConfig srt;
  srt.name = "Primary";
  srt.protocol = OutputProtocol::SRT;
  srt.srt.host = "ingest.example.test";
  srt.srt.port = 9000;
  srt.srt.streamId = "#!::r=primary";
  srt.srt.latencyMs = 180;
  const std::string uri = buildSrtUri(srt);
  assert(uri.starts_with("srt://ingest.example.test:9000"));
  assert(uri.find("streamid") != std::string::npos);
  std::string failure;
  assert(validateOutputConfig(srt, failure));
  srt.srt.passphrase = "short";
  assert(!validateOutputConfig(srt, failure));

  api::StreamConfiguration kaltura;
  kaltura.urls.primarySrt = QUrl("srt://oaa4j4vh.p.srt.publish.live.kaltura.com:7045");
  kaltura.urls.backupSrt = QUrl("srt://oaa4j4vh.b.srt.publish.live.kaltura.com:7045");
  kaltura.keys.primarySrt = "#:::e=1_oaa4j4vh,st=0,p=7a2dbe04";
  kaltura.keys.backupSrt = "#:::e=1_oaa4j4vh,st=1,p=7a2dbe04";
  const StreamOutputConfig primary = mapKalturaOutput(kaltura, OutputRole::Primary);
  const StreamOutputConfig backup = mapKalturaOutput(kaltura, OutputRole::Backup);
  assert(primary.streamKey == "1");
  assert(backup.streamKey == "1");
  assert(buildSrtUri(primary) ==
    "srt://oaa4j4vh.p.srt.publish.live.kaltura.com:7045"
    "?streamid=#:::e=1_oaa4j4vh,st=0,p=7a2dbe04&latency=3000000");
  assert(buildSrtUri(backup) ==
    "srt://oaa4j4vh.b.srt.publish.live.kaltura.com:7045"
    "?streamid=#:::e=1_oaa4j4vh,st=1,p=7a2dbe04&latency=3000000");
  assert(endpointWithoutSecrets(
    "rtmps://ingest.example.test:443/kLive"
    "?t=synthetic-token") ==
    "rtmps://oaa4j4vh.p.kpublish.kaltura.com:443/kLive");
  assert(endpointWithoutSecrets(
    "srt://oaa4j4vh.p.srt.publish.live.kaltura.com:7045"
    "?streamid=synthetic-stream-id&latency=3000000") ==
    "srt://oaa4j4vh.p.srt.publish.live.kaltura.com:7045");
  return 0;
}
