#include "photo_ui.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "extra/libs/fsdrv/lv_fsdrv.h"
#include "extra/libs/png/lv_png.h"
#include "extra/libs/sjpg/lv_sjpg.h"

static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_album;
static lv_obj_t *s_hint;
static lv_obj_t *s_dim_overlay;
static int s_brightness = 70;
static bool s_lv_file_decoders_ready;
static char s_photo_src[PHOTO_STORAGE_MAX_PATH + 3];

LV_FONT_DECLARE(lv_font_photo_zh_16)

static const lv_font_t *font_cjk(void)
{
    return &lv_font_photo_zh_16;
}

static void ensure_lv_file_decoders(void)
{
    if (s_lv_file_decoders_ready) {
        return;
    }

    lv_fs_stdio_init();
    lv_png_init();
    lv_split_jpeg_init();
    s_lv_file_decoders_ready = true;
}

static void create_label(lv_obj_t **label, lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    *label = lv_label_create(parent);
    lv_obj_set_style_text_font(*label, font, 0);
    lv_obj_set_style_text_color(*label, color, 0);
    lv_obj_set_style_text_letter_space(*label, 0, 0);
}

static void apply_brightness_overlay(void)
{
    if (s_dim_overlay == NULL || !lv_obj_is_valid(s_dim_overlay)) {
        return;
    }
    int brightness = s_brightness;
    if (brightness < 0) {
        brightness = 0;
    } else if (brightness > 100) {
        brightness = 100;
    }
    lv_opa_t opa = (lv_opa_t)((100 - brightness) * LV_OPA_COVER / 100);
    lv_obj_set_style_bg_opa(s_dim_overlay, opa, 0);
    lv_obj_move_foreground(s_dim_overlay);
}

static void create_brightness_overlay(lv_obj_t *parent)
{
    s_dim_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_dim_overlay);
    lv_obj_set_size(s_dim_overlay, 800, 480);
    lv_obj_align(s_dim_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_dim_overlay, lv_color_black(), 0);
    lv_obj_clear_flag(s_dim_overlay, LV_OBJ_FLAG_CLICKABLE);
    apply_brightness_overlay();
}

static void create_status_screen(const char *title, const char *status)
{
    ensure_lv_file_decoders();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    s_dim_overlay = NULL;
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111316), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 800, 480);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111316), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_center(panel);

    const lv_font_t *text_font = font_cjk();

    create_label(&s_title, panel, text_font, lv_color_hex(0xF4EFE6));
    lv_label_set_text(s_title, title != NULL ? title : "智能相框");
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 46, 42);

    create_label(&s_status, panel, text_font, lv_color_hex(0x91D6C9));
    lv_label_set_text(s_status, status != NULL ? status : "");
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 50, 110);

    lv_obj_t *line = lv_obj_create(panel);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 708, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x2E5D58), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, 46, 158);

    create_label(&s_album, panel, text_font, lv_color_hex(0xF4EFE6));
    lv_label_set_text(s_album, "等待设置");
    lv_obj_align(s_album, LV_ALIGN_TOP_LEFT, 50, 204);

    create_label(&s_hint, panel, text_font, lv_color_hex(0xB8B0A6));
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint, 700);
    lv_obj_align(s_hint, LV_ALIGN_TOP_LEFT, 50, 274);

    create_brightness_overlay(scr);
}

void photo_ui_show_boot(void)
{
    create_status_screen("智能相框", "正在启动网络");
    lv_label_set_text(s_album, "正在检查 Wi-Fi");
    lv_label_set_text(s_hint, "如果还没有保存 Wi-Fi，相框会自动进入蓝牙配网模式，请在 App 里继续操作。");
    lv_obj_align(s_hint, LV_ALIGN_TOP_LEFT, 50, 274);
}

void photo_ui_show_wifi_reconnect(int timeout_seconds)
{
    char line[384];

    create_status_screen("智能相框", "正在连接上次的 Wi-Fi");
    lv_label_set_text(s_album, "正在检查之前保存的网络");
    snprintf(line, sizeof(line),
             "如果 %d 秒内连不上之前的 Wi-Fi，相框会自动进入蓝牙配网模式。看到蓝牙名称后，请在 App 里重新配网。",
             timeout_seconds);
    lv_label_set_text(s_hint, line);
}

void photo_ui_show_provisioning(const char *device_uid, const char *prov_name, const char *pop, const char *status, bool binding_known, bool bound)
{
    char line[384];
    const char *binding_status = binding_known ? (bound ? "已绑定" : "未绑定") : "状态未知";

    create_status_screen("智能相框", status);
    snprintf(line, sizeof(line), "设备：%s", device_uid);
    lv_label_set_text(s_album, line);

    if (pop != NULL && pop[0] != '\0') {
        snprintf(line, sizeof(line),
                 "蓝牙名称：%s\n配网码：%s\n绑定状态：%s\n请打开 App，选择这台设备并输入配网码。",
                 prov_name, pop, binding_status);
    } else {
        snprintf(line, sizeof(line),
                 "蓝牙名称：%s\n绑定状态：%s\n请打开 App，选择这台设备进行配网。",
                 prov_name, binding_status);
    }
    lv_label_set_text(s_hint, line);
}

void photo_ui_show_album(const photo_album_t *album, const char *status)
{
    char line[128];

    create_status_screen("智能相框", status);

    if (!album->mounted) {
        lv_label_set_text(s_album, "未检测到 SD 卡");
        lv_label_set_text(s_hint, "相框程序已启动。请插入 FAT32 SD 卡，或等待云端同步后显示照片。");
        return;
    }

    snprintf(line, sizeof(line), "本地照片：%u 张", (unsigned)album->count);
    lv_label_set_text(s_album, line);

    if (album->count == 0) {
        lv_label_set_text(s_hint, "SD 卡已挂载。你可以等待 App 云端同步照片，或在 /photo 目录放入 800x480 图片。");
    } else {
        const char *first = strrchr(album->paths[0], '/');
        first = first == NULL ? album->paths[0] : first + 1;
        snprintf(line, sizeof(line), "第一张：%.96s", first);
        lv_label_set_text(s_hint, line);
    }
}

void photo_ui_show_local_photo(const char *path, const char *status)
{
    ensure_lv_file_decoders();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    s_dim_overlay = NULL;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    snprintf(s_photo_src, sizeof(s_photo_src), "S:%s", path);

    lv_obj_t *img = lv_img_create(scr);
    lv_img_set_src(img, s_photo_src);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_font(label, font_cjk(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xF4EFE6), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(label, 8, 0);
    lv_label_set_text(label, status != NULL ? status : "");
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 18, -18);

    create_brightness_overlay(scr);
}

void photo_ui_show_cloud(const char *device_uid, const char *status, int media_count)
{
    char line[384];

    create_status_screen("智能相框", status);
    snprintf(line, sizeof(line), "设备：%s", device_uid);
    lv_label_set_text(s_album, line);

    if (media_count < 0) {
        lv_label_set_text(s_hint, "设备未写入配置，请联系售后。App 暂时无法继续设置。");
    } else if (media_count > 0) {
        snprintf(line, sizeof(line), "云端照片：%d 张。正在按 App 中的相册和播放设置显示。", media_count);
        lv_label_set_text(s_hint, line);
    } else {
        lv_label_set_text(s_hint, "请先在 App 里绑定这台相框，然后上传照片。");
    }
}

void photo_ui_set_brightness(int brightness)
{
    if (brightness < 0) {
        brightness = 0;
    } else if (brightness > 100) {
        brightness = 100;
    }
    s_brightness = brightness;
    apply_brightness_overlay();
}
