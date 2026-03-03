# MimiClaw 架构 (Architecture)

> ESP32-S3 AI Agent 固件 — 基于裸机 C/FreeRTOS 实现（无 Linux）。

---

## 系统概览 (System Overview)

```
Telegram App (用户)
    │
    │  HTTPS 长轮询 (Long Polling)
    │
    ▼
┌──────────────────────────────────────────────────┐
│               ESP32-S3 (MimiClaw)                │
│                                                  │
│   ┌─────────────┐       ┌──────────────────┐     │
│   │  Telegram    │──────▶│   入站队列        │     │
│   │  轮询器      │       └────────┬─────────┘     │
│   │  (Core 0)    │               │                │
│   └─────────────┘               ▼                │
│                     ┌────────────────────────┐    │
│   ┌─────────────┐  │     Agent 循环         │    │
│   │  WebSocket   │─▶│     (Core 1)           │    │
│   │  服务器      │  │                        │    │
│   │  (:18789)    │  │  上下文 ──▶ LLM 代理   │    │
│   └─────────────┘  │  构建器      (HTTPS)   │    │
│                     │       ▲          │      │    │
│   ┌─────────────┐  │       │     工具调用?   │    │
│   │  串口 CLI    │  │       │          ▼      │    │
│   │  (Core 0)    │  │  工具结果 ◀─ 工具      │    │
│   └─────────────┘  │              (网页搜索)  │    │
│                     └──────────┬─────────────┘    │
│                                │                  │
│                         ┌──────▼───────┐          │
│                         │ 出站队列      │          │
│                         └──────┬───────┘          │
│                                │                  │
│                         ┌──────▼───────┐          │
│                         │  出站分发     │          │
│                         │  (Core 0)    │          │
│                         └──┬────────┬──┘          │
│                            │        │             │
│                     Telegram    WebSocket          │
│                     sendMessage  send              │
│                                                   │
│   ┌──────────────────────────────────────────┐    │
│   │  SPIFFS (12 MB)                          │    │
│   │  /spiffs/config/  SOUL.md, USER.md       │    │
│   │  /spiffs/memory/  MEMORY.md, YYYY-MM-DD  │    │
│   │  /spiffs/sessions/ tg_<chat_id>.jsonl    │    │
│   └──────────────────────────────────────────┘    │
55→└───────────────────────────────────────────────────┘
         │
         │  Anthropic Messages API (HTTPS)
         │  + Brave Search API (HTTPS)
         ▼
   ┌───────────┐   ┌──────────────┐
   │ Claude API │   │ Brave Search │
   └───────────┘   └──────────────┘
```

---

## 数据流 (Data Flow)

```
1. 用户在 Telegram (或 WebSocket) 发送消息
2. 通道轮询器接收消息，封装进 mimi_msg_t 结构体
3. 消息被推送到入站队列 (FreeRTOS xQueue)
4. Agent 循环 (Core 1) 取出消息:
   a. 从 SPIFFS 加载会话历史 (JSONL)
   b. 构建系统提示词 (SOUL.md + USER.md + MEMORY.md + 近期笔记 + 工具指南)
   c. 构建 cJSON 消息数组 (历史记录 + 当前消息)
   d. ReAct 循环 (最多 10 次迭代):
      i.   通过 HTTPS 调用 Claude API (非流式，带工具定义)
      ii.  解析 JSON 响应 → 文本块 + 工具调用块 (tool_use)
      iii. 如果 stop_reason == "tool_use":
           - 执行每个工具 (例如 web_search → Brave Search API)
           - 将助手内容 + 工具结果追加到消息中
           - 继续循环
      iv.  如果 stop_reason == "end_turn": 带着最终文本跳出循环
   e. 将用户消息 + 最终助手文本保存到会话文件
   f. 将响应推送到出站队列
5. 出站分发 (Core 0) 取出响应:
   a. 根据通道字段路由 ("telegram" → sendMessage, "websocket" → WS frame)
6. 用户收到回复
```

---

## 模块映射 (Module Map)

```
main/
├── mimi.c                  入口点 — app_main() 编排初始化 + 启动
├── mimi_config.h           所有编译时常量 + 包含构建时机密
├── mimi_secrets.h          构建时凭据 (gitignored, 优先级最高)
├── mimi_secrets.h.example  mimi_secrets.h 的模板
│
├── bus/
│   ├── message_bus.h       mimi_msg_t 结构体, 队列 API
│   └── message_bus.c       两个 FreeRTOS 队列: 入站 + 出站
│
├── wifi/
│   ├── wifi_manager.h      WiFi STA 生命周期 API
│   └── wifi_manager.c      事件处理, 指数退避重连
│
├── telegram/
│   ├── telegram_bot.h      机器人初始化/启动, send_message API
│   └── telegram_bot.c      长轮询循环, JSON 解析, 消息拆分
│
├── llm/
│   ├── llm_proxy.h         llm_chat() + llm_chat_tools() API, 工具调用类型
│   └── llm_proxy.c         Anthropic Messages API (非流式), 工具调用解析
│
├── agent/
│   ├── agent_loop.h        Agent 任务初始化/启动
│   ├── agent_loop.c        ReAct 循环: LLM 调用 → 工具执行 → 重复
│   ├── context_builder.h   系统提示词 + 消息构建器 API
│   └── context_builder.c   读取引导文件 + 记忆 + 工具指南
│
├── tools/
│   ├── tool_registry.h     工具定义结构体, 注册/分发 API
│   ├── tool_registry.c     工具注册, JSON Schema 构建器, 按名称分发
│   ├── tool_web_search.h   网页搜索工具 API
│   └── tool_web_search.c   Brave Search API via HTTPS (直连 + 代理)
│
├── memory/
│   ├── memory_store.h      长期 + 每日记忆 API
│   ├── memory_store.c      MEMORY.md 读/写, 每日 .md 追加/读取
│   ├── session_mgr.h       单次聊天会话 API
│   └── session_mgr.c       JSONL 会话文件, 环形缓冲历史
│
├── gateway/
│   ├── ws_server.h         WebSocket 服务器 API
│   └── ws_server.c         ESP HTTP 服务器 + WS 升级, 客户端追踪
│
├── proxy/
│   ├── http_proxy.h        代理连接 API
│   └── http_proxy.c        HTTP CONNECT 隧道 + TLS via esp_tls
│
├── cli/
│   ├── serial_cli.h        CLI 初始化 API
│   └── serial_cli.c        esp_console REPL 带调试/维护命令
│
└── ota/
    ├── ota_manager.h       OTA 更新 API
    └── ota_manager.c       esp_https_ota 封装
```

---

## FreeRTOS 任务布局 (FreeRTOS Task Layout)

| 任务 (Task) | 核心 (Core) | 优先级 (Priority) | 栈大小 (Stack) | 描述 (Description) |
|---|---|---|---|---|
| `tg_poll`          | 0    | 5        | 12 KB  | Telegram 长轮询 (30s 超时)  |
| `agent_loop`       | 1    | 6        | 12 KB  | 消息处理 + Claude API 调用 |
| `outbound`         | 0    | 5        | 8 KB   | 路由响应到 Telegram / WS     |
| `serial_cli`       | 0    | 3        | 4 KB   | USB 串口控制台 REPL              |
| httpd (internal)   | 0    | 5        | —      | WebSocket 服务器 (esp_http_server)   |
| wifi_event (IDF)   | 0    | 8        | —      | WiFi 事件处理 (ESP-IDF)        |

**核心分配策略**: Core 0 处理 I/O (网络, 串口, WiFi)。Core 1 专用于 Agent 循环 (CPU 密集型的 JSON 构建 + 等待 HTTPS)。

---

## 内存预算 (Memory Budget)

| 用途 (Purpose) | 位置 (Location) | 大小 (Size) |
|---|---|---|
| FreeRTOS 任务栈 | Internal SRAM  | ~40 KB   |
| WiFi 缓冲区 | Internal SRAM  | ~30 KB   |
| TLS 连接 x2 (Telegram + Claude) | PSRAM      | ~120 KB  |
| JSON 解析缓冲区 | PSRAM          | ~32 KB   |
| 会话历史缓存 | PSRAM          | ~32 KB   |
| 系统提示词缓冲区 | PSRAM          | ~16 KB   |
| LLM 响应流缓冲区 | PSRAM          | ~32 KB   |
| 剩余可用 | PSRAM          | ~7.7 MB  |

大缓冲区 (32 KB+) 通过 `heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM)` 从 PSRAM 分配。

---

## Flash 分区布局 (Flash Partition Layout)

```
偏移量      大小      名称        用途
─────────────────────────────────────────────
0x009000    24 KB     nvs         ESP-IDF 内部使用 (WiFi 校准等)
0x00F000     8 KB     otadata     OTA 启动状态
0x011000     4 KB     phy_init    WiFi PHY 校准
0x020000     2 MB     ota_0       固件槽位 A
0x220000     2 MB     ota_1       固件槽位 B
0x420000    12 MB     spiffs      Markdown 记忆, 会话, 配置
0xFF0000    64 KB     coredump    崩溃转储存储
```

总计: 16 MB flash。

---

## 存储布局 (Storage Layout - SPIFFS)

SPIFFS 是扁平文件系统 — 没有真正的目录。文件使用类似路径的名称。

```
/spiffs/config/SOUL.md          AI 人格定义
/spiffs/config/USER.md          用户档案
/spiffs/memory/MEMORY.md        长期持久记忆
/spiffs/memory/2026-02-05.md    每日笔记 (每天一个文件)
/spiffs/sessions/tg_12345.jsonl 会话历史 (每个 Telegram 聊天一个文件)
```

会话文件是 JSONL (每行一个 JSON 对象):
```json
{"role":"user","content":"Hello","ts":1738764800}
{"role":"assistant","content":"Hi there!","ts":1738764802}
```

---

## 配置 (Configuration)

所有配置完全通过 `mimi_secrets.h` 在编译时完成。没有运行时配置 — 修改任何设置都需要 `idf.py fullclean && idf.py build`。

| 宏定义 (Define) | 描述 (Description) |
|---|---|
| `MIMI_SECRET_WIFI_SSID`     | WiFi SSID                               |
| `MIMI_SECRET_WIFI_PASS`     | WiFi 密码                           |
| `MIMI_SECRET_TG_TOKEN`      | Telegram Bot API token                  |
| `MIMI_SECRET_API_KEY`       | Anthropic API key                       |
| `MIMI_SECRET_MODEL`         | 模型 ID (默认: claude-opus-4-6)     |
| `MIMI_SECRET_PROXY_HOST`    | HTTP 代理主机名/IP (可选)       |
| `MIMI_SECRET_PROXY_PORT`    | HTTP 代理端口 (可选)              |
| `MIMI_SECRET_SEARCH_KEY`    | Brave Search API key (可选)         |

NVS 仍会被初始化 (ESP-IDF WiFi 内部需要) 但不用于应用配置。

---

## 消息总线协议 (Message Bus Protocol)

内部消息总线使用两个传输 `mimi_msg_t` 的 FreeRTOS 队列:

```c
typedef struct {
    char channel[16];   // "telegram", "websocket", "cli"
    char chat_id[32];   // Telegram chat ID 或 WS client ID
    char *content;      // 堆分配的文本 (所有权转移)
} mimi_msg_t;
```

- **入站队列 (Inbound queue)**: 通道 → Agent 循环 (深度: 8)
- **出站队列 (Outbound queue)**: Agent 循环 → 分发 → 通道 (深度: 8)
- 内容字符串的所有权在推送时转移；接收者必须调用 `free()`。

---

## WebSocket 协议 (WebSocket Protocol)

端口: **18789**。最大客户端数: **4**。

**客户端 → 服务器:**
```json
{"type": "message", "content": "Hello", "chat_id": "ws_client1"}
```

**服务器 → 客户端:**
```json
{"type": "response", "content": "Hi there!", "chat_id": "ws_client1"}
```

客户端 `chat_id` 在连接时自动分配 (`ws_<fd>`) 但可以在第一条消息中覆盖。

---

## Claude API 集成 (Claude API Integration)

端点: `POST https://api.anthropic.com/v1/messages`

请求格式 (Anthropic 原生, 非流式, 带工具):
```json
{
  "model": "claude-opus-4-6",
  "max_tokens": 4096,
  "system": "<system prompt>",
  "tools": [
    {
      "name": "web_search",
      "description": "Search the web for current information.",
      "input_schema": {"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]}
    }
  ],
  "messages": [
    {"role": "user", "content": "Hello"},
    {"role": "assistant", "content": "Hi!"},
    {"role": "user", "content": "What's the weather today?"}
  ]
  // ...
}
```

与 OpenAI 的主要区别: `system` 是顶层字段，不在 `messages` 数组内。

非流式 JSON 响应:
```json
{
  "id": "msg_xxx",
  "type": "message",
  "role": "assistant",
  "content": [
    {"type": "text", "text": "Let me search for that."},
    {"type": "tool_use", "id": "toolu_xxx", "name": "web_search", "input": {"query": "weather today"}}
  ],
  "stop_reason": "tool_use"
}
```

当 `stop_reason` 为 `"tool_use"` 时，Agent 循环执行每个工具并将结果发回:
```json
{"role": "assistant", "content": [<text + tool_use blocks>]}
{"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_xxx", "content": "..."}]}
```

循环重复直到 `stop_reason` 为 `"end_turn"` (最多 10 次迭代)。

---

## 启动序列 (Startup Sequence)

```
app_main()
  ├── init_nvs()                    NVS flash 初始化 (如果损坏则擦除)
  ├── esp_event_loop_create_default()
  ├── init_spiffs()                 挂载 SPIFFS 到 /spiffs
  ├── message_bus_init()            创建入站 + 出站队列
  ├── memory_store_init()           验证 SPIFFS 路径
  ├── session_mgr_init()
  ├── wifi_manager_init()           初始化 WiFi STA 模式 + 事件处理程序
  ├── http_proxy_init()             从构建时机密加载代理配置
  ├── telegram_bot_init()           从构建时机密加载机器人 token
  ├── llm_proxy_init()              从构建时机密加载 API key + 模型
  ├── tool_registry_init()          注册工具, 构建工具 JSON
  ├── agent_loop_init()
  ├── serial_cli_init()             启动 REPL (无需 WiFi 即可工作)
  │
  ├── wifi_manager_start()          使用构建时凭据连接
  │   └── wifi_manager_wait_connected(30s)
  │
  └── [如果 WiFi 已连接]
      ├── telegram_bot_start()      启动 tg_poll 任务 (Core 0)
      ├── agent_loop_start()        启动 agent_loop 任务 (Core 1)
      ├── ws_server_start()         在端口 18789 启动 httpd
      └── outbound_dispatch task    启动出站任务 (Core 0)
```

如果 WiFi 凭据丢失或连接超时，CLI 仍然可用以进行诊断。

---

## 串口 CLI 命令 (Serial CLI Commands)

CLI 仅提供调试和维护命令。所有配置通过 `mimi_secrets.h` 完成。

| 命令 (Command) | 描述 (Description) |
|---|---|
| `wifi_status`                  | 显示连接状态和 IP        |
| `memory_read`                  | 打印 MEMORY.md 内容             |
| `memory_write <CONTENT>`       | 覆盖 MEMORY.md                  |
| `session_list`                 | 列出所有会话文件               |
| `session_clear <CHAT_ID>`      | 删除一个会话文件                |
| `heap_info`                    | 显示内部 + PSRAM 空闲字节数     |
| `restart`                      | 重启设备                    |
| `help`                         | 列出所有可用命令           |

---

## Nanobot 参考映射 (Nanobot Reference Mapping)

| Nanobot 模块 (Module) | MimiClaw 等效 (Equivalent) | 备注 (Notes) |
|---|---|---|
| `agent/loop.py`             | `agent/agent_loop.c`           | 带工具使用的 ReAct 循环     |
| `agent/context.py`          | `agent/context_builder.c`      | 加载 SOUL.md + USER.md + memory + tool guidance |
| `agent/memory.py`           | `memory/memory_store.c`        | MEMORY.md + 每日笔记      |
| `session/manager.py`        | `memory/session_mgr.c`         | 每个聊天 JSONL, 环形缓冲历史  |
| `channels/telegram.py`      | `telegram/telegram_bot.c`      | 原始 HTTP, 无 python-telegram-bot |
| `bus/events.py` + `queue.py`| `bus/message_bus.c`            | FreeRTOS 队列 vs asyncio   |
| `providers/litellm_provider.py` | `llm/llm_proxy.c`         | 仅直接 Anthropic API    |
| `config/schema.py`          | `mimi_config.h` + `mimi_secrets.h` | 仅构建时机密  |
| `cli/commands.py`           | `cli/serial_cli.c`             | esp_console REPL             |
| `agent/tools/*`             | `tools/tool_registry.c` + `tool_web_search.c` | 通过 Brave API 进行网页搜索 |
| `agent/subagent.py`         | *(尚未实现)*        | 见 TODO.md                  |
| `agent/skills.py`           | *(尚未实现)*        | 见 TODO.md                  |
| `cron/service.py`           | *(尚未实现)*        | 见 TODO.md                  |
| `heartbeat/service.py`      | *(尚未实现)*        | 见 TODO.md                  |
