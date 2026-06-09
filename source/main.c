// pctltcp-sysmodule - Switch Parental Control Web Server (boot2 sysmodule)
// Build: make -> pctltcp-sysmodule.nsp (with APP_JSON)
// Install: sd:/atmosphere/contents/010000000000BD23/exefs.nsp + flags/boot2.flag
//
// v1.7.9: Fix HTTP 8081 unreachable after ANY sleep duration
//         - Always do http_server_full_restart() on wake, regardless of
//           sleep duration and regardless of whether WiFi went down.
//         - Remove g_sleep_enter_loop loop-count check (was buggy:
//           compared loop count diff, not real seconds).
//         - Keep g_wake_sleep_duration for logging only.
//         - Remove did_full_restart flag (always restart on wake now).
// v1.7.7: Fix HTTP 8081 unreachable after sleep (WiFi stayed up case)
//         - Record g_last_wake_loop + g_wake_sleep_duration on EVERY wake,
//           regardless of whether WiFi went down (g_sleep_mode).
//         - Remove spurious continue after sleep/wake detection so IP-check
//           block always runs after wake.
//         - In IP-present block, also do full HTTP restart when WiFi stayed
//           up during sleep but sleep duration >60s (g_last_wake_loop > 0).
// v1.7.6: Add heartbeat + force full HTTP restart after >60s sleep
//         - Add g_sleep_mode flag, set on sleep/wake detection
//         - Add http_server_set_sleep_mode() API (pauses HTTP accept loop)
//         - Skip IP recovery/IP change restarts in sleep mode
//         - HTTP thread sleeps 1s per iteration in sleep mode
//         - Clear sleep mode on IP present (wake)

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "pctl_handler.h"
#include "http_server.h"
#include "heartbeat_client.h"

/* ================================================================
 * Sysmodule CRT0 overrides - CRITICAL for boot survival
 * ================================================================ */

#define INNER_HEAP_SIZE 0x80000  /* 512 KiB - same as sys-con */

/* ---- CRT0 global overrides ---- */
u32 __nx_applet_type = AppletType_None;  /* 0 - no applet */
u32 __nx_fs_num_sessions = 2;            /* FS sessions for sysmodule */

/* ---- Custom heap ---- */
void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

/* ---- __appInit - initialize all needed services ---- */
Result __appInit(void) {
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc)) return rc;

    /* Get firmware version - REQUIRED for libnx version-aware functions */
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc)) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) return rc;

    /* Time service - REQUIRED for correct day-of-week calculation. */
    rc = timeInitialize();
    if (R_FAILED(rc)) {
        /* Non-fatal: pctl day-of-week may be wrong, but module can still run */
    }

    /* DO NOT call smExit() here! We need SM for nifm/pctl later. */
    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) return rc;

    return 0;
}

/* ---- __appExit ---- */
void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    timeExit();
    smExit();  /* SM is kept alive for the entire process lifetime */
}

/* ---- Constants ---- */
#define PROGRAM_ID  0x010000000000BD23ULL
#define LOG_FILE    "sdmc:/switch/pctltcp-sysmodule/sysmodule.log"
#define MAX_LOG_SIZE (100 * 1024)

/* ---- Logging ---- */
static void rotate_log_if_needed(void) {
    FILE *f = fopen(LOG_FILE, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        if (size > MAX_LOG_SIZE) {
            char old[256];
            snprintf(old, sizeof(old), "%s.old", LOG_FILE);
            rename(LOG_FILE, old);
        }
    }
}

void log_msg(const char *msg) {
    rotate_log_if_needed();
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        /* Try all clock sources for reliable timestamps */
        u64 now_posix = 0;
        Result rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &now_posix);
        if (R_FAILED(rc) || now_posix <= 946684800ULL) {
            rc = timeGetCurrentTime(TimeType_LocalSystemClock, &now_posix);
        }
        if (R_FAILED(rc) || now_posix <= 946684800ULL) {
            rc = timeGetCurrentTime(TimeType_UserSystemClock, &now_posix);
        }

        if (R_SUCCEEDED(rc) && now_posix > 946684800ULL) {
            TimeCalendarTime cal;
            TimeCalendarAdditionalInfo additional;

            rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            if (R_FAILED(rc)) {
                /* Try again - sometimes first call fails in sysmodule context */
                rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            }

            if (R_SUCCEEDED(rc)) {
                fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                        cal.year, cal.month, cal.day,
                        cal.hour, cal.minute, cal.second, msg);
                fclose(f);
                return;
            }
        }
        /* Fallback: no timestamp */
        fprintf(f, "[?] %s\n", msg);
        fclose(f);
    }
}

static void log_result(const char *ctx, Result rc) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s (0x%08X)",
             ctx, R_SUCCEEDED(rc) ? "OK" : "FAILED", (unsigned)rc);
    log_msg(buf);
}

/* ---- IP to string ---- */
static void ip_to_str(u32 ip, char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "%d.%d.%d.%d",
             (int)((ip >>  0) & 0xFF),
             (int)((ip >>  8) & 0xFF),
             (int)((ip >> 16) & 0xFF),
             (int)((ip >> 24) & 0xFF));
}

/* ================================================================
 * Network service management
 * ================================================================ */

static bool g_net_up = false;
static u32  g_last_http_loop_count = 0;
static bool g_lan_ip_confirmed = false;  /* 记录是否已确认 LAN IP 可达 */
static u64  g_last_http_restart_loop = 0;    /* Loop counter of last HTTP restart */
#define HTTP_RESTART_COOLDOWN_LOOPS  60        /* Max 1 restart per 60 seconds */
static u64  g_ip_lost_since_loop = 0;         /* Loop when IP was first detected as lost */
#define IP_LOST_RESTART_THRESHOLD   30         /* Only trigger restart after IP lost 30s */
static bool g_sleep_mode = false;               /* Suppress network ops during sleep */
static u64  g_last_wake_loop = 0;             /* Loop when we last detected wake (time jump) */
static u64  g_wake_sleep_duration = 0;         /* Approximate sleep duration in seconds */

/* ------------------------------------------------------------------ */
/*  更新隧道状态（主循环调用，读取 pctl 数据供心跳上报）                    */
/* ------------------------------------------------------------------ */

/* 当计时器耗尽时，pctl_get_remaining_time 可能返回溢出的超大值
 * （> 24 小时），需要钳制为 0 — 参考自 switch-pctltcp-web 项目 */
static u32 clamp_remaining_min(u64 remaining_ns) {
    if (remaining_ns == 0)
        return 0;
    if (remaining_ns > 86400000000000ULL)   /* > 24h 视为耗尽 */
        return 0;
    return (u32)NS_TO_MINUTES(remaining_ns);
}

static void update_tunnel_status(void) {
    TunnelStatus status;
    memset(&status, -1, sizeof(status));

    tunnel_pctl_lock();
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        /* pctl 不可用，跳过 */
        tunnel_pctl_unlock();
        return;
    }

    /* 今日限额 */
    u32 daily_limit = 0;
    if (R_SUCCEEDED(pctl_get_daily_limit_minutes(&daily_limit))) {
        status.today_limit = (int)daily_limit;
    }

    /* 今日剩余时间（钳制溢出值） */
    u64 remaining_ns = 0;
    if (R_SUCCEEDED(pctl_get_remaining_time(&remaining_ns))) {
        u32 remaining_min = clamp_remaining_min(remaining_ns);
        status.today_remaining = (int)remaining_min;
        /* 已玩 = 限额 - 剩余 */
        if (status.today_limit >= 0 && status.today_remaining >= 0) {
            status.today_played = status.today_limit - status.today_remaining;
            if (status.today_played < 0) status.today_played = 0;
        }
    }

    /* 一周7天限额（Sun=0..Sat=6） */
    for (int d = 0; d < 7; d++) {
        u32 day_limit = 0;
        if (R_SUCCEEDED(pctl_get_day_limit_minutes(d, &day_limit))) {
            status.weekly_limits[d] = (int)day_limit;
        }
    }

    pctl_exit();
    tunnel_pctl_unlock();

    tunnel_update_status(&status);
}

/* ---- Network init ---- */
static Result net_init(void) {
    Result rc;

    /* Network interface manager - try System type for sysmodule context */
    rc = nifmInitialize(NifmServiceType_System);
    if (R_FAILED(rc)) {
        rc = nifmInitialize(NifmServiceType_User);
    }
    if (R_FAILED(rc)) {
        log_result("nifmInitialize", rc);
        return rc;
    }

    /* Sockets - use System service type with explicit config */
    SocketInitConfig cfg = {
        .tcp_tx_buf_size = 0x4000,
        .tcp_rx_buf_size = 0x4000,
        .tcp_tx_buf_max_size = 0x10000,
        .tcp_rx_buf_max_size = 0x10000,
        .udp_tx_buf_size = 0x1000,
        .udp_rx_buf_size = 0x4000,
        .sb_efficiency = 2,
        .bsd_service_type = BsdServiceType_System,
    };
    rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        log_result("socketInitialize", rc);
        nifmExit();
        return rc;
    }

    /* HTTP server - start BEFORE waiting for IP.
     * bind(INADDR_ANY) must happen while the interface has no IP.
     * If we start HTTP after IP is up, lwIP may not accept connections
     * on the newly-assigned interface address. */
    http_server_start();
    if (!http_server_is_running()) {
        log_msg("HTTP server start FAILED.");
        socketExit();
        nifmExit();
        return -1;
    }

    g_net_up = true;
    log_msg("Network services initialized, HTTP server started.");

    /* Log IP address (may be 0 at this point) */
    char ip[64] = {0};
    u32 ipaddr = 0;
    Result nifm_rc = nifmGetCurrentIpAddress(&ipaddr);
    if (R_SUCCEEDED(nifm_rc) && ipaddr != 0) {
        ip_to_str(ipaddr, ip, sizeof(ip));
        char msg[256];
        snprintf(msg, sizeof(msg), "Web UI: http://%s:%d", ip, HTTP_PORT);
        log_msg(msg);
    } else {
        log_msg("HTTP server started, waiting for WiFi IP...");
    }

    /* 启动远程隧道 */
    tunnel_start();
    if (tunnel_is_running()) {
        log_msg("Remote tunnel started.");
    } else {
        log_msg("WARNING: Remote tunnel failed to start.");
    }

    return 0;
}

/* ---- Network cleanup ---- */
static void net_cleanup(void) {
    tunnel_stop();

    if (http_server_is_running()) {
        http_server_stop();
        log_msg("HTTP server stopped.");
    }

    if (g_net_up) {
        socketExit();
        nifmExit();
        log_msg("Network services cleaned up.");
    }
    g_net_up = false;
}

/* ---- HTTP server restart (for sleep/wake recovery) ---- */
static Result http_restart(void) {
    /* Wait for WiFi to reconnect: poll nifm for valid IP address. */
    log_msg("Waiting for WiFi to reconnect...");
    int wifi_wait = 0;
    while (wifi_wait < 30) {
        u32 ip = 0;
        Result rc = nifmGetCurrentIpAddress(&ip);
        if (R_SUCCEEDED(rc) && ip != 0) {
            char ipstr[64];
            ip_to_str(ip, ipstr, sizeof(ipstr));
            char msg[256];
            snprintf(msg, sizeof(msg), "WiFi back (IP=%s), restarting HTTP server.", ipstr);
            log_msg(msg);
            break;
        }
        svcSleepThread(1000000000ULL);
        wifi_wait++;
    }
    if (wifi_wait >= 30) {
        log_msg("WiFi not back after 30s, restarting HTTP server anyway.");
    }

    svcSleepThread(200000000ULL); /* 0.2s 短暂等待 WiFi 稳定 */

    /* Use restart() instead of stop+start — the thread keeps running,
     * we just swap the server socket. This eliminates all thread lifecycle
     * bugs (fd reuse, generation races, pthread_join crashes). */
    http_server_restart();
    if (!http_server_is_running()) {
        log_msg("HTTP server restart FAILED.");
        return -1;
    }

    /* Reset the thread loop counter so the health check starts fresh */
    g_last_http_loop_count = 0;

    log_msg("HTTP server restarted successfully.");
    return 0;
}

/* ---- 远程命令执行（主线程串行执行，避免 pctl 并发） ---- */

static void execute_tunnel_cmd(TunnelCommand *cmd) {
    if (!cmd || cmd->type == TUNNEL_CMD_NONE) return;

    tunnel_pctl_lock();
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        tunnel_pctl_unlock();
        log_result("tunnel: pctl_init", rc);
        return;
    }

    switch (cmd->type) {
    case TUNNEL_CMD_ADD_MINUTES: {
        u32 daily_limit = 0;
        pctl_get_daily_limit_minutes(&daily_limit);
        /* 使用有符号运算，防止 unsigned 溢出导致负数变成超大值 */
        int new_limit = (int)daily_limit + cmd->param;
        if (new_limit < 0) new_limit = 0;
        if (new_limit > 1440) new_limit = 1440;
        int today = pctl_get_today_day();
        rc = pctl_set_day_limit_minutes(today, (u32)new_limit);
        if (R_SUCCEEDED(rc)) {
            pctl_stop_play_timer();
            pctl_start_play_timer();
        }
        break;
    }
    case TUNNEL_CMD_SET_DAY_LIMIT: {
        if (cmd->day_of_week >= 0 && cmd->day_of_week <= 6) {
            rc = pctl_set_day_limit_minutes(cmd->day_of_week, (u32)cmd->param);
        } else {
            int today = pctl_get_today_day();
            rc = pctl_set_day_limit_minutes(today, (u32)cmd->param);
        }
        break;
    }
    case TUNNEL_CMD_RESET_PLAY_TIME: {
        rc = pctl_reset_play_time();
        break;
    }
    case TUNNEL_CMD_SET_WEEKLY_LIMITS: {
        int ok_count = 0;
        for (int d = 0; d < 7; d++) {
            if (cmd->weekly[d] >= 0) {
                Result day_rc = pctl_set_day_limit_minutes(d, (u32)cmd->weekly[d]);
                if (R_SUCCEEDED(day_rc)) ok_count++;
            }
        }
        if (ok_count > 0) rc = 0;
        else rc = -1;
        char msg[64];
        snprintf(msg, sizeof(msg), "tunnel: set_weekly_limits %d/7 days ok", ok_count);
        log_msg(msg);
        pctl_exit();
        tunnel_pctl_unlock();
        return;
    }
    default:
        break;
    }

    pctl_exit();
    tunnel_pctl_unlock();

    char msg[128];
    snprintf(msg, sizeof(msg), "tunnel: cmd %d param=%d dow=%d -> %s (0x%08X)",
             cmd->type, cmd->param, cmd->day_of_week,
             R_SUCCEEDED(rc) ? "OK" : "FAIL", (unsigned)rc);
    log_msg(msg);
}

/* ================================================================
 * Main service init - called once at startup
 * ================================================================ */
static bool s_base_ready = false;

static Result init_services(void) {
    /* Create log directory */
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/pctltcp-sysmodule", 0777);

    log_msg("pctltcp-sysmodule starting (v1.8.2 - remote tunnel)...");

    /* 初始化隧道模块的互斥锁（必须在 tunnel_update_status 之前） */
    tunnel_init();

    /* Load timezone rule for correct day-of-week calculation. */
    {
        Result tz_rc = pctl_load_timezone();
        if (R_FAILED(tz_rc)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "WARNING: timezone load failed (0x%08X), day-of-week may be wrong", (unsigned)tz_rc);
            log_msg(buf);
        } else {
            log_msg("Timezone rule loaded successfully.");
        }
    }

    /* Log time service status */
    {
        u64 test_time = 0;
        Result time_rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &test_time);
        if (R_FAILED(time_rc)) {
            time_rc = timeGetCurrentTime(TimeType_UserSystemClock, &test_time);
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "Time service: %s (time=%llu, rc=0x%08X)",
                 R_SUCCEEDED(time_rc) ? "OK" : "FAILED",
                 (unsigned long long)test_time, (unsigned)time_rc);
        log_msg(buf);
    }

    s_base_ready = true;
    return 0;
}

/* ================================================================
 * sysmodule entry point
 * ================================================================ */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* At boot2, many services are not yet registered with SM. */
    svcSleepThread(15000000000ULL);  /* 15 seconds */

    /* Initialize base services (time, timezone, etc.) */
    Result rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: Service initialization failed, entering idle loop.");
    }

    /* Initialize network (socket, nifm, HTTP server) with sparse retry. */
    if (s_base_ready) {
        for (int attempt = 0; attempt < 30; attempt++) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) break;
            svcSleepThread(5000000000ULL);  /* 5 seconds */
        }
        if (R_FAILED(rc)) {
            log_msg("WARNING: Network init failed after 30 retries");
        }
    }

    log_msg("pctltcp-sysmodule initialization complete.");

    /* ---- Main loop ---- */
    u64 loop = 0;
    char last_ip[64] = {0};
    u64 last_ip_check = 0;
    int nifm_fail_count = 0;
    u64 g_sleep_enter_loop = 0;  /* loop when sleep mode was entered */

    while (1) {
        /* ---- Sleep/wake detection ---- */
        u64 t_before = 0;
        timeGetCurrentTime(TimeType_UserSystemClock, &t_before);
        svcSleepThread(1000000000ULL);  /* 1 second (could be longer if slept) */
        u64 t_after = 0;
        timeGetCurrentTime(TimeType_UserSystemClock, &t_after);
        loop++;

        /* ---- Main loop heartbeat (every 60s) ---- */
        if ((loop % 60) == 0 && loop > 60) {
            char hb[256];
            snprintf(hb, sizeof(hb),
                     "main loop heartbeat (loop=%llu, sleep_mode=%d, net_up=%d)",
                     (unsigned long long)loop, (int)g_sleep_mode, (int)g_net_up);
            log_msg(hb);
        }

        if (loop > 5 && g_net_up && (t_after - t_before) > 5) {
            /* Time jump detected — Switch just woke from sleep.
             * Record wake time and sleep duration for the IP-check block
             * to decide whether a full HTTP restart is needed.
             * This runs regardless of whether WiFi stayed up or not. */
            g_last_wake_loop = loop;
            g_wake_sleep_duration = (t_after - t_before);

            /* Check if WiFi is still alive right now.
             * If yes, we just woke up and don't need sleep mode.
             * If no, enter sleep mode and wait for WiFi recovery. */
            u32 test_ip = 0;
            Result test_rc = nifmGetCurrentIpAddress(&test_ip);

            if (R_SUCCEEDED(test_rc) && test_ip != 0) {
                /* WiFi survived sleep — no sleep mode needed.
                 * Fall through to IP-check block below, which will
                 * see g_last_wake_loop > 0 and decide on full restart. */
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Wake detected (%llus jump), WiFi still up",
                         (unsigned long long)(t_after - t_before));
                log_msg(msg);
                tunnel_restart();   /* 设 wake 标志 + 热重载配置 */
                nifm_fail_count = 0;
                /* Do NOT enter sleep mode — WiFi is fine */
                /* Do NOT continue — let IP-check block run below */
            } else {
                /* WiFi is down — enter sleep mode */
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Sleep/wake detected (%llus jump), WiFi down, entering sleep mode...",
                         (unsigned long long)(t_after - t_before));
                log_msg(msg);
                g_sleep_mode = true;
                g_sleep_enter_loop = loop;  /* Record when we entered sleep */
                http_server_set_sleep_mode(true);
                tunnel_restart();
                nifm_fail_count = 0;
            }
            /* NO continue — fall through to IP-check block */
        }

        /* ---- Health check: HTTP server running? ---- */
        /* Only check if the server is completely down (socket invalid).
         * The "thread stuck" check has been removed because it produces
         * false positives — the HTTP thread can be blocked in select()
         * or accept() without being truly stuck.
         * Socket-level recovery is handled internally by accept_fail_count
         * in http_server.c (accept_fail_count logic). */
        if (g_net_up && !g_sleep_mode && (loop % 5 == 0)) {
            if (!http_server_is_running()) {
                log_msg("HTTP server down, reinitializing network...");
                http_restart();
                g_last_http_restart_loop = loop;
                nifm_fail_count = 0;
                continue;
            }


            /* 更新隧道状态（供心跳上报） */
            update_tunnel_status();

            /* 从队列取出并执行命令（串行） */
            TunnelCommand cmd;
            int remaining;
            do {
                remaining = tunnel_dequeue_cmd(&cmd);
                if (cmd.type != TUNNEL_CMD_NONE) {
                    execute_tunnel_cmd(&cmd);
                }
            } while (remaining > 0);
        }

        /* ---- Health check: nifm responsive? ---- */
        /* Skip during sleep mode — nifm calls may fail temporarily
         * while WiFi chip is in low-power state. */
        if (g_net_up && !g_sleep_mode && (loop % 10 == 0)) {
            u32 ipaddr = 0;
            Result nifm_rc = nifmGetCurrentIpAddress(&ipaddr);
            if (R_FAILED(nifm_rc)) {
                nifm_fail_count++;
                if (nifm_fail_count >= 3) {
                    log_msg("nifm unresponsive (3 failures), reinitializing...");
                    http_restart();
                    g_last_http_restart_loop = loop;
                    nifm_fail_count = 0;
                    continue;
                }
            } else {
                nifm_fail_count = 0;
            }
        }

        /* ---- Startup retry: if network is not up yet ---- */
        if (!g_net_up && s_base_ready && (loop % 30 == 0)) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) {
                log_msg("Network init succeeded on retry!");
            }
        }

        /* ---- Periodic restart if HTTP died but net is still up ---- */
        if (g_net_up && !g_sleep_mode && (loop % 60 == 0) && !http_server_is_running()) {
            log_msg("HTTP server down (periodic check), reinitializing...");
            http_restart();
            g_last_http_restart_loop = loop;
            nifm_fail_count = 0;
        }

        /* ---- Check for IP change / first IP obtained ---- */
        if (g_net_up && (loop - last_ip_check >= 10)) {   /* every 10s */
            char new_ip[64] = {0};
            u32 a = 0;
            Result ip_rc = nifmGetCurrentIpAddress(&a);
            if (R_SUCCEEDED(ip_rc) && a != 0) {
                ip_to_str(a, new_ip, sizeof(new_ip));
            }

            if (new_ip[0] == 0) {
                /* WiFi briefly lost during sleep/wake.
                 * Only reset confirmed flag after SUSTAINED loss (30+ seconds).
                 * Brief dropouts should NOT trigger restart. */
                if (g_ip_lost_since_loop == 0) {
                    g_ip_lost_since_loop = loop;
                }
                if ((loop - g_ip_lost_since_loop) > IP_LOST_RESTART_THRESHOLD) {
                    g_lan_ip_confirmed = false;
                }
            } else {
                /* IP is present — wake recovery already handled before
                 * this block (g_last_wake_loop > 0 trigger). */
                if (g_sleep_mode) {
                    g_sleep_mode = false;
                    http_server_set_sleep_mode(false);
                    g_last_http_loop_count = 0;
                    log_msg("Wake: sleep mode cleared (full restart already done)");
                }

                g_ip_lost_since_loop = 0;

                if (!g_lan_ip_confirmed) {
                    /* IP came back after sustained loss — need restart,
                     * but respect cooldown to avoid rapid restarts.
                     * Skip restart in sleep mode (will restart on wake). */
                    g_lan_ip_confirmed = true;
                    if (!g_sleep_mode && (loop - g_last_http_restart_loop) >= HTTP_RESTART_COOLDOWN_LOOPS) {
                        g_last_http_restart_loop = loop;
                        char m[256];
                        snprintf(m, sizeof(m),
                                 "LAN IP restored (%s) after sustained loss, restarting HTTP...",
                                 new_ip);
                        log_msg(m);
                        http_restart();
                    } else {
                        if (g_sleep_mode)
                            log_msg("LAN IP restored (sleep mode, skip restart)");
                        else
                            log_msg("LAN IP restored (cooldown active, skip restart)");
                    }
                }

                if (strcmp(last_ip, new_ip) != 0) {
                    /* IP actually changed (different address) — always restart,
                     * but still respect cooldown.
                     * Skip restart in sleep mode (will restart on wake). */
                    char m[256];
                    snprintf(m, sizeof(m), "IP changed: %s -> %s",
                             last_ip[0] ? last_ip : "(none)", new_ip);
                    log_msg(m);
                    strcpy(last_ip, new_ip);
                    {
                        char u[256];
                        snprintf(u, sizeof(u), "Web UI: http://%s:%d", new_ip, HTTP_PORT);
                        log_msg(u);
                    }
                    if (!g_sleep_mode && (loop - g_last_http_restart_loop) >= HTTP_RESTART_COOLDOWN_LOOPS) {
                        g_last_http_restart_loop = loop;
                        http_restart();
                    } else {
                        if (g_sleep_mode)
                            log_msg("IP changed (sleep mode, skip restart)");
                    }
                }
            }
            last_ip_check = loop;
        }

        /* ---- Periodic full HTTP reinitialization (every ~4 hours) ---- */
        /* After many socket create/close cycles, lwIP internal state
         * (TCP PCBs, netconn structures) may accumulate leaks or corruption.
         * A full restart (stop thread + start fresh) clears all state. */
        if (g_net_up && !g_sleep_mode && loop > 100 && (loop % 14400 == 0)) {
            {
                char m[128];
                snprintf(m, sizeof(m), "Periodic full HTTP reinit (loop=%llu, restart_count=%u)",
                         (unsigned long long)loop, http_server_get_restart_count());
                log_msg(m);
            }
            http_server_full_restart();
            g_last_http_loop_count = 0;
            g_last_http_restart_loop = loop;
        }
    }

    /* Unreachable */
    return 0;
}
