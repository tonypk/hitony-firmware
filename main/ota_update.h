#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start OTA firmware update from HTTP URL.
 *
 * Spawns a background task that downloads the firmware, writes it to the
 * inactive OTA partition, and reboots on success. Progress is shown on LCD.
 *
 * @param url  HTTP URL of the firmware binary
 * @return true if OTA task was launched, false if already running or error
 */
bool ota_start_update(const char* url);

/**
 * @return true if an OTA update is currently in progress
 */
bool ota_is_running(void);

#ifdef __cplusplus
}
#endif
