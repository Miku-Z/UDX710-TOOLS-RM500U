/**
 * @file usb_mode.h
 * @brief USB模式切换头文件 (适配 RM500U-CN /mnt/data/usbenum.ini)
 */

#ifndef USB_MODE_H
#define USB_MODE_H

#include "mongoose.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB模式定义 (兼容前端 mode_value: 1=NCM, 2=ECM, 3=RNDIS, 4=MBIM) */
#define USB_MODE_CDC_NCM  1
#define USB_MODE_CDC_ECM  2
#define USB_MODE_RNDIS    3
#define USB_MODE_MBIM     4

/* RM500U-CN USB 枚举配置文件 */
#define USBENUM_INI_PATH  "/mnt/data/usbenum.ini"
#define USB_CONFIG_F1     "/sys/kernel/config/usb_gadget/g1/configs/b.1/f1"

/**
 * @brief 设置USB模式
 * @param mode 模式值 (1=CDC-NCM, 2=CDC-ECM, 3=RNDIS, 4=MBIM)
 * @param permanent 保留参数 (写入 usbenum.ini 重启后生效)
 * @return 0成功, -1失败
 */
int usb_mode_set(int mode, int permanent);

/**
 * @brief 获取当前USB模式
 * @return 模式值 (1=CDC-NCM, 2=CDC-ECM, 3=RNDIS, 4=MBIM), -1表示未设置
 */
int usb_mode_get(void);

/**
 * @brief 获取模式名称
 * @param mode 模式值
 * @return 模式名称字符串
 */
const char* usb_mode_name(int mode);

/**
 * @brief 获取当前硬件USB模式（从configfs读取）
 * @return 模式值, -1表示无法读取
 */
int usb_mode_get_current_hardware(void);

/* HTTP API处理函数 */
void handle_usb_mode_get(struct mg_connection *c, struct mg_http_message *hm);
void handle_usb_mode_set(struct mg_connection *c, struct mg_http_message *hm);
void handle_usb_advance(struct mg_connection *c, struct mg_http_message *hm);

#ifdef __cplusplus
}
#endif

#endif /* USB_MODE_H */
