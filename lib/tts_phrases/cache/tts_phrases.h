#ifndef TTS_PHRASES_H
#define TTS_PHRASES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固定话术枚举 —— 由 tools/gen_tts_clips.py 从 tools/phrases.csv 自动生成，请勿手改 */
typedef enum {
    TTS_PHRASE_PORTAL_START = 0,   /* 配网中，请连接设备热点 */
    TTS_PHRASE_PORTAL_OK            , /* 配网成功 */
    TTS_PHRASE_NET_CONNECTED        , /* 网络已连接 */
    TTS_PHRASE_STARTUP              , /* 小智已启动 */
    TTS_PHRASE_NET_DISCONNECTED     , /* 网络已断开 */
    TTS_PHRASE_CONNECT_FAILED       , /* 连接失败，请检查密码 */
    TTS_PHRASE_PLEASE_WAIT          , /* 请稍候 */
    TTS_PHRASE_OK                   , /* 好的 */
    TTS_PHRASE_CANCEL               , /* 已取消 */
    TTS_PHRASE_PROCESSING           , /* 正在处理 */
    TTS_PHRASE_VOLUME_UP            , /* 音量已增大 */
    TTS_PHRASE_VOLUME_DOWN          , /* 音量已减小 */
    TTS_PHRASE_WAKE_REPLY           , /* 我在，请说 */
    TTS_PHRASE_AUTH_FAILED          , /* 鉴权失败 */
    TTS_PHRASE_AI_CONNECTED         , /* AI大模型连接成功 */
    TTS_PHRASE_AI_DISCONNECTED      , /* AI大模型连接失败 */
    TTS_PHRASE_VOLUME_MAX           , /* 音量已最大 */
    TTS_PHRASE_VOLUME_MIN           , /* 音量已最小 */
    TTS_PHRASE_VOLUME_ZERO          , /* 已静音 */
    TTS_PHRASE_MAX
} tts_phrase_t;

/* 一条离线语音片段：原始 s16le 单声道 PCM */
typedef struct {
    const uint8_t *data;        /* PCM 数据，NULL 表示未提供 */
    uint32_t       len;         /* 字节数 */
    uint32_t       sample_rate; /* 采样率 Hz */
} tts_clip_t;

/* 由 tools/gen_tts_clips.py 生成 */
extern const tts_clip_t tts_clips[TTS_PHRASE_MAX];

/* 该话术对应的中文文案（供云 TTS 兜底 / 日志） */
const char *tts_phrase_text(tts_phrase_t ph);

#ifdef __cplusplus
}
#endif

#endif /* TTS_PHRASES_H */
