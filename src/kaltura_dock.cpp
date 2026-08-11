#include "kaltura_live/kaltura_dock.hpp"
#include "kaltura_live/kaltura_player_embed.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QColor>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QFont>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUuid>
#include <QWheelEvent>

#include <algorithm>

namespace kaltura_live {

class BrowserPanelWidget : public QWidget {
public:
  virtual void setURL(const std::string &url) = 0;
  virtual void setStartupScript(const std::string &script) = 0;
  virtual void allowAllPopups(bool allow) = 0;
  virtual void closeBrowser() = 0;
  virtual void reloadPage() = 0;
  virtual bool zoomPage(int direction) = 0;
  virtual void executeJavaScript(const std::string &script) = 0;
};

struct BrowserPanelCookieManager;

struct BrowserPanelApi {
  virtual ~BrowserPanelApi() = default;
  virtual bool init_browser() = 0;
  virtual bool initialized() = 0;
  virtual bool wait_for_browser_init() = 0;
  virtual BrowserPanelWidget *create_widget(
    QWidget *parent, const std::string &url,
    BrowserPanelCookieManager *cookieManager = nullptr) = 0;
};

class KalturaPlayerPreview final : public QWidget {
public:
  explicit KalturaPlayerPreview(QWidget *parent = nullptr) : QWidget(parent)
  {
    setFixedSize(240, 135);
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);

    placeholder_ = new QLabel("Validate a KS and select a live entry", this);
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setWordWrap(true);
    placeholder_->setStyleSheet(
      "QLabel { border: 1px solid palette(mid); border-radius: 6px; "
      "background-color: palette(alternate-base); color: palette(mid); "
      "padding: 4px; }");
    layout_->addWidget(placeholder_);

    connect(&pageServer_, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket *socket = pageServer_.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
          QByteArray request = socket->property("kalturaRequest").toByteArray();
          request += socket->readAll();
          if (!request.contains("\r\n\r\n")) {
            socket->setProperty("kalturaRequest", request);
            return;
          }
          if (socket->property("kalturaResponded").toBool()) return;
          socket->setProperty("kalturaResponded", true);
          const bool found =
            request.startsWith("GET " + pagePath_.toUtf8() + " ");
          const QByteArray body = found ? playerHtml_ : QByteArray("Not found");
          QByteArray response = found ? "HTTP/1.1 200 OK\r\n"
                                      : "HTTP/1.1 404 Not Found\r\n";
          response += "Content-Type: text/html; charset=utf-8\r\n";
          response += "Cache-Control: no-store, no-cache, must-revalidate\r\n";
          response += "Content-Security-Policy: default-src 'self' https: data: blob:; "
                      "script-src 'self' 'unsafe-inline' 'unsafe-eval' https:; "
                      "style-src 'self' 'unsafe-inline' https:; "
                      "connect-src 'self' https: wss:; media-src https: blob:; "
                      "img-src https: data: blob:\r\n";
          response += "Content-Length: " + QByteArray::number(body.size()) +
                      "\r\nConnection: close\r\n\r\n";
          socket->write(response + body);
          socket->disconnectFromHost();
        });
      }
    });
  }

  ~KalturaPlayerPreview() override
  {
    destroyBrowser();
    delete browserApi_;
  }

  void setPlayer(std::int64_t partnerId, const std::string &entryId,
                 const std::string &session)
  {
    if (partnerId <= 0 || entryId.empty() || session.empty()) {
      playerHtml_.clear();
      pagePath_.clear();
      destroyBrowser();
      showPlaceholder("Validate a KS and select a live entry");
      return;
    }

    const QByteArray html = QByteArray::fromStdString(
      buildKalturaPlayerHtml(partnerId, entryId, session));
    if (browserWidget_ && html == playerHtml_) return;
    if (!pageServer_.isListening() &&
        !pageServer_.listen(QHostAddress::LocalHost, 0)) {
      destroyBrowser();
      showPlaceholder("Unable to start the local Kaltura player");
      return;
    }

    playerHtml_ = html;
    pagePath_ = "/player/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const std::string url =
      QString("http://127.0.0.1:%1%2")
        .arg(pageServer_.serverPort())
        .arg(pagePath_)
        .toStdString();
    if (!ensureBrowserApi()) {
      destroyBrowser();
      showPlaceholder("Kaltura player unavailable\nEnable OBS Browser Source");
      return;
    }

    destroyBrowser();
    browserWidget_ = browserApi_->create_widget(this, url, nullptr);
    if (!browserWidget_) {
      showPlaceholder("Unable to create the Kaltura player");
      return;
    }
    browserWidget_->setFixedSize(size());
    browserWidget_->setFocusPolicy(Qt::StrongFocus);
    layout_->addWidget(browserWidget_);
    placeholder_->hide();
    browserWidget_->show();
  }

  void refreshPlayer()
  {
    if (browserWidget_) browserWidget_->reloadPage();
  }

private:
  bool ensureBrowserApi()
  {
    if (browserApi_) return true;
    obs_module_t *module = obs_get_module("obs-browser");
    void *library = module ? obs_get_module_lib(module) : nullptr;
    if (!library) return false;

    using CreateBrowserApi = BrowserPanelApi *(*)();
    using BrowserApiVersion = int (*)();
    auto createApi = reinterpret_cast<CreateBrowserApi>(
      os_dlsym(library, "obs_browser_create_qcef"));
    auto apiVersion = reinterpret_cast<BrowserApiVersion>(
      os_dlsym(library, "obs_browser_qcef_version_export"));
    if (!createApi || !apiVersion || apiVersion() < 1) return false;
    browserApi_ = createApi();
    if (!browserApi_) return false;
    if (!browserApi_->initialized() && !browserApi_->init_browser()) {
      delete browserApi_;
      browserApi_ = nullptr;
      return false;
    }
    return true;
  }

  void destroyBrowser()
  {
    if (!browserWidget_) return;
    browserWidget_->closeBrowser();
    layout_->removeWidget(browserWidget_);
    delete browserWidget_;
    browserWidget_ = nullptr;
  }

  void showPlaceholder(const QString &message)
  {
    placeholder_->setText(message);
    placeholder_->show();
  }

  QVBoxLayout *layout_ = nullptr;
  QLabel *placeholder_ = nullptr;
  QTcpServer pageServer_;
  QByteArray playerHtml_;
  QString pagePath_;
  BrowserPanelApi *browserApi_ = nullptr;
  BrowserPanelWidget *browserWidget_ = nullptr;
};

namespace {
class ScrollSafeSpinBox final : public QSpinBox {
public:
  using QSpinBox::QSpinBox;

protected:
  void wheelEvent(QWheelEvent *event) override
  {
    // Let the dock's scroll area handle the wheel instead of changing a
    // connection setting while the user is navigating the page.
    event->ignore();
  }
};

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
    "<tr><td>Protocol</td><td>&nbsp;&nbsp;<b>%4</b></td></tr>"
    "<tr><td>Connected</td><td>&nbsp;&nbsp;<b>%5</b></td></tr>"
    "<tr><td>Bitrate</td><td>&nbsp;&nbsp;<b>%6</b></td></tr>"
    "<tr><td>Dropped Frames</td><td>&nbsp;&nbsp;<b>%7 / %8</b></td></tr>"
    "<tr><td>Reconnect Attempts</td><td>&nbsp;&nbsp;<b>%9</b></td></tr>"
    "<tr><td>Elapsed</td><td>&nbsp;&nbsp;<b>%10s</b></td></tr>"
    "<tr><td>Connect Time</td><td>&nbsp;&nbsp;<b>%11</b></td></tr>"
    "</table>%12")
    .arg(name, color, state, QString::fromLatin1(outputProtocolName(health.protocol)),
         connected, formatBitrate(health.bitrateKbps))
    .arg(health.droppedFrames)
    .arg(health.totalFrames)
    .arg(health.reconnectAttempts)
    .arg(health.elapsedSeconds)
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
                         OutputConfigCallback outputConfigCallback,
                         SettingsCallback settingsCallback,
                         QWidget *parent)
  : QWidget(parent)
{
  systemPalette_ = palette();
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);
  auto *fixedHeader = new QWidget(this);
  auto *fixedLayout = new QVBoxLayout(fixedHeader);
  fixedLayout->setContentsMargins(12, 12, 12, 10);
  fixedLayout->setSpacing(8);
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
  entryPlayer_ = new KalturaPlayerPreview(this);
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
  fixedLayout->addLayout(dockHeader);
  fixedLayout->addSpacing(6);
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
  auto *refreshPlayerButton = new QPushButton("Refresh Player", entryCard);
  refreshPlayerButton->setToolTip("Reload only the Kaltura player");
  entryCardHeader->addWidget(refreshPlayerButton);
  entryCardHeader->addWidget(statusValue_);
  auto *entryContent = new QHBoxLayout();
  entryContent->setSpacing(12);
  entryContent->addWidget(entryPlayer_, 0, Qt::AlignTop);
  auto *entryDetails = new QVBoxLayout();
  entryDetails->setSpacing(4);
  entryDetails->addWidget(entryNameValue_);
  entryDetails->addWidget(entryIdValue_);
  entryDetails->addWidget(entryDescriptionValue_);
  entryDetails->addStretch(1);
  entryContent->addLayout(entryDetails, 1);
  entryCardLayout->addLayout(entryCardHeader);
  entryCardLayout->addLayout(entryContent);
  fixedLayout->addWidget(entryCard);
  rootLayout->addWidget(fixedHeader);
  auto *fixedHeaderDivider = new QFrame(this);
  fixedHeaderDivider->setFrameShape(QFrame::HLine);
  fixedHeaderDivider->setFrameShadow(QFrame::Sunken);
  fixedHeaderDivider->setLineWidth(1);
  fixedHeaderDivider->setToolTip("The entry information above remains fixed while scrolling");
  rootLayout->addWidget(fixedHeaderDivider);
  rootLayout->addWidget(scrollArea, 1);

  streamingTitle_ = new QLabel("Real-time Streaming", this);
  QFont healthTitleFont = streamingTitle_->font();
  healthTitleFont.setBold(true);
  streamingTitle_->setFont(healthTitleFont);
  layout->addWidget(streamingTitle_);

  const auto createEditor = [this](OutputEditor &editor, const QString &name) {
    editor.name = name.toStdString();
    auto *frame = new QFrame(this);
    frame->setObjectName(name + "OutputCard");
    frame->setStyleSheet("QFrame { border: 1px solid palette(mid); border-radius: 8px; }");
    auto *box = new QVBoxLayout(frame);
    auto *heading = new QLabel(name.toUpper(), frame);
    QFont font = heading->font();
    font.setBold(true);
    heading->setFont(font);
    editor.enabled = new QCheckBox("Enabled", frame);
    editor.protocol = new QComboBox(frame);
    editor.protocol->addItem("RTMP", static_cast<int>(OutputProtocol::RTMP));
    editor.protocol->addItem("RTMPS", static_cast<int>(OutputProtocol::RTMPS));
    editor.protocol->addItem("SRT", static_cast<int>(OutputProtocol::SRT));
    auto *header = new QHBoxLayout();
    header->addWidget(heading);
    header->addStretch(1);
    header->addWidget(editor.protocol);
    box->addLayout(header);
    editor.srtLatencyField = new QWidget(frame);
    auto *latencyForm = new QFormLayout(editor.srtLatencyField);
    latencyForm->setContentsMargins(0, 0, 0, 0);
    editor.latency = new ScrollSafeSpinBox(editor.srtLatencyField);
    editor.latency->setRange(250, 8000);
    editor.latency->setValue(3000);
    editor.latency->setSuffix(" ms");
    latencyForm->addRow("Latency", editor.latency);
    box->addWidget(editor.srtLatencyField);
    auto *detailsToggle = new QToolButton(frame);
    detailsToggle->setText("Connection details");
    detailsToggle->setCheckable(true);
    detailsToggle->setArrowType(Qt::RightArrow);
    detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    detailsToggle->setVisible(false);
    auto *details = new QWidget(frame);
    auto *detailsBox = new QVBoxLayout(details);
    detailsBox->setContentsMargins(12, 0, 0, 0);
    editor.endpoint = new QLineEdit(frame);
    editor.endpoint->setPlaceholderText("rtmps:// or srt:// ingest endpoint");
    auto *common = new QFormLayout();
    common->addRow(editor.enabled);
    common->addRow("Endpoint", editor.endpoint);
    detailsBox->addLayout(common);

    editor.rtmpFields = new QWidget(frame);
    auto *rtmpForm = new QFormLayout(editor.rtmpFields);
    rtmpForm->setContentsMargins(0, 0, 0, 0);
    editor.key = new QLineEdit(editor.rtmpFields);
    editor.key->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    editor.username = new QLineEdit(editor.rtmpFields);
    editor.password = new QLineEdit(editor.rtmpFields);
    editor.password->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    rtmpForm->addRow("Stream key", editor.key);
    rtmpForm->addRow("Username", editor.username);
    rtmpForm->addRow("Password", editor.password);
    detailsBox->addWidget(editor.rtmpFields);

    editor.srtFields = new QWidget(frame);
    auto *srtForm = new QFormLayout(editor.srtFields);
    srtForm->setContentsMargins(0, 0, 0, 0);
    editor.host = new QLineEdit(editor.srtFields);
    editor.port = new QSpinBox(editor.srtFields);
    editor.port->setRange(0, 65535);
    editor.mode = new QComboBox(editor.srtFields);
    editor.mode->addItem("Caller", static_cast<int>(SrtMode::Caller));
    editor.mode->addItem("Listener", static_cast<int>(SrtMode::Listener));
    editor.mode->addItem("Rendezvous", static_cast<int>(SrtMode::Rendezvous));
    editor.streamId = new QLineEdit(editor.srtFields);
    srtForm->addRow("Host", editor.host);
    srtForm->addRow("Port", editor.port);
    srtForm->addRow("Mode", editor.mode);
    srtForm->addRow("Stream ID", editor.streamId);
    detailsBox->addWidget(editor.srtFields);

    auto *advancedToggle = new QToolButton(frame);
    advancedToggle->setText("Advanced options");
    advancedToggle->setCheckable(true);
    advancedToggle->setArrowType(Qt::RightArrow);
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *advanced = new QWidget(frame);
    auto *advancedForm = new QFormLayout(advanced);
    advancedForm->setContentsMargins(12, 0, 0, 0);
    editor.passphrase = new QLineEdit(advanced);
    editor.passphrase->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    editor.pbkeylen = new QComboBox(advanced);
    editor.pbkeylen->addItem("Automatic", 0);
    editor.pbkeylen->addItem("16", 16);
    editor.pbkeylen->addItem("24", 24);
    editor.pbkeylen->addItem("32", 32);
    editor.timeout = new QSpinBox(advanced);
    editor.timeout->setRange(0, 120000);
    editor.timeout->setSuffix(" ms");
    editor.packetSize = new QSpinBox(advanced);
    editor.packetSize->setRange(188, 65536);
    editor.reconnect = new QCheckBox("Reconnect automatically", advanced);
    editor.reconnectDelay = new QSpinBox(advanced);
    editor.reconnectDelay->setRange(1, 60);
    editor.reconnectDelay->setSuffix(" s");
    editor.reconnectRetries = new QSpinBox(advanced);
    editor.reconnectRetries->setRange(0, 10000);
    advancedForm->addRow("SRT passphrase", editor.passphrase);
    advancedForm->addRow("SRT PBKEYLEN", editor.pbkeylen);
    advancedForm->addRow("SRT timeout", editor.timeout);
    advancedForm->addRow("Packet size", editor.packetSize);
    advancedForm->addRow(editor.reconnect);
    advancedForm->addRow("Reconnect delay", editor.reconnectDelay);
    advancedForm->addRow("Maximum retries", editor.reconnectRetries);
    advanced->setVisible(false);
    detailsBox->addWidget(advancedToggle);
    detailsBox->addWidget(advanced);
    editor.apply = new QPushButton("Apply " + name + " Settings", frame);
    editor.apply->setVisible(false);
    box->addWidget(editor.apply);
    details->setVisible(false);
    box->addWidget(detailsToggle);
    box->addWidget(details);
    QObject::connect(detailsToggle, &QToolButton::toggled,
      [detailsToggle, details](bool open) {
        detailsToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        details->setVisible(open);
      });
    QObject::connect(advancedToggle, &QToolButton::toggled,
      [advancedToggle, advanced](bool open) {
        advancedToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        advanced->setVisible(open);
      });
    QObject::connect(editor.protocol, &QComboBox::currentIndexChanged,
                     [this, &editor](int) { updateProtocolFields(editor); });
    return frame;
  };
  auto *startBothButton = new QPushButton("Start Both", this);
  auto *stopBothButton = new QPushButton("Stop Both", this);
  auto *bothControls = new QHBoxLayout();
  bothControls->addWidget(startBothButton);
  bothControls->addWidget(stopBothButton);
  layout->addLayout(bothControls);
  layout->addWidget(createEditor(primaryEditor_, "Primary"));
  layout->addWidget(primaryHealthValue_);
  auto *primaryControls = new QHBoxLayout();
  primaryControls->addWidget(startPrimaryButton_);
  primaryControls->addWidget(stopPrimaryButton_);
  layout->addLayout(primaryControls);
  layout->addWidget(createEditor(backupEditor_, "Backup"));
  layout->addWidget(backupHealthValue_);
  auto *backupControls = new QHBoxLayout();
  backupControls->addWidget(startBackupButton_);
  backupControls->addWidget(stopBackupButton_);
  layout->addLayout(backupControls);
  layout->addSpacing(12);

  auto *captionsTitle = new QLabel("Live Captions (beta)", this);
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
  captionPreviewToggle_ = new QToolButton(this);
  captionPreviewToggle_->setText("CEA-608 output monitor");
  captionPreviewToggle_->setCheckable(true);
  captionPreviewToggle_->setChecked(false);
  captionPreviewToggle_->setArrowType(Qt::RightArrow);
  captionPreviewToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  captionPreviewToggle_->setAutoRaise(true);
  QFont captionPreviewToggleFont = captionPreviewToggle_->font();
  captionPreviewToggleFont.setBold(true);
  captionPreviewToggle_->setFont(captionPreviewToggleFont);
  captionPreviewContent_ = new QWidget(this);
  auto *captionPreviewLayout = new QVBoxLayout(captionPreviewContent_);
  captionPreviewLayout->setContentsMargins(16, 0, 0, 0);
  captionPreviewLayout->setSpacing(6);
  auto *copyCaptionSegmentsButton = new QPushButton("Copy All", captionPreviewContent_);
  copyCaptionSegmentsButton->setToolTip(
    "Copies the complete retained CEA-608 output history to the clipboard.");
  auto *captionSegmentsHeader = new QHBoxLayout();
  captionSegmentsHeader->addStretch(1);
  captionSegmentsHeader->addWidget(copyCaptionSegmentsButton);
  auto *captionSegmentsHelp = new QLabel(
    "Actual broadcast-safe segments handed to each active OBS output. "
    "This confirms OBS insertion, not receipt by the RTMP server.", captionPreviewContent_);
  captionSegmentsHelp->setWordWrap(true);
  captionPreviewLayout->addLayout(captionSegmentsHeader);
  captionPreviewLayout->addWidget(captionSegmentsHelp);
  captionPreviewLayout->addWidget(captionSegments_);
  captionPreviewContent_->setVisible(false);
  layout->addWidget(captionPreviewToggle_);
  layout->addWidget(captionPreviewContent_);
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
  QObject::connect(startBothButton, &QPushButton::clicked,
                   [this, primary = primaryControlCallback, backup = backupControlCallback]() {
                     if (primary && primaryEditor_.loadedConfig.enabled) primary(true);
                     if (backup && backupEditor_.loadedConfig.enabled) backup(true);
                   });
  QObject::connect(stopBothButton, &QPushButton::clicked,
                   [primary = primaryControlCallback, backup = backupControlCallback]() {
                     if (primary) primary(false);
                     if (backup) backup(false);
                   });
  QObject::connect(primaryEditor_.protocol, &QComboBox::activated,
                   [this, callback = outputConfigCallback](int) {
                     if (callback) callback(OutputRole::Primary,
                                            outputEditorConfig(primaryEditor_));
                   });
  QObject::connect(primaryEditor_.latency, &QSpinBox::valueChanged,
                   [this, callback = outputConfigCallback](int) {
                     if (callback) callback(OutputRole::Primary,
                                            outputEditorConfig(primaryEditor_));
                   });
  QObject::connect(backupEditor_.protocol, &QComboBox::activated,
                   [this, callback = outputConfigCallback](int) {
                     if (callback) callback(OutputRole::Backup,
                                            outputEditorConfig(backupEditor_));
                   });
  QObject::connect(backupEditor_.latency, &QSpinBox::valueChanged,
                   [this, callback = outputConfigCallback](int) {
                     if (callback) callback(OutputRole::Backup,
                                            outputEditorConfig(backupEditor_));
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
  QObject::connect(refreshPlayerButton, &QPushButton::clicked, entryPlayer_,
                   [player = entryPlayer_]() { player->refreshPlayer(); });
  QObject::connect(copyCaptionSegmentsButton, &QPushButton::clicked, [this]() {
    QApplication::clipboard()->setText(captionSegments_->toPlainText());
  });
  QObject::connect(captionPreviewToggle_, &QToolButton::toggled, [this](bool expanded) {
    captionPreviewToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    captionPreviewContent_->setVisible(expanded);
    if (expanded) {
      captionPreviewStartSequence_ = latestCaptionSequence_;
    }
    captionSegments_->clear();
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
  uint64_t newestSequence = 0;
  for (const captions::CaptionSegment &segment : health.recentSegments) {
    newestSequence = std::max(newestSequence, segment.sequence);
  }
  latestCaptionSequence_ = newestSequence;
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

  if (captionPreviewToggle_ && captionPreviewToggle_->isChecked()) {
    populateCaptionSegments(health);
  }
}

void KalturaDock::populateCaptionSegments(const captions::CaptionHealth &health)
{
  QStringList segments;
  for (auto segment = health.recentSegments.crbegin();
       segment != health.recentSegments.crend(); ++segment) {
    if (segment->sequence <= captionPreviewStartSequence_) {
      continue;
    }
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
      "The selected entry's player and broadcast details will appear here.");
    entryPlayer_->setPlayer(0, {}, {});
    return;
  }
  const QString name = QString::fromUtf8(settings.selectedEntryName);
  entryNameValue_->setText(name.isEmpty() ? "Untitled live entry" : name);
  entryIdValue_->setText("Entry ID: " + QString::fromUtf8(settings.selectedEntryId));
  const QString description = QString::fromUtf8(settings.selectedEntryDescription).trimmed();
  entryDescriptionValue_->setText(
    description.isEmpty() ? "No description provided." : description);
  entryPlayer_->setPlayer(settings.partnerId, settings.selectedEntryId,
                          settings.kalturaSession);
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
  primaryHealthValue_->setVisible(true);
  startPrimaryButton_->setVisible(true);
  stopPrimaryButton_->setVisible(true);
  const bool primaryActive = activeOrTransitioning(health.primary.state);
  startPrimaryButton_->setEnabled(primaryAvailable && !primaryActive);
  stopPrimaryButton_->setEnabled(primaryAvailable && primaryActive &&
                                 health.primary.state != OutputHealthState::Stopping);
  const bool backupAvailable = health.backup.state != OutputHealthState::Disabled;
  backupHealthValue_->setVisible(true);
  startBackupButton_->setVisible(true);
  stopBackupButton_->setVisible(true);
  const bool backupActive = activeOrTransitioning(health.backup.state);
  startBackupButton_->setEnabled(backupAvailable && !backupActive);
  stopBackupButton_->setEnabled(backupAvailable && backupActive &&
                                health.backup.state != OutputHealthState::Stopping);
  streamingTitle_->setVisible(true);
}

void KalturaDock::updateProtocolFields(OutputEditor &editor)
{
  const auto protocol = static_cast<OutputProtocol>(editor.protocol->currentData().toInt());
  editor.rtmpFields->setVisible(protocol != OutputProtocol::SRT);
  editor.srtFields->setVisible(protocol == OutputProtocol::SRT);
  editor.srtLatencyField->setVisible(protocol == OutputProtocol::SRT);
  editor.endpoint->setPlaceholderText(protocol == OutputProtocol::SRT
    ? "srt://host:port (optional when Host and Port are set)"
    : protocol == OutputProtocol::RTMPS ? "rtmps://ingest.example.com/app"
                                        : "rtmp://ingest.example.com/app");
}

void KalturaDock::populateOutputEditor(OutputEditor &editor,
                                       const StreamOutputConfig &config)
{
  editor.loadedConfig = config;
  const QList<QObject *> controls{editor.enabled, editor.protocol, editor.endpoint,
    editor.key, editor.username, editor.password, editor.host, editor.port, editor.mode,
    editor.latency, editor.passphrase, editor.pbkeylen, editor.streamId, editor.timeout,
    editor.packetSize, editor.reconnect, editor.reconnectDelay, editor.reconnectRetries};
  std::vector<std::unique_ptr<QSignalBlocker>> blockers;
  blockers.reserve(static_cast<size_t>(controls.size()));
  for (QObject *control : controls)
    blockers.push_back(std::make_unique<QSignalBlocker>(control));
  editor.enabled->setChecked(config.enabled);
  editor.protocol->setCurrentIndex(std::max(0, editor.protocol->findData(
    static_cast<int>(config.protocol))));
  editor.endpoint->setText(QString::fromUtf8(config.endpoint));
  editor.key->setText(QString::fromUtf8(config.streamKey));
  editor.username->setText(QString::fromUtf8(config.username));
  editor.password->setText(QString::fromUtf8(config.password));
  editor.host->setText(QString::fromUtf8(config.srt.host));
  editor.port->setValue(config.srt.port);
  editor.mode->setCurrentIndex(std::max(0, editor.mode->findData(static_cast<int>(config.srt.mode))));
  editor.latency->setValue(config.srt.latencyMs);
  editor.passphrase->setText(QString::fromUtf8(config.srt.passphrase));
  editor.pbkeylen->setCurrentIndex(std::max(0, editor.pbkeylen->findData(config.srt.pbkeylen)));
  editor.streamId->setText(QString::fromUtf8(config.srt.streamId));
  editor.timeout->setValue(config.srt.timeoutMs);
  editor.packetSize->setValue(config.srt.packetSize);
  editor.reconnect->setChecked(config.reconnect.enabled);
  editor.reconnectDelay->setValue(config.reconnect.delaySeconds);
  editor.reconnectRetries->setValue(config.reconnect.maxRetries);
  updateProtocolFields(editor);
}

StreamOutputConfig KalturaDock::outputEditorConfig(const OutputEditor &editor) const
{
  StreamOutputConfig config = editor.loadedConfig;
  config.name = editor.name;
  config.enabled = editor.enabled->isChecked();
  config.protocol = static_cast<OutputProtocol>(editor.protocol->currentData().toInt());
  config.endpoint = editor.endpoint->text().trimmed().toUtf8().toStdString();
  config.streamKey = editor.key->text().toUtf8().toStdString();
  config.username = editor.username->text().trimmed().toUtf8().toStdString();
  config.password = editor.password->text().toUtf8().toStdString();
  config.srt.host = editor.host->text().trimmed().toUtf8().toStdString();
  config.srt.port = static_cast<uint16_t>(editor.port->value());
  config.srt.mode = static_cast<SrtMode>(editor.mode->currentData().toInt());
  config.srt.latencyMs = editor.latency->value();
  config.srt.passphrase = editor.passphrase->text().toUtf8().toStdString();
  config.srt.pbkeylen = editor.pbkeylen->currentData().toInt();
  config.srt.streamId = editor.streamId->text().toUtf8().toStdString();
  config.srt.timeoutMs = editor.timeout->value();
  config.srt.packetSize = editor.packetSize->value();
  config.reconnect.enabled = editor.reconnect->isChecked();
  config.reconnect.delaySeconds = editor.reconnectDelay->value();
  config.reconnect.maxRetries = editor.reconnectRetries->value();
  config.manualOverride = true;
  if (config.protocol == OutputProtocol::RTMP && !config.kalturaRtmpEndpoint.empty())
    config.endpoint = config.kalturaRtmpEndpoint;
  else if (config.protocol == OutputProtocol::RTMPS && !config.kalturaRtmpsEndpoint.empty())
    config.endpoint = config.kalturaRtmpsEndpoint;
  else if (config.protocol == OutputProtocol::SRT && !config.kalturaSrtEndpoint.empty()) {
    config.endpoint = config.kalturaSrtEndpoint;
    config.srt.streamId = config.kalturaSrtStreamId;
  }
  if (config.protocol != OutputProtocol::SRT && config.streamKey.empty())
    config.streamKey = "1";
  return config;
}

void KalturaDock::setOutputConfigurations(const StreamOutputConfig &primary,
                                          const StreamOutputConfig &backup)
{
  populateOutputEditor(primaryEditor_, primary);
  populateOutputEditor(backupEditor_, backup);
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
