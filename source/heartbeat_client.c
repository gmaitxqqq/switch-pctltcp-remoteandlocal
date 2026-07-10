// heartbeat_client.c — Switch 远程+本地双服务器心跳客户端实现
// v2.0.0: 支持同时连接远程服务器和本地服务器（双通道）
//
// 核心设计：
//   1. Switch 作为客户端主动连接（无 accept() 问题）
//   2. 两个独立心跳线程：远程线程 + 本地线程
//   3. 各自独立重连、退避、恢复，互不影响
//   4. 两个服务器的命令都解析后推入同一个命令队列
//   5. 任意一个服务器连不上不影响另一个
//   6. 两个都连不上也只是功能不可用，不会崩溃

#include "heartbeat_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Logging — 委托给 main.c 的 log_msg                                */
/* ------------------------------------------------------------------ */
extern void log_msg(const char *msg);

/* ------------------------------------------------------------------ */
/*  版本号                                                             */
/* ------------------------------------------------------------------ */
#define TUNNEL_VERSION  "2.0.0"

/* ------------------------------------------------------------------ */
/*  前向声明                                                           */
/* ------------------------------------------------------------------ */
static const char *json_find_value(const char *json, const char *key);
static bool json_read_string(const char *value, char *buf, size_t bufsize);
static bool json_read_int(const char *value, int *out);

/* ------------------------------------------------------------------ */
/*  运行时配置                                                           */
/* ------------------------------------------------------------------ */
#define CFG_HOST_MAX    128
#define CFG_PSK_MAX     256

/* 远程服务器配置 */
static char s_cfg_host[CFG_HOST_MAX] = "";
static int  s_cfg_port = 0;
static char s_cfg_psk[CFG_PSK_MAX] = "";
static int  s_cfg_interval = 0;
static int  s_cfg_recv_timeout = 0;

/* 本地服务器配置（可选） */
static char s_cfg_local_host[CFG_HOST_MAX] = "";
static int  s_cfg_local_port = 0;
static char s_cfg_local_psk[CFG_PSK_MAX] = "";
static bool s_cfg_local_enabled = false;

static bool s_cfg_loaded = false;

static void load_config(void) {
    s_cfg_loaded = false;
    s_cfg_local_enabled = false;

    FILE *f = fopen(TUNNEL_CONFIG_PATH, "r");
    if (!f) {
        return;
    }

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) {
        return;
    }
    buf[n] = '\0';

    const char *val;

    /* 读取远程服务器配置 */
    val = json_find_value(buf, "host");
    if (!val || !json_read_string(val, s_cfg_host, sizeof(s_cfg_host))) {
        return;
    }

    val = json_find_value(buf, "port");
    if (!val || !json_read_int(val, &s_cfg_port) || s_cfg_port <= 0 || s_cfg_port > 65535) {
        return;
    }

    val = json_find_value(buf, "psk");
    if (!val || !json_read_string(val, s_cfg_psk, sizeof(s_cfg_psk))) {
        return;
    }

    s_cfg_interval = TUNNEL_DEFAULT_INTERVAL_SEC;
    val = json_find_value(buf, "interval");
    if (val) json_read_int(val, &s_cfg_interval);
    if (s_cfg_interval < 2) s_cfg_interval = TUNNEL_DEFAULT_INTERVAL_SEC;

    s_cfg_recv_timeout = TUNNEL_DEFAULT_RECV_TIMEOUT_SEC;
    val = json_find_value(buf, "recv_timeout");
    if (val) json_read_int(val, &s_cfg_recv_timeout);
    if (s_cfg_recv_timeout < 22) s_cfg_recv_timeout = TUNNEL_DEFAULT_RECV_TIMEOUT_SEC;

    /* 读取本地服务器配置（可选 — 没有则不启动本地线程） */
    s_cfg_local_enabled = false;
    val = json_find_value(buf, "local_host");
    if (val && json_read_string(val, s_cfg_local_host, sizeof(s_cfg_local_host))) {
        val = json_find_value(buf, "local_port");
        if (val && json_read_int(val, &s_cfg_local_port) && s_cfg_local_port > 0 && s_cfg_local_port <= 65535) {
            val = json_find_value(buf, "local_psk");
            if (val && json_read_string(val, s_cfg_local_psk, sizeof(s_cfg_local_psk))) {
                s_cfg_local_enabled = true;
                char log_buf[128];
                snprintf(log_buf, sizeof(log_buf), "tunnel: local server enabled (%s:%d)", s_cfg_local_host, s_cfg_local_port);
                log_msg(log_buf);
            }
        }
    }

    if (!s_cfg_local_enabled) {
        log_msg("tunnel: local server not configured, remote-only mode");
    }

    s_cfg_loaded = true;
}

/* ------------------------------------------------------------------ */
/*  命令队列（线程安全，环形缓冲区）                                       */
/* ------------------------------------------------------------------ */
static TunnelCommand s_cmd_queue[TUNNEL_CMD_QUEUE_SIZE];
static int s_cmd_head = 0;
static int s_cmd_tail = 0;
static Mutex s_cmd_mutex;

static int cmd_queue_count_locked(void) {
    int count = s_cmd_tail - s_cmd_head;
    if (count < 0) count += TUNNEL_CMD_QUEUE_SIZE;
    return count;
}

static bool cmd_queue_push(const TunnelCommand *cmd) {
    if (!cmd) return false;
    mutexLock(&s_cmd_mutex);

    int next = (s_cmd_tail + 1) % TUNNEL_CMD_QUEUE_SIZE;
    if (next == s_cmd_head) {
        s_cmd_head = (s_cmd_head + 1) % TUNNEL_CMD_QUEUE_SIZE;
    }

    s_cmd_queue[s_cmd_tail] = *cmd;
    s_cmd_tail = next;

    mutexUnlock(&s_cmd_mutex);
    return true;
}

/* ------------------------------------------------------------------ */
/*  状态数据（主循环写，心跳线程读）                                       */
/* ------------------------------------------------------------------ */
static TunnelStatus s_status;
static Mutex s_status_mutex;

void tunnel_update_status(const TunnelStatus *status) {
    if (!status) return;
    mutexLock(&s_status_mutex);
    s_status = *status;
    mutexUnlock(&s_status_mutex);
}

static void tunnel_get_status(TunnelStatus *out) {
    if (!out) return;
    mutexLock(&s_status_mutex);
    *out = s_status;
    mutexUnlock(&s_status_mutex);
}

/* ------------------------------------------------------------------ */
/*  pctl 互斥锁 — 防止多线程同时调用 pctl_init/exit 导致 IPC 冲突        */
/* ------------------------------------------------------------------ */
static Mutex s_pctl_mutex;

void tunnel_pctl_lock(void) {
    mutexLock(&s_pctl_mutex);
}

/* ------------------------------------------------------------------ */
/*  v2.0: 命令回执与去重（心跳线程上报，主循环写入）                     */
/*  单一全局最近回执 + 16 槽环形去重缓冲（双隧道共用，由 s_ack_mutex 保护）*/
/* ------------------------------------------------------------------ */
#define TUNNEL_ACKED_MAX   16
static char s_acked_ids[TUNNEL_ACKED_MAX][32];
static char s_acked_results[TUNNEL_ACKED_MAX][16];
static int  s_acked_idx = 0;
static Mutex s_ack_mutex;
static char s_last_ack_id[32] = "";
static char s_last_ack_result[16] = "";

void tunnel_pctl_unlock(void) {
    mutexUnlock(&s_pctl_mutex);
}

/* ------------------------------------------------------------------ */
/*  最小 JSON 解析器                                                   */
/* ------------------------------------------------------------------ */

static const char *json_find_value(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *pos = strstr(json, pattern);
    if (!pos) return NULL;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':') {
        pos++;
    }
    return pos;
}

static bool json_read_string(const char *value, char *buf, size_t bufsize) {
    if (!value || *value != '"' || !buf || bufsize == 0) return false;
    value++;
    size_t i = 0;
    while (*value && *value != '"' && i < bufsize - 1) {
        if (*value == '\\' && *(value + 1)) value++;
        buf[i++] = *value++;
    }
    buf[i] = '\0';
    return (*value == '"');
}

static bool json_read_int(const char *value, int *out) {
    if (!value || !out) return false;
    char *end = NULL;
    long val = strtol(value, &end, 10);
    if (end == value) return false;
    *out = (int)val;
    return true;
}

static int json_read_int_array(const char *value, int *arr, int max_count) {
    if (!value || *value != '[' || !arr || max_count <= 0) return 0;
    value++;
    int count = 0;
    while (count < max_count) {
        while (*value == ' ' || *value == '\t' || *value == '\n' || *value == '\r') value++;
        if (*value == ']' || *value == '\0') break;
        if (*value == 'n' && strncmp(value, "null", 4) == 0) {
            arr[count] = -1;
            value += 4;
        } else {
            char *end = NULL;
            long val = strtol(value, &end, 10);
            if (end == value) break;
            arr[count] = (int)val;
            value = end;
        }
        count++;
        while (*value == ' ' || *value == '\t') value++;
        if (*value == ',') value++;
    }
    return count;
}

/**
 * 解析心跳响应 JSON，提取命令并推入队列。
 */
static void parse_heartbeat_response(const char *response) {
    if (!response) return;

    const char *cmd_val = json_find_value(response, "command");
    if (!cmd_val) return;
    if (strncmp(cmd_val, "null", 4) == 0) return;

    const char *action_val = json_find_value(cmd_val, "action");
    const char *value_val  = json_find_value(cmd_val, "value");

    if (!action_val) return;

    char action[64] = {0};
    if (!json_read_string(action_val, action, sizeof(action))) return;

    int value = 0;
    if (value_val) json_read_int(value_val, &value);

    TunnelCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.param = value;
    cmd.day_of_week = -1;
    memset(cmd.weekly, -1, sizeof(cmd.weekly));

    /* v2.0: 读取服务端下发的命令唯一ID（老服务端不下发时为空，向后兼容） */
    const char *cmd_id_val = json_find_value(cmd_val, "cmd_id");
    if (cmd_id_val) {
        json_read_string(cmd_id_val, cmd.cmd_id, sizeof(cmd.cmd_id));
    }

    if (strcmp(action, "add_minutes") == 0) {
        cmd.type = TUNNEL_CMD_ADD_MINUTES;
    } else if (strcmp(action, "set_day_limit") == 0) {
        cmd.type = TUNNEL_CMD_SET_DAY_LIMIT;
        const char *dow_val = json_find_value(cmd_val, "day_of_week");
        if (dow_val) json_read_int(dow_val, &cmd.day_of_week);
    } else if (strcmp(action, "reset_play_time") == 0) {
        cmd.type = TUNNEL_CMD_RESET_PLAY_TIME;
    } else if (strcmp(action, "set_weekly_limits") == 0) {
        cmd.type = TUNNEL_CMD_SET_WEEKLY_LIMITS;
        const char *weekly_val = json_find_value(cmd_val, "weekly");
        if (weekly_val) {
            json_read_int_array(weekly_val, cmd.weekly, 7);
        }
    } else {
        return;
    }

    /* v2.0: cmd_id 去重 —— 防止 ack 丢失后服务端重发导致命令被重复执行 */
    if (cmd.cmd_id[0]) {
        char prev_result[16] = "";
        if (tunnel_cmd_already_executed(cmd.cmd_id, prev_result, sizeof(prev_result))) {
            /* 已执行过（ack 可能丢失），重新回执让服务端确认，不重复执行 */
            tunnel_set_last_ack(cmd.cmd_id, prev_result[0] ? prev_result : "success");
            return;
        }
    }

    cmd_queue_push(&cmd);
}

/* ------------------------------------------------------------------ */
/*  HTTP/1.1 客户端（raw socket，无外部依赖）                        */
/* ------------------------------------------------------------------ */

static int http_connect(const char *host, int port, int recv_timeout) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        if (err != 110 && err != 115 && err != 114) {
            char buf[128];
            snprintf(buf, sizeof(buf), "tunnel: connect failed (errno=%d)", err);
            log_msg(buf);
        }
        close(fd);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = recv_timeout;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

static bool http_post_json(const char *host, int port, int recv_timeout,
                           const char *path, const char *auth_token,
                           const char *body, char *resp_buf, size_t resp_size) {
    int fd = http_connect(host, port, recv_timeout);
    if (fd < 0) return false;

    char req_header[768];
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s?key=%s", path, auth_token);
    int body_len = (int)strlen(body);
    snprintf(req_header, sizeof(req_header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %d\r\n"
        "User-Agent: Switch-PctlTunnel/" TUNNEL_VERSION "\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        full_path, host, port, auth_token, body_len);

    ssize_t sent = send(fd, req_header, strlen(req_header), 0);
    if (sent < 0) {
        close(fd);
        return false;
    }

    sent = send(fd, body, body_len, 0);
    if (sent < 0) {
        close(fd);
        return false;
    }

    ssize_t total = 0;
    while (total < (ssize_t)(resp_size - 1)) {
        ssize_t n = recv(fd, resp_buf + total, resp_size - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    close(fd);

    if (total == 0) {
        return false;
    }

    resp_buf[total] = '\0';

    char *json_start = strstr(resp_buf, "\r\n\r\n");
    if (!json_start) {
        return false;
    }

    json_start += 4;
    size_t json_len = strlen(json_start);
    memmove(resp_buf, json_start, json_len + 1);

    return true;
}

/* ------------------------------------------------------------------ */
/*  心跳线程（远程 + 本地，各自独立）                                     */
/* ------------------------------------------------------------------ */
static Thread s_thread_remote;
static Thread s_thread_local;
static volatile bool s_running_remote = false;
static volatile bool s_running_local = false;
static volatile bool s_wake_flag_remote = false;   /* 远程线程专用唤醒标志 */
static volatile bool s_wake_flag_local = false;    /* 本地线程专用唤醒标志 */
static volatile bool s_thread_remote_active = false;
static volatile bool s_thread_local_active = false;

/* 命令执行后强制立即心跳（提前跳出睡眠，上报新状态） */
static volatile bool s_force_heartbeat_remote = false;
static volatile bool s_force_heartbeat_local = false;

/* 退避参数 */
#define BACKOFF_BASE_SEC    3
#define BACKOFF_MAX_SEC     300

/**
 * 通用心跳线程函数
 * @param arg: TunnelServerType (TUNNEL_SERVER_REMOTE or TUNNEL_SERVER_LOCAL)
 *
 * 关键设计：
 *   - 每个线程有独立的 wake flag，不会被另一个线程消费
 *   - 失败时指数退避，不会疯狂重连
 *   - 配置丢失时线程退出，由 tunnel_restart() 重新创建
 *   - 无论连接成功还是失败，都不会崩溃
 */
static void heartbeat_thread_func(void *arg) {
    TunnelServerType server_type = (TunnelServerType)(uintptr_t)arg;

    /* 根据服务器类型选择配置和状态 */
    const char *host;
    int port;
    const char *psk;
    const char *server_name;
    volatile bool *running_flag;
    volatile bool *wake_flag;
    volatile bool *active_flag;
    volatile bool *force_flag;

    if (server_type == TUNNEL_SERVER_LOCAL) {
        host = s_cfg_local_host;
        port = s_cfg_local_port;
        psk = s_cfg_local_psk;
        server_name = "local";
        running_flag = &s_running_local;
        wake_flag = &s_wake_flag_local;
        active_flag = &s_thread_local_active;
        force_flag = &s_force_heartbeat_local;
    } else {
        host = s_cfg_host;
        port = s_cfg_port;
        psk = s_cfg_psk;
        server_name = "remote";
        running_flag = &s_running_remote;
        wake_flag = &s_wake_flag_remote;
        active_flag = &s_thread_remote_active;
        force_flag = &s_force_heartbeat_remote;
    }

    *running_flag = true;
    time_t start_time = time(NULL);
    char resp_buf[2048];
    char body_buf[1024];

    int backoff = BACKOFF_BASE_SEC;
    int fail_streak = 0;

    while (*running_flag) {
        /* 检查唤醒标志 */
        if (*wake_flag) {
            *wake_flag = false;
            backoff = BACKOFF_BASE_SEC;
            /* 唤醒后立即重试，不等 */
        }

        /* 构建心跳 JSON 体 */
        TunnelStatus cur;
        tunnel_get_status(&cur);

        int pos = snprintf(body_buf, sizeof(body_buf),
            "{\"uptime\":%d,\"version\":\"%s\"",
            (int)(time(NULL) - start_time), TUNNEL_VERSION);

        if (cur.today_limit >= 0) {
            pos += snprintf(body_buf + pos, sizeof(body_buf) - pos,
                ",\"today_limit\":%d", cur.today_limit);
        }
        if (cur.today_played >= 0) {
            pos += snprintf(body_buf + pos, sizeof(body_buf) - pos,
                ",\"today_played\":%d", cur.today_played);
        }
        if (cur.today_remaining >= 0) {
            pos += snprintf(body_buf + pos, sizeof(body_buf) - pos,
                ",\"today_remaining\":%d", cur.today_remaining);
        }

        bool has_weekly = false;
        for (int d = 0; d < 7; d++) {
            if (cur.weekly_limits[d] >= 0) { has_weekly = true; break; }
        }
        if (has_weekly) {
            pos += snprintf(body_buf + pos, sizeof(body_buf) - pos, ",\"weekly_limits\":[");
            for (int d = 0; d < 7; d++) {
                if (d > 0) pos += snprintf(body_buf + pos, sizeof(body_buf) - pos, ",");
                if (cur.weekly_limits[d] >= 0) {
                    pos += snprintf(body_buf + pos, sizeof(body_buf) - pos, "%d", cur.weekly_limits[d]);
                } else {
                    pos += snprintf(body_buf + pos, sizeof(body_buf) - pos, "null");
                }
            }
            pos += snprintf(body_buf + pos, sizeof(body_buf) - pos, "]");
        }

        /* v2.0: 上报最近一次命令执行回执（服务端据此确认命令成功/失败） */
        mutexLock(&s_ack_mutex);
        if (s_last_ack_id[0]) {
            pos += snprintf(body_buf + pos, sizeof(body_buf) - pos,
                ",\"last_executed_cmd_id\":\"%s\"", s_last_ack_id);
            pos += snprintf(body_buf + pos, sizeof(body_buf) - pos,
                ",\"last_executed_result\":\"%s\"", s_last_ack_result);
        }
        mutexUnlock(&s_ack_mutex);

        snprintf(body_buf + pos, sizeof(body_buf) - pos, "}");

        /* 发送心跳 */
        bool ok = http_post_json(host, port, s_cfg_recv_timeout,
                                 TUNNEL_HEARTBEAT_PATH, psk,
                                 body_buf, resp_buf, sizeof(resp_buf));

        if (ok) {
            parse_heartbeat_response(resp_buf);
            if (fail_streak > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "tunnel: %s heartbeat recovered, connection OK", server_name);
                log_msg(buf);
                fail_streak = 0;
            }
            backoff = BACKOFF_BASE_SEC;
        } else {
            fail_streak++;
            /* 只有连续失败 3 次以上才打日志，避免刷屏 */
            if (fail_streak == 3 || (fail_streak % 10 == 0)) {
                char buf[128];
                snprintf(buf, sizeof(buf), "tunnel: %s heartbeat failed (%d times), retry in %ds",
                         server_name, fail_streak, backoff);
                log_msg(buf);
            }
        }

        /* 休眠，支持 wake 唤醒 + force 提前心跳（分段等待，每次1秒检查一次） */
        int sleep_secs = ok ? backoff : backoff;
        for (int i = 0; i < sleep_secs && *running_flag; i++) {
            if (*wake_flag) {
                *wake_flag = false;
                backoff = BACKOFF_BASE_SEC;
                break;
            }
            if (*force_flag) {
                *force_flag = false;
                /* 命令执行后强制立即心跳，跳出睡眠 */
                break;
            }
            svcSleepThread(1000000000ULL);
        }

        /* 退避递增 */
        if (!ok) {
            backoff = backoff * 2;
            if (backoff > BACKOFF_MAX_SEC) backoff = BACKOFF_MAX_SEC;

            /* 每 3 次失败重新加载配置文件（支持热重载）*/
            if (fail_streak % 3 == 0) {
                load_config();
                if (!s_cfg_loaded) {
                    /* 配置丢失 → 线程退出，由 tunnel_restart() 重建 */
                    char buf[128];
                    snprintf(buf, sizeof(buf), "tunnel: %s config lost, thread exiting", server_name);
                    log_msg(buf);
                    break;
                }
                /* 重新读取服务器配置（可能已更改）*/
                if (server_type == TUNNEL_SERVER_LOCAL) {
                    host = s_cfg_local_host;
                    port = s_cfg_local_port;
                    psk = s_cfg_local_psk;
                } else {
                    host = s_cfg_host;
                    port = s_cfg_port;
                    psk = s_cfg_psk;
                }
            }
        }
    }

    *active_flag = false;

    char buf[64];
    snprintf(buf, sizeof(buf), "tunnel: %s thread exited", server_name);
    log_msg(buf);
}

/* ------------------------------------------------------------------ */
/*  公共 API                                                           */
/* ------------------------------------------------------------------ */

void tunnel_init(void) {
    mutexInit(&s_cmd_mutex);
    mutexInit(&s_status_mutex);
    mutexInit(&s_pctl_mutex);
    mutexInit(&s_ack_mutex);
}

void tunnel_start(void) {
    load_config();
    if (!s_cfg_loaded) {
        log_msg("tunnel: config not loaded, cannot start");
        return;
    }

    TunnelStatus init_status;
    memset(&init_status, -1, sizeof(init_status));
    tunnel_update_status(&init_status);

    /* 启动远程服务器心跳线程 */
    s_running_remote = true;
    Result rc = threadCreate(&s_thread_remote, heartbeat_thread_func,
                             (void*)TUNNEL_SERVER_REMOTE, NULL, 0x10000, 0x2C, -2);
    if (R_FAILED(rc)) {
        s_running_remote = false;
        log_msg("tunnel: failed to create remote thread");
    } else {
        rc = threadStart(&s_thread_remote);
        if (R_FAILED(rc)) {
            s_running_remote = false;
            log_msg("tunnel: failed to start remote thread");
        } else {
            s_thread_remote_active = true;
            log_msg("tunnel: remote server thread started");
        }
    }

    /* 如果配置了本地服务器，也启动本地心跳线程 */
    if (s_cfg_local_enabled) {
        s_running_local = true;
        rc = threadCreate(&s_thread_local, heartbeat_thread_func,
                          (void*)TUNNEL_SERVER_LOCAL, NULL, 0x10000, 0x2C, -2);
        if (R_FAILED(rc)) {
            s_running_local = false;
            log_msg("tunnel: failed to create local thread (non-fatal)");
        } else {
            rc = threadStart(&s_thread_local);
            if (R_FAILED(rc)) {
                s_running_local = false;
                log_msg("tunnel: failed to start local thread (non-fatal)");
            } else {
                s_thread_local_active = true;
                log_msg("tunnel: local server thread started");
            }
        }
    }
}

void tunnel_stop(void) {
    /* 停止远程线程 */
    if (s_running_remote) {
        s_running_remote = false;
        s_wake_flag_remote = true;

        if (s_thread_remote_active) {
            for (int i = 0; i < 30 && s_thread_remote_active; i++) {
                svcSleepThread(100000000ULL);
            }
        }
        threadClose(&s_thread_remote);
        s_thread_remote_active = false;
    }

    /* 停止本地线程 */
    if (s_running_local) {
        s_running_local = false;
        s_wake_flag_local = true;

        if (s_thread_local_active) {
            for (int i = 0; i < 30 && s_thread_local_active; i++) {
                svcSleepThread(100000000ULL);
            }
        }
        threadClose(&s_thread_local);
        s_thread_local_active = false;
    }
}

void tunnel_restart(void) {
    /* 热重载配置 */
    load_config();

    /* 唤醒两个线程 */
    s_wake_flag_remote = true;
    s_wake_flag_local = true;

    /* 如果远程线程已退出（配置丢失等），重新创建 */
    if (!s_thread_remote_active && s_cfg_loaded) {
        s_running_remote = true;
        Result rc = threadCreate(&s_thread_remote, heartbeat_thread_func,
                                 (void*)TUNNEL_SERVER_REMOTE, NULL, 0x10000, 0x2C, -2);
        if (R_SUCCEEDED(rc)) {
            rc = threadStart(&s_thread_remote);
            if (R_SUCCEEDED(rc)) {
                s_thread_remote_active = true;
                log_msg("tunnel: remote thread recreated");
            }
        }
    }

    /* 如果本地线程已退出但配置了本地服务器，重新创建 */
    if (!s_thread_local_active && s_cfg_local_enabled) {
        s_running_local = true;
        Result rc = threadCreate(&s_thread_local, heartbeat_thread_func,
                                 (void*)TUNNEL_SERVER_LOCAL, NULL, 0x10000, 0x2C, -2);
        if (R_SUCCEEDED(rc)) {
            rc = threadStart(&s_thread_local);
            if (R_SUCCEEDED(rc)) {
                s_thread_local_active = true;
                log_msg("tunnel: local thread recreated");
            }
        }
    }

    /* 如果本地服务器被禁用且本地线程正在运行，停止它 */
    if (!s_cfg_local_enabled && s_running_local) {
        s_running_local = false;
        s_wake_flag_local = true;
        if (s_thread_local_active) {
            for (int i = 0; i < 30 && s_thread_local_active; i++) {
                svcSleepThread(100000000ULL);
            }
        }
        threadClose(&s_thread_local);
        s_thread_local_active = false;
        log_msg("tunnel: local thread stopped (disabled in config)");
    }
}

bool tunnel_is_running(void) {
    return s_thread_remote_active || s_thread_local_active;
}

int tunnel_dequeue_cmd(TunnelCommand *cmd) {
    if (!cmd) return 0;
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = TUNNEL_CMD_NONE;
    cmd->day_of_week = -1;
    memset(cmd->weekly, -1, sizeof(cmd->weekly));

    mutexLock(&s_cmd_mutex);
    if (s_cmd_head == s_cmd_tail) {
        mutexUnlock(&s_cmd_mutex);
        return 0;
    }
    *cmd = s_cmd_queue[s_cmd_head];
    s_cmd_head = (s_cmd_head + 1) % TUNNEL_CMD_QUEUE_SIZE;
    int count = cmd_queue_count_locked();
    mutexUnlock(&s_cmd_mutex);

    return count;
}

void tunnel_notify_wake(void) {
    s_wake_flag_remote = true;
    s_wake_flag_local = true;
}

void tunnel_force_heartbeat(void) {
    s_force_heartbeat_remote = true;
    s_force_heartbeat_local = true;
}

/* ------------------------------------------------------------------ */
/*  v2.0: 命令回执与去重                                               */
/* ------------------------------------------------------------------ */

void tunnel_set_last_ack(const char *cmd_id, const char *result) {
    if (!cmd_id || !cmd_id[0]) return;
    mutexLock(&s_ack_mutex);
    strncpy(s_last_ack_id, cmd_id, sizeof(s_last_ack_id) - 1);
    s_last_ack_id[sizeof(s_last_ack_id) - 1] = '\0';
    strncpy(s_last_ack_result, result ? result : "fail", sizeof(s_last_ack_result) - 1);
    s_last_ack_result[sizeof(s_last_ack_result) - 1] = '\0';
    strncpy(s_acked_ids[s_acked_idx], cmd_id, sizeof(s_acked_ids[s_acked_idx]) - 1);
    s_acked_ids[s_acked_idx][sizeof(s_acked_ids[s_acked_idx]) - 1] = '\0';
    strncpy(s_acked_results[s_acked_idx], result ? result : "fail", sizeof(s_acked_results[s_acked_idx]) - 1);
    s_acked_results[s_acked_idx][sizeof(s_acked_results[s_acked_idx]) - 1] = '\0';
    s_acked_idx = (s_acked_idx + 1) % TUNNEL_ACKED_MAX;
    mutexUnlock(&s_ack_mutex);
}

bool tunnel_cmd_already_executed(const char *cmd_id, char *out_result, size_t rsz) {
    if (!cmd_id || !cmd_id[0]) return false;
    mutexLock(&s_ack_mutex);
    bool found = false;
    for (int i = 0; i < TUNNEL_ACKED_MAX; i++) {
        if (strcmp(s_acked_ids[i], cmd_id) == 0) {
            found = true;
            if (out_result && rsz > 0) {
                strncpy(out_result, s_acked_results[i], rsz - 1);
                out_result[rsz - 1] = '\0';
            }
            break;
        }
    }
    mutexUnlock(&s_ack_mutex);
    return found;
}
