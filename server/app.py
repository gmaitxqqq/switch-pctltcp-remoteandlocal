"""
switch-pctltcp-remote — Server-side API

Lightweight FastAPI service for Switch remote parental control.
Receives heartbeat from Switch, queues commands for delivery.

Endpoints:
  GET  /                  — Admin web dashboard (requires ?key=PSK_ADMIN)
  POST /heartbeat         — Switch heartbeat (PSK_SWITCH auth)
  POST /admin/command     — Admin push command (PSK_ADMIN auth)
  GET  /admin/status      — Admin check status (PSK_ADMIN auth)

Security:
  - Dual Bearer token authentication
  - Dashboard requires admin key in URL parameter (?key=xxx)
  - Path whitelist enforced by WAF
  - Rate limiting recommended at WAF level
"""

from fastapi import FastAPI, HTTPException, Header, Request
from fastapi.responses import HTMLResponse
from pydantic import BaseModel
from collections import deque
from datetime import datetime, timezone, timedelta
import asyncio
import ipaddress
import os

app = FastAPI(title="Switch Parental Control Remote API", version="1.5.0")

# China Standard Time (UTC+8)
CST = timezone(timedelta(hours=8))

# Long polling: server holds heartbeat connection up to this many seconds
LONG_POLL_TIMEOUT = 20

# After this many seconds without a heartbeat, Switch is considered offline
OFFLINE_THRESHOLD = 90

# ---------------------------------------------------------------------------
# Configuration — override via environment variables
# ---------------------------------------------------------------------------
PSK_SWITCH = os.environ.get("PSK_SWITCH", "sw-change-me-to-a-long-random-string")
PSK_ADMIN  = os.environ.get("PSK_ADMIN",  "adm-change-me-to-another-random-string")

# LAN subnet — requests from this network skip the key check for dashboard
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

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------
pending_commands: deque = deque(maxlen=10)
last_seen: dict = {
    "time": None, "ip": None, "version": "",
    "today_limit": -1, "today_played": -1, "today_remaining": -1,
    "weekly_limits": None,
}
cmd_counter: int = 0
# Signal for long polling: wake up any waiting heartbeat connection
cmd_event: asyncio.Event = asyncio.Event()


def _check_auth(authorization: str | None, expected_token: str,
                request: Request = None) -> None:
    """验证 Bearer token，同时支持从 URL ?key= 参数读取（WAF 友好）"""
    # 1. 从 Authorization header 获取
    if authorization and authorization == f"Bearer {expected_token}":
        return
    # 2. 从 URL ?key= 参数获取（Switch 端 WAF 白名单需要）
    if request:
        key = request.query_params.get("key", "")
        if key == expected_token:
            return
    raise HTTPException(status_code=401, detail="unauthorized")


def _next_cmd_id() -> str:
    global cmd_counter
    cmd_counter += 1
    return f"cmd-{cmd_counter:04d}"


# ---------------------------------------------------------------------------
# Admin web dashboard (single-page, self-contained)
# Protected by ?key=PSK_ADMIN URL parameter
# ---------------------------------------------------------------------------
FORBIDDEN_HTML = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>403 禁止访问</title>
<style>
body{display:flex;justify-content:center;align-items:center;min-height:100vh;
  font-family:-apple-system,sans-serif;background:#f0f2f5;color:#888}
.box{text-align:center}
h1{font-size:72px;color:#d9d9d9;margin:0}
p{font-size:16px;margin-top:8px}
</style></head><body>
<div class="box"><h1>403</h1><p>禁止访问</p></div>
</body></html>"""

DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Switch 家长控制</title>
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
.log{font-size:12px;color:#888;max-height:120px;overflow-y:auto;
  padding:8px;background:#fafafa;border-radius:6px;margin-top:8px}
.log-entry{padding:2px 0;border-bottom:1px solid #f0f0f0}
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
.batch-row input:focus{border-color:#1890ff}
</style>
</head>
<body>
<div id="toast" class="toast"></div>

<div class="card">
  <h1>Switch 家长控制</h1>
  <p class="subtitle">远程管理面板</p>
  <div class="status-row">
    <span class="status-label">Switch 状态</span>
    <span id="sw-status" class="status-value offline">离线</span>
  </div>
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
    <span>限额 <em id="today-limit" class="val">--</em></span>
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
    <label>Custom minutes (negative = reduce)</label>
    <input id="custom-min" type="number" placeholder="e.g. 45 or -10" min="-1440" max="1440">
    <div class="btn-group" style="margin-top:8px">
      <button class="btn btn-orange" onclick="sendCmd('add_minutes',+document.getElementById('custom-min').value)">增加时间</button>
      <button class="btn btn-orange" onclick="sendCmd('set_day_limit',+document.getElementById('custom-min').value)">设置今日限额</button>
    </div>
  </div>
  <div class="btn-group" style="margin-top:10px">
    <button class="btn btn-red" onclick="sendCmd('set_day_limit',0)">取消今日限额</button>
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

<div class="card">
  <h1>操作日志</h1>
  <div id="log" class="log"></div>
</div>

<script>
var ADMIN_KEY = '__ADMIN_KEY_PLACEHOLDER__';
// weekly_limits from Switch: [Sun=0, Mon=1, Tue=2, Wed=3, Thu=4, Fri=5, Sat=6]
var weeklyData = null;
// Display order: Mon Tue Wed Thu Fri Sat Sun (Chinese convention)
var dispDays = ['周一','周二','周三','周四','周五','周六','周日'];
var dispIdx  = [1,2,3,4,5,6,0]; // pctl day index for each display position

function $(id){return document.getElementById(id)}
function showToast(msg,ok){
  var t=$('toast');t.textContent=msg;t.className='toast '+(ok?'toast-ok':'toast-err')+' show';
  setTimeout(function(){t.className='toast'},2000);
}
function addLog(msg){
  var d=new Date();var ts=d.toLocaleTimeString();
  var el=$('log');el.innerHTML='<div class="log-entry">['+ts+'] '+msg+'</div>'+el.innerHTML;
}
function fmtMin(m){
  if(m===null||m===undefined||m<0) return '--';
  if(m===0) return '不限';
  var h=Math.floor(m/60);var min=m%60;
  return h>0?(min>0?h+'时'+min+'分':h+'小时'):min+'分钟';
}
function updateWeeklyDisplay(){
  var el=$('weekly-display');
  if(!weeklyData){el.innerHTML='<div style="color:#999;font-size:13px">等待 Switch 上报数据...</div>';return}
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
  $('today-remaining').textContent=fmtMin(remaining);
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
  // Pre-fill day settings from weekly data
  if(weeklyData){
    for(var i=0;i<7;i++){
      var inp=$('d'+i);
      if(inp&&!inp.value&&weeklyData[i]>=0) inp.value=weeklyData[i]===0?'':weeklyData[i];
    }
  }
}
async function sendCmd(action,value,extra){
  if(action!=='set_weekly_limits'&&action!=='reset_play_time'&&action!=='set_day_limit'&&(isNaN(value)||value===0)){showToast('请输入有效数值',false);return}
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
    if(r.ok){showToast('命令已排队: '+d.cmd_id,true);addLog('已发送: '+action+(value?'='+value:'')+' ('+d.cmd_id+')')}
    else{showToast('错误: '+(d.detail||r.status),false);addLog('失败: '+action+(value?'='+value:''))}
  }catch(e){showToast('网络错误',false);addLog('网络错误')}
}
function setWeeklyBatch(){
  var v=+document.getElementById('batch-min').value;
  if(isNaN(v)||v<0){showToast('请输入有效分钟数',false);return}
  sendCmd('set_weekly_limits',0,{weekly:[v,v,v,v,v,v,v]});
}
function setDay(dayIdx){
  var inp=$('d'+dayIdx);
  var v=+inp.value;
  if(isNaN(v)||v<0){showToast('请输入有效分钟数',false);return}
  sendCmd('set_day_limit',v,{day_of_week:dayIdx});
}
async function refreshStatus(){
  try{
    var r=await fetch('/admin/status',{headers:{'Authorization':'Bearer '+ADMIN_KEY}});
    var d=await r.json();
    if(r.ok){
      var ls=d.switch_last_seen;
      if(ls&&ls.online){
        $('sw-status').textContent='在线';$('sw-status').className='status-value online';
        $('sw-time').textContent=ls.time?ls.time.replace('T',' ').substring(0,19):'--';
      }else{
        $('sw-status').textContent='离线';$('sw-status').className='status-value offline';
        if(ls&&ls.time){
          $('sw-time').textContent=ls.time.replace('T',' ').substring(0,19)+' (离线)';
        }else{$('sw-time').textContent='--'}
      }
      $('sw-pending').textContent=d.pending_count;
      if(ls){
        if(ls.weekly_limits){weeklyData=ls.weekly_limits;updateWeeklyDisplay()}
        updateTodayStats(ls);
      }
    }
  }catch(e){}
}
refreshStatus();setInterval(refreshStatus,10000);
</script>
</body>
</html>"""


@app.get("/", response_class=HTMLResponse)
def dashboard(request: Request):
    key = request.query_params.get("key", "")

    # Determine client IP (check proxy headers first)
    client_ip = request.headers.get("x-real-ip") or \
                request.headers.get("x-forwarded-for", "").split(",")[0].strip() or \
                request.client.host if request.client else ""

    # LAN access — no key required, go straight in
    if _is_lan_ip(client_ip):
        html = DASHBOARD_HTML.replace("__ADMIN_KEY_PLACEHOLDER__", PSK_ADMIN)
        return html

    # External access — must have correct key, otherwise 403
    if key != PSK_ADMIN:
        return HTMLResponse(content=FORBIDDEN_HTML, status_code=403)

    # Correct key — serve dashboard with token embedded
    html = DASHBOARD_HTML.replace("__ADMIN_KEY_PLACEHOLDER__", PSK_ADMIN)
    return html


# ---------------------------------------------------------------------------
# Switch endpoints
# ---------------------------------------------------------------------------
class HeartbeatRequest(BaseModel):
    uptime: int = 0
    version: str = ""
    today_limit: int = -1
    today_played: int = -1
    today_remaining: int = -1
    weekly_limits: list[int] | None = None


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

    # If there's already a pending command, return immediately
    if pending_commands:
        cmd = pending_commands.popleft()
        return {"status": "ok", "command": cmd}

    # Long poll: hold connection until command arrives or timeout
    # This gives near-instant command delivery instead of waiting up to 30s
    try:
        await asyncio.wait_for(cmd_event.wait(), timeout=LONG_POLL_TIMEOUT)
        cmd_event.clear()
        if pending_commands:
            cmd = pending_commands.popleft()
            return {"status": "ok", "command": cmd}
    except asyncio.TimeoutError:
        pass  # No command arrived during long poll window

    return {"status": "ok", "command": None}


# ---------------------------------------------------------------------------
# Admin endpoints
# ---------------------------------------------------------------------------
class CommandPush(BaseModel):
    action: str       # "add_minutes" | "set_day_limit" | "reset_play_time" | "set_weekly_limits"
    value: int = 0
    day_of_week: int | None = None   # 0=Sun..6=Sat (pctl day order)
    weekly: list[int] | None = None  # 7 values (Sun..Sat in pctl order)


@app.post("/admin/command")
def push_command(body: CommandPush, authorization: str = Header(None)):
    _check_auth(authorization, PSK_ADMIN)

    valid_actions = {"add_minutes", "set_day_limit", "reset_play_time", "set_weekly_limits"}
    if body.action not in valid_actions:
        raise HTTPException(status_code=400, detail=f"invalid action: {body.action}")

    if body.action == "set_weekly_limits":
        if not body.weekly or len(body.weekly) != 7:
            raise HTTPException(status_code=400, detail="set_weekly_limits requires weekly array of 7 values")
        cmd = {
            "action": body.action,
            "value": 0,
            "weekly": body.weekly,
            "cmd_id": _next_cmd_id(),
        }
    elif body.action == "set_day_limit" and body.day_of_week is not None:
        if body.day_of_week < 0 or body.day_of_week > 6:
            raise HTTPException(status_code=400, detail="day_of_week must be 0-6 (Sun=0..Sat=6)")
        cmd = {
            "action": body.action,
            "value": body.value,
            "day_of_week": body.day_of_week,
            "cmd_id": _next_cmd_id(),
        }
    else:
        cmd = {
            "action": body.action,
            "value": body.value,
            "cmd_id": _next_cmd_id(),
        }

    pending_commands.append(cmd)
    # Wake up any long-polling heartbeat connection
    cmd_event.set()
    return {"status": "ok", "queued": len(pending_commands), "cmd_id": cmd["cmd_id"]}


@app.get("/admin/status")
def status(authorization: str = Header(None)):
    _check_auth(authorization, PSK_ADMIN)

    # Calculate online status based on heartbeat timeout
    result = dict(last_seen)  # copy to avoid modifying global dict
    if result["time"]:
        last = datetime.fromisoformat(result["time"])
        age = (datetime.now(CST) - last).total_seconds()
        result["online"] = age < OFFLINE_THRESHOLD
        result["last_seen_age"] = int(age)
    else:
        result["online"] = False
        result["last_seen_age"] = None

    return {
        "switch_last_seen": result,
        "pending_commands": list(pending_commands),
        "pending_count": len(pending_commands),
    }


# ---------------------------------------------------------------------------
# Health check (no auth — for Docker healthcheck / load balancer)
# ---------------------------------------------------------------------------
@app.get("/health")
def health():
    return {"status": "ok"}
