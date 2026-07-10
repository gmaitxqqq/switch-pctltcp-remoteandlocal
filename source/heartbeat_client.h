#ifndef HEARTBEAT_CLIENT_H
#define HEARTBEAT_CLIENT_H

#include <switch.h>

// 配置文件路径（SD 卡上）
#define TUNNEL_CONFIG_PATH  "sdmc:/switch/pctltcp-sysmodule/tunnel.conf"

// 心跳 API 路径（固定，无需配置）
#define TUNNEL_HEARTBEAT_PATH  "/heartbeat"

// 默认值（配置文件缺失时的回退）
#define TUNNEL_DEFAULT_PORT          9090
#define TUNNEL_DEFAULT_INTERVAL_SEC  3    // 心跳间隔（秒），长轮询模式下只需短间隔
#define TUNNEL_DEFAULT_RECV_TIMEOUT_SEC     25  // 接收超时需 >= 服务器长轮询时间(20s) + 余量

// 服务器类型（用于区分远程和本地服务器）
typedef enum {
    TUNNEL_SERVER_REMOTE = 0,
    TUNNEL_SERVER_LOCAL = 1,
    TUNNEL_SERVER_COUNT = 2
} TunnelServerType;

// 命令类型
typedef enum {
    TUNNEL_CMD_NONE = 0,
    TUNNEL_CMD_ADD_MINUTES,       // 增加游玩时间（param=分钟数量）
    TUNNEL_CMD_SET_DAY_LIMIT,     // 设置当日时间限额（param=分钟数，0=无限制）
    TUNNEL_CMD_RESET_PLAY_TIME,   // 重置当日游玩时间
    TUNNEL_CMD_SET_WEEKLY_LIMITS, // 设置一周7天限额（weekly[7]数组）
} TunnelCmdType;

// 命令结构
typedef struct {
    TunnelCmdType type;
    int param;                    // 命令参数（分钟数等）
    int day_of_week;              // 指定星期几（0=Sun..6=Sat，-1=未指定）
    int weekly[7];                // 7天限额（Sun=0..Sat=6，-1=未设置）
    char cmd_id[32];              // 服务端下发的命令唯一ID（用于回执与去重）
} TunnelCommand;

// Switch 状态数据（主循环写入，心跳线程读取）
typedef struct {
    int today_limit;              // 今日限额（分钟），-1=未知
    int today_played;             // 今日已玩（分钟），-1=未知
    int today_remaining;          // 今日剩余（分钟），-1=未知
    int weekly_limits[7];         // 一周7天限额（Sun=0..Sat=6），-1=未知
} TunnelStatus;

// 命令队列（线程安全，心跳线程写，主循环读）
#define TUNNEL_CMD_QUEUE_SIZE 8

// 公共 API
void tunnel_init(void);          // 初始化互斥锁（init_services 中调用，必须在任何 lock 之前）
void tunnel_start(void);         // 启动心跳线程（net_init 成功后调用）
void tunnel_stop(void);          // 停止心跳线程
void tunnel_restart(void);       // 设 wake 标志 + 热重载配置（不停止线程，唤醒后立即生效）
bool tunnel_is_running(void);    // 查询状态
int tunnel_dequeue_cmd(TunnelCommand *cmd);  // 从队列取一个命令，返回剩余命令数（0=空）
void tunnel_notify_wake(void);   // 通知心跳线程发生了 sleep/wake，需要重连
void tunnel_force_heartbeat(void); // 命令执行后强制立即心跳（提前跳出睡眠上报新状态）
void tunnel_update_status(const TunnelStatus *status);  // 主循环调用，更新状态数据
void tunnel_pctl_lock(void);    // pctl 互斥锁（主循环和心跳线程互斥调用 pctl）
void tunnel_pctl_unlock(void);

// 命令回执与去重（v2.0 新增，配合服务端 ack 机制）
void tunnel_set_last_ack(const char *cmd_id, const char *result);  // 主循环执行命令后调用，记录回执
bool tunnel_cmd_already_executed(const char *cmd_id, char *out_result, size_t rsz);  // 收命令前查重；已执行过返回 true 并写入当时结果

#endif
