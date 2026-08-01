#include "kaltura_live/captions/cea608_caption_inserter.hpp"

#include <QDateTime>
#include <QRegularExpression>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <deque>
#include <utility>

namespace kaltura_live::captions {
namespace {
constexpr int kTickIntervalMs = 50;
constexpr int kMinimumCaptionGapMs = 250;
constexpr int kMaximumLatenessMs = 2'000;
constexpr size_t kMaximumQueuedCaptions = 128;
constexpr size_t kMaximumRecentSegments = 200;
constexpr int kColumnsPerLine = 32;

std::string textForLog(std::string text)
{
  std::replace(text.begin(), text.end(), '\n', '|');
  return text;
}

std::string deliveryTargets(const CaptionDeliveryResult &result)
{
  std::string targets;
  if (result.primaryQueued) {
    targets = "Primary";
  }
  if (result.backupQueued) {
    targets += targets.empty() ? "Backup" : "+Backup";
  }
  if (targets.empty() && result.outputsQueued > 0) {
    targets = std::to_string(result.outputsQueued) + " OBS output(s)";
  }
  return targets.empty() ? "none" : targets;
}

QString normalizedText(const std::string &text)
{
  QString value = QString::fromUtf8(text);
  value.replace(QChar(0x2018), '\'');
  value.replace(QChar(0x2019), '\'');
  value.replace(QChar(0x201c), '"');
  value.replace(QChar(0x201d), '"');
  value.replace(QChar(0x2013), '-');
  value.replace(QChar(0x2014), '-');
  value.replace(QChar(0x2026), "...");
  value.replace(QRegularExpression("\\s+"), " ");
  return value.trimmed();
}

QStringList wrapWords(const QString &value)
{
  QStringList lines;
  QString line;
  const QStringList words = value.split(' ', Qt::SkipEmptyParts);
  for (QString word : words) {
    while (word.size() > kColumnsPerLine) {
      if (!line.isEmpty()) {
        lines.push_back(line);
        line.clear();
      }
      lines.push_back(word.left(kColumnsPerLine));
      word.remove(0, kColumnsPerLine);
    }
    if (word.isEmpty()) {
      continue;
    }
    const QString candidate = line.isEmpty() ? word : line + ' ' + word;
    if (candidate.size() <= kColumnsPerLine) {
      line = candidate;
    } else {
      lines.push_back(line);
      line = word;
    }
  }
  if (!line.isEmpty()) {
    lines.push_back(line);
  }
  return lines;
}

double displayDurationFor(const std::string &text)
{
  const double readingSeconds = static_cast<double>(text.size()) / 15.0;
  return std::clamp(readingSeconds, 2.0, 6.0);
}
}

class Cea608CaptionInserter::Impl {
public:
  struct PendingCaption {
    uint64_t sequence = 0;
    std::string text;
    std::chrono::steady_clock::time_point dueAt;
  };

  Impl(DeliveryCallback callback, QObject *parent)
    : deliveryCallback(std::move(callback)), timer(new QTimer(parent))
  {
    timer->setInterval(kTickIntervalMs);
    timer->setTimerType(Qt::PreciseTimer);
    QObject::connect(timer, &QTimer::timeout, [this]() { tick(); });
  }

  ~Impl() { timer->stop(); }

  void log(bool warning, std::string message) const
  {
    if (diagnosticCallback) {
      diagnosticCallback(warning, std::move(message));
    }
  }

  void recordResult(const PendingCaption &caption,
                    const CaptionDeliveryResult &result, bool delivered)
  {
    CaptionSegment segment;
    segment.sequence = caption.sequence;
    segment.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toStdString();
    segment.text = caption.text;
    segment.outputsQueued = result.outputsQueued;
    segment.primaryQueued = result.primaryQueued;
    segment.backupQueued = result.backupQueued;
    segment.packetCount = result.packetCount;
    segment.nativeTimed = result.nativeTimed;
    segment.delivered = delivered;
    segment.error = result.error;
    currentHealth.recentSegments.push_back(std::move(segment));
    if (currentHealth.recentSegments.size() > kMaximumRecentSegments) {
      currentHealth.recentSegments.erase(currentHealth.recentSegments.begin());
    }
  }

  void tick()
  {
    if (!active || queue.empty()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (queue.front().dueAt > now ||
        (lastDelivery.time_since_epoch().count() != 0 &&
         now - lastDelivery < std::chrono::milliseconds(kMinimumCaptionGapMs))) {
      return;
    }

    CaptionDeliveryResult result;
    if (deliveryCallback) {
      result = deliveryCallback(queue.front().text,
                                displayDurationFor(queue.front().text), placement, alignment);
    } else {
      result.error = "The CEA-608 output delivery callback is unavailable.";
    }
    if (result.succeeded()) {
      recordResult(queue.front(), result, true);
      ++currentHealth.inserted;
      currentHealth.lastOutputCount = result.outputsQueued;
      currentHealth.lastError.clear();
      currentHealth.state = CaptionHealthState::Healthy;
      queue.pop_front();
      lastDelivery = now;
      log(
        false,
        "CEA-608 segment #" +
          std::to_string(currentHealth.recentSegments.back().sequence) +
          " handed to " + deliveryTargets(result) + " (" +
          (result.nativeTimed ? "OBS native frame timing" :
                                std::to_string(result.packetCount) + " packets") + "): \"" +
          textForLog(currentHealth.recentSegments.back().text) + "\"");
    } else if (now - queue.front().dueAt > std::chrono::milliseconds(kMaximumLatenessMs)) {
      recordResult(queue.front(), result, false);
      ++currentHealth.dropped;
      currentHealth.lastError = result.error.empty()
                                  ? "Caption delivery failed for an unknown reason."
                                  : result.error;
      currentHealth.state = CaptionHealthState::Error;
      log(
        true,
        "CEA-608 segment #" + std::to_string(queue.front().sequence) +
          " was not handed to an OBS output: " + currentHealth.lastError);
      queue.pop_front();
    } else {
      currentHealth.lastError = result.error;
      currentHealth.state = CaptionHealthState::Degraded;
    }
    currentHealth.queued = queue.size();
  }

  DeliveryCallback deliveryCallback;
  DiagnosticCallback diagnosticCallback;
  QTimer *timer = nullptr;
  std::deque<PendingCaption> queue;
  CaptionHealth currentHealth;
  CaptionStyle style = CaptionStyle::Standard;
  CaptionPlacement placement = CaptionPlacement::Bottom;
  CaptionAlignment alignment = CaptionAlignment::Center;
  bool enabled = false;
  bool active = false;
  uint64_t nextSequence = 1;
  std::chrono::steady_clock::time_point lastDelivery{};
};

Cea608CaptionInserter::Cea608CaptionInserter(DeliveryCallback callback, QObject *parent)
  : QObject(parent), impl_(std::make_unique<Impl>(std::move(callback), this))
{
}

Cea608CaptionInserter::~Cea608CaptionInserter() = default;

void Cea608CaptionInserter::configure(bool enabled, int delayMs, CaptionStyle style,
                                     CaptionPlacement placement,
                                     CaptionAlignment alignment)
{
  impl_->enabled = enabled;
  impl_->style = style;
  impl_->placement = placement;
  impl_->alignment = alignment;
  impl_->currentHealth.delayMs = std::clamp(delayMs, 0, 10'000);
  if (!impl_->active) {
    impl_->currentHealth.state = enabled ? CaptionHealthState::Ready
                                        : CaptionHealthState::Disabled;
  }
}

void Cea608CaptionInserter::start()
{
  impl_->queue.clear();
  impl_->lastDelivery = {};
  impl_->active = impl_->enabled;
  impl_->currentHealth.received = 0;
  impl_->currentHealth.inserted = 0;
  impl_->currentHealth.dropped = 0;
  impl_->currentHealth.queued = 0;
  impl_->currentHealth.lastOutputCount = 0;
  impl_->currentHealth.lastError.clear();
  impl_->currentHealth.recentSegments.clear();
  impl_->nextSequence = 1;
  impl_->currentHealth.state = impl_->enabled ? CaptionHealthState::Starting
                                             : CaptionHealthState::Disabled;
  if (impl_->active) {
    impl_->timer->start();
  }
}

void Cea608CaptionInserter::stop()
{
  impl_->timer->stop();
  impl_->active = false;
  if (!impl_->queue.empty()) {
    impl_->currentHealth.dropped += impl_->queue.size();
    impl_->currentHealth.lastError =
      "Streaming stopped before queued captions could be inserted.";
  }
  impl_->queue.clear();
  impl_->currentHealth.queued = 0;
  impl_->currentHealth.state = impl_->enabled ? CaptionHealthState::Ready
                                             : CaptionHealthState::Disabled;
}

bool Cea608CaptionInserter::submit(std::string text)
{
  if (!impl_->active) {
    return false;
  }
  ++impl_->currentHealth.received;
  std::vector<std::string> segments = formatSegments(text, impl_->style);
  if (segments.empty()) {
    ++impl_->currentHealth.dropped;
    impl_->currentHealth.lastError = "The caption contained no CEA-608-safe text.";
    impl_->currentHealth.state = CaptionHealthState::Degraded;
    return false;
  }
  const auto dueAt = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(impl_->currentHealth.delayMs);
  for (std::string &segment : segments) {
    if (impl_->queue.size() >= kMaximumQueuedCaptions) {
      impl_->queue.pop_front();
      ++impl_->currentHealth.dropped;
      impl_->currentHealth.lastError = "Caption queue overflow; the oldest caption was dropped.";
      impl_->currentHealth.state = CaptionHealthState::Degraded;
    }
    const uint64_t sequence = impl_->nextSequence++;
    impl_->queue.push_back({sequence, std::move(segment), dueAt});
    impl_->log(
      false,
      "CEA-608 segment #" + std::to_string(sequence) + " queued for insertion after " +
        std::to_string(impl_->currentHealth.delayMs) + " ms: \"" +
        textForLog(impl_->queue.back().text) + "\"");
  }
  impl_->currentHealth.queued = impl_->queue.size();
  return true;
}

void Cea608CaptionInserter::setDiagnosticCallback(DiagnosticCallback callback)
{
  impl_->diagnosticCallback = std::move(callback);
}

void Cea608CaptionInserter::setProviderStatus(std::string status, bool error)
{
  impl_->currentHealth.providerStatus = std::move(status);
  if (error) {
    impl_->currentHealth.lastError = impl_->currentHealth.providerStatus;
    impl_->currentHealth.state = CaptionHealthState::Error;
  } else if (impl_->active && impl_->currentHealth.state != CaptionHealthState::Healthy) {
    impl_->currentHealth.state = CaptionHealthState::Starting;
  }
}

CaptionHealth Cea608CaptionInserter::health() const
{
  CaptionHealth result = impl_->currentHealth;
  result.queued = impl_->queue.size();
  return result;
}

std::string Cea608CaptionInserter::formatText(const std::string &text, CaptionStyle style)
{
  std::vector<std::string> segments = formatSegments(text, style);
  return segments.empty() ? std::string{} : std::move(segments.front());
}

std::vector<std::string> Cea608CaptionInserter::formatSegments(
  const std::string &text, CaptionStyle style)
{
  QString value = normalizedText(text);
  if (style == CaptionStyle::Uppercase) {
    value = value.toUpper();
  }
  const int maximumLines = style == CaptionStyle::Compact ? 1 : 2;
  const QStringList lines = wrapWords(value);
  if (lines.isEmpty()) {
    return {};
  }
  std::vector<std::string> segments;
  segments.reserve(static_cast<size_t>((lines.size() + maximumLines - 1) / maximumLines));
  for (qsizetype offset = 0; offset < lines.size(); offset += maximumLines) {
    QStringList screen;
    for (int line = 0; line < maximumLines && offset + line < lines.size(); ++line) {
      screen.push_back(lines.at(offset + line));
    }
    segments.push_back(screen.join('\n').toUtf8().toStdString());
  }
  return segments;
}

}  // namespace kaltura_live::captions
