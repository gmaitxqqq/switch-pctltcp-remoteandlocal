// heartbeat_client.c — Switch 远程+本地双服务器心跳客户端实现
// v1.8.0: 支持同时连接远程服务器和本地服务器（双通道）
//
// 核心修改：
//   1. 添加本地服务器配置（local_host, local_port, local_psk）
//   2. 创建两个心跳线程：远程线程 + 本地线程
//   3. 两个服务器的命令都解析后推入同一个命令队列
//   4. 配置热重载支持双服务器
//
// 历史版本：
//   v1.7.1: 修复 EALREADY(114)、息屏唤醒延迟、息屏崩溃
//   v1.7.1 核心修改：
//     1. http_connect(): inet_addr() 代替 getaddrinfo()（lwIP 更稳定）
//     2. http_connect(): 纯阻塞 connect，不用 SO_SNDTIMEO（避免 EALREADY）
//     3. http_post_json(): 每次心跳后关闭 socket（Connection: close 协议要求）
//     4. tunnel_restart(): 不停止线程，只设 s_wake_flag + 关闭 socket 强制重连
//     5. tunnel_stop(): 先关 socket 再等线程，避免死锁
//     6. 日志：仅异常时打，正常心跳静默

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
/*  版本号（v1.8.0: 支持远程+本地双服务器）                                                             */
/* ------------------------------------------------------------------ */
#define TUNNEL_VERSION  "1.8.0"

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
        return;  /* 静默失败，tunnel_start 会检查 s_cfg_loaded */
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

    /* 读取本地服务器配置（可选）*/
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
        /* 队列满，静默丢弃最旧的，不打日志 */
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
        return;  /* 未知命令，静默忽略 */
    }

    cmd_queue_push(&cmd);
}

/* ------------------------------------------------------------------ */
/*  HTTP/1.1 客户端（raw socket，无外部依赖）                        */
/*  v1.7.1: inet_addr + 纯阻塞 connect + SO_REUSEADDR              */
/* ------------------------------------------------------------------ */

static int http_connect(const char *host, int port, int recv_timeout) {
    /* 使用 inet_addr() 代替 getaddrinfo()，lwIP 上更稳定 */
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

    /* SO_REUSEADDR: 允许复用处于 TIME_WAIT 的本地地址，避免 EALREADY */
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    /* 阻塞式 connect，不使用 SO_SNDTIMEO（lwIP 上会导致非阻塞行为和 EALREADY）*/
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        /* EALREADY(114)/ETIMEDOUT(110)/EINPROGRESS(115) 属于正常的重试场景，不打日志 */
        if (err != 110 && err != 115 && err != 114) {
            char buf[128];
            snprintf(buf, sizeof(buf), "tunnel: connect failed (errno=%d)", err);
            log_msg(buf);
        }
        close(fd);
        return -1;
    }

    /* 设置接收超时（长轮询需要 25+ 秒） */
    struct timeval tv;
    tv.tv_sec = recv_timeout;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

/** 发送 HTTP POST 并读取响应体（只取 JSON 部分）
 *  每次调用创建新连接，响应后关闭 socket（Connection: close 协议要求）。
 *  这样避免了 EALREADY 问题，因为每次心跳之间有 backoff 间隔，
 *  lwIP 有足够时间清理旧 TCP PCB。
 */
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

    /* 发送请求 */
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

    /* 读取响应 */
    ssize_t total = 0;
    while (total < (ssize_t)(resp_size - 1)) {
        ssize_t n = recv(fd, resp_buf + total, resp_size - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    close(fd);  /* Connection: close — 每次心跳后关闭 socket */

    if (total == 0) {
        return false;
    }

    resp_buf[total] = '\0';

    /* 找到 JSON 正文（跳过 HTTP 头部） */
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
/*  心跳线程（远程 + 本地）                                           */
/* ------------------------------------------------------------------ */
static Thread s_thread_remote;  /* 远程服务器线程 */
static Thread s_thread_local;   /* 本地服务器线程（可选）*/
static volatile bool s_running_remote = false;
static volatile bool s_running_local = false;
static volatile bool s_wake_flag = false;
static volatile bool s_thread_remote_active = false;
static volatile bool s_thread_local_active = false;
static time_t s_start_time = 0;

/* 退避参数 — 正常间隔 3 秒（长轮询模式下等待由服务器端处理）
 * 失败时指数退避，最大 300 秒 */
#define BACKOFF_BASE_SEC    3
#define BACKOFF_MAX_SEC     300

/**
 * 通用心跳线程函数
 * @param arg: TunnelServerType (TUNNEL_SERVER_REMOTE or TUNNEL_SERVER_LOCAL)
 */
static void heartbeat_thread_func(void *arg) {
    TunnelServerType server_type = (TunnelServerType)(uintptr_t)arg;
    
    /* 根据服务器类型选择配置 */
    const char *host;
    int port;
    const char *psk;
    const char *server_name;
    
    if (server_type == TUNNEL_SERVER_LOCAL) {
        host = s_cfg_local_host;
        port = s_cfg_local_port;
        psk = s_cfg_local_psk;
        server_name = "local";
        s_running_local = true;
    } else {
        host = s_cfg_host;
        port = s_cfg_port;
        psk = s_cfg_psk;
        server_name = "remote";
        s_running_remote = true;
    }
    
    time_t start_time = time(NULL);
    char resp_buf[2048];
    char body_buf[1024];

    int backoff = BACKOFF_BASE_SEC;
    int fail_streak = 0;

    while ((server_type == TUNNEL_SERVER_LOCAL && s_running_local) ||
           (server_type == TUNNEL_SERVER_REMOTE && s_running_remote)) {
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
            if (fail_streak >= 3) {
                char buf[128];
                snprintf(buf, sizeof(buf), "tunnel: %s heartbeat failed, retry in %ds", server_name, backoff);
                log_msg(buf);
            }
        }

        /* 休眠，支持 wake 唤醒（分段等待，每次1秒检查一次） */
        int sleep_secs = (server_type == TUNNEL_SERVER_LOCAL) ? backoff / 2 : backoff;  /* 本地服务器重试更快 */
        for (int i = 0; i < sleep_secs && 
             ((server_type == TUNNEL_SERVER_LOCAL && s_running_local) ||
              (server_type == TUNNEL_SERVER_REMOTE && s_running_remote)); i++) {
            if (s_wake_flag) {
                s_wake_flag = false;
                backoff = BACKOFF_BASE_SEC;  /* 唤醒后立即重置退避，不等 */
                break;
            }
            svcSleepThread(1000000000ULL);
        }

        /* 退避递增 */
        if (!ok) {
            backoff = backoff * 2;
            if (backoff > BACKOFF_MAX_SEC) backoff = BACKOFF_MAX_SEC;

            /* 重连前重新加载配置文件（支持热重载）*/
            if (fail_streak % 3 == 0) {
                load_config();
                if (!s_cfg_loaded) {
                    break;  /* 配置丢失，退出线程 */
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

    if (server_type == TUNNEL_SERVER_LOCAL) {
        s_thread_local_active = false;
    } else {
        s_thread_remote_active = false;
    }
}

/* ------------------------------------------------------------------ */
/*  公共 API                                                           */
/* ------------------------------------------------------------------ */

void tunnel_init(void) {
    mutexInit(&s_cmd_mutex);
    mutexInit(&s_status_mutex);
    mutexInit(&s_pctl_mutex);
}

void tunnel_start(void) {
    load_config();
    if (!s_cfg_loaded) {
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
    } else {
        rc = threadStart(&s_thread_remote);
        if (R_FAILED(rc)) {
            s_running_remote = false;
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
        } else {
            rc = threadStart(&s_thread_local);
            if (R_FAILED(rc)) {
                s_running_local = false;
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
        s_wake_flag = true;
        
        /* 非阻塞等待：最多等 3 秒让线程自然退出 */
        if (s_thread_remote_active) {
            for (int i = 0; i < 30 && s_thread_remote_active; i++) {
                svcSleepThread(100000000ULL);  /* 100ms */
            }
        }
        threadClose(&s_thread_remote);
        s_thread_remote_active = false;
    }

    /* 停止本地线程 */
    if (s_running_local) {
        s_running_local = false;
        s_wake_flag = true;
        
        if (s_thread_local_active) {
            for (int i = 0; i < 30 && s_thread_local_active; i++) {
                svcSleepThread(100000000ULL);  /* 100ms */
            }
        }
        threadClose(&s_thread_local);
        s_thread_local_active = false;
    }
}

void tunnel_restart(void) {
    /* 热重载配置（不停止线程，唤醒后立即生效）*/
    s_wake_flag = true;
    load_config();  /* 重新加载配置 */
    
    /* 如果远程线程未运行，启动它 */
    if (!s_running_remote && s_cfg_loaded) {
        s_running_remote = true;
        Result rc = threadCreate(&s_thread_remote, heartbeat_thread_func,
                                 (void*)TUNNEL_SERVER_REMOTE, NULL, 0x10000, 0x2C, -2);
        if (R_SUCCEEDED(rc)) {
            rc = threadStart(&s_thread_remote);
            if (R_SUCCEEDED(rc)) {
                s_thread_remote_active = true;
            }
        }
    }
    
    /* 如果配置了本地服务器且本地线程未运行，启动它 */
    if (s_cfg_local_enabled && !s_running_local) {
        s_running_local = true;
        Result rc = threadCreate(&s_thread_local, heartbeat_thread_func,
                                 (void*)TUNNEL_SERVER_LOCAL, NULL, 0x10000, 0x2C, -2);
        if (R_SUCCEEDED(rc)) {
            rc = threadStart(&s_thread_local);
            if (R_SUCCEEDED(rc)) {
                s_thread_local_active = true;
            }
        }
    }
    
    /* 如果本地服务器被禁用且本地线程正在运行，停止它 */
    if (!s_cfg_local_enabled && s_running_local) {
        s_running_local = false;
        s_wake_flag = true;
        if (s_thread_local_active) {
            for (int i = 0; i < 30 && s_thread_local_active; i++) {
                svcSleepThread(100000000ULL);
            }
        }
        threadClose(&s_thread_local);
        s_thread_local_active = false;
    }
}

bool tunnel_is_running(void) {
    return s_running_remote || s_running_local;
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
    s_wake_flag = true;
}
