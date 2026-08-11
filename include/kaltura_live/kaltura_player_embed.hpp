#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kaltura_live {

inline constexpr std::int64_t kKalturaPlayerUiConfId = 58'233'132;

[[nodiscard]] std::string buildKalturaPlayerHtml(
  std::int64_t partnerId, std::string_view entryId, std::string_view session);

}  // namespace kaltura_live
