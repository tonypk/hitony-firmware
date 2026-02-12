# Phase 1 完成 ✅

## 已完成的工作

### 1. 全局队列系统
- ✅ **app_queues.cc** - 全局队列初始化和消息分配
  - 音频流队列（输入、AFE、编码、解码、输出）
  - 网络队列（WebSocket收发）
  - 控制队列（状态事件、UI命令）
  - 事件组（全局事件标志）
  - PSRAM消息分配器

### 2. 任务管理器
- ✅ **task_manager.h/cc** - 统一任务管理
  - 任务创建和管理
  - 双核任务分配
  - 任务统计和监控
  - 栈使用监控

### 3. 音频任务框架
- ✅ **audio_tasks.cc** - 7个音频处理任务
  - `audio_input_task` - 音频采集（Core 1, Prio 20）
  - `audio_output_task` - 音频播放（Core 1, Prio 19）
  - `afe_process_task` - AFE处理（Core 1, Prio 18）
  - `wake_detect_task` - 唤醒词检测（Core 1, Prio 16）
  - `audio_mixer_task` - 音频混音（Core 1, Prio 15）
  - `opus_decode_task` - Opus解码（Core 1, Prio 14）
  - `opus_encode_task` - Opus编码（Core 1, Prio 13）

### 4. 控制任务框架
- ✅ **control_tasks.cc** - 5个控制任务
  - `websocket_task` - WebSocket通信（Core 0, Prio 8）
  - `state_machine_task` - 状态管理（Core 0, Prio 6）
  - `ui_update_task` - UI更新（Core 0, Prio 3）
  - `led_control_task` - LED控制（Core 0, Prio 2）
  - `heartbeat_task` - 系统监控（Core 0, Prio 1）

### 5. 应用初始化
- ✅ **app_init.cc** - 批量创建所有任务
- ✅ **main_new.cc** - 新的主程序框架
- ✅ **CMakeLists.txt** - 更新构建配置

## 架构特点

### 双核任务分离
```
Core 0 (Protocol CPU)        Core 1 (Application CPU)
├─ WiFi/网络协议            ├─ 音频采集 (Prio 20)
├─ WebSocket通信            ├─ 音频播放 (Prio 19)
├─ 状态机管理               ├─ AFE处理  (Prio 18)
├─ UI更新                   ├─ 唤醒词   (Prio 16)
└─ LED控制                  ├─ 混音     (Prio 15)
                            ├─ Opus解码 (Prio 14)
                            └─ Opus编码 (Prio 13)
```

### 消息队列通信
```
音频输入 → AFE队列 → AFE处理 → 编码队列 → Opus编码 → WS发送队列 → WebSocket

WebSocket → WS接收队列 → Opus解码 → 解码队列 → 混音 → 输出队列 → 音频播放
```

### 内存管理
- 消息分配使用PSRAM
- 避免内存碎片
- 自动释放管理

## 下一步：使用新架构

### 方式1：完整替换（推荐）

```bash
cd /Users/anna/Documents/xiaozhi/echoear-firmware/main

# 备份旧main.cc
mv main.cc main_old.cc

# 使用新main.cc
mv main_new.cc main.cc

# 编译
cd ..
idf.py build
```

### 方式2：逐步迁移

保留旧main.cc，在其中调用新架构：

```cpp
// 在旧main.cc的app_main()中添加：

// 初始化队列
init_global_queues();

// 创建新任务
create_all_tasks();

// 保留部分旧代码继续运行
```

## 当前状态

### 已实现 ✅
- [x] 任务框架创建
- [x] 队列通信架构
- [x] 双核任务分配
- [x] Opus解码任务（已集成）
- [x] 基础音频输入/输出
- [x] UI命令队列
- [x] 状态事件系统
- [x] 系统监控（心跳任务）

### 待实现 TODO
- [ ] AFE音频前端集成
- [ ] 唤醒词检测集成
- [ ] Opus编码器集成
- [ ] WebSocket客户端重构
- [ ] 状态机完整实现
- [ ] 音频混音器完整实现
- [ ] WiFi自动重连
- [ ] LED动画效果

## 验证步骤

### 1. 编译测试

```bash
cd /Users/anna/Documents/xiaozhi/echoear-firmware

# 使用新main.cc
mv main/main.cc main/main_old.cc
mv main/main_new.cc main/main.cc

# 编译
idf.py build
```

**预期结果**：
- ✅ 编译成功
- ✅ 无错误
- ✅ 二进制大小合理

### 2. 烧录测试

```bash
idf.py flash monitor
```

**预期结果**：
- ✅ 设备启动
- ✅ 打印启动banner
- ✅ 所有任务创建成功
- ✅ 无崩溃

### 3. 功能测试

**观察日志**：
```
I (xxx) main: [Phase 1] Basic Initialization...
I (xxx) main: [Phase 2] Creating global queues...
I (xxx) app_queues: All queues initialized successfully
I (xxx) main: [Phase 3] Initializing hardware modules...
I (xxx) audio_i2s: Codec initialized (ES7210 + ES8311)
I (xxx) lvgl_ui: LVGL initialized
I (xxx) main: [Phase 4] Creating all tasks...
I (xxx) task_mgr: Created task: audio_input (stack=8192, prio=20, core=1)
I (xxx) task_mgr: Created task: audio_output (stack=8192, prio=19, core=1)
...
I (xxx) main: [Phase 5] Startup complete!
I (xxx) main: EchoEar is ready! 🎤
```

**每10秒打印心跳**：
```
I (xxx) heartbeat: === System Heartbeat ===
I (xxx) heartbeat: Events: WiFi=0 WS=0 Wake=0 VAD=0 TTS=0 Recording=0
I (xxx) heartbeat: Free heap: 200000 bytes, PSRAM: 7800000 bytes
I (xxx) task_mgr: Task Statistics:
I (xxx) task_mgr: Name             Prio    Stack     Free    CPU%
...
```

### 4. 性能测试

**检查项**：
- [ ] CPU使用率 < 50%（空闲时）
- [ ] 内存稳定（无泄漏）
- [ ] 所有任务正常运行
- [ ] 无栈溢出警告

## 预期问题和解决

### 问题1：编译错误

**可能原因**：缺少头文件包含

**解决**：检查`#include`语句

### 问题2：任务创建失败

**可能原因**：栈或堆内存不足

**解决**：
- 减少某些任务的栈大小
- 检查PSRAM是否正确初始化

### 问题3：LVGL崩溃

**原因**：UI命令队列未使用

**解决**：确保所有UI调用通过`send_ui_command()`

## 性能对比

### 旧架构
- 单一主循环
- 轮询模式
- 阻塞调用
- 无任务隔离

### 新架构 ✨
- 12个独立任务
- 事件驱动
- 非阻塞队列
- 双核并行
- 优先级调度

## 成果

🎉 **Phase 1 基础架构重构完成！**

- ✅ 12个任务框架已创建
- ✅ 消息队列通信已建立
- ✅ 双核任务分配已实现
- ✅ 任务管理和监控已完成
- ✅ 基础音频流水线已搭建

**下一步**：Phase 2 - AFE音频前端集成

---

**准备好了吗？让我们编译并测试新架构！** 🚀
