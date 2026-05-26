/*
 * On-flash image gallery launcher for the Waveshare ESP32-S3-Touch-AMOLED-1.43.
 *
 * Images are bundled into a read-only FAT partition ("storage") built from
 * ./flash_data at compile time and flashed with the app - no SD card needed.
 * Each folder under /flash is an "app":
 *   /flash/makeup/  (PNG files)
 *   /flash/hinge/   (PNG files)
 *
 * Boot:  animated "Cyberdeck Booting up!" loader while every image is decoded
 *        (background task), then fades into the HOME screen.
 * Home:  one rounded app icon per folder (its first image); tap to open.
 * App:   swipe LEFT / RIGHT for previous / next image; swipe UP to close (the
 *        photo shrinks back toward its icon, iPhone-style).
 *
 * Every image is decoded once at boot into a compact RGB565 bitmap in PSRAM,
 * so the home icons and the slides are all instant.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdlib.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"

#include "lvgl.h"
#include "display_bsp.h"

/* lodepng is compiled into LVGL (LV_USE_PNG). Its allocator is malloc
 * (LV_MEM_CUSTOM=y), so it is safe to call off the LVGL thread. */
extern unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                                 const unsigned char *in, size_t insize);

static const char *TAG = "gallery";

#define STORAGE_LABEL  "storage"
#define FLASH_MOUNT    "/flash"
#define MAX_IMAGES     32              /* per album */
#define SLIDE_MS       260             /* image slide animation duration */
#define PINK           0xFFB6C1
#define ICON_SZ        150             /* home-screen app icon size (px)  */

typedef struct {
    const char  *folder;
    char        *files[MAX_IMAGES];
    lv_img_dsc_t dsc[MAX_IMAGES];
    bool         have[MAX_IMAGES];
    lv_img_dsc_t thumb;                 /* ICON_SZ square icon (first image) */
    bool         have_thumb;
    int          icon_x;                /* x offset of this app's home icon  */
    int          count;
    int          index;
} album_t;

static album_t s_album[] = {
    { .folder = "makeup" },
    { .folder = "hinge"  },
};
static const int s_nalbums = sizeof(s_album) / sizeof(s_album[0]);
static int  s_active = 0;
static bool s_swipe_handled = false;
static bool s_anim_busy = false;

static lv_obj_t  *s_home_scr;
static lv_obj_t  *s_app;               /* full-screen app overlay on the home screen */
static lv_obj_t  *s_layer[2];          /* two full-screen img layers for sliding */
static int        s_front = 0;
static bool       s_app_busy = false;  /* open/close transition running */
static bool       s_app_opening = false;
static bool       s_home_swiped = false; /* this touch on home was a swipe, not a tap */

/* loading screen + progress shared with the decoder task */
static lv_obj_t  *s_load_cont;
static lv_obj_t  *s_pct_label;
static lv_timer_t *s_load_timer;
static volatile int  s_loaded = 0;
static int           s_total  = 0;
static volatile bool s_decode_done = false;

/* ------------------------------------------------------------------ scanning */

static bool has_png_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".png") == 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static void scan_album(album_t *al)
{
    char dir[64];
    snprintf(dir, sizeof(dir), FLASH_MOUNT "/%s", al->folder);
    DIR *d = opendir(dir);
    if (!d) { ESP_LOGW(TAG, "no folder %s", dir); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && al->count < MAX_IMAGES) {
        if (ent->d_name[0] == '.') continue;
        if (!has_png_ext(ent->d_name)) continue;
        al->files[al->count] = strdup(ent->d_name);
        if (!al->files[al->count]) break;
        al->count++;
    }
    closedir(d);
    qsort(al->files, al->count, sizeof(al->files[0]), cmp_name);
}

/* ------------------------------------------------------- background decoding */

static bool decode_file_to_dsc(const char *folder, const char *file, lv_img_dsc_t *out)
{
    char path[320];
    snprintf(path, sizeof(path), FLASH_MOUNT "/%s/%s", folder, file);

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    unsigned char *png = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!png) { fclose(f); return false; }
    size_t rd = fread(png, 1, sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(png); return false; }

    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&rgba, &w, &h, png, (size_t)sz);
    free(png);
    if (err || !rgba) { free(rgba); return false; }

    uint32_t px = (uint32_t)w * h;
    lv_color_t *buf = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!buf) { free(rgba); return false; }
    for (uint32_t i = 0; i < px; i++) {
        buf[i] = lv_color_make(rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2]);
    }
    free(rgba);

    out->header.always_zero = 0;
    out->header.w = w;
    out->header.h = h;
    out->header.cf = LV_IMG_CF_TRUE_COLOR;
    out->data = (const uint8_t *)buf;
    out->data_size = px * sizeof(lv_color_t);
    return true;
}

/* Nearest-neighbour downscale of an album's first image into an ICON_SZ square
 * thumbnail (plain memory ops - safe off the LVGL thread). */
static void make_thumb(album_t *al)
{
    if (!al->have[0]) return;
    const lv_color_t *src = (const lv_color_t *)al->dsc[0].data;
    uint32_t sw = al->dsc[0].header.w, sh = al->dsc[0].header.h;
    if (!src || !sw || !sh) return;
    lv_color_t *t = heap_caps_malloc((size_t)ICON_SZ * ICON_SZ * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!t) return;
    for (int y = 0; y < ICON_SZ; y++) {
        uint32_t sy = (uint32_t)y * sh / ICON_SZ;
        for (int x = 0; x < ICON_SZ; x++) {
            uint32_t sx = (uint32_t)x * sw / ICON_SZ;
            t[y * ICON_SZ + x] = src[sy * sw + sx];
        }
    }
    al->thumb.header.always_zero = 0;
    al->thumb.header.w = ICON_SZ;
    al->thumb.header.h = ICON_SZ;
    al->thumb.header.cf = LV_IMG_CF_TRUE_COLOR;
    al->thumb.data = (const uint8_t *)t;
    al->thumb.data_size = (size_t)ICON_SZ * ICON_SZ * sizeof(lv_color_t);
    al->have_thumb = true;
}

static void decoder_task(void *arg)
{
    for (int a = 0; a < s_nalbums; a++) {
        album_t *al = &s_album[a];
        for (int i = 0; i < al->count; i++) {
            if (decode_file_to_dsc(al->folder, al->files[i], &al->dsc[i])) al->have[i] = true;
            else ESP_LOGE(TAG, "decode failed: %s/%s", al->folder, al->files[i]);
            s_loaded++;
        }
        make_thumb(al);                 /* build the home-screen icon */
    }
    ESP_LOGI(TAG, "decoded %d image(s); free PSRAM: %u KB", s_loaded,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    s_decode_done = true;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------- gallery (per app) */

static void anim_x_exec(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v); }
static void anim_y_exec(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v); }
static void anim_in_done(lv_anim_t *a) { (void)a; s_anim_busy = false; }

static void start_slide(lv_obj_t *incoming, lv_obj_t *outgoing, lv_dir_t dir)
{
    bool from_positive = (dir == LV_DIR_LEFT);
    lv_coord_t span = DISP_H_RES;
    lv_coord_t in_start = from_positive ? span : -span;
    lv_coord_t out_end  = from_positive ? -span : span;

    lv_obj_set_pos(incoming, in_start, 0);
    lv_obj_clear_flag(incoming, LV_OBJ_FLAG_HIDDEN);
    s_anim_busy = true;

    lv_anim_t in;
    lv_anim_init(&in);
    lv_anim_set_var(&in, incoming);
    lv_anim_set_exec_cb(&in, anim_x_exec);
    lv_anim_set_values(&in, in_start, 0);
    lv_anim_set_time(&in, SLIDE_MS);
    lv_anim_set_path_cb(&in, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&in, anim_in_done);
    lv_anim_start(&in);

    lv_anim_t out;
    lv_anim_init(&out);
    lv_anim_set_var(&out, outgoing);
    lv_anim_set_exec_cb(&out, anim_x_exec);
    lv_anim_set_values(&out, 0, out_end);
    lv_anim_set_time(&out, SLIDE_MS);
    lv_anim_set_path_cb(&out, lv_anim_path_ease_out);
    lv_anim_start(&out);
}

static void show(int album, int idx, lv_dir_t dir)
{
    album_t *al = &s_album[album];
    if (al->count == 0) return;
    if (idx < 0)        idx = al->count - 1;
    else if (idx >= al->count) idx = 0;
    al->index = idx;
    s_active = album;

    lv_obj_t *incoming = s_layer[1 - s_front];
    lv_obj_t *outgoing = s_layer[s_front];
    if (al->have[idx]) lv_img_set_src(incoming, &al->dsc[idx]);

    if (dir == LV_DIR_NONE) {
        lv_obj_set_pos(incoming, 0, 0);
        lv_obj_clear_flag(incoming, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(outgoing, LV_OBJ_FLAG_HIDDEN);
    } else {
        start_slide(incoming, outgoing, dir);
    }
    s_front = 1 - s_front;
}

#define MINI_ZOOM    48        /* image size at the icon (~19% of full)     */
#define APP_ANIM_MS  360
#define ICON_Y       (-6)      /* must match the icon y offset on home      */

static void anim_zoom_exec(void *obj, int32_t v) { lv_img_set_zoom((lv_obj_t *)obj, (uint16_t)v); }
static void anim_opa_exec(void *obj, int32_t v)  { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

static void app_anim_done(lv_anim_t *a)
{
    lv_obj_t *img = (lv_obj_t *)a->var;
    lv_img_set_zoom(img, LV_IMG_ZOOM_NONE);          /* settle to clean full-screen */
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_style_opa(img, LV_OPA_COVER, 0);
    if (!s_app_opening) lv_obj_add_flag(s_app, LV_OBJ_FLAG_HIDDEN);  /* closed -> show home */
    s_app_busy = false;
}

/* iPhone-style open/close: the photo grows from / shrinks toward its app icon
 * while fading, revealing the home screen behind the transparent overlay. */
static void animate_app(int a, bool opening)
{
    lv_obj_t *img = s_layer[s_front];                /* the visible image */
    lv_img_set_pivot(img, DISP_H_RES / 2, DISP_V_RES / 2);
    lv_coord_t tx = s_album[a].icon_x, ty = ICON_Y;

    int32_t z0, z1, x0, x1, y0, y1, o0, o1;
    if (opening) { z0 = MINI_ZOOM; z1 = LV_IMG_ZOOM_NONE; x0 = tx; x1 = 0; y0 = ty; y1 = 0; o0 = LV_OPA_TRANSP; o1 = LV_OPA_COVER; }
    else         { z0 = LV_IMG_ZOOM_NONE; z1 = MINI_ZOOM; x0 = 0; x1 = tx; y0 = 0; y1 = ty; o0 = LV_OPA_COVER; o1 = LV_OPA_TRANSP; }

    lv_img_set_zoom(img, z0);
    lv_obj_set_pos(img, x0, y0);
    lv_obj_set_style_opa(img, o0, 0);

    s_app_opening = opening;
    s_app_busy = true;
    lv_anim_path_cb_t path = opening ? lv_anim_path_ease_out : lv_anim_path_ease_in;

    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, img);
    lv_anim_set_time(&an, APP_ANIM_MS);
    lv_anim_set_path_cb(&an, path);

    lv_anim_set_exec_cb(&an, anim_zoom_exec);
    lv_anim_set_values(&an, z0, z1);
    lv_anim_set_ready_cb(&an, app_anim_done);        /* finalize on the zoom anim only */
    lv_anim_start(&an);

    lv_anim_set_ready_cb(&an, NULL);
    lv_anim_set_exec_cb(&an, anim_x_exec);
    lv_anim_set_values(&an, x0, x1);
    lv_anim_start(&an);

    lv_anim_set_exec_cb(&an, anim_y_exec);
    lv_anim_set_values(&an, y0, y1);
    lv_anim_start(&an);

    lv_anim_set_exec_cb(&an, anim_opa_exec);
    lv_anim_set_values(&an, o0, o1);
    lv_anim_start(&an);
}

static void open_app(int a)
{
    s_active = a;
    lv_obj_clear_flag(s_app, LV_OBJ_FLAG_HIDDEN);
    show(a, 0, LV_DIR_NONE);                         /* image 0 on the visible layer */
    animate_app(a, true);
}

static void close_app(void)
{
    animate_app(s_active, false);
}

static void app_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_GESTURE:
        if (s_swipe_handled || s_anim_busy || s_app_busy) break;
        switch (lv_indev_get_gesture_dir(lv_indev_get_act())) {
        case LV_DIR_LEFT:
            show(s_active, s_album[s_active].index + 1, LV_DIR_LEFT);
            s_swipe_handled = true;
            break;
        case LV_DIR_RIGHT:
            show(s_active, s_album[s_active].index - 1, LV_DIR_RIGHT);
            s_swipe_handled = true;
            break;
        case LV_DIR_TOP:              /* swipe up -> close the app (back to home) */
            close_app();
            s_swipe_handled = true;
            break;
        default:
            break;
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        s_swipe_handled = false;
        break;
    default:
        break;
    }
}

/* The app is a transparent full-screen overlay on the home screen, so when its
 * image shrinks away on close the home screen shows through behind it. */
static void build_app(void)
{
    s_app = lv_obj_create(s_home_scr);
    lv_obj_remove_style_all(s_app);
    lv_obj_set_size(s_app, DISP_H_RES, DISP_V_RES);
    lv_obj_center(s_app);
    lv_obj_set_style_bg_opa(s_app, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_app, LV_OBJ_FLAG_SCROLLABLE);
    /* receive gestures here instead of letting them bubble up to the home screen */
    lv_obj_clear_flag(s_app, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_app, LV_OBJ_FLAG_CLICKABLE);    /* capture gestures while open */
    lv_obj_add_flag(s_app, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 2; i++) {
        s_layer[i] = lv_img_create(s_app);
        lv_obj_set_pos(s_layer[i], 0, 0);
        lv_obj_add_flag(s_layer[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_front = 0;

    lv_obj_add_event_cb(s_app, app_event_cb, LV_EVENT_ALL, NULL);
}

/* ------------------------------------------------------------- home / launcher */

/* Swipes on the home screen do nothing - just flag that this touch was a swipe
 * so the icon's click is ignored (only a real tap opens an app). */
static void home_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) s_home_swiped = true;
}

static void app_icon_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_home_swiped = false;                      /* new touch on an icon */
        break;
    case LV_EVENT_CLICKED:
        if (s_home_swiped || s_app_busy) break;     /* ignore swipes, only taps open */
        open_app((int)(intptr_t)lv_event_get_user_data(e));
        break;
    default:
        break;
    }
}

static void make_app_icon(lv_obj_t *parent, int a, lv_coord_t x)
{
    const int SZ = ICON_SZ;
    s_album[a].icon_x = x;            /* remembered for the open/close animation */
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, SZ, SZ);
    lv_obj_align(box, LV_ALIGN_CENTER, x, ICON_Y);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 26, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(PINK), 0);
    lv_obj_set_style_opa(box, LV_OPA_70, LV_STATE_PRESSED);   /* tap feedback */
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);

    if (s_album[a].have_thumb) {
        lv_obj_t *img = lv_img_create(box);
        lv_img_set_src(img, &s_album[a].thumb);   /* already ICON_SZ square */
        lv_obj_center(img);
    }

    lv_obj_t *name = lv_label_create(parent);
    lv_label_set_text(name, s_album[a].folder);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
    lv_obj_align_to(name, box, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    lv_obj_add_event_cb(box, app_icon_cb, LV_EVENT_ALL, (void *)(intptr_t)a);
}

static void build_home_scr(void)
{
    s_home_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_home_scr, lv_color_black(), 0);
    lv_obj_clear_flag(s_home_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_home_scr, home_event_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_home_scr);
    lv_label_set_text(title, "Cyberdeck");
    lv_obj_set_style_text_color(title, lv_color_hex(PINK), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 56);

    int n = 0;
    for (int a = 0; a < s_nalbums; a++) if (s_album[a].count > 0) n++;

    if (n == 0) {
        lv_obj_t *msg = lv_label_create(s_home_scr);
        lv_obj_set_style_text_color(msg, lv_color_white(), 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(msg, "No PNG images found\nin /flash/makeup or /flash/hinge");
        lv_obj_center(msg);
        return;
    }

    /* lay the icons out left-to-right, centered as a group */
    const lv_coord_t step = 200;
    lv_coord_t x = -((n - 1) * step) / 2;
    for (int a = 0; a < s_nalbums; a++) {
        if (s_album[a].count == 0) continue;
        make_app_icon(s_home_scr, a, x);
        x += step;
    }
}

/* ------------------------------------------------------------ loading screen */

/* Poll decode progress; when finished, build the screens and fade into home. */
static void load_timer_cb(lv_timer_t *t)
{
    int pct = s_total ? (s_loaded * 100 / s_total) : 100;
    if (pct > 100) pct = 100;
    if (s_pct_label) lv_label_set_text_fmt(s_pct_label, "%d%%", pct);

    if (!s_decode_done) return;

    build_home_scr();
    build_app();                  /* overlay (child of home) - build home first */
    /* fade from the loader screen to home, deleting the loader screen */
    lv_scr_load_anim(s_home_scr, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, true);

    lv_timer_del(t);
    s_load_timer = NULL;
}

static void build_loader(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    s_load_cont = lv_obj_create(scr);
    lv_obj_remove_style_all(s_load_cont);
    lv_obj_set_size(s_load_cont, DISP_H_RES, DISP_V_RES);
    lv_obj_center(s_load_cont);
    lv_obj_set_style_bg_color(s_load_cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_load_cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_load_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *spinner = lv_spinner_create(s_load_cont, 1000, 60);
    lv_obj_set_size(spinner, 150, 150);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_arc_width(spinner, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x4D343C), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(PINK), LV_PART_INDICATOR);

    s_pct_label = lv_label_create(s_load_cont);
    lv_obj_set_style_text_color(s_pct_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_pct_label, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_pct_label, "0%");
    lv_obj_align(s_pct_label, LV_ALIGN_CENTER, 0, -6);

    lv_obj_t *title = lv_label_create(s_load_cont);
    lv_obj_set_style_text_color(title, lv_color_hex(PINK), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_label_set_text(title, "Cyberdeck Booting up!");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 92);

    s_load_timer = lv_timer_create(load_timer_cb, 70, NULL);
}

static void mount_flash_fs(void)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_ro(FLASH_MOUNT, STORAGE_LABEL, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount '%s' at %s: %s",
                 STORAGE_LABEL, FLASH_MOUNT, esp_err_to_name(err));
    }
}

void app_main(void)
{
    waveshare_lcd_init();
    mount_flash_fs();

    for (int a = 0; a < s_nalbums; a++) {
        scan_album(&s_album[a]);
        s_total += s_album[a].count;
        ESP_LOGI(TAG, "folder %s: %d image(s)", s_album[a].folder, s_album[a].count);
    }

    if (lvgl_lock(-1)) {
        if (s_total > 0) {
            build_loader();              /* animated loader + progress timer */
        } else {
            build_home_scr();            /* shows the "no images" message */
            lv_scr_load(s_home_scr);
        }
        lvgl_unlock();
    }

    if (s_total > 0) {
        /* lower priority than the LVGL task so the spinner stays smooth */
        xTaskCreate(decoder_task, "img_decode", 8192, NULL, 1, NULL);
    }
}
