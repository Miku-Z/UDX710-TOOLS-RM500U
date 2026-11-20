/**
 * @file usb_mode.c
 * @brief USB模式切换实现 (适配 RM500U-CN /usr/bin/usbenum.sh & /mnt/data/usbenum.ini)
 *
 * virtualcn 映射:
 *   0 = rndis  (USB_MODE_RNDIS   = 3)
 *   1 = ecm    (USB_MODE_CDC_ECM = 2)
 *   2 = ncm    (USB_MODE_CDC_NCM = 1)
 *   3 = mbim   (USB_MODE_MBIM    = 4)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "mongoose.h"
#include "usb_mode.h"
#include "http_utils.h"
#include "json_builder.h"

static int virtualcn_to_mode(int vcn) {
    switch (vcn) {
        case 0: return USB_MODE_RNDIS;
        case 1: return USB_MODE_CDC_ECM;
        case 2: return USB_MODE_CDC_NCM;
        case 3: return USB_MODE_MBIM;
        default: return -1;
    }
}

static int mode_to_virtualcn(int mode) {
    switch (mode) {
        case USB_MODE_RNDIS:   return 0;
        case USB_MODE_CDC_ECM: return 1;
        case USB_MODE_CDC_NCM: return 2;
        case USB_MODE_MBIM:    return 3;
        default: return -1;
    }
}

const char* usb_mode_name(int mode) {
    switch (mode) {
        case USB_MODE_CDC_NCM: return "cdc_ncm";
        case USB_MODE_CDC_ECM: return "cdc_ecm";
        case USB_MODE_RNDIS:   return "rndis";
        case USB_MODE_MBIM:    return "mbim";
        default: return "unknown";
    }
}

static int usb_mode_from_name(const char *name) {
    if (!name) return -1;
    if (strcmp(name, "cdc_ncm") == 0 || strcmp(name, "ncm") == 0) return USB_MODE_CDC_NCM;
    if (strcmp(name, "cdc_ecm") == 0 || strcmp(name, "ecm") == 0) return USB_MODE_CDC_ECM;
    if (strcmp(name, "rndis") == 0)   return USB_MODE_RNDIS;
    if (strcmp(name, "mbim") == 0)    return USB_MODE_MBIM;
    return -1;
}

/* 从 /mnt/data/usbenum.ini 读取 virtualcn */
int usb_mode_get(void) {
    FILE *f = fopen(USBENUM_INI_PATH, "r");
    if (!f) return -1;

    char line[256];
    int mode = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "virtualcn=", 10) == 0) {
            int vcn = atoi(line + 10);
            mode = virtualcn_to_mode(vcn);
            break;
        }
    }
    fclose(f);
    return mode;
}

/* 更新 /mnt/data/usbenum.ini 中的 virtualcn=<n>，保留其他配置项 */
int usb_mode_set(int mode, int permanent) {
    (void)permanent;
    int vcn = mode_to_virtualcn(mode);
    if (vcn < 0) {
        printf("[usb_mode] 无效的模式值: %d\n", mode);
        return -1;
    }

    char buf[4096] = {0};
    FILE *f = fopen(USBENUM_INI_PATH, "r");
    int replaced = 0;
    if (f) {
        char line[256];
        size_t pos = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "virtualcn=", 10) == 0) {
                int n = snprintf(buf + pos, sizeof(buf) - pos, "virtualcn=%d\n", vcn);
                if (n > 0 && pos + (size_t)n < sizeof(buf)) pos += (size_t)n;
                replaced = 1;
            } else {
                size_t len = strlen(line);
                if (pos + len < sizeof(buf)) {
                    memcpy(buf + pos, line, len);
                    pos += len;
                    buf[pos] = '\0';
                }
            }
        }
        fclose(f);
    }

    if (!replaced) {
        size_t pos = strlen(buf);
        snprintf(buf + pos, sizeof(buf) - pos, "virtualcn=%d\n", vcn);
    }

    f = fopen(USBENUM_INI_PATH, "w");
    if (!f) {
        printf("[usb_mode] 无法写入 %s: %s\n", USBENUM_INI_PATH, strerror(errno));
        return -1;
    }
    fputs(buf, f);
    fclose(f);
    sync();
    printf("[usb_mode] 已写入 %s: virtualcn=%d (%s)\n", USBENUM_INI_PATH, vcn, usb_mode_name(mode));
    return 0;
}

/* 从 configfs f1 软链接读取当前硬件 USB 模式 */
int usb_mode_get_current_hardware(void) {
    char target[256] = {0};
    ssize_t len = readlink(USB_CONFIG_F1, target, sizeof(target) - 1);
    if (len <= 0) return -1;
    target[len] = '\0';

    if (strstr(target, "rndis")) return USB_MODE_RNDIS;
    if (strstr(target, "ecm"))   return USB_MODE_CDC_ECM;
    if (strstr(target, "ncm"))   return USB_MODE_CDC_NCM;
    if (strstr(target, "mbim"))  return USB_MODE_MBIM;
    return -1;
}

/* GET /api/usb/mode */
void handle_usb_mode_get(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_GET(c, hm);

    int mode = usb_mode_get();
    if (mode <= 0) {
        mode = usb_mode_get_current_hardware();
    }
    if (mode <= 0) {
        mode = USB_MODE_RNDIS;
    }

    const char *mode_name = usb_mode_name(mode);

    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_int(j, "Code", 0);
    json_add_str(j, "Error", "");
    json_key_obj_open(j, "Data");
    json_add_str(j, "mode", mode_name);
    json_add_int(j, "mode_value", mode);
    json_add_bool(j, "is_temporary", 0);
    json_obj_close(j);
    json_obj_close(j);

    HTTP_OK_FREE(c, json_finish(j));
}

/* POST /api/usb/mode */
void handle_usb_mode_set(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_POST(c, hm);

    char mode_str[32] = {0};
    int permanent = 1;
    int bval = 0;

    char *mode_val = mg_json_get_str(hm->body, "$.mode");
    if (mode_val) {
        strncpy(mode_str, mode_val, sizeof(mode_str) - 1);
        free(mode_val);
    }

    if (mg_json_get_bool(hm->body, "$.permanent", &bval)) {
        permanent = bval;
    }

    if (strlen(mode_str) == 0) {
        JsonBuilder *j = json_new();
        json_obj_open(j);
        json_add_int(j, "Code", 1);
        json_add_str(j, "Error", "mode参数不能为空");
        json_add_null(j, "Data");
        json_obj_close(j);
        HTTP_OK_FREE(c, json_finish(j));
        return;
    }

    int mode = usb_mode_from_name(mode_str);
    if (mode < 0) {
        JsonBuilder *j = json_new();
        json_obj_open(j);
        json_add_int(j, "Code", 1);
        json_add_str(j, "Error", "无效的模式，支持: cdc_ncm, cdc_ecm, rndis, mbim");
        json_add_null(j, "Data");
        json_obj_close(j);
        HTTP_OK_FREE(c, json_finish(j));
        return;
    }

    if (usb_mode_set(mode, permanent) != 0) {
        JsonBuilder *j = json_new();
        json_obj_open(j);
        json_add_int(j, "Code", 1);
        json_add_str(j, "Error", "写入 /mnt/data/usbenum.ini 失败");
        json_add_null(j, "Data");
        json_obj_close(j);
        HTTP_OK_FREE(c, json_finish(j));
        return;
    }

    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_int(j, "Code", 0);
    json_add_str(j, "Error", "");
    json_key_obj_open(j, "Data");
    json_add_str(j, "mode", usb_mode_name(mode));
    json_add_bool(j, "permanent", 1);
    json_add_str(j, "message", "已写入 /mnt/data/usbenum.ini，重启模块后生效");
    json_obj_close(j);
    json_obj_close(j);

    HTTP_OK_FREE(c, json_finish(j));
}

/* POST /api/usb-advance - 禁用 UDX710 危险热切换 */
void handle_usb_advance(struct mg_connection *c, struct mg_http_message *hm) {
    HTTP_CHECK_POST(c, hm);

    JsonBuilder *j = json_new();
    json_obj_open(j);
    json_add_int(j, "Code", 1);
    json_add_str(j, "Error", "RM500U-CN 请使用标准 USB 模式保存并重启生效");
    json_add_null(j, "Data");
    json_obj_close(j);
    HTTP_OK_FREE(c, json_finish(j));
}
