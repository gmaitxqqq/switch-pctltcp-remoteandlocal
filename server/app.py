"""
switch-pctltcp-remote — Server-side API (v2.0 — 增强版)

Lightweight FastAPI service for Switch remote parental control.
Receives heartbeat from Switch, queues commands for delivery.

v2.0 Enhancements:
  - P0-1: Command status tracking system + audit log
  - P0-2: SQLite command persistence via db.py
  - P0-3: Queue full -> HTTP 429 rejection instead of silent drop
  - P1-1: Weekly reset idempotency (skip if already run today)
  - P1-2: Heartbeat acknowledgment mechanism (last_executed_cmd_id/result)
  - P1-3: Dashboard UI overhaul -- command status table with filters
  - P1-4: Enhanced /admin/status with command_records & audit_log
  - P2-1: Health check with uptime, queue depth, switch online, DB status
  - P2-2: Offline warning banner in dashboard

Endpoints:
  GET  /                       -- Admin web dashboard (requires ?key=PSK_ADMIN)
  POST /heartbeat              -- Switch heartbeat (PSK_SWITCH auth)
  POST /admin/command          -- Admin push command (PSK_ADMIN auth)
  GET  /admin/status           -- Admin check status (PSK_ADMIN auth)
  POST /admin/weekly-reset     -- Manually trigger weekly time reclaim (PSK_ADMIN auth)
  GET  /admin/schedule-status  -- Check built-in scheduler status (PSK_ADMIN auth)
  GET  /health                 -- Enhanced health check (no auth)

Security:
  - Dual Bearer token authentication
  - Dashboard requires admin key in URL parameter (?key=xxx)
  - Path whitelist enforced by WAF
  - Rate limiting recommended at WAF level
"""

import json
import logging
import os
import secrets
import time
from dataclasses import dataclass, field
from enum import Enum

from fastapi import FastAPI, HTTPException, Header, Request
from fastapi.responses import HTMLResponse
from pydantic import BaseModel
from collections import deque
from datetime import datetime, timezone, timedelta
from contextlib import asynccontextmanager
import asyncio
import ipaddress

from apscheduler.schedulers.asyncio import AsyncIOScheduler
from apscheduler.triggers.cron import CronTrigger

# Local persistence layer
import db as _db_mod
from db import (
    init_db,
    persist_command,
    load_pending_commands,
    load_recent_commands,
    mark_command_status,
    log_audit,
    get_recent_audit_log,
)

logger = logging.getLogger("switch-pctl")

app = FastAPI(title="Switch Parental Control Remote API", version="2.0.1")

# China Standard Time (UTC+8)
CST = timezone(timedelta(hours=8))

# Long polling: server holds heartbeat connection up to this many seconds
LONG_POLL_TIMEOUT = 20

# After this many seconds without a heartbeat, Switch is considered offline
OFFLINE_THRESHOLD = 90

# Maximum number of pending commands in queue
COMMAND_QUEUE_MAXLEN = 10

# DELIVERED -> timeout in seconds without ACK -> auto EXPIRED (may retry)
DELIVERED_EXPIRE_SECONDS = 300  # 5 minutes

# DELIVERED max retries before permanent expiry
DELIVERED_MAX_RETRIES = 3

# ---------------------------------------------------------------------------
# Configuration -- override via environment variables
# ---------------------------------------------------------------------------
PSK_SWITCH = os.environ.get("PSK_SWITCH", "sw-change-me-to-a-long-random-string")
PSK_ADMIN = os.environ.get("PSK_ADMIN", "adm-change-me-to-another-random-string")

# LAN subnet -- requests from this network skip the key check for dashboard
LAN_SUBNETS_STR = os.environ.get("LAN_SUBNETS", "192.168.0.0/16,10.0.0.0/8,172.16.0.0/12")


def _parse_subnets(raw: str) -> list:
    nets = []
    for s in raw.split(","):
        s = s.strip()
        if not s:
            continue
        try:
            nets.append(ipaddress.ip_network(s, strict=False))
        except ValueError:
            pass
    return nets


LAN_SUBNETS = _parse_subnets(LAN_SUBNETS_STR)

# ---------------------------------------------------------------------------
# Weekly reset scheduler configuration
# ---------------------------------------------------------------------------
WEEKLY_RESET_ENABLED = os.environ.get("WEEKLY_RESET_ENABLED", "true").lower() == "true"
WEEKLY_RESET_CRON = os.environ.get(
    "WEEKLY_RESET_CRON",
    "0 1 * * mon"  # APScheduler day_of_week uses 0=Mon..6=Sun (NOT Unix cron 0=Sun).
                   # 'mon' is unambiguous; '1' would mean TUESDAY (the old bug).
)
WEEKLY_RESET_MINUTES = int(os.environ.get("WEEKLY_RESET_MINUTES", "1"))


def _is_lan_ip(ip_str: str) -> bool:
    if not ip_str or ip_str == "unknown":
        return False
    try:
        addr = ipaddress.ip_address(ip_str)
        for net in LAN_SUBNETS:
            if addr in net:
                return True
    except ValueError:
        pass
    return False


# ===========================================================================
# P0-1: Command Status Tracking System
# ===========================================================================
class CmdStatus(str, Enum):
    """Command lifecycle status."""
    PENDING   = "pending"       # Queued, waiting for Switch pickup
    DELIVERED = "delivered"     # Sent to Switch via heartbeat
    ACKED     = "acked"         # Switch confirmed execution
    FAILED    = "failed"        # Switch reported failure
    EXPIRED   = "expired"       # Timeout not picked up / DELIVERED timed out
    DISCARDED = "discarded"     # Queue full, rejected


@dataclass
class CommandRecord:
    """Complete lifecycle record of one command."""
    cmd_id: str
    action: str
    value: int = 0
    params: dict = field(default_factory=dict)
    status: CmdStatus = CmdStatus.PENDING
    created_at: float = field(default_factory=time.time)
    delivered_at: float | None = None
    acked_at: float | None = None
    error_msg: str | None = None
    source: str = "manual"      # "manual" | "scheduler"
    retry_count: int = 0

    def to_dict(self) -> dict:
        """Serialize to JSON-friendly dict."""
        return {
            "cmd_id": self.cmd_id,
            "action": self.action,
            "value": self.value,
            "params": self.params,
            "status": self.status.value,
            "created_at": datetime.fromtimestamp(self.created_at, tz=CST).strftime(
                "%Y-%m-%d %H:%M:%S"
            ),
            "delivered_at": (
                datetime.fromtimestamp(self.delivered_at, tz=CST).strftime("%Y-%m-%d %H:%M:%S")
                if self.delivered_at else None
            ),
            "acked_at": (
                datetime.fromtimestamp(self.acked_at, tz=CST).strftime("%Y-%m-%d %H:%M:%S")
                if self.acked_at else None
            ),
            "error_msg": self.error_msg,
            "source": self.source,
            "retry_count": self.retry_count,
        }


# ---------------------------------------------------------------------------
# Global State
# ---------------------------------------------------------------------------
# Active command dict: cmd_id -> CommandRecord (replaces old deque)
active_commands: dict[str, CommandRecord] = {}

# Pending queue (deque for FIFO order; maxlen used only as cap, no silent drop)
_pending_queue: deque[str] = deque()

last_seen: dict = {
    "time": None, "ip": None, "version": "",
    "today_limit": -1, "today_played": -1, "today_remaining": -1,
    "weekly_limits": None,
}
cmd_counter: int = 0
# Per-process random suffix so cmd_id is globally unique across multiple
# server instances (remote tunnel + local LAN server). Without it, both
# servers start cmd_counter at 0 and emit identical "cmd-0001", which the
# Switch's dedup ring mis-classifies as already-executed and silently drops
# (while the server marks it ACKED) -> "下发时间有时不生效".
_CMD_ID_SUFFIX = secrets.token_hex(3)  # 6 hex chars, stable for this process
# Signal for long polling: wake up any waiting heartbeat connection
cmd_event: asyncio.Event = asyncio.Event()

# Audit log (in-memory cache, max 500 entries)
command_log: list[dict] = []

# Command history (completed/expired/discarded, max 200 entries)
command_history: list[CommandRecord] = []

# Process start timestamp (for health check)
_process_start_time: float = time.time()

# Weekly reset idempotency tracking
_last_reset_info: dict = {"last_run_date": None, "last_run": None, "last_cmd_id": None}

# DB connection availability flag
_db_available: bool = False
_db_conn = None  # persistent connection for async operations


def _check_auth(authorization: str | None, expected_token: str,
                request: Request = None) -> None:
    """Verify Bearer token, also supports ?key= param (WAF friendly)"""
    if authorization and authorization == f"Bearer {expected_token}":
        return
    if request:
        key = request.query_params.get("key", "")
        if key == expected_token:
            return
    raise HTTPException(status_code=401, detail="unauthorized")


def _next_cmd_id() -> str:
    global cmd_counter
    cmd_counter += 1
    return f"cmd-{cmd_counter:04d}-{_CMD_ID_SUFFIX}"


# ===========================================================================
# Audit Log
# ===========================================================================
def _log_command_event(cmd_id: str, event: str, detail: str = "",
                        source: str = "system") -> None:
    """
    Record a command event to in-memory audit log + async DB write.
    Events: created / delivered / acked / failed / expired / discarded
    """
    entry: dict = {
        "time": datetime.now(CST).strftime("%m-%d %H:%M:%S"),
        "cmd_id": cmd_id,
        "event": event,
        "detail": detail,
        "source": source,
    }
    command_log.insert(0, entry)
    if len(command_log) > 500:
        command_log.pop()
    logger.info("CMD [%s] %s: %s", cmd_id, event, detail)

    # Async DB write (non-blocking)
    if _db_available and _db_conn:
        asyncio.create_task(log_audit(_db_conn, entry))


def _move_to_history(record: CommandRecord) -> None:
    """Move a terminal-state command into history list."""
    command_history.insert(0, record)
    if len(command_history) > 200:
        command_history.pop()
    active_commands.pop(record.cmd_id, None)


def _update_record_status(
    cmd_id: str,
    new_status: CmdStatus,
    error_msg: str | None = None,
) -> CommandRecord | None:
    """
    Update command status, sync to DB and memory history.
    Returns updated record or None.
    """
    rec = active_commands.get(cmd_id)
    if rec is None:
        return None
    old_status = rec.status
    rec.status = new_status
    now = time.time()
    if new_status == CmdStatus.DELIVERED:
        rec.delivered_at = now
    elif new_status in (CmdStatus.ACKED, CmdStatus.FAILED, CmdStatus.EXPIRED):
        rec.acked_at = now
    if error_msg is not None:
        rec.error_msg = error_msg

    # Persist to DB
    if _db_available and _db_conn:
        asyncio.create_task(mark_command_status(
            _db_conn, cmd_id, new_status,
            delivered_at=rec.delivered_at,
            acked_at=rec.acked_at,
            error_msg=error_msg,
            retry_count=rec.retry_count,
        ))

    # Terminal state -> move to history
    if new_status in (CmdStatus.ACKED, CmdStatus.FAILED, CmdStatus.EXPIRED, CmdStatus.DISCARDED):
        _move_to_history(rec)
        _log_command_event(cmd_id, new_status.value, error_msg or f"was {old_status.value}")

    return rec


# ===========================================================================
# P0-3: Queue management helpers
# ===========================================================================
def _queue_is_full() -> bool:
    """Check if active PENDING+DELIVERED count >= COMMAND_QUEUE_MAXLEN."""
    pending_count = sum(
        1 for r in active_commands.values()
        if r.status in (CmdStatus.PENDING, CmdStatus.DELIVERED)
    )
    return pending_count >= COMMAND_QUEUE_MAXLEN


def _enqueue_command(record: CommandRecord) -> None:
    """Add command to queue and active dict. Call only when _queue_is_full() == False."""
    active_commands[record.cmd_id] = record
    _pending_queue.append(record.cmd_id)


def _get_next_pending_cmd() -> CommandRecord | None:
    """
    Pop next PENDING command from queue (FIFO), mark as DELIVERED.
    Returns CommandRecord or None.
    """
    while _pending_queue:
        cid = _pending_queue.popleft()
        rec = active_commands.get(cid)
        if rec is not None and rec.status == CmdStatus.PENDING:
            _update_record_status(cid, CmdStatus.DELIVERED)
            return rec
        # Skip if not found or not PENDING (e.g. already expired/retried)
    return None


def _pending_queue_len() -> int:
    """Return count of PENDING+DELIVERED commands."""
    return sum(
        1 for r in active_commands.values()
        if r.status in (CmdStatus.PENDING, CmdStatus.DELIVERED)
    )


# ===========================================================================
# Weekly Reset (with idempotency + audit log)
# ===========================================================================
async def _do_weekly_reset():
    """
    Queue set_weekly_limits [WEEKLY_RESET_MINUTES]*7.
    Idempotent: skips if already executed today.
    Returns cmd_id or None (skipped).
    """
    today = datetime.now(CST).strftime("%Y-%m-%d")
    if _last_reset_info.get("last_run_date") == today:
        logger.info("Weekly reset already run today (%s), skipping", today)
        return None

    cmd_id = _next_cmd_id()

    record = CommandRecord(
        cmd_id=cmd_id,
        action="set_weekly_limits",
        value=0,
        params={"weekly": [WEEKLY_RESET_MINUTES] * 7},
        source="scheduler",
    )
    _enqueue_command(record)
    if _db_available and _db_conn:
        asyncio.create_task(persist_command(_db_conn, record))
    _log_command_event(
        cmd_id, "created",
        f"set_weekly_limits [{WEEKLY_RESET_MINUTES}]x7",
        "scheduler",
    )

    cmd_event.set()  # wake up long-polling heartbeats

    _last_reset_info["last_run_date"] = today
    _last_reset_info["last_run"] = datetime.now(CST).isoformat()
    _last_reset_info["last_cmd_id"] = cmd_id

    logger.info(
        "Weekly reset: queued set_weekly_limits [%s x7] (cmd_id=%s)",
        WEEKLY_RESET_MINUTES, cmd_id,
    )
    return cmd_id


# ---------------------------------------------------------------------------
# Expired command reclamation background task
# ---------------------------------------------------------------------------
async def _reclaim_expired_commands_loop() -> None:
    """
    Background loop: every 30s check DELIVERED commands past timeout.
    Mark as EXPIRED and re-enqueue if retries remain.
    """
    while True:
        await asyncio.sleep(30)
        try:
            now = time.time()
            for cid, rec in list(active_commands.items()):
                if rec.status != CmdStatus.DELIVERED:
                    continue
                if rec.delivered_at is None:
                    continue
                age = now - rec.delivered_at
                if age < DELIVERED_EXPIRE_SECONDS:
                    continue

                if rec.retry_count < DELIVERED_MAX_RETRIES:
                    # Re-enqueue as PENDING
                    rec.retry_count += 1
                    rec.status = CmdStatus.PENDING
                    rec.delivered_at = None
                    _pending_queue.append(cid)
                    cmd_event.set()  # 唤醒正在长轮询等待的心跳，立即重新投递回收的命令（否则要等长轮询超时+下次心跳，休眠积压命令"触发不了"）
                    if _db_available and _db_conn:
                        asyncio.create_task(persist_command(_db_conn, rec))
                    _log_command_event(
                        cid, "expired",
                        f"DELIVERED {age:.0f}s timeout, retry {rec.retry_count}/{DELIVERED_MAX_RETRIES}",
                        "system",
                    )
                    logger.info(
                        "CMD [%s] expired after %.0fs, re-enqueued (retry %d/%d)",
                        cid, age, rec.retry_count, DELIVERED_MAX_RETRIES,
                    )
                else:
                    # Max retries exceeded -> permanent expiry
                    _update_record_status(
                        cid, CmdStatus.EXPIRED,
                        error_msg=f"DELIVERED timeout, max retries ({DELIVERED_MAX_RETRIES}) exceeded"
                    )
                    logger.warning(
                        "CMD [%s] expired permanently after %d retries", cid, rec.retry_count
                    )
        except Exception as exc:
            logger.error("_reclaim_expired_commands_loop error: %s", exc)


# APScheduler instance
_scheduler: AsyncIOScheduler | None = None


@asynccontextmanager
async def lifespan(_app: FastAPI):
    """Startup/shutdown: init DB, restore pending commands, start schedulers."""
    global _db_available, _scheduler, _db_conn

    # ---- Initialize database ----
    try:
        _db_conn = await init_db()
        _db_available = True
        logger.info("Database initialized successfully")
    except Exception as exc:
        _db_available = False
        logger.error("Database initialization FAILED (running without persistence): %s", exc)

    # ---- Restore unfinished commands ----
    if _db_available and _db_conn:
        try:
            restored = await load_pending_commands(_db_conn)
            for r in restored:
                active_commands[r.cmd_id] = r
                if r.status == CmdStatus.PENDING:
                    _pending_queue.append(r.cmd_id)
            logger.info("Restored %d command(s) from DB (pending=%d)",
                        len(restored), len(_pending_queue))
        except Exception as exc:
            logger.error("Failed to restore pending commands: %s", exc)

    # ---- Start weekly reset scheduler ----
    if WEEKLY_RESET_ENABLED:
        _scheduler = AsyncIOScheduler(timezone=CST)
        try:
            minute, hour, _, _, day_of_week = WEEKLY_RESET_CRON.strip().split()
            _scheduler.add_job(
                _do_weekly_reset,
                CronTrigger(
                    minute=minute,
                    hour=hour,
                    day_of_week=day_of_week,
                    timezone=CST,
                ),
                id="weekly_reset",
                name="Weekly time reclaim",
                replace_existing=True,
            )
            _scheduler.start()
            logger.info(
                "Weekly reset scheduler started: cron='%s' (CST), minutes=%d",
                WEEKLY_RESET_CRON, WEEKLY_RESET_MINUTES,
            )
        except Exception as e:
            logger.error("Failed to start weekly reset scheduler: %s", e)

    # ---- Start expired reclaimer ----
    reclaim_task = asyncio.create_task(_reclaim_expired_commands_loop())
    logger.info("Expired command reclaimer started")

    yield

    # ---- Shutdown ----
    reclaim_task.cancel()
    try:
        await reclaim_task
    except (asyncio.CancelledError, Exception):
        pass

    if _scheduler and _scheduler.running:
        _scheduler.shutdown(wait=False)
        logger.info("Weekly reset scheduler stopped")

    if _db_conn:
        await _db_conn.close()


app.router.lifespan_context = lifespan


# ===========================================================================
# Admin web dashboard (single-page, self-contained)
# ===========================================================================
FORBIDDEN_HTML = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>403 forbidden</title>
<style>
body{display:flex;justify-content:center;align-items:center;min-height:100vh;
  font-family:-apple-system,sans-serif;background:#f0f2f5;color:#888}
.box{text-align:center}
h1{font-size:72px;color:#d9d9d9;margin:0}
p{font-size:16px;margin-top:8px}
</style></head><body>
<div class="box"><h1>403</h1><p>Forbidden</p></div>
</body></html>"""

DASHBOARD_HTML = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Switch Parental Control</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  background:#f0f2f5;min-height:100vh;padding:16px}
.card{background:#fff;border-radius:12px;padding:20px;margin-bottom:16px;
  box-shadow:0 1px 3px rgba(0,0,0,.1)}
h1{font-size:20px;margin-bottom:4px}
.subtitle{color:#666;font-size:13px;margin-bottom:16px}
.status-row{display:flex;justify-content:space-between;padding:8px 0;
  border-bottom:1px solid #f0f0f0;font-size:14px}
.status-row:last-child{border-bottom:none}
.status-label{color:#888}
.status-value{font-weight:500}
.online{color:#52c41a}.offline{color:#ff4d4f}
.btn-group{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:8px}
.btn{padding:14px 8px;border:none;border-radius:8px;font-size:15px;
  font-weight:500;cursor:pointer;transition:opacity .15s}
.btn:active{opacity:.7}
.btn-green{background:#52c41a;color:#fff}
.btn-blue{background:#1890ff;color:#fff}
.btn-orange{background:#fa8c16;color:#fff}
.btn-red{background:#ff4d4f;color:#fff}
.btn-sm{padding:8px 14px;font-size:13px}
.input-group{margin-top:12px}
.input-group label{display:block;font-size:13px;color:#666;margin-bottom:4px}
.input-group input{width:100%;padding:10px;border:1px solid #d9d9d9;
  border-radius:6px;font-size:16px;outline:none}
.input-group input:focus{border-color:#1890ff}
.toast{position:fixed;top:20px;left:50%;transform:translateX(-50%);
  padding:10px 24px;border-radius:8px;color:#fff;font-size:14px;
  z-index:999;opacity:0;transition:opacity .3s}
.toast.show{opacity:1}
.toast-ok{background:#52c41a}.toast-err{background:#ff4d4f}
.progress-wrap{background:#f0f0f0;border-radius:8px;height:12px;margin:12px 0;overflow:hidden}
.progress-bar{height:100%;border-radius:8px;transition:width .5s;background:#52c41a}
.progress-bar.warn{background:#fa8c16}
.progress-bar.danger{background:#ff4d4f}
.progress-bar.nolimit{background:#1890ff}
.stats-row{display:flex;justify-content:space-between;font-size:13px;color:#666;margin-top:4px}
.stats-row span{flex:1;text-align:center}
.stats-row .val{font-weight:600;color:#333}
.weekly-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px 16px;margin-top:8px}
.weekly-item{display:flex;justify-content:space-between;padding:6px 0;
  border-bottom:1px solid #f5f5f5;font-size:14px}
.weekly-item .day{color:#888}
.weekly-item .limit{font-weight:500}
.weekly-item .nolimit{color:#52c41a}
.divider{border:none;border-top:1px solid #eee;margin:14px 0}
.day-row{display:flex;align-items:center;gap:8px;margin-bottom:8px;font-size:14px}
.day-row span{width:36px;color:#666;flex-shrink:0}
.day-row input{flex:1;padding:8px;border:1px solid #d9d9d9;border-radius:6px;
  font-size:14px;outline:none;min-width:0}
.day-row input:focus{border-color:#1890ff}
.batch-row{display:flex;gap:8px;align-items:center;margin-top:6px}
.batch-row input{flex:1;padding:8px;border:1px solid #d9d9d9;border-radius:6px;
  font-size:14px;outline:none}
.custom-row{display:flex;gap:8px;align-items:center;margin-top:6px}
.custom-row input{flex:1;padding:10px;border:1px solid #d9d9d9;border-radius:6px;
  font-size:14px;outline:none}
/* Command table styles */
.filter-bar{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px;font-size:13px}
.filter-btn{padding:4px 12px;border:none;border-radius:4px;cursor:pointer;
  background:#f0f0f0;color:#333;font-size:13px;transition:all .15s}
.filter-btn.active{background:#1890ff;color:#fff}
#cmd-table{overflow-x:auto}
#cmd-table table{width:100%;border-collapse:collapse;font-size:13px}
#cmd-table th{padding:6px 4px;text-align:left;background:#fafafa;
  border-bottom:2px solid #eee;font-weight:600;color:#555}
#cmd-table td{padding:6px 4px;border-bottom:1px solid #f5f5f5;vertical-align:top}
/* Offline warning */
.offline-warning{background:#fff2e0;border:1px solid #ffd591;
  padding:8px;border-radius:6px;margin-top:8px;color:#d46b08;font-size:13px}
</style>
</head>
<body>
<div id="toast" class="toast"></div>

<div class="card">
  <h1>Switch 家长控制</h1>
  <p class="subtitle">远程管理面板 v2.0</p>
  <div class="status-row">
    <span class="status-label">Switch 状态</span>
    <span id="sw-status" class="status-value offline">离线</span>
  </div>
  <div id="offline-warning"></div>
  <div class="status-row">
    <span class="status-label">最近心跳</span>
    <span id="sw-time" class="status-value">--</span>
  </div>
  <div class="status-row">
    <span class="status-label">待执行命令</span>
    <span id="sw-pending" class="status-value">0</span>
  </div>
  <div class="status-row">
    <span class="status-label">响应模式</span>
    <span class="status-value" style="color:#1890ff">长轮询 (秒级响应)</span>
  </div>
</div>

<div class="card">
  <h1>今日状态</h1>
  <div class="progress-wrap"><div id="progress-bar" class="progress-bar" style="width:0%"></div></div>
  <div class="stats-row">
    <span>额度 <em id="today-limit" class="val">--</em></span>
    <span>已玩 <em id="today-played" class="val">--</em></span>
    <span>剩余 <em id="today-remaining" class="val">--</em></span>
  </div>
</div>

<div class="card">
  <h1>快捷操作</h1>
  <p class="subtitle">一键时间控制</p>
  <div class="btn-group">
    <button class="btn btn-green" onclick="sendCmd('add_minutes',5)">+5 分钟</button>
    <button class="btn btn-green" onclick="sendCmd('add_minutes',10)">+10 分钟</button>
    <button class="btn btn-blue" onclick="sendCmd('add_minutes',15)">+15 分钟</button>
    <button class="btn btn-blue" onclick="sendCmd('add_minutes',20)">+20 分钟</button>
  </div>
  <div class="input-group">
    <label>自定义分钟（负数=减少时间）</label>
    <div class="custom-row">
      <input id="custom-min" type="number" placeholder="例如 45 或 -10" min="-1440" max="1440">
      <button class="btn btn-orange btn-sm" onclick="sendCmd('add_minutes',+document.getElementById('custom-min').value)">设置今日时间</button>
  </div>
  <div class="btn-group" style="margin-top:10px">
    <button class="btn btn-red" onclick="sendCmd('set_day_limit',0)">关闭今日倒计时（不限时）</button>
    <button class="btn btn-green" onclick="sendCmd('set_day_limit',120)">开启今日倒计时（2小时）</button>
  </div>
  <hr class="divider">
  <p class="subtitle">每周时间回收</p>
  <div class="btn-group" style="margin-top:8px">
    <button class="btn btn-orange" onclick="weeklyReset()" style="grid-column:1/-1">🗑️ 回收本周所有时间（设为1分钟）</button>
  </div>
</div>

<div class="card">
  <h1>本周配额</h1>
  <p class="subtitle">每日游玩时间限制</p>
  <div id="weekly-display" class="weekly-grid"></div>
</div>

<div class="card">
  <h1>配额设置</h1>
  <p class="subtitle">设置每日游玩时间</p>
  <div class="input-group">
    <label>全部设置为（分钟，0=不限）</label>
    <div class="batch-row">
      <input id="batch-min" type="number" placeholder="例如 120" min="0" max="1440">
      <button class="btn btn-blue btn-sm" onclick="setWeeklyBatch()">一键应用</button>
    </div>
  </div>
  <hr class="divider">
  <div id="day-settings">
    <div class="day-row"><span>周一</span><input id="d1" type="number" placeholder="分钟" min="0" max="1440"><button class="btn btn-blue btn-sm" onclick="setDay(1)">设置</button></div>
    <div class="day-row"><span>周二</span><input id="d2" type="number" placeholder="分钟" min="0" max="1440"><button class="btn btn-blue btn-sm" onclick="setDay(2)">设置</button></div>
    <div class="day-row"><span>周三</span><input id="d3" type="number" placeholder="分钟" min="0" max="1440"><button class="btn btn-blue btn-sm" onclick="setDay(3)">设置</button></div>
    <div class="day-row"><span>周四</span><input id="d4" type="number" placeholder="分钟" min="0" max="1440"><button class="btn btn-blue btn-sm" onclick="setDay(4)">设置</button></div>
    <div class="day-row"><span>周五</span><input id="d5" type="number" placeholder="分钟" min="0" max="1440"><button class="btn btn-blue btn-sm" onclick="setDay(5)">设置</button></div>
    <div class="day-row"><span>周六</span><input id="d6" type="number" placeholder="分钟" min="0" max="1440"><button class="btn btn-blue btn-sm" onclick="setDay(6)">设置</button></div>
    <div class="day-row"><span>周日</span><input id="d0" type="number" placeholder="分钟" min="0" max="1440"><button class="btn btn-blue btn-sm" onclick="setDay(0)">设置</button></div>
  </div>
</div>

<!-- Command Records Table -->
<div class="card">
  <h1>命令记录</h1>
  <div class="filter-bar">
    <button class="filter-btn active" id="filt-all" onclick="filterLog('all')">全部</button>
    <button class="filter-btn" id="filt-pending" onclick="filterLog('pending')">⏳ 待下发</button>
    <button class="filter-btn" id="filt-delivered" onclick="filterLog('delivered')">📨 已送达</button>
    <button class="filter-btn" id="filt-acked" onclick="filterLog('ack'+'ed')">✅ 已确认</button>
    <button class="filter-btn" id="filt-failed" onclick="filterLog('failed')">❌ 失败</button>
  </div>
  <div id="cmd-table">
    <table>
      <thead><tr>
        <th style="padding:6px 4px;text-align:left">时间</th>
        <th style="padding:6px 4px;text-align:left">命令</th>
        <th style="padding:6px 4px;text-left">参数</th>
        <th style="padding:6px 4px;text-align:left">来源</th>
        <th style="padding:6px 4px;text-align:center">状态</th>
      </tr></thead>
      <tbody id="cmd-tbody"></tbody>
    </table>
  </div>
</div>

<!-- Scheduler Info -->
<div class="card">
  <h1>定时回收</h1>
  <div class="status-row"><span class="status-label">调度器</span><span id="sw-schedule" class="status-value">--</span></div>
  <div class="status-row"><span class="status-label">上次运行</span><span id="schedule-last-run" class="status-value">--</span></div>
  <div class="status-row"><span class="status-label">上次命令</span><span id="schedule-last-cmd" class="status-value">--</span></div>
  <div class="status-row"><span class="status-label">下次运行</span><span id="schedule-next" class="status-value">--</span></div>
</div>

<script>
var ADMIN_KEY = '__ADMIN_KEY_PLACEHOLDER__';
var weeklyData = null;
var dispDays = ['\u5468\u4e00','\u5468\u4e8c','\u5468\u4e09','\u5468\u56db','\u5468\u4e94','\u5468\u516d','\u5468\u65e5'];
var dispIdx  = [1,2,3,4,5,6,0];

// Command records state
var currentFilter = 'all';
var cmdRecords = [];

function $(id){return document.getElementById(id)}
function showToast(msg,ok){
  var t=$('toast');t.textContent=msg;t.className='toast '+(ok?'toast-ok':'toast-err')+' show';
  setTimeout(function(){t.className='toast'},2000);
}
function fmtErr(d, status){
  if(d===undefined||d===null) return String(status||'unknown');
  if(typeof d==='object'){ try{ return JSON.stringify(d); }catch(e){ return String(d); } }
  return String(d);
}
function fmtMin(m,isRemaining){
  if(m===null||m===undefined||m<0) return '--';
  if(isRemaining&&m===0) return '\u5df2\u8017\u5c3d';
  if(!isRemaining&&m===0) return '\u4e0d\u9650';
  var h=Math.floor(m/60);var min=m%60;
  return h>0?(min>0?h+'\u65f6'+min+'\u5206':h+'\u5c0f\u65f6'):min+'\u5206\u949f';
}
function updateWeeklyDisplay(){
  var el=$('weekly-display');
  if(!weeklyData){el.innerHTML='<div style="color:#999;font-size:13px">\u7b49\u5f85 Switch \u4e0a\u62a5\u6570\u636e...</div>';return}
  var html='';
  for(var i=0;i<7;i++){
    var pidx=dispIdx[i];
    var limit=weeklyData[pidx];
    var cls=limit===0?'nolimit':'limit';
    html+='<div class="weekly-item"><span class="day">'+dispDays[i]+'</span><span class="'+cls+'">'+fmtMin(limit)+'</span></div>';
  }
  el.innerHTML=html;
}
function updateTodayStats(data){
  var limit=data.today_limit;
  var played=data.today_played;
  var remaining=data.today_remaining;
  $('today-limit').textContent=fmtMin(limit);
  $('today-played').textContent=fmtMin(played);
  $('today-remaining').textContent=fmtMin(remaining,true);
  var bar=$('progress-bar');
  if(limit!==null&&limit>0&&played!==null&&played>=0){
    var pct=Math.min(100,Math.round(played/limit*100));
    bar.style.width=pct+'%';
    bar.className='progress-bar'+(pct>90?' danger':(pct>70?' warn':''));
  }else if(limit===0){
    bar.style.width='100%';bar.className='progress-bar nolimit';
  }else{
    bar.style.width='0%';bar.className='progress-bar';
  }
  if(weeklyData){
    for(var i=0;i<7;i++){
      var inp=$('d'+i);
      if(inp&&!inp.value&&weeklyData[i]>=0) inp.value=weeklyData[i]===0?'':weeklyData[i];
    }
  }
}

/* ---- Command Table Rendering ---- */
function renderCmdTable(){
  var tbody=$('cmd-tbody');
  if(!cmdRecords||cmdRecords.length===0){
    tbody.innerHTML='<tr><td colspan="5" style="padding:20px;text-align:center;color:#999">\u6682\u65e0\u547d\u4ee4\u8bb0\u5f55</td></tr>';
    return;
  }
  var html='';
  var filtered=currentFilter==='all'?cmdRecords:cmdRecords.filter(function(r){return r.status===currentFilter});
  filtered.forEach(function(r){
    var st=r.status;
    var statusMap={
      pending:{text:'⏳ 待下发',cls:'#faad14'},
      delivered:{text:'📨 已送达',cls:'#1890ff'},
      acked:{text:'✅ 已确认',cls:'#52c41a'},
      failed:{text:'❌ 失败',cls:'#ff4d4f'},
      expired:{text:'⏳ 已过期',cls:'#d9d9d9'},
      discarded:{text:'🗑️ 已丢弃',cls:'#d9d9d9'}
    };
    var s=statusMap[st]||{text:st,cls:'#888'};
    html+='<tr style="border-bottom:1px solid #f5f5f5">'+
      '<td style="padding:6px 4px;color:#666">'+esc(r.created_at||'')+'</td>'+
      '<td style="padding:6px 4px;font-weight:500">'+esc(r.action)+'</td>'+
      '<td style="padding:6px 4px">'+fmtParams(r.action,r.value,r.params)+'</td>'+
      '<td style="padding:6px 4px">'+(r.source==='scheduler'?'<span style="color:#fa8c16">\u5b9a\u65f6\u5668</span>':'\u624b\u52a8')+'</td>'+
      '<td style="padding:6px 4px;text-align:center"><span style="color:'+s.cls+';font-weight:500">'+s.text+'</span></td>'+
    '</tr>';
  });
  tbody.innerHTML=html;
}
function fmtParams(action,value,params){
  if(!action) return '-';
  if(action==='set_weekly_limits'&&params&&params.weekly)return params.weekly.join(',');
  if(action==='set_day_limit'&&params&&params.day_of_week!==undefined)
    return ['\u65e5','\u4e00','\u4e8c','\u4e09','\u56db','\u4e94','\u516d'][params.day_of_week]+'='+value;
  return value||'-';
}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function filterLog(f){
  currentFilter=f;
  document.querySelectorAll('.filter-btn').forEach(function(b){
    var isActive=b.id==='filt-'+f;
    b.style.background=isActive?'#1890ff':'#f0f0f0';
    b.style.color=isActive?'#fff':'#333';
  });
  renderCmdTable();
}

async function sendCmd(action,value,extra){
  if(action!=='set_weekly_limits'&&action!=='reset_play_time'&&action!=='set_day_limit'&&(isNaN(value)||value===0)){showToast('\u8bf7\u8f93\u5165\u6709\u6548\u6570\u503c',false);return}
  if(action!=='add_minutes'&&value<0){showToast('Negative only for Add time',false);return}
  try{
    var body={action:action,value:value||0};
    if(extra) Object.assign(body,extra);
    var r=await fetch('/admin/command',{
      method:'POST',
      headers:{'Content-Type':'application/json','Authorization':'Bearer '+ADMIN_KEY},
      body:JSON.stringify(body)
    });
    var d=await r.json();
    if(r.ok){showToast('\u547d\u4ee4\u5df2\u6392\u961f: '+d.cmd_id,true)}
    else{showToast('\u9519\u8bef: '+fmtErr(d.detail, r.status),false)}
  }catch(e){showToast('\u7f51\u7edc\u9519\u8bef',false)}
}
function setWeeklyBatch(){
  var v=+document.getElementById('batch-min').value;
  if(isNaN(v)||v<0){showToast('\u8bf7\u8f93\u5165\u6709\u6548\u5206\u949f\u6570',false);return}
  sendCmd('set_weekly_limits',0,{weekly:[v,v,v,v,v,v,v]});
}
function setDay(dayIdx){
  var inp=$('d'+dayIdx);
  var v=+inp.value;
  if(isNaN(v)||v<0){showToast('\u8bf7\u8f93\u5165\u6709\u6548\u5206\u949f\u6570',false);return}
  sendCmd('set_day_limit',v,{day_of_week:dayIdx});
}
async function weeklyReset(){
  try{
    var r=await fetch('/admin/weekly-reset',{
      method:'POST',
      headers:{'Content-Type':'application/json','Authorization':'Bearer '+ADMIN_KEY}
    });
    var d=await r.json();
    if(r.ok){showToast('\u5df2\u56de\u6536: '+d.message+' ('+d.cmd_id+')',true)}
    else{showToast('\u9519\u8bef: '+fmtErr(d.detail, r.status),false)}
  }catch(e){showToast('\u7f51\u7edc\u9519\u8bef',false);}
}
async function refreshSchedule(){
  try{
    var r=await fetch('/admin/schedule-status',{headers:{'Authorization':'Bearer '+ADMIN_KEY}});
    var d=await r.json();
    if(r.ok&&d.scheduler){
      var s=d.scheduler;
      if(s.enabled){
        var next=s.next_run_cst||'\u8ba1\u7b97\u4e2d...';
        $('sw-schedule').textContent='\u5468\u4e00 01:00 | \u4e0b\u6b21: '+next.substring(0,16);
        $('sw-schedule').style.color='#52c41a';
      }else{
        $('sw-schedule').textContent='\u5df2\u7981\u7528 (WEEKLY_RESET_ENABLED=false)';
        $('sw-schedule').style.color='#fa8c16';
      }
    }
    if(d.last_reset){
      var lr=d.last_reset;
      if(lr.last_run)$('schedule-last-run').textContent=lr.last_run.replace('T',' ').substring(0,19);
      if(lr.last_cmd_id)$('schedule-last-cmd').textContent=lr.last_cmd_id;
    }
    if(d.scheduler&&d.scheduler.next_run_cst)$('schedule-next').textContent=d.scheduler.next_run_cst.substring(0,16);
  }catch(e){}
}

async function refreshStatus(){
  try{
    var r=await fetch('/admin/status',{headers:{'Authorization':'Bearer '+ADMIN_KEY}});
    var d=await r.json();
    if(!r.ok)return;

    var ls=d.switch_last_seen;
    if(ls&&ls.online){
      $('sw-status').textContent='\u5728\u7ebf';$('sw-status').className='status-value online';
      $('sw-time').textContent=ls.time?ls.time.replace('T',' ').substring(0,19):'--';
      $('offline-warning').innerHTML='';
    }else{
      $('sw-status').textContent='\u79bb\u7ebf';$('sw-status').className='status-value offline';
      if(ls&&ls.time){
        $('sw-time').textContent=ls.time.replace('T',' ').substring(0,19)+' (\u79bb\u7ebf)';
      }else{$('sw-time').textContent='--'}
      $('offline-warning').innerHTML=
        '<div class="offline-warning">⚠ Switch 当前离线 — 命令将在上线后自动依次下发</div>';
    }

    $('sw-pending').textContent=d.pending_count;

    if(ls){
      if(ls.weekly_limits){weeklyData=ls.weekly_limits;updateWeeklyDisplay()}
      updateTodayStats(ls);
    }

    // Command records
    if(d.command_records){
      cmdRecords=d.command_records;
      renderCmdTable();
    }

  }catch(e){}
}
refreshStatus();setInterval(refreshStatus,10000);
refreshSchedule();setInterval(refreshSchedule,60000);
updateWeeklyDisplay();
</script>
</body>
</html>"""


@app.get("/", response_class=HTMLResponse)
def dashboard(request: Request):
    key = request.query_params.get("key", "")

    client_ip = request.headers.get("x-real-ip") or \
                request.headers.get("x-forwarded-for", "").split(",")[0].strip() or \
                request.client.host if request.client else ""

    if _is_lan_ip(client_ip):
        return DASHBOARD_HTML.replace("__ADMIN_KEY_PLACEHOLDER__", PSK_ADMIN)

    if key != PSK_ADMIN:
        return HTMLResponse(content=FORBIDDEN_HTML, status_code=403)

    return DASHBOARD_HTML.replace("__ADMIN_KEY_PLACEHOLDER__", PSK_ADMIN)


# ===========================================================================
# Switch endpoints
# ===========================================================================
class HeartbeatRequest(BaseModel):
    uptime: int = 0
    version: str = ""
    today_limit: int = -1
    today_played: int = -1
    today_remaining: int = -1
    weekly_limits: list[int] | None = None
    # P1-2: Ack fields
    last_executed_cmd_id: str | None = None
    last_executed_result: str | None = None   # "success" | "fail" | "timeout"


@app.post("/heartbeat")
async def heartbeat(
    request: Request,
    body: HeartbeatRequest,
    authorization: str = Header(None),
    x_forwarded_for: str = Header(None),
    x_real_ip: str = Header(None),
):
    _check_auth(authorization, PSK_SWITCH, request)

    last_seen["time"] = datetime.now(CST).isoformat()
    last_seen["ip"] = x_real_ip or x_forwarded_for or "unknown"
    last_seen["version"] = body.version
    last_seen["today_limit"] = body.today_limit
    last_seen["today_played"] = body.today_played
    last_seen["today_remaining"] = body.today_remaining
    if body.weekly_limits and len(body.weekly_limits) == 7:
        last_seen["weekly_limits"] = body.weekly_limits

    # P1-2: Process Switch-reported previous command result
    if body.last_executed_cmd_id:
        lid = body.last_executed_cmd_id
        result = body.last_executed_result
        rec = active_commands.get(lid)
        if rec and rec.status == CmdStatus.DELIVERED:
            if result == "success":
                _update_record_status(lid, CmdStatus.ACKED)
                _log_command_event(lid, "acked", "Switch reported success", "switch")
            elif result == "fail":
                _update_record_status(lid, CmdStatus.FAILED,
                                      error_msg="Switch reported execution failure")
                _log_command_event(lid, "failed", "Switch reported failure", "switch")

    # Return next pending command immediately if available
    next_cmd = _get_next_pending_cmd()
    if next_cmd is not None:
        _log_command_event(next_cmd.cmd_id, "delivered",
                            f"action={next_cmd.action}", "system")
        return {"status": "ok", "command": _record_to_switch_cmd(next_cmd)}

    # Long poll: wait for command or timeout
    try:
        await asyncio.wait_for(cmd_event.wait(), timeout=LONG_POLL_TIMEOUT)
        cmd_event.clear()
        next_cmd = _get_next_pending_cmd()
        if next_cmd is not None:
            _log_command_event(next_cmd.cmd_id, "delivered",
                                f"action={next_cmd.action}", "system")
            return {"status": "ok", "command": _record_to_switch_cmd(next_cmd)}
    except asyncio.TimeoutError:
        pass

    return {"status": "ok", "command": None}


def _record_to_switch_cmd(rec: CommandRecord) -> dict:
    """Convert CommandRecord to Switch-compatible format (backward compatible)."""
    cmd: dict = {
        "action": rec.action,
        "value": rec.value,
        "cmd_id": rec.cmd_id,
    }
    if rec.params:
        cmd.update(rec.params)
    return cmd


# ===========================================================================
# Admin endpoints
# ===========================================================================
class CommandPush(BaseModel):
    action: str
    value: int = 0
    day_of_week: int | None = None
    weekly: list[int] | None = None


@app.post("/admin/command")
async def push_command(body: CommandPush, authorization: str = Header(None)):
    _check_auth(authorization, PSK_ADMIN)

    valid_actions = {"add_minutes", "set_day_limit", "reset_play_time", "set_weekly_limits"}
    if body.action not in valid_actions:
        raise HTTPException(status_code=400, detail=f"invalid action: {body.action}")

    # P0-3: Queue full -> HTTP 429 reject
    if _queue_is_full():
        new_cmd_id = _next_cmd_id()
        _log_command_event(new_cmd_id, "discarded",
                           "\u961f\u5217\u5df2\u6ee1(maxlen=10)", "system")
        logger.error(
            "Queue full (max=%d), rejecting: action=%s value=%s",
            COMMAND_QUEUE_MAXLEN, body.action, body.value,
        )
        raise HTTPException(
            status_code=429,
            detail=f"\u547d\u4ee4\u961f\u5217\u5df2\u6ee1({COMMAND_QUEUE_MAXLEN})\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5\uff08Switch \u5728\u7ebf\u65f6\u4f1a\u81ea\u52a8\u6d88\u8d39\uff09",
        )

    # Build command record
    cmd_id = _next_cmd_id()
    params: dict = {}
    if body.action == "set_weekly_limits":
        if not body.weekly or len(body.weekly) != 7:
            raise HTTPException(
                status_code=400,
                detail="set_weekly_limits requires weekly array of 7 values",
            )
        params = {"weekly": body.weekly}
        value = 0
    elif body.action == "set_day_limit" and body.day_of_week is not None:
        if body.day_of_week < 0 or body.day_of_week > 6:
            raise HTTPException(
                status_code=400,
                detail="day_of_week must be 0-6 (Sun=0..Sat=6)",
            )
        params = {"day_of_week": body.day_of_week}
        value = body.value
    else:
        value = body.value

    record = CommandRecord(
        cmd_id=cmd_id,
        action=body.action,
        value=value,
        params=params,
        source="manual",
    )
    _enqueue_command(record)
    if _db_available and _db_conn:
        asyncio.create_task(persist_command(_db_conn, record))
    _log_command_event(
        cmd_id, "created",
        f"{body.action} value={body.value}",
        "manual",
    )
    cmd_event.set()
    return {"status": "ok", "queued": _pending_queue_len(), "cmd_id": cmd_id}


@app.get("/admin/status")
def status(authorization: str = Header(None)):
    _check_auth(authorization, PSK_ADMIN)

    result = dict(last_seen)
    if result["time"]:
        last = datetime.fromisoformat(result["time"])
        age = (datetime.now(CST) - last).total_seconds()
        result["online"] = age < OFFLINE_THRESHOLD
        result["last_seen_age"] = int(age)
    else:
        result["online"] = False
        result["last_seen_age"] = None

    # Build command records (active + recent history)
    records_list: list[dict] = []
    for rec in active_commands.values():
        records_list.append(rec.to_dict())
    history_needed = 50 - len(records_list)
    if history_needed > 0:
        for rec in command_history[:history_needed]:
            records_list.append(rec.to_dict())

    # Audit log (recent 50)
    audit_entries = list(command_log[:50])

    return {
        "switch_last_seen": result,
        "pending_commands": [
            _record_to_switch_cmd(active_commands[cid])
            for cid in _pending_queue
            if cid in active_commands and active_commands[cid].status == CmdStatus.PENDING
        ],
        "pending_count": _pending_queue_len(),
        "command_records": records_list,
        "audit_log": audit_entries,
    }


@app.post("/admin/weekly-reset")
async def weekly_reset_endpoint(authorization: str = Header(None)):
    _check_auth(authorization, PSK_ADMIN)

    cmd_id = await _do_weekly_reset()
    if cmd_id is None:
        return {
            "status": "skipped",
            "message": "Weekly reset already executed today (idempotent)",
            "cmd_id": None,
        }
    return {
        "status": "ok",
        "message": f"All 7 days set to {WEEKLY_RESET_MINUTES} minute(s)",
        "cmd_id": cmd_id,
        "queued": _pending_queue_len(),
    }


@app.get("/admin/schedule-status")
def schedule_status(authorization: str = Header(None)):
    _check_auth(authorization, PSK_ADMIN)

    job_info = None
    if _scheduler and _scheduler.running:
        job = _scheduler.get_job("weekly_reset")
        if job:
            next_run = job.next_run_time
            job_info = {
                "enabled": True,
                "cron": WEEKLY_RESET_CRON,
                "reset_minutes": WEEKLY_RESET_MINUTES,
                "next_run": next_run.isoformat() if next_run else None,
                "next_run_cst": next_run.astimezone(CST).strftime("%Y-%m-%d %H:%M:%S %Z") if next_run else None,
            }
    else:
        job_info = {
            "enabled": WEEKLY_RESET_ENABLED,
            "cron": WEEKLY_RESET_CRON,
            "reset_minutes": WEEKLY_RESET_MINUTES,
            "next_run": None,
            "next_run_cst": None,
            "note": "Scheduler not running",
        }

    return {
        "scheduler": job_info,
        "last_reset": _last_reset_info,
    }


# ===========================================================================
# P2-1: Enhanced health check
# ===========================================================================
@app.get("/health")
async def health() -> dict:
    uptime = time.time() - _process_start_time
    is_online = False
    if last_seen.get("time"):
        try:
            last_ts = datetime.fromisoformat(last_seen["time"])
            is_online = (datetime.now(CST) - last_ts).total_seconds() < OFFLINE_THRESHOLD
        except (ValueError, TypeError):
            pass

    db_ok = _db_available
    if _db_available and _db_conn:
        try:
            cur = await _db_conn.execute("SELECT 1")
            await cur.fetchone()
        except Exception:
            db_ok = False

    return {
        "status": "ok",
        "uptime_seconds": round(uptime, 1),
        "pending_commands": _pending_queue_len(),
        "switch_online": is_online,
        "scheduler_running": (_scheduler is not None and _scheduler.running),
        "db_connected": db_ok,
    }
