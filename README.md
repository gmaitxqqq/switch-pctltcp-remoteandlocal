# switch-pctltcp-remoteandlocal

Nintendo Switch 家长控制 sysmodule，**同时支持远程服务器 + 本地局域网服务器双通道控制**。

## 功能特点

| 通道 | 方式 | 使用场景 | 优势 |
|------|------|----------|------|
| **远程服务器** | 通过公网服务器下发命令 | 外出时远程管理 | 随时随地控制 |
| **本地服务器** | 浏览器访问 PVE 虚拟机上的 FastAPI 服务 | 家里同一个局域网 | 更快更稳，远程不稳定时备用 |

### 双通道优势

- **可靠性**: 远程服务器不稳定时，本地局域网服务器作为备用通道
- **低延迟**: 本地服务器在局域网内，命令响应更快
- **灵活性**: 两个服务器的命令都会被执行（取先到达者）

### 特性

- 开机自启（boot2 sysmodule）
- Web UI 直观操作（本地服务器 + 远程管理面板）
- 按天设置游玩时间限制（5 分钟增量）
- 周配额设置（每天独立限额）
- 实时查看剩余/已玩时间
- **双服务器心跳隧道**（Switch 同时连接远程 + 本地服务器）
- **长轮询**: 命令秒级响应（发指令 ~1-2 秒生效）
- 双 Token 认证（Switch 端 + 管理员端分离）
- 休眠/唤醒自动恢复
- 网络断线自动重连 + 指数退避
- 配置文件热重载（改 tunnel.conf 无需重启 Switch）
- WAF 友好（HTTP/1.1 + User-Agent + PSK in URL）
- 支持负数减少游玩时间

## 快速开始

### Switch 端

#### 前置要求

- Nintendo Switch（Atmosphere CFW）
- 无需自行编译，直接从 [Release](../../releases) 下载最新版本

#### 安装

1. 下载最新 Release 的 `pctltcp-sysmodule.zip`
2. 解压到 SD 卡根目录，确保目录结构为：
   ```
   sdmc:/atmosphere/contents/010000000000BD23/
   ├── exefs.nsp
   ├── toolbox.json
   └── flags/
       └── boot2.flag
   ```
3. 在 SD 卡上创建远程+本地连接配置文件：
   ```
   sdmc:/switch/pctltcp-sysmodule/tunnel.conf
   ```
   内容示例（参考 `tunnel.conf.example`）：
   ```json
   {
       "host": "1.2.3.4",
       "port": 9090,
       "psk": "sw-your-random-string",
       "local_host": "192.168.1.100",
       "local_port": 8000,
       "local_psk": "local-your-random-string"
   }
   ```
   | 字段 | 必填 | 说明 |
   |------|------|------|
   | `host` | ✅ | 远程服务器 IP 或域名 |
   | `port` | ✅ | 远程服务器端口 |
   | `psk` | ✅ | Switch 端 Token（`sw-` 开头） |
   | `local_host` | ❌ | 本地服务器 IP（可选） |
   | `local_port` | ❌ | 本地服务器端口（可选） |
   | `local_psk` | ❌ | 本地服务器 Token（可选） |
   | `interval` | ❌ | 心跳间隔秒数，默认 3 |
   | `connect_timeout` | ❌ | 连接超时秒数，默认 10 |
   | `recv_timeout` | ❌ | 接收超时秒数，默认 25 |

   > ⚠️ `psk` 必须与服务端 `PSK_SWITCH` 一致！
   > > 配置文件修改后无需重启 Switch，下次心跳失败重连时自动热重载。

4. 重启 Switch（或用 Hekate 重启到 CFW）

#### 自行编译

需要 devkitPro + libnx 环境：
```bash
export DEVKITPRO=/path/to/devkitpro
make
```

### 服务端

#### 前置要求

- Docker + Docker Compose
- 雷池 WAF（推荐，提供 TLS 和防护）或任意反向代理

#### 部署

1. 进入 `server/` 目录

2. **修改 Token**（编辑 `docker-compose.yml`）：
   ```yaml
   - PSK_SWITCH=sw-你生成的随机字符串      # 必须与 Switch 端 tunnel.conf 的 psk 一致
   - PSK_ADMIN=adm-另一个不同的随机字符串   # 你自己管理用的 token
   ```

3. 启动服务：
   ```bash
   docker compose up -d --build
   ```

4. 验证服务：
   ```bash
   # 健康检查
   curl http://127.0.0.1:8888/health

   # 模拟 Switch 心跳
   curl -X POST http://127.0.0.1:8888/heartbeat \
     -H "Content-Type: application/json" \
     -H "Authorization: Bearer sw-你的PSK_SWITCH" \
     -d '{"uptime": 0}'

   # 下发命令（给 Switch 增加 30 分钟）
   curl -X POST http://127.0.0.1:8888/admin/command \
     -H "Content-Type: application/json" \
     -H "Authorization: Bearer adm-你的PSK_ADMIN" \
     -d '{"action": "add_minutes", "value": 30}'
   ```

#### 雷池 WAF 配置

在雷池中新建防护站点：
- **后端地址**: `http://127.0.0.1:8888`
- **域名/端口**: 按你的域名或 IP 配置

添加白名单规则（放行 Switch 心跳）：
| 条件 | 说明 |
|------|------|
| URL 包含 `/heartbeat` **且** 包含 `key=sw-` | 只放行带正确 PSK 的心跳请求 |

推荐的自定义规则：
| 规则 | 说明 |
|------|------|
| 路径不在 `/heartbeat` `/admin/` `/health` → 拦截 | 只放行这三个 API |
| `/heartbeat` 只允许 POST | 防止误访问 |
| 请求频率超限 → 拦截 | 防暴力请求 |

## 使用方式

### 局域网控制

浏览器打开 `http://<Switch-IP>:8081`，使用 Web UI 操作。
- 局域网访问无需 Token
- 支持 +5/+10/+15/+20 快捷按钮
- 支持自定义分钟数（含负数减少时间）

### 远程控制

**Web 管理面板**：浏览器访问 `https://你的域名/?key=adm-你的PSK_ADMIN`

**API 调用**：
```bash
# 查看 Switch 在线状态
curl https://你的域名/admin/status \
  -H "Authorization: Bearer adm-你的PSK_ADMIN"

# 增加游玩时间 60 分钟
curl -X POST https://你的域名/admin/command \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer adm-你的PSK_ADMIN" \
  -d '{"action": "add_minutes", "value": 60}'

# 减少游玩时间 10 分钟（负数）
curl -X POST https://你的域名/admin/command \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer adm-你的PSK_ADMIN" \
  -d '{"action": "add_minutes", "value": -10}'

# 设置当日时间限制 120 分钟
curl -X POST https://你的域名/admin/command \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer adm-你的PSK_ADMIN" \
  -d '{"action": "set_day_limit", "value": 120}'

# 设置一周配额（7天，Sun=0..Sat=6）
curl -X POST https://你的域名/admin/command \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer adm-你的PSK_ADMIN" \
  -d '{"action": "set_weekly_limits", "value": 0, "weekly": [60,120,120,120,120,180,180]}'
```

## 架构

```
┌─────────────┐    长轮询     ┌──────────────────────┐   管理API   ┌─────────────┐
│   Switch    │ ───────────> │  远程服务器             │ <───────── │  你的设备    │
│  sysmodule  │ <─────────── │  (公网 IP)            │ ─────────> │  浏览器/curl │
│             │              │  FastAPI + 长轮询       │            │             │
└─────────────┘              └──────────────────────┘            └─────────────┘
       │
       │    局域网心跳
       │
       ▼
┌──────────────────────┐
│  本地服务器            │
│  (PVE 虚拟机)        │
│  FastAPI + Web UI    │
│  http://pve-ip:8000 │
└──────────────────────┘

**双通道工作原理**：
1. Switch 同时连接远程服务器和本地服务器（两个心跳线程）
2. 两个服务器都可以下发命令给 Switch
3. 命令通过线程安全队列传递给主循环执行
4. 远程不稳定时，本地局域网作为备用通道（更快更稳）

## 安全说明

- **双 Token 分离**: Switch 用 `PSK_SWITCH`（只能心跳），管理员用 `PSK_ADMIN`（可下发命令）
- **URL + Header 双重认证**: 支持 `Authorization: Bearer` 和 `?key=` 查询参数，适配 WAF 场景
- **命令队列模式**: 心跳线程不直接调用 pctl IPC，通过线程安全队列交给主循环串行执行
- **雷池 WAF**: 网络层防护，TLS 终止，路径白名单，频率限制
- **配置文件外置**: PSK 不硬编码在源码中，避免 GitHub 泄露密钥

## 文件结构

```
├── source/
│   ├── main.c                 # 主程序入口 + CRT0 + 命令处理
│   ├── http_server.c/h        # 局域网 HTTP 服务端
│   ├── pctl_handler.c/h       # pctl IPC 封装
│   └── heartbeat_client.c/h   # 远程心跳客户端（长轮询 + 配置文件热重载）
├── server/
│   ├── app.py                 # FastAPI 服务端（长轮询 + 双 Token 认证）
│   ├── requirements.txt
│   ├── Dockerfile
│   └── docker-compose.yml
├── .github/workflows/build.yml # CI 自动构建
├── Makefile
├── pctltcp-sysmodule.json     # NPDM 权限配置
├── toolbox.json               # Hekate 工具箱声明
## 版本历史

- **v1.8.2** — **彻底修复** HTTP 服务"假死"问题（每 10 秒误重启一次）。根因：`main.c` 中的健康检查逻辑（判断线程是否卡住）存在根本性缺陷——无论使用 loop count 还是时间戳，都会误判，导致 HTTP 服务被无谓重启，长期运行后 8081 端口无法访问。修复：彻底删除 `main.c` 中的"线程卡住"健康检查，`accept()` 失败后的 socket 重建完全由 `http_server.c` 内部的 `accept_fail_count` 逻辑处理，与远程隧道模块保持一致的设计思路。
- **v1.8.0** — **彻底修复**休眠唤醒后局域网 8081 端口无法访问的问题。根因：`accept()` 失败后不重建 socket，lwIP 内部状态损坏后永远无法恢复。修复：借鉴远程隧道"每次都是新 socket"的思路，在 `accept()` 连续失败时主动关闭并重建 listening socket，彻底解决长期运行后 8081 不通的问题。
- **v1.7.2** — 修复 WiFi 就绪后 LAN 8081 端口绑定问题（在首次获取 IP 后重新绑定）
- **v1.7.1** — 修复罕见情况下心跳线程崩溃的问题，提升稳定性
- **v1.7.0** — 优化 Web UI 响应式布局，支持移动端访问；新增命令执行日志

- **v1.6.0** — 长轮询（秒级命令响应）、负数减少时间、UTC+8 时区、unsigned 溢出修复
- **v1.5.0** — 远程心跳隧道、配置文件化（不再硬编码）、WAF 友好 HTTP/1.1、热重载
- **v1.4.1** — 修复 sleep/wake 检测
- **v1.4** — 健康检查 + 网络恢复
- **v1.3** — 初始版本，局域网 HTTP 控制

## 致谢

- [switch-pctltcp-web](https://github.com/gmaitxqqq/switch-pctltcp-web) — 局域网版基础
- [SysDVR](https://github.com/exelix11/SysDVR) — CI 构建参考
- [sys-con](https://github.com/o0Zz/sys-con) — Makefile NPDM 打包参考
