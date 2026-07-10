// pctltcp-sysmodule - Switch Parental Control (Remote + Local dual tunnel)
// Build: make -> pctltcp-sysmodule.nsp (with APP_JSON)
// Install: sd:/atmosphere/contents/010000000000BD23/exefs.nsp + flags/boot2.flag
//
// v2.0.0: 彻底移除 HTTP 8081 服务端（accept() 不稳定问题）
//         改用远程+本地双隧道架构：Switch 作为客户端主动连接
//         远程服务器 + 本地 PVE 服务器，双通道互为备份
//
// 历史：
//   v1.8.x: HTTP 8081 accept() 持续失败，无法根治
//   v2.0.0: 移除 HTTP 服务端，改为双客户端连接

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "pctl_handler.h"
#include "heartbeat_client.h"

/* ================================================================
 * Sysmodule CRT0 overrides - CRITICAL for boot survival
 * ================================================================ */

#define INNER_HEAP_SIZE 0x80000  /* 512 KiB */

/* ---- CRT0 global overrides ---- */
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 2;

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

    rc = timeInitialize();
    if (R_FAILED(rc)) {
        /* Non-fatal: timestamps may be wrong but module can run */
    }

    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) return rc;

    return 0;
}

/* ---- __appExit ---- */
void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    timeExit();
    smExit();
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

/* ================================================================
 * Network service management
 * ================================================================ */

static bool g_net_up = false;

/* ------------------------------------------------------------------ */
/*  更新隧道状态（主循环调用，读取 pctl 数据供心跳上报）                    */
/* ------------------------------------------------------------------ */

static u32 clamp_remaining_min(u64 remaining_ns) {
    if (remaining_ns == 0)
        return 0;
    if (remaining_ns > 86400000000000ULL)
        return 0;
    return (u32)NS_TO_MINUTES(remaining_ns);
}

static void update_tunnel_status(void) {
    TunnelStatus status;
    memset(&status, -1, sizeof(status));

    tunnel_pctl_lock();
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        tunnel_pctl_unlock();
        return;
    }

    u32 daily_limit = 0;
    if (R_SUCCEEDED(pctl_get_daily_limit_minutes(&daily_limit))) {
        status.today_limit = (int)daily_limit;
    }

    u64 remaining_ns = 0;
    if (R_SUCCEEDED(pctl_get_remaining_time(&remaining_ns))) {
        u32 remaining_min = clamp_remaining_min(remaining_ns);
        status.today_remaining = (int)remaining_min;
        if (status.today_limit >= 0 && status.today_remaining >= 0) {
            status.today_played = status.today_limit - status.today_remaining;
            if (status.today_played < 0) status.today_played = 0;
        }
    }

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

/* ---- 远程命令执行（主线程串行执行，避免 pctl 并发） ---- */

static void execute_tunnel_cmd(TunnelCommand *cmd) {
    if (!cmd || cmd->type == TUNNEL_CMD_NONE) return;

    tunnel_pctl_lock();
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        tunnel_set_last_ack(cmd->cmd_id, "fail");
        tunnel_pctl_unlock();
        log_result("tunnel: pctl_init", rc);
        return;
    }

    switch (cmd->type) {
    case TUNNEL_CMD_ADD_MINUTES: {
        u32 daily_limit = 0;
        pctl_get_daily_limit_minutes(&daily_limit);
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
        tunnel_set_last_ack(cmd->cmd_id, R_SUCCEEDED(rc) ? "success" : "fail");
        return;
    }
    default:
        break;
    }

    pctl_exit();
    tunnel_pctl_unlock();

    tunnel_set_last_ack(cmd->cmd_id, R_SUCCEEDED(rc) ? "success" : "fail");

    char msg[128];
    snprintf(msg, sizeof(msg), "tunnel: cmd %d param=%d dow=%d -> %s (0x%08X)",
             cmd->type, cmd->param, cmd->day_of_week,
             R_SUCCEEDED(rc) ? "OK" : "FAIL", (unsigned)rc);
    log_msg(msg);

    /* 命令执行后：等1秒让pctl生效，更新状态，强制立即心跳上报 */
    if (R_SUCCEEDED(rc)) {
        svcSleepThread(1000000000ULL);   /* 1 second */
        update_tunnel_status();
        tunnel_force_heartbeat();
    }
}

/* ---- Network init ---- */
static Result net_init(void) {
    Result rc;

    rc = nifmInitialize(NifmServiceType_System);
    if (R_FAILED(rc)) {
        rc = nifmInitialize(NifmServiceType_User);
    }
    if (R_FAILED(rc)) {
        log_result("nifmInitialize", rc);
        return rc;
    }

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

    g_net_up = true;
    log_msg("Network services initialized (no HTTP server).");

    /* 启动隧道（远程 + 本地双通道） */
    tunnel_start();
    if (tunnel_is_running()) {
        log_msg("Tunnel started (remote + local).");
    } else {
        log_msg("WARNING: Tunnel failed to start (check tunnel.conf).");
    }

    return 0;
}

/* ---- Network cleanup ---- */
static void net_cleanup(void) {
    tunnel_stop();

    if (g_net_up) {
        socketExit();
        nifmExit();
        log_msg("Network services cleaned up.");
    }
    g_net_up = false;
}

/* ================================================================
 * Main service init - called once at startup
 * ================================================================ */
static bool s_base_ready = false;

static Result init_services(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/pctltcp-sysmodule", 0777);

    log_msg("pctltcp-sysmodule starting (v2.0.0 - remote+local dual tunnel)...");

    tunnel_init();

    {
        Result tz_rc = pctl_load_timezone();
        if (R_FAILED(tz_rc)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "WARNING: timezone load failed (0x%08X)", (unsigned)tz_rc);
            log_msg(buf);
        } else {
            log_msg("Timezone rule loaded successfully.");
        }
    }

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

    Result rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: Service initialization failed, entering idle loop.");
    }

    /* Initialize network with sparse retry. */
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

    log_msg("pctltcp-sysmodule initialization complete (v2.0.0).");

    /* ---- Main loop ----
     * 极简设计：不再有 HTTP 服务端，不再有复杂的 IP 监控。
     * 隧道客户端自己处理重连和退避，主循环只负责：
     *   1. 检测息屏唤醒 → 通知隧道重连
     *   2. 更新 pctl 状态 → 供心跳上报
     *   3. 取出并执行命令
     *   4. 网络恢复重试
     */
    u64 loop = 0;

    while (1) {
        /* ---- Sleep/wake detection ---- */
        u64 t_before = 0;
        timeGetCurrentTime(TimeType_UserSystemClock, &t_before);
        svcSleepThread(1000000000ULL);  /* 1 second (could be longer if slept) */
        u64 t_after = 0;
        timeGetCurrentTime(TimeType_UserSystemClock, &t_after);
        loop++;

        /* Detect time jump > 5 seconds → Switch just woke from sleep */
        if (loop > 5 && g_net_up && (t_after - t_before) > 5) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Wake detected (%llus jump), restarting tunnel...",
                     (unsigned long long)(t_after - t_before));
            log_msg(msg);
            tunnel_restart();
        }

        /* ---- Update tunnel status + execute commands (every 5s) ---- */
        if (g_net_up && (loop % 5 == 0)) {
            update_tunnel_status();

            TunnelCommand cmd;
            int remaining;
            do {
                remaining = tunnel_dequeue_cmd(&cmd);
                if (cmd.type != TUNNEL_CMD_NONE) {
                    execute_tunnel_cmd(&cmd);
                }
            } while (remaining > 0);
        }

        /* ---- Network recovery: if network is not up yet ---- */
        if (!g_net_up && s_base_ready && (loop % 30 == 0)) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) {
                log_msg("Network init succeeded on retry!");
            }
        }

        /* ---- Periodic heartbeat log (every 60s) ---- */
        if ((loop % 60) == 0 && loop > 60) {
            char hb[128];
            snprintf(hb, sizeof(hb),
                     "main loop heartbeat (loop=%llu, net_up=%d)",
                     (unsigned long long)loop, (int)g_net_up);
            log_msg(hb);
        }
    }

    /* Unreachable */
    return 0;
}
