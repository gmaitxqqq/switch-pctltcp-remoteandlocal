/**
 * pctl_handler.c - Nintendo Switch PCTL service IPC wrapper
 *
 * Based on switch-parental-timer v11.5 source/main.c
 * (which successfully calls pctl IPC on .nro context).
 *
 * IPC command IDs (verified working on fw 22.1.0):
 *   1451:   StartPlayTimer
 *   1452:   StopPlayTimer
 *   1453:   IsPlayTimerEnabled          (out: bool, inline)
 *   1454:   GetPlayTimerRemainingTime  (out: u64, inline)
 *   1455:   IsRestrictedByPlayTimer   (out: bool, inline)
 *   145601: GetPlayTimerSettings       (out: u16[34], pointer buffer)
 *   195101: SetPlayTimerSettingsForDebug (in:  u16[34], pointer buffer)
 *
 * PlayTimerSettings layout: u16[34] (0x44 bytes)
 *   [0]      header magic  (0x0101 when days are set)
 *   [1]      header flag   (0x0001 when enabled)
 *   [2-6]    reserved      (zero)
 *   Day n (Sun=0 .. Sat=6):
 *     [7+4n+0]  day flag     (0x0600 = configured)
 *     [7+4n+1]  day enable   (0x0100 = restricted, 0x0000 = skip)
 *     [7+4n+2]  day minutes  (0=blocked, 1-1440=limit, 0xFFFF=unlimited)
 *     [7+4n+3]  day padding  (zero)
 */

#include "pctl_handler.h"
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */
static Service s_pctlSrv;
static bool s_initialized = false;

static TimeZoneRule s_tz_rule;
static bool s_tz_rule_loaded = false;

/* ------------------------------------------------------------------ */
/* pctl_init: Use libnx pctlInitialize() for proper service setup      */
/* ------------------------------------------------------------------ */
Result pctl_init(void)
{
    Result rc;

    if (s_initialized)
        return 0;

    rc = pctlInitialize();
    if (R_FAILED(rc))
        return rc;

    Service *srv = pctlGetServiceSession_Service();
    if (srv == NULL) {
        pctlExit();
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }
    s_pctlSrv = *srv;
    s_initialized = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* pctl_exit                                                           */
/* ------------------------------------------------------------------ */
void pctl_exit(void)
{
    if (!s_initialized)
        return;

    pctlExit();
    s_initialized = false;
}

bool pctl_is_initialized(void)
{
    return s_initialized;
}

/* ------------------------------------------------------------------ */
/* Timezone loading for correct day-of-week in sysmodule context       */
/* ------------------------------------------------------------------ */

Result pctl_load_timezone(void)
{
    Result rc;
    TimeLocationName tz_name = {0};

    rc = setsysInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = setsysGetDeviceTimeZoneLocationName(&tz_name);
    setsysExit();

    if (R_FAILED(rc) || tz_name.name[0] == '\0') {
        return rc;
    }

    rc = timeLoadTimeZoneRule(&tz_name, &s_tz_rule);
    if (R_SUCCEEDED(rc)) {
        s_tz_rule_loaded = true;
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* Re-initialize pctl session (needed between certain calls)           */
/* ------------------------------------------------------------------ */
static Result pctl_reinit(void)
{
    pctl_exit();
    return pctl_init();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Result pctl_start_play_timer(void)
{
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatch(&s_pctlSrv, 1451);
}

Result pctl_stop_play_timer(void)
{
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatch(&s_pctlSrv, 1452);
}

Result pctl_is_enabled(bool *enabled)
{
    if (!enabled) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u8 tmp = 0;
    Result rc = serviceDispatchOut(&s_pctlSrv, 1453, tmp);
    if (R_SUCCEEDED(rc))
        *enabled = (tmp != 0);
    return rc;
}

Result pctl_get_remaining_time(u64 *remaining_ns)
{
    if (!remaining_ns) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u64 tmp = 0;
    Result rc = serviceDispatchOut(&s_pctlSrv, 1454, tmp);
    if (R_SUCCEEDED(rc))
        *remaining_ns = tmp;
    return rc;
}

Result pctl_is_restricted(bool *restricted)
{
    if (!restricted) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u8 tmp = 0;
    Result rc = serviceDispatchOut(&s_pctlSrv, 1455, tmp);
    if (R_SUCCEEDED(rc))
        *restricted = (tmp != 0);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Settings read/write                                                  */
/* ------------------------------------------------------------------ */

Result pctl_get_settings(PlayTimerSettings *settings)
{
    if (!settings) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(settings, 0, sizeof(*settings));

    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u16 c[34];
    memset(c, 0, sizeof(c));

    Service *srv = pctlGetServiceSession_Service();
    if (!srv) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result rc = serviceDispatchOut(srv, 145601, c);
    if (R_SUCCEEDED(rc)) {
        memcpy(settings, c, sizeof(c));
    }
    return rc;
}

Result pctl_set_settings(const PlayTimerSettings *settings)
{
    if (!settings) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;

    u16 c[34];
    memcpy(c, settings->raw, sizeof(c));

    Service *srv = pctlGetServiceSession_Service();
    if (!srv) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatchIn(srv, 195101, c);
}

/* ------------------------------------------------------------------ */
/* Day-aware convenience functions                                     */
/* ------------------------------------------------------------------ */

Result pctl_get_day_limit_minutes(int day, u32 *minutes)
{
    if (!minutes || day < 0 || day > 7)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    if (day == 7) {
        *minutes = 0;
        for (int d = 0; d < PCTL_DAYS; d++) {
            u16 m = settings.raw[PCTL_DAY_MINUTES_OFFSET(d)];
            if (m == PT_DAY_NOLIMIT) {
                *minutes = 0;
                return 0;
            }
            if (m > *minutes) *minutes = m;
        }
    } else {
        u16 m = settings.raw[PCTL_DAY_MINUTES_OFFSET(day)];
        *minutes = (m == PT_DAY_NOLIMIT) ? 0 : m;
    }
    return 0;
}

Result pctl_set_day_limit_minutes(int day, u32 minutes)
{
    if (day < 0 || day >= PCTL_DAYS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    u16 val;
    if (minutes == 0) {
        val = PT_DAY_NOLIMIT;
    } else {
        if (minutes > 1440) minutes = 1440;
        val = (u16)minutes;
    }

    if (settings.raw[0] == 0) {
        settings.raw[0] = 0x0101;
        settings.raw[1] = 0x0001;
    }

    settings.raw[PCTL_DAY_FLAG_OFFSET(day)]    = (val != PT_DAY_NOLIMIT) ? 0x0100 : 0x0000;
    settings.raw[PCTL_DAY_MINUTES_OFFSET(day)] = val;

    return pctl_set_settings(&settings);
}

Result pctl_set_daily_limit_minutes(u32 minutes)
{
    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    u16 val;
    if (minutes == 0) {
        val = PT_DAY_NOLIMIT;
    } else {
        if (minutes > 1440) minutes = 1440;
        val = (u16)minutes;
    }

    settings.raw[0] = (val != PT_DAY_NOLIMIT) ? 0x0101 : 0x0000;
    settings.raw[1] = (val != PT_DAY_NOLIMIT) ? 0x0001 : 0x0000;

    for (int d = 0; d < PCTL_DAYS; d++) {
        settings.raw[PCTL_DAY_FLAG_OFFSET(d)]    = (val != PT_DAY_NOLIMIT) ? 0x0100 : 0x0000;
        settings.raw[PCTL_DAY_MINUTES_OFFSET(d)] = val;
    }

    return pctl_set_settings(&settings);
}

int pctl_get_today_day(void)
{
    u64 now_posix = 0;
    Result rc;

    rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &now_posix);
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
        if (R_SUCCEEDED(rc)) {
            return (int)additional.wday;
        }

        if (s_tz_rule_loaded) {
            rc = timeToCalendarTime(&s_tz_rule, now_posix, &cal, &additional);
            if (R_SUCCEEDED(rc)) {
                return (int)additional.wday;
            }
        }

        {
            time_t t = (time_t)now_posix;
            struct tm *tm_info = gmtime(&t);
            if (tm_info)
                return tm_info->tm_wday;
        }
    }

    {
        time_t t = time(NULL);
        if (t != (time_t)-1 && t > 946684800ULL) {
            struct tm *tm_info = localtime(&t);
            if (tm_info)
                return tm_info->tm_wday;
        }
    }

    return 0;
}

Result pctl_get_daily_limit_minutes(u32 *minutes)
{
    int today = pctl_get_today_day();
    return pctl_get_day_limit_minutes(today, minutes);
}

Result pctl_reset_play_time(void)
{
    Result rc;

    rc = pctl_stop_play_timer();
    if (R_FAILED(rc)) return rc;

    PlayTimerSettings settings;
    rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    rc = pctl_set_settings(&settings);
    if (R_FAILED(rc)) return rc;

    rc = pctl_start_play_timer();
    if (R_FAILED(rc)) return rc;

    return 0;
}
