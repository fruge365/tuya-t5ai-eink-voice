/**
 * @file eink_chat.c
 * @brief 涂鸦 T5AI 墨水屏 NFC 板 AI 对话固件
 *
 * 交互流程：按 ENTER 键开始录音，松开结束录音；
 * PCM 录音上传百炼 qwen-audio-3.1-asr-flash 识别为文本，
 * 再由 qwen-plus 生成回答，最终显示在 400x300 墨水屏上。
 *
 * 串口 CLI 命令（115200，调试串口）：
 *   wifi <ssid> <password>   连接 WiFi 并持久化
 *   chat <text>              跳过录音，直接向大模型提问
 *   net                      查看网络状态
 */

#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tkl_memory.h"
#include "bk_cli.h"
#include "tkl_uart.h"
#include "tal_uart.h"
#include "tuya_transporter.h"
#include "tuya_tls.h"

extern const char g_aliyun_ca_pem[];
extern const unsigned int g_aliyun_ca_pem_len;

/* 关闭底层厂商 VAD：其静音一段时间后会停止向应用层输出麦克风数据，
 * 导致待机窗口采样不全、实时会话期间语音无法上行（详见 tkl_audio.c 注释） */
extern void tkl_ai_disable_vendor_vad(void);

#include "netmgr.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif
#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
#include "lwip_init.h"
#endif

#include "board_com_api.h"
#include "tkl_gpio.h"
#include <math.h>

#include "tdl_audio_manage.h"
#include "tdl_button_manage.h"
#include "tdl_display_manage.h"

#include "u8g2.h"
#include "u8g2_port.h"

#include "cJSON.h"
#include "mix_method.h"

#include "http_session.h"

#include "eink_xz.h"

#include "eink_boot_greeting.h"

#include <string.h>
#include <stdio.h>

/***********************************************************
************************macro define************************
***********************************************************/
#define TAG "eink_chat"

/* 百炼开放接口配置
 * 敏感配置（API Key / WiFi 凭据）放在 app_secrets.h，不入库；
 * 首次构建请把 app_secrets.example.h 复制为 app_secrets.h 并填写。 */
#if defined(__has_include)
#  if __has_include("app_secrets.h")
#    include "app_secrets.h"
#  endif
#endif
#ifndef AI_API_KEY
#  define AI_API_KEY        "sk-REPLACE_WITH_YOUR_KEY"
#endif
#ifndef WIFI_SSID_DEFAULT
#  define WIFI_SSID_DEFAULT "REPLACE_WITH_YOUR_SSID"
#endif
#ifndef WIFI_PWD_DEFAULT
#  define WIFI_PWD_DEFAULT  "REPLACE_WITH_YOUR_PASSWORD"
#endif
#define AI_HOST         "llm-rwc82wtulg6ogzm2.cn-beijing.maas.aliyuncs.com"
#define ASR_PATH        "/api/v1/services/aigc/multimodal-generation/generation"
#define LLM_PATH        "/compatible-mode/v1/chat/completions"
#define ASR_MODEL       "qwen-audio-3.1-asr-flash"
#define LLM_MODEL       "qwen-plus"
#define SYS_PROMPT      "你是一个简短实用的中文语音助手，回答口语化，控制在60字以内。"

/* 录音配置：16k 16bit 单声道 */
#define REC_SAMPLE_RATE   16000
#define REC_MAX_SECONDS   25
#define REC_BUF_SIZE      (REC_SAMPLE_RATE * 2 * REC_MAX_SECONDS)      /* 800KB PCM */
#define B64_BUF_SIZE      (((REC_BUF_SIZE + 2) / 3) * 4 + 8)
#define JSON_BUF_SIZE     (B64_BUF_SIZE + 8192)
#define RESP_BUF_SIZE     (64 * 1024)

#define LLM_SYSTEM_PROMPT_MAX 128

typedef enum {
    APP_ST_IDLE = 0,
    APP_ST_RECORDING,
    APP_ST_PROCESSING,
} APP_STATE_E;

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    u8g2_t  u8g2;
    TDL_AUDIO_HANDLE_T audio;
    APP_STATE_E state;
    volatile BOOL_T rec_active;
    volatile uint32_t rec_len;          /* 已录 PCM 字节数 */
    uint8_t  *pcm_buf;                  /* PSRAM */
    char     *b64_buf;                  /* PSRAM */
    char     *json_buf;                 /* PSRAM */
    char     *resp_buf;                 /* PSRAM */
    char     question[512];
    char     answer[512];
    SEM_HANDLE work_sem;
THREAD_HANDLE worker;
THREAD_HANDLE cmd_uart;
THREAD_HANDLE rt_thread;
THREAD_HANDLE wake_thread;
THREAD_HANDLE audio_ra_thread;
    volatile BOOL_T worker_quit;
} APP_CTRL_T;

static APP_CTRL_T sg_app;

static void __draw_screen(const char *state_line, const char *q, const char *a);
static void __notify_answer(const char *q, const char *a);
static OPERATE_RET __do_asr(char *out, int out_len);
static OPERATE_RET __do_llm(const char *question, char *out, int out_len);
static void __start_record(void);
static void __stop_record(void);
static BOOL_T __rt_live_start(void);
static int  __wake_peak_scan(void);
static BOOL_T __rt_auto_call(void);

/***********************************************************
***********************display define***********************
***********************************************************/
/* 中文点阵字体（文泉驿，GB2312 全量） */
#define FONT_TITLE  u8g2_font_wqy13_t_gb2312
#define FONT_TEXT   u8g2_font_wqy13_t_gb2312

/* 墨水屏本体未接，状态改为串口日志输出 */
static void __draw_screen(const char *state_line, const char *q, const char *a)
{
    if (state_line && state_line[0]) {
        PR_NOTICE("[状态] %s", state_line);
    }
    if (q && q[0]) {
        PR_NOTICE("[你说] %s", q);
    }
    if (a && a[0]) {
        PR_NOTICE("[AI] %s", a);
    }
}

/***********************************************************
************************audio define************************
***********************************************************/
/* 实时通话状态（供麦克风回调与后文共用） */
typedef enum {
    RT_IDLE = 0,
    RT_STREAM,
    RT_TAIL,
    RT_WAIT,
} RT_STATE_E;

static volatile int s_rt_state = RT_IDLE;

/* ---- 连续实时对话（v1.6） ---- */
#define RT_SESSION_IDLE_MS  15000   /* 15 秒无交互自动结束会话 */
#define RT_GUARD_MS         400     /* 喇叭播完后的静默保护，防回声误触发 */
#define RT_SPEAK_PEAK       3000    /* 块内人声峰值门限，超过才重置空闲计时（环境噪声实测可达 2000） */
static volatile BOOL_T   s_rt_live    = FALSE;   /* 连续对话会话进行中 */
static volatile BOOL_T   s_rt_end_req = FALSE;   /* 请求结束会话（按键） */
static volatile uint32_t s_rt_idle_ms = 0;       /* 距上次交互的毫秒数 */
static volatile uint32_t s_rt_guard_ms = 0;      /* 播放结束后的静默保护计时 */
static SEM_HANDLE s_rt_mutex = NULL;             /* 序列化会话启动（唤醒线程 vs 命令线程） */

/* 喇叭自声隔离窗：本机播放期间+余音期内抑制待机唤醒扫描。
 * 防止喇叭放出的 AI 语音/问候语（含唤醒词「小舞」）被麦克风收回，
 * 触发误唤醒后把 AI 自己的话送去识别造成自问自答循环 */
static volatile uint32_t s_spk_mute_until = 0;   /* 唤醒静默截止时刻(ms) */
static void eink_chat_spk_mute(uint32_t ms)
{
    uint32_t until = tal_system_get_millisecond() + ms;
    if ((int32_t)(until - s_spk_mute_until) > 0) { s_spk_mute_until = until; }
}

/* 供 eink_xz.c 调用的桥接：音频句柄与串口回复 */
static void __uart0_out(const char *s);   /* 定义见 uart 命令区 */

TDL_AUDIO_HANDLE_T eink_chat_audio_handle(void)
{
    return sg_app.audio;
}

void __uart_reply(const char *msg)
{
    __uart0_out(msg);
}

static void __mic_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status,
                     uint8_t *data, uint32_t len)
{
    /* 小智会话优先分流（不受 rec_active 窗口控制） */
    if (eink_xz_is_active()) {
        if (TDL_AUDIO_FRAME_FORMAT_PCM == type && data && len > 0) {
            eink_xz_on_pcm(data, len);
        }
        return;
    }
    /* 预连接已握手但还没开麦：这份音频既要补发给服务器（唤醒确认后上行），
     * 也要继续写进采样窗给 ASR 识别唤醒词，所以这里不 return */
    if (eink_xz_is_paused()) {
        /* 排除喇叭自声（测试音/问候语），否则这段声音会被当成用户语音上行 */
        if (TDL_AUDIO_FRAME_FORMAT_PCM == type && data && len > 0 &&
            tal_system_get_millisecond() >= s_spk_mute_until) {
            eink_xz_on_pcm(data, len);
        }
    }

    if (!sg_app.rec_active || NULL == data || 0 == len) {
        return;
    }
    if (TDL_AUDIO_FRAME_FORMAT_PCM != type) {
        return;
    }

    if (RT_IDLE != s_rt_state) {
        /* 实时通话：环形写入、无限累计，发送端按序消费 */
        uint32_t pos = sg_app.rec_len % REC_BUF_SIZE;
        uint32_t first = REC_BUF_SIZE - pos;
        if (first > len) { first = len; }
        memcpy(sg_app.pcm_buf + pos, data, first);
        if (len > first) {
            memcpy(sg_app.pcm_buf, data + first, len - first);
        }
        sg_app.rec_len += len;
        return;
    }

    uint32_t remain = REC_BUF_SIZE - sg_app.rec_len;
    if (0 == remain) {
        /* 录满自动结束 */
        sg_app.rec_active = FALSE;
        sg_app.state = APP_ST_PROCESSING;
        tal_semaphore_post(sg_app.work_sem);
        return;
    }
    uint32_t cp = (len > remain) ? remain : len;
    memcpy(sg_app.pcm_buf + sg_app.rec_len, data, cp);
    sg_app.rec_len += cp;
}

/***********************************************************
************************button define************************
***********************************************************/
static BOOL_T __rt_connect(void);
static void __ws_close(void);
static void __ws_poll(void);
static char s_rt_last_ev[48];

/* ---- 待机唤醒 ---- */
#define WAKE_WORD_DEFAULT "你好小武"
#define WAKE_WIN_MS       3200      /* 一次待机采样窗口 */
#define WAKE_TALK_MS      6500      /* 唤醒后自动录音时长 */
#define WAKE_GAIN         8         /* 送 ASR 前的软件增益（麦克风灵敏度偏低） */
#define WAKE_PAD_SAMPLES  4800      /* 截取时前后各留 0.3s */
#define WAKE_PEAK_MIN     800       /* 峰值门限默认值（安静环境实测 300~2000，说话 9500+），可用 wake thr 调整 */
static volatile BOOL_T s_wake_on = TRUE;      /* 上电即进入待机监听 */
static char            s_wake_word[32] = WAKE_WORD_DEFAULT;
static volatile uint32_t s_wake_cnt = 0;
static volatile int    s_wake_peak = 0;
static volatile int    s_wake_peak_strong = 0;   /* 本次窗口内最大单步峰值（预连接判据） */
static volatile int    s_wake_thr = WAKE_PEAK_MIN;
static char            s_wake_last[128];
static volatile BOOL_T s_mic_test = FALSE;

static void __button_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc)
{
    (void)argc;

    if (0 == strcmp(name, "button3")) {          /* ENTER 键 */
        if (TDL_BUTTON_PRESS_DOWN == event) {
            if (s_rt_live) {
                /* 对话进行中再次按下：请求结束（由 rt_thread 统一收尾） */
                s_rt_end_req = TRUE;
            } else if (APP_ST_IDLE == sg_app.state) {
                if (!__rt_live_start()) {
                    PR_ERR("[RT] 会话不可用，回退到单轮识别");
                    __start_record();
                }
            }
        } else if (TDL_BUTTON_PRESS_UP == event) {
            /* 连续对话不随松开结束；仅单轮录音流程松开结束 */
            if (!s_rt_live && APP_ST_RECORDING == sg_app.state && RT_IDLE == s_rt_state) {
                __stop_record();
            }
        }
    }
}

static void __buttons_init(void)
{
    OPERATE_RET rt;
    TDL_BUTTON_HANDLE hdl = NULL;
    TDL_BUTTON_CFG_T cfg = {
        .long_start_valid_time    = 2000,
        .long_keep_timer          = 500,
        .button_debounce_time     = 50,
        .button_repeat_valid_count = 2,
        .button_repeat_valid_time = 500,
    };

    char name[16];
    for (int i = 1; i <= 6; i++) {
        snprintf(name, sizeof(name), "button%d", i);
        rt = tdl_button_create(name, &cfg, &hdl);
        if (OPRT_OK == rt) {
            tdl_button_event_register(hdl, TDL_BUTTON_PRESS_DOWN, __button_cb);
            tdl_button_event_register(hdl, TDL_BUTTON_PRESS_UP, __button_cb);
        } else {
            PR_ERR("按键 %s 创建失败：%d", name, rt);
        }
    }
}

/***********************************************************
************************record flow*************************
***********************************************************/
static void __start_record(void)
{
    if (APP_ST_IDLE != sg_app.state) {
        return;
    }
    sg_app.rec_len = 0;
    sg_app.rec_active = TRUE;
    sg_app.state = APP_ST_RECORDING;
    sg_app.question[0] = '\0';
    sg_app.answer[0] = '\0';
    __draw_screen("录音中... 松开结束", NULL, NULL);
    PR_NOTICE("开始录音");
}

static void __stop_record(void)
{
    if (APP_ST_RECORDING != sg_app.state) {
        return;
    }
    sg_app.rec_active = FALSE;
    if (sg_app.rec_len < REC_SAMPLE_RATE) {      /* 少于 1 秒 */
        sg_app.state = APP_ST_IDLE;
        __draw_screen("录音太短", NULL, NULL);
        return;
    }
    sg_app.state = APP_ST_PROCESSING;
    tal_semaphore_post(sg_app.work_sem);
    PR_NOTICE("录音结束，%d 字节", sg_app.rec_len);
}

static void __worker_thread(void *arg)
{
    (void)arg;
    while (!sg_app.worker_quit) {
        tal_semaphore_wait(sg_app.work_sem, SEM_WAIT_FOREVER);
        if (sg_app.worker_quit) { break; }

        OPERATE_RET rt;
        char *q = sg_app.question;

        if ('\0' == q[0]) {                     /* 录音流程：先 ASR */
            __draw_screen("识别中...", NULL, NULL);
            rt = __do_asr(q, sizeof(sg_app.question));
            if (OPRT_OK != rt || '\0' == q[0]) {
                PR_ERR("识别失败：%d", rt);
                sg_app.state = APP_ST_IDLE;
                __draw_screen("识别失败", NULL, NULL);
                continue;
            }
            PR_NOTICE("asr: %s", q);
        }

        __draw_screen("思考中...", q, NULL);
        rt = __do_llm(q, sg_app.answer, sizeof(sg_app.answer));
        if (OPRT_OK != rt) {
            PR_ERR("大模型失败：%d", rt);
            strncpy(sg_app.answer, "大模型请求失败", sizeof(sg_app.answer) - 1);
        }
        PR_NOTICE("llm: %s", sg_app.answer);

        sg_app.state = APP_ST_IDLE;
        __draw_screen("按 ENTER 键说话", q, sg_app.answer);
        __notify_answer(q, sg_app.answer);
    }
    sg_app.worker = NULL;
}

/***********************************************************
**************************HTTP*******************************
***********************************************************/
static OPERATE_RET __http_post_json(const char *path, const char *json,
                                    char *resp, int resp_len, int *status_code)
{
    /* 不用 http_session：它会向 Tuya 云申请域名证书，未激活设备必然失败。
     * 这里直接 TCP+TLS，用手的内置 CA 校验，对阿里云网关可用。 */
    tuya_transporter_t tcp = NULL, tls = NULL;
    static char s_req[768];
    static char s_raw[4096];
    OPERATE_RET rt = OPRT_COM_ERROR;
    int total = 0, code = 0, n = 0;
    size_t body_len = strlen(json);

    tcp = tuya_transporter_create(TRANSPORT_TYPE_TCP, NULL);
    if (NULL == tcp) {
        PR_ERR("[http] tcp create failed");
        return OPRT_COM_ERROR;
    }
    tls = tuya_transporter_create(TRANSPORT_TYPE_TLS, tcp);
    if (NULL == tls) {
        PR_ERR("[http] tls create failed");
        tuya_transporter_destroy(tcp);
        return OPRT_COM_ERROR;
    }

    tuya_tls_config_t tcfg = {0};
    tcfg.mode         = TUYA_TLS_SERVER_CERT_MODE;
    tcfg.hostname     = (char *)AI_HOST;
    tcfg.port         = 443;
    tcfg.timeout      = 20000;
    tcfg.verify       = true;
    tcfg.ca_cert      = (char *)g_aliyun_ca_pem;
    tcfg.ca_cert_size = (int)g_aliyun_ca_pem_len;
    if (OPRT_OK != tuya_transporter_ctrl(tls, TUYA_TRANSPORTER_SET_TLS_CONFIG, &tcfg)) {
        PR_ERR("[http] tls config failed");
        goto __http_exit;
    }
    if (OPRT_OK != tuya_transporter_connect(tls, AI_HOST, 443, 20000)) {
        PR_ERR("[http] tls connect failed");
        goto __http_exit;
    }

    n = snprintf(s_req, sizeof(s_req),
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Authorization: Bearer %s\r\n"
                 "Content-Type: application/json\r\n"
                 "X-DashScope-SSE: disable\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n\r\n",
                 path, AI_HOST, AI_API_KEY, (unsigned)body_len);
    if (n <= 0 || n >= (int)sizeof(s_req)) {
        goto __http_exit;
    }
    if (n != tuya_transporter_write(tls, (uint8_t *)s_req, n, 8000)) {
        PR_ERR("[http] send header failed");
        goto __http_exit;
    }
    /* 请求体可能很大（ASR 的 base64 音频），分片写避免超出发送缓冲 */
    {
        uint32_t off = 0;
        while (off < body_len) {
            uint32_t chunk = (body_len - off > 2048) ? 2048 : (uint32_t)(body_len - off);
            int w = tuya_transporter_write(tls, (uint8_t *)(json + off), (int)chunk, 8000);
            if (w <= 0) {
                PR_ERR("[http] send body failed at %u", off);
                goto __http_exit;
            }
            off += (uint32_t)w;
        }
    }

    while (total < (int)sizeof(s_raw) - 1) {
        int rd = tuya_transporter_read(tls, (uint8_t *)(s_raw + total), (int)(sizeof(s_raw) - 1 - total), 30000);
        if (rd <= 0) { break; }
        total += rd;
    }
    s_raw[total] = '\0';
    PR_DEBUG("[http] raw %d bytes", total);

    /* 状态行 HTTP/1.1 200 OK */
    if (total > 12 && 0 == strncmp(s_raw, "HTTP/1.", 7)) {
        code = atoi(s_raw + 9);
    } else {
        PR_ERR("[http] no http header: %.40s", s_raw);
        goto __http_exit;
    }
    if (status_code) { *status_code = code; }

    {
        char *body = strstr(s_raw, "\r\n\r\n");
        if (NULL == body) {
            goto __http_exit;
        }
        body += 4;
        memset(resp, 0, resp_len);

        if (NULL != strstr(s_raw, "Transfer-Encoding: chunked") ||
            NULL != strstr(s_raw, "transfer-encoding: chunked")) {
            /* chunked 解码：<hex 长度>\r\n<data>\r\n ... 0\r\n\r\n */
            char *p = body;
            int out = 0;
            while (out < resp_len - 1) {
                long clen = strtol(p, &p, 16);
                if (clen <= 0) { break; }
                if (p[0] == '\r' && p[1] == '\n') { p += 2; }
                if (out + (int)clen > resp_len - 1) { clen = resp_len - 1 - out; }
                memcpy(resp + out, p, (size_t)clen);
                out += (int)clen;
                p += clen;
                while (*p == '\r' || *p == '\n') { p++; }
            }
            resp[out] = '\0';
        } else {
            snprintf(resp, resp_len, "%s", body);
        }
        rt = (resp[0] != '\0') ? OPRT_OK : OPRT_COM_ERROR;
    }

__http_exit:
    if (tls) { tuya_transporter_close(tls); tuya_transporter_destroy(tls); }
    if (tcp) { tuya_transporter_destroy(tcp); }
    return rt;
}

/* 组装 ASR 请求 JSON 并调用 */
static OPERATE_RET __do_asr(char *out, int out_len)
{
    OPERATE_RET rt;
    int status = 0;
    uint32_t pcm_len = sg_app.rec_len;
    uint32_t b64_len = ((pcm_len + 2) / 3) * 4 + 1;

    tuya_base64_encode(sg_app.pcm_buf, sg_app.b64_buf, (int)pcm_len);
    sg_app.b64_buf[b64_len - 1] = '\0';

    int n = snprintf(sg_app.json_buf, JSON_BUF_SIZE,
        "{\"model\":\"%s\",\"input\":{\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"type\":\"input_audio\",\"input_audio\":"
        "{\"data\":\"data:audio/wav;base64,%s\"}}]}]},"
        "\"parameters\":{\"format\":\"pcm\",\"sample_rate\":\"16000\"}}",
        ASR_MODEL, sg_app.b64_buf);
    if (n <= 0 || n >= JSON_BUF_SIZE) {
        PR_ERR("json too long");
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    rt = __http_post_json(ASR_PATH, sg_app.json_buf, sg_app.resp_buf,
                          RESP_BUF_SIZE, &status);
    if (OPRT_OK != rt) {
        return rt;
    }
    if (status != 200) {
        PR_ERR("asr http %d: %.360s", status, sg_app.resp_buf);
        return OPRT_COM_ERROR;
    }

    cJSON *root = cJSON_Parse(sg_app.resp_buf);
    if (!root) { return OPRT_CJSON_PARSE_ERR; }
    cJSON *sent = cJSON_GetObjectItem(root, "sentence");
    cJSON *text = NULL;
    if (cJSON_IsObject(sent)) {
        text = cJSON_GetObjectItem(sent, "text");
    } else if (cJSON_IsArray(sent) && cJSON_GetArraySize(sent) > 0) {
        text = cJSON_GetObjectItem(cJSON_GetArrayItem(sent, 0), "text");
    }
    out[0] = '\0';
    if (cJSON_IsString(text) && text->valuestring) {
        strncpy(out, text->valuestring, out_len - 1);
    }
    cJSON_Delete(root);
    return OPRT_OK;
}

/* 组装 LLM 请求 JSON 并调用 */
static OPERATE_RET __do_llm(const char *question, char *out, int out_len)
{
    OPERATE_RET rt;
    int status = 0;

    char q_esc[600];
    const char *s = question;
    char *d = q_esc;
    char *dend = q_esc + sizeof(q_esc) - 4;
    while (*s && d < dend) {
        if ('"' == *s || '\\' == *s) { *d++ = '\\'; }
        else if ('\n' == *s) { *d++ = ' '; s++; continue; }
        *d++ = *s++;
    }
    *d = '\0';

    int n = snprintf(sg_app.json_buf, JSON_BUF_SIZE,
        "{\"model\":\"%s\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}]}",
        LLM_MODEL, SYS_PROMPT, q_esc);
    if (n <= 0 || n >= JSON_BUF_SIZE) {
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    rt = __http_post_json(LLM_PATH, sg_app.json_buf, sg_app.resp_buf,
                          RESP_BUF_SIZE, &status);
    if (OPRT_OK != rt) { return rt; }
    if (status != 200) {
        PR_ERR("llm http %d: %.200s", status, sg_app.resp_buf);
        return OPRT_COM_ERROR;
    }

    cJSON *root = cJSON_Parse(sg_app.resp_buf);
    if (!root) { return OPRT_CJSON_PARSE_ERR; }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    out[0] = '\0';
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *msg = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (cJSON_IsString(content) && content->valuestring) {
            strncpy(out, content->valuestring, out_len - 1);
        }
    }
    cJSON_Delete(root);
    return OPRT_OK;
}

/***********************************************************
**************************CLI********************************
***********************************************************/
static void __cmd_wifi(char *out, int len, int argc, char *argv[])
{
    (void)out; (void)len;
    if (argc < 3) {
        PR_NOTICE("用法：wifi <名称> <密码>");
        return;
    }
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    netconn_wifi_info_t info = {0};
    strncpy(info.ssid, argv[1], sizeof(info.ssid) - 1);
    strncpy(info.pswd, argv[2], sizeof(info.pswd) - 1);
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &info);
    PR_NOTICE("正在连接 WiFi %s ...", info.ssid);
#endif
}

static void __cmd_chat(char *out, int len, int argc, char *argv[])
{
    (void)out; (void)len;
    if (argc < 2) {
        PR_NOTICE("用法：chat <文本>");
        return;
    }
    if (APP_ST_IDLE != sg_app.state) {
        PR_NOTICE("当前正忙，请稍后再试");
        return;
    }
    sg_app.question[0] = '\0';
    for (int i = 1; i < argc; i++) {
        size_t cur = strlen(sg_app.question);
        snprintf(sg_app.question + cur, sizeof(sg_app.question) - cur, "%s ", argv[i]);
    }
    sg_app.rec_len = 0;                         /* 跳过录音 */
    sg_app.state = APP_ST_PROCESSING;
    tal_semaphore_post(sg_app.work_sem);
}

static void __cmd_net(char *out, int len, int argc, char *argv[])
{
    (void)out; (void)len; (void)argc; (void)argv;
    netmgr_status_e st = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_STATUS, &st);
    PR_NOTICE("net status: %d (0=down 1=up)", (int)st);
}

/***********************************************************
******************UART0(烧录口)命令通道**********************
***********************************************************/
/* 墨水屏没接、UART0 被 CP shell 占用，故用烧录口(USB 双串口的第 1 口)
 * 做命令通道：HTML 控制台连此口发命令，固件在此解析执行。*/
static void __uart0_out(const char *s)
{
    tal_uart_write(TUYA_UART_NUM_0, (void *)s, strlen(s));
}

/* 把一轮问答结果回显到命令通道，页面不用切日志口也能看到文字 */
static void __notify_answer(const char *q, const char *a)
{
    char buf[640];
    if (q && q[0]) {
        snprintf(buf, sizeof(buf), "RSP:已识别 %s\r\n", q);
        __uart0_out(buf);
    }
    if (a && a[0]) {
        snprintf(buf, sizeof(buf), "RSP:回答 %s\r\n", a);
        __uart0_out(buf);
    }
}

static void __uart_cmd_dispatch(char *line)
{
    char reply[256];

    if (0 == strncmp(line, "wifi ", 5)) {
        char *ssid = line + 5;
        char *pwd  = strchr(ssid, ' ');
        if (pwd) { *pwd = '\0'; pwd++; }
        if (ssid[0] && pwd && pwd[0]) {
            __cmd_wifi(NULL, 0, 3, (char *[]){(char *)"wifi", ssid, pwd});
            snprintf(reply, sizeof(reply), "RSP:配网 正在连接 %s\r\n", ssid);
        } else {
            snprintf(reply, sizeof(reply), "RSP:错误 用法 wifi <名称> <密码>\r\n");
        }
    } else if (0 == strcmp(line, "net")) {
        netmgr_status_e st = NETMGR_LINK_DOWN;
        netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_STATUS, &st);
        NW_IP_S ip = {0};
        if (NETMGR_LINK_UP == st || NETMGR_LINK_UP_SWITH == st) {
            netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_IP, &ip);
            snprintf(reply, sizeof(reply), "RSP:网络 已连接 IP=%s 网关=%s\r\n",
                     ip.ip[0] ? ip.ip : "0.0.0.0", ip.gw[0] ? ip.gw : "-");
        } else {
            snprintf(reply, sizeof(reply), "RSP:网络 %s\r\n",
                     (NETMGR_LINK_UP == st) ? "已连接" : "未连接");
        }
    } else if (0 == strncmp(line, "chat ", 5)) {
        char *text = line + 5;
        if (text[0]) {
            __cmd_chat(NULL, 0, 2, (char *[]){(char *)"chat", text});
            snprintf(reply, sizeof(reply), "RSP:已发送 等待回答\r\n");
        } else {
            snprintf(reply, sizeof(reply), "RSP:错误 用法 chat <文本>\r\n");
        }
    } else if (0 == strcmp(line, "rt")) {
        /* 只验证实时通道：TLS + WebSocket 升级握手，随后立即关闭 */
        if (__rt_connect()) {
            s_rt_last_ev[0] = '\0';
            for (int i = 0; i < 40; i++) {
                __ws_poll();
                tal_system_sleep(20);
            }
            snprintf(reply, sizeof(reply), "RSP:实时通道 握手成功 事件=%s\r\n",
                     s_rt_last_ev[0] ? s_rt_last_ev : "none");
        } else {
            snprintf(reply, sizeof(reply), "RSP:实时通道 握手失败\r\n");
        }
        __ws_close();
    } else if (0 == strncmp(line, "wake", 4)) {
        char *arg = line + 4;
        while (' ' == *arg) { arg++; }
        if (0 == strncmp(arg, "thr", 3)) {
            char *v = arg + 3;
            while (' ' == *v) { v++; }
            if (v[0]) { s_wake_thr = atoi(v); }
            if (s_wake_thr < 30) { s_wake_thr = 30; }
            snprintf(reply, sizeof(reply), "RSP:待机 灵敏度门限=%d\r\n", s_wake_thr);
        } else if (0 == strcmp(arg, "off")) {
            s_wake_on = FALSE;
            snprintf(reply, sizeof(reply), "RSP:待机 已关闭\r\n");
        } else if (arg[0] && 0 != strcmp(arg, "on")) {
            snprintf(s_wake_word, sizeof(s_wake_word), "%s", arg);
            s_wake_on = TRUE;
            s_wake_cnt = 0;
            s_wake_last[0] = '\0';
            snprintf(reply, sizeof(reply), "RSP:待机 已开启 唤醒词=%s\r\n", s_wake_word);
        } else if (arg[0]) {
            s_wake_on = TRUE;
            s_wake_cnt = 0;
            snprintf(reply, sizeof(reply), "RSP:WAKE on word=%s\r\n", s_wake_word);
        } else {
            snprintf(reply, sizeof(reply),
                     "RSP:待机 %s 唤醒词=%s 命中=%u 音量=%d 门限=%d 最近听到=%s\r\n",
                     s_wake_on ? "已开启" : "已关闭", s_wake_word,
                     (unsigned)s_wake_cnt, s_wake_peak, s_wake_thr,
                     s_wake_last[0] ? s_wake_last : "无");
        }
    } else if (0 == strncmp(line, "mic", 3)) {
        /* 麦克风诊断：录 N 秒，回显峰值，用于确认拾音与门限 */
        int sec = 3;
        char *arg = line + 3;
        while (' ' == *arg) { arg++; }
        if (arg[0]) { sec = atoi(arg); }
        if (sec < 1) { sec = 1; }
        if (sec > 20) { sec = 20; }
        s_mic_test = TRUE;
        tal_system_sleep(300);
        sg_app.rec_len = 0;
        sg_app.rec_active = TRUE;
        tal_system_sleep(sec * 1000);
        sg_app.rec_active = FALSE;
        int pk = __wake_peak_scan();
        s_mic_test = FALSE;
        snprintf(reply, sizeof(reply), "RSP:拾音 %d秒 峰值=%d 字节=%u\r\n",
                 sec, pk, (unsigned)sg_app.rec_len);
    } else if (0 == strncmp(line, "call", 4)) {
        char *arg = line + 4;
        while (' ' == *arg) { arg++; }
        if (0 == strcmp(arg, "end")) {
            if (eink_xz_is_active()) {
                eink_xz_end();
                snprintf(reply, sizeof(reply), "RSP:正在结束对话\r\n");
            } else if (s_rt_live) {
                s_rt_end_req = TRUE;
                snprintf(reply, sizeof(reply), "RSP:正在结束对话\r\n");
            } else {
                snprintf(reply, sizeof(reply), "RSP:当前没有进行中的对话\r\n");
            }
        } else if (0 == arg[0]) {
            if (eink_xz_enabled()) {
                eink_xz_start();
                snprintf(reply, sizeof(reply), "RSP:小智连接中 请稍候\r\n");
            } else if (__rt_live_start()) {
                snprintf(reply, sizeof(reply), "RSP:实时对话开始 请直接说话\r\n");
            } else {
                snprintf(reply, sizeof(reply), "RSP:对话失败 通道连接错误或正忙\r\n");
            }
        } else {
            snprintf(reply, sizeof(reply), "RSP:用法 call [end]\r\n");
        }
    } else if (0 == strncmp(line, "xz", 2) &&
               (0 == line[2] || ' ' == line[2])) {
        /* 小智协议通道：xz 开会话 | xz end 结束 | xz on/off 模式开关 */
        char *arg = line + 2;
        while (' ' == *arg) { arg++; }
        if (0 == strcmp(arg, "end")) {
            if (eink_xz_is_active()) {
                eink_xz_end();
                snprintf(reply, sizeof(reply), "RSP:正在结束小智对话\r\n");
            } else {
                snprintf(reply, sizeof(reply), "RSP:当前没有小智对话\r\n");
            }
        } else if (0 == strcmp(arg, "pre")) {
            snprintf(reply, sizeof(reply), eink_xz_preconnect()
                     ? "RSP:预连接已发起 握手期间不开麦\r\n"
                     : "RSP:预连接未发起（已有会话/冷却中/小智关闭）\r\n");
        } else if (0 == strcmp(arg, "arm")) {
            eink_xz_arm();
            for (int i = 0; i < 100 && !eink_xz_is_active(); i++) {
                if (!eink_xz_engaged()) { break; }
                tal_system_sleep(80);
            }
            snprintf(reply, sizeof(reply), eink_xz_is_active()
                     ? "RSP:已开麦 可直接说话\r\n" : "RSP:通道未就绪 见日志\r\n");
        } else if (0 == strcmp(arg, "abort")) {
            eink_xz_abort();
            snprintf(reply, sizeof(reply), "RSP:已放弃预连接\r\n");
        } else if (0 == strncmp(arg, "boost", 5)) {
            int b = atoi(arg + 5);
            if (b < 0) { b = 0; }
            if (b > 60) { b = 60; }
            eink_xz_set_boost(b);
            snprintf(reply, sizeof(reply), "RSP:开局增益补偿=%d%% 持续3秒\r\n", b);
        } else if (0 == strcmp(arg, "on")) {
            eink_xz_set_enable(TRUE);
            snprintf(reply, sizeof(reply), "RSP:小智模式已开启 唤醒词将走 xiaozhi.me\r\n");
        } else if (0 == strcmp(arg, "off")) {
            eink_xz_set_enable(FALSE);
            eink_xz_standby_off();
            snprintf(reply, sizeof(reply), "RSP:小智模式已关闭\r\n");
        } else if (0 == arg[0]) {
            eink_xz_start();
            snprintf(reply, sizeof(reply), "RSP:小智连接中 请稍候\r\n");
        } else {
            snprintf(reply, sizeof(reply), "RSP:用法 xz | xz end | xz on/off\r\n");
        }
    } else if (0 == strncmp(line, "spk", 3) &&
               (0 == line[3] || ' ' == line[3])) {
        /* 喇叭自检：播放 1s 1kHz 正弦波（1kHz@16k = 每 16 样本一周期）
         * spk      播放前强制拉高 GPIO39（本板功放使能脚，高电平使能）
         * spk l/h  强制拉低/拉高 GPIO39 */
        char *arg = line + 3;
        while (' ' == *arg) { arg++; }
        static int16_t tone[640];
        TDL_AUDIO_HANDLE_T audio = eink_chat_audio_handle();
        if (NULL == audio) {
            snprintf(reply, sizeof(reply), "RSP:spk fail audio handle NULL\r\n");
        } else {
            TUYA_GPIO_LEVEL_E pa_lv = TUYA_GPIO_LEVEL_HIGH;
            if ('l' == arg[0] || 'L' == arg[0]) { pa_lv = TUYA_GPIO_LEVEL_LOW; }
            {
                TUYA_GPIO_BASE_CFG_T gcfg = {0};
                gcfg.direct = TUYA_GPIO_OUTPUT;
                gcfg.mode   = TUYA_GPIO_PULLUP;
                gcfg.level  = pa_lv;
                tkl_gpio_init(TUYA_GPIO_NUM_39, &gcfg);
                tkl_gpio_write(TUYA_GPIO_NUM_39, pa_lv);
            }
            for (int blk = 0; blk < 25; blk++) {
                for (int i = 0; i < 640; i++) {
                    float ph = (float)(((blk * 640 + i) % 16)) / 16.0f;
                    tone[i] = (int16_t)(10000.0f * eink_xz_get_speaker_vol() / 100.0f * sinf(6.2831853f * ph));
                }
                tdl_audio_play(audio, (uint8_t *)tone, 1280);
            }
            eink_chat_spk_mute(1800);
            snprintf(reply, sizeof(reply), "RSP:spk%c 播放完成 PA39=%s 请听喇叭\r\n",
                     (TUYA_GPIO_LEVEL_HIGH == pa_lv) ? 'h' : 'l',
                     (TUYA_GPIO_LEVEL_HIGH == pa_lv) ? "HIGH(使能)" : "LOW(静音)");
        }
    } else if (0 == strcmp(line, "spkab")) {
        /* PA 极性 A/B 对照：连续 10 秒 beep，前 5 秒 PA=低、后 5 秒 PA=高。
         * 听第 5 秒处声音是否消失/出现，即可判定功放真实使能电平 */
        static int16_t ab_tone[640];
        TDL_AUDIO_HANDLE_T audio = eink_chat_audio_handle();
        if (NULL == audio) {
            snprintf(reply, sizeof(reply), "RSP:spkab fail audio handle NULL\r\n");
        } else {
            for (int seg = 0; seg < 2; seg++) {
                TUYA_GPIO_BASE_CFG_T gc = {0};
                gc.direct = TUYA_GPIO_OUTPUT;
                gc.mode   = TUYA_GPIO_PULLDOWN;
                gc.level  = (0 == seg) ? TUYA_GPIO_LEVEL_LOW : TUYA_GPIO_LEVEL_HIGH;
                tkl_gpio_init(TUYA_GPIO_NUM_39, &gc);
                tkl_gpio_write(TUYA_GPIO_NUM_39, gc.level);
                for (int blk = 0; blk < 125; blk++) {   /* 5 秒 = 125×40ms */
                    for (int i = 0; i < 640; i++) {
                        float ph = (float)(((blk * 640 + i) % 16)) / 16.0f;
                        ab_tone[i] = (int16_t)(10000.0f * eink_xz_get_speaker_vol() / 100.0f * sinf(6.2831853f * ph));
                    }
                    tdl_audio_play(audio, (uint8_t *)ab_tone, 1280);
                }
            }
            eink_chat_spk_mute(11500);
            snprintf(reply, sizeof(reply),
                     "RSP:spkab 完成 前5秒PA=低 后5秒PA=高 第5秒处有变化吗\r\n");
        }
    } else if (0 == strncmp(line, "spklong", 7)) {
        /* 连续恒幅 1kHz 长音：用来判断「唤醒后开头几句声音小」是硬件起振还是数据侧。
         * 听感对照：前 1~2 秒明显偏小、越往后越大 → 功放/耦合电容起振；
         * 从头到尾一样响 → 硬件正常，问题在链路侧（看 xz boost / 播放预热） */
        int sec = 8;
        char *arg = line + 7;
        while (' ' == *arg) { arg++; }
        if (arg[0]) { sec = atoi(arg); }
        if (sec < 1) { sec = 1; }
        if (sec > 15) { sec = 15; }
        TDL_AUDIO_HANDLE_T audio = eink_chat_audio_handle();
        if (NULL == audio) {
            snprintf(reply, sizeof(reply), "RSP:spklong fail audio handle NULL\r\n");
        } else {
            static int16_t tlong[640];
            const int blocks = sec * 25;        /* 40ms 一块 */
            eink_chat_spk_mute(sec * 1000 + 2000);
            __uart_reply("RSP:spklong 开始 恒幅1kHz 注意听开头与后段是否一样响\r\n");
            for (int blk = 0; blk < blocks; blk++) {
                for (int i = 0; i < 640; i++) {
                    float ph = (float)(((blk * 640 + i) % 16)) / 16.0f;
                    tlong[i] = (int16_t)(16000.0f * eink_xz_get_speaker_vol() / 100.0f
                                         * sinf(6.2831853f * ph));
                }
                tdl_audio_play(audio, (uint8_t *)tlong, 1280);
                if (0 == (blk % 25)) {
                    PR_NOTICE("[SPK] 长音 t=%ds/%ds", blk / 25, sec);
                }
            }
            eink_chat_spk_mute(1500);
            snprintf(reply, sizeof(reply), "RSP:spklong 完成 %d秒 恒幅1kHz\r\n", sec);
        }
    } else if (0 == strncmp(line, "vol", 3) &&
               (0 == line[3] || ' ' == line[3])) {
        /* 喇叭音量：vol 查询 | vol 0-100 设置并断电保存。
         * 软件音量：DAC 增益固定拉满保信噪比，应用层对播放样本衰减 */
        if (0 == line[3]) {
            snprintf(reply, sizeof(reply), "RSP:vol 当前音量=%d (0-100)\r\n",
                     eink_xz_get_speaker_vol());
        } else {
            int v = atoi(line + 4);
            if (v < 0) { v = 0; }
            if (v > 100) { v = 100; }
            eink_xz_set_speaker_vol((uint8_t)v);
            uint8_t sv = (uint8_t)v;
            (void)tal_kv_set("spk_vol", &sv, 1);
            snprintf(reply, sizeof(reply), "RSP:vol 音量=%d 已断电保存\r\n", sv);
        }
    } else if (0 == strcmp(line, "vt")) {
        /* bk 层 voice 回环测试：mic→spk 直通。
         * 对麦克风说话应能从喇叭听到自己的声音（验证 DAC/PA/喇叭模拟链路）。
         * 测试后需重启设备恢复待机识别 */
        extern OPERATE_RET tkl_ai_uninit(void);
        extern void cli_voice_test_cmd(char *pcWriteBuffer, int xWriteBufferLen,
                                       int argc, char **argv);
        tkl_ai_uninit();
        char *vargv[9] = {"voice", "start", "onboard", "16000", "0",
                          "pcm", "pcm", "onboard", "16000"};
        cli_voice_test_cmd(NULL, 0, 9, vargv);
        eink_chat_spk_mute(2500);
        snprintf(reply, sizeof(reply), "RSP:voice 回环已启动 对mic说话测试 重启恢复\r\n");
    } else if (0 == strncmp(line, "spkscan", 7) &&
               (0 == line[7] || ' ' == line[7])) {
        /* 功放使能引脚扫描（低电平使能 + 报数模式）：
         * 第 n 个候选引脚连响 n 声 beep（数 beep 次数即知引脚序号）
         * 序号->引脚：1=GPIO7 2=17 3=19 4=23 5=26 6=27 7=28 8=39 9=42 10=45 */
        char *arg = line + 7;
        while (' ' == *arg) { arg++; }
        TUYA_GPIO_LEVEL_E en_lv = ('h' == arg[0] || 'H' == arg[0])
                                  ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW;
        static const int cand[] = {7, 17, 19, 23, 26, 27, 28, 39, 42, 45};
        static int16_t tone[640];
        TDL_AUDIO_HANDLE_T audio = eink_chat_audio_handle();
        if (NULL == audio) {
            snprintf(reply, sizeof(reply), "RSP:spkscan fail audio handle NULL\r\n");
        } else {
            for (int gi = 0; gi < (int)(sizeof(cand) / sizeof(cand[0])); gi++) {
                TUYA_GPIO_BASE_CFG_T gcfg = {0};
                gcfg.direct = TUYA_GPIO_OUTPUT;
                gcfg.mode   = TUYA_GPIO_PULLUP;
                gcfg.level  = (TUYA_GPIO_LEVEL_HIGH == en_lv) ? TUYA_GPIO_LEVEL_LOW
                                                              : TUYA_GPIO_LEVEL_HIGH;
                tkl_gpio_init((TUYA_GPIO_NUM_E)cand[gi], &gcfg);
                tkl_gpio_write((TUYA_GPIO_NUM_E)cand[gi], en_lv);
                for (int k = 0; k <= gi; k++) {
                    /* 一声 beep：6 块 1.2kHz（约 0.24s） */
                    for (int blk = 0; blk < 6; blk++) {
                        for (int i = 0; i < 640; i++) {
                            float ph = (float)(i % 13) / 13.0f;
                            tone[i] = (int16_t)(10000.0f * eink_xz_get_speaker_vol() / 100.0f * sinf(6.2831853f * ph));
                        }
                        tdl_audio_play(audio, (uint8_t *)tone, 1280);
                    }
                    /* beep 间隔：6 块静音 */
                    memset(tone, 0, sizeof(tone));
                    for (int blk = 0; blk < 6; blk++) {
                        tdl_audio_play(audio, (uint8_t *)tone, 1280);
                    }
                }
                /* 引脚间隔：约 1.5s 静音 */
                for (int blk = 0; blk < 37; blk++) {
                    tdl_audio_play(audio, (uint8_t *)tone, 1280);
                }
                tkl_gpio_write((TUYA_GPIO_NUM_E)cand[gi], gcfg.level);
                snprintf(reply, sizeof(reply), "RSP:第%d个 GPIO%d(共%d声) 下一个\r\n",
                         gi + 1, cand[gi], gi + 1);
                __uart0_out(reply);
            }
            snprintf(reply, sizeof(reply), "RSP:扫描结束 数beep次数定位引脚\r\n");
            __uart0_out(reply);
            eink_chat_spk_mute(2500);
            return;
        }
    } else if (0 == strncmp(line, "spkbin", 6) &&
               (0 == line[6] || ' ' == line[6])) {
        /* 二分测试：spkbin <组号>，每次只测一组候选引脚（低电平使能）
         * 组0=GPIO7,17,19,23  组1=GPIO26,27,28,39  组2=GPIO42,45 */
        char *arg = line + 6;
        while (' ' == *arg) { arg++; }
        /* 完整数字解析：0/1/2=组测试；>=10=单引脚测试 */
        int grp = arg[0] ? (int)strtol(arg, NULL, 10) : 0;
        static const int grp0[] = {7, 17, 19, 23};
        static const int grp1[] = {26, 27, 28, 39};
        static const int grp2[] = {42, 45};
        const int *list;
        int cnt;
        if (grp <= 2) {
            list = (0 == grp) ? grp0 : (1 == grp) ? grp1 : grp2;
            cnt = (0 == grp || 1 == grp) ? 4 : 2;
        } else {
            list = &grp;  /* 复用 grp 变量存引脚号 */
            cnt = 1;
        }
        static int16_t tone[640];
        TDL_AUDIO_HANDLE_T audio = eink_chat_audio_handle();
        if (NULL == audio) {
            snprintf(reply, sizeof(reply), "RSP:spkbin fail audio handle NULL\r\n");
        } else {
            for (int gi = 0; gi < cnt; gi++) {
                TUYA_GPIO_BASE_CFG_T gcfg = {0};
                gcfg.direct = TUYA_GPIO_OUTPUT;
                gcfg.mode   = TUYA_GPIO_PULLUP;
                gcfg.level  = TUYA_GPIO_LEVEL_HIGH;
                tkl_gpio_init((TUYA_GPIO_NUM_E)list[gi], &gcfg);
                tkl_gpio_write((TUYA_GPIO_NUM_E)list[gi], TUYA_GPIO_LEVEL_LOW);
                /* 1 秒测试音 */
                for (int blk = 0; blk < 25; blk++) {
                    for (int i = 0; i < 640; i++) {
                        float ph = (float)(i % 13) / 13.0f;
                        tone[i] = (int16_t)(10000.0f * eink_xz_get_speaker_vol() / 100.0f * sinf(6.2831853f * ph));
                    }
                    tdl_audio_play(audio, (uint8_t *)tone, 1280);
                }
                tkl_gpio_write((TUYA_GPIO_NUM_E)list[gi], TUYA_GPIO_LEVEL_HIGH);
                /* 1.2 秒静音间隔 */
                memset(tone, 0, sizeof(tone));
                for (int blk = 0; blk < 30; blk++) {
                    tdl_audio_play(audio, (uint8_t *)tone, 1280);
                }
                snprintf(reply, sizeof(reply), "RSP:测完 GPIO%d\r\n", list[gi]);
                __uart0_out(reply);
            }
            snprintf(reply, sizeof(reply), "RSP:第%d组完成 请回答响了吗\r\n", grp);
            __uart0_out(reply);
            return;
        }
    } else if (0 == strcmp(line, "ver")) {
        snprintf(reply, sizeof(reply), "RSP:版本 eink_chat 1.8 小智预连接+起振补偿\r\n");
    } else if (0 == strcmp(line, "help")) {
        snprintf(reply, sizeof(reply),
                 "RSP:命令 wifi <名称> <密码> | net | chat <文本> | rt | wake <唤醒词|on|off|thr 门限> | mic <秒> | call [end] | xz [end|on|off|pre|arm|abort|boost N] | spk 喇叭自检 | spklong <秒> | vol <0-100> | ver\r\n");
    } else if (line[0]) {
        snprintf(reply, sizeof(reply), "RSP:错误 未知命令\r\n");
    } else {
        return;
    }
    __uart0_out(reply);
}

static void __cmd_uart_thread(void *arg)
{
    static char line[256];
    int pos = 0;
    uint8_t buf[64];
    int hb = 0;
    (void)arg;

    while (1) {
        int n = tal_uart_read(TUYA_UART_NUM_0, buf, sizeof(buf));
        if (n > 0) {
            /* 调试：收到任何字节都回显到日志口 */
            PR_NOTICE("uart0 rx %d bytes: %.*s", n, n > 40 ? 40 : n, (char *)buf);
            for (int i = 0; i < n; i++) {
                char c = (char)buf[i];
                if ('\n' == c || '\r' == c) {
                    if (pos > 0) {
                        line[pos] = '\0';
                        pos = 0;
                        __uart_cmd_dispatch(line);
                    }
                } else if (pos < (int)sizeof(line) - 1) {
                    line[pos++] = c;
                }
            }
        } else {
            tal_system_sleep(20);
            if (++hb >= 50) {           /* 1 秒心跳，验证 TX 通路 */
                hb = 0;
                __uart0_out("RSP:hb0\r\n");
            }
        }
    }
}



/***********************************************************
*******************realtime 语音通话***********************
***********************************************************/
#define RT_URI "wss://llm-rwc82wtulg6ogzm2.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime?model=qwen-audio-3.1-realtime-flash"
#define RT_HEADERS "Origin: https://dashscope.aliyuncs.com\r\nAuthorization: Bearer " AI_API_KEY "\r\n"
#define RT_ACCUM_SZ     (64 * 1024)   /* response.audio.delta 实测单帧可达 ~25KB，24KB 会误判 frame too large 并断连 */
#define RT_PLAY_RING_SZ (64 * 1024)
#define RT_CHUNK        1280        /* 40ms @16k PCM16 */
#define RT_TAIL_BYTES   (32000)     /* 2s 静音尾巴，触发 VAD 收尾 */

static tuya_transporter_t s_rt_tcp = NULL;
static tuya_transporter_t s_rt_tls = NULL;
static volatile BOOL_T    s_rt_ws_up = FALSE;

static volatile uint32_t s_rt_sent = 0;
static volatile uint32_t s_rt_tail = 0;
static char           *s_rt_accum = NULL;      /* PSRAM 拼帧 */
static volatile uint32_t s_rt_accum_len = 0;
static volatile uint32_t s_rt_payload_len = 0;
static volatile BOOL_T   s_rt_first_app = FALSE;   /* 本会话首帧音频打点 */
static char           *s_rt_json = NULL;       /* PSRAM 上行 JSON */
static char           *s_rt_pcm  = NULL;       /* PSRAM 下行解码 */
static uint8_t        *s_rt_ring = NULL;       /* PSRAM 播放环形缓冲 */
static volatile uint32_t s_rt_wr = 0, s_rt_rd = 0;
static char            s_rt_asr[256];
static char            s_rt_ans[512];
static volatile BOOL_T s_rt_want = FALSE;      /* 下一轮是否尝试直连实时会话 */

static void __rt_handle_msg(char *msg);
static BOOL_T __rt_connect(void);

/* 下行音频：24k -> 16k 线性插值后入环形缓冲 */
static void __rt_queue_audio(const char *b64)
{
    if (!s_rt_pcm) {
        return;
    }
    int nb = tuya_base64_decode((char *)b64, (unsigned char *)s_rt_pcm);
    if (nb < 4) {
        return;
    }
    int in_samples = nb / 2;
    int out_samples = in_samples * 2 / 3;
    int16_t *in = (int16_t *)s_rt_pcm;

    for (int i = 0; i < out_samples; i++) {
        float pos = i * 1.5f;
        int   i0 = (int)pos;
        float f  = pos - i0;
        int16_t a = in[i0];
        int16_t b = (i0 + 1 < in_samples) ? in[i0 + 1] : a;
        int16_t v = (int16_t)(a + (b - a) * f);

        uint32_t next = s_rt_wr + 2;
        if ((next - s_rt_rd) > RT_PLAY_RING_SZ) {
            break;   /* 环形满，丢尾 */
        }
        s_rt_ring[s_rt_wr % RT_PLAY_RING_SZ]        = (uint8_t)(v & 0xFF);
        s_rt_ring[(s_rt_wr + 1) % RT_PLAY_RING_SZ]  = (uint8_t)((v >> 8) & 0xFF);
        s_rt_wr = next;
    }
}

static void __rt_handle_msg(char *msg)
{
    {
        const char *tp = strstr(msg, "\"type\":\"");
        if (tp) {
            tp += 8;
            int i = 0;
            while (tp[i] && tp[i] != '"' && i < (int)sizeof(s_rt_last_ev) - 1) {
                s_rt_last_ev[i] = tp[i];
                i++;
            }
            s_rt_last_ev[i] = '\0';
        }
    }

    cJSON *root = cJSON_Parse(msg);
    if (NULL == root) {
        return;
    }
    cJSON *jt = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(jt)) {
        cJSON_Delete(root);
        return;
    }
    const char *ty = jt->valuestring;
    PR_NOTICE("[RT] ev %s", ty);

    /* 只有识别/回答类事件才算真实交互；VAD 起停等噪声事件不算，
     * 否则静音下服务端周期性 speech_started 会阻止空闲超时 */
    if (0 == strncmp(ty, "response.", 9) || NULL != strstr(ty, "transcription")) {
        s_rt_idle_ms = 0;
    }

    if (strstr(ty, "input_audio_transcription.delta")) {
        cJSON *jd = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(jd)) {
            size_t cur = strlen(s_rt_asr);
            snprintf(s_rt_asr + cur, sizeof(s_rt_asr) - cur, "%s", jd->valuestring);
        }
    } else if (strstr(ty, "input_audio_transcription.completed")) {
        cJSON *jt2 = cJSON_GetObjectItem(root, "transcript");
        if (cJSON_IsString(jt2)) {
            snprintf(s_rt_asr, sizeof(s_rt_asr), "%s", jt2->valuestring);
            PR_NOTICE("[你说] %s", s_rt_asr);
        }
    } else if (0 == strcmp(ty, "response.audio.delta")) {
        cJSON *jd = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(jd)) {
            __rt_queue_audio(jd->valuestring);
        }
    } else if (strstr(ty, "response.audio_transcript.delta")) {
        cJSON *jd = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(jd)) {
            size_t cur = strlen(s_rt_ans);
            snprintf(s_rt_ans + cur, sizeof(s_rt_ans) - cur, "%s", jd->valuestring);
        }
    } else if (0 == strcmp(ty, "response.done")) {
        PR_NOTICE("[AI] %s", s_rt_ans);
        if (s_rt_live) {
            char buf[640];
            snprintf(buf, sizeof(buf), "RSP:回答 %s\r\n", s_rt_ans);
            __uart0_out(buf);
            /* 播放结束、静默保护过后由 rt_thread 继续上行监听，无需改状态 */
        } else {
            s_rt_state = RT_IDLE;
            s_rt_sent = 0;
            s_rt_tail = 0;
        }
    } else if (0 == strcmp(ty, "error")) {
        PR_ERR("[RT] err: %s", msg);
        if (s_rt_live) {
            s_rt_live = FALSE;
            sg_app.rec_active = FALSE;
            sg_app.state = APP_ST_IDLE;
            __uart0_out("RSP:对话 错误，会话结束\r\n");
        }
        s_rt_state = RT_IDLE;
    }
    cJSON_Delete(root);
}

#define RT_HOST "llm-rwc82wtulg6ogzm2.cn-beijing.maas.aliyuncs.com"
#define RT_PATH "/api-ws/v1/realtime?model=qwen-audio-3.1-realtime-flash"

/* 读满 n 字节 */
static int __ws_read_exact(uint8_t *buf, uint32_t n, int timeout_ms)
{
    uint32_t got = 0;
    while (got < n) {
        int r = tuya_transporter_read(s_rt_tls, buf + got, n - got, timeout_ms);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    return 0;
}

/* 发送一帧（客户端必须 mask） */
static int __ws_send_frame(uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    if (!s_rt_ws_up || NULL == s_rt_tls) {
        return -1;
    }
    uint8_t hdr[14];
    int hl = 0;
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

    hdr[hl++] = 0x80 | opcode;
    if (len < 126) {
        hdr[hl++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        hdr[hl++] = 0x80 | 126;
        hdr[hl++] = (uint8_t)(len >> 8);
        hdr[hl++] = (uint8_t)(len & 0xFF);
    } else {
        hdr[hl++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) {
            hdr[hl++] = (uint8_t)(((uint64_t)len >> (i * 8)) & 0xFF);
        }
    }
    memcpy(hdr + hl, mask, 4);
    hl += 4;

    if (tuya_transporter_write(s_rt_tls, hdr, hl, 2000) < 0) {
        return -1;
    }
    /* 分片写 payload，避免大块临时缓冲 */
    uint8_t chunk[256];
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = (len - off > sizeof(chunk)) ? sizeof(chunk) : (len - off);
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = payload[off + i] ^ mask[(off + i) & 3];
        }
        if (tuya_transporter_write(s_rt_tls, chunk, n, 2000) < 0) {
            return -1;
        }
        off += n;
    }
    return 0;
}

static void __ws_close(void)
{
    s_rt_ws_up = FALSE;
    if (s_rt_tls) {
        tuya_transporter_close(s_rt_tls);
        tuya_transporter_destroy(s_rt_tls);
        s_rt_tls = NULL;
    }
    if (s_rt_tcp) {
        tuya_transporter_destroy(s_rt_tcp);
        s_rt_tcp = NULL;
    }
}

static BOOL_T __rt_connect(void)
{
    if (s_rt_ws_up && s_rt_tls) {
        return TRUE;
    }
    __ws_close();

    s_rt_tcp = tuya_transporter_create(TRANSPORT_TYPE_TCP, NULL);
    if (NULL == s_rt_tcp) {
        PR_ERR("[RT] tcp transporter create failed");
        return FALSE;
    }
    s_rt_tls = tuya_transporter_create(TRANSPORT_TYPE_TLS, s_rt_tcp);
    if (NULL == s_rt_tls) {
        PR_ERR("[RT] tls transporter create failed");
        __ws_close();
        return FALSE;
    }

    /* 未接入 Tuya 云证书服务，实时通道同样使用内置 CA 校验 */
    tuya_tls_config_t rt_tls_cfg = {0};
    rt_tls_cfg.mode         = TUYA_TLS_SERVER_CERT_MODE;
    rt_tls_cfg.hostname     = (char *)RT_HOST;
    rt_tls_cfg.port         = 443;
    rt_tls_cfg.timeout      = 8000;
    rt_tls_cfg.verify       = true;
    rt_tls_cfg.ca_cert      = (char *)g_aliyun_ca_pem;
    rt_tls_cfg.ca_cert_size = (int)g_aliyun_ca_pem_len;
    if (OPRT_OK != tuya_transporter_ctrl(s_rt_tls, TUYA_TRANSPORTER_SET_TLS_CONFIG, &rt_tls_cfg)) {
        PR_ERR("[RT] tls config failed");
        __ws_close();
        return FALSE;
    }
    if (OPRT_OK != tuya_transporter_connect(s_rt_tls, RT_HOST, 443, 8000)) {
        PR_ERR("[RT] tls connect failed");
        __ws_close();
        return FALSE;
    }

    /* HTTP 101 升级请求 */
    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: https://dashscope.aliyuncs.com\r\n"
        "Authorization: Bearer " AI_API_KEY "\r\n"
        "\r\n", RT_PATH, RT_HOST);
    if (tuya_transporter_write(s_rt_tls, (uint8_t *)req, n, 3000) < 0) {
        PR_ERR("[RT] upgrade write failed");
        __ws_close();
        return FALSE;
    }

    /* 读 101 响应头 */
    char rsp[1024];
    int got = 0;
    while (got < (int)sizeof(rsp) - 1) {
        int r = tuya_transporter_read(s_rt_tls, (uint8_t *)rsp + got, 1, 3000);
        if (r <= 0) {
            break;
        }
        got += r;
        if (got >= 4 && 0 == strcmp(rsp + got - 4, "\r\n\r\n")) {
            break;
        }
    }
    rsp[got] = '\0';
    if (NULL == strstr(rsp, "101")) {
        PR_ERR("[RT] upgrade failed: %.120s", rsp);
        __ws_close();
        return FALSE;
    }
    s_rt_ws_up = TRUE;
    s_rt_first_app = FALSE;   /* 重置首帧音频打点 */
    s_rt_last_ev[0] = '\0';   /* 清空上次会话残留事件，避免误判就绪 */

    /* session.update */
    const char *upd =
        "{\"type\":\"session.update\",\"session\":{"
        "\"modalities\":[\"text\",\"audio\"],\"voice\":\"longanqian\","
        "\"instructions\":\"你是简短口语化中文助手，回答60字以内，直接回答不要寒暄\","
        "\"input_audio_format\":\"pcm\",\"output_audio_format\":\"pcm\","
        "\"input_audio_transcription\":{\"model\":\"qwen3-asr-flash-realtime\"},"
        "\"turn_detection\":{\"type\":\"server_vad\",\"threshold\":0.3,\"silence_duration_ms\":800}}}";
    __ws_send_frame(0x01, (const uint8_t *)upd, strlen(upd));
    PR_NOTICE("[RT] session ready");
    return TRUE;
}

/* 轮询收帧：文本帧交给 JSON 处理，ping 回 pong */
static void __ws_poll(void)
{
    if (!s_rt_ws_up || NULL == s_rt_tls) {
        return;
    }
    uint8_t hdr[2];
    if (__ws_read_exact(hdr, 2, 20) < 0) {
        return;
    }
    int opcode = hdr[0] & 0x0F;
    uint32_t len = hdr[1] & 0x7F;
    int masked = hdr[1] & 0x80;

    if (len == 126) {
        uint8_t ext[2];
        if (__ws_read_exact(ext, 2, 20) < 0) goto fail;
        len = ((uint32_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (__ws_read_exact(ext, 8, 20) < 0) goto fail;
        len = 0;
        for (int i = 4; i < 8; i++) len = (len << 8) | ext[i];
    }
    uint8_t maskk[4] = {0};
    if (masked && __ws_read_exact(maskk, 4, 20) < 0) goto fail;

    if (len > RT_ACCUM_SZ) {
        /* 大帧读掉并丢弃（不追加 NUL），保住会话而非断连 */
        PR_ERR("[RT] frame too large, discard: %u", len);
        uint8_t sink[256];
        uint32_t left = len;
        while (left > 0) {
            uint32_t want = (left > sizeof(sink)) ? sizeof(sink) : left;
            if (__ws_read_exact(sink, want, 2000) < 0) { goto fail; }
            left -= want;
        }
        return;
    }
    if (len > 0) {
        if (__ws_read_exact((uint8_t *)s_rt_accum, len, 2000) < 0) goto fail;
        if (masked) {
            for (uint32_t i = 0; i < len; i++) s_rt_accum[i] ^= maskk[i & 3];
        }
    }
    s_rt_accum[len] = '\0';

    if (0x01 == opcode && len > 0) {
        __rt_handle_msg(s_rt_accum);
    } else if (0x09 == opcode) {
        __ws_send_frame(0x0A, (const uint8_t *)s_rt_accum, len);
    } else if (0x08 == opcode) {
        /* close 帧载荷前 2 字节为状态码，其后为原因文本 */
        if (len >= 2) {
            PR_ERR("[RT] server closed code=%u reason=%.80s",
                   ((uint32_t)s_rt_accum[0] << 8) | s_rt_accum[1], s_rt_accum + 2);
        } else {
            PR_ERR("[RT] server closed (no status)");
        }
        __ws_close();
    }
    return;

fail:
    __ws_close();
}

static void __rt_send_pcm(const uint8_t *pcm, uint32_t len)
{
    if (!s_rt_ws_up || NULL == s_rt_json) {
        return;
    }
    /* 头部含音频值的起始引号，base64 数据必须紧跟其后。
     * 旧版偏移 44 覆盖了引号，导致 JSON 畸形、服务端 COMMON_ERROR */
    const char *hdr = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    int n = strlen(hdr);
    /* 关键修复：头部必须真正拷入缓冲区。此前 hdr 只用来取长度，
     * s_rt_json 开头 45 字节是上一次会话残留的 base64 垃圾，
     * 导致每帧 JSON 都缺头 → 服务端 grpc_json_transcode_failure 断连 */
    memcpy(s_rt_json, hdr, n);
    char *b64 = s_rt_json + n;
    tuya_base64_encode(pcm, b64, (int)len);
    /* mbedtls_base64_encode 输出不带 NUL 结尾，且缓冲区残留旧内容会使
     * strlen 多扫（历史 bug：JSON 尾部多出 "\"}"，服务端
     * grpc_json_transcode_failure 断连）。必须按公式计算长度 */
    n += (int)(((len + 2) / 3) * 4);
    s_rt_json[n++] = '"';
    s_rt_json[n++] = '}';
    s_rt_json[n] = '\0';
    if (s_rt_first_app < 3) {
        s_rt_first_app++;
        PR_NOTICE("[RT] append#%d len=%u head=%.48s tail=%.40s",
                  s_rt_first_app, (unsigned)n, s_rt_json, s_rt_json + n - 40);
    }
    __ws_send_frame(0x01, (const uint8_t *)s_rt_json, n);
}

/* 实时线程：下行播放 + 上行送音 + 会话生命周期管理 */
static void __rt_thread(void *arg)
{
    static uint8_t play[RT_CHUNK];
    static uint8_t up[RT_CHUNK];
    (void)arg;

    while (1) {
        /* 下行播放 */
        while ((s_rt_wr - s_rt_rd) >= RT_CHUNK) {
            for (int i = 0; i < RT_CHUNK; i++) {
                play[i] = s_rt_ring[(s_rt_rd + i) % RT_PLAY_RING_SZ];
            }
            s_rt_rd += RT_CHUNK;
            if (sg_app.audio) {
                /* 软件音量衰减（与下行 TTS 同一音量） */
                for (int i = 0; i < RT_CHUNK / 2; i++) {
                    ((int16_t *)play)[i] = eink_xz_vol_scale(((int16_t *)play)[i]);
                }
                tdl_audio_play(sg_app.audio, play, RT_CHUNK);
            }
        }

        __ws_poll();

        /* 上行 */
        if (RT_STREAM == s_rt_state) {
            if (s_rt_wr != s_rt_rd) {
                /* 喇叭在响：暂停上行并丢弃此刻拾音（无回声消除，半双工） */
                s_rt_sent = sg_app.rec_len;
                s_rt_guard_ms = RT_GUARD_MS;
                s_rt_idle_ms = 0;
            } else if (s_rt_guard_ms > 0) {
                /* 播放刚结束，短暂静默保护 */
                s_rt_guard_ms -= 20;
                s_rt_sent = sg_app.rec_len;
            } else {
                int cap = 4;
                while ((sg_app.rec_len - s_rt_sent) >= RT_CHUNK && cap-- > 0) {
                    uint32_t pos = s_rt_sent % REC_BUF_SIZE;
                    uint32_t first = REC_BUF_SIZE - pos;
                    if (first > RT_CHUNK) { first = RT_CHUNK; }
                    memcpy(up, sg_app.pcm_buf + pos, first);
                    if (RT_CHUNK > first) {
                        memcpy(up + first, sg_app.pcm_buf, RT_CHUNK - first);
                    }
                    __rt_send_pcm(up, RT_CHUNK);
                    s_rt_sent += RT_CHUNK;
                    /* 只有真正有人声才重置空闲计时，静音流不算活跃 */
                    int pk = 0;
                    for (int i = 0; i < RT_CHUNK / 2; i++) {
                        int a = ((int16_t *)up)[i];
                        if (a < 0) { a = -a; }
                        if (a > pk) { pk = a; }
                    }
                    if (pk > RT_SPEAK_PEAK) {
                        s_rt_idle_ms = 0;
                    }
                }
            }
        } else if (RT_TAIL == s_rt_state) {
            if (s_rt_tail < RT_TAIL_BYTES) {
                memset(play, 0, RT_CHUNK);
                __rt_send_pcm(play, RT_CHUNK);
                s_rt_tail += RT_CHUNK;
            } else {
                s_rt_state = RT_WAIT;
                PR_NOTICE("[RT] 等待 AI 回复");
            }
        }

        /* 连续会话：按键请求结束 / 空闲超时 */
        if (s_rt_live) {
            s_rt_idle_ms += 20;   /* 每 20ms 循环累加 */
            if (s_rt_end_req || s_rt_idle_ms > RT_SESSION_IDLE_MS) {
                PR_NOTICE("[RT] %s", s_rt_end_req ? "手动结束对话" : "长时间没有说话，对话结束");
                __uart0_out(s_rt_end_req ? "RSP:对话已结束\r\n" : "RSP:对话结束\r\n");
                s_rt_end_req = FALSE;
                s_rt_live = FALSE;
                sg_app.rec_active = FALSE;
                sg_app.state = APP_ST_IDLE;
                s_rt_state = RT_IDLE;
                __ws_close();
            }
        }

        tal_system_sleep(20);
    }
}

/* 开启连续实时对话会话：连上、等会话就绪、开始持续上行。
 * 非阻塞，会话生命周期由 rt_thread 管理（超时/出错/手动结束）。 */
static BOOL_T __rt_live_start(void)
{
    if (s_rt_live) {
        return TRUE;
    }
    /* 先占位为忙，避免待机线程与命令线程同时进入 */
    sg_app.state = APP_ST_PROCESSING;
    tal_semaphore_wait(s_rt_mutex, SEM_WAIT_FOREVER);

    BOOL_T ok = FALSE;
    do {
        if (s_rt_live || RT_IDLE != s_rt_state) {
            break;
        }
        BOOL_T conn_ok = FALSE;
        for (int attempt = 0; attempt < 2 && !conn_ok; attempt++) {
            if (attempt > 0) {
                PR_ERR("[RT] 连接失败，重试第 %d 次", attempt);
                tal_system_sleep(300);
            }
            conn_ok = __rt_connect();
        }
        if (!conn_ok) {
            break;
        }
        /* 必须等 session.created/updated，否则服务端会直接 COMMON_ERROR */
        for (int i = 0; i < 100; i++) {
            if (strstr(s_rt_last_ev, "session.updated") || strstr(s_rt_last_ev, "session.created")) {
                break;
            }
            __ws_poll();
            tal_system_sleep(30);
        }
        if (!s_rt_last_ev[0]) {
            PR_ERR("[RT] 会话未就绪，放弃");
            __ws_close();
            break;
        }
        PR_NOTICE("[RT] 会话就绪(%s)，进入实时对话", s_rt_last_ev);
        sg_app.rec_len = 0;
        s_rt_sent  = 0;
        s_rt_tail  = 0;
        s_rt_asr[0] = '\0';
        s_rt_ans[0] = '\0';
        s_rt_idle_ms  = 0;
        s_rt_guard_ms = 0;
        s_rt_end_req  = FALSE;
        s_rt_state = RT_STREAM;
        sg_app.rec_active = TRUE;
        sg_app.state = APP_ST_RECORDING;
        s_rt_live = TRUE;
        ok = TRUE;
    } while (0);

    tal_semaphore_post(s_rt_mutex);
    if (!ok) {
        sg_app.state = APP_ST_IDLE;
    }
    return ok;
}

/* ========================= 待机唤醒 ========================= */
/* 归一化：只保留 ASCII 字母数字与 CJK 汉字（0x4E00-0x9FA5），
 * 剔除中文标点（，。！？等码点均 < 0x4E00，若不过滤会污染尾部同音比对） */
static void __wake_norm(const char *in, char *out, int n)
{
    int j = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && j < n - 3) {
        if (*p < 0x80) {
            char c = (char)*p;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z')) {
                out[j++] = c;
            }
            p++;
        } else {
            int len = (*p >= 0xF0) ? 4 : ((*p >= 0xE0) ? 3 : 2);
            uint32_t cp = 0;
            if (4 == len) {
                cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                     ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
            } else if (3 == len) {
                cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                     (uint32_t)(p[2] & 0x3F);
            } else {
                cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
            }
            if (cp >= 0x4E00 && cp <= 0x9FA5) {
                out[j++] = (char)p[0];
                out[j++] = (char)p[1];
                out[j++] = (char)p[2];
            }
            p += len;
        }
    }
    out[j] = '\0';
}

/* 单汉字同音比较：UTF-8 三字节。同字或同音组内视为相同。
 * ASR 常把唤醒词尾字写成同音字（"小五"→"小武/小舞/小伍"），按字面比对会漏唤醒 */
static BOOL_T __wake_ch_sim(const char *x, const char *y)
{
    static const char *homo[] = {
        "五武舞伍吴午雾无物务乌屋",
        "好号郝浩豪毫",
        "你尼拟泥匿",
        "小晓筱孝效笑",
        "智志至致置治芝之支知只",
    };
    if (0 == memcmp(x, y, 3)) {
        return TRUE;
    }
    for (int g = 0; g < (int)(sizeof(homo) / sizeof(homo[0])); g++) {
        BOOL_T fx = FALSE, fy = FALSE;
        for (const char *p = homo[g]; *p; p += 3) {
            if (0 == memcmp(p, x, 3)) { fx = TRUE; }
            if (0 == memcmp(p, y, 3)) { fy = TRUE; }
        }
        if (fx && fy) { return TRUE; }
    }
    return FALSE;
}

static BOOL_T __wake_match(const char *asr_text)
{
    char a[256];
    char w[64];
    __wake_norm(asr_text, a, sizeof(a));
    __wake_norm(s_wake_word, w, sizeof(w));
    if ('\0' == a[0] || '\0' == w[0]) {
        return FALSE;
    }
    if (strstr(a, w)) {
        return TRUE;
    }
    int wl = (int)strlen(w);
    int al = (int)strlen(a);
    /* 容错一：命中唤醒词开头两字（如"你好"），且文本长度接近唤醒词（防整句误触发） */
    if (wl >= 6) {
        char head[8];
        memcpy(head, w, 6);
        head[6] = '\0';
        if (strstr(a, head) && al >= wl - 6) {
            return TRUE;
        }
    }
    /* 容错二：结尾两字逐字同音比对（"小武/小舞"→"小五"也算） */
    if (wl >= 6 && al >= 6) {
        const char *atail = a + al - 6;
        const char *wtail = w + wl - 6;
        if (__wake_ch_sim(atail, wtail) && __wake_ch_sim(atail + 3, wtail + 3)) {
            return TRUE;
        }
    }
    return FALSE;
}

/* 峰值检测：安静时不发起 ASR 请求，省流量也省延迟 */
static int __wake_peak_scan(void)
{
    int peak = 0;
    int16_t *pcm = (int16_t *)sg_app.pcm_buf;
    uint32_t n = sg_app.rec_len / 2;
    for (uint32_t i = 0; i < n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) { peak = v; }
    }
    s_wake_peak = peak;
    return peak;
}

/* 整窗放大后送 ASR：弱信号下裁剪容易把词尾截掉，改为只做增益 */
static void __wake_prepare(void)
{
    int16_t *pcm = (int16_t *)sg_app.pcm_buf;
    uint32_t n = sg_app.rec_len / 2;
    if (0 == n) { return; }
    int g = (s_wake_peak > 6000) ? 1 : (s_wake_peak > 2000 ? 3 : WAKE_GAIN);
    for (uint32_t i = 0; i < n; i++) {
        int v = (int)pcm[i] * g;
        if (v > 32767) { v = 32767; }
        if (v < -32768) { v = -32768; }
        pcm[i] = (int16_t)v;
    }
    PR_NOTICE("[WAKE] 送识别 %dms gain=%d peak=%d", (int)(n * 1000 / REC_SAMPLE_RATE), g, s_wake_peak);
}

/* 唤醒后进入连续实时对话：边说边传，多轮交互，空闲或手动结束 */
static BOOL_T __rt_auto_call(void)
{
    if (eink_xz_enabled()) {
        eink_xz_arm();              /* 预连接就绪 → 立即开麦并补发缓冲音频 */
        /* 还在握手就等它完成（预连接通常已就绪；兜底最多等 4 秒） */
        for (int i = 0; i < 100 && !eink_xz_is_active(); i++) {
            if (!eink_xz_engaged()) { break; }
            tal_system_sleep(80);
        }
        if (!eink_xz_is_active()) {
            PR_ERR("[对话] 小智通道未就绪，重新发起连接");
            eink_xz_arm();
            for (int i = 0; i < 100 && !eink_xz_is_active(); i++) {
                if (!eink_xz_engaged()) { break; }
                tal_system_sleep(80);
            }
        }
        while (eink_xz_engaged()) {
            tal_system_sleep(100);
        }
        eink_chat_spk_mute(1500);   /* 会话刚结束，AI 语音余音期不做唤醒 */
        sg_app.state = APP_ST_IDLE;
        return TRUE;
    }
    if (!__rt_live_start()) {
        PR_ERR("[对话] 连接实时通道失败");
        return FALSE;
    }
    PR_NOTICE("[对话] 实时对话进行中，直接说话（再按 ENTER 或静默 15 秒结束）");
    while (s_rt_live) {
        tal_system_sleep(100);
    }
    /* 收尾由 rt_thread 完成（关闭 WS、状态复位） */
    sg_app.state = APP_ST_IDLE;
    PR_NOTICE("[对话] 结束，播放缓冲 已写=%u 已读=%u",
              (unsigned)s_rt_wr, (unsigned)s_rt_rd);
    return TRUE;
}

/* 采样窗口：默认最长 WAKE_WIN_MS，检测到「说过话 + 随后 500ms 静音」就提前收窗，
 * 少等 1~2 秒；首次越阈立刻发起小智预连接，把 OTA+TLS+WS 握手藏进 ASR 耗时里 */
static void __wake_capture(void)
{
    const int step = 100;
    uint32_t prev = 0;
    BOOL_T voiced = FALSE;
    int sil_ms = 0;
    BOOL_T pre = FALSE;

    s_wake_peak_strong = 0;
    sg_app.rec_len = 0;
    sg_app.rec_active = TRUE;
    for (int t = 0; t < WAKE_WIN_MS; t += step) {
        tal_system_sleep(step);

        /* 喇叭自声/测试音期间整窗作废：别把自己的声音当唤醒词去握手 */
        if (tal_system_get_millisecond() < s_spk_mute_until) {
            sg_app.rec_active = FALSE;
            sg_app.rec_len = 0;
            return;
        }

        /* 本步新增样本的峰值（窗口 3.2s、线性写入，pos 不会绕回） */
        {
            int16_t *pcm = (int16_t *)sg_app.pcm_buf;
            uint32_t n = sg_app.rec_len / 2;
            int pk = 0;
            for (uint32_t i = prev; i < n; i++) {
                int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > pk) { pk = v; }
            }
            prev = n;
            if (pk > s_wake_peak_strong) { s_wake_peak_strong = pk; }
            if (pk >= s_wake_thr) {
                voiced = TRUE;
                sil_ms = 0;
            } else if (voiced) {
                sil_ms += step;
            }
        }

        /* 听到明确的说话声（峰值 ≥4000，环境噪声通常 <2000）才投机握手，
         * 且要持续 300ms 以上，避免关门声、拍桌子这类瞬时噪声触发连接 */
        if (voiced && !pre && !s_mic_test && t >= 300 && s_wake_peak_strong >= 4000) {
            pre = eink_xz_preconnect();
        }
        if (voiced && sil_ms >= 500 && t >= 1200) {
            break;      /* 说完就收窗，不再死等满 3.2s */
        }
    }
    sg_app.rec_active = FALSE;
}

/* 待机循环：采样 → 能量门限 → ASR → 唤醒词匹配 → 自动对话 */
static void __wake_thread(void *arg)
{
    char q[256];
    (void)arg;

    while (1) {
        if (!s_wake_on) {
            tal_system_sleep(200);
            continue;
        }
        if (eink_xz_engaged() || RT_IDLE != s_rt_state || APP_ST_IDLE != sg_app.state) {
            tal_system_sleep(300);
            continue;
        }

        /* 待机常连：空闲时把与 xiaozhi.me 的会话提前握好（挂起不开麦），
         * 唤醒命中后直接开麦，省掉每次 5 秒左右的 TLS+WS 握手 */
        if (tal_system_get_millisecond() > s_spk_mute_until) {
            (void)eink_xz_preconnect();
        }
        if (tal_system_get_millisecond() < s_spk_mute_until) {
            sg_app.rec_active = FALSE;         /* 喇叭自声隔离窗：不采样不识别 */
            tal_system_sleep(200);
            continue;
        }

        __wake_capture();
        /* 采样窗口可能横跨静默窗武装点：送识别前复查，横跨则整窗丢弃 */
        if (tal_system_get_millisecond() < s_spk_mute_until) {
            continue;
        }

        if (s_mic_test) {
            tal_system_sleep(100);
            continue;
        }
        if (sg_app.rec_len < (uint32_t)(REC_SAMPLE_RATE * 2)) {
            continue;
        }
        if (__wake_peak_scan() < s_wake_thr) {
            continue;                          /* 没人说话，不请求 */
        }

        __wake_prepare();
        q[0] = '\0';
        OPERATE_RET rt = __do_asr(q, sizeof(q));
        if (OPRT_OK != rt || '\0' == q[0]) {
            PR_ERR("[待机] 识别失败 %d", rt);
            eink_xz_abort();
            tal_system_sleep(1500);
            continue;
        }
        PR_NOTICE("[待机] 听到: %s (peak=%d)", q, s_wake_peak);
        snprintf(s_wake_last, sizeof(s_wake_last), "%s", q);
        {
            char buf[300];
            snprintf(buf, sizeof(buf), "RSP:听到 %s\r\n", q);
            __uart0_out(buf);
        }

        if (!__wake_match(q)) {
            eink_xz_abort();          /* 不是唤醒词：关掉投机建立的预连接 */
            continue;
        }

        s_wake_cnt++;
        PR_NOTICE("[WAKE] 命中唤醒词: %s", q);
        __uart0_out("RSP:唤醒 命中唤醒词，开始对话\r\n");
        __rt_auto_call();
        sg_app.rec_len = 0;
    }
}

/* 注意：日志口跑的是 Beken bk_cli，必须用 cli_register_commands 注册；
 * "net" 已被厂商占用，状态查询命令改名为 mynet */
static const struct cli_command s_cli_cmds[] = {
    { .name = "wifi",  .help = "wifi <ssid> <pwd>: connect wifi",  .function = __cmd_wifi },
    { .name = "chat",  .help = "chat <text>: ask the LLM",         .function = __cmd_chat },
    { .name = "mynet", .help = "mynet: show network status",       .function = __cmd_net },
};

/***********************************************************
**************************main*******************************
***********************************************************/
/* 音频服务重建线程：延迟 3 秒等首次初始化稳定后执行销毁重建（前置声明） */
static void __audio_reinit_thread(void *arg);

static void __user_main(void)
{
    OPERATE_RET rt;

    memset(&sg_app, 0, sizeof(sg_app));
    sg_app.state = APP_ST_IDLE;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();

    /* PSRAM 大缓冲 */
    sg_app.pcm_buf  = (uint8_t *)tkl_system_psram_malloc(REC_BUF_SIZE);
    sg_app.b64_buf  = (char *)tkl_system_psram_malloc(B64_BUF_SIZE);
    sg_app.json_buf = (char *)tkl_system_psram_malloc(JSON_BUF_SIZE);
    sg_app.resp_buf = (char *)tkl_system_psram_malloc(RESP_BUF_SIZE);
    if (!sg_app.pcm_buf || !sg_app.b64_buf || !sg_app.json_buf || !sg_app.resp_buf) {
        PR_ERR("PSRAM 申请失败");
        return;
    }

    tal_semaphore_create_init(&sg_app.work_sem, 0, 1);
    tal_semaphore_create_init(&s_rt_mutex, 1, 1);   /* 会话启动互斥 */

    /* 网络 */
    tuya_tls_init();
#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif
    netmgr_init(NETCONN_WIFI);
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    {
        netconn_wifi_info_t wi = {0};
        strncpy(wi.ssid, WIFI_SSID_DEFAULT, sizeof(wi.ssid) - 1);
        strncpy(wi.pswd, WIFI_PWD_DEFAULT, sizeof(wi.pswd) - 1);
        netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &wi);
        PR_NOTICE("上电自动配网：%s", wi.ssid);
    }
#endif

    /* 板级硬件 */
    tkl_ai_disable_vendor_vad();   /* 必须在音频 init 前调用，保证麦克风连续出帧 */
    rt = board_register_hardware();
    if (OPRT_OK != rt) {
        PR_ERR("board register failed: %d", rt);
        return;
    }

    /* 音频 */
    rt = tdl_audio_find("audio_codec", &sg_app.audio);
    if (OPRT_OK != rt) {
        PR_ERR("未找到音频设备：%d", rt);
    } else {
        rt = tdl_audio_open(sg_app.audio, __mic_cb);
        PR_NOTICE("音频打开结果：%d", rt);

        /* 功放使能强制接管（该板 PA=GPIO39 高电平使能，spkab A/B 对照实测）：
         * tkl 层的 PA 管理不可靠——tkl_ao_set_vol 只控 DAC 增益不驱动 PA 脚。
         * 应用层显式初始化并常拉高，确保任何时刻播放都有输出。 */
        {
            TUYA_GPIO_BASE_CFG_T pa_cfg = {
                .direct = TUYA_GPIO_OUTPUT,
                .mode   = TUYA_GPIO_PULLUP,
                .level  = TUYA_GPIO_LEVEL_HIGH,
            };
            tkl_gpio_init(TUYA_GPIO_NUM_39, &pa_cfg);
            tkl_gpio_write(TUYA_GPIO_NUM_39, TUYA_GPIO_LEVEL_HIGH);
            PR_NOTICE("功放 GPIO39 已强制使能（常拉高）");

            /* 恢复用户音量（vol 命令保存的 0-100，未保存过则用默认 60） */
            {
                uint8_t *vbuf = NULL;
                size_t vlen = 0;
                if (OPRT_OK == tal_kv_get("spk_vol", &vbuf, &vlen) &&
                    NULL != vbuf && vlen >= 1 && vbuf[0] <= 100) {
                    eink_xz_set_speaker_vol(vbuf[0]);
                    PR_NOTICE("恢复音量设置：%d", vbuf[0]);
                }
                if (NULL != vbuf) { tal_kv_free(vbuf); }
            }
        }
    }

    /* 显示（墨水屏未接，跳过初始化，避免驱动卡死） */
    PR_NOTICE("墨水屏未接入，跳过显示");

    __draw_screen("启动中...", NULL, NULL);
    PR_NOTICE("步骤：状态输出就绪");

    /* 按键 */
    __buttons_init();
    PR_NOTICE("步骤：按键就绪");

    /* 工作线程 */
    THREAD_CFG_T thrd = {0};
    thrd.stackDepth = 1024 * 8;
    thrd.priority   = THREAD_PRIO_1;
    thrd.thrdname   = "eink_worker";
    tal_thread_create_and_start(&sg_app.worker, NULL, NULL, __worker_thread, NULL, &thrd);
    PR_NOTICE("步骤：问答线程就绪");

    /* 实时通话资源 */
    s_rt_accum = (char *)tkl_system_psram_malloc(RT_ACCUM_SZ);
    s_rt_json  = (char *)tkl_system_psram_malloc(8 * 1024);
    s_rt_pcm   = (char *)tkl_system_psram_malloc(16 * 1024);
    s_rt_ring  = (uint8_t *)tkl_system_psram_malloc(RT_PLAY_RING_SZ);
    if (s_rt_accum && s_rt_json && s_rt_pcm && s_rt_ring) {
        THREAD_CFG_T thrd_rt = {0};
        thrd_rt.stackDepth = 1024 * 8;
        thrd_rt.priority   = THREAD_PRIO_2;
        thrd_rt.thrdname   = "rt_call";
        tal_thread_create_and_start(&sg_app.rt_thread, NULL, NULL, __rt_thread, NULL, &thrd_rt);
        PR_NOTICE("步骤：实时通道线程就绪");
    } else {
        PR_ERR("实时通道内存申请失败");
    }

    /* 待机唤醒线程 */
    {
        THREAD_CFG_T thrd_wake = {0};
        thrd_wake.stackDepth = 1024 * 10;
        thrd_wake.priority   = THREAD_PRIO_3;
        thrd_wake.thrdname   = "wake_listen";
        if (OPRT_OK != tal_thread_create_and_start(&sg_app.wake_thread, NULL, NULL,
                                                   __wake_thread, NULL, &thrd_wake)) {
            PR_ERR("待机线程创建失败");
        } else {
            PR_NOTICE("步骤：待机监听线程就绪");
        }
    }

    /* UART0（烧录口）命令通道 */
    TAL_UART_CFG_T ucfg = {0};
    ucfg.base_cfg.baudrate = 115200;
    ucfg.base_cfg.parity   = TUYA_UART_PARITY_TYPE_NONE;
    ucfg.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    ucfg.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    ucfg.rx_buffer_size    = 1024;
    ucfg.open_mode         = 0;   /* 非阻塞读 */
    if (OPRT_OK == tal_uart_init(TUYA_UART_NUM_0, &ucfg)) {
        THREAD_CFG_T thrd2 = {0};
        thrd2.stackDepth = 1024 * 6;
        thrd2.priority   = THREAD_PRIO_2;
        thrd2.thrdname   = "cmd_uart";
        tal_thread_create_and_start(&sg_app.cmd_uart, NULL, NULL, __cmd_uart_thread, NULL, &thrd2);
        __uart0_out("RSP:固件就绪 命令通道可用\r\n");
        PR_NOTICE("步骤：串口命令通道就绪");
    } else {
        PR_ERR("串口0 初始化失败");
    }

    /* CLI（Beken bk_cli） */
    int cli_rt = cli_register_commands(s_cli_cmds, sizeof(s_cli_cmds) / sizeof(s_cli_cmds[0]));
    PR_NOTICE("步骤：CLI 注册=%d", cli_rt);

    /* 小智协议通道（OTA -> WSS -> OPUS，角色/记忆由 xiaozhi.me 管理端） */
    eink_xz_init();

    /* 音频服务销毁重建（自愈）：开机自建的 bk voice 服务存在启动竞态，
     * 喇叭通道随机无声。延迟 3 秒执行「销毁 → 重建」（与 vt 测试命令同路径，
     * 实测每次都能恢复出声），由后台线程执行避免阻塞启动 */
    {
        THREAD_CFG_T thrd_ra = {0};
        thrd_ra.stackDepth = 1024 * 4;
        thrd_ra.priority   = THREAD_PRIO_1;
        thrd_ra.thrdname   = "audio_reinit";
        tal_thread_create_and_start(&sg_app.audio_ra_thread, NULL, NULL,
                                    __audio_reinit_thread, NULL, &thrd_ra);
    }
    __draw_screen("按 ENTER 键说话 | wifi ssid pwd 配网", NULL, NULL);
    PR_NOTICE("eink_chat 启动完成");
}

/* 开机问候语播放：音频服务重建成功（链路就绪）后本地内嵌语音上线提示。
 * 内容："叮咚～我回来啦！小舞已上线，随时可以叫我哦。"（edge-tts 内嵌 PCM） */
static void __play_boot_greeting(void)
{
    TDL_AUDIO_HANDLE_T audio = eink_chat_audio_handle();
    if (NULL == audio) {
        return;
    }
    /* 播放全程+1.5s 余音期内静默唤醒（问候语含唤醒词「小舞」） */
    eink_chat_spk_mute(BOOT_GREETING_SAMPLES * 1000 / REC_SAMPLE_RATE + 1500);
    static int16_t chunk[640];
    const int vol = eink_xz_get_speaker_vol();
    for (int off = 0; off < BOOT_GREETING_SAMPLES; off += 640) {
        int n = BOOT_GREETING_SAMPLES - off;
        if (n > 640) { n = 640; }
        for (int i = 0; i < n; i++) {
            chunk[i] = eink_xz_vol_scale(s_boot_greeting[off + i]);
        }
        tdl_audio_play(audio, (uint8_t *)chunk, n * 2);
    }
    PR_NOTICE("[开机] 上线问候语已播放（音量=%d）", vol);
}

/* 音频服务重建线程：延迟 3 秒等首次初始化稳定后执行销毁重建 */
static void __audio_reinit_thread(void *arg)
{
    (void)arg;
    /* 立即武装静默窗：覆盖 3s 延迟 + 问候语 5.1s + 余音 1.5s，
     * 防止开机采样迭代横跨重建爆破音与问候语（含唤醒词）造成误识别 */
    eink_chat_spk_mute(3000 + BOOT_GREETING_SAMPLES * 1000 / REC_SAMPLE_RATE + 1500);
    tal_system_sleep(3000);
    extern OPERATE_RET tkl_audio_service_reinit(void);
    OPERATE_RET rt = tkl_audio_service_reinit();
    PR_NOTICE("音频服务重建结果：%d", rt);
    if (OPRT_OK == rt) {
        __play_boot_greeting();
    }
}

#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    __user_main();
    while (1) {
        tal_system_sleep(500);
    }
}
#else

static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    __user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = 1024 * 8;
    thrd_param.priority   = THREAD_PRIO_1;
    thrd_param.thrdname   = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
