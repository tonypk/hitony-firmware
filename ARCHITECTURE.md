# EchoEar 固件架构设计文档

## 概述

基于ESP32-S3的顶级智能音箱固件，充分利用双核、8MB PSRAM和ESP-SR算法，实现业界领先的语音交互体验。

## 硬件平台

- **主控**: ESP32-S3 (Dual Core Xtensa LX7 @ 240MHz)
- **内存**: 8MB PSRAM + 512KB SRAM
- **音频**: ES7210 (双麦ADC) + ES8311 (单声道DAC)
- **显示**: ST77916 LCD (480x480) + CST816S 触摸
- **连接**: WiFi 802.11n

## 核心架构

### 1. 双核任务分配

#### Core 0 - 协议处理核心
- WiFi/网络协议栈
- WebSocket客户端通信
- HTTP配网服务器
- 状态机管理
- UI渲染和更新
- 事件分发

#### Core 1 - 音频处理核心
- 音频采集 (I2S + DMA)
- AFE音频前端处理
- 唤醒词检测
- Opus编解码
- 音频混音
- 音频播放 (I2S + DMA)

### 2. 任务优先级设计

```
优先级    任务名称              核心   栈大小   功能
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
20       audio_input_task      C1     8KB      音频采集（最高）
19       audio_output_task     C1     8KB      音频播放
18       afe_process_task      C1    16KB      AFE处理
16       wake_detect_task      C1    12KB      唤醒词检测
15       audio_mixer_task      C1     8KB      音频混音
14       opus_decode_task      C1    16KB      TTS解码
13       opus_encode_task      C1    16KB      ASR编码
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
8        websocket_task        C0    16KB      WebSocket通信
6        state_machine_task    C0     8KB      状态管理
3        ui_update_task        C0     8KB      UI更新
2        led_control_task      C0     4KB      LED动画
1        heartbeat_task        C0     4KB      系统心跳
```

### 3. 音频处理流水线

```
┌─────────────────────────────────────────────────────────┐
│                    音频输入流水线                        │
└─────────────────────────────────────────────────────────┘

双麦克风 (ES7210)
    ↓ I2S DMA (16kHz, 16bit, 2ch)
audio_input_task
    ↓ queue (2048 samples × 2ch)
afe_process_task
├─ AEC (回声消除)
├─ NS  (噪声抑制)
├─ AGC (自动增益)
└─ Beamforming (波束形成)
    ↓ queue (960 samples, processed)
┌────────────────┬─────────────────┐
│  wake_detect   │   VAD检测       │
│  (唤醒词)      │   (语音活动)    │
└────────────────┴─────────────────┘
         ↓ 唤醒成功
   opus_encode_task
         ↓ Opus packets
   websocket_task (上传)


┌─────────────────────────────────────────────────────────┐
│                    音频输出流水线                        │
└─────────────────────────────────────────────────────────┘

websocket_task (接收TTS)
    ↓ Opus packets
opus_decode_task
    ↓ PCM samples (16kHz, 16bit, 1ch)
audio_mixer_task
├─ 音量控制
├─ 提示音混合
└─ 淡入淡出
    ↓ Mixed PCM
audio_output_task
    ↓ I2S DMA
ES8311 DAC → 扬声器
```

### 4. 消息队列通信

所有任务间通信通过FreeRTOS队列，避免共享内存和锁：

```cpp
// 音频流队列
QueueHandle_t audio_input_queue;      // 原始音频 → AFE
QueueHandle_t afe_output_queue;       // AFE处理后 → 唤醒词/编码
QueueHandle_t audio_encode_queue;     // 编码前 → Opus编码
QueueHandle_t audio_decode_queue;     // 解码后 → 混音
QueueHandle_t audio_output_queue;     // 混音后 → 播放

// 网络队列
QueueHandle_t ws_tx_queue;            // 发送队列（Opus包）
QueueHandle_t ws_rx_queue;            // 接收队列（Opus包）

// 控制队列
QueueHandle_t wake_event_queue;       // 唤醒事件
QueueHandle_t state_event_queue;      // 状态事件
QueueHandle_t ui_cmd_queue;           // UI命令
```

### 5. 内存管理

#### PSRAM分配策略 (8MB)
```cpp
// 启动时静态分配大缓冲区
struct app_buffers_t {
    // 音频缓冲区（~1.2MB）
    uint8_t audio_input[256*1024];    // 256KB
    uint8_t audio_output[256*1024];   // 256KB
    uint8_t afe_work[512*1024];       // 512KB
    uint8_t opus_work[128*1024];      // 128KB
    uint8_t audio_ring[1*1024*1024];  // 1MB环形缓冲

    // 网络缓冲区（128KB）
    uint8_t ws_tx[64*1024];
    uint8_t ws_rx[64*1024];
} __attribute__((section(".spiram")));

// 剩余 ~6.6MB PSRAM用于动态分配
```

#### DMA内存分配策略
```cpp
// LVGL双缓冲（需要DMA兼容内存）
lv_color_t* buf1 = heap_caps_malloc(480*40*2, MALLOC_CAP_DMA);
lv_color_t* buf2 = heap_caps_malloc(480*40*2, MALLOC_CAP_DMA);

// I2S DMA缓冲区（需要内部RAM）
uint8_t* i2s_buf = heap_caps_malloc(8192, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
```

### 6. 状态机设计

```cpp
typedef enum {
    STATE_BOOT,              // 启动中
    STATE_PROVISIONING,      // 配网中
    STATE_WIFI_CONNECTING,   // WiFi连接中
    STATE_WIFI_CONNECTED,    // WiFi已连接
    STATE_WS_CONNECTED,      // WebSocket已连接
    STATE_IDLE,              // 空闲（等待唤醒）
    STATE_LISTENING,         // 录音中
    STATE_PROCESSING,        // 云端处理中
    STATE_SPEAKING,          // TTS播放中
    STATE_ERROR,             // 错误状态
} app_state_t;

typedef enum {
    EVENT_WIFI_CONNECTED,
    EVENT_WIFI_DISCONNECTED,
    EVENT_WS_CONNECTED,
    EVENT_WS_DISCONNECTED,
    EVENT_WAKE_DETECTED,
    EVENT_VAD_START,
    EVENT_VAD_END,
    EVENT_TTS_START,
    EVENT_TTS_END,
    EVENT_TOUCH_PRESSED,
    EVENT_ERROR,
} app_event_t;
```

## 先进功能

### 1. AFE音频前端处理

使用ESP-SR的AFE算法实现：
- **AEC (回声消除)**: 消除扬声器回放音频
- **NS (噪声抑制)**: 压制环境噪声
- **AGC (自动增益)**: 自动调整麦克风增益
- **Beamforming (波束形成)**: 双麦克风定向拾音
- **VAD (语音活动检测)**: 检测语音起止

### 2. 多唤醒词支持

支持同时加载多个唤醒词模型：
- "你好小智" (主唤醒词)
- "嗨小智" (备用唤醒词)
- 自定义唤醒词

### 3. 低延迟音频

- **音频输入延迟**: < 50ms (采集 → AFE → 编码)
- **音频输出延迟**: < 100ms (接收 → 解码 → 播放)
- **总往返延迟**: < 500ms (包含网络)

### 4. 智能音量控制

- 自适应音量调节（根据环境噪声）
- 淡入淡出效果
- 提示音混合（唤醒提示、错误提示）

### 5. 流畅UI体验

- 60FPS动画
- 异步UI更新（消息队列）
- 眼睛动画、表情切换
- 触摸反馈

### 6. 断线重连

- WiFi自动重连（指数退避）
- WebSocket心跳保活
- 断线后自动恢复状态

### 7. OTA升级

- 支持HTTP/HTTPS OTA
- 双分区A/B升级
- 升级失败自动回滚

## 性能指标

### CPU使用率
- Core 0: ~40% (网络 + UI)
- Core 1: ~60% (音频处理)
- 峰值: ~80% (TTS播放时)

### 内存使用
- SRAM: ~300KB / 512KB
- PSRAM: ~2MB / 8MB (可扩展到6MB+)

### 功耗
- 空闲: ~200mA @ 3.3V
- 唤醒词检测: ~250mA
- 录音/播放: ~300mA
- WiFi传输: ~350mA

### 延迟指标
- 唤醒响应: < 300ms
- 音频上传: < 50ms
- TTS播放: < 100ms
- 总往返: < 500ms

## 开发规范

### 代码风格
- C++ 标准: C++17
- 命名: snake_case (函数/变量), PascalCase (类)
- 日志: 使用ESP_LOG系列宏
- 错误处理: 返回值检查 + 日志

### 任务设计原则
1. **单一职责**: 每个任务只做一件事
2. **无阻塞**: 避免长时间阻塞，使用队列
3. **优先级**: 音频任务 > 网络任务 > UI任务
4. **核心绑定**: 音频任务绑定Core1，网络绑定Core0
5. **栈大小**: 根据实际测量设置，预留20%余量

### 内存管理原则
1. **静态优先**: 启动时预分配大缓冲区
2. **PSRAM优先**: 大缓冲区使用PSRAM
3. **DMA限制**: DMA缓冲区必须在内部RAM
4. **避免碎片**: 使用内存池或环形缓冲

### 线程安全原则
1. **消息队列**: 任务间通信优先使用队列
2. **事件组**: 用于状态标志
3. **互斥锁**: 仅用于共享资源（如LVGL）
4. **原子操作**: 简单标志使用std::atomic

## 测试验证

### 单元测试
- AFE算法测试
- Opus编解码测试
- 状态机测试
- 内存泄漏测试

### 集成测试
- 唤醒词准确率测试
- 音频质量测试
- 网络稳定性测试
- 长时间运行测试

### 性能测试
- CPU使用率监控
- 内存使用监控
- 延迟测试
- 功耗测试

## 未来扩展

1. **边缘AI**: 本地ASR识别（ESP-SR）
2. **多语言**: 支持英语、日语等
3. **蓝牙**: 添加蓝牙音箱功能
4. **Matter**: 支持Matter智能家居协议
5. **语音克隆**: 自定义TTS音色
6. **多轮对话**: 上下文记忆
