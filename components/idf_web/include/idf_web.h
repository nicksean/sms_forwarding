#ifndef IDF_WEB_H
#define IDF_WEB_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t idf_web_start(void);
void idf_web_stop(void);

#ifdef __cplusplus
}
#endif

#endif
