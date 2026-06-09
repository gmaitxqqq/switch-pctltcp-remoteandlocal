/**
 * http_server.c - Minimal HTTP server for Switch parental control
 *
 * REST API:
 *   GET  /              -> Embedded HTML UI
 *   GET  /api/status    -> JSON: {daily_limit_min, remaining_min, played_min, today, today_name, version}
 *   POST /api/allow     -> Add minutes to today's limit (additive)
 *                          body: minutes=N
 *                          calc: new_limit = current_limit + N
 *   Version: v1.8.2
 *
 * Architecture: The HTTP thread runs for the entire lifetime of the sysmodule.
 * It never stops and restarts — instead, http_server_restart() simply closes
 * the old server socket and creates a new one. The thread picks up the new fd
 * on its next select() iteration. This eliminates all thread lifecycle bugs
 * (pthread_join crashes, fd reuse, generation guard races, etc.).
 */
#include "http_server.h"
#include "pctl_handler.h"
#include "heartbeat_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

/* Forward declaration — defined in main.c. NOT variadic! */
extern void log_msg(const char *msg);

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static volatile int  s_server_fd    = -1;   /* server listen socket   */
static volatile int  s_client_fd    = -1;   /* current client socket  */
static volatile bool s_running      = false;
static volatile bool s_thread_alive = false; /* thread exists & looping */
static volatile u32  s_thread_loop_count = 0; /* incremented each loop iteration */
static volatile u32  s_restart_count = 0;  /* total socket-swap restarts */
static volatile bool s_sleep_mode   = false;  /* suppress accept/logs during sleep */
static pthread_t s_thread;

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */
static void http_send(int fd, const char *status, const char *ctype, const char *body)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        status, ctype, (int)strlen(body));
    write(fd, header, hlen);
    write(fd, body, strlen(body));
}

static int http_read_request(int fd, char *buf, int bufsize)
{
    int total = 0;
    while (total < bufsize - 1) {
        int n = read(fd, buf + total, bufsize - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* API handlers                                                        */
/* ------------------------------------------------------------------ */
static u32 clamp_remaining_min(u64 remaining_ns)
{
    if (remaining_ns == 0)
        return 0;
    if (remaining_ns > 86400000000000ULL)
        return 0;
    return (u32)NS_TO_MINUTES(remaining_ns);
}

static void api_status(int fd)
{
    u64 remaining_ns = 0;
    u32 daily_limit  = 0;
    u32 remaining_min = 0;
    u32 played_min    = 0;
    int today = 0;

    tunnel_pctl_lock();
    Result rc = pctl_init();
    if (R_SUCCEEDED(rc)) {
        pctl_get_remaining_time(&remaining_ns);
        pctl_get_daily_limit_minutes(&daily_limit);
        remaining_min = clamp_remaining_min(remaining_ns);
        played_min    = (daily_limit > remaining_min) ? (daily_limit - remaining_min) : 0;
        today = pctl_get_today_day();
        pctl_exit();
    }
    tunnel_pctl_unlock();

    char json[256];
    static const char *day_names[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    snprintf(json, sizeof(json),
        "{\"daily_limit_min\":%u,\"remaining_min\":%u,\"played_min\":%u,\"today\":%d,\"today_name\":\"%s\",\"version\":\"v1.8.2\"}",
        daily_limit, remaining_min, played_min, today, day_names[today]);

    http_send(fd, "200 OK", "application/json", json);
}

static void api_allow(int fd, const char *body)
{
    int allow_min = 0;
    const char *p = strstr(body, "minutes");
    if (p) {
        p = strchr(p + 7, '=');
        if (p) allow_min = atoi(p + 1);  /* 支持负数：减时间 */
    }

    tunnel_pctl_lock();
    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        tunnel_pctl_unlock();
        http_send(fd, "200 OK", "application/json", "{\"success\":0,\"error\":\"pctl_init_failed\"}");
        return;
    }

    int today = pctl_get_today_day();

    if (allow_min == 0) {
        rc = pctl_set_day_limit_minutes(today, 0);
    } else {
        u32 daily_limit = 0;
        pctl_get_daily_limit_minutes(&daily_limit);

        /* 用 int 计算，支持负数减时间，然后 clamp 到 [0, 1440] */
        int new_limit = (int)daily_limit + allow_min;
        if (new_limit < 0) new_limit = 0;
        if (new_limit > 1440) new_limit = 1440;

        rc = pctl_set_day_limit_minutes(today, (u32)new_limit);
        /* 修改限额后重启计时器，强制系统用新限额重新计算剩余时间 */
        if (R_SUCCEEDED(rc)) {
            pctl_stop_play_timer();
            pctl_start_play_timer();
        }
    }

    pctl_exit();
    tunnel_pctl_unlock();

    char json[128];
    snprintf(json, sizeof(json),
        "{\"success\":%d}",
        R_SUCCEEDED(rc) ? 1 : 0);

    http_send(fd, "200 OK", "application/json", json);
}

/* Embedded Web UI                                                     */
/* ------------------------------------------------------------------ */
static const char *WEB_HTML =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Switch Timer v1.8.2</title>"
"<style>"
"body{font-family:sans-serif;background:#1a1a2e;color:#fff;text-align:center;padding:20px;margin:0}"
".box{background:rgba(255,255,255,0.1);border-radius:12px;padding:20px;margin:15px 0}"
".big{font-size:2.5em;font-weight:bold;margin:10px 0}"
".lbl{color:rgba(255,255,255,0.6);font-size:0.9em}"
".row{display:flex;gap:10px;justify-content:center;margin:15px 0}"
".tile{flex:1;background:rgba(255,255,255,0.08);border-radius:10px;padding:14px}"
"input{width:90px;font-size:1.5em;text-align:center;padding:8px;border:none;border-radius:8px;background:rgba(255,255,255,0.15);color:#fff}"
".btns{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;margin:12px 0}"
"button{font-size:1em;padding:10px 18px;border:none;border-radius:8px;background:#3b82f6;color:#fff;cursor:pointer}"
"button:active{transform:scale(0.95)}"
".btn-sm{background:#374151;font-size:0.9em;padding:8px 14px}"
".btn-minus{background:#7f1d1d;font-size:0.9em;padding:8px 14px}"
"#msg{margin-top:8px;color:#fbbf24;font-size:0.9em;min-height:20px}"
".badge{display:inline-block;background:#10b981;color:#fff;font-size:0.7em;padding:2px 8px;border-radius:10px;margin-left:8px}"
"</style>"
"</head>"
"<body>"
"<h2>Switch Parental Control <small>v1.8.2</small> <span class='badge'>LAN + Remote</span></h2>"
"<div class='box'>"
"<div class='row'>"
"<div class='tile'><div class='lbl'>Played</div><div class='big' id='played'>--</div></div>"
"<div class='tile'><div class='lbl'>Remaining</div><div class='big' id='remain'>--</div></div>"
"</div>"
"<div class='lbl' style='margin-top:4px'>Limit: <span id='limit'>--</span> min</div>"
"</div>"
"<div class='box'>"
"<div class='lbl'>Allow to play (minutes)</div>"
"<input type='number' id='min' value='30' min='-1440' max='1440'>"
"<br>"
"<div class='btns'>"
"<button class='btn-minus' onclick='quickSet(-30)'>-30</button>"
"<button class='btn-minus' onclick='quickSet(-10)'>-10</button>"
"<button class='btn-sm' onclick='quickSet(15)'>+15</button>"
"<button class='btn-sm' onclick='quickSet(30)'>+30</button>"
"<button class='btn-sm' onclick='quickSet(60)'>+60</button>"
"<button class='btn-sm' onclick='quickSet(90)'>+90</button>"
"</div>"
"<button onclick='allow()'>Confirm</button>"
"<div id='msg'></div>"
"</div>"
"<script>"
"function load(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('limit').textContent=d.daily_limit_min;"
"document.getElementById('remain').textContent=d.remaining_min+'m';"
"document.getElementById('played').textContent=d.played_min+'m';"
"}).catch(()=>{document.getElementById('msg').textContent='Load failed'});"
"}"
"function quickSet(m){"
"document.getElementById('min').value=m;"
"}"
"function allow(){"
"var m=parseInt(document.getElementById('min').value)||0;"
"document.getElementById('msg').textContent='Saving...';"
"fetch('/api/allow',{method:'POST',body:'minutes='+m}).then(r=>r.json()).then(d=>{"
"document.getElementById('msg').textContent=d.success?'Done!':'Failed';"
"setTimeout(function(){document.getElementById('msg').textContent='';load();},1200);"
"}).catch(()=>{document.getElementById('msg').textContent='Error'});"
"}"
"load();"
"setInterval(load,30000);"
"</script>"
"</body>"
"</html>";

/* ------------------------------------------------------------------ */
/* Route dispatcher                                                    */
/* ------------------------------------------------------------------ */
static void handle_request(int fd)
{
    char buf[2048];
    int n = http_read_request(fd, buf, sizeof(buf));
    if (n <= 0) { close(fd); return; }

    char method[16] = {0}, path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);

    if (strcmp(method, "OPTIONS") == 0) {
        http_send(fd, "204 No Content", "text/plain", "");
        close(fd);
        return;
    }

    char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4;

    if (strcmp(path, "/") == 0 && strcmp(method, "GET") == 0) {
        http_send(fd, "200 OK", "text/html; charset=utf-8", WEB_HTML);
    } else if (strcmp(path, "/api/status") == 0) {
        api_status(fd);
    } else if (strcmp(path, "/api/allow") == 0 && strcmp(method, "POST") == 0) {
        api_allow(fd, body ? body : "");
    } else {
        http_send(fd, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
    }

    close(fd);
}

/* ------------------------------------------------------------------ */
/* Server thread — runs for the entire sysmodule lifetime              */
/*                                                                      */
/* The thread NEVER exits during normal operation. It reads s_server_fd */
/* on each iteration. If s_server_fd == -1, it sleeps and retries.      */
/* http_server_restart() closes the old socket and sets a new one;      */
/* the thread picks it up automatically within one select timeout.       */
/* This eliminates ALL thread lifecycle bugs (no stop/start, no join,   */
/* no fd reuse, no generation guard races).                             */
/* ------------------------------------------------------------------ */
static void *http_thread_func(void *arg)
{
    (void)arg;
    int accept_fail_count = 0;  /* Track consecutive accept() failures */

    {
        char m[128];
        snprintf(m, sizeof(m), "http_thread_func: started (s_server_fd=%d)", s_server_fd);
        log_msg(m);
    }

    while (s_running) {
        if (s_sleep_mode) {
            svcSleepThread(1000000000ULL);  /* 1s — don't accept clients while asleep */
            continue;
        }
        s_thread_loop_count++;

        /* Read the server fd each iteration — never cached locally.
         * This is critical: if http_server_restart() closes the old fd
         * and creates a new one, we must see the new fd, not a stale copy. */
        int fd = s_server_fd;
        if (fd < 0) {
            /* No server socket (between restart/close). Wait and retry. */
            svcSleepThread(200000000ULL);  /* 200ms */
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 500000;  /* 500ms — quick enough to see fd changes */

        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);

        /* Re-read s_server_fd after select — it may have changed during
         * the select call (restart closed old fd, created new one).
         * If it changed, the fd we passed to select is stale — skip accept. */
        if (s_server_fd != fd) continue;

        if (ret < 0) {
            /* select error — fd was probably closed by restart().
             * Don't exit! Just wait for the new fd. */
            svcSleepThread(200000000ULL);
            continue;
        }
        if (ret == 0) continue;

        if (FD_ISSET(fd, &rfds)) {
            /* One more check: the fd might have changed while we were
             * in select(). If so, don't accept on the old fd. */
            if (s_server_fd != fd) continue;

            int client_fd = accept(fd, NULL, NULL);
            if (client_fd < 0) {
                /* accept() failed — check if the socket is fundamentally broken.
                 * Remote tunnel never has this problem because it creates a fresh
                 * socket for every heartbeat. Here we mimic that strategy:
                 * if accept() keeps failing, close the server socket and let
                 * the main loop rebuild it. */
                accept_fail_count++;
                int err = errno;
                if (err == EBADF || err == ENOTSOCK || err == EINVAL || err == EOPNOTSUPP) {
                    /* Fatal socket errors — the listening socket is broken.
                     * Close it and set s_server_fd=-1 so the main loop
                     * (or the next iteration) will create a fresh one. */
                    char m[128];
                    snprintf(m, sizeof(m), "http_thread: accept fatal errno=%d, rebuilding socket", err);
                    log_msg(m);
                    int old_fd = s_server_fd;
                    s_server_fd = -1;
                    close(old_fd);
                    accept_fail_count = 0;
                    svcSleepThread(200000000ULL);
                    continue;
                }
                if (accept_fail_count > 10) {
                    /* Too many consecutive failures — force socket rebuild. */
                    char m[128];
                    snprintf(m, sizeof(m), "http_thread: %d consecutive accept failures, rebuilding", accept_fail_count);
                    log_msg(m);
                    int old_fd = s_server_fd;
                    s_server_fd = -1;
                    close(old_fd);
                    accept_fail_count = 0;
                    svcSleepThread(200000000ULL);
                    continue;
                }
                continue;
            }

            /* Set I/O timeouts on client socket — this is CRITICAL.
             * Without timeouts, read() in http_read_request() can block
             * forever if a client connects but doesn't send data (e.g.,
             * half-open connection after sleep/wake). The thread would
             * be stuck and unable to accept new connections. */
            {
                struct timeval tmo;
                tmo.tv_sec  = 3;
                tmo.tv_usec = 0;
                setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
                setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
            }

            accept_fail_count = 0;

            /* Track client fd so restart() can shutdown() it */
            s_client_fd = client_fd;
            handle_request(client_fd);
            s_client_fd = -1;
        }
    }

    /* Thread is exiting (only happens on final http_server_stop) */
    s_thread_alive = false;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Helper: create and bind a server socket on HTTP_PORT                */
/* Returns the new fd, or -1 on failure.                               */
/* ------------------------------------------------------------------ */
static int create_server_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_msg("create_server_socket: socket() failed");
        return -1;
    }

    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(HTTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg("create_server_socket: bind() failed");
        close(fd);
        return -1;
    }

    if (listen(fd, 4) < 0) {
        log_msg("create_server_socket: listen() failed");
        close(fd);
        return -1;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "create_server_socket: OK (fd=%d, port=%d)", fd, HTTP_PORT);
    log_msg(msg);
    return fd;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void http_server_start(void)
{
    /* If thread is already alive, just ensure there's a server socket */
    if (s_thread_alive) {
        if (s_server_fd < 0) {
            int fd = create_server_socket();
            if (fd >= 0) {
                s_server_fd = fd;
                log_msg("http_server_start: new socket for running thread, OK");
            } else {
                log_msg("http_server_start: create_server_socket failed");
            }
        } else {
            log_msg("http_server_start: already running, OK");
        }
        return;
    }

    /* Create server socket first */
    int fd = create_server_socket();
    if (fd < 0) return;

    s_server_fd = fd;
    s_running   = true;

    /* Create the thread — it will run until http_server_stop() */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 0x10000);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&s_thread, &attr, http_thread_func, NULL);
    s_thread_alive = true;
    pthread_attr_destroy(&attr);

    log_msg("http_server_start: OK");
}

void http_server_stop(void)
{
    s_running = false;

    /* Shutdown client to unblock handle_request */
    if (s_client_fd >= 0) {
        shutdown(s_client_fd, SHUT_RDWR);
    }

    /* Close server socket to unblock select */
    if (s_server_fd >= 0) {
        int fd = s_server_fd;
        s_server_fd = -1;
        close(fd);
    }

    /* Wait for thread to exit. The thread is detached, so its resources
     * are auto-reclaimed when it exits. We do NOT call pthread_join()
     * because that can cause 2168-0002 if the thread is still in a
     * blocking syscall on Horizon OS. */
    if (s_thread_alive) {
        for (int i = 0; i < 50 && s_thread_alive; i++) {
            svcSleepThread(100000000ULL);  /* 100ms, up to 5s */
        }
        if (s_thread_alive) {
            log_msg("http_server_stop: thread did not exit in 5s");
        }
    }
}

void http_server_restart(void)
{
    s_restart_count++;

    /* If socket-swap count exceeded threshold, upgrade to full restart
     * (recreate thread + clean up lwIP state) */
    if (s_restart_count > 30) {
        log_msg("http_server_restart: count exceeded 30, upgrading to full restart");
        http_server_full_restart();
        return;
    }

    /* Shutdown current client if any — unblocks handle_request */
    if (s_client_fd >= 0) {
        shutdown(s_client_fd, SHUT_RDWR);
    }

    /* Close old server socket */
    if (s_server_fd >= 0) {
        int old_fd = s_server_fd;
        s_server_fd = -1;   /* Thread sees -1 → waits instead of selecting */
        close(old_fd);

        /* Give lwIP time to fully clean up the old socket's internal state.
         * Without this delay, creating a new socket immediately can sometimes
         * get the same fd number, causing select() state confusion. */
        svcSleepThread(100000000ULL);  /* 100ms */
    }

    /* Create new server socket */
    int new_fd = create_server_socket();
    if (new_fd < 0) {
        log_msg("http_server_restart: failed to create new socket");
        return;
    }

    /* Set the new fd — thread picks it up on next iteration (within 500ms) */
    s_server_fd = new_fd;

    /* If thread died somehow (shouldn't happen), create a new one */
    if (!s_thread_alive) {
        s_running = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 0x10000);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&s_thread, &attr, http_thread_func, NULL);
        s_thread_alive = true;
        pthread_attr_destroy(&attr);
        log_msg("http_server_restart: thread recreated");
    }

    {
        char cnt_msg[128];
        snprintf(cnt_msg, sizeof(cnt_msg), "http_server_restart: OK (count=%u)", s_restart_count);
        log_msg(cnt_msg);
    }
}

u32 http_server_get_loop_count(void)
{
    return s_thread_loop_count;
}

/* Full restart: stops the HTTP thread completely, waits for cleanup,
 * then creates a brand new thread and socket. This clears any
 * accumulated lwIP internal state (TCP PCBs, netconn structures)
 * that may have been corrupted by many socket create/close cycles. */
void http_server_full_restart(void)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "http_server_full_restart: stopping (was count=%u)", s_restart_count);
    log_msg(msg);

    /* 1. Signal thread to stop */
    s_running = false;

    /* 2. Unblock client if any */
    if (s_client_fd >= 0) {
        shutdown(s_client_fd, SHUT_RDWR);
    }

    /* 3. Close server socket to unblock select() */
    if (s_server_fd >= 0) {
        int old_fd = s_server_fd;
        s_server_fd = -1;
        close(old_fd);
    }

    /* 4. Wait for thread to exit (up to 5s) */
    if (s_thread_alive) {
        for (int i = 0; i < 50 && s_thread_alive; i++) {
            svcSleepThread(100000000ULL);  /* 100ms */
        }
        if (s_thread_alive) {
            log_msg("http_server_full_restart: thread did not exit, forcing");
        }
        s_thread_alive = false;
    }

    /* 5. Give lwIP time to fully clean up all internal state.
     * 500ms is important — during overnight operation, many socket
     * cycles may have left orphaned TCP PCBs in TIME_WAIT or similar states. */
    svcSleepThread(500000000ULL);

    /* 6. Create fresh server socket */
    int fd = create_server_socket();
    if (fd < 0) {
        log_msg("http_server_full_restart: socket creation failed");
        s_restart_count = 0;
        return;
    }

    s_server_fd = fd;
    s_client_fd = -1;
    s_running = true;
    s_restart_count = 0;  /* Reset counter after full restart */

    /* 7. Create new thread */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 0x10000);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&s_thread, &attr, http_thread_func, NULL);
    s_thread_alive = true;
    pthread_attr_destroy(&attr);

    log_msg("http_server_full_restart: OK (thread recreated, count reset)");
}

u32 http_server_get_restart_count(void)
{
    return s_restart_count;
}

bool http_server_is_running(void)
{
    return s_running && s_server_fd >= 0;
}

void http_server_set_sleep_mode(bool mode)
{
    if (s_sleep_mode == mode) return;  /* No state change, skip */
    s_sleep_mode = mode;
    if (mode) {
        log_msg("http_server: entering sleep mode (no accept)");
    } else {
        log_msg("http_server: leaving sleep mode (accept enabled)");
    }
}
