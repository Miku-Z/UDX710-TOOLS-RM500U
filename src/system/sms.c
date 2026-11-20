/**
 * @file sms.c
 * @brief 短信管理模块实现 - D-Bus监控、发送、Webhook转发
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <gio/gio.h>
#include "sms.h"
#include "database.h"
#include "exec_utils.h"

/* 短信模块专用互斥锁 */
static pthread_mutex_t g_sms_mutex = PTHREAD_MUTEX_INITIALIZER;
static GDBusConnection *g_sms_dbus_conn = NULL;
static guint g_signal_subscription_id = 0;
static guint g_state_subscription_id = 0;
static guint g_name_watch_id = 0;
static int g_sms_initialized = 0;
static int g_ofono_available = 0;
static pthread_t g_sms_worker_thread;
static volatile int g_sms_worker_running = 0;
static time_t g_last_ims_check = 0;
static int g_ims_registered = 0;

/* 发送状态追踪（真实捕获 oFono MessageState 的 Sent/Failed） */
#define MAX_PENDING_SENT 16
typedef struct {
    char uuid[128];
    int db_id;
    volatile int state; /* 0=pending, 1=sent, -1=failed */
} PendingSentSms;
static PendingSentSms g_pending_sent[MAX_PENDING_SENT];
static int g_pending_sent_idx = 0;
static pthread_mutex_t g_pending_sent_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Webhook配置 */
static WebhookConfig g_webhook_config = {0};

/* 最大短信存储数量 */
#define DEFAULT_MAX_SMS_COUNT 50
#define DEFAULT_MAX_SENT_COUNT 10
static int g_max_sms_count = DEFAULT_MAX_SMS_COUNT;
static int g_max_sent_count = DEFAULT_MAX_SENT_COUNT;

/* Webhook发送日志（内存存储，重启后清空） */
#define MAX_WEBHOOK_LOGS 30
static SmsWebhookLog g_webhook_logs[MAX_WEBHOOK_LOGS];
static int g_webhook_log_count = 0;
static int g_webhook_log_id = 0;
static pthread_mutex_t g_webhook_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 添加Webhook日志 */
static void add_webhook_log(const char *sender, const char *request, const char *response, int result);

/* 前向声明 */
static void on_incoming_message(GDBusConnection *conn, const gchar *sender_name,
    const gchar *object_path, const gchar *interface_name, const gchar *signal_name,
    GVariant *parameters, gpointer user_data);
static void on_message_state(GDBusConnection *conn, const gchar *sender_name,
    const gchar *object_path, const gchar *interface_name, const gchar *signal_name,
    GVariant *parameters, gpointer user_data);
static int save_sms_to_db(const char *sender, const char *content, time_t timestamp);
static int save_sent_sms_to_db(const char *recipient, const char *content, time_t timestamp, const char *status);
static void send_webhook_notification(const SmsMessage *msg);
static void load_sms_config(void);
static void subscribe_sms_signal(void);
static void unsubscribe_sms_signal(void);
static void on_ofono_appeared(GDBusConnection *conn, const gchar *name, const gchar *name_owner, gpointer user_data);
static void on_ofono_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data);
static void apply_sms_fix_on_init(void);
static void ensure_sms_direct_route(void);
static void ensure_ims_registered(int force);
static void sms_drain_me_storage(void);
static void *sms_worker_loop(void *arg);
static void handle_incoming_sms_common(const char *sender, const char *content, time_t now);

/* Hex解码函数 - 将hex字符串解码为原始字节 */
static void hex_decode(const char *hex, char *out, size_t out_size) {
    size_t i = 0, j = 0;
    size_t hex_len = strlen(hex);
    
    while (i < hex_len && j < out_size - 1) {
        unsigned int byte;
        if (sscanf(hex + i, "%2x", &byte) == 1) {
            out[j++] = (char)byte;
            i += 2;
        } else {
            break;
        }
    }
    out[j] = '\0';
}

/* 保存短信到数据库 */
static int save_sms_to_db(const char *sender, const char *content, time_t timestamp) {
    char sql[4096];
    char escaped_sender[128];
    char escaped_content[3072];
    
    /* 转义发件人单引号 */
    size_t sj = 0;
    for (size_t i = 0; sender[i] && sj < sizeof(escaped_sender) - 2; i++) {
        if (sender[i] == '\'') {
            escaped_sender[sj++] = '\'';
            escaped_sender[sj++] = '\'';
        } else {
            escaped_sender[sj++] = sender[i];
        }
    }
    escaped_sender[sj] = '\0';

    /* 转义内容单引号 */
    size_t j = 0;
    for (size_t i = 0; content[i] && j < sizeof(escaped_content) - 2; i++) {
        if (content[i] == '\'') {
            escaped_content[j++] = '\'';
            escaped_content[j++] = '\'';
        } else {
            escaped_content[j++] = content[i];
        }
    }
    escaped_content[j] = '\0';
    
    snprintf(sql, sizeof(sql),
        "INSERT INTO sms (sender, content, timestamp, is_read) VALUES ('%s', '%s', %ld, 0);",
        escaped_sender, escaped_content, (long)timestamp);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    /* 清理超出限制的旧短信 */
    if (ret == 0) {
        printf("[SMS] 短信保存成功，当前最大限制: %d\n", g_max_sms_count);
        snprintf(sql, sizeof(sql),
            "DELETE FROM sms WHERE id NOT IN (SELECT id FROM sms ORDER BY id DESC LIMIT %d);",
            g_max_sms_count);
        pthread_mutex_lock(&g_sms_mutex);
        db_execute(sql);
        pthread_mutex_unlock(&g_sms_mutex);
    } else {
        printf("[SMS] 短信保存失败!\n");
    }
    
    return ret;
}

/* 订阅短信信号 */
static void subscribe_sms_signal(void) {
    if (!g_sms_dbus_conn) {
        printf("[SMS] D-Bus未连接，无法订阅信号\n");
        return;
    }
    
    /* 先取消旧的订阅 */
    unsubscribe_sms_signal();
    
    /* 添加D-Bus match规则 - 与Go版本保持一致 */
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        g_sms_dbus_conn,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "AddMatch",
        g_variant_new("(s)", "type='signal',interface='org.ofono.MessageManager',member='IncomingMessage'"),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1, NULL, &error
    );
    
    if (error) {
        printf("[SMS] 添加D-Bus match规则失败: %s\n", error->message);
        g_error_free(error);
    } else {
        printf("[SMS] D-Bus match规则添加成功\n");
        if (result) g_variant_unref(result);
    }
    
    /* 订阅短信信号 */
    g_signal_subscription_id = g_dbus_connection_signal_subscribe(
        g_sms_dbus_conn,
        "org.ofono",
        "org.ofono.MessageManager",
        "IncomingMessage",
        NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_incoming_message,
        NULL, NULL
    );

    /* 订阅发送状态信号 (MessageState: Sent / Failed) */
    g_state_subscription_id = g_dbus_connection_signal_subscribe(
        g_sms_dbus_conn,
        "org.ofono",
        "org.ofono.MessageManager",
        "MessageState",
        NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_message_state,
        NULL, NULL
    );
    
    printf("[SMS] 短信信号订阅ID: %u, 状态订阅ID: %u\n", g_signal_subscription_id, g_state_subscription_id);
}

/* 取消短信信号订阅 */
static void unsubscribe_sms_signal(void) {
    if (g_signal_subscription_id > 0 && g_sms_dbus_conn) {
        g_dbus_connection_signal_unsubscribe(g_sms_dbus_conn, g_signal_subscription_id);
        printf("[SMS] 已取消信号订阅ID: %u\n", g_signal_subscription_id);
    }
    if (g_state_subscription_id > 0 && g_sms_dbus_conn) {
        g_dbus_connection_signal_unsubscribe(g_sms_dbus_conn, g_state_subscription_id);
    }
    g_signal_subscription_id = 0;
    g_state_subscription_id = 0;
}

/* oFono服务出现回调 */
static void on_ofono_appeared(GDBusConnection *conn, const gchar *name, 
    const gchar *name_owner, gpointer user_data) {
    (void)conn; (void)user_data;
    
    printf("[SMS] oFono服务已启动: %s (owner: %s)\n", name, name_owner);
    g_ofono_available = 1;
    
    /* 重新订阅短信信号，后台线程统一维护 ME 存储与 IMS 注册 */
    subscribe_sms_signal();
}

/* oFono服务消失回调 */
static void on_ofono_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    (void)conn; (void)user_data;
    
    printf("[SMS] oFono服务已停止: %s\n", name);
    g_ofono_available = 0;
    
    /* 取消信号订阅 */
    unsubscribe_sms_signal();
}

/* D-Bus连接关闭回调 */
static void on_dbus_connection_closed(GDBusConnection *conn, gboolean remote_peer_vanished,
    GError *error, gpointer user_data) {
    (void)conn; (void)user_data;
    
    printf("[SMS] D-Bus连接已关闭! remote_peer_vanished=%d, error=%s\n", 
           remote_peer_vanished, error ? error->message : "无");
    
    /* 清理状态 */
    g_signal_subscription_id = 0;
    g_state_subscription_id = 0;
    g_name_watch_id = 0;
    g_ofono_available = 0;
    g_sms_dbus_conn = NULL;
}

/* 统一入库与 5 秒去重（兼容 oFono D-Bus 推送与 ME 存储排空双通道） */
static void handle_incoming_sms_common(const char *sender, const char *content, time_t now) {
    static pthread_mutex_t dedup_lock = PTHREAD_MUTEX_INITIALIZER;
    static char last_sender[64] = {0};
    static char last_content[2048] = {0};
    static time_t last_ts = 0;

    if (!sender || !content || content[0] == '\0') return;

    pthread_mutex_lock(&dedup_lock);
    if (last_ts > 0 && (now - last_ts) >= 0 && (now - last_ts) <= 5 &&
        strcmp(last_sender, sender) == 0 &&
        strncmp(last_content, content, sizeof(last_content) - 1) == 0) {
        pthread_mutex_unlock(&dedup_lock);
        return;
    }
    strncpy(last_sender, sender, sizeof(last_sender) - 1);
    last_sender[sizeof(last_sender) - 1] = '\0';
    strncpy(last_content, content, sizeof(last_content) - 1);
    last_content[sizeof(last_content) - 1] = '\0';
    last_ts = now;
    pthread_mutex_unlock(&dedup_lock);

    printf("[SMS] 新短信入库 - 发件人: %s, 内容: %s\n", sender, content);

    if (save_sms_to_db(sender, content, now) == 0) {
        printf("[SMS] 短信已保存到系统数据库\n");

        if (g_webhook_config.enabled && strlen(g_webhook_config.url) > 0) {
            SmsMessage msg = {0};
            strncpy(msg.sender, sender, sizeof(msg.sender) - 1);
            strncpy(msg.content, content, sizeof(msg.content) - 1);
            msg.timestamp = now;
            send_webhook_notification(&msg);
        }
    }
}

/* D-Bus信号处理 - 真实捕获短信发送结果 (MessageState: Sent / Failed) */
static void on_message_state(GDBusConnection *conn, const gchar *sender_name,
    const gchar *object_path, const gchar *interface_name, const gchar *signal_name,
    GVariant *parameters, gpointer user_data) {
    (void)conn; (void)sender_name; (void)object_path;
    (void)interface_name; (void)signal_name; (void)user_data;

    if (!parameters || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(oa{sv})"))) {
        return;
    }

    const gchar *msg_path = NULL;
    GVariant *props = NULL;
    g_variant_get(parameters, "(&o@a{sv})", &msg_path, &props);
    if (!props) return;

    char uuid[128] = {0};
    char status[32] = {0};

    GVariant *v_uuid = g_variant_lookup_value(props, "UUID", G_VARIANT_TYPE_STRING);
    if (v_uuid) {
        const gchar *u = g_variant_get_string(v_uuid, NULL);
        if (u) snprintf(uuid, sizeof(uuid), "%s", u);
        g_variant_unref(v_uuid);
    } else if (msg_path) {
        const char *m = strstr(msg_path, "message_");
        if (m) snprintf(uuid, sizeof(uuid), "%s", m + 8);
    }

    GVariant *v_st = g_variant_lookup_value(props, "Status", G_VARIANT_TYPE_STRING);
    if (v_st) {
        const gchar *s = g_variant_get_string(v_st, NULL);
        if (s) snprintf(status, sizeof(status), "%s", s);
        g_variant_unref(v_st);
    }
    g_variant_unref(props);

    if (uuid[0] == '\0' || status[0] == '\0') return;

    int new_state = 0;
    const char *db_status = NULL;
    if (strcasecmp(status, "Sent") == 0) {
        new_state = 1;
        db_status = "sent";
    } else if (strcasecmp(status, "Failed") == 0 || strcasecmp(status, "Error") == 0) {
        new_state = -1;
        db_status = "failed";
    } else {
        return;
    }

    int target_db_id = -1;
    pthread_mutex_lock(&g_pending_sent_mutex);
    for (int i = 0; i < MAX_PENDING_SENT; i++) {
        if (g_pending_sent[i].uuid[0] != '\0' && strcmp(g_pending_sent[i].uuid, uuid) == 0) {
            g_pending_sent[i].state = new_state;
            target_db_id = g_pending_sent[i].db_id;
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_sent_mutex);

    if (target_db_id > 0 && db_status) {
        char sql[128];
        snprintf(sql, sizeof(sql), "UPDATE sent_sms SET status = '%s' WHERE id = %d;", db_status, target_db_id);
        pthread_mutex_lock(&g_sms_mutex);
        db_execute(sql);
        pthread_mutex_unlock(&g_sms_mutex);
    }
}

/* D-Bus信号处理 - 接收新短信 */
static void on_incoming_message(GDBusConnection *conn, const gchar *sender_name,
    const gchar *object_path, const gchar *interface_name, const gchar *signal_name,
    GVariant *parameters, gpointer user_data) {
    
    (void)conn; (void)sender_name; (void)object_path;
    (void)interface_name; (void)signal_name; (void)user_data;
    
    printf("[SMS] 收到新短信信号! path=%s\n", object_path);
    
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv})"))) {
        printf("[SMS] 短信信号参数类型不匹配\n");
        return;
    }
    
    const gchar *content = NULL;
    GVariant *props = NULL;
    g_variant_get(parameters, "(&s@a{sv})", &content, &props);
    
    if (!content || !props) {
        printf("[SMS] 解析短信内容失败\n");
        if (props) g_variant_unref(props);
        return;
    }
    
    /* 提取发送者 */
    char sender[64] = "未知";
    GVariant *sender_var = g_variant_lookup_value(props, "Sender", G_VARIANT_TYPE_STRING);
    if (sender_var) {
        const gchar *s = g_variant_get_string(sender_var, NULL);
        if (s) strncpy(sender, s, sizeof(sender) - 1);
        g_variant_unref(sender_var);
    }
    
    handle_incoming_sms_common(sender, content, time(NULL));
    g_variant_unref(props);
}

/* 发送Webhook通知 */
static void send_webhook_notification_ext(const SmsMessage *msg, int force) {
    if (!force && (!g_webhook_config.enabled || strlen(g_webhook_config.url) == 0)) {
        return;
    }
    
    if (strlen(g_webhook_config.url) == 0) {
        printf("[SMS] Webhook URL未配置\n");
        return;
    }
    
    printf("[SMS] 发送Webhook通知到: %s (force=%d)\n", g_webhook_config.url, force);
    
    /* 替换变量 */
    char body[4096];
    strncpy(body, g_webhook_config.body, sizeof(body) - 1);
    body[sizeof(body) - 1] = '\0';
    
    /* 简单的变量替换 */
    char *p;
    char temp[4096];
    
    /* 替换 #{sender} */
    while ((p = strstr(body, "#{sender}")) != NULL) {
        *p = '\0';
        snprintf(temp, sizeof(temp), "%s%s%s", body, msg->sender, p + 9);
        strncpy(body, temp, sizeof(body) - 1);
    }
    
    /* 替换 #{content} */
    while ((p = strstr(body, "#{content}")) != NULL) {
        *p = '\0';
        snprintf(temp, sizeof(temp), "%s%s%s", body, msg->content, p + 10);
        strncpy(body, temp, sizeof(body) - 1);
    }
    
    /* 替换 #{time} */
    char time_str[32];
    struct tm *tm_info = localtime(&msg->timestamp);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    while ((p = strstr(body, "#{time}")) != NULL) {
        *p = '\0';
        snprintf(temp, sizeof(temp), "%s%s%s", body, time_str, p + 7);
        strncpy(body, temp, sizeof(body) - 1);
    }
    
    /* 将body写入临时文件，避免shell转义问题 */
    const char *tmp_file = "/tmp/webhook_body.json";
    FILE *fp = fopen(tmp_file, "w");
    if (fp) {
        fputs(body, fp);
        fclose(fp);
    } else {
        printf("[SMS] 无法创建临时文件\n");
        add_webhook_log(msg->sender, body, "创建临时文件失败", 0);
        return;
    }
    
    /* 构建curl命令 */
    char cmd[8192];
    char headers_part[1024] = "";
    
    /* 解析自定义headers */
    if (strlen(g_webhook_config.headers) > 0) {
        char headers_copy[512];
        strncpy(headers_copy, g_webhook_config.headers, sizeof(headers_copy) - 1);
        headers_copy[sizeof(headers_copy) - 1] = '\0';
        
        char *line = strtok(headers_copy, "\n");
        while (line) {
            while (*line == ' ' || *line == '\r') line++;
            if (strlen(line) > 0 && strchr(line, ':')) {
                char header_arg[256];
                char *cr = strchr(line, '\r');
                if (cr) *cr = '\0';
                snprintf(header_arg, sizeof(header_arg), " -H '%s'", line);
                strncat(headers_part, header_arg, sizeof(headers_part) - strlen(headers_part) - 1);
            }
            line = strtok(NULL, "\n");
        }
    }
    
    /* 同步执行curl获取响应 */
    if (strstr(headers_part, "Content-Type") == NULL) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 10 -X POST '%s' -H 'Content-Type: application/json'%s -d @%s 2>&1",
            g_webhook_config.url, headers_part, tmp_file);
    } else {
        snprintf(cmd, sizeof(cmd),
            "curl -s --max-time 10 -X POST '%s'%s -d @%s 2>&1",
            g_webhook_config.url, headers_part, tmp_file);
    }
    
    printf("[SMS] 执行: %s\n", cmd);
    
    /* 使用popen捕获响应 */
    char response[1024] = "";
    FILE *pipe = popen(cmd, "r");
    if (pipe) {
        size_t total = 0;
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe) && total < sizeof(response) - 1) {
            size_t len = strlen(buf);
            if (total + len < sizeof(response)) {
                strcat(response, buf);
                total += len;
            }
        }
        pclose(pipe);
    } else {
        strncpy(response, "执行curl失败", sizeof(response) - 1);
    }
    
    /* 删除临时文件 */
    unlink(tmp_file);
    
    /* 判断是否成功 */
    int result = (strlen(response) > 0 && strstr(response, "curl:") == NULL) ? 1 : 0;
    
    printf("[SMS] Webhook响应: %s\n", response);
    
    /* 记录日志 */
    add_webhook_log(msg->sender, body, response, result);
}

/* 发送Webhook通知 */
static void send_webhook_notification(const SmsMessage *msg) {
    send_webhook_notification_ext(msg, 0);
}

/* 初始化短信模块 */
int sms_init(const char *db_path) {
    GError *error = NULL;
    
    if (g_sms_initialized) {
        return 0;
    }
    
    printf("[SMS] 初始化短信模块\n");
    
    /* 初始化数据库模块 */
    if (db_init(db_path) != 0) {
        printf("[SMS] 数据库初始化失败\n");
        return -1;
    }
    
    printf("[SMS] 数据库路径: %s\n", db_get_path());
    
    /* 加载配置 */
    load_sms_config();
    sms_get_webhook_config(&g_webhook_config);
    
    /* 连接D-Bus */
    g_sms_dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!g_sms_dbus_conn) {
        printf("[SMS] D-Bus连接失败: %s\n", error ? error->message : "未知错误");
        if (error) g_error_free(error);
        return -1;
    }
    
    /* 监听D-Bus连接关闭事件 */
    g_signal_connect(g_sms_dbus_conn, "closed", G_CALLBACK(on_dbus_connection_closed), NULL);
    
    /* 监控oFono服务状态 - 当服务重启时自动重新订阅 */
    g_name_watch_id = g_bus_watch_name_on_connection(
        g_sms_dbus_conn,
        "org.ofono",
        G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_ofono_appeared,
        on_ofono_vanished,
        NULL, NULL
    );
    
    printf("[SMS] oFono服务监控ID: %u\n", g_name_watch_id);
    
    /* 应用短信修复设置 - 在D-Bus连接后执行 */
    apply_sms_fix_on_init();
    
    /* 立即尝试订阅信号（如果oFono已经运行） */
    subscribe_sms_signal();
    g_ofono_available = 1;  /* 假设oFono可用，后续会通过监控更新 */

    /* 启动后台轻量轮询线程：冷启动补齐 IMS 注册(AT+IMSEN=1) + ME 短信排空入库 */
    g_sms_worker_running = 1;
    if (pthread_create(&g_sms_worker_thread, NULL, sms_worker_loop, NULL) != 0) {
        g_sms_worker_running = 0;
    }
    
    printf("[SMS] 短信模块初始化成功\n");
    g_sms_initialized = 1;
    return 0;
}

/* 关闭短信模块 */
void sms_deinit(void) {
    if (!g_sms_initialized) return;

    if (g_sms_worker_running) {
        g_sms_worker_running = 0;
        pthread_join(g_sms_worker_thread, NULL);
    }
    
    /* 取消信号订阅 */
    unsubscribe_sms_signal();
    
    /* 取消oFono服务监控 */
    if (g_name_watch_id > 0) {
        g_bus_unwatch_name(g_name_watch_id);
        g_name_watch_id = 0;
    }
    
    if (g_sms_dbus_conn) {
        g_object_unref(g_sms_dbus_conn);
        g_sms_dbus_conn = NULL;
    }
    
    g_ofono_available = 0;
    g_sms_initialized = 0;
    printf("[SMS] 短信模块已关闭\n");
}


/* 发送短信 */
int sms_send(const char *recipient, const char *content, char *result_path, size_t path_size) {
    GError *error = NULL;
    GVariant *result = NULL;
    
    if (!recipient || !content || strlen(recipient) == 0 || strlen(content) == 0) {
        printf("发送短信参数无效\n");
        return -1;
    }
    
    if (!g_sms_dbus_conn || !g_ofono_available) {
        printf("[SMS] D-Bus未连接或oFono服务不可用\n");
        return -1;
    }

    /* 5G SA 场景下发短信前若 IMS 尚未注册，立刻补发 AT+IMSEN=1 并短暂等待注册生效 */
    if (!g_ims_registered) {
        ensure_ims_registered(1);
        if (g_ims_registered) {
            usleep(300000);
        }
    }
    
    printf("[SMS] 发送短信到 %s: %s\n", recipient, content);
    
    /* 调用 org.ofono.MessageManager.SendMessage */
    result = g_dbus_connection_call_sync(
        g_sms_dbus_conn,
        "org.ofono",
        "/ril_0",
        "org.ofono.MessageManager",
        "SendMessage",
        g_variant_new("(ss)", recipient, content),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        15000,  /* 15秒超时 */
        NULL,
        &error
    );
    
    if (!result) {
        printf("[SMS] 发送短信失败: %s\n", error ? error->message : "未知错误");
        if (error) g_error_free(error);
        save_sent_sms_to_db(recipient, content, time(NULL), "failed");
        return -1;
    }
    
    /* 获取返回的消息路径与 UUID */
    const gchar *path = NULL;
    g_variant_get(result, "(&o)", &path);
    
    char path_copy[256] = {0};
    if (path) {
        snprintf(path_copy, sizeof(path_copy), "%s", path);
        if (result_path && path_size > 0) {
            snprintf(result_path, path_size, "%s", path);
        }
    }
    
    printf("[SMS] 短信已提交至基带，路径: %s\n", path_copy[0] ? path_copy : "N/A");
    g_variant_unref(result);

    /* 先记录为 pending，并等待最多 3 秒获取 oFono MessageState 真实送达/失败状态 */
    int db_id = save_sent_sms_to_db(recipient, content, time(NULL), "pending");
    const char *uuid_ptr = strstr(path_copy, "message_");
    int slot = -1;
    if (uuid_ptr && uuid_ptr[8] != '\0') {
        pthread_mutex_lock(&g_pending_sent_mutex);
        slot = g_pending_sent_idx % MAX_PENDING_SENT;
        g_pending_sent_idx++;
        snprintf(g_pending_sent[slot].uuid, sizeof(g_pending_sent[slot].uuid), "%s", uuid_ptr + 8);
        g_pending_sent[slot].db_id = db_id;
        g_pending_sent[slot].state = 0;
        pthread_mutex_unlock(&g_pending_sent_mutex);

        GMainContext *ctx = g_main_context_default();
        for (int i = 0; i < 30; i++) {
            while (g_main_context_pending(ctx)) {
                g_main_context_iteration(ctx, FALSE);
            }
            int st = 0;
            pthread_mutex_lock(&g_pending_sent_mutex);
            st = g_pending_sent[slot].state;
            pthread_mutex_unlock(&g_pending_sent_mutex);
            if (st == 1) {
                return 0; /* 运营商真实返回 Sent */
            }
            if (st == -1) {
                return -1; /* 运营商真实返回 Failed */
            }
            usleep(100000);
        }
    }
    
    return 0;
}

/* 获取短信列表 - 使用hex编码避免特殊字符问题，兼容无JSON扩展的SQLite */
int sms_get_list(SmsMessage *messages, int max_count) {
    char sql[512];
    char *output = NULL;
    
    if (!messages || max_count <= 0) return -1;
    
    /* 分配大缓冲区 */
    output = (char *)malloc(256 * 1024);
    if (!output) return -1;
    
    /* 使用hex编码content字段，用|分隔，每行一条记录 */
    snprintf(sql, sizeof(sql),
        "SELECT id || '|' || sender || '|' || hex(content) || '|' || timestamp || '|' || is_read FROM sms ORDER BY id DESC LIMIT %d;",
        max_count);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_query_string(sql, output, 256 * 1024);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret != 0 || strlen(output) == 0) {
        printf("[SMS] 获取短信列表失败或为空\n");
        free(output);
        return 0;
    }
    
    /* 解析输出 - 格式: id|sender|hex_content|timestamp|is_read\n */
    int count = 0;
    char *line = output;
    char *next_line;
    
    while (line && *line && count < max_count) {
        /* 找到行尾 */
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        
        /* 跳过空行 */
        if (strlen(line) == 0) {
            line = next_line;
            continue;
        }
        
        /* 解析字段: id|sender|hex_content|timestamp|is_read */
        char *fields[5] = {NULL};
        int field_count = 0;
        char *p = line;
        char *field_start = p;
        
        while (*p && field_count < 5) {
            if (*p == '|') {
                *p = '\0';
                fields[field_count++] = field_start;
                field_start = p + 1;
            }
            p++;
        }
        if (field_count < 5 && field_start) {
            fields[field_count++] = field_start;
        }
        
        if (field_count >= 5) {
            messages[count].id = atoi(fields[0]);
            strncpy(messages[count].sender, fields[1], sizeof(messages[count].sender) - 1);
            messages[count].sender[sizeof(messages[count].sender) - 1] = '\0';
            
            /* hex解码content */
            hex_decode(fields[2], messages[count].content, sizeof(messages[count].content));
            
            messages[count].timestamp = (time_t)atol(fields[3]);
            messages[count].is_read = atoi(fields[4]);
            count++;
        }
        
        line = next_line;
    }
    
    free(output);
    printf("[SMS] 获取到 %d 条短信\n", count);
    return count;
}

/* 获取短信总数 */
int sms_get_count(void) {
    const char *sql = "SELECT COUNT(*) FROM sms;";
    return db_query_int(sql, -1);
}

/* 删除短信 */
int sms_delete(int id) {
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM sms WHERE id = %d;", id);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    return ret;
}

/* 清空所有短信 */
int sms_clear_all(void) {
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute("DELETE FROM sms;");
    pthread_mutex_unlock(&g_sms_mutex);
    return ret;
}

/* 获取Webhook配置 */
int sms_get_webhook_config(WebhookConfig *config) {
    char output[4096];
    
    if (!config) return -1;
    
    memset(config, 0, sizeof(WebhookConfig));
    
    const char *sql = "SELECT enabled, platform, url, body, headers FROM webhook_config WHERE id = 1;";
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_query_rows(sql, "|", output, sizeof(output));
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret != 0 || strlen(output) == 0) {
        /* 使用默认配置 */
        config->enabled = 0;
        strcpy(config->platform, "pushplus");
        return 0;
    }
    
    /* 解析输出 */
    char *fields[5] = {NULL};
    int field_count = 0;
    char *p = output;
    char *start = p;
    
    while (*p && field_count < 5) {
        if (*p == '|') {
            *p = '\0';
            fields[field_count++] = start;
            start = p + 1;
        }
        p++;
    }
    if (field_count < 5 && start) {
        fields[field_count++] = start;
    }
    
    if (field_count >= 5) {
        config->enabled = atoi(fields[0]);
        strncpy(config->platform, fields[1], sizeof(config->platform) - 1);
        strncpy(config->url, fields[2], sizeof(config->url) - 1);
        strncpy(config->body, fields[3], sizeof(config->body) - 1);
        strncpy(config->headers, fields[4], sizeof(config->headers) - 1);
        
        /* 去除末尾换行符 */
        char *nl = strchr(config->headers, '\n');
        if (nl) *nl = '\0';
        
        /* 反转义特殊字符 */
        db_unescape_string(config->url);
        db_unescape_string(config->body);
        db_unescape_string(config->headers);
    }
    
    return 0;
}

/* 保存Webhook配置 */
int sms_save_webhook_config(const WebhookConfig *config) {
    char sql[8192];
    char escaped_body[4096];
    char escaped_headers[1024];
    char escaped_url[1024];
    
    if (!config) return -1;
    
    /* 转义特殊字符 */
    db_escape_string(config->body, escaped_body, sizeof(escaped_body));
    db_escape_string(config->headers, escaped_headers, sizeof(escaped_headers));
    db_escape_string(config->url, escaped_url, sizeof(escaped_url));
    
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO webhook_config (id, enabled, platform, url, body, headers) "
        "VALUES (1, %d, '%s', '%s', '%s', '%s');",
        config->enabled, config->platform, escaped_url, escaped_body, escaped_headers);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret == 0) {
        /* 更新内存中的配置 */
        memcpy(&g_webhook_config, config, sizeof(WebhookConfig));
        printf("[SMS] Webhook配置保存成功\n");
    } else {
        printf("[SMS] Webhook配置保存失败\n");
    }
    
    return ret;
}

/* 测试Webhook */
int sms_test_webhook(void) {
    SmsMessage test_msg = {
        .id = 0,
        .sender = "+8613800138000",
        .content = "这是一条测试短信",
        .timestamp = time(NULL),
        .is_read = 0
    };
    
    if (strlen(g_webhook_config.url) == 0) {
        printf("[SMS] Webhook URL为空\n");
        return -1;
    }
    
    /* 测试时强制发送，无需检查enabled状态 */
    send_webhook_notification_ext(&test_msg, 1);
    return 0;
}

/* 检查短信模块状态 */
int sms_check_status(void) {
    printf("[SMS] 状态检查 - 初始化: %d, D-Bus连接: %p, oFono可用: %d, 信号订阅: %u\n",
           g_sms_initialized, (void*)g_sms_dbus_conn, g_ofono_available, g_signal_subscription_id);
    return g_sms_initialized && g_sms_dbus_conn && g_ofono_available && g_signal_subscription_id > 0;
}

/* 定期维护短信模块 - 增强版 */
void sms_maintenance(void) {
    static int check_count = 0;
    check_count++;
    
    /* 每10次检查输出一次状态（约10秒一次，假设1秒调用一次） */
    if (check_count % 10 == 0) {
        printf("[SMS] 维护检查 #%d - D-Bus: %p, oFono: %d, 订阅ID: %u\n",
               check_count, (void*)g_sms_dbus_conn, g_ofono_available, g_signal_subscription_id);
    }
    
    if (!g_sms_initialized) {
        return;
    }
    
    /* 检查D-Bus连接是否有效 */
    if (!g_sms_dbus_conn || g_dbus_connection_is_closed(g_sms_dbus_conn)) {
        printf("[SMS] D-Bus连接无效，尝试重新连接...\n");
        GError *error = NULL;
        g_sms_dbus_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (g_sms_dbus_conn) {
            printf("[SMS] D-Bus重新连接成功\n");
            g_signal_connect(g_sms_dbus_conn, "closed", G_CALLBACK(on_dbus_connection_closed), NULL);
            subscribe_sms_signal();
        } else {
            printf("[SMS] D-Bus重新连接失败: %s\n", error ? error->message : "未知错误");
            if (error) g_error_free(error);
        }
        return;
    }
    
    /* 检查信号订阅是否丢失 */
    if (g_signal_subscription_id == 0) {
        printf("[SMS] 检测到信号订阅丢失，重新订阅...\n");
        subscribe_sms_signal();
    }

    /* 后台线程持续负责 IMS 保活与 ME 短信排空，此处仅做兜底检查 */
    if (!g_sms_worker_running) {
        ensure_sms_direct_route();
    }
}

/* ==================== 3GPP TS 23.040 SMS-DELIVER PDU 解码与长短信重组 ==================== */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t max_out) {
    size_t n = 0;
    while (hex[0] && hex[1] && n < max_out) {
        int hi = hex_nibble(hex[0]);
        int lo = hex_nibble(hex[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

static size_t utf8_append_cp(char *out, size_t pos, size_t max_out, uint32_t cp) {
    if (cp < 0x80) {
        if (pos + 1 < max_out) out[pos++] = (char)cp;
    } else if (cp < 0x800) {
        if (pos + 2 < max_out) {
            out[pos++] = (char)(0xC0 | (cp >> 6));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp < 0x10000) {
        if (pos + 3 < max_out) {
            out[pos++] = (char)(0xE0 | (cp >> 12));
            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp <= 0x10FFFF) {
        if (pos + 4 < max_out) {
            out[pos++] = (char)(0xF0 | (cp >> 18));
            out[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return pos;
}

/* GSM 7-bit 基础字符表 (3GPP TS 23.038) */
static const uint16_t k_gsm7_basic[128] = {
    0x0040,0x00A3,0x0024,0x00A5,0x00E8,0x00E9,0x00F9,0x00EC,
    0x00F2,0x00C7,0x000A,0x00D8,0x00F8,0x000D,0x00C5,0x00E5,
    0x0394,0x005F,0x03A6,0x0393,0x039B,0x03A9,0x03A0,0x03A8,
    0x03A3,0x0398,0x039E,0x001B,0x00C6,0x00E6,0x00DF,0x00C9,
    0x0020,0x0021,0x0022,0x0023,0x00A4,0x0025,0x0026,0x0027,
    0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,
    0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x00A1,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,
    0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,
    0x0058,0x0059,0x005A,0x00C4,0x00D6,0x00D1,0x00DC,0x00A7,
    0x00BF,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,
    0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,
    0x0078,0x0079,0x007A,0x00E4,0x00F6,0x00F1,0x00FC,0x00E0
};

static uint16_t gsm7_ext_cp(uint8_t c) {
    switch (c) {
        case 0x0A: return 0x000C;
        case 0x14: return '^';
        case 0x28: return '{';
        case 0x29: return '}';
        case 0x2F: return '\\';
        case 0x3C: return '[';
        case 0x3D: return '~';
        case 0x3E: return ']';
        case 0x40: return '|';
        case 0x65: return 0x20AC; /* € */
        default:   return ' ';
    }
}

static void decode_gsm7(const uint8_t *data, size_t data_len, int start_septet, int total_septets,
                        char *out, size_t max_out) {
    size_t pos = 0;
    int esc = 0;
    for (int s = start_septet; s < total_septets; s++) {
        int bit_off = s * 7;
        size_t byte_idx = (size_t)(bit_off / 8);
        int bit_idx = bit_off % 8;
        if (byte_idx >= data_len) break;
        uint16_t val = (uint16_t)(data[byte_idx] >> bit_idx);
        if (bit_idx > 1 && byte_idx + 1 < data_len) {
            val |= (uint16_t)(data[byte_idx + 1] << (8 - bit_idx));
        }
        uint8_t ch = (uint8_t)(val & 0x7F);
        if (esc) {
            pos = utf8_append_cp(out, pos, max_out, gsm7_ext_cp(ch));
            esc = 0;
        } else if (ch == 0x1B) {
            esc = 1;
        } else {
            pos = utf8_append_cp(out, pos, max_out, k_gsm7_basic[ch]);
        }
    }
    if (pos < max_out) out[pos] = '\0';
    else if (max_out > 0) out[max_out - 1] = '\0';
}

static void decode_ucs2(const uint8_t *data, size_t data_len, char *out, size_t max_out) {
    size_t pos = 0;
    for (size_t i = 0; i + 1 < data_len; i += 2) {
        uint32_t cp = ((uint32_t)data[i] << 8) | data[i + 1];
        /* 处理 UTF-16 代理对 (Emoji 等 4 字节字符) */
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < data_len) {
            uint32_t lo = ((uint32_t)data[i + 2] << 8) | data[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (lo - 0xDC00));
                i += 2;
            }
        }
        pos = utf8_append_cp(out, pos, max_out, cp);
    }
    if (pos < max_out) out[pos] = '\0';
    else if (max_out > 0) out[max_out - 1] = '\0';
}

static int bcd_swap_dec(uint8_t b) {
    int lo = b & 0x0F;
    int hi = (b >> 4) & 0x0F;
    if (lo > 9 || hi > 9) return 0;
    return lo * 10 + hi;
}

typedef struct {
    char sender[64];
    char text[1024];
    time_t timestamp;
    uint16_t concat_ref;
    uint8_t concat_total;
    uint8_t concat_seq;
} DecodedSmsPdu;

static int decode_sms_deliver_pdu(const char *hex_pdu, DecodedSmsPdu *out) {
    if (!hex_pdu || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->timestamp = time(NULL);

    uint8_t pdu[300];
    size_t pdu_len = hex_to_bytes(hex_pdu, pdu, sizeof(pdu));
    if (pdu_len < 12) return -1;

    size_t pos = 0;
    /* 1. 跳过 SMSC 中心号码 */
    uint8_t smsc_len = pdu[pos++];
    if (pos + smsc_len >= pdu_len) return -1;
    pos += smsc_len;

    /* 2. PDU-Type (MTI=0 为 SMS-DELIVER) */
    uint8_t pdu_type = pdu[pos++];
    if ((pdu_type & 0x03) != 0x00) return -1;
    int has_udh = (pdu_type & 0x40) != 0;

    /* 3. 发件人地址 OA */
    if (pos + 2 > pdu_len) return -1;
    uint8_t oa_digits = pdu[pos++];
    uint8_t oa_type = pdu[pos++];
    size_t oa_bytes = (size_t)((oa_digits + 1) / 2);
    if (pos + oa_bytes > pdu_len) return -1;

    if ((oa_type & 0x70) == 0x50) {
        /* 字母数字发件人 (GSM 7-bit) */
        int septets = (oa_digits * 4) / 7;
        decode_gsm7(pdu + pos, oa_bytes, 0, septets, out->sender, sizeof(out->sender));
    } else {
        size_t sp = 0;
        if ((oa_type & 0x70) == 0x10 && sp + 1 < sizeof(out->sender)) {
            out->sender[sp++] = '+';
        }
        static const char k_bcd_map[16] = "0123456789*#abc";
        for (size_t i = 0; i < oa_bytes && sp + 2 < sizeof(out->sender); i++) {
            uint8_t lo = pdu[pos + i] & 0x0F;
            uint8_t hi = (pdu[pos + i] >> 4) & 0x0F;
            if (lo < 15) out->sender[sp++] = k_bcd_map[lo];
            if (hi < 15) out->sender[sp++] = k_bcd_map[hi];
        }
        out->sender[sp] = '\0';
    }
    if (out->sender[0] == '\0') snprintf(out->sender, sizeof(out->sender), "未知");
    pos += oa_bytes;

    /* 4. PID & DCS */
    if (pos + 2 + 7 + 1 > pdu_len) return -1;
    pos++; /* PID */
    uint8_t dcs = pdu[pos++];

    int coding = 0; /* 0=GSM7, 1=8bit, 2=UCS2 */
    if ((dcs & 0xC0) == 0x00 || (dcs & 0xF0) == 0xF0) {
        int alpha = (dcs >> 2) & 0x03;
        if (alpha == 1) coding = 1;
        else if (alpha == 2) coding = 2;
    } else if ((dcs & 0xF0) == 0xE0) {
        coding = 2;
    }

    /* 5. SCTS 7 字节时间戳 */
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = 2000 + bcd_swap_dec(pdu[pos]) - 1900;
    tmv.tm_mon  = bcd_swap_dec(pdu[pos + 1]) - 1;
    tmv.tm_mday = bcd_swap_dec(pdu[pos + 2]);
    tmv.tm_hour = bcd_swap_dec(pdu[pos + 3]);
    tmv.tm_min  = bcd_swap_dec(pdu[pos + 4]);
    tmv.tm_sec  = bcd_swap_dec(pdu[pos + 5]);
    tmv.tm_isdst = -1;
    pos += 7;
    if (tmv.tm_mon >= 0 && tmv.tm_mon <= 11 && tmv.tm_mday >= 1 && tmv.tm_mday <= 31) {
        time_t parsed_ts = mktime(&tmv);
        if (parsed_ts > 1600000000) out->timestamp = parsed_ts;
    }

    /* 6. UDL & UD (含可选 UDH 长短信分片头) */
    uint8_t udl = pdu[pos++];
    const uint8_t *ud = pdu + pos;
    size_t ud_avail = pdu_len - pos;

    size_t udh_total_bytes = 0;
    if (has_udh && ud_avail >= 1) {
        uint8_t udhl = ud[0];
        if ((size_t)(1 + udhl) <= ud_avail) {
            udh_total_bytes = (size_t)(1 + udhl);
            size_t ipos = 1;
            while (ipos + 2 <= udh_total_bytes) {
                uint8_t iei = ud[ipos];
                uint8_t iel = ud[ipos + 1];
                if (ipos + 2 + iel > udh_total_bytes) break;
                if (iei == 0x00 && iel == 3) {
                    out->concat_ref   = ud[ipos + 2];
                    out->concat_total = ud[ipos + 3];
                    out->concat_seq   = ud[ipos + 4];
                } else if (iei == 0x08 && iel == 4) {
                    out->concat_ref   = (uint16_t)((ud[ipos + 2] << 8) | ud[ipos + 3]);
                    out->concat_total = ud[ipos + 4];
                    out->concat_seq   = ud[ipos + 5];
                }
                ipos += 2 + iel;
            }
        }
    }

    if (coding == 2) {
        size_t body_len = (udl > udh_total_bytes) ? (size_t)(udl - udh_total_bytes) : 0;
        if (udh_total_bytes + body_len > ud_avail) {
            body_len = (ud_avail > udh_total_bytes) ? (ud_avail - udh_total_bytes) : 0;
        }
        decode_ucs2(ud + udh_total_bytes, body_len, out->text, sizeof(out->text));
    } else if (coding == 1) {
        size_t body_len = (udl > udh_total_bytes) ? (size_t)(udl - udh_total_bytes) : 0;
        if (udh_total_bytes + body_len > ud_avail) {
            body_len = (ud_avail > udh_total_bytes) ? (ud_avail - udh_total_bytes) : 0;
        }
        if (body_len >= sizeof(out->text)) body_len = sizeof(out->text) - 1;
        memcpy(out->text, ud + udh_total_bytes, body_len);
        out->text[body_len] = '\0';
    } else {
        int skip_septets = (udh_total_bytes > 0) ? (int)((udh_total_bytes * 8 + 6) / 7) : 0;
        decode_gsm7(ud, ud_avail, skip_septets, (int)udl, out->text, sizeof(out->text));
    }

    return 0;
}

/* 长短信分片重组缓存 */
#define MAX_CONCAT_GROUPS 8
#define MAX_CONCAT_PARTS  12
typedef struct {
    int in_use;
    char sender[64];
    uint16_t ref;
    uint8_t total;
    uint8_t received_mask_count;
    time_t timestamp;
    time_t last_update;
    int has_part[MAX_CONCAT_PARTS];
    char parts[MAX_CONCAT_PARTS][512];
} SmsConcatGroup;

static SmsConcatGroup g_concat_groups[MAX_CONCAT_GROUPS];
static pthread_mutex_t g_concat_mutex = PTHREAD_MUTEX_INITIALIZER;

static void flush_concat_group_locked(SmsConcatGroup *g) {
    char combined[2048] = {0};
    size_t pos = 0;
    for (int i = 0; i < g->total && i < MAX_CONCAT_PARTS; i++) {
        if (g->has_part[i]) {
            size_t len = strlen(g->parts[i]);
            if (pos + len >= sizeof(combined) - 1) len = sizeof(combined) - 1 - pos;
            memcpy(combined + pos, g->parts[i], len);
            pos += len;
        }
    }
    combined[pos] = '\0';
    char sender_copy[64];
    snprintf(sender_copy, sizeof(sender_copy), "%s", g->sender);
    time_t ts = g->timestamp;
    memset(g, 0, sizeof(*g));
    if (combined[0] != '\0') {
        handle_incoming_sms_common(sender_copy, combined, ts);
    }
}

static void submit_decoded_sms_part(const DecodedSmsPdu *msg) {
    if (!msg || msg->text[0] == '\0') return;
    if (msg->concat_total <= 1 || msg->concat_total > MAX_CONCAT_PARTS ||
        msg->concat_seq < 1 || msg->concat_seq > msg->concat_total) {
        handle_incoming_sms_common(msg->sender, msg->text, msg->timestamp);
        return;
    }

    pthread_mutex_lock(&g_concat_mutex);
    int idx = -1;
    int free_idx = -1;
    for (int i = 0; i < MAX_CONCAT_GROUPS; i++) {
        if (g_concat_groups[i].in_use) {
            if (g_concat_groups[i].ref == msg->concat_ref &&
                g_concat_groups[i].total == msg->concat_total &&
                strcmp(g_concat_groups[i].sender, msg->sender) == 0) {
                idx = i;
                break;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (idx < 0) {
        idx = (free_idx >= 0) ? free_idx : 0;
        if (g_concat_groups[idx].in_use) {
            flush_concat_group_locked(&g_concat_groups[idx]);
        }
        memset(&g_concat_groups[idx], 0, sizeof(g_concat_groups[idx]));
        g_concat_groups[idx].in_use = 1;
        snprintf(g_concat_groups[idx].sender, sizeof(g_concat_groups[idx].sender), "%s", msg->sender);
        g_concat_groups[idx].ref = msg->concat_ref;
        g_concat_groups[idx].total = msg->concat_total;
        g_concat_groups[idx].timestamp = msg->timestamp;
    }

    SmsConcatGroup *g = &g_concat_groups[idx];
    int part_idx = msg->concat_seq - 1;
    if (!g->has_part[part_idx]) {
        g->has_part[part_idx] = 1;
        g->received_mask_count++;
    }
    snprintf(g->parts[part_idx], sizeof(g->parts[part_idx]), "%s", msg->text);
    g->last_update = time(NULL);

    if (g->received_mask_count >= g->total) {
        flush_concat_group_locked(g);
    }
    pthread_mutex_unlock(&g_concat_mutex);
}

static void flush_expired_concat_groups(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_concat_mutex);
    for (int i = 0; i < MAX_CONCAT_GROUPS; i++) {
        if (g_concat_groups[i].in_use && (now - g_concat_groups[i].last_update) >= 25) {
            flush_concat_group_locked(&g_concat_groups[i]);
        }
    }
    pthread_mutex_unlock(&g_concat_mutex);
}

/* ==================== 5G SA IMS 自动注册保活 (AT+IMSEN=1) ==================== */

static void ensure_ims_registered(int force) {
    extern int execute_at(const char *command, char **result);
    time_t now = time(NULL);
    if (!force && g_last_ims_check > 0 && (now - g_last_ims_check) < 30) {
        return;
    }
    g_last_ims_check = now;

    char *res = NULL;
    if (execute_at("AT+CIREG?", &res) != 0 || !res) {
        return;
    }

    int registered = (strstr(res, "+CIREG:2,1") != NULL ||
                      strstr(res, "+CIREG: 2,1") != NULL ||
                      strstr(res, "+CIREG:1,1") != NULL ||
                      strstr(res, "+CIREG: 1,1") != NULL);
    g_free(res);
    res = NULL;

    if (registered) {
        g_ims_registered = 1;
        return;
    }

    g_ims_registered = 0;
    /* 严禁调用 org.ofono.IpMultimediaSystem.Register (会崩 ofonod)；直接下发基带标准 IMS 启动命令 */
    if (execute_at("AT+CAVIMS=1", &res) == 0 && res) { g_free(res); res = NULL; }
    if (execute_at("AT+CASIMS=1", &res) == 0 && res) { g_free(res); res = NULL; }
    if (execute_at("AT+IMSEN=1", &res) == 0 && res)  { g_free(res); res = NULL; }

    /* 短暂复查注册结果 */
    usleep(500000);
    if (execute_at("AT+CIREG?", &res) == 0 && res) {
        if (strstr(res, "+CIREG:2,1") != NULL || strstr(res, "+CIREG: 2,1") != NULL ||
            strstr(res, "+CIREG:1,1") != NULL || strstr(res, "+CIREG: 1,1") != NULL) {
            g_ims_registered = 1;
        }
        g_free(res);
    }
}

/* ==================== ME 暂存 -> 系统数据库排空入库 (零 SIM 卡写入) ==================== */

/* 确保短信存储指向 ME 且 CNMI=2,1,0,0,0（实测 RM500U-CN 上 CNMI=3,2 会丢短信） */
static void ensure_sms_direct_route(void) {
    extern int execute_at(const char *command, char **result);
    if (!sms_get_fix_enabled()) return;

    char *res = NULL;
    int need_cpms = 1;
    if (execute_at("AT+CPMS?", &res) == 0 && res) {
        if (strstr(res, "+CPMS: \"ME\"") != NULL || strstr(res, "+CPMS:\"ME\"") != NULL) {
            need_cpms = 0;
        }
        g_free(res);
        res = NULL;
    }
    if (need_cpms) {
        if (execute_at("AT+CPMS=\"ME\",\"ME\",\"ME\"", &res) == 0 && res) {
            g_free(res);
            res = NULL;
        }
    }

    int need_cnmi = 1;
    if (execute_at("AT+CNMI?", &res) == 0 && res) {
        if (strstr(res, "+CNMI: 2,1,0,0,0") != NULL || strstr(res, "+CNMI:2,1,0,0,0") != NULL) {
            need_cnmi = 0;
        }
        g_free(res);
        res = NULL;
    }
    if (need_cnmi) {
        if (execute_at("AT+CSMS=0", &res) == 0 && res) { g_free(res); res = NULL; }
        if (execute_at("AT+CNMI=2,1,0,0,0", &res) == 0 && res) { g_free(res); res = NULL; }
    }
}

static void sms_drain_me_storage(void) {
    extern int execute_at(const char *command, char **result);
    if (!sms_get_fix_enabled()) return;

    char *res = NULL;
    if (execute_at("AT+CPMS?", &res) != 0 || !res) {
        return;
    }

    int is_me = (strstr(res, "+CPMS: \"ME\"") != NULL || strstr(res, "+CPMS:\"ME\"") != NULL);
    int used = 0, total = 0;
    const char *p = strstr(res, "+CPMS:");
    if (p) {
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        char m1[16] = {0};
        if (sscanf(p, "\"%15[^\"]\",%d,%d", m1, &used, &total) == 3) {
            is_me = (strcmp(m1, "ME") == 0);
        }
    }
    g_free(res);
    res = NULL;

    if (!is_me) {
        ensure_sms_direct_route();
        return;
    }

    if (used <= 0) {
        flush_expired_concat_groups();
        return;
    }

    if (total <= 0 || total > 120) total = 100;
    int found = 0;

    for (int idx = 1; idx <= total && found < used && g_sms_worker_running; idx++) {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idx);
        if (execute_at(cmd, &res) != 0 || !res) {
            continue;
        }

        const char *cmgr = strstr(res, "+CMGR:");
        if (cmgr) {
            found++;
            /* 查找 +CMGR 行后的 PDU 十六进制行 */
            const char *nl = strpbrk(cmgr, "\r\n");
            if (nl) {
                nl++;
                while (*nl == '\r' || *nl == '\n' || *nl == ' ') nl++;
                char pdu_hex[640] = {0};
                size_t k = 0;
                while (nl[k] && isxdigit((unsigned char)nl[k]) && k < sizeof(pdu_hex) - 1) {
                    pdu_hex[k] = nl[k];
                    k++;
                }
                pdu_hex[k] = '\0';

                if (k >= 24) {
                    DecodedSmsPdu decoded;
                    if (decode_sms_deliver_pdu(pdu_hex, &decoded) == 0) {
                        submit_decoded_sms_part(&decoded);
                    }
                }
            }

            /* 读取入库后立即从 ME 删除，保持 ME 0/100 永不写满 */
            char del_cmd[32];
            char *del_res = NULL;
            snprintf(del_cmd, sizeof(del_cmd), "AT+CMGD=%d", idx);
            if (execute_at(del_cmd, &del_res) == 0 && del_res) {
                g_free(del_res);
            }
        }

        g_free(res);
        res = NULL;
    }

    flush_expired_concat_groups();
}

static void *sms_worker_loop(void *arg) {
    (void)arg;
    int first_run = 1;
    while (g_sms_worker_running) {
        if (g_ofono_available) {
            if (first_run) {
                ensure_sms_direct_route();
                ensure_ims_registered(1);
                first_run = 0;
            } else {
                ensure_ims_registered(0);
            }
            sms_drain_me_storage();
        }
        for (int i = 0; i < 40 && g_sms_worker_running; i++) {
            usleep(100000); /* 4 秒周期，支持快速退出 */
        }
    }
    return NULL;
}

/* 获取短信接收修复开关状态（RM500U-CN 强制启用 ME 暂存自动排空入系统 SQLite，不再暴露 R106 开关） */
int sms_get_fix_enabled(void) {
    return 1;
}

/* 设置短信接收修复开关 */
int sms_set_fix_enabled(int enabled) {
    extern int execute_at(const char *command, char **result);
    char sql[256];
    char *at_result = NULL;
    
    /* 开启=切至 ME 存储 + CNMI=2,1,0,0,0 自动排空入数据库；关闭=回退 SM (SIM卡) */
    const char *cpms_cmd = enabled ? "AT+CPMS=\"ME\",\"ME\",\"ME\"" : "AT+CPMS=\"SM\",\"SM\",\"SM\"";
    if (execute_at(cpms_cmd, &at_result) != 0) {
        if (at_result) g_free(at_result);
        return -1;
    }
    if (at_result) { g_free(at_result); at_result = NULL; }

    if (execute_at("AT+CNMI=2,1,0,0,0", &at_result) == 0 && at_result) {
        g_free(at_result);
        at_result = NULL;
    }
    
    /* 保存到数据库 */
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO sms_config (id, max_count, max_sent_count, sms_fix_enabled) "
        "VALUES (1, %d, %d, %d);", 
        g_max_sms_count, g_max_sent_count, enabled ? 1 : 0);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    return ret;
}

/* 初始化时应用短信修复设置 */
static void apply_sms_fix_on_init(void) {
    if (sms_get_fix_enabled()) {
        ensure_sms_direct_route();
    }
}

/* 保存发送记录到数据库，返回新插入记录的 ID */
static int save_sent_sms_to_db(const char *recipient, const char *content, time_t timestamp, const char *status) {
    char sql[4096];
    char escaped_content[3072];
    
    size_t j = 0;
    for (size_t i = 0; content[i] && j < sizeof(escaped_content) - 2; i++) {
        if (content[i] == '\'') {
            escaped_content[j++] = '\'';
            escaped_content[j++] = '\'';
        } else {
            escaped_content[j++] = content[i];
        }
    }
    escaped_content[j] = '\0';
    
    snprintf(sql, sizeof(sql),
        "INSERT INTO sent_sms (recipient, content, timestamp, status) VALUES ('%s', '%s', %ld, '%s');",
        recipient, escaped_content, (long)timestamp, status);
    
    int inserted_id = -1;
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    if (ret == 0) {
        inserted_id = db_query_int("SELECT MAX(id) FROM sent_sms;", -1);
        snprintf(sql, sizeof(sql),
            "DELETE FROM sent_sms WHERE id NOT IN (SELECT id FROM sent_sms ORDER BY id DESC LIMIT %d);",
            g_max_sent_count);
        db_execute(sql);
    }
    pthread_mutex_unlock(&g_sms_mutex);
    
    return inserted_id;
}

/* 删除发送记录 */
int sms_delete_sent(int id) {
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM sent_sms WHERE id = %d;", id);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    return ret;
}

/* 获取发送记录列表 - 使用hex编码避免特殊字符问题，兼容无JSON扩展的SQLite */
int sms_get_sent_list(SentSmsMessage *messages, int max_count) {
    char sql[512];
    char *output = NULL;
    
    if (!messages || max_count <= 0) return -1;
    
    /* 分配大缓冲区 */
    output = (char *)malloc(256 * 1024);
    if (!output) return -1;
    
    /* 使用hex编码content字段，用|分隔，每行一条记录 */
    snprintf(sql, sizeof(sql),
        "SELECT id || '|' || recipient || '|' || hex(content) || '|' || timestamp || '|' || status FROM sent_sms ORDER BY id DESC LIMIT %d;",
        max_count);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_query_string(sql, output, 256 * 1024);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret != 0 || strlen(output) == 0) {
        printf("[SMS] 获取发送记录列表失败或为空\n");
        free(output);
        return 0;
    }
    
    /* 解析输出 - 格式: id|recipient|hex_content|timestamp|status\n */
    int count = 0;
    char *line = output;
    char *next_line;
    
    while (line && *line && count < max_count) {
        /* 找到行尾 */
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        
        /* 跳过空行 */
        if (strlen(line) == 0) {
            line = next_line;
            continue;
        }
        
        /* 解析字段: id|recipient|hex_content|timestamp|status */
        char *fields[5] = {NULL};
        int field_count = 0;
        char *p = line;
        char *field_start = p;
        
        while (*p && field_count < 5) {
            if (*p == '|') {
                *p = '\0';
                fields[field_count++] = field_start;
                field_start = p + 1;
            }
            p++;
        }
        if (field_count < 5 && field_start) {
            fields[field_count++] = field_start;
        }
        
        if (field_count >= 5) {
            messages[count].id = atoi(fields[0]);
            strncpy(messages[count].recipient, fields[1], sizeof(messages[count].recipient) - 1);
            messages[count].recipient[sizeof(messages[count].recipient) - 1] = '\0';
            
            /* hex解码content */
            hex_decode(fields[2], messages[count].content, sizeof(messages[count].content));
            
            messages[count].timestamp = (time_t)atol(fields[3]);
            strncpy(messages[count].status, fields[4], sizeof(messages[count].status) - 1);
            messages[count].status[sizeof(messages[count].status) - 1] = '\0';
            count++;
        }
        
        line = next_line;
    }
    
    free(output);
    printf("[SMS] 获取到 %d 条发送记录\n", count);
    return count;
}

/* 获取最大存储数量 */
int sms_get_max_count(void) {
    return g_max_sms_count;
}

/* 获取发送记录最大存储数量 */
int sms_get_max_sent_count(void) {
    return g_max_sent_count;
}

/* 加载配置 */
static void load_sms_config(void) {
    char output[128];
    const char *sql = "SELECT max_count || '|' || max_sent_count FROM sms_config WHERE id = 1;";
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_query_string(sql, output, sizeof(output));
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret == 0 && strlen(output) > 0) {
        char *sep = strchr(output, '|');
        if (sep) {
            *sep = '\0';
            int mc = atoi(output);
            int msc = atoi(sep + 1);
            if (mc > 0) g_max_sms_count = mc;
            if (msc > 0) g_max_sent_count = msc;
        }
    }
    
    printf("[SMS] 配置加载完成: 收件箱最大=%d, 发件箱最大=%d\n", g_max_sms_count, g_max_sent_count);
}

/* 设置最大存储数量 */
int sms_set_max_count(int count) {
    if (count < 10 || count > 150) {
        printf("最大存储数量必须在10-150之间\n");
        return -1;
    }
    
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO sms_config (id, max_count, max_sent_count) VALUES (1, %d, %d);", 
        count, g_max_sent_count);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret == 0) {
        g_max_sms_count = count;
    }
    
    return ret;
}

/* 设置发送记录最大存储数量 */
int sms_set_max_sent_count(int count) {
    if (count < 1 || count > 50) {
        printf("发送记录最大存储数量必须在1-50之间\n");
        return -1;
    }
    
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO sms_config (id, max_count, max_sent_count) VALUES (1, %d, %d);", 
        g_max_sms_count, count);
    
    pthread_mutex_lock(&g_sms_mutex);
    int ret = db_execute(sql);
    pthread_mutex_unlock(&g_sms_mutex);
    
    if (ret == 0) {
        g_max_sent_count = count;
    }
    
    return ret;
}

/* 添加Webhook发送日志（内存存储） */
static void add_webhook_log(const char *sender, const char *request, const char *response, int result) {
    pthread_mutex_lock(&g_webhook_log_mutex);
    
    /* 循环缓冲区 */
    int idx = g_webhook_log_count % MAX_WEBHOOK_LOGS;
    
    g_webhook_logs[idx].id = ++g_webhook_log_id;
    strncpy(g_webhook_logs[idx].sender, sender ? sender : "", sizeof(g_webhook_logs[idx].sender) - 1);
    strncpy(g_webhook_logs[idx].request, request ? request : "", sizeof(g_webhook_logs[idx].request) - 1);
    strncpy(g_webhook_logs[idx].response, response ? response : "", sizeof(g_webhook_logs[idx].response) - 1);
    g_webhook_logs[idx].result = result;
    g_webhook_logs[idx].created_at = time(NULL);
    
    if (g_webhook_log_count < MAX_WEBHOOK_LOGS) {
        g_webhook_log_count++;
    }
    
    pthread_mutex_unlock(&g_webhook_log_mutex);
    
    printf("[SMS] Webhook日志已添加, ID=%d, 结果=%d\n", g_webhook_log_id, result);
}

/* JSON字符串转义工具 */
static void json_escape(char *dest, const char *src, size_t dest_size) {
    if (!dest || !src || dest_size == 0) return;
    
    size_t i = 0, j = 0;
    while (src[i] && j < dest_size - 1) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = '"'; } else break;
        } else if (c == '\\') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = '\\'; } else break;
        } else if (c == '\b') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 'b'; } else break;
        } else if (c == '\f') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 'f'; } else break;
        } else if (c == '\n') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 'n'; } else break;
        } else if (c == '\r') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 'r'; } else break;
        } else if (c == '\t') {
            if (j + 2 < dest_size) { dest[j++] = '\\'; dest[j++] = 't'; } else break;
        } else if (c < 32) {
            if (j + 6 < dest_size) {
                j += snprintf(dest + j, 7, "\\u%04x", c);
            } else break;
        } else {
            dest[j++] = c;
        }
        i++;
    }
    dest[j] = '\0';
}

/* 获取Webhook发送日志 */
int sms_get_webhook_logs(char *json_output, size_t size, int max_count) {
    if (!json_output || size == 0) {
        return -1;
    }
    
    if (max_count <= 0 || max_count > MAX_WEBHOOK_LOGS) {
        max_count = 20;
    }
    
    pthread_mutex_lock(&g_webhook_log_mutex);
    
    int offset = 0;
    offset += snprintf(json_output + offset, size - offset, "[");
    
    int count = (g_webhook_log_count < max_count) ? g_webhook_log_count : max_count;
    int first = 1;
    
    /* 从最新的日志开始输出 */
    for (int i = 0; i < count && offset < (int)size - 10; i++) {
        int idx;
        if (g_webhook_log_count <= MAX_WEBHOOK_LOGS) {
            idx = g_webhook_log_count - 1 - i;
        } else {
            idx = (g_webhook_log_count - 1 - i) % MAX_WEBHOOK_LOGS;
        }
        
        if (idx < 0) break;
        
        if (!first) offset += snprintf(json_output + offset, size - offset, ",");
        first = 0;
        
        char escaped_req[3072] = "";
        char escaped_resp[3072] = "";
        
        json_escape(escaped_req, g_webhook_logs[idx].request, sizeof(escaped_req));
        json_escape(escaped_resp, g_webhook_logs[idx].response, sizeof(escaped_resp));
        
        offset += snprintf(json_output + offset, size - offset,
            "{\"id\":%d,\"sender\":\"%s\",\"request\":\"%s\",\"response\":\"%s\",\"result\":%d,\"created_at\":%ld}",
            g_webhook_logs[idx].id,
            g_webhook_logs[idx].sender,
            escaped_req,
            escaped_resp,
            g_webhook_logs[idx].result,
            (long)g_webhook_logs[idx].created_at);
            
        if (offset >= (int)size - 10) break;
    }
    
    offset += snprintf(json_output + offset, size - offset, "]");
    
    pthread_mutex_unlock(&g_webhook_log_mutex);
    
    return 0;
}
