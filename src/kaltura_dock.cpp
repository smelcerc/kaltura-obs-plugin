#include "kaltura_live/kaltura_dock.hpp"

#include <QColor>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QFont>
#include <QLabel>
#include <QHBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QUrl>

#include <algorithm>

namespace kaltura_live {

namespace {
QString statusName(int status)
{
  switch (status) {
  case 0: return "Error importing";
  case 1: return "Importing";
  case 2: return "Ready";
  case 3: return "Deleted";
  case 4: return "Pending";
  case 5: return "Moderating";
  case 6: return "Blocked";
  case 7: return "No content";
  default: return QString("Unknown (%1)").arg(status);
  }
}

QString entryStatusColor(int status)
{
  switch (status) {
  case 2: return "#18b66a";
  case 1:
  case 4:
  case 5: return "#e5a50a";
  case 3:
  case 7: return "#8b919e";
  default: return "#ed4245";
  }
}

QString healthStateName(OutputHealthState state)
{
  switch (state) {
  case OutputHealthState::Disabled: return "Disabled";
  case OutputHealthState::Idle: return "Ready";
  case OutputHealthState::Starting: return "Starting";
  case OutputHealthState::Live: return "Live";
  case OutputHealthState::Reconnecting: return "Reconnecting";
  case OutputHealthState::Stopping: return "Stopping";
  case OutputHealthState::Error: return "Error";
  }
  return "Unknown";
}

QString healthColor(const OutputHealth &health)
{
  switch (health.state) {
  case OutputHealthState::Live: {
    const double droppedRatio = health.totalFrames > 0
      ? static_cast<double>(health.droppedFrames) / health.totalFrames
      : 0.0;
    if (health.congestion >= 0.8F || droppedRatio >= 0.05) {
      return "#ed4245";
    }
    if (health.congestion >= 0.25F || droppedRatio >= 0.01) {
      return "#e5a50a";
    }
    return "#18b66a";
  }
  case OutputHealthState::Reconnecting:
  case OutputHealthState::Starting:
  case OutputHealthState::Stopping: return "#e5a50a";
  case OutputHealthState::Disabled:
  case OutputHealthState::Idle:
  case OutputHealthState::Error: return "#ed4245";
  }
  return "#ed4245";
}

QString formatBitrate(uint64_t bitrateKbps)
{
  if (bitrateKbps >= 1000) {
    return QString::number(bitrateKbps / 1000.0, 'f', 2) + " Mbps";
  }
  return QString::number(bitrateKbps) + " Kbps";
}

QString formatHealth(const QString &name, const OutputHealth &health)
{
  const QString state = healthStateName(health.state);
  const QString color = healthColor(health);
  const QString connected = health.connected ? "Yes" : "No";
  const QString latency = health.connected ? QString::number(health.latencyMs) + " ms" : "—";
  QString error;
  if (!health.lastError.empty()) {
    error = "<br><span style='color:#ed4245'>" +
            QString::fromUtf8(health.lastError).toHtmlEscaped() + "</span>";
  }
  return QString(
    "<div style='font-size:14px'><b>%1</b>&nbsp;&nbsp;"
    "<span style='color:%2'>&#9679;&nbsp;%3</span></div>"
    "<table cellspacing='3' cellpadding='0'>"
    "<tr><td>Connected</td><td>&nbsp;&nbsp;<b>%4</b></td></tr>"
    "<tr><td>Bitrate</td><td>&nbsp;&nbsp;<b>%5</b></td></tr>"
    "<tr><td>Dropped Frames</td><td>&nbsp;&nbsp;<b>%6</b></td></tr>"
    "<tr><td>Reconnect Attempts</td><td>&nbsp;&nbsp;<b>%7</b></td></tr>"
    "<tr><td>Latency</td><td>&nbsp;&nbsp;<b>%8</b></td></tr>"
    "</table>%9")
    .arg(name, color, state, connected, formatBitrate(health.bitrateKbps))
    .arg(health.droppedFrames)
    .arg(health.reconnectAttempts)
    .arg(latency, error);
}

QString captionHealthStateName(captions::CaptionHealthState state)
{
  switch (state) {
  case captions::CaptionHealthState::Disabled: return "Disabled";
  case captions::CaptionHealthState::Ready: return "Ready";
  case captions::CaptionHealthState::Starting: return "Starting";
  case captions::CaptionHealthState::Healthy: return "Healthy";
  case captions::CaptionHealthState::Degraded: return "Degraded";
  case captions::CaptionHealthState::Error: return "Error";
  }
  return "Unknown";
}

QString captionHealthColor(captions::CaptionHealthState state)
{
  if (state == captions::CaptionHealthState::Healthy ||
      state == captions::CaptionHealthState::Ready) {
    return "#18b66a";
  }
  if (state == captions::CaptionHealthState::Starting ||
      state == captions::CaptionHealthState::Degraded) {
    return "#e5a50a";
  }
  return "#ed4245";
}
}

KalturaDock::KalturaDock(CaptionsToggleCallback captionsToggleCallback,
                         CaptionDelayCallback captionDelayCallback,
                         CaptionStyleCallback captionStyleCallback,
                         WhisperModelCallback whisperModelCallback,
                         OutputControlCallback primaryControlCallback,
                         OutputControlCallback backupControlCallback,
                         SettingsCallback settingsCallback,
                         QWidget *parent)
  : QWidget(parent)
{
  systemPalette_ = palette();
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  auto *scrollArea = new QScrollArea(this);
  scrollArea->setWidgetResizable(true);
  scrollArea->setFrameShape(QFrame::NoFrame);
  scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  auto *content = new QWidget(scrollArea);
  content->setMinimumWidth(280);
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);
  scrollArea->setWidget(content);
  rootLayout->addWidget(scrollArea);

  auto *title = new QLabel("Kaltura Live Plugin", this);
  QFont titleFont = title->font();
  titleFont.setPointSize(titleFont.pointSize() + 1);
  titleFont.setBold(true);
  title->setFont(titleFont);
  settingsButton_ = new QPushButton("Settings…", this);
  settingsButton_->setToolTip("Open Kaltura Live Settings");

  statusValue_ = new QLabel("Not Connected", this);
  statusValue_->setTextFormat(Qt::RichText);
  entryNameValue_ = new QLabel("No live entry selected", this);
  entryIdValue_ = new QLabel(this);
  entryDescriptionValue_ = new QLabel(this);
  thumbnailValue_ = new QLabel(this);
  thumbnailValue_->setFixedSize(160, 90);
  thumbnailValue_->setAlignment(Qt::AlignCenter);
  thumbnailValue_->setText("No thumbnail");
  thumbnailValue_->setStyleSheet(
    "QLabel { border: 1px solid palette(mid); border-radius: 6px; "
    "background-color: palette(alternate-base); color: palette(mid); }");
  thumbnailNetwork_ = new QNetworkAccessManager(this);
  primaryHealthValue_ = new QLabel(this);
  backupHealthValue_ = new QLabel(this);
  startPrimaryButton_ = new QPushButton("Start Primary", this);
  stopPrimaryButton_ = new QPushButton("Stop Primary", this);
  startBackupButton_ = new QPushButton("Start Backup", this);
  stopBackupButton_ = new QPushButton("Stop Backup", this);
  captionsToggle_ = new QCheckBox("Enable live captions", this);
  captionDelay_ = new QSpinBox(this);
  captionDelay_->setRange(0, 10);
  captionDelay_->setSingleStep(1);
  captionDelay_->setSuffix(" s");
  captionDelay_->setToolTip(
    "Delays outgoing program video and audio so locally generated captions can catch up. "
    "This takes effect the next time an output starts.");
  captionStyle_ = new QComboBox(this);
  captionStyle_->addItem("Standard · 2 lines", static_cast<int>(CaptionStyle::Standard));
  captionStyle_->addItem("Compact · 1 line", static_cast<int>(CaptionStyle::Compact));
  captionStyle_->addItem("Uppercase · 2 lines", static_cast<int>(CaptionStyle::Uppercase));
  captionStyle_->setToolTip(
    "CEA-608 supports a limited presentation model. Styles are broadcast-safe text layouts.");
  whisperModel_ = new QComboBox(this);
  whisperModel_->addItem("Tiny · Faster", static_cast<int>(WhisperModel::Tiny));
  whisperModel_->addItem("Base · More accurate", static_cast<int>(WhisperModel::Base));
  whisperModel_->setToolTip(
    "Tiny uses less CPU. Base is more accurate but may add latency on Intel Macs.");
  captionsToggle_->setToolTip(
    "Captions the final OBS program-audio mix locally. Audio never leaves this Mac.");
  captionStatus_ = new QLabel("Local Whisper · Disabled", this);
  captionHealth_ = new QLabel(this);
  captionHealth_->setWordWrap(true);
  captionSegments_ = new QPlainTextEdit(this);
  captionSegments_->setReadOnly(true);
  captionSegments_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  captionSegments_->setMinimumHeight(120);
  captionSegments_->setMaximumHeight(220);
  captionSegments_->setPlaceholderText(
    "No CEA-608 segments have been queued to an active OBS output yet.");
  captionSegments_->setToolTip(
    "Shows formatted CEA-608 segments handed to active OBS outputs. OBS does not "
    "provide an acknowledgement from the remote RTMP server.");
  captionSegments_->document()->setMaximumBlockCount(1'000);
  entryNameValue_->setWordWrap(true);
  QFont entryNameFont = entryNameValue_->font();
  entryNameFont.setPointSize(entryNameFont.pointSize() + 2);
  entryNameFont.setBold(true);
  entryNameValue_->setFont(entryNameFont);
  entryIdValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  entryIdValue_->setStyleSheet("color: palette(window-text); font-size: 12px;");
  entryDescriptionValue_->setWordWrap(true);
  entryDescriptionValue_->setStyleSheet("color: palette(window-text);");
  primaryHealthValue_->setWordWrap(true);
  backupHealthValue_->setWordWrap(true);
  const QString dashboardCardStyle =
    "QLabel { border: 1px solid palette(mid); border-radius: 8px; padding: 10px; "
    "background-color: palette(base); }";
  primaryHealthValue_->setStyleSheet(dashboardCardStyle);
  backupHealthValue_->setStyleSheet(dashboardCardStyle);

  auto *dockHeader = new QHBoxLayout();
  dockHeader->addWidget(title);
  dockHeader->addStretch(1);
  dockHeader->addWidget(settingsButton_);
  layout->addLayout(dockHeader);
  layout->addSpacing(6);
  auto *entryCard = new QFrame(this);
  entryCard->setObjectName("kalturaEntryCard");
  entryCard->setStyleSheet(
    "QFrame#kalturaEntryCard { border: 1px solid palette(mid); border-radius: 10px; "
    "background-color: palette(base); }");
  auto *entryCardLayout = new QVBoxLayout(entryCard);
  entryCardLayout->setContentsMargins(12, 10, 12, 12);
  entryCardLayout->setSpacing(8);
  auto *entryCardHeader = new QHBoxLayout();
  auto *entryCardTitle = new QLabel("Current Live Entry", entryCard);
  QFont entryCardTitleFont = entryCardTitle->font();
  entryCardTitleFont.setBold(true);
  entryCardTitle->setFont(entryCardTitleFont);
  entryCardHeader->addWidget(entryCardTitle);
  entryCardHeader->addStretch(1);
  entryCardHeader->addWidget(statusValue_);
  auto *entryContent = new QHBoxLayout();
  entryContent->setSpacing(12);
  entryContent->addWidget(thumbnailValue_, 0, Qt::AlignTop);
  auto *entryDetails = new QVBoxLayout();
  entryDetails->setSpacing(4);
  entryDetails->addWidget(entryNameValue_);
  entryDetails->addWidget(entryIdValue_);
  entryDetails->addWidget(entryDescriptionValue_);
  entryDetails->addStretch(1);
  entryContent->addLayout(entryDetails, 1);
  entryCardLayout->addLayout(entryCardHeader);
  entryCardLayout->addLayout(entryContent);
  layout->addWidget(entryCard);
  layout->addSpacing(12);

  streamingTitle_ = new QLabel("Real-time Streaming", this);
  QFont healthTitleFont = streamingTitle_->font();
  healthTitleFont.setBold(true);
  streamingTitle_->setFont(healthTitleFont);
  layout->addWidget(streamingTitle_);
  layout->addWidget(primaryHealthValue_);
  auto *primaryControls = new QHBoxLayout();
  primaryControls->addWidget(startPrimaryButton_);
  primaryControls->addWidget(stopPrimaryButton_);
  layout->addLayout(primaryControls);
  layout->addWidget(backupHealthValue_);
  auto *backupControls = new QHBoxLayout();
  backupControls->addWidget(startBackupButton_);
  backupControls->addWidget(stopBackupButton_);
  layout->addLayout(backupControls);
  layout->addSpacing(12);

  auto *captionsTitle = new QLabel("Live Captions", this);
  QFont captionsTitleFont = captionsTitle->font();
  captionsTitleFont.setBold(true);
  captionsTitle->setFont(captionsTitleFont);
  layout->addWidget(captionsTitle);
  layout->addWidget(captionsToggle_);
  auto *captionForm = new QFormLayout();
  captionForm->addRow("Program delay", captionDelay_);
  captionForm->addRow("Model", whisperModel_);
  captionForm->addRow("Style", captionStyle_);
  layout->addLayout(captionForm);
  layout->addWidget(captionStatus_);
  layout->addWidget(captionHealth_);
  auto *captionSegmentsTitle = new QLabel("CEA-608 output monitor", this);
  QFont captionSegmentsTitleFont = captionSegmentsTitle->font();
  captionSegmentsTitleFont.setBold(true);
  captionSegmentsTitle->setFont(captionSegmentsTitleFont);
  auto *copyCaptionSegmentsButton = new QPushButton("Copy All", this);
  copyCaptionSegmentsButton->setToolTip(
    "Copies the complete retained CEA-608 output history to the clipboard.");
  auto *captionSegmentsHeader = new QHBoxLayout();
  captionSegmentsHeader->addWidget(captionSegmentsTitle);
  captionSegmentsHeader->addStretch(1);
  captionSegmentsHeader->addWidget(copyCaptionSegmentsButton);
  auto *captionSegmentsHelp = new QLabel(
    "Actual broadcast-safe segments handed to each active OBS output. "
    "This confirms OBS insertion, not receipt by the RTMP server.", this);
  captionSegmentsHelp->setWordWrap(true);
  layout->addLayout(captionSegmentsHeader);
  layout->addWidget(captionSegmentsHelp);
  layout->addWidget(captionSegments_);
  layout->addStretch(1);

  setStreamingHealth({});
  setCaptionHealth({});
  QObject::connect(captionsToggle_, &QCheckBox::toggled,
                   [callback = std::move(captionsToggleCallback)](bool enabled) {
                     if (callback) {
                       callback(enabled);
                     }
                   });
  QObject::connect(captionDelay_, &QSpinBox::valueChanged,
                   [callback = std::move(captionDelayCallback)](int delaySeconds) {
                     if (callback) {
                       callback(delaySeconds * 1000);
                     }
                   });
  QObject::connect(captionStyle_, &QComboBox::currentIndexChanged,
                   [this, callback = std::move(captionStyleCallback)](int) {
                     if (callback) {
                       callback(static_cast<CaptionStyle>(captionStyle_->currentData().toInt()));
                     }
                   });
  QObject::connect(whisperModel_, &QComboBox::currentIndexChanged,
                   [this, callback = std::move(whisperModelCallback)](int) {
                     if (callback) {
                       callback(static_cast<WhisperModel>(whisperModel_->currentData().toInt()));
                     }
                   });
  QObject::connect(startPrimaryButton_, &QPushButton::clicked,
                   [callback = primaryControlCallback]() {
                     if (callback) callback(true);
                   });
  QObject::connect(stopPrimaryButton_, &QPushButton::clicked,
                   [callback = std::move(primaryControlCallback)]() {
                     if (callback) callback(false);
                   });
  QObject::connect(startBackupButton_, &QPushButton::clicked,
                   [callback = backupControlCallback]() {
                     if (callback) callback(true);
                   });
  QObject::connect(stopBackupButton_, &QPushButton::clicked,
                   [callback = std::move(backupControlCallback)]() {
                     if (callback) callback(false);
                   });
  QObject::connect(settingsButton_, &QPushButton::clicked,
                   [callback = std::move(settingsCallback)]() {
                     if (callback) callback();
                   });
  QObject::connect(copyCaptionSegmentsButton, &QPushButton::clicked, [this]() {
    QApplication::clipboard()->setText(captionSegments_->toPlainText());
  });
}

void KalturaDock::setCaptionsEnabled(bool enabled)
{
  const QSignalBlocker blocker(captionsToggle_);
  captionsToggle_->setChecked(enabled);
  if (!captionsToggle_->isEnabled()) {
    return;
  }
  setCaptionStatus(enabled ? "Local Whisper · Ready" : "Local Whisper · Disabled");
}

void KalturaDock::setCaptionConfiguration(int delayMs, CaptionStyle style, WhisperModel model)
{
  const QSignalBlocker delayBlocker(captionDelay_);
  const QSignalBlocker styleBlocker(captionStyle_);
  const QSignalBlocker modelBlocker(whisperModel_);
  programDelaySeconds_ = std::clamp((delayMs + 999) / 1000, 0, 10);
  captionDelay_->setValue(programDelaySeconds_);
  const int index = captionStyle_->findData(static_cast<int>(style));
  captionStyle_->setCurrentIndex(index >= 0 ? index : 0);
  const int modelIndex = whisperModel_->findData(static_cast<int>(model));
  whisperModel_->setCurrentIndex(modelIndex >= 0 ? modelIndex : 0);
}

void KalturaDock::setCaptionsLocked(bool locked)
{
  captionsToggle_->setEnabled(!locked);
  captionDelay_->setEnabled(!locked);
  captionStyle_->setEnabled(!locked);
  whisperModel_->setEnabled(!locked);
  captionsToggle_->setToolTip(
    locked ? "Caption selection is locked until streaming stops."
           : "Captions the final OBS program-audio mix locally. Audio never leaves this Mac.");
}

void KalturaDock::setCaptionHealth(const captions::CaptionHealth &health)
{
  QString error;
  if (!health.lastError.empty() && health.lastError != health.providerStatus) {
    error = "<br><span style='color:#ed4245'>" +
            QString::fromUtf8(health.lastError).toHtmlEscaped() + "</span>";
  }
  captionHealth_->setText(
    QString("<span style='color:%1'>&#9679;&nbsp;<b>%2</b></span>"
            " &nbsp;Queued: %3 &nbsp;Inserted: %4 &nbsp;Dropped: %5"
            "<br>Program delay: %6 s &nbsp;Last delivery: %7 output(s)%8")
      .arg(captionHealthColor(health.state), captionHealthStateName(health.state))
      .arg(health.queued)
      .arg(health.inserted)
      .arg(health.dropped)
      .arg(programDelaySeconds_)
      .arg(health.lastOutputCount)
      .arg(error));

  QStringList segments;
  for (auto segment = health.recentSegments.crbegin();
       segment != health.recentSegments.crend(); ++segment) {
    QString destinations;
    if (segment->primaryQueued) {
      destinations = "Primary";
    }
    if (segment->backupQueued) {
      destinations += destinations.isEmpty() ? "Backup" : " + Backup";
    }
    if (destinations.isEmpty() && segment->outputsQueued > 0) {
      destinations = QString("%1 OBS output(s)").arg(segment->outputsQueued);
    }
    const QString result = segment->delivered
      ? segment->nativeTimed
          ? QString("TIMED → %1 · OBS native video-frame insertion").arg(destinations)
          : QString("QUEUED → %1 · %2 packet(s)")
              .arg(destinations)
              .arg(segment->packetCount)
      : QString("FAILED · %1").arg(QString::fromUtf8(segment->error));
    QString text = QString::fromUtf8(segment->text);
    text.replace('\n', "\n    ");
    segments.append(QString("[%1] #%2 %3\n    %4")
                      .arg(QString::fromUtf8(segment->timestamp))
                      .arg(segment->sequence)
                      .arg(result, text));
  }
  const QString updatedText = segments.join("\n\n");
  const QString currentText = captionSegments_->toPlainText();
  if (updatedText == currentText) {
    return;
  }

  QScrollBar *scrollBar = captionSegments_->verticalScrollBar();
  const int previousMaximum = scrollBar->maximum();
  const int previousValue = scrollBar->value();
  const bool followingNewest = previousValue == 0;
  const QTextCursor previousCursor = captionSegments_->textCursor();
  const bool hadSelection = previousCursor.hasSelection();
  const QString selectedText = previousCursor.selectedText();
  int selectionAnchor = previousCursor.anchor();
  int selectionPosition = previousCursor.position();
  if (hadSelection && !currentText.isEmpty() && updatedText.endsWith(currentText)) {
    const int insertedCharacters = updatedText.size() - currentText.size();
    selectionAnchor += insertedCharacters;
    selectionPosition += insertedCharacters;
  } else if (hadSelection) {
    const int relocatedSelection = updatedText.indexOf(selectedText);
    if (relocatedSelection >= 0) {
      selectionAnchor = relocatedSelection;
      selectionPosition = relocatedSelection + selectedText.size();
    }
  }

  captionSegments_->setPlainText(updatedText);
  if (hadSelection) {
    const int textSize = static_cast<int>(updatedText.size());
    QTextCursor restoredCursor(captionSegments_->document());
    restoredCursor.setPosition(std::clamp(selectionAnchor, 0, textSize));
    restoredCursor.setPosition(std::clamp(selectionPosition, 0, textSize),
                               QTextCursor::KeepAnchor);
    captionSegments_->setTextCursor(restoredCursor);
  }
  const int addedScrollRange = scrollBar->maximum() - previousMaximum;
  scrollBar->setValue(followingNewest ? 0 : previousValue + addedScrollRange);
}

void KalturaDock::setCaptionStatus(const QString &status, bool error)
{
  captionStatus_->setText(status.toHtmlEscaped());
  captionStatus_->setStyleSheet(error ? "color: #ed4245;" : "color: palette(text);");
}

void KalturaDock::setProjectSettings(const PluginSettings &settings)
{
  const bool primaryEnabled =
    settings.preferredEndpoint != StreamingEndpoint::Backup;
  const bool backupEnabled =
    settings.preferredEndpoint != StreamingEndpoint::Primary;
  startPrimaryButton_->setVisible(primaryEnabled);
  stopPrimaryButton_->setVisible(primaryEnabled);
  startBackupButton_->setVisible(backupEnabled);
  stopBackupButton_->setVisible(backupEnabled);
  if (settings.kalturaSession.empty()) {
    statusValue_->setText("<span style='color:#ed4245'>&#9679;&nbsp;<b>Not connected</b></span>");
  } else if (settings.selectedEntryId.empty()) {
    statusValue_->setText("<span style='color:#e5a50a'>&#9679;&nbsp;<b>No entry</b></span>");
  } else {
    statusValue_->setText(
      QString("<span style='color:%1'>&#9679;&nbsp;<b>%2</b></span>")
        .arg(entryStatusColor(settings.selectedEntryStatus),
             statusName(settings.selectedEntryStatus).toHtmlEscaped()));
  }
  if (settings.selectedEntryId.empty()) {
    entryNameValue_->setText("No live entry selected");
    entryIdValue_->setText("Choose an entry in Kaltura Live Settings");
    entryDescriptionValue_->setText(
      "The selected entry's thumbnail and broadcast details will appear here.");
    loadEntryThumbnail({});
    return;
  }
  const QString name = QString::fromUtf8(settings.selectedEntryName);
  entryNameValue_->setText(name.isEmpty() ? "Untitled live entry" : name);
  entryIdValue_->setText("Entry ID: " + QString::fromUtf8(settings.selectedEntryId));
  const QString description = QString::fromUtf8(settings.selectedEntryDescription).trimmed();
  entryDescriptionValue_->setText(
    description.isEmpty() ? "No description provided." : description);
  loadEntryThumbnail(QString::fromUtf8(settings.selectedEntryThumbnailUrl));
}

void KalturaDock::loadEntryThumbnail(const QString &url)
{
  const QString normalizedUrl = url.trimmed();
  if (normalizedUrl == currentThumbnailUrl_) {
    return;
  }
  currentThumbnailUrl_ = normalizedUrl;
  const quint64 requestId = ++thumbnailRequestId_;
  thumbnailValue_->setPixmap({});
  if (normalizedUrl.isEmpty()) {
    thumbnailValue_->setText("No thumbnail");
    return;
  }

  const QUrl thumbnailUrl(normalizedUrl);
  if (!thumbnailUrl.isValid() ||
      (thumbnailUrl.scheme() != "https" && thumbnailUrl.scheme() != "http")) {
    thumbnailValue_->setText("Invalid thumbnail");
    return;
  }
  thumbnailValue_->setText("Loading…");
  QNetworkRequest request(thumbnailUrl);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(10'000);
  request.setRawHeader("Accept", "image/*");
  QNetworkReply *reply = thumbnailNetwork_->get(request);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, requestId]() {
    const auto contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if (requestId != thumbnailRequestId_) {
      reply->deleteLater();
      return;
    }
    const QByteArray bytes = reply->readAll();
    QPixmap thumbnail;
    const bool validImage = reply->error() == QNetworkReply::NoError &&
                            contentLength <= 5'000'000 && bytes.size() <= 5'000'000 &&
                            thumbnail.loadFromData(bytes);
    if (validImage) {
      thumbnailValue_->setText({});
      thumbnailValue_->setPixmap(thumbnail.scaled(
        thumbnailValue_->size(), Qt::KeepAspectRatioByExpanding,
        Qt::SmoothTransformation));
    } else {
      thumbnailValue_->setPixmap({});
      thumbnailValue_->setText("Thumbnail unavailable");
    }
    reply->deleteLater();
  });
}

void KalturaDock::setStreamingHealth(const StreamingHealth &health)
{
  primaryHealthValue_->setText(formatHealth("Primary", health.primary));
  backupHealthValue_->setText(formatHealth("Backup", health.backup));
  const auto activeOrTransitioning = [](OutputHealthState state) {
    return state == OutputHealthState::Starting || state == OutputHealthState::Live ||
           state == OutputHealthState::Reconnecting || state == OutputHealthState::Stopping;
  };
  const bool primaryAvailable = health.primary.state != OutputHealthState::Disabled;
  primaryHealthValue_->setVisible(primaryAvailable);
  startPrimaryButton_->setVisible(primaryAvailable);
  stopPrimaryButton_->setVisible(primaryAvailable);
  const bool primaryActive = activeOrTransitioning(health.primary.state);
  startPrimaryButton_->setEnabled(primaryAvailable && !primaryActive);
  stopPrimaryButton_->setEnabled(primaryAvailable && primaryActive &&
                                 health.primary.state != OutputHealthState::Stopping);
  const bool backupAvailable = health.backup.state != OutputHealthState::Disabled;
  backupHealthValue_->setVisible(backupAvailable);
  startBackupButton_->setVisible(backupAvailable);
  stopBackupButton_->setVisible(backupAvailable);
  const bool backupActive = activeOrTransitioning(health.backup.state);
  startBackupButton_->setEnabled(backupAvailable && !backupActive);
  stopBackupButton_->setEnabled(backupAvailable && backupActive &&
                                health.backup.state != OutputHealthState::Stopping);
  streamingTitle_->setVisible(primaryAvailable || backupAvailable);
}

void KalturaDock::setTheme(Theme theme)
{
  if (theme == Theme::System) {
    setPalette(systemPalette_);
    setAutoFillBackground(false);
    return;
  }

  QPalette themedPalette = systemPalette_;
  if (theme == Theme::Dark) {
    themedPalette.setColor(QPalette::Window, QColor("#25262a"));
    themedPalette.setColor(QPalette::WindowText, QColor("#f1f3f5"));
  } else {
    themedPalette.setColor(QPalette::Window, QColor("#f5f6f8"));
    themedPalette.setColor(QPalette::WindowText, QColor("#202124"));
  }
  setPalette(themedPalette);
  setAutoFillBackground(true);
}

}  // namespace kaltura_live
