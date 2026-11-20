# 全局设计模式强制规则 v2.0（跨技术栈通用）

> 本文件为**永久全局规则**，适用于所有类型的项目：嵌入式 C/C++、Web 前端、Python、Java 后端、脚本工具等。
> 任何代码改动必须符合以下规则。
> 创建日期：2026-11-20
> 更新日期：2026-11-20

---

## 规则 1：分层架构（强制）

### 通用五层结构（按项目类型裁剪，层数可多可少，但上层不能依赖下层的具体实现）

```
┌─────────────────────────────────────┐
│  应用编排层                          │  只管串流程，不含核心算法/业务逻辑
│  main / AppController / router       │
├─────────────────────────────────────┤
│  业务组件层                          │  核心能力，用策略模式解耦外部依赖
│  service / usecase / hub             │
├─────────────────────────────────────┤
│  驱动/适配层                          │  桥接外部系统（数据库、三方API、硬件）
│  adapter / bridge / repository       │
├─────────────────────────────────────┤
│  基础设施层                          │  具体实现（GPIO、SQL、HTTP客户端）
│  driver / impl / client              │
├─────────────────────────────────────┤
│  第三方库                             │  不改源码，一律通过适配层包装
└─────────────────────────────────────┘
```

### 铁律

1. **上层可以依赖下层的抽象（接口/头文件），下层绝对不能依赖上层**
2. **同层模块之间只能通过公开接口交互**，不能互相访问 `private` / `static` / 内部变量
3. **每层只做这一层该做的事**——编排层不写算法，驱动层不写业务判断

### 按项目类型的层名映射

| 层 | 嵌入式 C/C++ | Web 前端 (Vue/React) | Python/Java 后端 |
|----|-------------|---------------------|-----------------|
| 应用编排 | main.c / app_*.c | router + page 组件 | Controller |
| 业务组件 | *_hub / *_manager | store + composables | Service |
| 驱动适配 | *_bridge / *_driver | API client + adapter | Repository / DAO |
| 基础设施 | ESP-IDF driver | fetch / axios | JDBC / HTTP client |

---

## 规则 2：SOLID 原则（强制，永不放松）

| 原则 | 强制要求 | 反例 |
|------|---------|------|
| **SRP 单一职责** | 一个模块/类/文件只做一件事 | board.c 同时做引脚+init+滤波+输出；Vue 组件超过 300 行 |
| **OCP 开放封闭** | 新增功能通过扩展（加文件/加类）而非修改原有代码 | ✅ 策略模式加新驱动；❌ 改 hub 的 switch-case 加厂商 |
| **DIP 依赖倒置** | 依赖抽象接口，不依赖具体实现 | ✅ `interface Speaker { speak() }` 注入；❌ 直接在 Service 里 `new Max98357()` |
| **ISP 接口隔离** | 暴露的接口只包含调用方需要的方法 | ✅ 分 `tts_hub_say_fixed()` 和 `tts_hub_speak_any()`；❌ 一个大接口塞满所有函数 |
| **LSP 里氏替换** | 同接口不同实现的行为必须一致 | 讯飞/百度 TTS 都产出 PCM；MySQL/Postgres Repository 都返回相同 Domain 对象 |

---

## 规则 3：必须使用的设计模式

### 3.1 任何项目都应该有（强制）

| 模式 | 何时必须用 | 各语言示例 |
|------|-----------|-----------|
| **适配器 Adapter** | 第三方库接口不匹配你的系统 | C: `*_bridge.h/cpp`；Java: `XxxAdapter implements MyInterface`；TS: `xxxGateway.ts` |
| **策略 Strategy** | 同一能力有多种实现可选 | C: 函数表 `xxx_driver_t`；Java: `XxxStrategy interface` + `XxxStrategyA/B`；TS: 联合类型 + switch |
| **桥接 Bridge** | 能力本身与具体实现分离 | C: 回调注入；Java: 构造器注入；TS: 依赖注入容器 |
| **状态模式 State** | 有状态变化检测代码（见规则 5） | 任何语言都适用，见规则 5 |
| **发布-订阅 PubSub** | 一个事件触发多个模块动作（见规则 6） | 任何语言都适用，见规则 6 |
| **依赖注入 DI** | 模块 A 需要模块 B 的服务 | C: 函数表/回调注入；Java/Spring: `@Autowired`；TS: Inversify/NestJS |

### 3.2 可选但推荐（按项目类型）

| 模式 | 适用场景 |
|------|---------|
| 工厂 Factory | 有多种实现需要动态选择（如根据配置创建不同的 Client） |
| 单例 Singleton | 全局唯一的资源（数据库连接池、硬件控制器）—— 慎用，避免隐藏依赖 |
| 构建器 Builder | 创建复杂对象时（如 HTTP Request、AI Prompt） |
| 观察者 Observer | 单一订阅者场景（如 KWS 回调），用函数即可，不需要完整 PubSub |
| 模板方法 Template Method | 多个算法步骤固定但某些步骤可定制（如音频管线的 pipeline 各阶段） |

---

## 规则 4：禁止的反模式（红线，任何项目都不能碰）

| # | 禁止项 | 替代方案 |
|---|--------|---------|
| 1 | **密钥/Token/密码硬编码** 在源码文件里 | 环境变量 / 配置文件（gitignore）/ 密钥管理服务 / NVS |
| 2 | **回调里阻塞等待**（`while + sleep/delay` 循环） | 回调只发信号/事件，后续流程放异步任务/事件循环 |
| 3 | **模块反向依赖上层**（下层 `import` 上层头文件/模块） | 重组层 / 加桥接层 / 依赖反转 |
| 4 | **一个文件/类过大**（嵌入式 .c > 500 行，Java > 300 行，Vue > 300 行） | 拆分成职责单一的小文件/小组件 |
| 5 | **全局变量跨模块读写** 作为通信手段 | 依赖注入 / 函数参数传递 / 事件总线 / 消息队列 |
| 6 | **启动流程里阻塞大循环**（`setup()` 里 `while(1)`；Vue `created()` 里长时间同步请求） | 启动必须快速返回，重初始化放后台任务/async |
| 7 | **try-catch / if-err 把所有错误都吞掉** 不记录 | 错误必须 log / 上报，可恢复的要 fallback，不可恢复的要 abort |
| 8 | **日志前缀不统一** | 统一用小写模块名：`"main"` `"service.tts"` `"db.user"` |
| 9 | **魔法数字/字符串** 散在代码里 | 定义为具名常量 / enum / 配置项 |

---

## 规则 5：状态模式（强制）

### 何时必须引入

> 当你写出以下任何一种代码时 → 就是状态模式的信号：
>
> - `bool prev_xxx = false; if (xxx != prev_xxx) { ... }` —— C / Python / JS
> - `if (state == "A") ... else if (state == "B") ...` —— 多个分支判断当前状态
> - `status` 字段在多个地方被 `switch-case` 处理

### 强制要求

1. 一个业务流程的所有状态必须用 **enum / 联合类型** 集中定义
2. 状态转换通过**显式转换函数**进行，不能在业务代码里直接赋值 `state = NEW_STATE`
3. 每个状态可选有 **进入动作**（on_enter）和 **退出动作**（on_exit）
4. 转换函数必须**幂等**（相同状态重复调用不产生副作用）

### 各语言写法示例

**C 语言**：
```c
typedef enum { FSM_LISTENING, FSM_AWAKE, FSM_PROCESSING, FSM_SPEAKING } app_state_t;
void main_fsm_transition(app_state_t new_state);
```

**Java / Python**：
```java
public enum AppState { LISTENING, AWAKE, PROCESSING, SPEAKING }
void transitionTo(AppState newState);
```

**TypeScript**：
```typescript
type AppState = 'listening' | 'awake' | 'processing' | 'speaking'
function transitionTo(newState: AppState): void
```

---

## 规则 6：发布-订阅模式（强制）

### 何时必须引入

> 当你写出以下任何一种代码时 → 就是事件总线的信号：
>
> - 一个事件（如 "登录成功" "WiFi 连上"）触发 **3 个以上模块** 的动作
> - 你想在不修改事件发布者的情况下，加一个新的响应模块
> - 模块之间通过"主控制器"手动串调用（controller 成了上帝对象）

### 强制要求

1. 全局一个事件总线（EventBus / EventEmitter），所有模块共用
2. 发布者**不知道谁在订阅**，只管 `publish(EVENT_X, data)`
3. 订阅者**不知道谁在发布**，只管 `subscribe(EVENT_X, handler)`
4. 事件命名格式：`entity.action`（如 `user.login`、`wifi.got_ip`、`tts.done`）

### 各语言写法示例

**C 语言**：阻塞式事件总线（单线程足够）
```c
event_bus_subscribe(EVT_WIFI_GOT_IP, my_handler, NULL);
event_bus_publish(EVT_WIFI_GOT_IP, NULL);
```

**Node.js / TypeScript**：EventEmitter
```typescript
const bus = new EventEmitter()
bus.on('wifi.got_ip', handler)
bus.emit('wifi.got_ip')
```

**Python**：简单回调字典
```python
bus.subscribe('wifi.got_ip', handler)
bus.publish('wifi.got_ip')
```

**Java/Spring**：`ApplicationEventPublisher`
```java
eventPublisher.publishEvent(new WifiConnectedEvent())
```

### 与状态机配合使用

```
事件总线（解耦）              状态机（控流程）
  谁发/谁收 ←──→         现在在哪/下一步去哪
     ↓                        ↓
KWS publish('wake.word') ──→ main_fsm transitionTo(AWAKE)
                                 ↓
                           FSM 内部调 TTS/OLED/KWS
                           （这些模块不需要知道 KWS，
                            也不需要知道 FSM 之外谁在订阅）
```

---

## 规则 7：命名规范（强制，所有语言统一思路）

### 通用原则

| 元素 | 格式 | 示例 |
|------|------|------|
| 文件名 | 与包含的主要类/模块同名 | `tts_hub.c` / `TtsHub.java` / `ttsHub.ts` |
| 模块名 | 有意义的名词，不用缩写除非行业通用 | ✅ `tts_hub` ❌ `tthb` |
| 函数/方法 | **动词开头** | ✅ `init_tts()` / `get_user()` / `publishEvent()` |
| 变量 | **名词**，布尔变量用 is/has/can 前缀 | `is_connected` / `has_key` / `can_retry` |
| 常量/枚举值 | **UPPER_SNAKE_CASE** | `MAX_RETRY_COUNT` / `STATE_LISTENING` |
| 类型名 | **PascalCase**（大驼峰）| `tts_provider_t` / `AppState` / `WifiEvent` |

### 各语言差异

| 语言 | 函数/方法 | 变量 | 类/类型 |
|------|----------|------|--------|
| C | `snake_case` | `s_` 前缀静态，`g_` 前缀全局 | `xxx_t` |
| C++ | `snake_case` 或 `camelCase` | 同 C | `PascalCase` |
| Java/Kotlin | `camelCase` | `camelCase` | `PascalCase` |
| Python | `snake_case` | `snake_case` | `PascalCase` |
| TS/JS | `camelCase` | `camelCase` | `PascalCase` |

---

## 规则 8：模块初始化顺序（必须文档化）

每个模块/组件的文档或头文件顶部必须注释**前置条件**：

**C 语言**（头文件注释）：
```c
/**
 * 前置条件：
 *   - 必须在 WiFi 连上 + configTime() SNTP 对时之后调用
 *   - 提前调用会 ESP_ERROR_CHECK abort
 */
void board_init_tts(void);
```

**Java / Python**（类注释或方法注释）：
```java
/**
 * Requires: database connection pool initialized,
 *           configuration loaded from application.yml
 */
public UserService(DatabasePool pool) { ... }
```

**关键原则**：如果模块 A 依赖模块 B 先初始化，**必须有文档**，不能靠读代码猜。

---

## 规则 9：测试/验证（强制，按项目类型）

| 项目类型 | 最低要求 |
|---------|---------|
| 嵌入式 | 烧录后能跑起来 = 验证通过（硬件模拟成本高，代码评审要严） |
| 后端 API | 至少有接口测试；关键 Service 要有单元测试 |
| 前端 | 至少手动走通核心路径；有条件加 E2E 测试 |
| Python 脚本 | 加 `if __name__ == "__main__":` 自检代码 |

**原则**：任何改动必须至少验证过"能跑"再提交。

---

# 附录 A：嵌入式 C/C++ 补充规则

1. **PSRAM 优先**：>1KB 动态分配用 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`
2. **FreeRTOS 任务栈**：= 本地变量 + 调用链深度 × 512 字节，不确定就留大
3. **外设独占**：I2S/SPI/UART 同一外设只能 init 一次，board 层统一管
4. **DMA 缓冲**：必须静态或 PSRAM，不能分配在内部 RAM heap
5. **delay() 限制**：只能在 setup() 或低优先级任务，主循环/回调里禁止
6. **ESP_ERROR_CHECK**：只用在"初始化失败必须 abort"的路径，可恢复错误用 `if + log + fallback`

---

# 附录 B：Web 前端补充规则

1. **组件职责单一**：每个 Vue/React 组件只做一件事，超过 300 行必须拆
2. **状态上提/下发**：组件自己的状态自己管，共享状态放 store/event bus
3. **API 统一走适配层**：所有 fetch/axios 调用封装在 `api/` 目录，组件不直接发请求
4. **样式隔离**：不影响全局；需要全局样式单独放

---

# 附录 C：Python 补充规则

1. **类型注解**：函数签名必须有 type hint，复杂返回值用 `TypedDict` / `dataclass`
2. **配置外置**：`.env` / `config.yaml`，敏感信息不进代码
3. **异常处理**：`try-except` 必须指定具体异常类型，不能 `except:` 裸捕获
4. **依赖最小化**：新库引入前先评估"能用标准库实现吗？"

---

# 修改历史

| 版本 | 日期 | 变更 |
|------|------|------|
| v1.0 | 2026-11-20 | 初始 9 条（嵌入式专用） |
| v2.0 | 2026-11-20 | 重构为跨技术栈通用规则，分层架构映射到各语言，新增状态模式/发布-订阅强制要求，嵌入式/前端/Python 补充规则改为附录 |
