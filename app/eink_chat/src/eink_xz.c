/**
 * @file eink_xz.c
 * @brief eink_chat 小智(xiaozhi)协议客户端
 *
 * 流程：POST OTA 取 websocket.url/token（未绑定时返回激活码，轮询 activate），
 * WSS 连接 -> hello 握手 -> listen(auto) -> 上行 OPUS(16k/60ms)；
 * 下行 tts 期间收 OPUS 二进制帧解码播放，tts stop 后恢复 listen。
 * 版本：eink_chat 1.7
 */
#include "eink_xz.h"

#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_memory.h"
#include "tkl_output.h"
#include "tkl_wifi.h"
#include "tuya_transporter.h"
#include "tuya_tls.h"
#include "cJSON.h"
#include "tdl_audio_manage.h"

extern const char g_tenclass_ca_pem[];
extern const unsigned int g_tenclass_ca_pem_len;

#include <opus.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/***********************************************************
************************macro define************************
***********************************************************/
#define XZ_OTA_URL_DEF  "https://api.tenclass.net/xiaozhi/ota/"
#define XZ_APP_VER      "1.7"
#define XZ_BOARD_NAME   "TUYA_T5AI_EINK_NFC"

#define XZ_SRV_SAMPLE   16000       /* 上行采样率 */
#define XZ_FRAME_MS     60          /* OPUS 帧长 */
#define XZ_FRAME_SM     (XZ_SRV_SAMPLE * XZ_FRAME_MS / 1000)     /* 960 采样 */
#define XZ_FRAME_BYTES  (XZ_FRAME_SM * 2)                        /* 1920 字节 */

#define XZ_MIC_RING_SZ  (256 * 1024)   /* 上行 PCM 环形缓冲（PSRAM） */
#define XZ_PLAY_RING_SZ (96 * 1024)    /* 下行 16k PCM 环形缓冲（PSRAM） */
#define XZ_ACCUM_SZ     (32 * 1024)    /* WS 文本帧拼帧缓冲 */
#define XZ_DEC_BUF_SZ   (16 * 1024)    /* OPUS 解码输出缓冲 */

#define XZ_IDLE_MS      (60 * 1000)    /* 60 秒无交互自动结束 */
#define XZ_ECHO_GUARD_MS  1000         /* 播放后拾音丢弃窗（余音/间隙期） */
#define XZ_UP_CHUNK     4              /* 每轮最多上行帧数 */

#define XZ_PRE_TIMEOUT_MS  (15 * 60 * 1000) /* 常驻待机会话：15 分钟没人用它才关（握手 5s 太贵，别每次唤醒都付） */
#define XZ_KEEPALIVE_MS    30000        /* 待机期间发 WS ping，尽量不让服务器踢掉空闲连接 */
#define XZ_PRE_COOLDOWN_MS 2000   /* 服务器会踢掉空闲待机会话：2 秒内就热回来，缩小唤醒慢的窗口 */
#define XZ_PRE_KEEP_MS     2500        /* 预连接期间只留最近 2.5 秒拾音 */        /* 两次预连接最小间隔，避免环境噪声反复触发握手 */
#define XZ_PLAY_FRAME      1280        /* 40ms @16k PCM16 */
#define XZ_BOOST_MS        3000        /* 会话开局增益补偿时长（功放起振期补音量） */

/* 设备状态 */
#define XZ_ST_IDLE      0
#define XZ_ST_LISTEN    1
#define XZ_ST_SPEAK     2
#define XZ_ST_PAUSED    3       /* 预连接：已握手，等唤醒确认才开麦 */

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    volatile BOOL_T  enabled;       /* 小智模式开关（唤醒路由） */
    volatile int     state;         /* XZ_ST_xxx */
    volatile BOOL_T  ws_up;
    volatile BOOL_T  end_req;
    volatile BOOL_T  active;        /* 会话进行中（mic 分流） */
    volatile BOOL_T  busy;          /* 连接中（OTA+握手，TLS 栈需求大） */
    volatile BOOL_T  start_req;     /* 请求开启会话（由 xz_call 线程执行） */
    volatile BOOL_T  pre_req;       /* 预连接请求：握手完成后挂起，不开麦 */
    volatile BOOL_T  paused;        /* 预连接已就绪、暂不上行 */
    volatile BOOL_T  confirm;       /* 握手途中收到唤醒确认：直接开麦，别挂起 */
    uint32_t keepalive_ms;          /* 待机会话上次 ping 时刻 */
    volatile uint32_t pre_ms;       /* 预连接时间戳（超时与冷却判断） */
    volatile BOOL_T  preroll;       /* 本会话首帧播放前先灌静音，等 DAC/功放进入稳态 */
    uint32_t play_start_ms;         /* 本会话首次出声时刻（增益补偿基准） */
    int      boost;                 /* 开局增益补偿百分比（0 关闭） */

    char     ota_url[160];
    char     ws_url[192];
    char     ws_token[160];
    char     sid[80];               /* session_id */
    char     mac_str[24];
    char     uuid[40];

    /* WS 通道 */
    tuya_transporter_t tcp;
    tuya_transporter_t tls;

    /* 上行 */
    uint8_t *mic_ring;              /* PSRAM */
    volatile uint32_t mic_wr, mic_rd;
    OpusEncoder *enc;               /* PSRAM */
    uint8_t  enc_out[1500];

    /* 下行 */
    int      srv_rate;              /* 服务器下行采样率 16000/24000 */
    OpusDecoder *dec;               /* PSRAM */
    uint8_t *dec_pcm;               /* PSRAM */
    uint8_t *play_ring;             /* PSRAM 存 16k PCM */
    volatile uint32_t play_wr, play_rd;

    char    *accum;                 /* PSRAM 文本帧拼帧 */
    uint32_t accum_len;

    uint32_t idle_ms;

    /* 回声守卫：喇叭停止播放后的余音期（混响/PA 衰减）内丢弃拾音，
     * 防止 AI 自己的话被上行当作用户语音（自闻自答循环） */
    uint32_t echo_guard_ms;

    /* 最近一次实际调用 tdl_audio_play 的时刻(ms)：下行 ring 排空的间隙
     * （ring_wait=0）并不代表喇叭静音，此时间戳才是可靠的回声守卫依据 */
    uint32_t last_play_ms;
} XZ_CTX_T;

static XZ_CTX_T s_xz;

extern TDL_AUDIO_HANDLE_T eink_chat_audio_handle(void);

/***********************************************************
************************util define*************************
***********************************************************/
extern void __uart_reply(const char *msg);   /* eink_chat.c 提供 */

static int __xz_read_exact(uint8_t *buf, uint32_t n, int timeout_ms)
{
    uint32_t got = 0;
    while (got < n) {
        int r = tuya_transporter_read(s_xz.tls, buf + got, n - got, timeout_ms);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    return 0;
}

/* URL 解析：scheme://host[:port]/path（支持 wss/https，端口默认 443） */
static BOOL_T __xz_parse_url(const char *url, char *host, int hostsz,
                             char *path, int pathsz, int *port)
{
    const char *p = strstr(url, "://");
    if (NULL == p) {
        return FALSE;
    }
    p += 3;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        int hl = (int)(colon - p);
        if (hl >= hostsz) { return FALSE; }
        memcpy(host, p, hl); host[hl] = '\0';
        *port = atoi(colon + 1);
    } else {
        int hl = slash ? (int)(slash - p) : (int)strlen(p);
        if (hl >= hostsz) { return FALSE; }
        memcpy(host, p, hl); host[hl] = '\0';
        *port = 443;
    }
    if (slash) {
        strncpy(path, slash, pathsz - 1); path[pathsz - 1] = '\0';
    } else {
        snprintf(path, pathsz, "/");
    }
    return TRUE;
}

/* MAC 与 UUID（由 MAC 派生 UUIDv4，重启稳定） */
static void __xz_load_identity(void)
{
    NW_MAC_S mac = {0};
    if (OPRT_OK == tkl_wifi_get_mac((const WF_IF_E)0, &mac) &&
        (mac.mac[0] | mac.mac[1] | mac.mac[2] | mac.mac[3] | mac.mac[4] | mac.mac[5])) {
        snprintf(s_xz.mac_str, sizeof(s_xz.mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac.mac[0], mac.mac[1], mac.mac[2], mac.mac[3], mac.mac[4], mac.mac[5]);
        uint8_t b[16];
        for (int i = 0; i < 16; i++) {
            b[i] = (uint8_t)(mac.mac[i % 6] ^ (0xA5 * (i + 1)));
        }
        b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);   /* version 4 */
        b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);   /* variant 10xx */
        snprintf(s_xz.uuid, sizeof(s_xz.uuid),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    } else {
        snprintf(s_xz.mac_str, sizeof(s_xz.mac_str), "00:00:00:00:00:00");
        snprintf(s_xz.uuid, sizeof(s_xz.uuid), "00000000-0000-4000-8000-000000000000");
    }
    PR_NOTICE("[XZ] Device-Id=%s Client-Id=%s", s_xz.mac_str, s_xz.uuid);
}

/***********************************************************
**************************HTTP OTA***************************
***********************************************************/
static OPERATE_RET __xz_http_post(const char *url, const char *headers,
                                  const char *body, char *resp, int resp_len,
                                  int *status_code)
{
    tuya_transporter_t tcp = NULL, tls = NULL;
    static char req[1024];
    static char raw[4096];
    OPERATE_RET rt = OPRT_COM_ERROR;
    char host[96] = {0}, path[128] = {0};
    int port = 443, total = 0, n = 0;

    if (!__xz_parse_url(url, host, sizeof(host), path, sizeof(path), &port)) {
        PR_ERR("[XZ] bad url: %s", url);
        return OPRT_COM_ERROR;
    }

    tcp = tuya_transporter_create(TRANSPORT_TYPE_TCP, NULL);
    if (NULL == tcp) { return OPRT_COM_ERROR; }
    tls = tuya_transporter_create(TRANSPORT_TYPE_TLS, tcp);
    if (NULL == tls) { tuya_transporter_destroy(tcp); return OPRT_COM_ERROR; }

    tuya_tls_config_t tcfg = {0};
    tcfg.mode         = TUYA_TLS_SERVER_CERT_MODE;
    tcfg.hostname     = host;
    tcfg.port         = (uint16_t)port;
    tcfg.timeout      = 15000;
    tcfg.verify       = true;
    tcfg.ca_cert      = (char *)g_tenclass_ca_pem;
    tcfg.ca_cert_size = (int)g_tenclass_ca_pem_len;
    if (OPRT_OK != tuya_transporter_ctrl(tls, TUYA_TRANSPORTER_SET_TLS_CONFIG, &tcfg)) {
        goto __xz_http_exit;
    }
    if (OPRT_OK != tuya_transporter_connect(tls, host, port, 15000)) {
        PR_ERR("[XZ] ota tls connect failed");
        goto __xz_http_exit;
    }

    n = snprintf(req, sizeof(req),
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n"
                 "%s\r\n",
                 path, host, (unsigned)strlen(body), headers);
    if (n <= 0 || n >= (int)sizeof(req)) { goto __xz_http_exit; }
    if (n != tuya_transporter_write(tls, (uint8_t *)req, n, 8000)) {
        goto __xz_http_exit;
    }
    if (tuya_transporter_write(tls, (uint8_t *)body, (int)strlen(body), 8000) <= 0) {
        goto __xz_http_exit;
    }

    while (total < (int)sizeof(raw) - 1) {
        int rd = tuya_transporter_read(tls, (uint8_t *)raw + total,
                                       (int)(sizeof(raw) - 1 - total), 15000);
        if (rd <= 0) { break; }
        total += rd;
    }
    raw[total] = '\0';

    if (total > 12 && 0 == strncmp(raw, "HTTP/1.", 7)) {
        if (status_code) { *status_code = atoi(raw + 9); }
    } else {
        PR_ERR("[XZ] ota no http header: %.40s", raw);
        goto __xz_http_exit;
    }

    {
        char *bd = strstr(raw, "\r\n\r\n");
        if (NULL == bd) { goto __xz_http_exit; }
        bd += 4;
        snprintf(resp, resp_len, "%s", bd);
        rt = OPRT_OK;
    }

__xz_http_exit:
    if (tls) { tuya_transporter_close(tls); tuya_transporter_destroy(tls); }
    if (tcp) { tuya_transporter_destroy(tcp); }
    return rt;
}

static OPERATE_RET __xz_ota_query(char *resp, int resp_len, int *status)
{
    static char body[768];
    static char hdr[320];

    snprintf(hdr, sizeof(hdr),
             "Device-Id: %s\r\n"
             "Client-Id: %s\r\n"
             "User-Agent: " XZ_BOARD_NAME "/" XZ_APP_VER "\r\n"
             "Accept-Language: zh-CN\r\n"
             "Activation-Version: 1\r\n",
             s_xz.mac_str, s_xz.uuid);

    int n = snprintf(body, sizeof(body),
        "{\"version\":2,\"language\":\"zh-CN\","
        "\"mac_address\":\"%s\",\"uuid\":\"%s\","
        "\"chip_model_name\":\"bk7258\","
        "\"application\":{\"name\":\"eink_chat\",\"version\":\"" XZ_APP_VER "\"},"
        "\"board\":{\"type\":\"" XZ_BOARD_NAME "\",\"name\":\"" XZ_BOARD_NAME "\","
        "\"manufacturer\":\"tuya\",\"mac\":\"%s\"},"
        "\"protocol\":{\"features\":{\"mcp\":false}}}",
        s_xz.mac_str, s_xz.uuid, s_xz.mac_str);
    if (n <= 0 || n >= (int)sizeof(body)) {
        return OPRT_BUFFER_NOT_ENOUGH;
    }
    return __xz_http_post(s_xz.ota_url, hdr, body, resp, resp_len, status);
}

/* 激活：返回 OPRT_OK=已绑定；OPRT_COM_ERROR=失败；1=超时未绑定 */
static int __xz_activate(char *code, int codesz)
{
    static char resp[512];
    static char body[8] = "{}";
    char act_url[192];
    int url_len = (int)strlen(s_xz.ota_url);

    snprintf(act_url, sizeof(act_url), "%sactivate", s_xz.ota_url);
    if (url_len > 0 && s_xz.ota_url[url_len - 1] != '/') {
        snprintf(act_url, sizeof(act_url), "%s/activate", s_xz.ota_url);
    }

    /* 最多 100 秒，每 3 秒轮询一次绑定状态 */
    for (int i = 0; i < 34; i++) {
        int status = 0;
        resp[0] = '\0';
        if (OPRT_OK != __xz_http_post(act_url, "", body, resp, sizeof(resp), &status)) {
            tal_system_sleep(3000);
            continue;
        }
        if (200 == status) {
            PR_NOTICE("[XZ] 激活成功");
            return OPRT_OK;
        }
        if (202 == status) {
            PR_NOTICE("[XZ] 等待绑定...(%d)", i);
            tal_system_sleep(3000);
            continue;
        }
        PR_ERR("[XZ] activate http %d: %.120s", status, resp);
        return OPRT_COM_ERROR;
    }
    (void)code; (void)codesz;
    return 1;
}

/* OTA 查询 + （必要时）激活。成功后填充 ws_url/ws_token */
static BOOL_T __xz_ota_prepare(void)
{
    static char resp[4096];
    int status = 0;

    for (int round = 0; round < 2; round++) {
        resp[0] = '\0';
        /* OTA 查询失败重试：服务器对密集新建连接偶发限流/握手失败，失败即放弃会浪费一次唤醒 */
        int ota_retry = 0;
        while (OPRT_OK != __xz_ota_query(resp, sizeof(resp), &status)) {
            PR_ERR("[XZ] ota query failed (retry %d)", ota_retry + 1);
            if (++ota_retry >= 3) {
                PR_ERR("[XZ] ota query 重试耗尽");
                return FALSE;
            }
            tal_system_sleep(1500);
        }
        if (200 != status) {
            PR_ERR("[XZ] ota http %d: %.200s", status, resp);
            return FALSE;
        }

        cJSON *root = cJSON_Parse(resp);
        if (NULL == root) {
            PR_ERR("[XZ] ota json bad");
            return FALSE;
        }
        cJSON *ws = cJSON_GetObjectItem(root, "websocket");
        cJSON *act = cJSON_GetObjectItem(root, "activation");
        BOOL_T got_ws = FALSE;
        if (cJSON_IsObject(ws)) {
            cJSON *u = cJSON_GetObjectItem(ws, "url");
            cJSON *t = cJSON_GetObjectItem(ws, "token");
            if (cJSON_IsString(u) && u->valuestring[0]) {
                snprintf(s_xz.ws_url, sizeof(s_xz.ws_url), "%s", u->valuestring);
                s_xz.ws_token[0] = '\0';
                if (cJSON_IsString(t) && t->valuestring[0]) {
                    snprintf(s_xz.ws_token, sizeof(s_xz.ws_token), "%s", t->valuestring);
                }
                got_ws = TRUE;
            }
        }
        if (got_ws) {
            cJSON_Delete(root);
            PR_NOTICE("[XZ] OTA OK ws=%s", s_xz.ws_url);
            return TRUE;
        }
        /* 未绑定：拿激活码，展示并轮询 */
        if (cJSON_IsObject(act)) {
            cJSON *code = cJSON_GetObjectItem(act, "code");
            cJSON *msg  = cJSON_GetObjectItem(act, "message");
            if (cJSON_IsString(code)) {
                PR_NOTICE("[XZ] 设备未绑定 激活码=%s", code->valuestring);
                char tip[96];
                snprintf(tip, sizeof(tip), "RSP:请在 xiaozhi.me 控制台输入激活码 %s\r\n",
                         code->valuestring);
                __uart_reply(tip);
            }
            if (cJSON_IsString(msg) && msg->valuestring[0]) {
                PR_NOTICE("[XZ] %s", msg->valuestring);
            }
        }
        cJSON_Delete(root);

        int rc = __xz_activate(NULL, 0);
        if (OPRT_OK != rc) {
            PR_ERR("[XZ] 激活未完成 rc=%d", rc);
            return FALSE;
        }
        /* 绑定完成后重新查询配置（round<2 时自动再查一次） */
    }
    PR_ERR("[XZ] ota 配置获取失败");
    return FALSE;
}

/***********************************************************
**************************WebSocket**************************
***********************************************************/
static void __xz_ws_close(void)
{
    s_xz.ws_up = FALSE;
    if (s_xz.tls) {
        tuya_transporter_close(s_xz.tls);
        tuya_transporter_destroy(s_xz.tls);
        s_xz.tls = NULL;
    }
    if (s_xz.tcp) {
        tuya_transporter_destroy(s_xz.tcp);
        s_xz.tcp = NULL;
    }
}

static int __xz_ws_send(uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    if (!s_xz.ws_up || NULL == s_xz.tls) {
        return -1;
    }
    uint8_t hdr[14];
    int hl = 0;
    uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};

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

    if (tuya_transporter_write(s_xz.tls, hdr, hl, 3000) < 0) {
        return -1;
    }
    uint8_t chunk[256];
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = (len - off > sizeof(chunk)) ? sizeof(chunk) : (len - off);
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = payload[off + i] ^ mask[(off + i) & 3];
        }
        if (tuya_transporter_write(s_xz.tls, chunk, n, 3000) < 0) {
            return -1;
        }
        off += n;
    }
    return 0;
}

static BOOL_T __xz_ws_connect(void)
{
    __xz_ws_close();

    char host[96] = {0}, path[128] = {0};
    int port = 443;
    if (!__xz_parse_url(s_xz.ws_url, host, sizeof(host), path, sizeof(path), &port)) {
        PR_ERR("[XZ] bad ws url: %s", s_xz.ws_url);
        return FALSE;
    }

    s_xz.tcp = tuya_transporter_create(TRANSPORT_TYPE_TCP, NULL);
    if (NULL == s_xz.tcp) { return FALSE; }
    s_xz.tls = tuya_transporter_create(TRANSPORT_TYPE_TLS, s_xz.tcp);
    if (NULL == s_xz.tls) {
        __xz_ws_close();
        return FALSE;
    }

    tuya_tls_config_t tcfg = {0};
    tcfg.mode         = TUYA_TLS_SERVER_CERT_MODE;
    tcfg.hostname     = host;
    tcfg.port         = (uint16_t)port;
    tcfg.timeout      = 8000;
    tcfg.verify       = true;
    tcfg.ca_cert      = (char *)g_tenclass_ca_pem;
    tcfg.ca_cert_size = (int)g_tenclass_ca_pem_len;
    if (OPRT_OK != tuya_transporter_ctrl(s_xz.tls, TUYA_TRANSPORTER_SET_TLS_CONFIG, &tcfg) ||
        OPRT_OK != tuya_transporter_connect(s_xz.tls, host, port, 8000)) {
        PR_ERR("[XZ] ws tls connect failed");
        __xz_ws_close();
        return FALSE;
    }

    char req[640];
    int n;
    if (s_xz.ws_token[0]) {
        n = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Authorization: Bearer %s\r\n"
            "Protocol-Version: 1\r\n"
            "Device-Id: %s\r\n"
            "Client-Id: %s\r\n"
            "\r\n", path, host, s_xz.ws_token, s_xz.mac_str, s_xz.uuid);
    } else {
        n = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Protocol-Version: 1\r\n"
            "Device-Id: %s\r\n"
            "Client-Id: %s\r\n"
            "\r\n", path, host, s_xz.mac_str, s_xz.uuid);
    }
    if (tuya_transporter_write(s_xz.tls, (uint8_t *)req, n, 3000) < 0) {
        PR_ERR("[XZ] upgrade write failed");
        __xz_ws_close();
        return FALSE;
    }

    char rsp[1024];
    int got = 0;
    while (got < (int)sizeof(rsp) - 1) {
        int r = tuya_transporter_read(s_xz.tls, (uint8_t *)rsp + got, 1, 5000);
        if (r <= 0) { break; }
        got += r;
        if (got >= 4 && 0 == strcmp(rsp + got - 4, "\r\n\r\n")) { break; }
    }
    rsp[got] = '\0';
    if (NULL == strstr(rsp, "101")) {
        PR_ERR("[XZ] upgrade failed: %.160s", rsp);
        __xz_ws_close();
        return FALSE;
    }
    s_xz.ws_up = TRUE;
    s_xz.accum_len = 0;
    PR_NOTICE("[XZ] websocket 已连接");
    return TRUE;
}

/* 发 JSON 文本帧 */
static void __xz_send_json(const char *json)
{
    __xz_ws_send(0x01, (const uint8_t *)json, strlen(json));
}

static void __xz_send_listen_start(void)
{
    char msg[160];
    if (s_xz.sid[0]) {
        snprintf(msg, sizeof(msg),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}",
                 s_xz.sid);
    } else {
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}");
    }
    __xz_send_json(msg);
}

/***********************************************************
**************************音频处理***************************
***********************************************************/
/* 24k -> 16k 线性插值重采样 */
static uint32_t __xz_resample_24k16k(const int16_t *in, uint32_t in_sm, int16_t *out)
{
    uint32_t out_sm = in_sm * 2 / 3;
    for (uint32_t i = 0; i < out_sm; i++) {
        uint32_t pos = i * 3 / 2;
        uint32_t frac = (i * 3) % 2;
        int32_t a = in[pos];
        int32_t b = (pos + 1 < in_sm) ? in[pos + 1] : a;
        out[i] = (int16_t)((a * (2 - frac) + b * frac) / 2);
    }
    return out_sm;
}

static void __xz_push_play(const uint8_t *pcm16k, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        s_xz.play_ring[s_xz.play_wr % XZ_PLAY_RING_SZ] = pcm16k[i];
        s_xz.play_wr++;
    }
}

/* 收到服务器下行 OPUS 帧：解码 -> 重采样 -> 入播放环形缓冲 */
static void __xz_on_audio(const uint8_t *data, uint32_t len)
{
    static uint32_t dn_frames = 0, dn_pushed = 0;
    if (NULL == s_xz.dec || 0 == len) {
        return;
    }
    int sm = opus_decode(s_xz.dec, data, (int32_t)len,
                         (int16_t *)s_xz.dec_pcm, XZ_DEC_BUF_SZ / 2, 0);
    if (sm <= 0) {
        PR_ERR("[XZ] opus_decode failed %d", sm);
        return;
    }
    if (24000 == s_xz.srv_rate) {
        uint32_t out_sm = __xz_resample_24k16k((int16_t *)s_xz.dec_pcm, (uint32_t)sm,
                                               (int16_t *)s_xz.dec_pcm);
        __xz_push_play(s_xz.dec_pcm, out_sm * 2);
    } else {
        __xz_push_play(s_xz.dec_pcm, (uint32_t)sm * 2);
    }
    dn_frames++;
    dn_pushed += (uint32_t)sm * 2;
    /* 下行音频电平探针：确认解码输出是真实语音而非全零/垃圾数据 */
    {
        static int dn_pk_max = 0;
        for (int i = 0; i < sm; i++) {
            int v = ((int16_t *)s_xz.dec_pcm)[i];
            if (v < 0) { v = -v; }
            if (v > dn_pk_max) { dn_pk_max = v; }
        }
        if (1 == dn_frames || 0 == (dn_frames % 50)) {
            PR_NOTICE("[XZ][DN] frames=%u push=%uKB ring_wait=%u pk=%d",
                      dn_frames, dn_pushed / 1024, s_xz.play_wr - s_xz.play_rd,
                      dn_pk_max);
            dn_pk_max = 0;
        }
    }
}

/* 麦克风 PCM 入环形缓冲（溢出丢最旧） */
void eink_xz_on_pcm(const uint8_t *data, uint32_t len)
{
    /* 预连接（paused）期间也收麦克风：这段时间用户多半已经开始说指令，
     * 唤醒确认后这段音频会一起补发给服务器，避免开头几句话被丢掉 */
    if (!(s_xz.active || s_xz.paused) || NULL == s_xz.mic_ring) {
        return;
    }
    uint32_t used = s_xz.mic_wr - s_xz.mic_rd;
    /* 预连接挂起态没有消费者：只保留最近 2.5 秒，避免唤醒确认后把十几秒
     * 旧音频（多半是静音/噪声）一起灌给服务器，推高 VAD 误判 */
    const uint32_t cap = s_xz.paused ? (uint32_t)(XZ_PRE_KEEP_MS * 16 * 2) : XZ_MIC_RING_SZ;
    if (used + len > cap) {
        s_xz.mic_rd += (used + len) - cap;
    }
    for (uint32_t i = 0; i < len; i++) {
        s_xz.mic_ring[s_xz.mic_wr % XZ_MIC_RING_SZ] = data[i];
        s_xz.mic_wr++;
    }
}

/***********************************************************
**************************消息处理***************************
***********************************************************/
static void __xz_handle_text(char *msg)
{
    cJSON *root = cJSON_Parse(msg);
    if (NULL == root) {
        PR_ERR("[XZ] bad json: %.80s", msg);
        return;
    }
    cJSON *jt = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(jt)) {
        cJSON_Delete(root);
        return;
    }
    const char *ty = jt->valuestring;
    s_xz.idle_ms = 0;   /* 任何下行都算活跃 */

    if (0 == strcmp(ty, "hello")) {
        cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid) && sid->valuestring[0]) {
            snprintf(s_xz.sid, sizeof(s_xz.sid), "%s", sid->valuestring);
        }
        cJSON *ap = cJSON_GetObjectItem(root, "audio_params");
        if (cJSON_IsObject(ap)) {
            cJSON *sr = cJSON_GetObjectItem(ap, "sample_rate");
            if (cJSON_IsNumber(sr) && sr->valueint > 0) {
                s_xz.srv_rate = sr->valueint;
            }
        }
        PR_NOTICE("[XZ] 服务器 hello session=%s rate=%d", s_xz.sid, s_xz.srv_rate);
    } else if (0 == strcmp(ty, "stt")) {
        cJSON *tx = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(tx)) {
            PR_NOTICE("[你说] %s", tx->valuestring ? tx->valuestring : "");
        }
    } else if (0 == strcmp(ty, "tts")) {
        cJSON *st = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(st)) {
            if (0 == strcmp(st->valuestring, "start")) {
                s_xz.state = XZ_ST_SPEAK;
                s_xz.mic_rd = s_xz.mic_wr;      /* 丢弃说话期间的拾音 */
                PR_NOTICE("[XZ] AI 开始说话");
            } else if (0 == strcmp(st->valuestring, "stop")) {
                PR_NOTICE("[XZ] AI 说完");
                s_xz.state = XZ_ST_LISTEN;
                s_xz.play_wr = 0; s_xz.play_rd = 0;
                __xz_send_listen_start();
            } else if (0 == strcmp(st->valuestring, "sentence_start")) {
                cJSON *tx = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(tx) && tx->valuestring) {
                    PR_NOTICE("[AI] %s", tx->valuestring);
                }
            }
        }
    } else if (0 == strcmp(ty, "llm")) {
        cJSON *em = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(em)) {
            PR_DEBUG("[XZ] emotion=%s", em->valuestring);
        }
    } else if (0 == strcmp(ty, "system")) {
        cJSON *cm = cJSON_GetObjectItem(root, "command");
        if (cJSON_IsString(cm) && 0 == strcmp(cm->valuestring, "reboot")) {
            PR_NOTICE("[XZ] 服务器请求重启");
        }
    } else {
        PR_DEBUG("[XZ] msg type=%s", ty);
    }
    cJSON_Delete(root);
}

/* 轮询收帧：文本/二进制/心跳 */
static void __xz_poll(void)
{
    if (!s_xz.ws_up || NULL == s_xz.tls) {
        return;
    }
    uint8_t hdr[2];
    if (__xz_read_exact(hdr, 2, 20) < 0) {
        return;
    }
    int opcode = hdr[0] & 0x0F;
    uint32_t len = hdr[1] & 0x7F;
    int masked = hdr[1] & 0x80;

    if (len == 126) {
        uint8_t ext[2];
        if (__xz_read_exact(ext, 2, 100) < 0) { goto __xz_poll_fail; }
        len = ((uint32_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (__xz_read_exact(ext, 8, 100) < 0) { goto __xz_poll_fail; }
        len = 0;
        for (int i = 4; i < 8; i++) { len = (len << 8) | ext[i]; }
    }
    uint8_t maskk[4] = {0};
    if (masked && __xz_read_exact(maskk, 4, 100) < 0) { goto __xz_poll_fail; }

    if (len > XZ_ACCUM_SZ) {
        /* 大帧读掉丢弃，保住会话 */
        PR_ERR("[XZ] frame too large, discard: %u", len);
        uint8_t sink[256];
        uint32_t left = len;
        while (left > 0) {
            uint32_t want = (left > sizeof(sink)) ? sizeof(sink) : left;
            if (__xz_read_exact(sink, want, 2000) < 0) { goto __xz_poll_fail; }
            left -= want;
        }
        return;
    }

    if (len > 0) {
        if (__xz_read_exact((uint8_t *)s_xz.accum, len, 3000) < 0) { goto __xz_poll_fail; }
        if (masked) {
            for (uint32_t i = 0; i < len; i++) {
                s_xz.accum[i] ^= maskk[i & 3];
            }
        }
    }

    if (0x01 == opcode && len > 0) {
        s_xz.accum[len] = '\0';
        __xz_handle_text(s_xz.accum);
    } else if (0x02 == opcode && len > 0) {
        __xz_on_audio((uint8_t *)s_xz.accum, len);
    } else if (0x09 == opcode) {
        __xz_ws_send(0x0A, (const uint8_t *)s_xz.accum, len);
    } else if (0x08 == opcode) {
        if (len >= 2) {
            PR_ERR("[XZ] server closed code=%u reason=%.80s",
                   ((uint32_t)(uint8_t)s_xz.accum[0] << 8) | (uint8_t)s_xz.accum[1],
                   s_xz.accum + 2);
        } else {
            PR_ERR("[XZ] server closed");
        }
        goto __xz_poll_fail;
    }
    return;

__xz_poll_fail:
    PR_ERR("[XZ] 连接断开，会话结束");
    __xz_ws_close();
    s_xz.active = FALSE;
    s_xz.state = XZ_ST_IDLE;
}

/***********************************************************
**************************会话线程***************************
***********************************************************/
static BOOL_T __xz_session_open(BOOL_T paused_mode)
{
    /* OPUS 编解码器初始化（状态结构体必须放 SRAM：PSRAM 走 QSPI 仅支持
     * 4 字节对齐访问，opus/SILK 内核按 int16/int8 粒度访问状态字段，
     * 放 PSRAM 会触发 UsageFault(UNALIGNED) 崩溃） */
    if (NULL == s_xz.enc) {
        s_xz.enc = (OpusEncoder *)tal_malloc(opus_encoder_get_size(1));
        if (NULL == s_xz.enc ||
            OPUS_OK != opus_encoder_init(s_xz.enc, XZ_SRV_SAMPLE, 1, OPUS_APPLICATION_VOIP)) {
            PR_ERR("[XZ] opus encoder init failed");
            return FALSE;
        }
        opus_encoder_ctl(s_xz.enc, OPUS_SET_BITRATE(36000));
        opus_encoder_ctl(s_xz.enc, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(s_xz.enc, OPUS_SET_VBR(1));
    }
    if (NULL == s_xz.dec) {
        s_xz.dec = (OpusDecoder *)tal_malloc(opus_decoder_get_size(1));
        if (NULL == s_xz.dec) {
            return FALSE;
        }
    }
    if (OPRT_OK != opus_decoder_init(s_xz.dec, s_xz.srv_rate, 1)) {
        PR_ERR("[XZ] opus decoder init failed");
        return FALSE;
    }

    if (!__xz_ws_connect()) {
        return FALSE;
    }

    /* hello 握手 */
    char hello[256];
    snprintf(hello, sizeof(hello),
        "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
        "\"features\":{\"mcp\":false},"
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,"
        "\"channels\":1,\"frame_duration\":%d}}", XZ_SRV_SAMPLE, XZ_FRAME_MS);
    __xz_send_json(hello);

    s_xz.state = XZ_ST_IDLE;
    for (int i = 0; i < 100; i++) {
        __xz_poll();
        if (!s_xz.ws_up) {
            return FALSE;
        }
        if (s_xz.sid[0] || s_xz.state != XZ_ST_IDLE) {
            break;
        }
        tal_system_sleep(30);
    }
    /* 服务器 hello 必达（session_id 可选），超时按 audio 失败处理 */
    if (!s_xz.ws_up) {
        return FALSE;
    }

    s_xz.mic_wr = 0; s_xz.mic_rd = 0;
    s_xz.play_wr = 0; s_xz.play_rd = 0;
    s_xz.idle_ms = 0;
    s_xz.end_req = FALSE;
    s_xz.preroll = TRUE;
    s_xz.play_start_ms = 0;
    if (s_xz.confirm) {
        paused_mode = FALSE;        /* 握手途中唤醒已命中：别再挂起，直接开麦 */
    }
    s_xz.confirm = FALSE;
    if (paused_mode) {
        /* 只握手不开麦：服务器不会收到上行，也不会自己开口 */
        s_xz.paused = TRUE;
        s_xz.active = FALSE;
        s_xz.state = XZ_ST_PAUSED;
        s_xz.pre_ms = (uint32_t)tal_system_get_millisecond();
        PR_NOTICE("[XZ] 预连接就绪（已握手，待唤醒确认后开麦）");
    } else {
        s_xz.paused = FALSE;
        s_xz.active = TRUE;
        s_xz.state = XZ_ST_LISTEN;
        __xz_send_listen_start();
        PR_NOTICE("[XZ] 小智对话就绪 请直接说话");
    }
    return TRUE;
}

/* 会话结束：不撕连接，退回「待机会话」状态，下次唤醒直接开麦 */
static void __xz_to_standby(void)
{
    if (!s_xz.ws_up) {
        return;
    }
    s_xz.active = FALSE;
    s_xz.paused = TRUE;
    s_xz.state = XZ_ST_PAUSED;
    s_xz.end_req = FALSE;
    s_xz.idle_ms = 0;
    s_xz.mic_wr = 0;
    s_xz.mic_rd = 0;
    s_xz.play_wr = 0;
    s_xz.play_rd = 0;
    s_xz.pre_ms = (uint32_t)tal_system_get_millisecond();
    s_xz.keepalive_ms = s_xz.pre_ms;
    PR_NOTICE("[XZ] 会话结束，连接留作待机（下次唤醒免握手）");
}

static void __xz_session_close(void)
{
    s_xz.active = FALSE;
    s_xz.state = XZ_ST_IDLE;
    __xz_ws_close();
    s_xz.sid[0] = '\0';
}

/* 实际的连接流程（在 xz_call 线程内执行，TLS 握手栈需求大） */
static BOOL_T __xz_do_start(BOOL_T paused_mode)
{
    if (NULL == s_xz.mic_ring) {
        s_xz.mic_ring = (uint8_t *)tkl_system_psram_malloc(XZ_MIC_RING_SZ);
        s_xz.play_ring = (uint8_t *)tkl_system_psram_malloc(XZ_PLAY_RING_SZ);
        s_xz.accum = (char *)tkl_system_psram_malloc(XZ_ACCUM_SZ);
        s_xz.dec_pcm = (uint8_t *)tkl_system_psram_malloc(XZ_DEC_BUF_SZ);
        if (!s_xz.mic_ring || !s_xz.play_ring || !s_xz.accum || !s_xz.dec_pcm) {
            PR_ERR("[XZ] psram malloc failed");
            return FALSE;
        }
    }

    /* OTA 配置（每次会话前刷新，确保 url/token 有效） */
    if (!__xz_ota_prepare()) {
        return FALSE;
    }

    if (!__xz_session_open(paused_mode)) {
        __xz_session_close();
        return FALSE;
    }
    return TRUE;
}

static void __xz_thread(void *arg)
{
    static uint8_t pcm[XZ_FRAME_BYTES];
    (void)arg;

    while (1) {
        /* 连接请求：OTA + TLS 握手都在本线程执行（栈 16KB） */
        if (s_xz.start_req && !s_xz.active && !s_xz.paused) {
            BOOL_T pre = s_xz.pre_req;
            s_xz.start_req = FALSE;
            s_xz.pre_req = FALSE;
            s_xz.busy = TRUE;
            BOOL_T ok = __xz_do_start(pre);
            s_xz.busy = FALSE;
            if (!ok) {
                __uart_reply("RSP:小智连接失败 见日志\r\n");
            }
            continue;
        }

        if (!(s_xz.active || s_xz.paused) || !s_xz.ws_up) {
            tal_system_sleep(100);
            continue;
        }

        __xz_poll();

        /* 常驻待机会话：不开麦、不播放，等唤醒确认。
         * 1) 连接被服务器断掉 → 立即关掉并按冷却重连，保持「醒来即用」
         * 2) 每 30 秒发 WS ping 保活
         * 3) 15 分钟没用才真正关闭 */
        if (s_xz.paused) {
            uint32_t now = (uint32_t)tal_system_get_millisecond();
            if (!s_xz.ws_up) {
                PR_NOTICE("[XZ] 待机会话被断开，重连");
                s_xz.paused = FALSE;
                s_xz.state = XZ_ST_IDLE;
                s_xz.pre_ms = now;
                __xz_session_close();
            } else if (now - s_xz.keepalive_ms >= XZ_KEEPALIVE_MS) {
                s_xz.keepalive_ms = now;
                (void)__xz_ws_send(0x09, NULL, 0);      /* ping */
            } else if (now - s_xz.pre_ms > XZ_PRE_TIMEOUT_MS) {
                PR_NOTICE("[XZ] 待机会话空闲超时，关闭");
                s_xz.paused = FALSE;
                s_xz.state = XZ_ST_IDLE;
                s_xz.pre_ms = now;
                __xz_session_close();
            }
            tal_system_sleep(20);
            continue;
        }

        if (!s_xz.active || !s_xz.ws_up) {
            continue;
        }

        /* 播放下行 */
        TDL_AUDIO_HANDLE_T audio = eink_chat_audio_handle();
        static BOOL_T play_null_said = FALSE;
        static uint32_t play_calls = 0;
        if (NULL == audio && !play_null_said) {
            PR_ERR("[XZ][DN] audio handle NULL, speaker 不可用");
            play_null_said = TRUE;
        }
        /* 本会话首帧前先灌 240ms 静音：DAC 与功放从静置进入工作有起振过程，
         * 直接开播会让开头一段声音明显偏小/听不见 */
        if (s_xz.preroll && audio) {
            static uint8_t zeros[XZ_PLAY_FRAME];
            for (int i = 0; i < 6; i++) {
                (void)tdl_audio_play(audio, zeros, sizeof(zeros));
            }
            s_xz.preroll = FALSE;
            s_xz.play_start_ms = (uint32_t)tal_system_get_millisecond();
            s_xz.last_play_ms = s_xz.play_start_ms;
            PR_NOTICE("[XZ][DN] 播放预热 240ms（起振等待）");
        }

        while ((s_xz.play_wr - s_xz.play_rd) >= 1280 && audio) {
            static uint8_t play[1280];
            static int play_pk = 0;
            for (int i = 0; i < 1280; i++) {
                play[i] = s_xz.play_ring[(s_xz.play_rd + i) % XZ_PLAY_RING_SZ];
            }
            s_xz.play_rd += 1280;
            /* 软件音量衰减（DAC 增益固定 0dB 保信噪比，音量在此控制）
             * 开局 XZ_BOOST_MS 内额外补 boost%，抵消功放起振期的音量偏低；
             * 稳态后回落为设定音量（xz boost 0 可关闭补偿） */
            {
                uint32_t since = (s_xz.play_start_ms)
                               ? ((uint32_t)tal_system_get_millisecond() - s_xz.play_start_ms)
                               : 0xFFFFFFFF;
                int extra = (0 != s_xz.play_start_ms && since < XZ_BOOST_MS) ? s_xz.boost : 0;
                for (int i = 0; i < 640; i++) {
                    int32_t v = ((int16_t *)play)[i];
                    if (extra > 0) { v = (v * (100 + extra)) / 100; }
                    ((int16_t *)play)[i] = eink_xz_vol_scale((int16_t)v);
                }
            }
            /* 播放数据电平探针：核对从环形缓冲拷出的样本是否与解码端一致 */
            {
                for (int i = 0; i < 640; i++) {
                    int v = ((int16_t *)play)[i];
                    if (v < 0) { v = -v; }
                    if (v > play_pk) { play_pk = v; }
                }
            }
            OPERATE_RET prt = tdl_audio_play(audio, play, 1280);
            s_xz.last_play_ms = (uint32_t)tal_system_get_millisecond();
            play_calls++;
            if (OPRT_OK != prt) {
                PR_ERR("[XZ][DN] tdl_audio_play err %d", prt);
            } else if (1 == play_calls || 0 == (play_calls % 50)) {
                PR_NOTICE("[XZ][DN] play_calls=%u ring_wait=%u pk=%d",
                          play_calls, s_xz.play_wr - s_xz.play_rd, play_pk);
                play_pk = 0;
            }
        }

        /* 上行（半双工：AI 说话期间暂停拾音，防回声） */
        if (XZ_ST_LISTEN == s_xz.state && audio) {
            if (s_xz.play_wr != s_xz.play_rd) {
                s_xz.mic_rd = s_xz.mic_wr;
                s_xz.idle_ms = 0;
                s_xz.echo_guard_ms = XZ_ECHO_GUARD_MS;  /* 播放后重新武装余音守卫 */
            } else if (s_xz.echo_guard_ms > 0) {
                /* 余音期：喇叭已无待播数据，但物理余响还在，继续丢弃拾音 */
                s_xz.echo_guard_ms -= 20;
                s_xz.mic_rd = s_xz.mic_wr;
                s_xz.idle_ms = 0;
            } else if ((uint32_t)tal_system_get_millisecond() - s_xz.last_play_ms
                       < XZ_ECHO_GUARD_MS) {
                /* ring 排空间隙（ring_wait=0 常态）：喇叭仍在发声，丢弃拾音。
                 * 旧逻辑此处直接上行，导致 AI 说话中的回声帧被服务器
                 * VAD 误判为用户插话（自闻自答的真正泄漏点） */
                s_xz.mic_rd = s_xz.mic_wr;
                s_xz.idle_ms = 0;
            } else {
                int cap = XZ_UP_CHUNK;
                while ((s_xz.mic_wr - s_xz.mic_rd) >= XZ_FRAME_BYTES && cap-- > 0) {
                    uint32_t pos = s_xz.mic_rd % XZ_MIC_RING_SZ;
                    uint32_t first = XZ_MIC_RING_SZ - pos;
                    if (first > XZ_FRAME_BYTES) { first = XZ_FRAME_BYTES; }
                    memcpy(pcm, s_xz.mic_ring + pos, first);
                    if (XZ_FRAME_BYTES > first) {
                        memcpy(pcm + first, s_xz.mic_ring, XZ_FRAME_BYTES - first);
                    }
                    s_xz.mic_rd += XZ_FRAME_BYTES;

                    int pk = 0;
                    for (int i = 0; i < XZ_FRAME_SM; i++) {
                        int a = ((int16_t *)pcm)[i];
                        if (a < 0) { a = -a; }
                        if (a > pk) { pk = a; }
                    }
                    if (pk > 3000) {
                        s_xz.idle_ms = 0;
                    }

                    int nb = opus_encode(s_xz.enc, (int16_t *)pcm, XZ_FRAME_SM,
                                         s_xz.enc_out, sizeof(s_xz.enc_out));
                    if (nb > 0) {
                        static uint32_t up_frames = 0;
                        up_frames++;
                        if (1 == up_frames || 0 == (up_frames % 25)) {
                            PR_NOTICE("[XZ][UP] sent=%u pk=%d nb=%d",
                                      up_frames, pk, nb);
                        }
                        if (__xz_ws_send(0x02, s_xz.enc_out, (uint32_t)nb) < 0) {
                            PR_ERR("[XZ] 上行发送失败");
                            __xz_ws_close();
                            s_xz.active = FALSE;
                            s_xz.state = XZ_ST_IDLE;
                            break;
                        }
                    } else {
                        PR_ERR("[XZ] opus_encode %d", nb);
                    }
                }
            }
        }

        /* 空闲超时 / 手动结束 */
        if (s_xz.active) {
            s_xz.idle_ms += 20;
            if (s_xz.end_req || s_xz.idle_ms > XZ_IDLE_MS) {
                PR_NOTICE("[XZ] %s", s_xz.end_req ? "结束小智对话" : "长时间没有说话，对话结束");
                __uart_reply(s_xz.end_req ? "RSP:小智对话已结束\r\n" : "RSP:小智对话结束\r\n");
                s_xz.end_req = FALSE;
                __xz_to_standby();      /* 连接留着待机，下次唤醒秒开 */
            }
        }

        tal_system_sleep(20);
    }
}

/***********************************************************
**************************对外接口***************************
***********************************************************/
BOOL_T eink_xz_enabled(void)
{
    return s_xz.enabled;
}

/* 喇叭软件音量：DAC 数字增益保持 0x3f(+18dB) 以盖过模拟底噪（电流声），
 * 音量在应用层对播放样本线性衰减。0x2d(0dB) DAC 增益实测底噪相对明显 */
static uint8_t s_xz_spk_vol = 60;

void eink_xz_set_speaker_vol(uint8_t vol)
{
    s_xz_spk_vol = (vol > 100) ? 100 : vol;
}

uint8_t eink_xz_get_speaker_vol(void)
{
    return s_xz_spk_vol;
}

int16_t eink_xz_vol_scale(int16_t sample)
{
    int32_t v = ((int32_t)sample * s_xz_spk_vol) / 100;
    if (v > 32767) { v = 32767; }
    if (v < -32768) { v = -32768; }
    return (int16_t)v;
}

void eink_xz_set_boost(int pct)
{
    if (pct < 0) { pct = 0; }
    if (pct > 60) { pct = 60; }
    s_xz.boost = pct;
}

void eink_xz_set_enable(BOOL_T on)
{
    s_xz.enabled = on;
    PR_NOTICE("[XZ] 小智模式 %s", on ? "已开启" : "已关闭");
}

BOOL_T eink_xz_is_active(void)
{
    return s_xz.active;
}

/* 连接中或会话中（唤醒线程等待用） */
BOOL_T eink_xz_engaged(void)
{
    return s_xz.active || s_xz.busy || s_xz.paused;
}

BOOL_T eink_xz_is_paused(void)
{
    return s_xz.paused;
}

/* 预连接：与唤醒词识别并行做握手，命中后 eink_xz_arm() 立即开麦 */
BOOL_T eink_xz_preconnect(void)
{
    if (!s_xz.enabled) {
        return FALSE;
    }
    if (s_xz.active || s_xz.paused || s_xz.start_req || s_xz.busy) {
        return FALSE;
    }
    if ((uint32_t)tal_system_get_millisecond() - s_xz.pre_ms < XZ_PRE_COOLDOWN_MS) {
        return FALSE;   /* 刚连过又断开：冷却，防噪声反复握手 */
    }
    s_xz.pre_req = TRUE;
    s_xz.start_req = TRUE;
    PR_NOTICE("[XZ] 预连接请求（识别唤醒词期间并行握手）");
    return TRUE;
}

/* 唤醒确认：预连接已就绪则立即开麦（缓冲的音频一并上行），否则发起正式会话 */
BOOL_T eink_xz_arm(void)
{
    if (s_xz.active && s_xz.ws_up) {
        return TRUE;
    }
    if (s_xz.paused && s_xz.ws_up) {
        s_xz.paused = FALSE;
        s_xz.active = TRUE;
        s_xz.state = XZ_ST_LISTEN;
        s_xz.end_req = FALSE;
        s_xz.idle_ms = 0;
        __xz_send_listen_start();
        PR_NOTICE("[XZ] 唤醒确认 开麦上行（预连接缓冲 %u 字节一并补发）",
                  (unsigned)(s_xz.mic_wr - s_xz.mic_rd));
        return TRUE;
    }
    s_xz.confirm = TRUE;            /* 正在握手：让这次连接完成后直接开麦 */
    if (!s_xz.busy && !s_xz.start_req) {
        s_xz.pre_req = FALSE;
        s_xz.start_req = TRUE;
    }
    s_xz.enabled = TRUE;
    return TRUE;
}

/* 放弃预连接：唤醒词没命中、或识别失败时调用 */
void eink_xz_abort(void)
{
    if (s_xz.paused) {
        PR_NOTICE("[XZ] 未确认唤醒，关闭预连接");
        s_xz.paused = FALSE;
        s_xz.state = XZ_ST_IDLE;
        s_xz.pre_ms = (uint32_t)tal_system_get_millisecond();
        __xz_session_close();
        return;
    }
    s_xz.pre_req = FALSE;
    if (!s_xz.active && s_xz.start_req) {
        s_xz.start_req = FALSE;
        s_xz.pre_ms = (uint32_t)tal_system_get_millisecond();
        PR_NOTICE("[XZ] 取消未完成的预连接");
    }
}

void eink_xz_end(void)
{
    if (s_xz.active) {
        s_xz.end_req = TRUE;
    }
}

/* 关闭待机会话（xz off / 长时间无网时用） */
void eink_xz_standby_off(void)
{
    if (s_xz.paused) {
        s_xz.paused = FALSE;
        s_xz.state = XZ_ST_IDLE;
        s_xz.pre_ms = (uint32_t)tal_system_get_millisecond();
        __xz_session_close();
    }
}

/* 仅投递请求，繁重的 TLS 握手由 xz_call 线程执行（栈更大） */
BOOL_T eink_xz_start(void)
{
    if (s_xz.active || s_xz.busy) {
        return TRUE;
    }
    s_xz.enabled = TRUE;
    s_xz.start_req = TRUE;
    return TRUE;
}

void eink_xz_init(void)
{
    memset(&s_xz, 0, sizeof(s_xz));
    s_xz.enabled = TRUE;   /* 默认开启：唤醒词命中后走小智通道（xiaozhi.me） */
    s_xz.srv_rate = 16000;
    s_xz.boost = 30;       /* 会话开局 3 秒额外 +30% 增益（功放起振补偿） */
    s_xz.play_start_ms = 0;
    __xz_load_identity();
    snprintf(s_xz.ota_url, sizeof(s_xz.ota_url), XZ_OTA_URL_DEF);

    THREAD_HANDLE th = NULL;
    /* 32KB：silk_encode_frame_FIX 序言需在栈上分配 ~10.7KB 局部变量，
     * 加 TLS 握手调用链开销，16KB 会栈溢出（UsageFault@encode_frame_FIX 序言） */
    THREAD_CFG_T cfg = { .stackDepth = 32 * 1024, .priority = THREAD_PRIO_2, .thrdname = "xz_call" };
    if (OPRT_OK == tal_thread_create_and_start(&th, NULL, NULL, __xz_thread, NULL, &cfg)) {
        PR_NOTICE("[XZ] 小智通道线程就绪 OTA=%s", s_xz.ota_url);
    } else {
        PR_ERR("[XZ] 线程创建失败");
    }
}
