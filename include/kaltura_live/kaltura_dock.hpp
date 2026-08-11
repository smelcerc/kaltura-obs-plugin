#pragma once

#include "kaltura_live/settings_manager.hpp"
#include "kaltura_live/streaming_manager.hpp"
#include "kaltura_live/captions/cea608_caption_inserter.hpp"

#include <QPalette>
#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QCheckBox;
class QComboBox;
class QSpinBox;
class QPushButton;
class QToolButton;
class QNetworkAccessManager;
class QPlainTextEdit;
class QLineEdit;

namespace kaltura_live {

class KalturaDock : public QWidget {
public:
  using CaptionsToggleCallback = std::function<void(bool)>;
  using CaptionDelayCallback = std::function<void(int)>;
  using CaptionStyleCallback = std::function<void(CaptionStyle)>;
  using WhisperModelCallback = std::function<void(WhisperModel)>;
  using OutputControlCallback = std::function<void(bool)>;
  using OutputConfigCallback = std::function<void(OutputRole, const StreamOutputConfig &)>;
  using SettingsCallback = std::function<void()>;

  explicit KalturaDock(CaptionsToggleCallback captionsToggleCallback,
                       CaptionDelayCallback captionDelayCallback,
                       CaptionStyleCallback captionStyleCallback,
                       WhisperModelCallback whisperModelCallback,
                       OutputControlCallback primaryControlCallback,
                       OutputControlCallback backupControlCallback,
                       OutputConfigCallback outputConfigCallback,
                       SettingsCallback settingsCallback,
                       QWidget *parent = nullptr);
  void setTheme(Theme theme);
  void setProjectSettings(const PluginSettings &settings);
  void setStreamingHealth(const StreamingHealth &health);
  void setOutputConfigurations(const StreamOutputConfig &primary,
                               const StreamOutputConfig &backup);
  void setCaptionsEnabled(bool enabled);
  void setCaptionConfiguration(int delayMs, CaptionStyle style, WhisperModel model);
  void setCaptionsLocked(bool locked);
  void setCaptionStatus(const QString &status, bool error = false);
  void setCaptionHealth(const captions::CaptionHealth &health);

private:
  void loadEntryThumbnail(const QString &url);
  void populateCaptionSegments(const captions::CaptionHealth &health);
  struct OutputEditor {
    QCheckBox *enabled = nullptr;
    QComboBox *protocol = nullptr;
    QLineEdit *endpoint = nullptr;
    QLineEdit *key = nullptr;
    QLineEdit *username = nullptr;
    QLineEdit *password = nullptr;
    QLineEdit *host = nullptr;
    QSpinBox *port = nullptr;
    QComboBox *mode = nullptr;
    QSpinBox *latency = nullptr;
    QWidget *srtLatencyField = nullptr;
    QLineEdit *passphrase = nullptr;
    QComboBox *pbkeylen = nullptr;
    QLineEdit *streamId = nullptr;
    QSpinBox *timeout = nullptr;
    QSpinBox *packetSize = nullptr;
    QCheckBox *reconnect = nullptr;
    QSpinBox *reconnectDelay = nullptr;
    QSpinBox *reconnectRetries = nullptr;
    QWidget *rtmpFields = nullptr;
    QWidget *srtFields = nullptr;
    QPushButton *apply = nullptr;
    std::string name;
    StreamOutputConfig loadedConfig;
  };
  void updateProtocolFields(OutputEditor &editor);
  void populateOutputEditor(OutputEditor &editor, const StreamOutputConfig &config);
  [[nodiscard]] StreamOutputConfig outputEditorConfig(const OutputEditor &editor) const;

  QLabel *statusValue_ = nullptr;
  QLabel *thumbnailValue_ = nullptr;
  QLabel *entryNameValue_ = nullptr;
  QLabel *entryIdValue_ = nullptr;
  QLabel *entryDescriptionValue_ = nullptr;
  QLabel *primaryHealthValue_ = nullptr;
  QLabel *backupHealthValue_ = nullptr;
  QLabel *streamingTitle_ = nullptr;
  QPushButton *startPrimaryButton_ = nullptr;
  QPushButton *stopPrimaryButton_ = nullptr;
  QPushButton *startBackupButton_ = nullptr;
  QPushButton *stopBackupButton_ = nullptr;
  QPushButton *settingsButton_ = nullptr;
  QCheckBox *captionsToggle_ = nullptr;
  QSpinBox *captionDelay_ = nullptr;
  QComboBox *captionStyle_ = nullptr;
  QComboBox *whisperModel_ = nullptr;
  QLabel *captionStatus_ = nullptr;
  QLabel *captionHealth_ = nullptr;
  QToolButton *captionPreviewToggle_ = nullptr;
  QWidget *captionPreviewContent_ = nullptr;
  QPlainTextEdit *captionSegments_ = nullptr;
  QNetworkAccessManager *thumbnailNetwork_ = nullptr;
  QString currentThumbnailUrl_;
  quint64 thumbnailRequestId_ = 0;
  int programDelaySeconds_ = 0;
  uint64_t latestCaptionSequence_ = 0;
  uint64_t captionPreviewStartSequence_ = 0;
  OutputEditor primaryEditor_;
  OutputEditor backupEditor_;
  QPalette systemPalette_;
};

}  // namespace kaltura_live
