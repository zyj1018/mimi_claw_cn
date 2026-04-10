# MimiClaw 项目 Code Wiki 文档

## 1. 项目整体架构 (System Architecture)

MimiClaw 是一个基于 ESP32-S3 芯片运行的纯 C 语言 AI Agent。它没有运行 Linux 操作系统，也没有 Node.js 等运行时环境，而是直接构建在裸机和 FreeRTOS 之上。

整个系统架构采用了基于 **消息总线 (Message Bus)** 的异步事件驱动设计，并支持通过不同渠道接入。系统的核心流程围绕一个名为 **Agent Loop (ReAct 循环)** 的核心模块运转，它利用大语言模型（LLM，支持 Anthropic 或 OpenAI）进行理解、决策，并在需要时调用内置工具（Tools），最后将结果返回给用户。

### 数据流与核心流程：

1. **输入阶段**：用户通过 Telegram、Feishu（飞书）、WebSocket 或者串口 CLI 发送消息。
2. **消息路由**：各个 Channel 轮询器接收到消息后，将其封装为 `mimi_msg_t` 结构体，推入 **入站队列 (Inbound Queue)**。
3. **Agent 处理 (Core 1)**：
   - 运行在专用核心 Core 1 上的 `agent_loop_task` 从队列中取出消息。
   - 结合存储在 SPIFFS 中的系统上下文 (`SOUL.md`, `USER.md`, `MEMORY.md` 等) 和历史会话，构建完整的 Prompt。
   - 调用 LLM 接口。若 LLM 决定调用工具 (如搜索网页、查询时间、添加定时任务等)，则进入 **ReAct 循环** 并在本地执行工具逻辑，然后将结果反馈给 LLM。
4. **输出阶段**：当 LLM 返回最终文本时，生成最终回复，并推入 **出站队列 (Outbound Queue)**。
5. **分发**：运行在 Core 0 上的分发任务 (`outbound_dispatch_task`) 根据通道 ID，将消息路由回对应的客户端 (如调用 Telegram API 发送消息)。

---

## 2. 主要模块职责 (Main Module Responsibilities)

项目主要代码位于 `main/` 目录下，按功能模块划分为：

- **[main/mimi.c](file:///workspace/main/mimi.c)**:
  系统入口。负责初始化 NVS、SPIFFS、各模块组件以及启动各个 FreeRTOS 任务。
- **[agent/](file:///workspace/main/agent)**:
  核心推理模块。包含 `agent_loop`（管理 ReAct 循环逻辑）和 `context_builder`（拼接系统人设、工具描述和会话上下文）。
- **[bus/](file:///workspace/main/bus)**:
  消息总线模块。通过 `message_bus` 维护 FreeRTOS 入站和出站队列，解耦了 Channel 层与 Agent 层的直接依赖。
- **[channels/](file:///workspace/main/channels)**:
  通信渠道模块。实现了 `telegram_bot` 和 `feishu_bot`，负责向对应平台拉取（Long Polling）消息和推送消息。
- **[llm/](file:///workspace/main/llm)**:
  大模型代理模块。`llm_proxy` 负责处理与大语言模型 (Claude/OpenAI API) 的 HTTP 请求和 JSON 响应解析，支持工具调用的数据结构化。
- **[memory/](file:///workspace/main/memory)**:
  记忆与会话管理模块。`memory_store` 负责读写长期记忆 (`MEMORY.md`)，`session_mgr` 负责在 SPIFFS 文件系统中维护和裁剪 JSONL 格式的单次会话历史记录。
- **[tools/](file:///workspace/main/tools)**:
  工具注册与执行模块。`tool_registry` 管理所有可供 LLM 调用的外部工具。内置工具包括 `tool_web_search` (Brave 搜索)、`tool_kimi_search` (Kimi 搜索)、`tool_get_time`、`tool_cron` 等。
- **[cron/](file:///workspace/main/cron)** 和 **[heartbeat/](file:///workspace/main/heartbeat)**:
  自动化任务模块。`cron_service` 支持 AI 自主创建基于时间的调度任务；`heartbeat` 定期检查待办列表并触发 Agent。
- **[wifi/](file:///workspace/main/wifi)**, **[gateway/](file:///workspace/main/gateway)**, **[proxy/](file:///workspace/main/proxy)**, **[cli/](file:///workspace/main/cli)**:
  底层网络和交互设施。负责 WiFi 连接、WebSocket 服务、HTTP 代理和 UART 串口配置命令行。

---

## 3. 关键类与函数说明 (Key Classes & Functions)

### 数据结构
- **[mimi_msg_t](file:///workspace/main/bus/message_bus.h#L15-L19)**: 
  消息总线传输的基本单元。包含渠道名 (`channel`)、会话标识 (`chat_id`) 和堆分配的消息文本 (`content`)。
- **[llm_response_t](file:///workspace/main/llm/llm_proxy.h#L46-L52)**: 
  解析后的 LLM 响应结构。包含最终文本 (`text`) 和需要执行的工具调用列表 (`calls`)，以及是否包含工具调用的标志位 (`tool_use`)。

### 核心函数
- **[app_main](file:///workspace/main/mimi.c#L104)**: 
  ESP-IDF 系统的 `main` 函数，执行全系统初始化序列，依次挂载 Flash、初始化消息队列、WiFi、服务，并启动任务。
- **[agent_loop_task](file:///workspace/main/agent/agent_loop.c#L171)**: 
  Agent 处理的常驻 FreeRTOS 任务。包含 ReAct 循环的核心代码：组装 Prompt -> 请求 LLM -> 如果需调用工具，则本地执行并追加到上下文 -> 再次请求，直到生成最终 `final_text`。
- **[message_bus_push_inbound](file:///workspace/main/bus/message_bus.c#L36)** & **[message_bus_push_outbound](file:///workspace/main/bus/message_bus.c#L60)**: 
  向队列投递消息的 API。由于使用 FreeRTOS Queue 实现，它是线程安全的，也是系统跨 Core 通信的核心桥梁。
- **[llm_chat_tools](file:///workspace/main/llm/llm_proxy.c#L446)**: 
  向 LLM 提供商发送非流式 HTTP 请求并等待响应，返回解析好的文本和 `tool_use` 列表。

---

## 4. 依赖关系 (Dependencies)

MimiClaw 运行在 ESP-IDF (>=5.5.0, <5.6.0) 环境上，主要依赖如下：

1. **ESP-IDF 官方组件**:
   - 基础系统：`freertos`, `nvs_flash`, `spiffs`, `vfs`, `console`
   - 网络模块：`esp_wifi`, `esp_netif`, `esp-tls`
   - 应用协议：`esp_http_client`, `esp_http_server`, `esp_websocket_client`
   - JSON 处理：`json` (ESP-IDF 封装的 cJSON 库)
   - OTA 更新：`esp_https_ota`, `app_update`
2. **硬件要求**:
   - ESP32-S3 开发板 (至少 16MB Flash + 8MB PSRAM)。由于 LLM 上下文（Prompt 拼接和 JSON 解析）非常大，因此**强依赖 PSRAM**，许多大内存如 `history_json` 和 `system_prompt` 都是使用 `MALLOC_CAP_SPIRAM` 分配在 PSRAM 上的。
3. **外部服务依赖**:
   - Anthropic (Claude) 或 OpenAI 的 API Key。
   - Telegram Bot Token (或飞书的凭据)。
   - Brave Search API Key (如果启用网页搜索)。

---

## 5. 项目运行方式 (How to Run)

### 编译前准备
1. 复制配置文件模板并进行编辑：
   ```bash
   cp main/mimi_secrets.h.example main/mimi_secrets.h
   ```
2. 编辑 `mimi_secrets.h`，填入你的 WiFi 名称密码、Telegram Token 以及 LLM API Key。你也可以在运行后通过串口 CLI 动态配置。

### 编译与烧录
本项目需使用 ESP-IDF 工具链进行编译。建议在 macOS/Ubuntu 下使用仓库中 `scripts/` 提供的脚本安装 IDF。
```bash
# 配置目标芯片为 ESP32-S3
idf.py set-target esp32s3

# 编译代码
idf.py fullclean && idf.py build

# 烧录到开发板并打开监视器 (将 PORT 替换为你的串口设备)
idf.py -p PORT flash monitor
```

### 运行时调试 (CLI 命令行)
设备通过 UART 接口连接电脑后，可以使用基于串口的命令行（REPL）进行设备维护，无需重新编译即可更新凭据（存入 NVS）：
- `wifi_set <SSID> <PASS>`：配置 WiFi。
- `set_api_key <KEY>`：更换 LLM API Key。
- `set_model_provider openai`：将大模型切换为 OpenAI。
- `config_show`：查看当前的脱敏配置信息。
- `memory_read` / `session_list`：查看记忆和会话文件。
- `restart`：重启设备。
