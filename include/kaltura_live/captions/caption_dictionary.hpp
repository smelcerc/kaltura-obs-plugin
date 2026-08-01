#pragma once

#include <string>
#include <vector>

namespace kaltura_live::captions {

struct CaptionDictionaryEntry {
  std::string spokenForm;
  std::string preferredText;
};

[[nodiscard]] std::string applyCaptionDictionary(
  std::string text, const std::vector<CaptionDictionaryEntry> &dictionary);

}  // namespace kaltura_live::captions
