#include "kaltura_live/logger.hpp"
#include "kaltura_live/plugin.hpp"
#include "kaltura_live/version.hpp"

#include <obs-module.h>

OBS_DECLARE_MODULE()

OBS_MODULE_AUTHOR("Kaltura Live Plugin Contributors")

MODULE_EXPORT const char *obs_module_description(void)
{
  return "Kaltura Live OBS plugin";
}

namespace {
kaltura_live::Plugin g_plugin;
}

bool obs_module_load(void)
{
  kaltura_live::Logger::write(kaltura_live::LogLevel::Info,
                              "Loading Kaltura Live plugin v" KALTURA_LIVE_VERSION_STRING);

  if (!g_plugin.initialize()) {
    kaltura_live::Logger::write(kaltura_live::LogLevel::Error, "Plugin initialization failed");
    return false;
  }

  return true;
}

void obs_module_unload(void)
{
  g_plugin.shutdown();
}
