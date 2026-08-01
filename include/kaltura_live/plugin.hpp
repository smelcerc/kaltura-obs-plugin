#pragma once

#include "kaltura_live/api/models.hpp"
#include "kaltura_live/settings_manager.hpp"
#include "kaltura_live/streaming_manager.hpp"

#include <obs-frontend-api.h>

#include <memory>
#include <QStringList>

class QAction;
class QMainWindow;
class QTimer;
class QToolBar;

namespace kaltura_live {

namespace api {
class KalturaApiClient;
class QtHttpTransport;
}

namespace captions {
class CaptionManager;
class Cea608CaptionInserter;
}

class KalturaDock;
class SettingsDialog;

class Plugin {
public:
  Plugin();
  ~Plugin();
  bool initialize();
  void shutdown();

private:
  static void onSettingsMenuClicked(void *privateData);
  static void onDockMenuClicked(void *privateData);
  static void onFrontendSave(obs_data_t *saveData, bool saving, void *privateData);
  static void onFrontendEvent(enum obs_frontend_event event, void *privateData);

  void showSettingsDialog();
  void showDock();
  void applySettings();
  bool applyObsStreamSettings(const api::StreamConfiguration &configuration,
                              StreamingEndpoint endpoint, std::string &failure);
  bool revertObsStreamSettings(std::string &failure);
  [[nodiscard]] bool canRevertObsStreamSettings() const;
  void clearPreviousObsStreamSettings();
  void updateStreamingHealth();
  void restoreStreamingConfiguration();
  void setCaptionsEnabled(bool enabled);
  void setCaptionDelay(int delayMs);
  void setCaptionStyle(CaptionStyle style);
  void setWhisperModel(WhisperModel model);
  void updateCaptionStatus(std::string status, bool error);
  void controlPrimaryOutput(bool start);
  void controlBackupOutput(bool start);
  void updateCaptionStreamingState();
  bool startCaptionPreview();
  void stopCaptionPreview();

  QMainWindow *mainWindow_ = nullptr;
  KalturaDock *dockWidget_ = nullptr;
  QAction *settingsAction_ = nullptr;
  QAction *showDockAction_ = nullptr;
  QToolBar *toolbar_ = nullptr;
  QAction *toolbarAction_ = nullptr;
  QTimer *healthTimer_ = nullptr;

  SettingsManager settingsManager_{};
  std::shared_ptr<api::QtHttpTransport> httpTransport_;
  std::unique_ptr<api::KalturaApiClient> apiClient_;
  std::unique_ptr<StreamingManager> streamingManager_;
  std::unique_ptr<captions::CaptionManager> captionManager_;
  std::unique_ptr<captions::Cea608CaptionInserter> captionInserter_;
  std::shared_ptr<int> callbackLifetime_ = std::make_shared<int>(0);
  uint64_t restoreRequestId_ = 0;
  std::string qtPluginPath_;
  std::string whisperModelsPath_;
  std::string previousServiceId_;
  QStringList captionTranscript_;
  SettingsDialog *activeSettingsDialog_ = nullptr;
  obs_data_t *previousServiceSettings_ = nullptr;
  bool captionInsertionActive_ = false;
};

}  // namespace kaltura_live
