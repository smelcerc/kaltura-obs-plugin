#include "kaltura_live/captions/cea608_caption_inserter.hpp"
#include "kaltura_live/captions/native_caption_text.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void expect(bool condition, const char *message)
{
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
}

void spinFor(int milliseconds)
{
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

int main(int argc, char **argv)
{
  QCoreApplication application(argc, argv);
  using kaltura_live::CaptionStyle;
  using kaltura_live::captions::Cea608CaptionInserter;

  expect(Cea608CaptionInserter::formatText("  hello   world  ", CaptionStyle::Standard) ==
           "hello world",
         "Whitespace must be normalized");
  expect(Cea608CaptionInserter::formatText("Mixed Case", CaptionStyle::Uppercase) ==
           "MIXED CASE",
         "Uppercase style must be applied");

  const auto shortCaption = kaltura_live::captions::prepareNativeCaptionText(
    "I", kaltura_live::CaptionPlacement::Bottom);
  expect(shortCaption.text.ends_with("I") &&
           std::count(shortCaption.text.begin(), shortCaption.text.end(), '\n') == 14,
         "Short bottom captions must retain placement without synthetic caption cells");
  const auto twoWordCaption = kaltura_live::captions::prepareNativeCaptionText(
    "Thank you", kaltura_live::CaptionPlacement::Bottom);
  expect(twoWordCaption.text.ends_with("Thank you"),
         "One- and two-word captions must remain visually unchanged");
  const std::string fullCaption = "This caption already fills at least one complete row";
  const auto normalCaption = kaltura_live::captions::prepareNativeCaptionText(
    fullCaption, kaltura_live::CaptionPlacement::Top);
  expect(normalCaption.text == fullCaption,
         "Normal top captions must not be padded or repositioned");

  const std::string compact = Cea608CaptionInserter::formatText(
    "This caption is intentionally longer than a single CEA-608 display row",
    CaptionStyle::Compact);
  expect(compact.size() <= 32 && compact.find('\n') == std::string::npos,
         "Compact captions must fit one 32-column row");

  const std::string standard = Cea608CaptionInserter::formatText(
    "This caption is intentionally long enough to exercise two broadcast safe rows",
    CaptionStyle::Standard);
  const size_t newline = standard.find('\n');
  expect(newline != std::string::npos && newline <= 32 &&
           standard.size() - newline - 1 <= 32,
         "Standard captions must fit two 32-column rows");

  const std::string completePhrase =
    "And more holiday dances and more activities we can do with our school and with our friends.";
  const std::vector<std::string> completeScreens =
    Cea608CaptionInserter::formatSegments(completePhrase, CaptionStyle::Standard);
  expect(completeScreens.size() == 2 &&
           completeScreens[0] == "And more holiday dances and more\nactivities we can do with our" &&
           completeScreens[1] == "school and with our friends.",
         "Long transcripts must continue onto another CEA-608 screen without truncation");

  int deliveries = 0;
  Cea608CaptionInserter inserter(
    [&deliveries](const std::string &, double duration,
                  kaltura_live::CaptionPlacement placement,
                  kaltura_live::CaptionAlignment alignment) {
      ++deliveries;
      expect(duration >= 2.0 && duration <= 6.0,
             "Display duration must remain within broadcast-safe limits");
      expect(placement == kaltura_live::CaptionPlacement::Bottom &&
               alignment == kaltura_live::CaptionAlignment::Center,
             "Configured caption placement and alignment must be delivered");
      kaltura_live::CaptionDeliveryResult result{1, {}};
      result.primaryQueued = true;
      result.packetCount = 12;
      result.nativeTimed = true;
      return result;
    });
  inserter.configure(true, 200, CaptionStyle::Standard,
                     kaltura_live::CaptionPlacement::Bottom,
                     kaltura_live::CaptionAlignment::Center);
  inserter.start();
  expect(inserter.submit("Delayed caption"),
         "An active inserter must accept a transcript phrase");
  spinFor(120);
  expect(deliveries == 0, "Caption delay must be honored");
  spinFor(180);
  expect(deliveries == 1, "Caption must be delivered after its configured delay");
  const auto health = inserter.health();
  expect(health.received == 1 && health.inserted == 1 && health.dropped == 0,
         "Successful insertion must update caption health");
  expect(health.recentSegments.size() == 1 &&
           health.recentSegments.front().text == "Delayed caption" &&
           health.recentSegments.front().delivered &&
           health.recentSegments.front().primaryQueued &&
           health.recentSegments.front().packetCount == 12 &&
           health.recentSegments.front().nativeTimed,
         "Successful insertion must retain observable CEA-608 delivery details");
  inserter.stop();
  return 0;
}
