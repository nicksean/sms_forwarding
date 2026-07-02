#ifndef WEB_ASSETS_H
#define WEB_ASSETS_H

#include <Arduino.h>

struct WebAsset {
  const uint8_t* data;
  size_t length;
  const char* mime;
  const char* etag;
};

extern const char WEB_ASSET_HASH[];
extern const WebAsset WEB_INDEX;
extern const WebAsset WEB_APP_CSS;
extern const WebAsset WEB_APP_JS;

const WebAsset* findWebPanelAsset(const String& name);

#endif
