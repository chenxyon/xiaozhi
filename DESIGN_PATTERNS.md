# 小智项目设计模式全景图

> 本文档说明当前项目**实际使用了哪些设计模式**、**分布在哪些模块**、**代码证据在哪**。
> 对应规则：DESIGN_RULES.md v2.0 规则 3（必须使用的设计模式）+ 规则 8（状态模式）+ 规则 9（发布-订阅）
> 最后更新：2026-11-20

---

## 设计模式清单（共 12 种）

> GoF 23 种中有 10 种直接入选；2 种是跨项目通用的基础组件（接口 + 编译期开关）。
> 顺序按"地基 → 构建 → 编排"排列。

| # | 设计模式 | GoF 分类 | 分布模块 | 核心目的 |
|---|---------|---------|---------|---------|
| 0 | **接口 Interface** | 基础组件（非 GoF）| 1 处：IDisplay 纯虚基类（+ 隐含：tts_driver_t / kws_engine 函数表）| 定义"能做什么"不定义"怎么做"——所有模式的地基 |
| 1 | **适配器 Adapter** | 结构型 | 5 处：ws_bridge / max98357_bridge / inmp441_bridge / secret_store / audio_output | 第三方库接口不匹配时做包装 |
| 2 | **策略 Strategy** | 行为型 | 2 处：tts_hub / kws_hub | 同一能力多厂商可选，新增驱动只加文件 |
| 3 | **桥接 Bridge** | 结构型 | 1 处：tts_hub → 输出槽 | 业务能力与具体输出实现分离 |
| 4 | **观察者 Observer** | 行为型 | 2 处：kws_hub / tts_hub | 事件通知，单向回调 |
| 5 | **单例 Singleton** | 创建型 | 3 处：g_oled / event_bus / NVS namespace | 全局唯一资源 |
| 6 | **门面 Facade** | 结构型 | 3 处：board / secret_store / audio_output | 封装底层复杂性，暴露简单接口 |
| 7 | **模板方法 Template Method** | 行为型 | 1 处：tts_driver_t | 多实现共享固定算法骨架 |
| 8 | **延迟初始化 Lazy Init** | 创建型 | 2 处：app_ai / app_ai AI 配置 | 第一次用时才创建 |
| 9 | **编译期开关 Compile-Time Switch** | 架构级 | 全工程：platformio.ini build_flags | 功能按需裁剪，关掉不占 Flash |
| 10 | **状态模式 State** | 行为型 | 1 处：main_fsm | 业务流程状态集中管理，告别 bool prev_xxx |
| 11 | **发布-订阅 PubSub** | 行为型 | 1 处：event_bus | 解耦模块：谁发谁收互不相识 |

---

## 详细说明

---

### 0. 接口 Interface（基础组件，1 处定义 + 3 处隐含）

**GoF 里没有单独叫"接口模式"的条目**，但接口是几乎所有模式的**地基**——策略、桥接、适配器、模板方法、工厂、观察者……全都依赖"先定义抽象接口，再实现具体实现"这个前提。

**各语言的接口写法：**
- C++ / Java / C#：`abstract class` 或 `interface`，纯虚函数（`= 0`）
- C：函数表 + 函数指针（如 `tts_driver_t`）
- Python：`ABC` + `@abstractmethod`
- TS：`interface`

#### 0.1 `IDisplay` —— 屏幕统一接口（显式定义）

| 项 | 内容 |
|----|------|
| 文件 | [lib/DisplayIface/display_iface.h](file:///d:/ESP32S3/xiaozhi/lib/DisplayIface/display_iface.h) |
| 语言 | C++ 纯虚基类 |
| 实现类 | `OledU8g2`（当前项目唯一实现）；预留 `TftTfspi`、墨水屏 |
| 接口方法 | 12 个：`init / deinit / show / draw_icon / draw_rect / draw_line / clear / refresh / set_color / draw_image / screen / is_ready / panel` |

```cpp
// display_iface.h —— 纯虚基类（= 0 表示只定义不实现）
class IDisplay {
public:
    virtual ~IDisplay() = default;
    virtual esp_err_t init(oled_panel_t panel, const void *pins) = 0;
    virtual esp_err_t show(const char *text, uint16_t x, uint16_t y) = 0;
    virtual esp_err_t refresh() = 0;
    // ... 更多纯虚方法
};

// OledU8g2.hpp —— 实现所有纯虚方法
class OledU8g2 : public IDisplay { /* 全部 12 个方法都有实现 */ };
```

#### 0.2 项目里的隐含接口（C 语言）

| 接口 | 文件 | 说明 |
|------|------|------|
| `tts_driver_t` | [src/tts/tts_hub.h#L30-L35](file:///d:/ESP32S3/xiaozhi/src/tts/tts_hub.h#L30-L35) | C 语言用函数表模拟接口——3 个函数指针就是"必须实现的方法" |
| `kws_engine` 内部函数表 | [src/wake/kws_hub.c](file:///d:/ESP32S3/xiaozhi/src/wake/kws_hub.c) | 同上，引擎函数表 |
| `tts_pcm_sink_t` | [src/tts/tts_hub.h#L27](file:///d:/ESP32S3/xiaozhi/src/tts/tts_hub.h#L27) | 单一回调也是一种简化接口 |

#### 0.3 ⚠️ 当前项目中 IDisplay 多态价值**未充分发挥**

目前 `g_oled` 是**具体类指针**，不是接口指针：

```cpp
// board.h —— 直接暴露具体类
class OledU8g2;
extern OledU8g2 g_oled;

// main.cpp —— 调 OledU8g2 专有 API
g_oled.set_clock_slot(0, ...);   // ← 这个方法 IDisplay 里没有！
```

**问题**：如果换屏（比如换成 TftTfspi 彩色屏），`main.cpp` 里所有 `g_oled.set_clock_slot(...)` 都要改。

**理想状态**（工厂 + 策略组合，待实现）：

```cpp
// board.h —— 只依赖接口
extern IDisplay *g_display;

// DisplayFactory —— 根据 panel 类型创建
class DisplayFactory {
public:
    static IDisplay* create(oled_panel_t panel, const void *pins) {
        switch (panel) {
        case OLED_PANEL_SSD1306_128X32: return new OledU8g2();
        case OLED_PANEL_ILI9341_TFT:    return new TftTfspi();
        default: return nullptr;
        }
    }
};
```

改完后：
- `main.cpp` 一行不动（只依赖 IDisplay 接口）
- 新增 TftTfspi 时**只改 DisplayFactory 一个文件**
- 符合 DESIGN_RULES.md 规则 2-DIP（依赖抽象不依赖具体）

**结论：** `IDisplay` 本身是正确的接口定义，但当前只起到"类型约束"作用（保证 OledU8g2 必须实现 12 个方法）。要让它成为完整的"策略 + 工厂"模式，还需要把 `g_oled` 改成 `IDisplay*`，加一层 DisplayFactory。**预留**。

---

### 1. 适配器 Adapter（5 处）

**GoF 定义：** 将一个类的接口转换成客户希望的另一个接口，使原本不兼容的类可以一起工作。

项目里 C++ 第三方库（ArduinoWebsockets、Max98357Amp）需要被纯 C 代码调用，通过 `*_bridge` 包装层实现。

#### 1.1 `ws_bridge` —— ArduinoWebsockets → C

| 项 | 内容 |
|----|------|
| 文件 | [src/tts/ws_bridge.h](file:///d:/ESP32S3/xiaozhi/src/tts/ws_bridge.h) / [ws_bridge.cpp](file:///d:/ESP32S3/xiaozhi/src/tts/ws_bridge.cpp) |
| 被适配者 | C++ `WebsocketsClient` 类（ArduinoWebsockets 库） |
| 客户端 | C 文件 `tts_xfyun.c`（讯飞 TTS）和 `kws_xfyun_iat.c`（讯飞 RTASR）|
| 代码证据 | `.cpp` 内部持有 `websockets::WebsocketsClient client`，对外暴露纯 C `ws_bridge_*` 函数 |

```cpp
// ws_bridge.cpp —— C++ 实现，对外纯 C 接口
struct ws_bridge {
    websockets::WebsocketsClient client;   // C++ 对象
    ws_bridge_on_msg_t  on_msg;            // C 风格回调
    ws_bridge_on_evt_t  on_evt;
    void               *ctx;
};

extern "C" ws_bridge_t *ws_bridge_create(void) {
    ws_bridge_t *wb = (ws_bridge_t *)calloc(1, sizeof(ws_bridge_t));
    return wb;
}
```

#### 1.2 `max98357_bridge` / `inmp441_bridge` —— C++ Arduino 库 → C

| 项 | 内容 |
|----|------|
| 文件 | `lib/Max98357Amp/max98357_bridge.cpp` / `lib/Inmp441Mic/inmp441_bridge.cpp` |
| 被适配者 | C++ Arduino 风格的驱动类 |
| 客户端 | `board.c` / `audio_output.c` |
| 代码证据 | 用 `extern "C"` 包一层 C 函数 |

#### 1.3 `secret_store` —— ESP-IDF nvs.h → 简化的 4 接口

| 项 | 内容 |
|----|------|
| 文件 | [src/auth/secret_store.h](file:///d:/ESP32S3/xiaozhi/src/auth/secret_store.h) / [secret_store.c](file:///d:/ESP32S3/xiaozhi/src/auth/secret_store.c) |
| 被适配者 | ESP-IDF `nvs.h`（20+ API：open/get/set/commit/close/erase…）|
| 客户端 | `board.c` / `app_ai.c` |
| 对外接口 | 仅 4 个函数：`init / get_str / set_str / erase_all` |

#### 1.4 `audio_output` —— tts_hub PCM 回调 → max98357_tts_sink

| 项 | 内容 |
|----|------|
| 文件 | [src/audio/audio_output.c](file:///d:/ESP32S3/xiaozhi/src/audio/audio_output.c) |
| 被适配接口 | max98357_tts_sink(pcm, bytes, NULL) —— 3 参数 |
| 客户端接口 | tts_pcm_sink_t(pcm, bytes) —— 2 参数 |
| 中间串联 | HPF 高通滤波 → chunk → max98357_tts_sink |

---

### 2. 策略 Strategy（2 处）

**GoF 定义：** 定义一系列算法，把它们一个个封装起来，使它们可以互相替换，变化独立于使用算法的客户端。

#### 2.1 `tts_hub` —— TTS 驱动策略

| 项 | 内容 |
|----|------|
| 文件 | [src/tts/tts_hub.h](file:///d:/ESP32S3/xiaozhi/src/tts/tts_hub.h#L30-L35) |
| 策略接口 | `tts_driver_t` 函数表 |
| 策略实现 | 当前 1 个：讯飞 `tts_xfyun.c`；预留 2 个：百度 / 阿里 |

```c
// tts_hub.h —— 策略接口
typedef struct {
    const char *name;
    esp_err_t (*init)(const void *cfg);
    esp_err_t (*speak)(const char *text, tts_pcm_cb_t cb, void *ctx);
    bool      (*is_busy)(void);
} tts_driver_t;

// 选择策略
typedef enum {
    TTS_PROVIDER_XFYUN = 0,   // 已实现
    TTS_PROVIDER_BAIDU,       // 待实现
    TTS_PROVIDER_ALI,         // 待实现
} tts_provider_t;
```

**扩展方式：** 加 `tts_baidu.c` 填一张 `tts_driver_t` 表，tts_hub 零改动。

#### 2.2 `kws_hub` —— 唤醒引擎策略

| 项 | 内容 |
|----|------|
| 文件 | [src/wake/kws_hub.h](file:///d:/ESP32S3/xiaozhi/src/wake/kws_hub.h#L34-L38) |
| 策略接口 | `kws_engine_id_t` 枚举 + 内部引擎函数表 |
| 策略实现 | 讯飞在线听写（已实现）；ESP-SR WakeNet 离线（预留） |

```c
typedef enum {
    KWS_ENGINE_NONE = 0,
    KWS_ENGINE_XFYUN_IAT,   // ✅ 已实现
    KWS_ENGINE_ESP_SR,      // 🔜 预留
} kws_engine_id_t;
```

---

### 3. 桥接 Bridge（1 处）

**GoF 定义：** 将抽象与实现分离，使两者可以独立变化。

| 项 | 内容 |
|----|------|
| 文件 | [src/tts/tts_hub.h#L27](file:///d:/ESP32S3/xiaozhi/src/tts/tts_hub.h#L27) |
| 抽象 | `tts_pcm_sink_t` —— tts_hub 产出 PCM 后交给谁 |
| 实现 | `audio_output_tts_sink` —— HPF 高通 → max98357 功放 |
| 客户端 | `board_init_devices()` 里注册 |

```c
// tts_hub.h —— 抽象
typedef void (*tts_pcm_sink_t)(const int16_t *pcm, size_t bytes);

// board.c —— 注入具体实现
audio_output_init(80.0f, k_audio_pins.sample_rate_hz, 2048);
tts_hub_prepare(audio_output_tts_sink, ...);   // 桥接点！
```

**对比策略模式：** 策略是"同一目的多种实现可互换"；桥接是"抽象层根本不知道实现层的存在"——tts_hub 不知道输出是功放、DAC 还是文件，只管 PCM。

---

### 4. 观察者 Observer（2 处）

**GoF 定义：** 对象间一对多依赖，一个对象状态变化通知所有依赖者。

#### 4.1 `kws_hub` —— 唤醒事件回调

| 项 | 内容 |
|----|------|
| 文件 | [src/wake/kws_hub.h#L54](file:///d:/ESP32S3/xiaozhi/src/wake/kws_hub.h#L54) |
| 事件类型 | `KWS_EV_WAKE` / `KWS_EV_TEXT` / `KWS_EV_ERROR` |
| 订阅者 | `main.cpp` 的 `wake_cb` |

```c
typedef void (*kws_event_cb_t)(kws_event_t ev, const char *text, void *ctx);

// main.cpp 里注册
kws_hub_set_callback(wake_cb, NULL);
```

#### 4.2 `tts_hub` —— PCM 产出回调

| 项 | 内容 |
|----|------|
| 文件 | [src/tts/tts_hub.h#L21-L22](file:///d:/ESP32S3/xiaozhi/src/tts/tts_hub.h#L21-L22) |
| 事件类型 | 合成完的 PCM 样本块 |
| 订阅者 | `tts_driver_t.speak` 的回调（最终调 `audio_output_tts_sink`）|

---

### 5. 单例 Singleton（3 处）

**GoF 定义：** 保证一个类只有一个实例，并提供全局访问点。

| # | 模块 | 文件 | 代码证据 |
|---|------|------|---------|
| 5.1 | `g_oled` | [src/display/oled_u8g2.cpp](file:///d:/ESP32S3/xiaozhi/src/display/oled_u8g2.cpp#L26) | `OledU8g2 g_oled;` —— 全局唯一实例，所有文件 `extern` 拿到 |
| 5.2 | `event_bus` | [src/bus/event_bus.c](file:///d:/ESP32S3/xiaozhi/src/bus/event_bus.c) | 静态数组 `s_subs` 全局唯一，init 一次即可 |
| 5.3 | NVS namespace | [src/auth/secret_store.c](file:///d:/ESP32S3/xiaozhi/src/auth/secret_store.c) | namespace `"xiaozhi"` 是全局唯一的持久化空间 |

---

### 6. 门面 Facade（3 处）

**GoF 定义：** 为子系统中的一组接口提供一个一致的界面，使其更容易使用。

| # | 门面 | 封装的底层 | 对外接口数 |
|---|------|----------|----------|
| 6.1 | `board.h` | 6 个外设模块（OLED/Flash/I2S/功放/麦/NVS）| 4 个函数：log / init_devices / init_tts / audio_config |
| 6.2 | `secret_store.h` | ESP-IDF nvs.h（20+ API）| 4 个函数：init / get_str / set_str / erase_all |
| 6.3 | `audio_output.h` | hpf_filter + max98357_bridge | 2 个函数：init / tts_sink |

```c
// 没有 Facade 时，用户要写：
nvs_handle_t h;
nvs_open("xiaozhi", NVS_READONLY, &h);
size_t len = 0;
nvs_get_str(h, "xfyun_appid", NULL, &len);
char *buf = malloc(len + 1);
nvs_get_str(h, "xfyun_appid", buf, &len);
buf[len] = '\0';
nvs_close(h);

// 有 Facade 后：
char appid[32];
secret_store_get_str("xfyun_appid", appid, sizeof(appid), NULL);
```

---

### 7. 模板方法 Template Method（1 处）

**GoF 定义：** 在父类中定义算法骨架，把某些步骤延迟到子类实现。多个实现共享固定流程。

| 项 | 内容 |
|----|------|
| 文件 | [src/tts/tts_hub.h#L30-L35](file:///d:/ESP32S3/xiaozhi/src/tts/tts_hub.h#L30-L35) |
| 固定骨架 | `init` → `speak(text, cb, ctx)` → cb 被多次调产出 PCM → `is_busy` 轮询 |
| 具体实现 | 讯飞 / 百度 / 阿里各自实现这三个步骤 |

```c
typedef struct {
    const char *name;
    esp_err_t (*init)(const void *cfg);           // 步骤 1：初始化
    esp_err_t (*speak)(const char *text, tts_pcm_cb_t cb, void *ctx);  // 步骤 2：合成
    bool      (*is_busy)(void);                    // 步骤 3：完成查询
} tts_driver_t;
```

每个驱动都必须按"init → speak → is_busy"这个固定模板来实现，hub 里的流程控制代码不用知道具体厂商是谁。

---

### 8. 延迟初始化 Lazy Init（2 处）

**GoF 定义：** 资源第一次被访问时才创建，避免启动时的开销。

| # | 模块 | 文件 | 代码证据 |
|---|------|------|---------|
| 8.1 | AI 响应缓冲 | [src/board/app_ai.c](file:///d:/ESP32S3/xiaozhi/src/board/app_ai.c) | `s_rx` 在第一次 `app_ai_chat()` 时才 `heap_caps_malloc(16KB, PSRAM)` |
| 8.2 | AI 配置 | [src/board/app_ai.c](file:///d:/ESP32S3/xiaozhi/src/board/app_ai.c) | `s_ensure_cfg()` —— 第一次调 AI 时才读 NVS 密钥/模型/URL |

```c
static char  *s_rx;  // NULL 表示还没分配
// ...
esp_err_t app_ai_chat(...) {
    if (!s_rx) {                                      // ← 第一次用时才分配
        s_rx = heap_caps_malloc(AI_RX_CAP, MALLOC_CAP_SPIRAM);
    }
    s_ensure_cfg();                                   // ← 第一次用时才读 NVS
    // ...
}
```

---

### 9. 编译期开关 Compile-Time Switch（架构级）

**类型：** 不属于 GoF 23 种，但在嵌入式/固件工程中非常重要。

| 项 | 内容 |
|----|------|
| 文件 | `platformio.ini` 的 `build_flags` |
| 开关数量 | 5 个 |
| 作用 | 关掉的功能**编译期直接裁掉**，不占 Flash / RAM |

```ini
build_flags = 
    -DKWS_ENABLE=0         ; 关闭唤醒识别（省电/调试）
    -DAUDIO_AMP_ENABLE=0   ; 关闭 MAX98357 功放（静音调试）
    -DAUDIO_MIC_ENABLE=1   ; 关闭 INMP441 麦克风
    -DBOARD_HAS_PSRAM      ; 启用 PSRAM 分配
```

**用法（C 代码）：**
```c
#if KWS_ENABLE
    kws_hub_init(...);   // 宏为 0 时这整行不会被编译进去
#endif
```

---

### 10. 状态模式 State（1 处）⚡ 2026-11-20 新增

**GoF 定义：** 对象在内部状态变化时改变其行为，看起来像是换了一个类。

| 项 | 内容 |
|----|------|
| 文件 | [src/bus/main_fsm.h](file:///d:/ESP32S3/xiaozhi/src/bus/main_fsm.h) / [main_fsm.c](file:///d:/ESP32S3/xiaozhi/src/bus/main_fsm.c) |
| 状态数 | 5 个：IDLE / LISTENING / AWAKE / PROCESSING / SPEAKING |
| 触发 | 事件总线的 `EVT_WIFI_GOT_IP` / `EVT_WAKE_WORD` / `EVT_USER_TEXT` / `EVT_AI_REPLY_READY` / `EVT_TTS_DONE` |

**状态转换图：**
```
                     EVT_WIFI_GOT_IP
    IDLE ────────────────────────────────→ LISTENING
      ↑                                       │
      │  WiFi 断开                            │ EVT_WAKE_WORD
      │ (任意状态 → IDLE)                      ↓
      │                                    AWAKE
      │                                       │
      │                                       │ EVT_USER_TEXT
      │                                       ↓
      │                                    PROCESSING
      │                                       │
      │                                       │ EVT_AI_REPLY_READY
      │                                       ↓
      │                                    SPEAKING
      │                                       │
      └──────────── LISTENING ←──────────────┘
                   EVT_TTS_DONE
```

**代码证据 —— 每个状态自动执行 on_enter 动作：**
```c
static void s_transition_to(app_state_t new_state) {
    if (new_state == s_state) return;   // 幂等
    s_state = new_state;
    switch (new_state) {
    case FSM_AWAKE:
        tts_hub_say_async(TTS_PHRASE_WAKE_REPLY);  // "我在请说"
        kws_hub_pause();
        break;
    case FSM_SPEAKING:
        kws_hub_pause();   // 避免录到喇叭自唤醒
        break;
    }
}
```

**改造前 vs 改造后：**
- 改造前：`main.cpp` + `qa_task` 里散落 3 处 `bool prev_paused; if (tts_busy != prev_paused)`
- 改造后：`main_fsm.c` 一个文件集中管理，`bool prev_paused` 消失

---

### 11. 发布-订阅 PubSub（1 处）⚡ 2026-11-20 新增

**GoF 定义：** 消息生产者不直接发给特定消费者，发布到中央事件总线，多个消费者可以订阅自己关心的事件。

| 项 | 内容 |
|----|------|
| 文件 | [src/bus/event_bus.h](file:///d:/ESP32S3/xiaozhi/src/bus/event_bus.h) / [event_bus.c](file:///d:/ESP32S3/xiaozhi/src/bus/event_bus.c) |
| 事件类型数 | 6 个：`WiFi` 2 + `KWS` 2 + `AI` 1 + `TTS` 1 |
| 每个事件最大订阅者 | 8 个 |

**事件类型清单：**
```c
typedef enum {
    EVT_WIFI_GOT_IP,       // WiFi 连上
    EVT_WIFI_DISCONNECTED, // WiFi 断开
    EVT_WAKE_WORD,         // 唤醒词命中
    EVT_USER_TEXT,         // 收到用户指令
    EVT_AI_REPLY_READY,    // AI 回复回来了
    EVT_TTS_DONE,          // TTS 播报完毕
} app_event_t;
```

**订阅矩阵：**

| 事件 | 发布者 | 订阅者 1 | 订阅者 2 |
|------|--------|---------|---------|
| EVT_WIFI_GOT_IP | `main.cpp` WiFi 连接 | `main_fsm`（→ LISTENING）| —— |
| EVT_WIFI_DISCONNECTED | `main.cpp` loop | `main_fsm`（→ IDLE）| —— |
| EVT_WAKE_WORD | `main.cpp` wake_cb | `main_fsm`（→ AWAKE）| —— |
| EVT_USER_TEXT | `main.cpp` wake_cb | `main_fsm`（→ PROCESSING）| `main.cpp`（启 AI 任务）|
| EVT_AI_REPLY_READY | `app_ai.c` qa_task | `main_fsm`（→ SPEAKING）| —— |
| EVT_TTS_DONE | （tts_hub 内部，当前已预留）| `main_fsm`（→ LISTENING）| —— |

**代码证据：**
```c
// 发布者（main.cpp）—— 不知道谁在收
event_bus_publish(EVT_WAKE_WORD, (void *)text);

// 订阅者（main_fsm.c 初始化时）—— 不知道谁在发
event_bus_subscribe(EVT_WAKE_WORD, s_on_kws, NULL);
event_bus_subscribe(EVT_USER_TEXT, s_on_kws, NULL);
```

---

## 设计模式 vs 目录对应图

```
src/
├── main.cpp                        ← 编排层，纯事件转发，无业务逻辑
│
├── bus/                            ⚡ 规则 8 / 规则 9（2026-11-20 新增）
│   ├── event_bus.h/c               │ 发布-订阅（模式 11）
│   └── main_fsm.h/c                │ 状态模式（模式 10）
│
├── tts/                            ← 业务组件层
│   ├── tts_hub.h/c                 │ 策略（模式 2）+ 桥接（模式 3）+ 模板方法（模式 7）+ 观察者（模式 4）+ 隐含接口（模式 0）
│   ├── tts_xfyun.c                 │ 策略实现（讯飞）
│   └── ws_bridge.h/cpp             │ 适配器（模式 1）
│
├── wake/                           ← 业务组件层
│   ├── kws_hub.h/c                 │ 策略（模式 2）+ 观察者（模式 4）+ 隐含接口（模式 0）
│   └── kws_xfyun_iat.c             │ 策略实现（讯飞 RTASR）
│
├── board/                          ← 板级装配层
│   ├── board.h/c                   │ 门面（模式 6）
│   ├── app_ai.c                    │ 延迟初始化（模式 8）
│   ├── app_wake / app_wifi / ...   │ 应用编排薄壳
│   └── secret_store.c              │ 适配器（模式 1）+ 门面（模式 6）
│
├── audio/                          ⚡ 规则 4-SRP（2026-11-20 新增，从 board.c 拆出）
│   ├── hpf_filter.h/c              │ 纯算法，无模式（工具模块）
│   └── audio_output.h/c            │ 适配器（模式 1）
│
├── display/oled_u8g2.cpp           │ 单例（模式 5：g_oled）+ IDisplay 实现
├── flash/w25q128.c                 │ 设备驱动
│
└── lib/                            ← PlatformIO 三方库
    └── DisplayIface/
        └── display_iface.h         ⚡ 接口（模式 0：IDisplay 纯虚基类）+ 工厂模式（预留）
```

---

## 模式数量演进

| 时间 | 模式数 | 主要新增 |
|------|-------|---------|
| 改造前（11-19）| 9 | 适配/策略/桥接/观察者/单例/门面/模板方法/延迟初始化/编译期开关 |
| 改造后（11-20 v1.0）| **11** | + 状态模式（main_fsm）+ 发布-订阅（event_bus）|
| 改造后（11-20 v1.1）| **12** | + 接口 Interface（IDisplay 纯虚基类 + 隐含接口），工厂模式标记为**预留** |

---

## 为什么不引入更多模式

| 模式 | 为什么不用 |
|------|----------|
| ~~工厂 Factory~~ | **预留** —— `IDisplay` 接口已经定义好，未来接 TftTfspi/墨水屏时加 DisplayFactory；当前只有 OledU8g2 一种实现，不值得 |
| 抽象工厂 Abstract Factory | 不需要在运行时切换整套驱动族 |
| 原型 Prototype | 没有需要克隆的对象 |
| 建造者 Builder | 当前没有需要链式组装的复杂对象（HTTP config 用结构体字面量足够）|
| 代理 Proxy | WiFi 已经是直连，没有"远程访问对象"的需求 |
| 享元 Flyweight | 没有大量重复的内部状态对象 |
| 迭代器 Iterator | 没有需要遍历的自定义集合 |
| 中介者 Mediator | event_bus + main_fsm 组合已经承担了中介者角色，不需要另加一层 |

---

## 修改历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v1.0 | 2026-11-20 | 初版，对应项目改造后状态（11 种模式）|
| v1.1 | 2026-11-20 | 新增"接口 Interface"（模式 0）—— IDisplay 纯虚基类；工厂模式标记为**预留**（未来接 TftTfspi 时加 DisplayFactory）；目录图补 lib/DisplayIface |
