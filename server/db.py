"""
switch-pctltcp-remote — SQLite persistence layer

Lightweight async persistence for command records and audit logs.
Prevents data loss on container restart (e.g. weekly reset queued at 01:00
won't disappear if Docker restarts before Switch comes online).
"""

import json
import logging
import os
import time
from dataclasses import asdict, dataclass, field
from enum import Enum

import aiosqlite

logger = logging.getLogger("switch-pctl.db")

DB_PATH = os.environ.get("DB_PATH", "/data/pctl_commands.db")

# ---------------------------------------------------------------------------
# Data models
# ---------------------------------------------------------------------------

class CmdStatus(str, Enum):
    PENDING   = "pending"      # Queued, waiting for Switch to pick up
    DELIVERED = "delivered"    # Sent to Switch via heartbeat
    ACKED     = "acked"        # Switch confirmed execution
    FAILED    = "failed"       # Switch reported failure
    EXPIRED   = "expired"      # Timeout — not picked up within threshold
    DISCARDED = "discarded"    # Queue full, rejected


@dataclass
class CommandRecord:
    cmd_id: str
    action: str
    value: int = 0
    params: dict = field(default_factory=dict)
    status: CmdStatus = CmdStatus.PENDING
    created_at: float = field(default_factory=time.time)
    delivered_at: float | None = None
    acked_at: float | None = None
    error_msg: str | None = None
    source: str = "manual"       # "manual" | "scheduler"
    retry_count: int = 0


# ---------------------------------------------------------------------------
# Schema init
# ---------------------------------------------------------------------------

async def init_db() -> aiosqlite.Connection:
    """Create tables if not exist, return connection."""
    conn = await aiosqlite.connect(DB_PATH)
    conn.row_factory = aiosqlite.Row
    await conn.executescript("""
        CREATE TABLE IF NOT EXISTS commands (
            cmd_id     TEXT PRIMARY KEY,
            action     TEXT    NOT NULL,
            value      INTEGER DEFAULT 0,
            params     TEXT    DEFAULT '{}',
            status     TEXT    DEFAULT 'pending',
            created_at  REAL    NOT NULL,
            delivered_at REAL,
            acked_at    REAL,
            error_msg  TEXT,
            source     TEXT    DEFAULT 'manual',
            retry_count INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_cmd_status ON commands(status);
        CREATE TABLE IF NOT EXISTS audit_log (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            time       TEXT    NOT NULL,
            cmd_id     TEXT    NOT NULL,
            event      TEXT    NOT NULL,
            detail     TEXT    DEFAULT '',
            source     TEXT    DEFAULT 'system'
        );
    """)
    await conn.commit()
    logger.info("Database initialized: %s", DB_PATH)
    return conn


# ---------------------------------------------------------------------------
# Command CRUD
# ---------------------------------------------------------------------------

async def persist_command(conn: aiosqlite.Connection, record: CommandRecord) -> None:
    """Upsert a command record."""
    await conn.execute(
        """INSERT OR REPLACE INTO commands
           (cmd_id, action, value, params, status, created_at,
            delivered_at, acked_at, error_msg, source, retry_count)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
        (
            record.cmd_id, record.action, record.value,
            json.dumps(record.params), record.status.value,
            record.created_at, record.delivered_at, record.acked_at,
            record.error_msg, record.source, record.retry_count,
        ),
    )
    await conn.commit()


async def load_pending_commands(conn: aiosqlite.Connection) -> list[CommandRecord]:
    """Load all PENDING / DELIVERED commands for recovery after restart."""
    cursor = await conn.execute(
        "SELECT * FROM commands WHERE status IN ('pending', 'delivered') ORDER BY created_at ASC"
    )
    rows = await cursor.fetchall()
    records = []
    for row in rows:
        records.append(CommandRecord(
            cmd_id=row["cmd_id"],
            action=row["action"],
            value=row["value"],
            params=json.loads(row.get("params") or "{}"),
            status=CmdStatus(row["status"]),
            created_at=row["created_at"],
            delivered_at=row["delivered_at"],
            acked_at=row["acked_at"],
            error_msg=row["error_msg"],
            source=row.get("source", "manual"),
            retry_count=row.get("retry_count", 0),
        ))
    logger.info("Loaded %d pending commands from database", len(records))
    return records


async def load_recent_commands(conn: aiosqlite.Connection, limit: int = 50) -> list[CommandRecord]:
    """Load recent commands (all statuses) for Dashboard display."""
    cursor = await conn.execute(
        "SELECT * FROM commands ORDER BY created_at DESC LIMIT ?",
        (limit,),
    )
    rows = await cursor.fetchall()
    records = []
    for row in rows:
        records.append(CommandRecord(
            cmd_id=row["cmd_id"],
            action=row["action"],
            value=row["value"],
            params=json.loads(row.get("params") or "{}"),
            status=CmdStatus(row["status"]),
            created_at=row["created_at"],
            delivered_at=row["delivered_at"],
            acked=row["acked_at"],
            error_msg=row["error_msg"],
            source=row.get("source", "manual"),
            retry_count=row.get("retry_count", 0),
        ))
    return records


async def mark_command_status(
    conn: aiosqlite.Connection,
    cmd_id: str,
    new_status: CmdStatus,
    *,
    delivered_at: float | None = None,
    acked_at: float | None = None,
    error_msg: str | None = None,
    retry_count: int | None = None,
) -> None:
    """Update status of an existing command."""
    sets = ["status = ?"]
    vals = [new_status.value]
    if delivered_at is not None:
        sets.append("delivered_at = ?")
        vals.append(delivered_at)
    if acked_at is not None:
        sets.append("acked_at = ?")
        vals.append(acked_at)
    if error_msg is not None:
        sets.append("error_msg = ?")
        vals.append(error_msg)
    if retry_count is not None:
        sets.append("retry_count = ?")
        vals.append(retry_count)
    vals.append(cmd_id)

    sql = f"UPDATE commands SET {', '.join(sets)} WHERE cmd_id = ?"
    await conn.execute(sql, vals)
    await conn.commit()


# ---------------------------------------------------------------------------
# Audit log
# ---------------------------------------------------------------------------

async def log_audit(conn: aiosqlite.Connection, entry: dict) -> None:
    """Append an entry to the audit_log table."""
    await conn.execute(
        "INSERT INTO audit_log (time, cmd_id, event, detail, source) VALUES (?, ?, ?, ?, ?)",
        (entry["time"], entry["cmd_id"], entry["event"], entry.get("detail", ""), entry.get("source", "system")),
    )
    await conn.commit()


async def get_recent_audit_log(conn: aiosqlite.Connection, limit: int = 50) -> list[dict]:
    """Fetch recent audit log entries, newest first."""
    cursor = await conn.execute(
        "SELECT time, cmd_id, event, detail, source FROM audit_log ORDER BY id DESC LIMIT ?",
        (limit,),
    )
    rows = await cursor.fetchall()
    return [dict(r) for r in rows]


# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

async def cleanup_old_records(conn: aiosqlite.Connection, days: int = 7) -> int:
    """Delete completed/expired/discarded commands older than N days.
    Returns number of rows deleted."""
    cutoff = time.time() - (days * 86400)
    cursor = await conn.execute(
        "DELETE FROM commands WHERE status IN ('acked','expired','discarded') AND created_at < ?",
        (cutoff,),
    )
    deleted = cursor.rowcount
    # Also clean up old audit logs
    await conn.execute(
        "DELETE FROM audit_log WHERE id NOT IN (SELECT id FROM audit_log ORDER BY id DESC LIMIT 500)"
    )
    await conn.commit()
    if deleted:
        logger.info("Cleaned up %d old command records (> %d days)", deleted, days)
    return deleted
