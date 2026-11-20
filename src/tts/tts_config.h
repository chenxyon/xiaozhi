#ifndef TTS_CONFIG_H
#define TTS_CONFIG_H

/* TTS 组件统一配置 —— 仅剩通用常量，所有厂商专属参数/密钥都通过
 * tts_hub_init(provider, cfg) 以结构体传入，组件零硬编码。 */

/* 云 TTS / 离线片段统一输入采样率（重采样前基准） */
#define TTS_SRC_HZ       16000

/* 单次云合成允许的最大文本字节数（UTF-8）。超过则按标点分段。 */
#define TTS_SEG_MAX_BYTES  1200

#endif /* TTS_CONFIG_H */
