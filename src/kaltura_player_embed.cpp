#include "kaltura_live/kaltura_player_embed.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QString>

namespace kaltura_live {

namespace {

QString jsonString(std::string_view value)
{
  QByteArray encoded = QJsonDocument(
    QJsonArray{QString::fromUtf8(value)}).toJson(QJsonDocument::Compact);
  if (encoded.size() >= 2) {
    encoded.remove(0, 1);
    encoded.chop(1);
  }
  return QString::fromUtf8(encoded);
}

}  // namespace

std::string buildKalturaPlayerHtml(std::int64_t partnerId, std::string_view entryId,
                                   std::string_view session)
{
  if (partnerId <= 0 || entryId.empty() || session.empty()) return {};
  return QString::fromLatin1(
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<style>html,body{width:100%;height:100%;margin:0;padding:0;overflow:hidden;"
    "background:#000}#kaltura_player_425343777{width:200%;height:200%;margin:0;"
    "padding:0;transform:scale(.5);transform-origin:top left;background:#000}</style>"
    "<script src=\"https://cdnapisec.kaltura.com/p/%1/embedPlaykitJs/uiconf_id/%2\"></script>"
    "</head><body><div id=\"kaltura_player_425343777\"></div><script>"
    "try{const kalturaPlayer=KalturaPlayer.setup({targetId:\"kaltura_player_425343777\","
    "provider:{partnerId:%1,uiConfId:%2,ks:%3}});"
    "kalturaPlayer.loadMedia({entryId:%4});}catch(e){console.error(e.message)}"
    "</script></body></html>")
    .arg(partnerId)
    .arg(kKalturaPlayerUiConfId)
    .arg(jsonString(session), jsonString(entryId))
    .toUtf8().toStdString();
}

}  // namespace kaltura_live
