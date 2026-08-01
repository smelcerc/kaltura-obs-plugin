#pragma once

#include "kaltura_live/api/models.hpp"
#include "kaltura_live/settings_manager.hpp"

#include <QDialog>
#include <QHash>
#include <QPalette>
#include <QPixmap>
#include <QSet>
#include <QStringList>

#include <functional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;
class QTimer;

namespace kaltura_live {

namespace api {
class KalturaApiClient;
}

class SettingsDialog : public QDialog {
public:
  using ApplyStreamSettings = std::function<bool(
    const api::StreamConfiguration &, StreamingEndpoint, std::string &)>;
  using RevertStreamSettings = std::function<bool(std::string &)>;
  using CanRevertStreamSettings = std::function<bool()>;

  SettingsDialog(const PluginSettings &currentSettings, api::KalturaApiClient &apiClient,
                 ApplyStreamSettings applyStreamSettings,
                 RevertStreamSettings revertStreamSettings,
                 CanRevertStreamSettings canRevertStreamSettings,
                 QWidget *parent = nullptr);

  [[nodiscard]] PluginSettings selectedSettings() const;
  void setCaptionTranscript(const QStringList &lines);
  void appendCaptionTranscript(const QString &text);

private:
  void validateSession();
  void validateAndAccept();
  void applyTheme(Theme theme);
  void setValidationBusy(bool busy);
  void clearConnectionDetails();
  void startEntryLoad();
  void loadEntryPage(int pageIndex, quint64 requestId);
  void populateEntryTable();
  void applyEntryFilters();
  void updateSelectedEntry();
  void loadVisibleThumbnails();
  void configureSelectedEntry(const api::LiveEntry &entry);
  void updateRevertButton();
  bool collectCaptionDictionary(
    std::vector<captions::CaptionDictionaryEntry> &dictionary,
    QString &failure) const;
  void importCaptionDictionaryCsv();
  void saveSampleCaptionDictionaryCsv();

  api::KalturaApiClient &apiClient_;
  QLineEdit *sessionEdit_ = nullptr;
  QPushButton *validateButton_ = nullptr;
  QLabel *validationStatus_ = nullptr;
  QGroupBox *connectionGroup_ = nullptr;
  QLabel *connectionState_ = nullptr;
  QLabel *connectedUser_ = nullptr;
  QLabel *connectedPartner_ = nullptr;
  QLabel *connectedExpiration_ = nullptr;
  QPlainTextEdit *diagnostics_ = nullptr;
  QGroupBox *entriesGroup_ = nullptr;
  QLineEdit *entrySearch_ = nullptr;
  QComboBox *entryStatusFilter_ = nullptr;
  QPushButton *refreshEntriesButton_ = nullptr;
  QPushButton *revertStreamSettingsButton_ = nullptr;
  QPushButton *previousPageButton_ = nullptr;
  QPushButton *nextPageButton_ = nullptr;
  QComboBox *pageSizeCombo_ = nullptr;
  QLabel *pageLabel_ = nullptr;
  QLabel *entriesStatus_ = nullptr;
  QTableWidget *entriesTable_ = nullptr;
  QNetworkAccessManager *thumbnailNetwork_ = nullptr;
  QHash<QString, QPixmap> thumbnailCache_;
  QSet<QString> thumbnailRequests_;
  std::vector<api::LiveEntry> liveEntries_;
  std::string validatedSession_;
  PluginSettings draftSettings_;
  ApplyStreamSettings applyStreamSettings_;
  RevertStreamSettings revertStreamSettings_;
  CanRevertStreamSettings canRevertStreamSettings_;
  QTimer *searchDebounce_ = nullptr;
  int currentPage_ = 1;
  int totalEntries_ = 0;
  QRadioButton *primaryEndpoint_ = nullptr;
  QRadioButton *backupEndpoint_ = nullptr;
  QRadioButton *bothEndpoints_ = nullptr;
  QComboBox *captionStyleCombo_ = nullptr;
  QComboBox *captionPlacementCombo_ = nullptr;
  QComboBox *captionAlignmentCombo_ = nullptr;
  QPlainTextEdit *captionTranscript_ = nullptr;
  QTableWidget *captionDictionaryTable_ = nullptr;
  QPushButton *removeDictionaryTermsButton_ = nullptr;
  QComboBox *loggingLevelCombo_ = nullptr;
  QCheckBox *debugLoggingCheck_ = nullptr;
  QComboBox *themeCombo_ = nullptr;
  QPalette systemPalette_;
  quint64 validationRequestId_ = 0;
  quint64 entryLoadRequestId_ = 0;
  quint64 streamConfigurationRequestId_ = 0;
};

}  // namespace kaltura_live
