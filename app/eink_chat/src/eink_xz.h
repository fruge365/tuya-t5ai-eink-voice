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

/* 麦克风 PCM（16k 16bit 单声道）输入，仅会话期间有效 */
void   eink_xz_on_pcm(const uint8_t *data, uint32_t len);

/* 开启小智对话（阻塞：OTA 配置 -> 激活 -> WS 握手 -> 进入 listen） */
BOOL_T eink_xz_start(void);

/* 请求结束当前小智会话（异步，由会话线程收尾） */
void   eink_xz_end(void);

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
