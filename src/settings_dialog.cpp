#include "kaltura_live/settings_dialog.hpp"
#include "kaltura_live/api/kaltura_api_client.hpp"
#include "kaltura_live/version.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QLocale>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScrollArea>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextDocument>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <util/platform.h>

#include <algorithm>
#include <set>
#include <utility>

namespace kaltura_live {

namespace {

constexpr int kMaximumDictionaryTerms = 250;
constexpr int kMaximumDictionaryFieldLength = 128;

QString obsLogDirectory()
{
  char path[4096] = {};
  if (os_get_config_path(path, sizeof(path), "obs-studio/logs") <= 0) {
    return {};
  }
  return QDir::fromNativeSeparators(QString::fromUtf8(path));
}

QString redactStreamSecrets(QString line)
{
  static const QRegularExpression querySecret(
    R"(([?&](?:t|token|auth|password|passphrase|streamid)=)([^&\s'"\]]+))",
    QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression labeledStreamId(
    R"(((?:stream[_ ]?id)\s*(?:=|:|\[)\s*)([^\]&\s]+))",
    QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression urlCredentials(R"((://)[^/@\s:]+:[^/@\s]+@)");

  line.replace(querySecret, "\\1<redacted>");
  line.replace(labeledStreamId, "\\1<redacted>");
  line.replace(urlCredentials, "\\1<credentials-redacted>@");
  return line;
}

bool isKalturaStreamingLine(const QString &line)
{
  if (!line.contains("[kaltura-live]", Qt::CaseInsensitive)) return false;
  static const QStringList terms = {
    "stream", "output", "encoder", "primary", "backup", "reconnect",
    "SRT", "RTMP", "RTMPS", "caption queue"
  };
  return std::any_of(terms.cbegin(), terms.cend(), [&line](const QString &term) {
    return line.contains(term, Qt::CaseInsensitive);
  });
}

bool isNativeStreamingLine(const QString &line)
{
  static const QStringList terms = {
    "[x264 encoder:", "[VideoToolbox encoder:", "[obs-ffmpeg mpegts muxer",
    "[rtmp stream:", "Output 'adv_stream'", "Output 'simple_stream'",
    "Output 'kaltura_", "advanced_video_stream", "simple_video_stream",
    "kaltura_backup_output", "process_packet:", "==== Streaming Start",
    "==== Streaming Stop", "Reconnecting in", "Could not update timestamps for skipped samples"
  };
  return std::any_of(terms.cbegin(), terms.cend(), [&line](const QString &term) {
    return line.contains(term, Qt::CaseInsensitive);
  });
}

QString filteredStreamLog(const QString &path)
{
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) return {};

  QString result;
  QTextStream source(&input);
  QTextStream output(&result);
  int encoderContextLines = 0;
  while (!source.atEnd()) {
    const QString line = source.readLine();
    const bool encoderHeader = line.contains(" encoder: '", Qt::CaseInsensitive) &&
      (line.contains("stream", Qt::CaseInsensitive) ||
       line.contains("kaltura", Qt::CaseInsensitive));
    const bool relevant = isKalturaStreamingLine(line) || isNativeStreamingLine(line) ||
                          encoderContextLines > 0;
    if (relevant) output << redactStreamSecrets(line) << '\n';
    if (encoderHeader) encoderContextLines = 16;
    else if (encoderContextLines > 0) --encoderContextLines;
  }
  return result.trimmed();
}

QString endpointSummary(const StreamOutputConfig &config)
{
  const QUrl endpoint(QString::fromUtf8(config.endpoint));
  if (!endpoint.isValid() || endpoint.host().isEmpty()) return "not configured";
  QString authority = endpoint.scheme() + "://" + endpoint.host();
  if (endpoint.port() > 0) authority += ':' + QString::number(endpoint.port());
  return authority + endpoint.path();
}

const char *srtModeName(SrtMode mode)
{
  switch (mode) {
  case SrtMode::Caller: return "caller";
  case SrtMode::Listener: return "listener";
  case SrtMode::Rendezvous: return "rendezvous";
  }
  return "unknown";
}

bool validDictionaryField(const QString &value, bool allowEmpty = false)
{
  const QString trimmed = value.trimmed();
  if ((!allowEmpty && trimmed.isEmpty()) ||
      trimmed.size() > kMaximumDictionaryFieldLength) {
    return false;
  }
  return std::none_of(trimmed.cbegin(), trimmed.cend(), [](QChar character) {
    return character.category() == QChar::Other_Control || character == '\n' ||
           character == '\r';
  });
}

QList<QStringList> parseCsv(const QString &contents, QString &failure)
{
  QList<QStringList> rows;
  QStringList row;
  QString field;
  bool quoted = false;
  for (qsizetype index = 0; index < contents.size(); ++index) {
    const QChar character = contents[index];
    if (quoted) {
      if (character == '"') {
        if (index + 1 < contents.size() && contents[index + 1] == '"') {
          field += '"';
          ++index;
        } else {
          quoted = false;
        }
      } else {
        field += character;
      }
      continue;
    }
    if (character == '"' && field.isEmpty()) {
      quoted = true;
    } else if (character == ',') {
      row.append(field);
      field.clear();
    } else if (character == '\n') {
      row.append(field);
      field.clear();
      rows.append(row);
      row.clear();
    } else if (character != '\r') {
      field += character;
    }
  }
  if (quoted) {
    failure = "The CSV contains an unterminated quoted field.";
    return {};
  }
  if (!field.isEmpty() || !row.isEmpty()) {
    row.append(field);
    rows.append(row);
  }
  return rows;
}

QString friendlyValidationError(const api::ApiError &error)
{
  switch (error.kind) {
  case api::ApiErrorKind::Timeout:
    return "Kaltura did not respond in time. Check your connection and try again.";
  case api::ApiErrorKind::Network:
  case api::ApiErrorKind::Http:
    return "Kaltura could not be reached. Check your connection and try again.";
  case api::ApiErrorKind::Api:
    if (error.code == "INVALID_KS" || error.code == "INVALID_KS_FORMAT" ||
        error.code == "SERVICE_FORBIDDEN") {
      return "This Kaltura Session is invalid or has expired.";
    }
    return "Kaltura could not validate this session. Check its permissions and try again.";
  case api::ApiErrorKind::InvalidJson:
  case api::ApiErrorKind::InvalidResponse:
    return "Kaltura returned an unexpected response. Please try again.";
  case api::ApiErrorKind::InvalidRequest:
    return "Enter a valid Kaltura Session before connecting.";
  }
  return "The Kaltura Session could not be validated.";
}

QString errorKindName(api::ApiErrorKind kind)
{
  switch (kind) {
  case api::ApiErrorKind::InvalidRequest: return "Invalid request";
  case api::ApiErrorKind::Network: return "Network exception";
  case api::ApiErrorKind::Timeout: return "Timeout exception";
  case api::ApiErrorKind::Http: return "HTTP exception";
  case api::ApiErrorKind::InvalidJson: return "JSON parsing exception";
  case api::ApiErrorKind::InvalidResponse: return "Invalid response";
  case api::ApiErrorKind::Api: return "Kaltura API exception";
  }
  return "Unknown exception";
}

QString maskedRequestDiagnostics(qsizetype sessionLength)
{
  const QString masked = QString("<KS redacted; %1 characters>").arg(sessionLength);
  const QStringList tlsBackends = QSslSocket::availableBackends();
  return QString(
           "Endpoint: https://www.kaltura.com/api_v3/service/session/action/get\n"
           "Method: POST\n"
           "Content-Type: application/x-www-form-urlencoded\n"
           "Qt runtime: %2\n"
           "TLS supported: %3\n"
           "Active TLS backend: %4\n"
           "Available TLS backends: %5\n"
           "TLS runtime: %6\n\n"
           "Equivalent command:\n"
           "curl -X POST 'https://www.kaltura.com/api_v3/service/session/action/get' \\\n"
           "  -H 'Accept: application/json' \\\n"
           "  --data-urlencode 'format=1' \\\n"
           "  --data-urlencode 'ks=%1' \\\n"
           "  --data-urlencode 'session=%1'\n\n"
           "Response: pending")
    .arg(masked, qVersion(), QSslSocket::supportsSsl() ? "yes" : "no",
         QSslSocket::activeBackend().isEmpty() ? "none" : QSslSocket::activeBackend(),
         tlsBackends.isEmpty() ? "none" : tlsBackends.join(", "),
         QSslSocket::sslLibraryVersionString().isEmpty()
           ? "not reported"
           : QSslSocket::sslLibraryVersionString());
}

QString safeExceptionDetails(const api::ApiError &error, const QByteArray &session)
{
  QString message = QString::fromUtf8(error.message);
  const QString rawSession = QString::fromUtf8(session);
  if (!rawSession.isEmpty()) {
    message.replace(rawSession, "<KS redacted>", Qt::CaseSensitive);
    message.replace(QString::fromLatin1(QUrl::toPercentEncoding(rawSession)),
                    "<KS redacted>", Qt::CaseSensitive);
  }
  return QString("Response: failed\nHTTP status: %1\nAttempts: %2\nException type: %3\n"
                 "Kaltura code: %4\nException: %5")
    .arg(error.httpStatus == 0 ? "not received" : QString::number(error.httpStatus))
    .arg(error.attempts)
    .arg(errorKindName(error.kind),
         QString::fromUtf8(error.code).isEmpty() ? "none" : QString::fromUtf8(error.code),
         message.isEmpty() ? "No additional message" : message);
}

QString entryStatusName(int status)
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

enum EntryColumn {
  ThumbnailColumn,
  NameColumn,
  IdColumn,
  DescriptionColumn,
  CreatedColumn,
  StatusColumn,
  EntryColumnCount,
};

}  // namespace

SettingsDialog::SettingsDialog(const PluginSettings &currentSettings,
                               api::KalturaApiClient &apiClient,
                               ApplyStreamSettings applyStreamSettings,
                               RevertStreamSettings revertStreamSettings,
                               CanRevertStreamSettings canRevertStreamSettings,
                               QWidget *parent)
  : QDialog(parent), apiClient_(apiClient), draftSettings_(currentSettings),
    applyStreamSettings_(std::move(applyStreamSettings)),
    revertStreamSettings_(std::move(revertStreamSettings)),
    canRevertStreamSettings_(std::move(canRevertStreamSettings))
{
  setWindowTitle("Kaltura Live Settings");
  setModal(true);
  setWindowFlag(Qt::WindowMaximizeButtonHint, true);
  setSizeGripEnabled(true);
  setMinimumSize(480, 420);
  resize(1000, 760);
  systemPalette_ = palette();

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(20, 20, 20, 16);
  layout->setSpacing(16);

  auto *title = new QLabel("Kaltura Live", this);
  QFont titleFont = title->font();
  titleFont.setPointSize(titleFont.pointSize() + 4);
  titleFont.setBold(true);
  title->setFont(titleFont);

  auto *description = new QLabel(
    "Configure the plugin and validate your Kaltura Session securely.", this);
  description->setWordWrap(true);

  auto *tabs = new QTabWidget(this);
  tabs->setMinimumSize(520, 620);

  auto *generalTab = new QWidget(tabs);
  auto *generalLayout = new QVBoxLayout(generalTab);
  generalLayout->setSizeConstraint(QLayout::SetMinimumSize);
  generalLayout->setContentsMargins(16, 18, 16, 16);
  generalLayout->setSpacing(12);

  auto *sessionGroup = new QGroupBox("Kaltura Session (KS)", generalTab);
  auto *sessionLayout = new QVBoxLayout(sessionGroup);
  sessionLayout->setSpacing(10);

  auto *sessionHelp = new QLabel(
    "Enter the session token used for Kaltura authentication. It is always masked and is never logged.",
    sessionGroup);
  sessionHelp->setWordWrap(true);

  auto *sessionRow = new QHBoxLayout();
  sessionEdit_ = new QLineEdit(sessionGroup);
  sessionEdit_->setEchoMode(QLineEdit::Password);
  sessionEdit_->setMaxLength(4096);
  sessionEdit_->setPlaceholderText("Enter Kaltura Session");
  sessionEdit_->setAccessibleName("Kaltura Session");
  sessionEdit_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoPredictiveText |
                                    Qt::ImhSensitiveData);
  sessionEdit_->setText(QString::fromStdString(currentSettings.kalturaSession));

  validateButton_ = new QPushButton("Validate", sessionGroup);
  validateButton_->setDefault(false);
  sessionRow->addWidget(sessionEdit_, 1);
  sessionRow->addWidget(validateButton_);

  validationStatus_ = new QLabel(sessionGroup);
  validationStatus_->setWordWrap(true);
  validationStatus_->setVisible(false);

  sessionLayout->addWidget(sessionHelp);
  sessionLayout->addLayout(sessionRow);
  sessionLayout->addWidget(validationStatus_);
  generalLayout->addWidget(sessionGroup);

  connectionGroup_ = new QGroupBox("Connection", generalTab);
  connectionGroup_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  auto *connectionLayout = new QHBoxLayout(connectionGroup_);
  connectionLayout->setContentsMargins(12, 8, 12, 8);
  connectionLayout->setSpacing(18);
  connectionState_ = new QLabel("✓ Connected", connectionGroup_);
  QFont connectedFont = connectionState_->font();
  connectedFont.setBold(true);
  connectionState_->setFont(connectedFont);
  connectionState_->setStyleSheet("color: #2e9b56;");
  connectedUser_ = new QLabel(connectionGroup_);
  connectedPartner_ = new QLabel(connectionGroup_);
  connectedExpiration_ = new QLabel(connectionGroup_);
  connectedUser_->setTextFormat(Qt::PlainText);
  connectedPartner_->setTextFormat(Qt::PlainText);
  connectedExpiration_->setTextFormat(Qt::PlainText);
  connectionLayout->addWidget(connectionState_);
  connectionLayout->addWidget(connectedUser_);
  connectionLayout->addWidget(connectedPartner_);
  connectionLayout->addWidget(connectedExpiration_);
  connectionLayout->addStretch(1);
  connectionGroup_->setVisible(false);
  generalLayout->addWidget(connectionGroup_);

  entriesGroup_ = new QGroupBox("Live entries", generalTab);
  auto *entriesLayout = new QVBoxLayout(entriesGroup_);
  auto *entryControls = new QHBoxLayout();
  entrySearch_ = new QLineEdit(entriesGroup_);
  entrySearch_->setPlaceholderText("Search entry name, ID, or description");
  entrySearch_->setClearButtonEnabled(true);
  entrySearch_->setMaxLength(256);
  entryStatusFilter_ = new QComboBox(entriesGroup_);
  entryStatusFilter_->addItem("All statuses", -1);
  refreshEntriesButton_ = new QPushButton("Refresh", entriesGroup_);
  revertStreamSettingsButton_ = new QPushButton("Revert OBS Settings", entriesGroup_);
  entryControls->addWidget(entrySearch_, 1);
  entryControls->addWidget(entryStatusFilter_);
  entryControls->addWidget(revertStreamSettingsButton_);
  entryControls->addWidget(refreshEntriesButton_);

  entriesStatus_ = new QLabel(entriesGroup_);
  entriesStatus_->setWordWrap(true);
  entriesTable_ = new QTableWidget(entriesGroup_);
  entriesTable_->setColumnCount(EntryColumnCount);
  entriesTable_->setHorizontalHeaderLabels(
    {"Thumbnail", "Entry Name", "Entry ID", "Description", "Created", "Status"});
  entriesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  entriesTable_->setSelectionMode(QAbstractItemView::SingleSelection);
  entriesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  entriesTable_->setAlternatingRowColors(true);
  entriesTable_->setSortingEnabled(true);
  entriesTable_->setIconSize(QSize(96, 54));
  entriesTable_->verticalHeader()->setVisible(false);
  entriesTable_->verticalHeader()->setDefaultSectionSize(62);
  entriesTable_->horizontalHeader()->setSectionResizeMode(ThumbnailColumn,
                                                          QHeaderView::Fixed);
  entriesTable_->setColumnWidth(ThumbnailColumn, 110);
  entriesTable_->horizontalHeader()->setSectionResizeMode(NameColumn,
                                                          QHeaderView::ResizeToContents);
  entriesTable_->horizontalHeader()->setSectionResizeMode(IdColumn,
                                                          QHeaderView::ResizeToContents);
  entriesTable_->horizontalHeader()->setSectionResizeMode(DescriptionColumn,
                                                          QHeaderView::Stretch);
  entriesTable_->horizontalHeader()->setSectionResizeMode(CreatedColumn,
                                                          QHeaderView::ResizeToContents);
  entriesTable_->horizontalHeader()->setSectionResizeMode(StatusColumn,
                                                          QHeaderView::ResizeToContents);
  entriesTable_->setMinimumHeight(280);
  thumbnailNetwork_ = new QNetworkAccessManager(this);
  searchDebounce_ = new QTimer(this);
  searchDebounce_->setSingleShot(true);
  searchDebounce_->setInterval(300);

  auto *pagination = new QHBoxLayout();
  previousPageButton_ = new QPushButton("Previous", entriesGroup_);
  nextPageButton_ = new QPushButton("Next", entriesGroup_);
  pageLabel_ = new QLabel("Page 1", entriesGroup_);
  pageLabel_->setAlignment(Qt::AlignCenter);
  pageSizeCombo_ = new QComboBox(entriesGroup_);
  for (int size : {10, 20, 50, 100}) {
    pageSizeCombo_->addItem(QString::number(size), size);
  }
  pageSizeCombo_->setCurrentIndex(pageSizeCombo_->findData(20));
  pagination->addWidget(previousPageButton_);
  pagination->addWidget(pageLabel_, 1);
  pagination->addWidget(nextPageButton_);
  pagination->addSpacing(12);
  pagination->addWidget(new QLabel("Entries per page:", entriesGroup_));
  pagination->addWidget(pageSizeCombo_);

  auto *selectionHelp = new QLabel(
    "Select one entry. The selection is saved in the current OBS project when you click OK.",
    entriesGroup_);
  selectionHelp->setWordWrap(true);
  entriesLayout->addLayout(entryControls);
  entriesLayout->addWidget(entriesStatus_);
  entriesLayout->addWidget(entriesTable_, 1);
  entriesLayout->addLayout(pagination);
  entriesLayout->addWidget(selectionHelp);
  entriesGroup_->setVisible(false);
  generalLayout->addWidget(entriesGroup_, 1);

  auto *diagnosticsGroup = new QGroupBox("Request diagnostics (KS redacted)", tabs);
  auto *diagnosticsLayout = new QVBoxLayout(diagnosticsGroup);
  diagnostics_ = new QPlainTextEdit(diagnosticsGroup);
  diagnostics_->setReadOnly(true);
  diagnostics_->setLineWrapMode(QPlainTextEdit::NoWrap);
  diagnostics_->setMinimumHeight(180);
  diagnostics_->setPlainText("No validation request has been sent yet.");
  QFont diagnosticsFont = diagnostics_->font();
  diagnosticsFont.setFamilies({"SF Mono", "Menlo", "Monaco", "monospace"});
  diagnostics_->setFont(diagnosticsFont);
  diagnosticsLayout->addWidget(diagnostics_);
  generalLayout->addStretch(1);

  auto *streamingTab = new QWidget(tabs);
  auto *streamingLayout = new QVBoxLayout(streamingTab);
  streamingLayout->setSizeConstraint(QLayout::SetMinimumSize);
  streamingLayout->setContentsMargins(16, 18, 16, 16);

  auto *endpointGroup = new QGroupBox("Stream destination", streamingTab);
  auto *endpointLayout = new QVBoxLayout(endpointGroup);
  auto *endpointHelp = new QLabel(
    "Choose which Kaltura destination is enabled. When Both is selected, Primary uses "
    "the standard OBS stream and Backup starts alongside it.",
    endpointGroup);
  endpointHelp->setWordWrap(true);
  primaryEndpoint_ = new QRadioButton("Primary only", endpointGroup);
  backupEndpoint_ = new QRadioButton("Backup only", endpointGroup);
  bothEndpoints_ = new QRadioButton("Both", endpointGroup);
  endpointLayout->addWidget(endpointHelp);
  endpointLayout->addSpacing(4);
  endpointLayout->addWidget(primaryEndpoint_);
  endpointLayout->addWidget(backupEndpoint_);
  endpointLayout->addWidget(bothEndpoints_);
  streamingLayout->addWidget(endpointGroup);
  switch (currentSettings.preferredEndpoint) {
  case StreamingEndpoint::Primary: primaryEndpoint_->setChecked(true); break;
  case StreamingEndpoint::Backup: backupEndpoint_->setChecked(true); break;
  case StreamingEndpoint::Both: bothEndpoints_->setChecked(true); break;
  }

  auto *captionPresentationGroup = new QGroupBox("Caption presentation", streamingTab);
  auto *captionPresentationLayout = new QFormLayout(captionPresentationGroup);
  auto *captionPresentationHelp = new QLabel(
    "Set the embedded CEA-608 cue layout. A viewer's player may override these preferences.",
    captionPresentationGroup);
  captionPresentationHelp->setWordWrap(true);
  captionStyleCombo_ = new QComboBox(captionPresentationGroup);
  captionStyleCombo_->addItem("Standard · 2 lines", static_cast<int>(CaptionStyle::Standard));
  captionStyleCombo_->addItem("Compact · 1 line", static_cast<int>(CaptionStyle::Compact));
  captionStyleCombo_->addItem("Uppercase · 2 lines", static_cast<int>(CaptionStyle::Uppercase));
  captionStyleCombo_->setCurrentIndex(captionStyleCombo_->findData(
    static_cast<int>(currentSettings.captionStyle)));
  captionPlacementCombo_ = new QComboBox(captionPresentationGroup);
  captionPlacementCombo_->addItem("Bottom", static_cast<int>(CaptionPlacement::Bottom));
  captionPlacementCombo_->addItem("Top", static_cast<int>(CaptionPlacement::Top));
  captionPlacementCombo_->setCurrentIndex(captionPlacementCombo_->findData(
    static_cast<int>(currentSettings.captionPlacement)));
  captionAlignmentCombo_ = new QComboBox(captionPresentationGroup);
  captionAlignmentCombo_->addItem("Center", static_cast<int>(CaptionAlignment::Center));
  captionAlignmentCombo_->addItem("Left", static_cast<int>(CaptionAlignment::Left));
  captionAlignmentCombo_->addItem("Right", static_cast<int>(CaptionAlignment::Right));
  captionAlignmentCombo_->setCurrentIndex(captionAlignmentCombo_->findData(
    static_cast<int>(currentSettings.captionAlignment)));
  captionTranscript_ = new QPlainTextEdit(captionPresentationGroup);
  captionTranscript_->setReadOnly(true);
  captionTranscript_->setPlaceholderText(
    "Enable live captions in the Kaltura Live dock, then speak to preview the "
    "program-audio transcription here.");
  captionTranscript_->setMinimumHeight(110);
  captionTranscript_->setMaximumHeight(180);
  captionTranscript_->document()->setMaximumBlockCount(100);
  captionTranscript_->setToolTip(
    "Local Whisper transcript preview for checking recognition and sync before going live.");
  auto *copyTranscriptButton = new QPushButton("Copy Transcript", captionPresentationGroup);
  copyTranscriptButton->setToolTip("Copies the complete retained live transcript.");
  captionPresentationLayout->addRow(captionPresentationHelp);
  captionPresentationLayout->addRow("Style:", captionStyleCombo_);
  captionPresentationLayout->addRow("Placement:", captionPlacementCombo_);
  captionPresentationLayout->addRow("Alignment:", captionAlignmentCombo_);
  captionPresentationLayout->addRow("Live transcript:", captionTranscript_);
  captionPresentationLayout->addRow(QString(), copyTranscriptButton);
  connect(copyTranscriptButton, &QPushButton::clicked, this, [this]() {
    QApplication::clipboard()->setText(captionTranscript_->toPlainText());
  });
  streamingLayout->addWidget(captionPresentationGroup);

  auto *dictionaryGroup = new QGroupBox("Custom transcription dictionary", streamingTab);
  auto *dictionaryLayout = new QVBoxLayout(dictionaryGroup);
  dictionaryLayout->setSpacing(10);
  auto *dictionaryHelp = new QLabel(
    "Add names, acronyms, and specialized terms. Preferred text is required and is supplied "
    "to Local Whisper as vocabulary context. Spoken form is optional; when provided, matching "
    "transcript text is replaced with the preferred text.",
    dictionaryGroup);
  dictionaryHelp->setWordWrap(true);
  captionDictionaryTable_ = new QTableWidget(0, 2, dictionaryGroup);
  captionDictionaryTable_->setHorizontalHeaderLabels({"Spoken form", "Preferred text"});
  captionDictionaryTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  captionDictionaryTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  captionDictionaryTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  captionDictionaryTable_->setMinimumHeight(180);
  captionDictionaryTable_->setToolTip(
    "Required CSV header: preferred_text. Optional header: spoken_form.");
  for (const captions::CaptionDictionaryEntry &entry : currentSettings.captionDictionary) {
    const int row = captionDictionaryTable_->rowCount();
    captionDictionaryTable_->insertRow(row);
    captionDictionaryTable_->setItem(
      row, 0, new QTableWidgetItem(QString::fromUtf8(entry.spokenForm)));
    captionDictionaryTable_->setItem(
      row, 1, new QTableWidgetItem(QString::fromUtf8(entry.preferredText)));
  }
  auto *dictionaryButtons = new QHBoxLayout();
  auto *addDictionaryTermButton = new QPushButton("Add Term", dictionaryGroup);
  removeDictionaryTermsButton_ = new QPushButton("Remove Selected", dictionaryGroup);
  auto *importDictionaryButton = new QPushButton("Import CSV…", dictionaryGroup);
  auto *saveSampleDictionaryButton = new QPushButton("Save Sample CSV…", dictionaryGroup);
  removeDictionaryTermsButton_->setEnabled(false);
  dictionaryButtons->addWidget(addDictionaryTermButton);
  dictionaryButtons->addWidget(removeDictionaryTermsButton_);
  dictionaryButtons->addStretch(1);
  dictionaryButtons->addWidget(importDictionaryButton);
  dictionaryButtons->addWidget(saveSampleDictionaryButton);
  dictionaryLayout->addWidget(dictionaryHelp);
  dictionaryLayout->addWidget(captionDictionaryTable_);
  dictionaryLayout->addLayout(dictionaryButtons);
  streamingLayout->addWidget(dictionaryGroup);
  streamingLayout->addStretch(1);

  auto *advancedTab = new QWidget(tabs);
  auto *advancedLayout = new QVBoxLayout(advancedTab);
  advancedLayout->setSizeConstraint(QLayout::SetMinimumSize);
  advancedLayout->setContentsMargins(16, 18, 16, 16);
  advancedLayout->setSpacing(12);

  auto *loggingGroup = new QGroupBox("Logging", advancedTab);
  auto *loggingLayout = new QFormLayout(loggingGroup);
  loggingLevelCombo_ = new QComboBox(loggingGroup);
  loggingLevelCombo_->addItem("Information", static_cast<int>(LoggingLevel::Info));
  loggingLevelCombo_->addItem("Warnings only", static_cast<int>(LoggingLevel::Warning));
  loggingLevelCombo_->addItem("Errors only", static_cast<int>(LoggingLevel::Error));
  loggingLevelCombo_->setCurrentIndex(loggingLevelCombo_->findData(
    static_cast<int>(currentSettings.loggingLevel)));
  debugLoggingCheck_ = new QCheckBox("Enable debug logging", loggingGroup);
  debugLoggingCheck_->setChecked(currentSettings.debugLogging);
  auto *downloadStreamLogsButton = new QPushButton("Download Stream Logs…", loggingGroup);
  downloadStreamLogsButton->setToolTip(
    "Save a redacted report containing only Kaltura streaming, OBS encoder, muxer, "
    "SRT, and RTMP log events.");
  loggingLayout->addRow("Logging level:", loggingLevelCombo_);
  loggingLayout->addRow(QString(), debugLoggingCheck_);
  loggingLayout->addRow(QString(), downloadStreamLogsButton);

  auto *appearanceGroup = new QGroupBox("Appearance", advancedTab);
  auto *appearanceLayout = new QFormLayout(appearanceGroup);
  themeCombo_ = new QComboBox(appearanceGroup);
  themeCombo_->addItem("Use OBS theme", static_cast<int>(Theme::System));
  themeCombo_->addItem("Light", static_cast<int>(Theme::Light));
  themeCombo_->addItem("Dark", static_cast<int>(Theme::Dark));
  themeCombo_->setCurrentIndex(themeCombo_->findData(static_cast<int>(currentSettings.theme)));
  appearanceLayout->addRow("Theme:", themeCombo_);

  advancedLayout->addWidget(loggingGroup);
  advancedLayout->addWidget(appearanceGroup);
  advancedLayout->addWidget(diagnosticsGroup, 1);
  advancedLayout->addStretch(1);

  const auto addScrollableTab = [tabs](QWidget *page, const QString &label) {
    auto *scrollArea = new QScrollArea(tabs);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setWidget(page);
    tabs->addTab(scrollArea, label);
  };
  addScrollableTab(generalTab, "General");
  addScrollableTab(streamingTab, "Streaming");
  addScrollableTab(advancedTab, "Advanced");

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  auto *updatePluginButton = buttons->addButton("Update Plugin…", QDialogButtonBox::ActionRole);
  updatePluginButton->setToolTip(
    "Open the latest verified Kaltura Live release (installed version "
    KALTURA_LIVE_VERSION_STRING ").");

  connect(validateButton_, &QPushButton::clicked, this, [this]() { validateSession(); });
  connect(updatePluginButton, &QPushButton::clicked, this, [this]() {
    const QUrl releaseUrl("https://github.com/smelcerc/kaltura-obs-plugin/releases/latest");
    if (!QDesktopServices::openUrl(releaseUrl)) {
      QMessageBox::warning(this, "Could Not Open Updates",
                           "Open https://github.com/smelcerc/kaltura-obs-plugin/releases/latest "
                           "to download the newest plugin installer.");
    }
  });
  connect(downloadStreamLogsButton, &QPushButton::clicked,
          this, [this]() { downloadStreamLogs(); });
  connect(sessionEdit_, &QLineEdit::textChanged, this, [this]() {
    ++validationRequestId_;
    ++entryLoadRequestId_;
    validatedSession_.clear();
    liveEntries_.clear();
    thumbnailCache_.clear();
    thumbnailRequests_.clear();
    entriesTable_->setRowCount(0);
    entriesGroup_->setVisible(false);
    currentPage_ = 1;
    totalEntries_ = 0;
    draftSettings_.selectedEntryId.clear();
    draftSettings_.partnerId = 0;
    draftSettings_.selectedEntryName.clear();
    draftSettings_.selectedEntryDescription.clear();
    draftSettings_.selectedEntryThumbnailUrl.clear();
    draftSettings_.selectedEntryStatus = 0;
    validationStatus_->clear();
    validationStatus_->setVisible(false);
    clearConnectionDetails();
  });
  connect(themeCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
    applyTheme(static_cast<Theme>(themeCombo_->currentData().toInt()));
  });
  connect(addDictionaryTermButton, &QPushButton::clicked, this, [this]() {
    if (captionDictionaryTable_->rowCount() >= kMaximumDictionaryTerms) {
      QMessageBox::warning(this, "Dictionary Limit Reached",
                           "The custom dictionary supports up to 250 terms.");
      return;
    }
    const int row = captionDictionaryTable_->rowCount();
    captionDictionaryTable_->insertRow(row);
    captionDictionaryTable_->setItem(row, 0, new QTableWidgetItem());
    captionDictionaryTable_->setItem(row, 1, new QTableWidgetItem());
    captionDictionaryTable_->setCurrentCell(row, 0);
    captionDictionaryTable_->editItem(captionDictionaryTable_->item(row, 0));
  });
  connect(removeDictionaryTermsButton_, &QPushButton::clicked, this, [this]() {
    QModelIndexList rows = captionDictionaryTable_->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(), [](const QModelIndex &left, const QModelIndex &right) {
      return left.row() > right.row();
    });
    for (const QModelIndex &row : rows) {
      captionDictionaryTable_->removeRow(row.row());
    }
  });
  connect(captionDictionaryTable_, &QTableWidget::itemSelectionChanged, this, [this]() {
    removeDictionaryTermsButton_->setEnabled(
      !captionDictionaryTable_->selectionModel()->selectedRows().isEmpty());
  });
  connect(importDictionaryButton, &QPushButton::clicked,
          this, [this]() { importCaptionDictionaryCsv(); });
  connect(saveSampleDictionaryButton, &QPushButton::clicked,
          this, [this]() { saveSampleCaptionDictionaryCsv(); });
  connect(entrySearch_, &QLineEdit::textChanged, this, [this]() {
    if (validatedSession_.empty()) {
      return;
    }
    ++entryLoadRequestId_;
    currentPage_ = 1;
    liveEntries_.clear();
    entriesTable_->setRowCount(0);
    previousPageButton_->setEnabled(false);
    nextPageButton_->setEnabled(false);
    entriesStatus_->setText("Searching…");
    searchDebounce_->start();
  });
  connect(searchDebounce_, &QTimer::timeout, this, [this]() { startEntryLoad(); });
  connect(entryStatusFilter_, &QComboBox::currentIndexChanged, this,
          [this](int) { applyEntryFilters(); });
  connect(refreshEntriesButton_, &QPushButton::clicked, this,
          [this]() { startEntryLoad(); });
  connect(revertStreamSettingsButton_, &QPushButton::clicked, this, [this]() {
    if (!revertStreamSettings_ ||
        QMessageBox::question(this, "Revert OBS Streaming Settings",
                              "Restore the OBS streaming settings that were active before "
                              "Kaltura configured them?") != QMessageBox::Yes) {
      return;
    }
    std::string failure;
    if (!revertStreamSettings_(failure)) {
      QMessageBox::warning(
        this, "Could Not Revert OBS Settings",
        QString::fromUtf8(failure).isEmpty() ? "The previous settings could not be restored."
                                             : QString::fromUtf8(failure));
      return;
    }
    updateRevertButton();
    QMessageBox::information(this, "OBS Settings Restored",
                             "The previous OBS streaming settings were restored.");
  });
  connect(previousPageButton_, &QPushButton::clicked, this, [this]() {
    if (currentPage_ > 1) {
      --currentPage_;
      startEntryLoad();
    }
  });
  connect(nextPageButton_, &QPushButton::clicked, this, [this]() {
    const int pageSize = pageSizeCombo_->currentData().toInt();
    if (currentPage_ * pageSize < totalEntries_) {
      ++currentPage_;
      startEntryLoad();
    }
  });
  connect(pageSizeCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
    if (!validatedSession_.empty()) {
      currentPage_ = 1;
      startEntryLoad();
    }
  });
  connect(entriesTable_, &QTableWidget::itemSelectionChanged, this,
          [this]() { updateSelectedEntry(); });
  connect(entriesTable_->verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int) { loadVisibleThumbnails(); });
  connect(entriesTable_->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
          [this](int, Qt::SortOrder) {
            QTimer::singleShot(0, this, [this]() { loadVisibleThumbnails(); });
          });
  connect(buttons, &QDialogButtonBox::accepted, this, [this]() { validateAndAccept(); });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  layout->addWidget(title);
  layout->addWidget(description);
  layout->addWidget(tabs, 1);
  layout->addWidget(buttons);

  applyTheme(currentSettings.theme);
  updateRevertButton();

  if (!sessionEdit_->text().isEmpty()) {
    QTimer::singleShot(0, this, [this]() { validateSession(); });
  }
}

PluginSettings SettingsDialog::selectedSettings() const
{
  PluginSettings settings = draftSettings_;
  settings.kalturaSession = sessionEdit_->text().toUtf8().toStdString();
  settings.preferredEndpoint = backupEndpoint_->isChecked()
    ? StreamingEndpoint::Backup
    : bothEndpoints_->isChecked() ? StreamingEndpoint::Both
                                  : StreamingEndpoint::Primary;
  settings.loggingLevel = static_cast<LoggingLevel>(loggingLevelCombo_->currentData().toInt());
  settings.debugLogging = debugLoggingCheck_->isChecked();
  settings.captionStyle = static_cast<CaptionStyle>(captionStyleCombo_->currentData().toInt());
  settings.captionPlacement = static_cast<CaptionPlacement>(
    captionPlacementCombo_->currentData().toInt());
  settings.captionAlignment = static_cast<CaptionAlignment>(
    captionAlignmentCombo_->currentData().toInt());
  QString dictionaryFailure;
  collectCaptionDictionary(settings.captionDictionary, dictionaryFailure);
  settings.theme = static_cast<Theme>(themeCombo_->currentData().toInt());
  return settings;
}

void SettingsDialog::setCaptionTranscript(const QStringList &lines)
{
  captionTranscript_->setPlainText(lines.join('\n'));
  captionTranscript_->verticalScrollBar()->setValue(
    captionTranscript_->verticalScrollBar()->maximum());
}

void SettingsDialog::appendCaptionTranscript(const QString &text)
{
  const QString normalized = text.trimmed();
  if (normalized.isEmpty()) {
    return;
  }
  QScrollBar *scrollBar = captionTranscript_->verticalScrollBar();
  const bool followingLatest = scrollBar->value() == scrollBar->maximum();
  const int previousValue = scrollBar->value();
  captionTranscript_->appendPlainText(normalized);
  scrollBar->setValue(followingLatest ? scrollBar->maximum() : previousValue);
}

void SettingsDialog::downloadStreamLogs()
{
  const QString logDirectoryPath = obsLogDirectory();
  const QDir logDirectory(logDirectoryPath);
  if (logDirectoryPath.isEmpty() || !logDirectory.exists()) {
    QMessageBox::warning(this, "Stream Logs Unavailable",
                         "The OBS log directory could not be found.");
    return;
  }

  QString report;
  QTextStream output(&report);
  output << "Kaltura Live Stream and Encoder Diagnostic Export\n"
         << "Generated: " << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << " UTC\n"
         << "Plugin version: " KALTURA_LIVE_VERSION_STRING "\n";
  if (!draftSettings_.selectedEntryId.empty()) {
    output << "Entry ID: " << QString::fromUtf8(draftSettings_.selectedEntryId) << '\n';
  }

  const auto writeOutputSummary = [&output](const char *label,
                                             const StreamOutputConfig &config) {
    const QUrl endpoint(QString::fromUtf8(config.endpoint));
    output << '\n' << label << " stream\n"
           << "  Enabled: " << (config.enabled ? "yes" : "no") << '\n'
           << "  Protocol: " << outputProtocolName(config.protocol) << '\n'
           << "  Destination: " << endpointSummary(config) << '\n'
           << "  Destination query parameters: "
           << (endpoint.hasQuery() ? "configured (values redacted)" : "none") << '\n'
           << "  Reconnect: " << (config.reconnect.enabled ? "enabled" : "disabled");
    if (config.reconnect.enabled) {
      output << " (delay " << config.reconnect.delaySeconds << "s, maximum "
             << config.reconnect.maxRetries << " retries)";
    }
    output << '\n';
    if (config.protocol == OutputProtocol::SRT) {
      output << "  SRT host: "
             << (config.srt.host.empty() ? endpoint.host()
                                         : QString::fromUtf8(config.srt.host))
             << '\n'
             << "  SRT port: "
             << (config.srt.port > 0 ? QString::number(config.srt.port)
                                     : QString::number(endpoint.port()))
             << '\n'
             << "  SRT mode: " << srtModeName(config.srt.mode) << '\n'
             << "  SRT latency: " << config.srt.latencyMs << " ms\n"
             << "  SRT Stream ID: "
             << (config.srt.streamId.empty() ? "not configured" : "configured (redacted)")
             << '\n'
             << "  OBS stream-key field: blank (Stream ID is carried in the URL)\n"
             << "  SRT passphrase: "
             << (config.srt.passphrase.empty() ? "not configured" : "configured (redacted)")
             << '\n'
             << "  SRT PBKEYLEN: "
             << (config.srt.pbkeylen == 0 ? QString("automatic")
                                          : QString::number(config.srt.pbkeylen))
             << '\n'
             << "  SRT timeout: " << config.srt.timeoutMs << " ms\n"
             << "  SRT packet size: " << config.srt.packetSize << " bytes\n";
    } else {
      output << "  Stream key: "
             << (config.streamKey.empty() ? "not configured" : "configured (redacted)")
             << '\n'
             << "  Username authentication: "
             << ((!config.username.empty() && !config.password.empty())
                   ? "configured (values redacted)" : "not configured")
             << '\n';
    }
  };
  writeOutputSummary("Primary", draftSettings_.primaryOutput);
  writeOutputSummary("Backup", draftSettings_.backupOutput);
  output << "\nOutput mapping\n"
         << "  adv_stream/simple_stream: OBS-managed Primary or sole selected stream\n"
         << "  kaltura_backup_output: plugin-managed Backup stream\n"
         << "  Credential, token, stream-key, passphrase, and SRT Stream-ID values are redacted.\n";

  int includedFiles = 0;
  const QFileInfoList logFiles = logDirectory.entryInfoList(
    {"*.txt", "*.log"}, QDir::Files | QDir::Readable, QDir::Name);
  for (const QFileInfo &logFile : logFiles) {
    const QString filtered = filteredStreamLog(logFile.absoluteFilePath());
    if (filtered.isEmpty()) continue;
    ++includedFiles;
    output << "\n============================================================\n"
           << "Source OBS log: " << logFile.fileName() << '\n'
           << "============================================================\n"
           << filtered << '\n';
  }
  if (includedFiles == 0) {
    QMessageBox::information(this, "No Stream Logs Found",
                             "OBS has not recorded any streaming or encoder events yet.");
    return;
  }

  QString downloadDirectory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (downloadDirectory.isEmpty()) downloadDirectory = QDir::homePath();
  const QString suggestedPath = QDir(downloadDirectory).filePath(
    "kaltura-stream-logs-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".txt");
  const QString destination = QFileDialog::getSaveFileName(
    this, "Download Stream Logs", suggestedPath, "Text files (*.txt)");
  if (destination.isEmpty()) return;

  QSaveFile file(destination);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QMessageBox::warning(this, "Could Not Save Stream Logs",
                         "The selected file could not be opened for writing.");
    return;
  }
  file.write(report.toUtf8());
  if (!file.commit()) {
    QMessageBox::warning(this, "Could Not Save Stream Logs",
                         "The stream log export could not be completed.");
    return;
  }
  QMessageBox::information(
    this, "Stream Logs Downloaded",
    QString("Saved focused Primary and Backup stream diagnostics from %1 OBS log file(s).")
      .arg(includedFiles));
}

void SettingsDialog::validateSession()
{
  const QByteArray session = sessionEdit_->text().toUtf8();
  if (!isValidKalturaSession(session.toStdString())) {
    clearConnectionDetails();
    diagnostics_->setPlainText("No request sent: the KS failed local input validation.");
    validationStatus_->setText("Enter 10–4096 printable characters without spaces.");
    validationStatus_->setStyleSheet("color: #d04444;");
    validationStatus_->setVisible(true);
    sessionEdit_->setFocus();
    return;
  }

  const quint64 requestId = ++validationRequestId_;
  setValidationBusy(true);
  clearConnectionDetails();
  validationStatus_->setText("Connecting to Kaltura…");
  validationStatus_->setStyleSheet(QString());
  validationStatus_->setVisible(true);
  diagnostics_->setPlainText(maskedRequestDiagnostics(session.size()));

  QPointer<SettingsDialog> dialog(this);
  apiClient_.validateSession(session.toStdString(),
                             [dialog, requestId, session](api::ApiResult<api::SessionInfo> result) {
    if (!dialog || dialog->validationRequestId_ != requestId) {
      return;
    }

    dialog->setValidationBusy(false);
    if (!result.succeeded()) {
      dialog->clearConnectionDetails();
      dialog->draftSettings_.partnerId = 0;
      dialog->validationStatus_->setText(friendlyValidationError(*result.error));
      dialog->validationStatus_->setStyleSheet("color: #d04444;");
      dialog->validationStatus_->setVisible(true);
      dialog->diagnostics_->appendPlainText(
        "\n\n" + safeExceptionDetails(*result.error, session));
      return;
    }

    const api::SessionInfo &sessionInfo = *result.value;
    const QString user = QString::fromUtf8(sessionInfo.userId);
    dialog->connectedUser_->setText(
      QString("User: %1").arg(user.isEmpty() ? "Not provided" : user));
    dialog->connectedPartner_->setText(
      QString("Partner: %1").arg(sessionInfo.partnerId));
    if (sessionInfo.expiry > 0) {
      const QDateTime expiration = QDateTime::fromSecsSinceEpoch(sessionInfo.expiry).toLocalTime();
      dialog->connectedExpiration_->setText(
        QString("Expires: %1").arg(QLocale().toString(expiration, QLocale::ShortFormat)));
    } else {
      dialog->connectedExpiration_->setText("Expires: Not provided");
    }
    dialog->validationStatus_->clear();
    dialog->validationStatus_->setVisible(false);
    dialog->connectionGroup_->setVisible(true);
    dialog->diagnostics_->appendPlainText(
      QString("\n\nResponse: success\nHTTP status: %1\nAttempts: %2\nException: none")
        .arg(result.httpStatus)
        .arg(result.attempts));
    dialog->validatedSession_ = session.toStdString();
    dialog->draftSettings_.partnerId = sessionInfo.partnerId;
    dialog->entriesGroup_->setVisible(true);
    dialog->currentPage_ = 1;
    dialog->startEntryLoad();
  });
}

void SettingsDialog::startEntryLoad()
{
  if (validatedSession_.empty()) {
    return;
  }

  const quint64 requestId = ++entryLoadRequestId_;
  liveEntries_.clear();
  entriesTable_->setRowCount(0);
  entriesStatus_->setText(QString("Loading page %1…").arg(currentPage_));
  refreshEntriesButton_->setEnabled(false);
  previousPageButton_->setEnabled(false);
  nextPageButton_->setEnabled(false);
  pageSizeCombo_->setEnabled(false);
  loadEntryPage(currentPage_, requestId);
}

void SettingsDialog::loadEntryPage(int pageIndex, quint64 requestId)
{
  const int pageSize = pageSizeCombo_->currentData().toInt();
  const std::string searchText = entrySearch_->text().trimmed().toUtf8().toStdString();
  QPointer<SettingsDialog> dialog(this);
  apiClient_.listLiveEntries(
    validatedSession_, {pageIndex, pageSize, searchText},
    [dialog, requestId](api::ApiResult<api::LiveEntryPage> result) mutable {
      if (!dialog || dialog->entryLoadRequestId_ != requestId) {
        return;
      }

      if (!result.succeeded()) {
        dialog->refreshEntriesButton_->setEnabled(true);
        dialog->pageSizeCombo_->setEnabled(true);
        dialog->previousPageButton_->setEnabled(dialog->currentPage_ > 1);
        const QString code = QString::fromUtf8(result.error->code);
        dialog->entriesStatus_->setText(
          code.isEmpty() ? "Live entries could not be loaded. Use Refresh to try again."
                         : QString("Live entries could not be loaded (%1). Use Refresh to try again.")
                             .arg(code));
        dialog->diagnostics_->appendPlainText(
          "\n\nLive-entry request:\n" +
          safeExceptionDetails(*result.error,
                               QByteArray::fromStdString(dialog->validatedSession_)));
        return;
      }

      api::LiveEntryPage &page = *result.value;
      dialog->liveEntries_ = std::move(page.entries);
      dialog->totalEntries_ = page.totalCount;

      dialog->populateEntryTable();
      dialog->refreshEntriesButton_->setEnabled(true);
      dialog->pageSizeCombo_->setEnabled(true);
      dialog->previousPageButton_->setEnabled(dialog->currentPage_ > 1);
      const int pageSize = dialog->pageSizeCombo_->currentData().toInt();
      dialog->nextPageButton_->setEnabled(dialog->currentPage_ * pageSize <
                                          dialog->totalEntries_);
      const int totalPages = std::max(1, (dialog->totalEntries_ + pageSize - 1) / pageSize);
      dialog->pageLabel_->setText(
        QString("Page %1 of %2").arg(dialog->currentPage_).arg(totalPages));
    });
}

void SettingsDialog::populateEntryTable()
{
  const QSignalBlocker tableBlocker(entriesTable_);
  entriesTable_->setSortingEnabled(false);
  entriesTable_->clearContents();
  entriesTable_->setRowCount(static_cast<int>(liveEntries_.size()));

  std::set<int> statuses;
  int selectedRow = -1;
  for (int row = 0; row < static_cast<int>(liveEntries_.size()); ++row) {
    const api::LiveEntry &entry = liveEntries_[static_cast<std::size_t>(row)];
    const QString id = QString::fromUtf8(entry.id);
    const QString name = QString::fromUtf8(entry.name);
    const QString description = QString::fromUtf8(entry.description);

    auto *thumbnailItem = new QTableWidgetItem();
    thumbnailItem->setData(Qt::UserRole, id);
    thumbnailItem->setData(Qt::UserRole + 1, entry.thumbnailUrl.toString());
    if (thumbnailCache_.contains(id)) {
      thumbnailItem->setIcon(QIcon(thumbnailCache_.value(id)));
    }

    auto *nameItem = new QTableWidgetItem(name.isEmpty() ? "Untitled" : name);
    nameItem->setData(Qt::UserRole, id);
    auto *idItem = new QTableWidgetItem(id);
    idItem->setData(Qt::UserRole, id);
    auto *descriptionItem = new QTableWidgetItem(description);
    descriptionItem->setData(Qt::UserRole, id);
    descriptionItem->setToolTip(description);
    auto *createdItem = new QTableWidgetItem();
    createdItem->setText(
      entry.createdAt > 0
        ? QDateTime::fromSecsSinceEpoch(entry.createdAt).toLocalTime().toString(
            "yyyy-MM-dd HH:mm")
        : "Unknown");
    auto *statusItem = new QTableWidgetItem(entryStatusName(entry.status));
    statusItem->setData(Qt::UserRole, entry.status);
    statusItem->setData(Qt::UserRole + 1, id);

    entriesTable_->setItem(row, ThumbnailColumn, thumbnailItem);
    entriesTable_->setItem(row, NameColumn, nameItem);
    entriesTable_->setItem(row, IdColumn, idItem);
    entriesTable_->setItem(row, DescriptionColumn, descriptionItem);
    entriesTable_->setItem(row, CreatedColumn, createdItem);
    entriesTable_->setItem(row, StatusColumn, statusItem);
    statuses.insert(entry.status);
    if (entry.id == draftSettings_.selectedEntryId) {
      selectedRow = row;
    }
  }

  const int previousStatus = entryStatusFilter_->currentData().toInt();
  {
    const QSignalBlocker filterBlocker(entryStatusFilter_);
    entryStatusFilter_->clear();
    entryStatusFilter_->addItem("All statuses", -1);
    for (int status : statuses) {
      entryStatusFilter_->addItem(entryStatusName(status), status);
    }
    const int previousIndex = entryStatusFilter_->findData(previousStatus);
    entryStatusFilter_->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
  }

  entriesTable_->setSortingEnabled(true);
  entriesTable_->sortItems(CreatedColumn, Qt::DescendingOrder);
  if (selectedRow >= 0) {
    for (int row = 0; row < entriesTable_->rowCount(); ++row) {
      if (entriesTable_->item(row, IdColumn)->text().toStdString() ==
          draftSettings_.selectedEntryId) {
        entriesTable_->selectRow(row);
        entriesTable_->scrollToItem(entriesTable_->item(row, NameColumn));
        break;
      }
    }
  }
  applyEntryFilters();
  QTimer::singleShot(0, this, [this]() { loadVisibleThumbnails(); });
}

void SettingsDialog::applyEntryFilters()
{
  if (!entriesTable_) {
    return;
  }
  const int statusFilter = entryStatusFilter_->currentData().toInt();
  int visible = 0;
  for (int row = 0; row < entriesTable_->rowCount(); ++row) {
    const bool statusMatches =
      statusFilter < 0 || entriesTable_->item(row, StatusColumn)->data(Qt::UserRole).toInt() ==
                            statusFilter;
    const bool show = statusMatches;
    entriesTable_->setRowHidden(row, !show);
    visible += show ? 1 : 0;
  }
  entriesStatus_->setText(
    QString("%1 entries shown on this page · %2 total matches")
      .arg(visible)
      .arg(totalEntries_));
  QTimer::singleShot(0, this, [this]() { loadVisibleThumbnails(); });
}

void SettingsDialog::updateSelectedEntry()
{
  const QList<QTableWidgetItem *> selection = entriesTable_->selectedItems();
  if (selection.isEmpty()) {
    return;
  }
  const int row = selection.front()->row();
  const std::string id = entriesTable_->item(row, IdColumn)->text().toUtf8().toStdString();
  const auto match = std::find_if(liveEntries_.cbegin(), liveEntries_.cend(),
                                  [&id](const api::LiveEntry &entry) {
                                    return entry.id == id;
                                  });
  if (match == liveEntries_.cend()) {
    return;
  }
  draftSettings_.selectedEntryId = match->id;
  draftSettings_.selectedEntryName = match->name;
  draftSettings_.selectedEntryDescription = match->description;
  draftSettings_.selectedEntryThumbnailUrl = match->thumbnailUrl.toString().toStdString();
  draftSettings_.selectedEntryStatus = match->status;
  configureSelectedEntry(*match);
}

void SettingsDialog::configureSelectedEntry(const api::LiveEntry &entry)
{
  if (!applyStreamSettings_ || validatedSession_.empty()) {
    return;
  }
  const quint64 requestId = ++streamConfigurationRequestId_;
  entriesStatus_->setText("Retrieving secure streaming configuration…");
  QPointer<SettingsDialog> dialog(this);
  apiClient_.getStreamConfiguration(
    validatedSession_, entry.id,
    [dialog, requestId, entry](api::ApiResult<api::StreamConfiguration> result) {
      if (!dialog || dialog->streamConfigurationRequestId_ != requestId) {
        return;
      }
      dialog->applyEntryFilters();
      if (!result.succeeded()) {
        QMessageBox::warning(
          dialog, "Streaming Configuration Unavailable",
          "Kaltura did not return a usable RTMP URL and stream key for this entry.");
        return;
      }

      const StreamingEndpoint endpoint = dialog->backupEndpoint_->isChecked()
        ? StreamingEndpoint::Backup
        : dialog->bothEndpoints_->isChecked() ? StreamingEndpoint::Both
                                               : StreamingEndpoint::Primary;
      const StreamOutputConfig primary = mapKalturaOutput(*result.value, OutputRole::Primary);
      const StreamOutputConfig backup = mapKalturaOutput(*result.value, OutputRole::Backup);
      std::string validationFailure;
      if (!validateOutputConfig(primary, validationFailure) ||
          !validateOutputConfig(backup, validationFailure)) {
        QMessageBox::warning(dialog, "Streaming Configuration Unavailable",
                             QString::fromUtf8(validationFailure));
        return;
      }

      const bool usesAuthentication = !result.value->keys.username.empty() &&
                                      !result.value->keys.password.empty();
      const QString entryName = QString::fromUtf8(entry.name).isEmpty()
                                  ? "Untitled"
                                  : QString::fromUtf8(entry.name);
      const QString endpointDetails = QString("Primary: %1 · %2\nBackup: %3 · %4")
        .arg(QString::fromLatin1(outputProtocolName(primary.protocol)),
             QString::fromUtf8(primary.endpoint),
             QString::fromLatin1(outputProtocolName(backup.protocol)),
             QString::fromUtf8(backup.endpoint));
      const QString destinationName = endpoint == StreamingEndpoint::Primary
        ? "Primary only" : endpoint == StreamingEndpoint::Backup ? "Backup only" : "Both";
      const QString confirmation =
        QString("Configure the selected Kaltura destination for this entry?\n\n"
                "Entry: %1\nEntry ID: %2\n%3\n"
                "Destination: %4\n"
                "Authentication: %5\nStream key: securely populated (hidden)\n\n"
                "Each output can be started, stopped, and edited separately in the dock.")
          .arg(entryName, QString::fromUtf8(entry.id), endpointDetails,
               destinationName, usesAuthentication ? "Required" : "Not required");
      if (QMessageBox::question(dialog, "Configure OBS Streaming", confirmation,
                                QMessageBox::Yes | QMessageBox::Cancel,
                                QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
      }

      std::string failure;
      if (!dialog->applyStreamSettings_(*result.value, endpoint, failure)) {
        QMessageBox::warning(
          dialog, "Could Not Configure OBS",
          QString::fromUtf8(failure).isEmpty() ? "OBS streaming settings could not be changed."
                                               : QString::fromUtf8(failure));
        return;
      }
      dialog->updateRevertButton();
      QMessageBox::information(
        dialog, "OBS Streaming Configured",
        "OBS is configured for the selected Kaltura entry. The stream key was populated "
        "securely and was not displayed.");
    });
}

void SettingsDialog::updateRevertButton()
{
  revertStreamSettingsButton_->setEnabled(canRevertStreamSettings_ &&
                                          canRevertStreamSettings_());
}

void SettingsDialog::loadVisibleThumbnails()
{
  if (!entriesTable_ || entriesTable_->rowCount() == 0) {
    return;
  }
  int firstRow = entriesTable_->rowAt(0);
  int lastRow = entriesTable_->rowAt(entriesTable_->viewport()->height() - 1);
  if (firstRow < 0) {
    firstRow = 0;
  }
  if (lastRow < 0) {
    lastRow = entriesTable_->rowCount() - 1;
  }

  for (int row = firstRow; row <= lastRow; ++row) {
    if (entriesTable_->isRowHidden(row)) {
      continue;
    }
    QTableWidgetItem *thumbnailItem = entriesTable_->item(row, ThumbnailColumn);
    const QString id = thumbnailItem->data(Qt::UserRole).toString();
    const QString urlText = thumbnailItem->data(Qt::UserRole + 1).toString();
    if (thumbnailCache_.contains(id)) {
      thumbnailItem->setIcon(QIcon(thumbnailCache_.value(id)));
      continue;
    }
    if (urlText.isEmpty() || thumbnailRequests_.contains(id)) {
      continue;
    }
    const QUrl url(urlText, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty()) {
      continue;
    }

    thumbnailRequests_.insert(id);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(10'000);
    QNetworkReply *reply = thumbnailNetwork_->get(request);
    reply->setProperty("entryId", id);
    reply->setProperty("entryLoadRequestId", entryLoadRequestId_);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
      const QString id = reply->property("entryId").toString();
      thumbnailRequests_.remove(id);
      if (reply->property("entryLoadRequestId").toULongLong() != entryLoadRequestId_) {
        reply->deleteLater();
        return;
      }
      const QByteArray imageData = reply->readAll();
      if (reply->error() == QNetworkReply::NoError && imageData.size() <= 5 * 1024 * 1024) {
        QPixmap image;
        if (image.loadFromData(imageData)) {
          thumbnailCache_.insert(
            id, image.scaled(96, 54, Qt::KeepAspectRatio, Qt::SmoothTransformation));
          for (int row = 0; row < entriesTable_->rowCount(); ++row) {
            QTableWidgetItem *item = entriesTable_->item(row, ThumbnailColumn);
            if (item && item->data(Qt::UserRole).toString() == id) {
              item->setIcon(QIcon(thumbnailCache_.value(id)));
            }
          }
        }
      }
      reply->deleteLater();
    });
  }
}

void SettingsDialog::setValidationBusy(bool busy)
{
  sessionEdit_->setEnabled(!busy);
  validateButton_->setEnabled(!busy);
  validateButton_->setText(busy ? "Validating…" : "Validate");
}

void SettingsDialog::clearConnectionDetails()
{
  connectionGroup_->setVisible(false);
  connectedUser_->clear();
  connectedPartner_->clear();
  connectedExpiration_->clear();
}

void SettingsDialog::validateAndAccept()
{
  if (!isValidKalturaSession(sessionEdit_->text().toUtf8().toStdString())) {
    validateSession();
    sessionEdit_->setFocus();
    QMessageBox::warning(this, "Invalid Kaltura Session",
                         "Enter a valid Kaltura Session before saving.");
    return;
  }

  std::vector<captions::CaptionDictionaryEntry> dictionary;
  QString dictionaryFailure;
  if (!collectCaptionDictionary(dictionary, dictionaryFailure)) {
    QMessageBox::warning(this, "Invalid Caption Dictionary", dictionaryFailure);
    captionDictionaryTable_->setFocus();
    return;
  }

  accept();
}

bool SettingsDialog::collectCaptionDictionary(
  std::vector<captions::CaptionDictionaryEntry> &dictionary,
  QString &failure) const
{
  dictionary.clear();
  if (captionDictionaryTable_->rowCount() > kMaximumDictionaryTerms) {
    failure = "The custom dictionary supports up to 250 terms.";
    return false;
  }
  QSet<QString> spokenForms;
  for (int row = 0; row < captionDictionaryTable_->rowCount(); ++row) {
    const QString spoken = captionDictionaryTable_->item(row, 0)
                             ? captionDictionaryTable_->item(row, 0)->text().trimmed() : QString();
    const QString preferred = captionDictionaryTable_->item(row, 1)
                                ? captionDictionaryTable_->item(row, 1)->text().trimmed() : QString();
    if (!validDictionaryField(spoken, true) || !validDictionaryField(preferred)) {
      failure = QString("Row %1 requires Preferred text. Spoken form is optional; each field "
                        "may contain no more than 128 characters.").arg(row + 1);
      return false;
    }
    if (!spoken.isEmpty()) {
      const QString duplicateKey = spoken.toCaseFolded();
      if (spokenForms.contains(duplicateKey)) {
        failure = QString("Row %1 duplicates the spoken form ‘%2’.").arg(row + 1).arg(spoken);
        return false;
      }
      spokenForms.insert(duplicateKey);
    }
    dictionary.push_back({spoken.toUtf8().toStdString(),
                          preferred.toUtf8().toStdString()});
  }
  return true;
}

void SettingsDialog::importCaptionDictionaryCsv()
{
  const QString path = QFileDialog::getOpenFileName(
    this, "Import Caption Dictionary", QString(), "CSV files (*.csv);;All files (*)");
  if (path.isEmpty()) {
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 1'000'000) {
    QMessageBox::warning(this, "Could Not Import Dictionary",
                         "The CSV could not be read or exceeds the 1 MB limit.");
    return;
  }
  QString contents = QString::fromUtf8(file.readAll());
  if (!contents.isEmpty() && contents.front() == QChar(0xfeff)) {
    contents.remove(0, 1);
  }
  QString failure;
  const QList<QStringList> rows = parseCsv(contents, failure);
  if (!failure.isEmpty() || rows.isEmpty()) {
    QMessageBox::warning(this, "Invalid Dictionary CSV",
                         failure.isEmpty() ? "The CSV is empty." : failure);
    return;
  }
  const QStringList headers = rows.front();
  int spokenColumn = -1;
  int preferredColumn = -1;
  for (int column = 0; column < headers.size(); ++column) {
    const QString header = headers[column].trimmed().toLower();
    if (header == "spoken_form") spokenColumn = column;
    if (header == "preferred_text") preferredColumn = column;
  }
  if (preferredColumn < 0) {
    QMessageBox::warning(
      this, "Invalid Dictionary CSV",
      "The required preferred_text header is missing. spoken_form is optional.");
    return;
  }
  std::vector<captions::CaptionDictionaryEntry> imported;
  QSet<QString> spokenForms;
  for (int index = 1; index < rows.size(); ++index) {
    const QStringList &fields = rows[index];
    if (fields.join(QString()).trimmed().isEmpty()) {
      continue;
    }
    if (preferredColumn >= fields.size()) {
      failure = QString("CSV row %1 does not contain preferred_text.").arg(index + 1);
      break;
    }
    const QString spoken = spokenColumn >= 0 && spokenColumn < fields.size()
                             ? fields[spokenColumn].trimmed() : QString();
    const QString preferred = fields[preferredColumn].trimmed();
    if (!validDictionaryField(spoken, true) || !validDictionaryField(preferred)) {
      failure = QString("CSV row %1 has an invalid or empty preferred_text value.").arg(index + 1);
      break;
    }
    if (!spoken.isEmpty()) {
      if (spokenForms.contains(spoken.toCaseFolded())) {
        failure = QString("CSV row %1 duplicates spoken_form ‘%2’.").arg(index + 1).arg(spoken);
        break;
      }
      spokenForms.insert(spoken.toCaseFolded());
    }
    imported.push_back({spoken.toUtf8().toStdString(), preferred.toUtf8().toStdString()});
    if (imported.size() > kMaximumDictionaryTerms) {
      failure = "The CSV contains more than 250 terms.";
      break;
    }
  }
  if (!failure.isEmpty()) {
    QMessageBox::warning(this, "Invalid Dictionary CSV", failure);
    return;
  }
  captionDictionaryTable_->setRowCount(0);
  for (const auto &entry : imported) {
    const int row = captionDictionaryTable_->rowCount();
    captionDictionaryTable_->insertRow(row);
    captionDictionaryTable_->setItem(
      row, 0, new QTableWidgetItem(QString::fromUtf8(entry.spokenForm)));
    captionDictionaryTable_->setItem(
      row, 1, new QTableWidgetItem(QString::fromUtf8(entry.preferredText)));
  }
  QMessageBox::information(this, "Dictionary Imported",
                           QString("Imported %1 custom terms.").arg(imported.size()));
}

void SettingsDialog::saveSampleCaptionDictionaryCsv()
{
  const QString path = QFileDialog::getSaveFileName(
    this, "Save Sample Caption Dictionary",
    "kaltura-caption-dictionary-sample.csv", "CSV files (*.csv)");
  if (path.isEmpty()) {
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
    QMessageBox::warning(this, "Could Not Save Sample",
                         "The sample CSV could not be written to that location.");
    return;
  }
  QTextStream stream(&file);
  stream << "spoken_form,preferred_text\n"
            "cal torah,Kaltura\n"
            "kay ess,KS\n"
            "live entree,Live Entry\n"
            ",RTMP\n";
}

void SettingsDialog::applyTheme(Theme theme)
{
  if (theme == Theme::System) {
    setPalette(systemPalette_);
    setStyleSheet(QString());
    return;
  }

  QPalette themedPalette = systemPalette_;
  if (theme == Theme::Dark) {
    themedPalette.setColor(QPalette::Window, QColor("#25262a"));
    themedPalette.setColor(QPalette::WindowText, QColor("#f1f3f5"));
    themedPalette.setColor(QPalette::Base, QColor("#17181b"));
    themedPalette.setColor(QPalette::AlternateBase, QColor("#303238"));
    themedPalette.setColor(QPalette::Text, QColor("#f1f3f5"));
    themedPalette.setColor(QPalette::Button, QColor("#35373d"));
    themedPalette.setColor(QPalette::ButtonText, QColor("#f1f3f5"));
    themedPalette.setColor(QPalette::Highlight, QColor("#6f55d9"));
    themedPalette.setColor(QPalette::HighlightedText, Qt::white);
  } else {
    themedPalette.setColor(QPalette::Window, QColor("#f5f6f8"));
    themedPalette.setColor(QPalette::WindowText, QColor("#202124"));
    themedPalette.setColor(QPalette::Base, Qt::white);
    themedPalette.setColor(QPalette::AlternateBase, QColor("#eceef2"));
    themedPalette.setColor(QPalette::Text, QColor("#202124"));
    themedPalette.setColor(QPalette::Button, QColor("#ffffff"));
    themedPalette.setColor(QPalette::ButtonText, QColor("#202124"));
    themedPalette.setColor(QPalette::Highlight, QColor("#6542d7"));
    themedPalette.setColor(QPalette::HighlightedText, Qt::white);
  }
  setPalette(themedPalette);
  setStyleSheet("QGroupBox { font-weight: 600; margin-top: 8px; } "
                "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; } "
                "QLineEdit, QComboBox { padding: 6px; } QPushButton { padding: 6px 14px; }");
}

}  // namespace kaltura_live
