#include "task_manager.h"
#include <esp_log.h>

static const char* TAG = "app_queues";

// ============================================================================
// 全局队列定义（2任务架构）
// ============================================================================

// 旧13任务架构保留（仅AdvancedAFE使用）
QueueHandle_t g_afe_output_queue = nullptr;

// 系统事件组
EventGroupHandle_t g_app_event_group = nullptr;

// Audio Task 与 Main Task 通信
QueueHandle_t g_audio_cmd_queue = nullptr;
QueueHandle_t g_opus_tx_queue = nullptr;
QueueHandle_t g_opus_playback_queue = nullptr;
EventGroupHandle_t g_audio_event_bits = nullptr;
QueueHandle_t g_fsm_event_queue = nullptr;
QueueHandle_t g_ws_rx_queue = nullptr;

bool init_global_queues() {
    ESP_LOGI(TAG, "Initializing global queues (2-task architecture)...");

    // AFE输出队列（仅AdvancedAFE使用）
    g_afe_output_queue = xQueueCreate(64, sizeof(audio_data_msg_t*));
    if (!g_afe_output_queue) {
        ESP_LOGE(TAG, "Failed to create afe_output_queue");
        return false;
    }

    // 系统事件组
    g_app_event_group = xEventGroupCreate();
    if (!g_app_event_group) {
        ESP_LOGE(TAG, "Failed to create app_event_group");
        return false;
    }

    // 初始化PCM RingBuffer（16KB PSRAM — 实际只需几百samples缓冲）
    if (!ringbuffer_init(&g_pcm_ringbuffer, 8192)) {  // 8K samples = 16KB（节省240KB）
        ESP_LOGE(TAG, "Failed to init PCM RingBuffer");
        return false;
    }

    // 初始化参考音频 RingBuffer (AEC用，4096 samples = 256ms @ 16kHz)
    if (!ringbuffer_init(&g_ref_ringbuffer, 4096)) {
        ESP_LOGE(TAG, "Failed to init Reference RingBuffer");
        return false;
    }

    // MIC1 RingBuffer (双麦克风，与 g_ref_ringbuffer 同容量)
    if (!ringbuffer_init(&g_mic1_ringbuffer, 4096)) {
        ESP_LOGE(TAG, "Failed to init MIC1 RingBuffer");
        return false;
    }

    // 初始化固定内存池（44KB PSRAM）
    if (!init_memory_pools()) {
        ESP_LOGE(TAG, "Failed to init memory pools");
        return false;
    }

    // Audio与Main通信队列（2任务架构）
    g_audio_cmd_queue = xQueueCreate(4, sizeof(audio_cmd_t));
    if (!g_audio_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create audio_cmd_queue");
        return false;
    }

    g_opus_tx_queue = xQueueCreate(8, sizeof(opus_packet_msg_t*));
    if (!g_opus_tx_queue) {
        ESP_LOGE(TAG, "Failed to create opus_tx_queue");
        return false;
    }

    g_opus_playback_queue = xQueueCreate(24, sizeof(opus_packet_msg_t*));  // 24项≈1.44s缓冲，留8个pool slot余量
    if (!g_opus_playback_queue) {
        ESP_LOGE(TAG, "Failed to create opus_playback_queue");
        return false;
    }

    g_fsm_event_queue = xQueueCreate(16, sizeof(fsm_event_msg_t));
    if (!g_fsm_event_queue) {
        ESP_LOGE(TAG, "Failed to create fsm_event_queue");
        return false;
    }

    g_audio_event_bits = xEventGroupCreate();
    if (!g_audio_event_bits) {
        ESP_LOGE(TAG, "Failed to create audio_event_bits");
        return false;
    }

    // WebSocket原始消息队列（事件回调→Main Loop，解耦WS任务）
    g_ws_rx_queue = xQueueCreate(48, sizeof(ws_raw_msg_t));
    if (!g_ws_rx_queue) {
        ESP_LOGE(TAG, "Failed to create ws_rx_queue");
        return false;
    }

    ESP_LOGI(TAG, "All queues initialized successfully");
    ESP_LOGI(TAG, "PCM RingBuffer (16KB) and Memory Pools (44KB) initialized");
    ESP_LOGI(TAG, "2-task communication queues initialized");
    return true;
}

// ============================================================================
// 消息分配和释放（使用PSRAM）
// ============================================================================

audio_data_msg_t* alloc_audio_msg(size_t samples, int channels) {
    size_t data_size = samples * channels * sizeof(int16_t);
    pool_type_t pool_type;

    // 选择合适的内存池
    if (data_size <= 2048) {
        pool_type = POOL_L_2K;
    } else if (data_size <= 4096) {
        pool_type = POOL_L_4K;
    } else {
        ESP_LOGE(TAG, "Audio msg too large: %zu bytes", data_size);
        return nullptr;
    }

    // 从Pool-S分配消息结构体
    audio_data_msg_t* msg = (audio_data_msg_t*)pool_alloc(POOL_S_64);
    if (!msg) {
        ESP_LOGE(TAG, "Failed to allocate audio msg struct from pool");
        return nullptr;
    }

    // 从Pool-L分配数据缓冲区
    msg->data = (int16_t*)pool_alloc(pool_type);
    if (!msg->data) {
        ESP_LOGE(TAG, "Failed to allocate audio data from pool");
        pool_free(POOL_S_64, msg);
        return nullptr;
    }

    msg->samples = samples;
    msg->channels = channels;
    msg->timestamp = xTaskGetTickCount();

    return msg;
}

void free_audio_msg(audio_data_msg_t* msg) {
    if (!msg) return;

    // 确定数据池类型
    size_t data_size = msg->samples * msg->channels * sizeof(int16_t);
    pool_type_t pool_type = (data_size <= 2048) ? POOL_L_2K : POOL_L_4K;

    // 释放数据和消息结构体
    if (msg->data) {
        pool_free(pool_type, msg->data);
    }
    pool_free(POOL_S_64, msg);
}

opus_packet_msg_t* alloc_opus_msg(size_t len) {
    pool_type_t pool_type;

    // 选择合适的内存池
    if (len <= 256) {
        pool_type = POOL_S_256;
    } else if (len <= 2048) {
        pool_type = POOL_L_2K;
    } else if (len <= 4096) {
        pool_type = POOL_L_4K;
    } else {
        ESP_LOGE(TAG, "Opus msg too large: %zu bytes", len);
        return nullptr;
    }

    // 从Pool-S分配消息结构体
    opus_packet_msg_t* msg = (opus_packet_msg_t*)pool_alloc(POOL_S_64);
    if (!msg) {
        ESP_LOGE(TAG, "Failed to allocate opus msg struct from pool");
        return nullptr;
    }

    // 从池分配数据缓冲区
    msg->data = (uint8_t*)pool_alloc(pool_type);
    if (!msg->data) {
        ESP_LOGE(TAG, "Failed to allocate opus data from pool");
        pool_free(POOL_S_64, msg);
        return nullptr;
    }

    msg->len = len;
    msg->timestamp = xTaskGetTickCount();

    return msg;
}

void free_opus_msg(opus_packet_msg_t* msg) {
    if (!msg) return;

    // 确定数据池类型
    pool_type_t pool_type;
    if (msg->len <= 256) {
        pool_type = POOL_S_256;
    } else if (msg->len <= 2048) {
        pool_type = POOL_L_2K;
    } else {
        pool_type = POOL_L_4K;
    }

    // 释放数据和消息结构体
    if (msg->data) {
        pool_free(pool_type, msg->data);
    }
    pool_free(POOL_S_64, msg);
}


// ============================================================================
// PCM RingBuffer Implementation
// ============================================================================

pcm_ringbuffer_t g_pcm_ringbuffer;
pcm_ringbuffer_t g_ref_ringbuffer;
pcm_ringbuffer_t g_mic1_ringbuffer;

bool ringbuffer_init(pcm_ringbuffer_t* rb, size_t capacity) {
    rb->buffer = (int16_t*)heap_caps_malloc(capacity * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!rb->buffer) {
        ESP_LOGE(TAG, "Failed to allocate RingBuffer in PSRAM");
        return false;
    }

    rb->capacity = capacity;
    rb->write_pos = 0;
    rb->read_pos = 0;

    ESP_LOGI(TAG, "RingBuffer initialized (lock-free SPSC): %zu samples (%zuKB)",
             capacity, (capacity * sizeof(int16_t)) / 1024);
    return true;
}

size_t ringbuffer_write(pcm_ringbuffer_t* rb, const int16_t* data, size_t samples) {
    if (!rb || !data || samples == 0) return 0;

    // 读取消费者指针的快照（volatile确保每次从内存读取）
    const size_t read_pos = rb->read_pos;
    const size_t write_pos = rb->write_pos;

    // 计算可用空间（保留1个样本防止满/空混淆）
    size_t available_space = (read_pos - write_pos - 1 + rb->capacity) % rb->capacity;
    size_t to_write = (samples < available_space) ? samples : available_space;

    if (to_write == 0) return 0;

    // 处理wrap-around
    size_t part1 = rb->capacity - write_pos;
    if (to_write <= part1) {
        memcpy(&rb->buffer[write_pos], data, to_write * sizeof(int16_t));
    } else {
        memcpy(&rb->buffer[write_pos], data, part1 * sizeof(int16_t));
        memcpy(&rb->buffer[0], data + part1, (to_write - part1) * sizeof(int16_t));
    }

    // 内存屏障：确保数据写入对消费者可见后再更新write_pos
    __sync_synchronize();
    rb->write_pos = (write_pos + to_write) % rb->capacity;

    return to_write;
}

size_t ringbuffer_read(pcm_ringbuffer_t* rb, int16_t* out, size_t samples) {
    if (!rb || !out || samples == 0) return 0;

    // 读取生产者指针的快照
    const size_t write_pos = rb->write_pos;
    const size_t read_pos = rb->read_pos;

    // 计算可读数据量
    size_t available = (write_pos - read_pos + rb->capacity) % rb->capacity;
    size_t to_read = (samples < available) ? samples : available;

    if (to_read == 0) return 0;

    // 内存屏障：确保读取write_pos后再读数据
    __sync_synchronize();

    // 处理wrap-around
    size_t part1 = rb->capacity - read_pos;
    if (to_read <= part1) {
        memcpy(out, &rb->buffer[read_pos], to_read * sizeof(int16_t));
    } else {
        memcpy(out, &rb->buffer[read_pos], part1 * sizeof(int16_t));
        memcpy(out + part1, &rb->buffer[0], (to_read - part1) * sizeof(int16_t));
    }

    // 内存屏障：确保数据读完后再更新read_pos
    __sync_synchronize();
    rb->read_pos = (read_pos + to_read) % rb->capacity;

    return to_read;
}

size_t ringbuffer_data_available(pcm_ringbuffer_t* rb) {
    if (!rb) return 0;
    // volatile读取确保获取最新值
    return (rb->write_pos - rb->read_pos + rb->capacity) % rb->capacity;
}

void ringbuffer_reset(pcm_ringbuffer_t* rb) {
    if (!rb) return;
    // 仅在idle状态调用（无并发访问）
    rb->write_pos = 0;
    rb->read_pos = 0;
}

// ============================================================================
// Fixed Memory Pool Implementation
// ============================================================================

memory_pool_t g_memory_pools[POOL_COUNT];

// 调试计数器
static uint32_t pool_alloc_count[POOL_COUNT] = {0};
static uint32_t pool_free_count[POOL_COUNT] = {0};

bool init_memory_pools() {
    const struct {
        uint32_t block_size;
        uint32_t block_count;
    } pool_configs[POOL_COUNT] = {
        {64, 32},    // POOL_S_64: 64B × 32 = 2KB (bitmap最大32块)
        {128, 32},   // POOL_S_128: 128B × 32 = 4KB
        {256, 32},   // POOL_S_256: 256B × 32 = 8KB (match playback queue capacity)
        {2048, 16},  // POOL_L_2K: 2KB × 16 = 32KB
        {4096, 8},   // POOL_L_4K: 4KB × 8 = 32KB (增加到8块)
    };

    uint32_t total_size = 0;

    for (int i = 0; i < POOL_COUNT; i++) {
        memory_pool_t* pool = &g_memory_pools[i];
        pool->block_size = pool_configs[i].block_size;
        pool->block_count = pool_configs[i].block_count;

        // 修复：当block_count=32时，(1U << 32)是UB（未定义行为）
        if (pool->block_count == 32) {
            pool->free_bitmap = 0xFFFFFFFF;  // 所有32位都是1（空闲）
        } else if (pool->block_count < 32) {
            pool->free_bitmap = (1U << pool->block_count) - 1;  // 所有块初始为空闲
        } else {
            ESP_LOGE(TAG, "Pool %d block_count %lu exceeds 32!", i, pool->block_count);
            return false;
        }

        pool->mutex = xSemaphoreCreateMutex();

        if (!pool->mutex) {
            ESP_LOGE(TAG, "Failed to create mutex for pool %d", i);
            return false;
        }

        // 从PSRAM分配内存池
        pool->memory = heap_caps_malloc(pool->block_size * pool->block_count, MALLOC_CAP_SPIRAM);
        if (!pool->memory) {
            ESP_LOGE(TAG, "Failed to allocate pool %d: %lu bytes", i,
                     pool->block_size * pool->block_count);
            return false;
        }

        total_size += pool->block_size * pool->block_count;
        ESP_LOGI(TAG, "Pool %d: %lu x %lu bytes = %lu KB",
                 i, pool->block_count, pool->block_size,
                 (pool->block_size * pool->block_count) / 1024);
    }

    ESP_LOGI(TAG, "Memory pools initialized: total %lu KB", total_size / 1024);
    return true;
}

void* pool_alloc(pool_type_t type) {
    if (type >= POOL_COUNT) return nullptr;

    memory_pool_t* pool = &g_memory_pools[type];
    xSemaphoreTake(pool->mutex, portMAX_DELAY);

    // 池分配统计（仅前3次打印，之后静默）
    static uint32_t total_calls[POOL_COUNT] = {0};
    total_calls[type]++;
    if (total_calls[type] <= 3) {
        ESP_LOGD(TAG, "pool_alloc(type=%d) called #%lu, bitmap=0x%08lX",
                 type, total_calls[type], pool->free_bitmap);
    }

    // 查找第一个空闲块（使用__builtin_ctz计数尾部零）
    if (pool->free_bitmap == 0) {
        xSemaphoreGive(pool->mutex);
        ESP_LOGW(TAG, "Pool %d exhausted! (bitmap=0x%08lX)", type, pool->free_bitmap);
        return nullptr;
    }

    int block_idx = __builtin_ctz(pool->free_bitmap);
    pool->free_bitmap &= ~(1U << block_idx);  // 标记为已使用

    pool_alloc_count[type]++;  // 调试计数

    xSemaphoreGive(pool->mutex);

    void* ptr = (uint8_t*)pool->memory + (block_idx * pool->block_size);
    return ptr;
}

void pool_free(pool_type_t type, void* ptr) {
    if (!ptr || type >= POOL_COUNT) return;

    memory_pool_t* pool = &g_memory_pools[type];
    xSemaphoreTake(pool->mutex, portMAX_DELAY);

    // 计算块索引
    ptrdiff_t offset = (uint8_t*)ptr - (uint8_t*)pool->memory;
    int block_idx = offset / pool->block_size;

    // 验证并标记为空闲
    if (block_idx >= 0 && block_idx < (int)pool->block_count) {
        pool->free_bitmap |= (1U << block_idx);
        pool_free_count[type]++;  // 调试计数
    } else {
        ESP_LOGE(TAG, "Invalid pool_free: ptr not in pool %d", type);
    }

    xSemaphoreGive(pool->mutex);
}

void pool_free_by_size(void* ptr, size_t len) {
    if (!ptr) return;
    pool_type_t type;
    if (len <= 64) type = POOL_S_64;
    else if (len <= 128) type = POOL_S_128;
    else if (len <= 256) type = POOL_S_256;
    else if (len <= 2048) type = POOL_L_2K;
    else type = POOL_L_4K;
    pool_free(type, ptr);
}

void pool_print_stats() {
    ESP_LOGI(TAG, "=== Memory Pool Stats ===");
    for (int i = 0; i < POOL_COUNT; i++) {
        memory_pool_t* pool = &g_memory_pools[i];
        xSemaphoreTake(pool->mutex, portMAX_DELAY);
        int free_blocks = __builtin_popcount(pool->free_bitmap);
        int used_blocks = pool->block_count - free_blocks;
        xSemaphoreGive(pool->mutex);

        ESP_LOGI(TAG, "Pool %d (%lu B): used=%d/%lu (%d%%), alloc=%lu, free=%lu, leak=%ld",
                 i, pool->block_size, used_blocks, pool->block_count,
                 (used_blocks * 100) / pool->block_count,
                 pool_alloc_count[i], pool_free_count[i],
                 (int32_t)(pool_alloc_count[i] - pool_free_count[i]));
    }
}
