# MimiClaw vs Nanobot — 功能差距追踪 (Feature Gap Tracker)

> 对比 `nanobot/` 参考实现。追踪 MimiClaw 尚未对齐的功能。
> 优先级: P0 = 核心缺失, P1 = 重要增强, P2 = 锦上添花

---

## P0 — 核心 Agent 能力 (Core Agent Capabilities)

### [x] ~~工具使用循环 (多轮 Agent 迭代)~~
- 已实现: `agent_loop.c` ReAct 循环，使用 `llm_chat_tools()`，最多 10 次迭代，非流式 JSON 解析

### [ ] 通过工具使用写入记忆 (Agent 驱动的记忆持久化)
- **openclaw**: Agent 使用标准的 `write`/`edit` 工具写入 `MEMORY.md` 和 `memory/YYYY-MM-DD.md`；系统提示词指示 Agent 持久化重要信息；预压缩记忆刷新 (pre-compaction memory flush) 触发一次静默的 Agent 回合，以便在达到上下文窗口限制前保存持久记忆。
- **MimiClaw**: 存在 `memory_write_long_term` 和 `memory_append_today`，但仅从 CLI 调用；Agent 循环从未写入记忆。
- **范围 (Scope)**: 将 `memory_write` 和 `memory_append_today` 作为 tool_use 工具暴露给 Claude；在系统提示词中增加何时持久化记忆的指导；可选增加预压缩刷新（当会话历史接近 `MIMI_SESSION_MAX_MSGS` 时触发记忆保存）。
- **依赖 (Depends on)**: 工具使用循环

### [x] ~~工具注册表 + web_search 工具~~
- 已实现: `tools/tool_registry.c` — 工具注册，JSON Schema 构建器，按名称分发
- 已实现: `tools/tool_web_search.c` — 通过 HTTPS 调用 Brave Search API（直连 + 代理支持）

### [ ] 更多内置工具 (More Built-in Tools)
- **nanobot 内置工具** 尚未移植: `read_file`, `write_file`, `edit_file`, `list_dir`, `message`
- **建议 (Recommendation)**: 适用于 ESP32 的合理工具子集: `read_file`, `write_file`, `list_dir` (SPIFFS), `message`, `memory_write`

### [ ] 子 Agent / 生成后台任务 (Subagent / Spawn Background Tasks)
- **nanobot**: `subagent.py` — SubagentManager 生成具有隔离工具集和系统提示词的独立 Agent 实例，并通过系统通道将结果通告回主 Agent。
- **MimiClaw**: 未实现
- **建议 (Recommendation)**: ESP32 内存有限；简化为单个后台 FreeRTOS 任务用于长时间运行的工作，完成后将结果注入入站队列。

---

## P1 — 重要功能 (Important Features)

### [ ] Telegram 用户白名单 (allow_from)
- **nanobot**: `channels/base.py` L59-82 — `is_allowed()` 检查 sender_id 是否在 allow_list 中
- **MimiClaw**: 无认证；任何人都可以向机器人发送消息并消耗 API 额度
- **建议 (Recommendation)**: 将 allow_from 列表作为编译时定义存储在 `mimi_secrets.h` 中，在 `process_updates()` 中过滤

### [ ] Telegram Markdown 转 HTML (Markdown to HTML Conversion)
- **nanobot**: `channels/telegram.py` L16-76 — `_markdown_to_telegram_html()` 全功能转换器：代码块、行内代码、粗体、斜体、链接、删除线、列表
- **MimiClaw**: 直接使用 `parse_mode: Markdown`；特殊字符可能导致发送失败（有回退到纯文本的机制）
- **建议 (Recommendation)**: 实现简化的 Markdown-to-HTML 转换器，或切换到 `parse_mode: HTML`

### [ ] Telegram /start 命令
- **nanobot**: `telegram.py` L183-192 — 处理 `/start` 命令，回复欢迎消息
- **MimiClaw**: 未处理；/start 作为普通消息发送给 Claude
- **建议 (Recommendation)**: 处理该命令，发送欢迎语

### [ ] Telegram 媒体处理 (图片/语音/文件)
- **nanobot**: `telegram.py` L194-289 — 处理图片、语音、音频、文档；下载文件；转录语音
- **MimiClaw**: 仅处理 `message.text`，忽略所有媒体消息
- **建议 (Recommendation)**: 图片可以进行 base64 编码供 Claude Vision 使用；语音需要 Whisper API（额外的 HTTPS 请求）

### [ ] 技能系统 (可插拔能力)
- **nanobot**: `agent/skills.py` — 从 SKILL.md 文件加载技能，支持常驻加载和按需加载，Frontmatter 元数据，需求检查
- **MimiClaw**: 未实现
- **建议 (Recommendation)**: 简化版本：将 SKILL.md 文件存储在 SPIFFS 上，通过 context_builder 加载到系统提示词中

### [ ] 完整引导文件对齐 (Full Bootstrap File Alignment)
- **nanobot**: 加载 `AGENTS.md`, `SOUL.md`, `USER.md`, `TOOLS.md`, `IDENTITY.md` (5 个文件)
- **MimiClaw**: 仅加载 `SOUL.md` 和 `USER.md`
- **建议 (Recommendation)**: 增加 AGENTS.md (行为准则) 和 TOOLS.md (工具文档)

### [ ] 更长的记忆回溯 (Longer Memory Lookback)
- **nanobot**: `memory.py` L56-80 — `get_recent_memories(days=7)` 默认为 7 天
- **MimiClaw**: `context_builder.c` 仅读取最近 3 天
- **建议 (Recommendation)**: 使其可配置，但要注意 token 预算

### [x] ~~系统提示词工具指南~~
- 已实现: `context_builder.c` 在系统提示词中包含工具使用指南

### [ ] 消息元数据 (媒体, 回复引用, 元数据)
- **nanobot**: `bus/events.py` — InboundMessage 包含 media, metadata 字段；OutboundMessage 包含 reply_to
- **MimiClaw**: `mimi_msg_t` 只有 channel + chat_id + content
- **建议 (Recommendation)**: 扩展 msg 结构体，增加 media_path 和 metadata 字段

### [ ] 出站订阅模式 (Outbound Subscription Pattern)
- **nanobot**: `bus/queue.py` L41-49 — 支持 `subscribe_outbound(channel, callback)` 订阅模型
- **MimiClaw**: 硬编码的 if-else 分发
- **建议 (Recommendation)**: 当前方法简单可靠；通道较少时不值得更改

---

## P2 — 高级功能 (Advanced Features)

### [ ] Cron 定时任务服务
- **nanobot**: `cron/service.py` — 完整的 cron 调度器，支持 at/every/cron 表达式，持久化存储，定时触发 Agent
- **MimiClaw**: 未实现
- **建议 (Recommendation)**: 使用 FreeRTOS 定时器实现简化版，仅支持 "每 N 分钟"

### [ ] 心跳服务 (Heartbeat Service)
- **nanobot**: `heartbeat/service.py` — 每 30 分钟读取 HEARTBEAT.md，如果发现任务则触发 Agent
- **MimiClaw**: 未实现
- **建议 (Recommendation)**: 简单的 FreeRTOS 定时器，定期检查 HEARTBEAT.md

### [ ] 多 LLM 提供商支持
- **nanobot**: `providers/litellm_provider.py` — 通过 LiteLLM 支持 OpenRouter, Anthropic, OpenAI, Gemini, DeepSeek, Groq, Zhipu, vLLM
- **MimiClaw**: 硬编码为 Anthropic Messages API
- **建议 (Recommendation)**: 抽象 LLM 接口，支持 OpenAI 兼容 API (大多数提供商都兼容)

### [ ] 语音转录 (Voice Transcription)
- **nanobot**: `providers/transcription.py` — Groq Whisper API
- **MimiClaw**: 未实现
- **建议 (Recommendation)**: 需要向 Whisper API 发送额外的 HTTPS 请求：下载 Telegram 语音 -> 转发 -> 获取文本

### [x] ~~构建时配置文件 + 运行时 NVS 覆盖~~
- 已实现: `mimi_secrets.h` 作为构建时默认值，NVS 作为通过 CLI 的运行时覆盖
- 两层配置: 构建时机密 → NVS 回退，CLI 命令用于设置/显示/重置

### [ ] WebSocket 网关协议增强
- **nanobot**: 网关端口 18790 + 更丰富的协议
- **MimiClaw**: 基本 JSON 协议，缺乏流式 token 推送
- **建议 (Recommendation)**: 增加 `{"type":"token","content":"..."}` 流式推送

### [ ] 多通道管理器
- **nanobot**: `channels/manager.py` — 多通道统一生命周期管理
- **MimiClaw**: 在 app_main() 中硬编码
- **建议 (Recommendation)**: 通道较少时不值得抽象

### [ ] WhatsApp / 飞书 (Feishu) 通道
- **nanobot**: `channels/whatsapp.py`, `channels/feishu.py`
- **MimiClaw**: 仅 Telegram + WebSocket
- **建议 (Recommendation)**: 低优先级，Telegram 已足够

### [x] ~~Telegram 代理支持 (HTTP CONNECT)~~
- 已实现: 通过 `proxy/http_proxy.c` 实现 HTTP CONNECT 隧道，可通过 `mimi_secrets.h` (`MIMI_SECRET_PROXY_HOST`/`MIMI_SECRET_PROXY_PORT`) 配置

### [ ] 会话元数据持久化
- **nanobot**: `session/manager.py` L136-153 — 会话文件包含元数据行 (created_at, updated_at)
- **MimiClaw**: JSONL 仅存储 role/content/ts，无元数据头
- **建议 (Recommendation)**: 低优先级

---

## 已完成对齐 (Completed Alignment)

- [x] Telegram Bot 长轮询 (getUpdates)
- [x] 消息总线 (入站/出站队列)
- [x] Agent 循环与 ReAct 工具使用 (多轮, 最多 10 次迭代)
- [x] Claude API (Anthropic Messages API, 非流式, tool_use 协议)
- [x] 工具注册表 + web_search 工具 (Brave Search API)
- [x] 上下文构建器 (系统提示词 + 引导文件 + 记忆 + 工具指南)
- [x] 记忆存储 (MEMORY.md + 每日笔记)
- [x] 会话管理器 (每个 chat_id 一个 JSONL, 环形缓冲历史)
- [x] WebSocket 网关 (端口 18789, JSON 协议)
- [x] 串口 CLI (esp_console, 调试/维护命令)
- [x] HTTP CONNECT 代理 (Telegram + Claude API + Brave Search 经由代理隧道)
- [x] OTA 更新
- [x] WiFi 管理器 (构建时凭据, 指数退避)
- [x] SPIFFS 存储
- [x] 构建时配置 (`mimi_secrets.h`) + 通过 CLI 的运行时 NVS 覆盖

---

## 建议实现顺序 (Suggested Implementation Order)

```
1. [done] 工具使用循环 + 工具注册表 + web_search
2. 通过工具使用写入记忆            <- 让 Agent 真正拥有记忆
3. 内置工具 (read_file, write_file, message)
4. Telegram 白名单 (allow_from)    <- 安全必备
5. 引导文件补全 (AGENTS.md, TOOLS.md)
6. 子 Agent (简化版)
7. Telegram Markdown -> HTML
8. 媒体处理
9. Cron / 心跳
10. 其他增强
```
