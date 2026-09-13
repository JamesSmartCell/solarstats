#pragma once

#include "esp_err.h"

void diag_start(void);
void diag_report_error(const char *code, const char *detail);
/* POST now (caller task). Use only when about to restart and Wi-Fi is up. */
void diag_report_error_blocking(const char *code, const char *detail);
