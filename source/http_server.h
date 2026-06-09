/**
 * http_server.h - Lightweight HTTP server for pctltcp-web
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <switch.h>

#define HTTP_PORT  8081

void http_server_start(void);    /* Create thread + bind socket */
void http_server_stop(void);     /* Final shutdown only */
void http_server_restart(void);  /* Rebind socket, thread keeps running */
void http_server_full_restart(void);      /* Full stop+start, recreate thread */
bool http_server_is_running(void);
u32  http_server_get_loop_count(void);  /* Thread health: how many loop iterations */
u32  http_server_get_restart_count(void); /* Total socket-swap restart count */
void http_server_set_sleep_mode(bool mode);  /* Suppress accept/logs during sleep */

#endif /* HTTP_SERVER_H */
