#include "kaltura_live/plugin.hpp"

#include "kaltura_live/kaltura_dock.hpp"
#include "kaltura_live/logger.hpp"
#include "kaltura_live/platform/platform.hpp"
#include "kaltura_live/settings_dialog.hpp"
#include "kaltura_live/captions/caption_manager.hpp"
#include "kaltura_live/captions/cea608_caption_inserter.hpp"
#include "kaltura_live/captions/whisper_provider.hpp"
#include "kaltura_live/api/kaltura_api_client.hpp"
#include "kaltura_live/api/qt_http_transport.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QMainWindow>
#include <QMetaObject>
#include <QMessageBox>
#include <QSslSocket>
#include <QStringList>
#include <QTimer>

#include <algorithm>

namespace {
constexpr const char *kUsApiEndpoint = "https://www.kaltura.com/api_v3";
constexpr const char *kDockId = "kaltura_live_dock";
constexpr const char *kDockTitle = "Kaltura Live";
constexpr const char *kSettingsMenuTitle = "Kaltura Live Settings...";
constexpr const char *kOpenDockMenuTitle = "Open Kaltura Live";

}  // namespace

namespace kaltura_live {

Plugin::Plugin() = default;
Plugin::~Plugin()
{
  clearPreviousObsStreamSettings();
}

bool Plugin::initialize()
{
  if (!callbackLifetime_) {
    callbackLifetime_ = std::make_shared<int>(0);
  }
  mainWindow_ = static_cast<QMainWindow *>(obs_frontend_get_main_window());
  if (!mainWindow_) {
    Logger::write(LogLevel::Error, "Failed to acquire OBS main window");
    return false;
  }

  const char *moduleBinaryPath = obs_get_module_binary_path(obs_current_module());
  const char *moduleDataPath = obs_get_module_data_path(obs_current_module());
  platform::RuntimePaths paths = platform::runtimePaths(moduleBinaryPath, moduleDataPath);
  qtPluginPath_ = std::move(paths.qtPluginDirectory);
  whisperModelsPath_ = std::move(paths.modelDirectory);
  if (!qtPluginPath_.empty()) {
    QCoreApplication::addLibraryPath(QString::fromUtf8(qtPluginPath_));
  }

  const QStringList availableTlsBackends = QSslSocket::availableBackends();
  QString activeTlsBackend = QSslSocket::activeBackend();
  if (activeTlsBackend.isEmpty() && availableTlsBackends.contains("securetransport")) {
    if (QSslSocket::setActiveBackend("securetransport")) {
      activeTlsBackend = QSslSocket::activeBackend();
    }
  }

  if (!QSslSocket::supportsSsl()) {
    Logger::write(LogLevel::Error, "No compatible Qt TLS backend is available");
  } else {
    const QString tlsSummary =
      QString("Qt TLS ready: backend=%1, runtime=%2")
        .arg(activeTlsBackend.isEmpty() ? QSslSocket::activeBackend() : activeTlsBackend,
             QSslSocket::sslLibraryVersionString());
    Logger::write(LogLevel::Info, tlsSummary.toUtf8().toStdString());
  }

  httpTransport_ = std::make_shared<api::QtHttpTransport>();
  api::ClientConfig apiConfig;
  apiConfig.serviceUrl = QUrl(kUsApiEndpoint);
  apiClient_ = std::make_unique<api::KalturaApiClient>(httpTransport_, std::move(apiConfig));
  streamingManager_ = std::make_unique<StreamingManager>();

  dockWidget_ = new KalturaDock(
    [this](bool enabled) { setCaptionsEnabled(enabled); },
    [this](int delayMs) { setCaptionDelay(delayMs); },
    [this](CaptionStyle style) { setCaptionStyle(style); },
    [this](WhisperModel model) { setWhisperModel(model); },
    [this](bool start) { controlPrimaryOutput(start); },
    [this](bool start) { controlBackupOutput(start); },
    [this](OutputRole role, const StreamOutputConfig &config) {
      updateOutputConfiguration(role, config);
    },
    [this]() { showSettingsDialog(); }, mainWindow_);
  const bool dockAdded = obs_frontend_add_dock_by_id(kDockId, kDockTitle, dockWidget_);
  if (!dockAdded) {
    Logger::write(LogLevel::Error, "Failed to add Kaltura Live dock");
    delete dockWidget_;
    dockWidget_ = nullptr;
    return false;
  }

  const std::weak_ptr<int> captionLifetime = callbackLifetime_;
  captionInserter_ = std::make_unique<captions::Cea608CaptionInserter>(
    [this](const std::string &text, double duration,
           CaptionPlacement placement, CaptionAlignment alignment) {
      if (!streamingManager_) {
        return CaptionDeliveryResult{0, "The streaming manager is unavailable."};
      }
      return streamingManager_->sendCaption(text, duration, placement, alignment);
    },
    mainWindow_);
  captionInserter_->setDiagnosticCallback([](bool warning, std::string message) {
    Logger::write(warning ? LogLevel::Warning : LogLevel::Debug, message);
  });
  captionManager_ = std::make_unique<captions::CaptionManager>(
    std::make_unique<captions::WhisperProvider>(),
    [this, captionLifetime](std::string text) {
      if (captionLifetime.expired() || !mainWindow_) {
        return;
      }
      QMetaObject::invokeMethod(
        mainWindow_,
        [this, captionLifetime, text = std::move(text)]() {
          if (!captionLifetime.expired()) {
            const QString transcriptLine = QString::fromUtf8(text).trimmed();
            if (!transcriptLine.isEmpty()) {
              captionTranscript_.append(transcriptLine);
              while (captionTranscript_.size() > 100) {
                captionTranscript_.removeFirst();
              }
              if (activeSettingsDialog_) {
                activeSettingsDialog_->appendCaptionTranscript(transcriptLine);
              }
            }
            if (captionInserter_) {
              const bool accepted = captionInserter_->submit(std::move(text));
              if (captionInsertionActive_ && !accepted) {
                Logger::write(
                  LogLevel::Warning,
                  "A live transcript phrase was rejected before the CEA-608 queue");
              }
            }
          }
        },
        Qt::QueuedConnection);
    },
    [this, captionLifetime](std::string status, bool error) {
      if (captionLifetime.expired() || !mainWindow_) {
        return;
      }
      QMetaObject::invokeMethod(
        mainWindow_,
        [this, captionLifetime, status = std::move(status), error]() {
          if (!captionLifetime.expired()) {
            updateCaptionStatus(status, error);
          }
        },
        Qt::QueuedConnection);
    });

  settingsAction_ = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(kSettingsMenuTitle));
  if (settingsAction_) {
    QObject::connect(settingsAction_, &QAction::triggered,
                     [this]() { onSettingsMenuClicked(this); });
  }

  showDockAction_ = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(kOpenDockMenuTitle));
  if (showDockAction_) {
    QObject::connect(showDockAction_, &QAction::triggered,
                     [this]() { onDockMenuClicked(this); });
  }

  obs_frontend_add_save_callback(&Plugin::onFrontendSave, this);
  obs_frontend_add_event_callback(&Plugin::onFrontendEvent, this);

  healthTimer_ = new QTimer(mainWindow_);
  healthTimer_->setInterval(1000);
  QObject::connect(healthTimer_, &QTimer::timeout, [this]() { updateStreamingHealth(); });
  healthTimer_->start();
  updateStreamingHealth();

  Logger::write(LogLevel::Info, "Plugin initialized");
  return true;
}

void Plugin::shutdown()
{
  ++restoreRequestId_;
  callbackLifetime_.reset();
  obs_frontend_remove_event_callback(&Plugin::onFrontendEvent, this);
  obs_frontend_remove_save_callback(&Plugin::onFrontendSave, this);

  if (healthTimer_) {
    healthTimer_->stop();
    delete healthTimer_;
    healthTimer_ = nullptr;
  }
  if (captionManager_) {
    captionManager_->stop();
    captionManager_.reset();
  }
  if (captionInserter_) {
    captionInserter_->stop();
    captionInserter_.reset();
  }
  if (streamingManager_) {
    streamingManager_->shutdown();
    streamingManager_.reset();
  }

  obs_frontend_remove_dock(kDockId);
  dockWidget_ = nullptr;

  apiClient_.reset();
  httpTransport_.reset();
  clearPreviousObsStreamSettings();

  if (!qtPluginPath_.empty()) {
    QCoreApplication::removeLibraryPath(QString::fromUtf8(qtPluginPath_));
    qtPluginPath_.clear();
  }

  Logger::write(LogLevel::Info, "Plugin shutdown");
}

void Plugin::onSettingsMenuClicked(void *privateData)
{
  auto *plugin = static_cast<Plugin *>(privateData);
  if (plugin) {
    plugin->showSettingsDialog();
  }
}

void Plugin::onDockMenuClicked(void *privateData)
{
  auto *plugin = static_cast<Plugin *>(privateData);
  if (plugin) {
    plugin->showDock();
  }
}

void Plugin::onFrontendSave(obs_data_t *saveData, bool saving, void *privateData)
{
  auto *plugin = static_cast<Plugin *>(privateData);
  if (!plugin) {
    return;
  }

  if (saving) {
    plugin->settingsManager_.save(saveData);
  } else {
    plugin->settingsManager_.load(saveData);
    plugin->applySettings();
    plugin->restoreStreamingConfiguration();
  }
}

void Plugin::onFrontendEvent(enum obs_frontend_event event, void *privateData)
{
  auto *plugin = static_cast<Plugin *>(privateData);
  if (!plugin || !plugin->streamingManager_) {
    return;
  }

  switch (event) {
  case OBS_FRONTEND_EVENT_STREAMING_STARTING:
    if (plugin->dockWidget_) {
      plugin->dockWidget_->setCaptionsLocked(true);
    }
    if (plugin->captionInserter_) {
      const PluginSettings &settings = plugin->settingsManager_.settings();
      plugin->captionInserter_->configure(settings.captionsEnabled,
                                          0,
                                          settings.captionStyle,
                                          settings.captionPlacement,
                                          settings.captionAlignment);
      plugin->captionInserter_->start();
      plugin->captionInsertionActive_ = settings.captionsEnabled;
    }
    plugin->startCaptionPreview();
    plugin->streamingManager_->onStreamingStarting();
    break;
  case OBS_FRONTEND_EVENT_STREAMING_STARTED:
    plugin->streamingManager_->onStreamingStarted();
    break;
  case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
    plugin->streamingManager_->onStreamingStopping();
    break;
  case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
    plugin->streamingManager_->onStreamingStopped();
    plugin->updateCaptionStreamingState();
    if (plugin->dockWidget_ && !plugin->streamingManager_->anyOutputActive()) {
      plugin->dockWidget_->setCaptionStatus(
        plugin->settingsManager_.settings().captionsEnabled ? "Local Whisper · Listening"
                                                            : "Local Whisper · Disabled");
    }
    break;
  case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
  case OBS_FRONTEND_EVENT_EXIT:
    if (plugin->captionManager_) {
      plugin->captionManager_->stop();
    }
    if (plugin->captionInserter_) {
      plugin->captionInserter_->stop();
    }
    plugin->streamingManager_->shutdown();
    break;
  default:
    break;
  }
  plugin->updateStreamingHealth();
}

void Plugin::showSettingsDialog()
{
  if (!apiClient_) {
    Logger::write(LogLevel::Error, "Kaltura API client is unavailable");
    return;
  }

  SettingsDialog dialog(
    settingsManager_.settings(), *apiClient_,
    [this](const api::StreamConfiguration &configuration, StreamingEndpoint endpoint,
           std::string &failure) {
      return applyObsStreamSettings(configuration, endpoint, failure);
    },
    [this](std::string &failure) { return revertObsStreamSettings(failure); },
    [this]() { return canRevertObsStreamSettings(); }, mainWindow_);
  dialog.setCaptionTranscript(captionTranscript_);
  activeSettingsDialog_ = &dialog;
  const int dialogResult = dialog.exec();
  activeSettingsDialog_ = nullptr;
  if (dialogResult == QDialog::Accepted) {
    const StreamingEndpoint previousEndpoint =
      settingsManager_.settings().preferredEndpoint;
    const auto previousDictionary = settingsManager_.settings().captionDictionary;
    PluginSettings selectedSettings = dialog.selectedSettings();
    selectedSettings.primaryOutput = settingsManager_.settings().primaryOutput;
    selectedSettings.backupOutput = settingsManager_.settings().backupOutput;
    if (selectedSettings.preferredEndpoint != previousEndpoint && streamingManager_ &&
        streamingManager_->anyOutputActive()) {
      selectedSettings.preferredEndpoint = previousEndpoint;
      QMessageBox::warning(
        mainWindow_, "Stop Streaming to Change Endpoint",
        "Stop every active output before changing between Primary, Backup, and Both.");
    }
    settingsManager_.update(selectedSettings);
    const bool endpointChanged =
      settingsManager_.settings().preferredEndpoint != previousEndpoint;
    const auto &updatedDictionary = settingsManager_.settings().captionDictionary;
    const bool dictionaryChanged = previousDictionary.size() != updatedDictionary.size() ||
      !std::equal(previousDictionary.begin(), previousDictionary.end(),
                  updatedDictionary.begin(), updatedDictionary.end(),
                  [](const auto &left, const auto &right) {
                    return left.spokenForm == right.spokenForm &&
                           left.preferredText == right.preferredText;
                  });
    if (dictionaryChanged &&
        (!streamingManager_ || !streamingManager_->anyOutputActive())) {
      stopCaptionPreview();
      captionTranscript_.clear();
    }
    applySettings();
    if (endpointChanged && streamingManager_) {
      streamingManager_->clearConfiguration();
      updateStreamingHealth();
      restoreStreamingConfiguration();
    }
    obs_frontend_save();
    Logger::write(LogLevel::Info, "Settings updated from dialog");
  }
}

void Plugin::showDock()
{
  if (!dockWidget_) {
    Logger::write(LogLevel::Warning, "Dock is unavailable");
    return;
  }

  dockWidget_->show();
  dockWidget_->raise();
  dockWidget_->activateWindow();
}

void Plugin::applySettings()
{
  const PluginSettings &settings = settingsManager_.settings();
  Logger::configure(settings.loggingLevel, settings.debugLogging);
  if (streamingManager_) {
    streamingManager_->setProgramDelay(settings.captionDelayMs);
  }
  if (dockWidget_) {
    dockWidget_->setTheme(settings.theme);
    dockWidget_->setProjectSettings(settings);
    dockWidget_->setOutputConfigurations(settings.primaryOutput, settings.backupOutput);
    dockWidget_->setCaptionsEnabled(settings.captionsEnabled);
    dockWidget_->setCaptionConfiguration(settings.captionDelayMs, settings.captionStyle,
                                         settings.whisperModel);
    dockWidget_->setCaptionsLocked(streamingManager_ && streamingManager_->anyOutputActive());
  }
  if (captionInserter_) {
    captionInserter_->configure(settings.captionsEnabled, 0,
                                settings.captionStyle, settings.captionPlacement,
                                settings.captionAlignment);
  }
  if (settings.captionsEnabled) {
    startCaptionPreview();
  } else {
    stopCaptionPreview();
  }
  Logger::write(LogLevel::Debug, "Debug logging is enabled");
}

void Plugin::setCaptionsEnabled(bool enabled)
{
  if (streamingManager_ && streamingManager_->anyOutputActive()) {
    if (dockWidget_) {
      dockWidget_->setCaptionsEnabled(settingsManager_.settings().captionsEnabled);
      dockWidget_->setCaptionsLocked(true);
    }
    return;
  }
  PluginSettings updated = settingsManager_.settings();
  updated.captionsEnabled = enabled;
  settingsManager_.update(updated);
  if (captionInserter_) {
    captionInserter_->configure(updated.captionsEnabled, 0,
                                updated.captionStyle, updated.captionPlacement,
                                updated.captionAlignment);
  }
  if (dockWidget_) {
    dockWidget_->setCaptionStatus(enabled ? "Local Whisper · Starting preview…"
                                          : "Local Whisper · Disabled");
  }
  if (enabled) {
    startCaptionPreview();
  } else {
    stopCaptionPreview();
  }
  obs_frontend_save();
  updateStreamingHealth();
}

void Plugin::setCaptionDelay(int delayMs)
{
  if (streamingManager_ && streamingManager_->anyOutputActive()) {
    applySettings();
    return;
  }
  PluginSettings updated = settingsManager_.settings();
  updated.captionDelayMs = delayMs;
  settingsManager_.update(updated);
  if (streamingManager_) {
    streamingManager_->setProgramDelay(updated.captionDelayMs);
  }
  if (captionInserter_) {
    captionInserter_->configure(updated.captionsEnabled, 0,
                                updated.captionStyle, updated.captionPlacement,
                                updated.captionAlignment);
  }
  obs_frontend_save();
}

void Plugin::setCaptionStyle(CaptionStyle style)
{
  if (streamingManager_ && streamingManager_->anyOutputActive()) {
    applySettings();
    return;
  }
  PluginSettings updated = settingsManager_.settings();
  updated.captionStyle = style;
  settingsManager_.update(updated);
  if (captionInserter_) {
    captionInserter_->configure(updated.captionsEnabled, 0,
                                updated.captionStyle, updated.captionPlacement,
                                updated.captionAlignment);
  }
  obs_frontend_save();
}

void Plugin::setWhisperModel(WhisperModel model)
{
  if (streamingManager_ && streamingManager_->anyOutputActive()) {
    applySettings();
    return;
  }
  PluginSettings updated = settingsManager_.settings();
  updated.whisperModel = model;
  settingsManager_.update(updated);
  stopCaptionPreview();
  captionTranscript_.clear();
  if (activeSettingsDialog_) {
    activeSettingsDialog_->setCaptionTranscript(captionTranscript_);
  }
  startCaptionPreview();
  obs_frontend_save();
}

bool Plugin::startCaptionPreview()
{
  if (!captionManager_ || !settingsManager_.settings().captionsEnabled) {
    return false;
  }
  if (captionManager_->running()) {
    return true;
  }
  const PluginSettings &settings = settingsManager_.settings();
  captions::CaptionProviderConfig providerConfig;
  providerConfig.modelName = settings.whisperModel == WhisperModel::Base ? "Base" : "Tiny";
  providerConfig.modelPath = whisperModelsPath_ +
    (settings.whisperModel == WhisperModel::Base ? "/ggml-base.en.bin"
                                                 : "/ggml-tiny.en.bin");
  providerConfig.dictionary = settings.captionDictionary;
  if (!settings.captionDictionary.empty()) {
    providerConfig.initialPrompt = "Vocabulary: ";
    for (const auto &entry : settings.captionDictionary) {
      const size_t additional = entry.preferredText.size() + 2;
      if (providerConfig.initialPrompt.size() + additional > 2'000) {
        break;
      }
      if (providerConfig.initialPrompt.size() > std::string("Vocabulary: ").size()) {
        providerConfig.initialPrompt += ", ";
      }
      providerConfig.initialPrompt += entry.preferredText;
    }
    providerConfig.initialPrompt += '.';
  }
  std::string failure;
  if (!captionManager_->start(providerConfig, failure)) {
    updateCaptionStatus(failure.empty() ? "Caption transcript preview could not start."
                                        : failure, true);
    return false;
  }
  updateCaptionStatus("Local Whisper · Listening to program audio", false);
  return true;
}

void Plugin::stopCaptionPreview()
{
  if (captionManager_) {
    captionManager_->stop();
  }
}

void Plugin::controlPrimaryOutput(bool start)
{
  if (!streamingManager_) {
    return;
  }
  std::string failure;
  if (start && !streamingManager_->anyOutputActive() &&
      !streamingManager_->anyOutputRequested() &&
      !updateFrontendStreamSettings(
        streamingManager_->outputConfiguration(OutputRole::Primary), failure)) {
    QMessageBox::warning(mainWindow_, "Could Not Update OBS Stream Settings",
                         QString::fromUtf8(failure));
    return;
  }
  const bool succeeded = start ? streamingManager_->startPrimary(failure)
                               : streamingManager_->stopPrimary(failure);
  if (!succeeded) {
    QMessageBox::warning(mainWindow_, start ? "Could Not Start Primary"
                                            : "Could Not Stop Primary",
                         QString::fromUtf8(failure));
  }
  updateCaptionStreamingState();
  updateStreamingHealth();
}

void Plugin::controlBackupOutput(bool start)
{
  if (!streamingManager_) {
    return;
  }
  std::string failure;
  if (start && !streamingManager_->anyOutputActive() &&
      !streamingManager_->anyOutputRequested() &&
      !updateFrontendStreamSettings(
        streamingManager_->outputConfiguration(OutputRole::Backup), failure)) {
    QMessageBox::warning(mainWindow_, "Could Not Update OBS Stream Settings",
                         QString::fromUtf8(failure));
    return;
  }
  const bool succeeded = start ? streamingManager_->startBackup(failure)
                               : streamingManager_->stopBackup(failure);
  if (!succeeded) {
    QMessageBox::warning(mainWindow_, start ? "Could Not Start Backup"
                                            : "Could Not Stop Backup",
                         QString::fromUtf8(failure));
  }
  updateCaptionStreamingState();
  updateStreamingHealth();
}

void Plugin::updateOutputConfiguration(OutputRole role, const StreamOutputConfig &config)
{
  if (!streamingManager_) return;
  std::string failure;
  if (!streamingManager_->configureOutput(role, config, failure)) {
    QMessageBox::warning(mainWindow_, "Could Not Apply Output Settings",
                         QString::fromUtf8(failure));
    if (dockWidget_)
      dockWidget_->setOutputConfigurations(
        settingsManager_.settings().primaryOutput,
        settingsManager_.settings().backupOutput);
    return;
  }
  if (role == OutputRole::Primary && !updateFrontendStreamSettings(config, failure)) {
    QMessageBox::warning(mainWindow_, "Could Not Update OBS Stream Settings",
                         QString::fromUtf8(failure));
    return;
  }
  PluginSettings updated = settingsManager_.settings();
  if (role == OutputRole::Primary) updated.primaryOutput = config;
  else updated.backupOutput = config;
  if (role == OutputRole::Primary && updated.primaryOutput.protocol != OutputProtocol::SRT &&
      updated.primaryOutput.streamKey.empty())
    updated.primaryOutput.streamKey = "1";
  if (role == OutputRole::Backup && updated.backupOutput.protocol != OutputProtocol::SRT &&
      updated.backupOutput.streamKey.empty())
    updated.backupOutput.streamKey = "1";
  settingsManager_.update(updated);
  if (dockWidget_)
    dockWidget_->setOutputConfigurations(updated.primaryOutput, updated.backupOutput);
  obs_frontend_save();
  updateStreamingHealth();
}

bool Plugin::updateFrontendStreamSettings(const StreamOutputConfig &config,
                                          std::string &failure)
{
  if (!config.enabled) return true;
  StreamOutputConfig normalized = config;
  if (normalized.protocol != OutputProtocol::SRT && normalized.streamKey.empty())
    normalized.streamKey = "1";
  if (!validateOutputConfig(normalized, failure)) return false;

  if (!previousServiceSettings_) {
    obs_service_t *currentService = obs_frontend_get_streaming_service();
    if (currentService) {
      const char *serviceId = obs_service_get_id(currentService);
      if (serviceId && *serviceId) {
        previousServiceId_ = serviceId;
        previousServiceSettings_ = obs_service_get_settings(currentService);
      }
    }
  }

  obs_data_t *settings = obs_data_create();
  const std::string server = normalized.protocol == OutputProtocol::SRT
    ? buildSrtUri(normalized) : normalized.endpoint;
  const std::string key = normalized.protocol == OutputProtocol::SRT
    ? std::string{} : normalized.streamKey;
  obs_data_set_string(settings, "server", server.c_str());
  obs_data_set_string(settings, "key", key.c_str());
  const bool useAuthentication = normalized.protocol == OutputProtocol::SRT
    ? !normalized.srt.passphrase.empty()
    : (!normalized.username.empty() && !normalized.password.empty());
  obs_data_set_bool(settings, "use_auth", useAuthentication);
  obs_data_set_bool(settings, "bwtest", false);
  obs_data_set_string(settings, "username", normalized.username.c_str());
  obs_data_set_string(settings, "password",
    normalized.protocol == OutputProtocol::SRT ? normalized.srt.passphrase.c_str()
                                               : normalized.password.c_str());
  obs_service_t *service = obs_service_create(
    "rtmp_custom", "kaltura_live_primary_service", settings, nullptr);
  obs_data_release(settings);
  if (!service) {
    failure = "OBS could not create the Primary streaming service.";
    return false;
  }
  obs_frontend_set_streaming_service(service);
  obs_frontend_save_streaming_service();
  obs_service_release(service);
  Logger::write(LogLevel::Info, "OBS Stream settings synchronized to Primary " +
                std::string(outputProtocolName(normalized.protocol)) + " output");
  return true;
}

void Plugin::updateCaptionStreamingState()
{
  if (!streamingManager_) {
    return;
  }
  const bool anyOutputActive = streamingManager_->anyOutputActive();
  const bool anyOutputRequested = streamingManager_->anyOutputRequested();
  const PluginSettings &settings = settingsManager_.settings();
  if (anyOutputRequested && settings.captionsEnabled && !captionInsertionActive_) {
    if (captionInserter_) {
      captionInserter_->configure(true, 0, settings.captionStyle,
                                  settings.captionPlacement, settings.captionAlignment);
      captionInserter_->start();
      captionInsertionActive_ = true;
    }
    startCaptionPreview();
  } else if (!anyOutputRequested && captionInsertionActive_) {
    if (captionInserter_) {
      captionInserter_->stop();
    }
    captionInsertionActive_ = false;
  }
  if (dockWidget_) {
    dockWidget_->setCaptionsLocked(anyOutputActive);
  }
}

void Plugin::updateCaptionStatus(std::string status, bool error)
{
  if (captionInserter_) {
    captionInserter_->setProviderStatus(status, error);
  }
  if (dockWidget_) {
    dockWidget_->setCaptionStatus(QString::fromUtf8(status), error);
  }
  Logger::write(error ? LogLevel::Warning : LogLevel::Debug,
                error ? "Caption provider reported an error" : "Caption provider status changed");
}

void Plugin::updateStreamingHealth()
{
  updateCaptionStreamingState();
  if (dockWidget_ && streamingManager_) {
    dockWidget_->setStreamingHealth(streamingManager_->health());
    if (captionInserter_) {
      dockWidget_->setCaptionHealth(captionInserter_->health());
    }
  }
}

void Plugin::restoreStreamingConfiguration()
{
  if (!apiClient_ || !streamingManager_) {
    return;
  }
  const PluginSettings &settings = settingsManager_.settings();
  if (!settings.primaryOutput.endpoint.empty() || !settings.backupOutput.endpoint.empty()) {
    std::string failure;
    const bool primaryReady = streamingManager_->configureOutput(
      OutputRole::Primary, settings.primaryOutput, failure);
    if (!primaryReady)
      Logger::write(LogLevel::Warning, "Could not restore Primary output: " + failure);
    else if (!updateFrontendStreamSettings(settings.primaryOutput, failure))
      Logger::write(LogLevel::Warning, "Could not restore OBS Stream settings: " + failure);
    failure.clear();
    const bool backupReady = streamingManager_->configureOutput(
      OutputRole::Backup, settings.backupOutput, failure);
    if (!backupReady)
      Logger::write(LogLevel::Warning, "Could not restore Backup output: " + failure);
    updateStreamingHealth();
    return;
  }
  if (settings.kalturaSession.empty() || settings.selectedEntryId.empty()) {
    streamingManager_->clearConfiguration();
    updateStreamingHealth();
    return;
  }

  const std::weak_ptr<int> lifetime = callbackLifetime_;
  const StreamingEndpoint endpoint = settings.preferredEndpoint;
  const uint64_t requestId = ++restoreRequestId_;
  apiClient_->getStreamConfiguration(
    settings.kalturaSession, settings.selectedEntryId,
    [this, lifetime, endpoint, requestId](api::ApiResult<api::StreamConfiguration> result) {
      if (lifetime.expired() || requestId != restoreRequestId_) {
        return;
      }
      if (!result.succeeded()) {
        Logger::write(LogLevel::Warning,
                      "Could not restore Kaltura output configuration for the selected entry");
        streamingManager_->clearConfiguration();
        updateStreamingHealth();
        return;
      }
      std::string failure;
      if (!streamingManager_->configure(*result.value, endpoint, failure)) {
        Logger::write(LogLevel::Warning,
                      "Could not restore Kaltura output routing: " + failure);
      } else {
        const StreamOutputConfig primaryConfiguration =
          streamingManager_->outputConfiguration(OutputRole::Primary);
        if (!updateFrontendStreamSettings(primaryConfiguration, failure))
          Logger::write(LogLevel::Warning,
                        "Could not synchronize OBS Stream settings: " + failure);
      }
      updateStreamingHealth();
    });
}

bool Plugin::applyObsStreamSettings(const api::StreamConfiguration &configuration,
                                    StreamingEndpoint endpoint, std::string &failure)
{
  ++restoreRequestId_;
  if (streamingManager_ && streamingManager_->anyOutputActive()) {
    failure = "Stop Primary and Backup before changing OBS streaming settings.";
    return false;
  }

  if (!streamingManager_) {
    failure = "The Kaltura streaming manager is unavailable.";
    return false;
  }

  if (!streamingManager_->configure(configuration, endpoint, failure)) {
    return false;
  }
  const StreamOutputConfig primaryConfiguration =
    streamingManager_->outputConfiguration(OutputRole::Primary);
  if (!updateFrontendStreamSettings(primaryConfiguration, failure)) {
    streamingManager_->clearConfiguration();
    return false;
  }
  PluginSettings updated = settingsManager_.settings();
  updated.primaryOutput = streamingManager_->outputConfiguration(OutputRole::Primary);
  updated.backupOutput = streamingManager_->outputConfiguration(OutputRole::Backup);
  settingsManager_.update(updated);
  Logger::write(LogLevel::Info,
                "Independent Primary and Backup outputs configured for selected entry");
  updateStreamingHealth();
  return true;
}

bool Plugin::revertObsStreamSettings(std::string &failure)
{
  if (streamingManager_ && streamingManager_->anyOutputActive()) {
    failure = "Stop Primary and Backup before reverting OBS streaming settings.";
    return false;
  }
  if (!previousServiceSettings_ || previousServiceId_.empty()) {
    failure = "No previous OBS streaming settings are available.";
    return false;
  }

  obs_service_t *service = obs_service_create(previousServiceId_.c_str(),
                                              "restored_streaming_service",
                                              previousServiceSettings_, nullptr);
  if (!service) {
    failure = "OBS could not recreate the previous streaming service.";
    return false;
  }
  obs_frontend_set_streaming_service(service);
  obs_frontend_save_streaming_service();
  obs_service_release(service);
  if (streamingManager_) {
    streamingManager_->clearConfiguration();
  }
  clearPreviousObsStreamSettings();
  updateStreamingHealth();
  Logger::write(LogLevel::Info, "Previous OBS streaming service restored");
  return true;
}

bool Plugin::canRevertObsStreamSettings() const
{
  return previousServiceSettings_ != nullptr && !previousServiceId_.empty();
}

void Plugin::clearPreviousObsStreamSettings()
{
  if (previousServiceSettings_) {
    obs_data_release(previousServiceSettings_);
    previousServiceSettings_ = nullptr;
  }
  previousServiceId_.clear();
}

}  // namespace kaltura_live
