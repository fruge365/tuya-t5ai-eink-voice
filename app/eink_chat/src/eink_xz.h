/**
 * @file eink_xz.h
 * @brief eink_chat 小智(xiaozhi)协议客户端对外接口
 *
 * 小智协议：WebSocket + OPUS。设备经 OTA 接口取 WS 地址与 token，
 * 握手 hello 后进入 listen/speaking 状态机，音频为 OPUS 二进制帧。
 * 角色人设/音色/记忆/聊天记录由 xiaozhi.me 控制台（或自建 server）管理。
 */
#ifndef __EINK_XZ_H__
#define __EINK_XZ_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 小智模式是否开启（开启后唤醒词/`call` 命令路由到小智通道） */
BOOL_T eink_xz_enabled(void);
void   eink_xz_set_enable(BOOL_T on);

/* 小智会话是否进行中（麦克风回调据此分流 PCM） */
BOOL_T eink_xz_is_active(void);

/* 连接中或会话中（唤醒线程等待用） */
BOOL_T eink_xz_engaged(void);

/* 预连接：在识别唤醒词的同时完成 OTA+TLS+WS 握手，但暂不开麦上行。
 * 唤醒命中后 eink_xz_arm() 立即开麦，把等待期说的话补发上去。
 * 返回 FALSE 表示未发起（已启用中/冷却中/未开启小智模式） */
BOOL_T eink_xz_preconnect(void);
/* 预连接是否处于「已握手、未开麦」状态 */
BOOL_T eink_xz_is_paused(void);
/* 确认唤醒：把预连接转成正式会话（已握手则立即开麦，否则发起正式连接） */
BOOL_T eink_xz_arm(void);
/* 关闭常驻待机会话（小智关闭/需要断网时） */
void   eink_xz_standby_off(void);

/* 放弃预连接/未命中的会话 */
void   eink_xz_abort(void);

/* 麦克风 PCM（16k 16bit 单声道）输入，仅会话期间有效 */
void   eink_xz_on_pcm(const uint8_t *data, uint32_t len);

/* 开启小智对话（阻塞：OTA 配置 -> 激活 -> WS 握手 -> 进入 listen） */
BOOL_T eink_xz_start(void);

/* 请求结束当前小智会话（异步，由会话线程收尾） */
void   eink_xz_end(void);

/* 会话开局增益补偿百分比（0~60，默认 30）：抵消功放起振期音量偏低，3 秒后回落 */
void    eink_xz_set_boost(int pct);

/* 应用初始化时调用：加载设备标识、创建会话线程 */
void   eink_xz_init(void);

/* 喇叭软件音量（0-100）：DAC 数字增益固定拉满以盖过模拟底噪，
 * 音量在应用层对播放样本做衰减（保信噪比）。下行 TTS 与测试音均生效 */
void    eink_xz_set_speaker_vol(uint8_t vol);
uint8_t eink_xz_get_speaker_vol(void);
int16_t eink_xz_vol_scale(int16_t sample);

#ifdef __cplusplus
}
#endif
#endif /* __EINK_XZ_H__ */
