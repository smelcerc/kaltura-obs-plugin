#pragma once

#include <QUrl>

#include <cstdint>
#include <string>
#include <vector>

namespace kaltura_live::api {

enum class SessionType {
  User,
  Admin,
  Unknown,
};

struct SessionInfo {
  std::int64_t partnerId = 0;
  std::string userId;
  std::int64_t expiry = 0;
  SessionType type = SessionType::Unknown;
  std::string privileges;
};

struct LiveEntry {
  std::string id;
  std::string name;
  std::string description;
  QUrl thumbnailUrl;
  std::int64_t createdAt = 0;
  int status = 0;
};

struct LiveEntryPage {
  std::vector<LiveEntry> entries;
  int totalCount = 0;
  int pageIndex = 1;
  int pageSize = 50;
};

struct StreamUrls {
  QUrl primary;
  QUrl backup;
  QUrl primarySecure;
  QUrl backupSecure;
  QUrl primaryRtsp;
  QUrl backupRtsp;
  QUrl primarySrt;
  QUrl backupSrt;
  QUrl playback;
  QUrl hlsPlayback;
};

struct StreamKeys {
  std::string rtmp;
  std::string primarySrt;
  std::string backupSrt;
  std::string username;
  std::string password;
};

struct StreamConfiguration {
  StreamUrls urls;
  StreamKeys keys;
};

}  // namespace kaltura_live::api
