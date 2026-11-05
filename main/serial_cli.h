/*
 * serial_cli.h - USB serial command interface (esp_console REPL).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wb_cli_start(void);

#ifdef __cplusplus
}
#endif
