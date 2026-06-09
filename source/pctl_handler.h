/**
 * pctl_handler.h - Nintendo Switch PCTL service wrapper
 *
 * Provides high-level functions for interacting with the Switch
 * parental control (pctl) play timer service.
 *
 * Uses libnx pctlInitialize() + serviceDispatch*() for proper IPC.
 *
 * Sysmodule compatibility:
 *   pctlInitialize() tries pctl:a -> pctl:s -> pctl:r -> pctl
 *   In sysmodule context, pctl:a is likely denied but pctl:s should work.
 *   Read commands work on pctl:s; write commands (195101) may need pctl:a.
 *
 * Based on NX-Pctl-Manager / switch-parental-timer pctl IPC research:
 *   - GetPlayTimerSettings (cmd 145601) returns 0x44 bytes for FW 18.0.0+
 *   - SetPlayTimerSettingsForDebug (cmd 195101) takes 0x44 bytes
 *   - Layout: u16[34], day n at [7+4n] (flag), [7+4n+2] (minutes)
 *   - Day order: Sun=0, Mon=1, ..., Sat=6
 */

#ifndef PCTL_HANDLER_H
#define PCTL_HANDLER_H

#include <switch.h>

#define PCTL_PLAY_TIMER_SETTINGS_SIZE   0x44   /* 68 bytes */
#define PCTL_SETTINGS_U16_COUNT         (PCTL_PLAY_TIMER_SETTINGS_SIZE / 2)  /* 34 */
#define PCTL_DAYS                       7      /* Sun..Sat */
#define PCTL_DAY_FLAG_OFFSET(n)         (7 + 4 * (n))     /* u16 offset */
#define PCTL_DAY_MINUTES_OFFSET(n)      (7 + 4 * (n) + 2) /* u16 offset */
#define PT_DAY_NOLIMIT                  0xFFFFu

/* Play timer settings: raw u16[34] */
#pragma pack(push, 1)
typedef struct {
    u16 raw[PCTL_SETTINGS_U16_COUNT];  /* 68 bytes = 0x44 */
} PlayTimerSettings;
#pragma pack(pop)

_Static_assert(sizeof(PlayTimerSettings) == PCTL_PLAY_TIMER_SETTINGS_SIZE,
    "PlayTimerSettings must be 0x44 bytes");

#define MINUTES_TO_NS(m)  ((u64)(m) * 60ULL * 1000000000ULL)
#define NS_TO_MINUTES(ns) ((u32)((ns) / (60ULL * 1000000000ULL)))

Result pctl_init(void);
void pctl_exit(void);
bool pctl_is_initialized(void);
Result pctl_start_play_timer(void);
Result pctl_stop_play_timer(void);
Result pctl_is_enabled(bool *enabled);
Result pctl_get_remaining_time(u64 *remaining_ns);
Result pctl_is_restricted(bool *restricted);
Result pctl_get_settings(PlayTimerSettings *settings);
Result pctl_set_settings(const PlayTimerSettings *settings);
Result pctl_get_day_limit_minutes(int day, u32 *minutes);
Result pctl_set_day_limit_minutes(int day, u32 minutes);
Result pctl_set_daily_limit_minutes(u32 minutes);
int pctl_get_today_day(void);
Result pctl_get_daily_limit_minutes(u32 *minutes);
Result pctl_reset_play_time(void);
Result pctl_load_timezone(void);

#endif /* PCTL_HANDLER_H */
