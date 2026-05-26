#ifndef DISPLAY_BSP_H
#define DISPLAY_BSP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISP_H_RES 466
#define DISP_V_RES 466

/* Bring up the QSPI AMOLED panel + FT3168 touch, initialise LVGL and start
 * the LVGL handler task. Must be called once before any other LVGL call. */
void waveshare_lcd_init(void);

/* LVGL is driven from a dedicated task and is not thread-safe, so any LVGL
 * call made from another task (e.g. app_main) must be wrapped in these. */
bool lvgl_lock(int timeout_ms);
void lvgl_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_BSP_H */
