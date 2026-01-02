#include "runtime_features.h"

#include "mc_config_store.h"

RuntimeFeatures getRuntimeFeatures() {
  RuntimeFeatures f;

  const char* wifiSsid = mcCfgWifiSsid();
  const char* ducoUser = mcCfgDucoUser();
  const char* azRegion = mcCfgAzRegion();
  const char* azKey    = mcCfgAzKey();
  const char* azVoice  = mcCfgAzVoice();

  f.wifiConfigured = wifiSsid && *wifiSsid;
  f.miningEnabled  = ducoUser && *ducoUser;
  f.ttsEnabled     = (azRegion && *azRegion) &&
                     (azKey    && *azKey) &&
                     (azVoice  && *azVoice);

  return f;
}
