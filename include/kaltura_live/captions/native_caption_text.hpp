#pragma once

#include "kaltura_live/settings_manager.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

namespace kaltura_live::captions {

struct NativeCaptionText {
  std::string text;
};

inline NativeCaptionText prepareNativeCaptionText(const std::string &text,
                                                  CaptionPlacement placement)
{
  NativeCaptionText result;
  const int visibleRows = std::clamp(
    1 + static_cast<int>(std::count(text.begin(), text.end(), '\n')), 1, 2);
  if (placement == CaptionPlacement::Bottom) {
    constexpr int kCea608Rows = 15;
    const int leadingRows = kCea608Rows - visibleRows;
    result.text.reserve(static_cast<size_t>(leadingRows) * 4 + text.size());
    for (int row = 0; row < leadingRows; ++row) {
      result.text += "\xE2\x80\x8B\n";
    }
  }

  result.text += text;
  return result;
}

}  // namespace kaltura_live::captions
