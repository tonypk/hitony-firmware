#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <vector>

/**
 * @brief 任务管理器 - 统一管理所有应用任务
 *
 * 负责：
 * - 创建和管理所有应用任务
 * - 双核任务分配
 * - 任务优先级管理
 * - 任务监控和统计
 */
class TaskManager {
public:
    // 任务定义
    struct TaskDef {
        const char* name;              // 任务名称
        TaskFunction_t func;           // 任务函数
        uint32_t stack_size;           // 栈大小（字）
        UBaseType_t priority;          // 优先级
        BaseType_t core_id;            // 核心ID（0/1/-1=任意）
        void* param;                   // 任务参数
        TaskHandle_t* handle;          // 任务句柄指针
    };

    // 任务统计信息
    struct TaskStats {
        char name[16];
        UBaseType_t priority;
        uint32_t stack_size;
        uint32_t stack_free;      // 剩余栈空间
        uint32_t runtime;         // 运行时间（ms）
        float cpu_usage;          // CPU使用率（%）
    };

    static TaskManager& instance() {
        static TaskManager inst;
        return inst;
    }

    /**
     * @brief 初始化任务管理器
     */
    bool init();

    /**
     * @brief 创建任务
     */
    bool create_task(const TaskDef& def);

    /**
     * @brief 批量创建任务
     */
    bool create_tasks(const std::vector<TaskDef>& defs);

    /**
     * @brief 获取任务统计信息
     */
    std::vector<TaskStats> get_task_stats();

    /**
     * @brief 打印任务统计（用于调试）
     */
    void print_task_stats();

    /**
     * @brief 监控任务栈使用情况
     */
    void monitor_stack_usage();

    /**
     * @brief 获取系统CPU使用率
     */
    void get_cpu_usage(float& core0_usage, float& core1_usage);

private:
    TaskManager() = default;
    std::vector<TaskHandle_t> tasks_;
};

// ============================================================================
// 全局任务定义（2任务架构）
// ============================================================================

void audio_main_task(void* arg);      // Audio Task (Core 1) - 48KB栈
void main_control_task(void* arg);    // Main Control Task (Core 0) - 12KB栈

// ============================================================================
// 全局队列定义（2任务架构）
// ============================================================================

// 旧架构保留队列（仅AdvancedAFE使用）
extern QueueHandle_t g_afe_output_queue;       // AFE → audio_main_task

// 系统事件组
extern EventGroupHandle_t g_app_event_group;   // WiFi/WebSocket全局事件

// 事件位定义
#define EVENT_WIFI_CONNECTED      BIT0
#define EVENT_WIFI_DISCONNECTED   BIT1
#define EVENT_WS_CONNECTED        BIT2
#define EVENT_WS_DISCONNECTED     BIT3
#define EVENT_WAKE_DETECTED       BIT4
#define EVENT_VAD_START           BIT5
#define EVENT_VAD_END             BIT6
#define EVENT_TTS_START           BIT7
#define EVENT_TTS_END             BIT8
#define EVENT_TOUCH_PRESSED       BIT9
#define EVENT_TOUCH_RELEASED      BIT10
#define EVENT_RECORDING_START     BIT11
#define EVENT_RECORDING_END       BIT12

// ============================================================================
// PCM RingBuffer 和 Memory Pool 定义
// ============================================================================

// PCM RingBuffer（16KB PSRAM，零拷贝循环缓冲区）
typedef struct {
    int16_t* buffer;           // 16KB PSRAM buffer
    size_t capacity;           // 8K samples (16KB / 2 bytes)
    volatile size_t write_pos; // 写指针（Audio Task）
    volatile size_t read_pos;  // 读指针（Main Task）
    SemaphoreHandle_t mutex;   // 并发访问保护
} pcm_ringbuffer_t;

extern pcm_ringbuffer_t g_pcm_ringbuffer;

// 参考音频 RingBuffer (AEC用，存储扬声器播放的PCM)
extern pcm_ringbuffer_t g_ref_ringbuffer;

// MIC1 RingBuffer (双麦克风)
extern pcm_ringbuffer_t g_mic1_ringbuffer;

// 固定内存池（5个池，78KB PSRAM总计）
typedef enum {
    POOL_S_64 = 0,   // 64B × 32块 = 2KB（消息结构体）
    POOL_S_128,      // 128B × 32块 = 4KB
    POOL_S_256,      // 256B × 32块 = 8KB（Opus包）
    POOL_L_2K,       // 2KB × 16块 = 32KB（音频缓冲）
    POOL_L_4K,       // 4KB × 8块 = 32KB（大音频缓冲）
    POOL_COUNT
} pool_type_t;

typedef struct {
    void* memory;          // 预分配PSRAM块
    uint32_t block_size;   // 每块大小
    uint32_t block_count;  // 总块数
    uint32_t free_bitmap;  // 空闲块位图（最大32块）
    SemaphoreHandle_t mutex;
} memory_pool_t;

extern memory_pool_t g_memory_pools[POOL_COUNT];

// ============================================================================
// Audio Task 与 Main Task 通信接口
// ============================================================================

// Audio控制命令（Main Task → Audio Task）
typedef enum {
    AUDIO_CMD_START_RECORDING,
    AUDIO_CMD_STOP_RECORDING,
    AUDIO_CMD_START_PLAYBACK,
    AUDIO_CMD_STOP_PLAYBACK,
} audio_cmd_t;

extern QueueHandle_t g_audio_cmd_queue;         // Main → Audio命令（长度4）
extern QueueHandle_t g_opus_tx_queue;           // Audio → Main编码包（长度8）
extern QueueHandle_t g_opus_playback_queue;     // Main → Audio播放包（长度32）
extern EventGroupHandle_t g_audio_event_bits;   // Audio → Main事件

// 音频事件位（Audio Task → Main Task）
#define AUDIO_EVENT_WAKE_DETECTED  BIT0
#define AUDIO_EVENT_VAD_START      BIT1
#define AUDIO_EVENT_VAD_END        BIT2
#define AUDIO_EVENT_ENCODE_READY   BIT3
#define AUDIO_EVENT_TOUCH_WAKE     BIT4   // 触摸唤醒（不受SPEAKING/MUSIC过滤）

// FSM事件类型（Main Task内部）
typedef enum {
    FSM_EVENT_WAKE_DETECTED,
    FSM_EVENT_RECORDING_START,
    FSM_EVENT_RECORDING_END,
    FSM_EVENT_TTS_START,
    FSM_EVENT_TTS_END,
    FSM_EVENT_TTS_ABORT,         // User interrupted TTS (re-wake during playback)
    FSM_EVENT_WS_CONNECTED,
    FSM_EVENT_WS_DISCONNECTED,
    FSM_EVENT_ERROR,
} fsm_event_type_t;

typedef struct {
    fsm_event_type_t event;
    union {
        char wake_word[32];
        char error_msg[128];
    } data;
} fsm_event_msg_t;

extern QueueHandle_t g_fsm_event_queue;  // FSM事件队列（长度16）

// WebSocket原始消息队列（WS事件回调 → Main Loop）
// 事件回调只做memcpy+push，所有解析在Main Loop中处理
enum {
    WS_MSG_BINARY = 0,       // 二进制帧（TTS Opus包）
    WS_MSG_TEXT = 1,          // 文本帧（JSON控制消息）
    WS_MSG_CONNECTED = 2,    // WebSocket已连接
    WS_MSG_DISCONNECTED = 3, // WebSocket断开
};

typedef struct {
    uint8_t* data;       // 数据指针（pool分配，CONNECTED/DISCONNECTED时为NULL）
    uint16_t len;        // 数据长度
    uint8_t msg_type;    // WS_MSG_*
} ws_raw_msg_t;

extern QueueHandle_t g_ws_rx_queue;      // WS原始消息队列（长度48）

// ============================================================================
// 消息定义（2任务架构）
// ============================================================================

// 音频数据消息
typedef struct {
    int16_t* data;       // 音频数据指针（需要释放）
    size_t samples;      // 样本数
    int channels;        // 声道数
    uint32_t timestamp;  // 时间戳
} audio_data_msg_t;

// Opus包消息
typedef struct {
    uint8_t* data;       // Opus数据指针（需要释放）
    size_t len;          // 数据长度
    uint32_t timestamp;  // 时间戳
} opus_packet_msg_t;

// ============================================================================
// 辅助函数
// ============================================================================

/**
 * @brief 初始化所有全局队列
 */
bool init_global_queues();

/**
 * @brief 创建所有应用任务
 */
bool create_all_tasks();

/**
 * @brief 暂停音频任务（释放内存用于配网模式）
 */
void suspend_audio_tasks();

/**
 * @brief 初始化先进的AFE音频前端处理器
 */
bool init_advanced_afe();

/**
 * @brief 分配音频数据消息（从PSRAM）
 */
audio_data_msg_t* alloc_audio_msg(size_t samples, int channels);

/**
 * @brief 释放音频数据消息
 */
void free_audio_msg(audio_data_msg_t* msg);

/**
 * @brief 分配Opus包消息（从PSRAM）
 */
opus_packet_msg_t* alloc_opus_msg(size_t len);

/**
 * @brief 释放Opus包消息
 */
void free_opus_msg(opus_packet_msg_t* msg);

/**
 * @brief 初始化PCM RingBuffer
 */
bool ringbuffer_init(pcm_ringbuffer_t* rb, size_t capacity);

/**
 * @brief 零拷贝写入RingBuffer（Audio Task使用）
 */
size_t ringbuffer_write(pcm_ringbuffer_t* rb, const int16_t* data, size_t samples);

/**
 * @brief 零拷贝读取RingBuffer（Main Task使用）
 */
size_t ringbuffer_read(pcm_ringbuffer_t* rb, int16_t* out, size_t samples);

/**
 * @brief 查询RingBuffer可读样本数
 */
size_t ringbuffer_data_available(pcm_ringbuffer_t* rb);

/**
 * @brief 重置RingBuffer读写指针
 */
void ringbuffer_reset(pcm_ringbuffer_t* rb);

/**
 * @brief 初始化固定内存池
 */
bool init_memory_pools();

/**
 * @brief 从内存池分配块
 */
void* pool_alloc(pool_type_t type);

/**
 * @brief 归还内存池块
 */
void pool_free(pool_type_t type, void* ptr);

/**
 * @brief 根据数据大小释放pool块（用于ws_raw_msg_t释放）
 */
void pool_free_by_size(void* ptr, size_t len);

/**
 * @brief 打印内存池使用统计
 */
void pool_print_stats();
