# 板级改动说明

本应用需要在 TuyaOpen 的板级文件 `boards/T5AI/TUYA_T5AI_EINK_NFC/tuya_t5ai_eink_nfc.c` 上做三处改动，
补丁见同目录 `board-TUYA_T5AI_EINK_NFC.patch`，在 TuyaOpen 根目录执行：

```bash
git apply docs/board-TUYA_T5AI_EINK_NFC.patch
```

## 1. 功放使能脚改为 GPIO39

```c
-#define BOARD_SPEAKER_EN_PIN TUYA_GPIO_NUM_42
+#define BOARD_SPEAKER_EN_PIN TUYA_GPIO_NUM_39
```

原配置的 42 脚在本板上不是功放使能脚，导致「播放流程正常、但喇叭完全没声音」。
改到 39 后配合下面的极性设置才能出声。

## 2. 墨水屏 BUSY 引脚置为无效

```c
-#define BOARD_EINK_SPI_BUSY_PIN TUYA_GPIO_NUM_13
+#define BOARD_EINK_SPI_BUSY_PIN TUYA_GPIO_NUM_MAX
```

本板墨水屏的 BUSY 线实际未连接，读 13 脚会一直等不到电平翻转，驱动卡死。
置为 `TUYA_GPIO_NUM_MAX` 后驱动走固定 3s 延时，屏幕不再阻塞启动。

## 3. 关闭 AEC

```c
-cfg.aec_enable = 1;
+cfg.aec_enable = 0;
```

AEC v3 硬件模式（`AEC_MODE_HARDWARE` + `ec_only_output=1`）实测会让喇叭输出通道静音：
回环测试 `vt` 在 `aec=0` 时能出声，正常播放路径无声。
本应用为半双工（AI 说话期间暂停拾音），没有 AEC 需求，直接关掉。

## 功放极性说明

`spk_pin_polarity = TUYA_GPIO_LEVEL_LOW` 中的 LOW 指的是「静音电平」：
tkl 层初始化时把引脚拉到播放电平（高），静音时拉低。用 `spkab` 命令做 A/B 对照可现场验证——
前 5 秒 PA=低、后 5 秒 PA=高，若第 5 秒声音出现，则确认本板为高电平使能。
