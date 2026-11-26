# T5AI Eink Voice Chat

涂鸦 **T5AI 墨水屏 NFC 开发板**上的端到端 AI 语音对话固件：**待机喊唤醒词就说话，说完它自动回话**，不依赖屏幕、不依赖涂鸦云，音频链路由设备自己走阿里云百炼 / 小智（xiaozhi.me）协议。

> 本仓库是**独立项目**，与早期 `tuya-t5ai-chat-bot`（your_chat_bot 定制版 + LVGL 屏幕聊天 UI）是两条不同的技术路线：
> - 旧项目：保留墨水屏 UI、按钮交互、走涂鸦云链路。
> - 本项目：**去掉屏幕**，改造成「串口私有命令通道 + 待机唤醒 + 自研 HTTPS/WebSocket 直连大模型」的可脚本化语音盒子。

---

## 1. 它能做什么

| 能力 | 说明 |
| --- | --- |
| 待机唤醒 | 上电即进入监听，喊「**你好，小武**」自动开始对话（默认唤醒词可改） |
| 待机常连 | 空闲时提前与 xiaozhi.me 握手并挂起（不开麦），唤醒确认后**立即开麦**，省掉每次 3~6 秒的 TLS+WS 握手 |
| 唤醒不丢话 | 等待识别/握手期间用户说的话会缓冲下来，开麦后一并补发，不再出现「唤醒后先说几句它没听见」 |
| 单轮识别 | 喊话后录 6.5s，先 ASR 再 LLM，喇叭播报回答 |
| 实时通话 | `call` 进入 WebSocket 全双工对话，server VAD 自动断句，可多轮往返 |
| 小智通道 | 接入 xiaozhi.me 协议（WebSocket + OPUS），人设/音色/记忆在控制台配置 |
| 串口命令 | 115200 串口一句一条命令，全部中文回包（`RSP:` 前缀，便于脚本解析） |
| 音量持久化 | `vol 0-100` 软件音量，写入 KV 断电保存 |
| 喇叭自检 | `spk` / `spkab` 现场判定功放使能极性与静音 |
| 开机问候 | 上电播放一段内置语音问候（数据内嵌在固件里） |
| 控制台 | 单文件 HTML，负责烧录、串口终端、一键命令、自检清单 |

## 2. 硬件与板级要点

- 板卡：`TUYA_T5AI_EINK_NFC`（BK7258 / T5AI，WiFi + 麦克风 + 喇叭 + 墨水屏 + NFC）
- 本工程对板级做了三处必要改动（见 `docs/board-TUYA_T5AI_EINK_NFC.patch`）：
  1. `BOARD_SPEAKER_EN_PIN` 改为 **GPIO39**（该板功放使能脚）。
  2. `BOARD_EINK_SPI_BUSY_PIN` 改为 `TUYA_GPIO_NUM_MAX`（BUSY 实际未接，改走驱动固定延时）。
  3. `aec_enable = 0`：AEC 硬件模式会让喇叭通路静音；本应用是半双工（AI 说话时暂停拾音），不需要 AEC。
- 功放极性为**高电平使能**，`spk_pin_polarity = LOW` 表示「播放时拉高、静音时拉低」。
- 串口：烧录 + 命令口 `115200`，日志口 `460800`（两块 USB CDC）。

## 3. 目录结构

```
app/eink_chat/              固件应用本体（直接放进 TuyaOpen/apps/ 使用）
├── CMakeLists.txt
├── Kconfig
├── app_default.config
├── config/TUYA_T5AI_EINK_NFC.config
└── src/
    ├── eink_chat.c           主逻辑：串口命令 / 录音 / REST ASR+LLM / 实时 WS / 待机唤醒
    ├── eink_xz.c, eink_xz.h  小智协议客户端：OTA 取地址与 token / OPUS / 音量
    ├── aliyun_ca.c           百炼网关内置 CA
    ├── tenclass_ca.c         小智 OTA 内置 CA
    ├── eink_boot_greeting.h  开机问候语音数据（内嵌 PCM）
    └── app_secrets.example.h 本地密钥模板（复制成 app_secrets.h 使用）
tools/
├── console.html           单文件控制台（烧录 / 串口终端 / 快捷命令 / 自检清单）
├── start-console.command  macOS 双击启动本地静态服务并打开控制台
└── api_check.py           三通道 API 自检（LLM / ASR / Realtime WS）
docs/
└── board-TUYA_T5AI_EINK_NFC.patch  TuyaOpen 板级改动补丁
```

## 4. 快速开始

### 4.1 准备源码树

```bash
git clone --depth 1 https://gitee.com/tuya-open/TuyaOpen.git
cd TuyaOpen
cp -r <本仓库>/app/eink_chat apps/eink_chat
git apply <本仓库>/docs/board-TUYA_T5AI_EINK_NFC.patch   # 板级三处改动
```

### 4.2 填入自己的密钥

```bash
cp apps/eink_chat/src/app_secrets.example.h apps/eink_chat/src/app_secrets.h
# 编辑 app_secrets.h：AI_API_KEY（阿里云百炼）、WIFI_SSID_DEFAULT / WIFI_PWD_DEFAULT
```

`app_secrets.h` 已在 `.gitignore` 中，不会被提交；缺失时会回退到占位值（能编译，但联网/鉴权会失败）。

### 4.3 编译与烧录

```bash
cd apps/eink_chat
python3 ../../tos.py build          # 产物 .build/bin/eink_chat_QIO_1.0.0.bin

# 烧录（tyutool 在 TuyaOpen/tools/tyutool）
./tyutool_cli write -d t5 -p /dev/cu.usbmodemXXXX -b 921600 \
  -s 0x00000000 --end 0x<bin大小> -f .build/bin/eink_chat_QIO_1.0.0.bin
```

也可以直接用 `tools/console.html`：起烧录服务 `tyutool_cli serve --plain`，页面里选串口一键烧录。

### 4.4 控制台

```bash
cd tools && ./start-console.command     # 起 127.0.0.1:8899 并打开页面
```

页面必须经 `http://127.0.0.1` 访问（烧录服务 `ws://127.0.0.1:9527` 只接受本机 http 来源）。

## 5. 串口命令

所有回包以 `RSP:` 开头，一行一条，便于脚本 grep。

| 命令 | 作用 |
| --- | --- |
| `wifi <ssid> <pwd>` | 配网并持久化 |
| `net` | 查看联网 / IP 状态 |
| `chat <文本>` | 跳过录音，直接问大模型（验证 LLM 链路） |
| `rt` | 百炼实时 WebSocket 会话（全双工，VAD 断句） |
| `call` / `call end` | 开/停对话：小智模式开启时走小智，否则走实时通道 |
| `xz` / `xz end` / `xz on` / `xz off` | 小智通道：直接开会话 / 结束 / 开关模式（默认开启） |
| `xz pre` / `xz arm` / `xz abort` | 预连接：只握手不开麦 / 唤醒确认后开麦 / 放弃预连接（调链路用） |
| `xz boost <0-60>` | 会话开局增益补偿百分比（默认 30，持续 3 秒，0 关闭） |
| `wake` | 查询待机状态：开关、唤醒词、命中次数、峰值、门限、最近听到的识别文本 |
| `wake on` / `wake off` / `wake <词>` | 待机开关 / 更换唤醒词 |
| `wake thr <值>` | 调整唤醒触发门限（安静环境实测底噪 300~2000，正常说话 9500+） |
| `mic <秒>` | 麦克风诊断：录 N 秒回显峰值，先确认拾音正常 |
| `spk` / `spk l` / `spk h` | 喇叭自检：播放 1s 1kHz 正弦波，可强制拉低/拉高 PA39 |
| `spkab` | PA 极性 A/B 对照：前 5 秒 PA 低、后 5 秒 PA 高，听第 5 秒有无变化 |
| `spklong <秒>` | 恒幅 1kHz 长音（默认 8 秒）：判断「唤醒后开头几句声音小」是功放起振还是链路问题 |
| `vol` / `vol 0-100` | 查询 / 设置软件音量（断电保存） |
| `ver` | 版本（当前 `eink_chat 1.8 小智预连接+起振补偿`） |
| `help` | 命令速查 |

## 6. 唤醒响应速度与音量

### 6.1 唤醒后「要等一会儿才理我」

时间都花在这三段上：采样窗等满 3.2 秒 → 唤醒词 ASR 1~2 秒 → TLS+WS 握手 0.4~6 秒（取决网络到 api.tenclass.net 的 RTT），这段里用户说的话原本直接被丢掉。现在做了三件事：

1. **说完就收窗**：检测到「说过话 + 随后 500ms 静音」提前结束采样窗，最少只等 1.2 秒。
2. **待机常连**：空闲时就把 WS 握好并挂起（`XZ_ST_PAUSED`），唤醒确认后 `arm` 只需发一条 `listen start`，握手时间归零。服务器会踢掉空闲会话，固件每 30 秒发 WS ping 保活，被踢后 2 秒内自动重连。
3. **唤醒不丢话**：挂起态也在收麦克风（只留最近 2.5 秒），开麦时 `mic_ring` 里的这段音频一并上行，所以「你好小武，现在几点」中间那段不会丢。

`wake` 命中后日志里能看到 `[XZ] 唤醒确认 开麦上行（预连接缓冲 N 字节一并补发）`，N 就是被救回来的音频。

### 6.2 唤醒后头几句音量偏小

数据侧电平探针显示，下行解码出的音频从第一帧起幅值就正常（`pk` 一万多），所以偏小的多半是**功放/耦合电容从静置进入工作的起振过程**。固件两条对策：

- **播放预热**：每个会话在真正出声前先灌 240ms 静音，让 DAC 与功放先进入稳态；
- **开局增益补偿**：会话最初 3 秒在设定音量上再补 `xz boost`（默认 +30%），之后回落（`eink_xz_vol_scale` 已做削顶保护，不会数字溢出）。

判断到底是硬件还是链路：复位后立刻执行 `spklong 6`，过 1 分钟再执行一次。若第一次开头明显偏小、后段变响，就是功放起振（补偿有效）；若两次都一样响，说明问题在链路侧，把 `xz boost 0` 关掉再看。

## 7. 链路自检

```bash
export DASHSCOPE_API_KEY=sk-xxxx
python3 tools/api_check.py          # 三条通道全测
python3 tools/api_check.py --asr    # 只测 ASR
```

用纯标准库实现（`urllib` + `websocket` 手写帧），依次验证：

1. **REST LLM** `qwen-plus`（`/compatible-mode/v1/chat/completions`）
2. **REST ASR** `qwen-audio-3.1-asr-flash`（`/api/v1/services/aigc/multimodal-generation/generation`，16k 单声道 PCM16 base64 直传）
3. **Realtime WS** `qwen-audio-3.1-realtime-flash`（`/api-ws/v1/realtime`，必须带 `Origin: https://dashscope.aliyuncs.com`）

## 8. 唤醒没反应怎么查

按顺序排查，一步一个结论：

1. `mic 3` —— 峰值若长期 <1000，是拾音/增益问题，不是识别问题。
2. `wake` —— 看「最近听到」那行：有没有识别出文本、被识别成了什么词。
3. 常见情况：ASR 把「武」听成「五 / 舞 / 吴」。固件已做前缀/后缀容错匹配，若仍不命中，用 `wake thr` 降门限或 `wake <你的词>` 换一个更易识别的唤醒词。
4. 喇叭刚播报完的时间窗内会主动抑制唤醒（防止把 AI 自己的话再送进识别，造成自问自答），等 2 秒再喊。
5. 确认 `xz on` 状态：小智模式开启时唤醒后走 xiaozhi.me，需要设备已在 xiaozhi.me 控制台绑定（首次会回激活码）。

## 9. 已知限制

- 半双工：喇叭播放期间不拾音（省掉 AEC，换稳定出声）。
- 待机会话是「尽力而为」：xiaozhi.me 会主动关闭空闲会话，固件会重连，但重连的那 1~6 秒里唤醒要先等握手（日志会打 `待机会话被断开，重连`）。
- 小智通道与百炼实时通道共用喇叭/麦克风，同一时刻只允许一个会话。
- 未内置开机自动配网 UI，配网靠 `wifi` 命令或 `app_secrets.h` 默认值。
- 墨水屏驱动仍被编译进镜像但应用不再使用；`BOARD_EINK_SPI_BUSY_PIN` 的改动是为了绕开未接 BUSY 引脚。
- 固件二进制未入库（内嵌 Key/WiFi，且体积大），请自行编译。

## 10. 许可与致谢

应用代码基于 TuyaOpen（Apache-2.0）生态开发，SDK 与板级代码版权归涂鸦及原仓库所有；本仓库仅包含应用层与工具脚本。
