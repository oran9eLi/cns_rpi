# M5 MQTT 发布 设计文档

版本：2026-07-09
状态：已批准，待写实施计划
适用范围：`docs/V1设计文档.md` §10 "M5 MQTT 发布" 里程碑

## 1. 范围

- 新增 `mqtt/` 模块：连接 broker、断线自动重连、把 M4 落地的 `payload::ToJson()` 输出按固定节奏发布出去。
- 遥测 topic 命名方案定稿：`{topic_prefix}/{vendor_id}/telemetry`。
- "身份就绪后才连 MQTT" 这个 `V1设计文档.md` §2/§9 记的启动依赖，在这个里程碑落地。

**明确不做的事**（避免范围蔓延到别的里程碑）：

- 不做命令下行/订阅——`command_dispatcher`、`COMMAND_LONG` 编码、`COMMAND_ACK` 回传都是 M6 的事。M5 的 `MqttClient` 只暴露 `Publish()`，不订阅任何 topic。
- 不做断网本地落盘缓存补发——`V1设计文档.md` §11 "QoS/可靠性要求" 本来就写了"待确定，可 V1 从简，V2 再补"，这次维持从简：断线期间不发布，不补发，重连后自然恢复。
- 不做"身份一直不就绪"的超时降级——见第 3 节，这是本次讨论明确拍板的决定，不是遗漏。
- 不改 `config.json` 的 `mqtt` schema——M2 阶段就已经把 `broker_host`/`broker_port`/`client_id`/`username`/`password`/`topic_prefix`/`qos`/`keepalive_seconds` 这 8 个字段占好位，本次全部复用，不新增字段（发布节奏、retain 都是代码里的固定值，不是运维需要调的旋钮，YAGNI）。

## 2. Topic 命名方案

```
{topic_prefix}/{vendor_id}/telemetry
```

- `topic_prefix`：来自 `config.json` 的 `mqtt.topic_prefix`（示例值 `cns_rpi`），区分环境/产品线用。
- `vendor_id`：厂商唯一产品识别码（`docs/设备标识符.md` 权威全局键），从 `state_store.vendor_id` 拿，M3c 已经能从 `OPEN_DRONE_ID_BASIC_ID` 解出来。
- `telemetry`：固定后缀，跟 M6 以后会加的 `command`/`ack` 同根，结构上留好扩展位。

不用 `rpi_serial` 兜底拼 topic（哪怕身份迟迟不来）——`docs/设备标识符.md` §5 的原则是 topic 寻址字段必须用权威全局键，用过渡键拼过 topic 之后再切换成 `vendor_id`，会导致订阅方（管控中心）跟着改订阅，属于文档里明确要避免的情况。

## 3. 身份就绪前的等待策略

`V1设计文档.md` §2/§9 提的"先建 UART 链路 → 收到身份帧解出唯一 ID('身份就绪') → 才初始化 MQTT client"这个启动依赖，本次拍板：**一直等，没有超时兜底**。

- `main.cpp` 主循环每轮检查 `state_store.Snapshot().vendor_id.has_value()`，没值就跳过 MQTT 相关的两个判断（连接判断、发布判断），继续正常做 UART 收发/解码/JSON 打印，不阻塞主循环。
- 如果 STM32 一直不发身份帧（固件没接线/还没实现这部分），效果是：MQTT 功能永远不会启动，但程序本身正常跑（不崩溃、UART 解码/打印不受影响），符合"没有身份就没有全局唯一寻址方式，功能性地无法安全发布"的设计意图。
- 这不是遗漏，是本次讨论明确排除的替代方案（超时后用 `rpi_serial` 兜底再切换）——理由见上一节。

## 4. 发布节奏

固定节奏 **1Hz**，不跟随 MAVLink 消息到达频率触发。

- 现状：`main.cpp` 现在每收到一帧 MAVLink 消息（哪怕是 HEARTBEAT 这种高频帧）就调一次 `LogJsonPayload` 打印完整快照，这个打印逻辑保留不动，继续服务真机调试目视核对。
- MQTT 发布走独立的节拍器判断，跟现有 `kHeartbeatInterval`（驱动往 STM32 发心跳）同级模式，新增 `kTelemetryPublishInterval = std::chrono::seconds(1)`。到点即取 `state_store` 当前快照发布一次，不管这一秒内 UART 侧收了几帧。
- 好处：避免 HEARTBEAT 等高频帧把 MQTT 发布频率带上去打爆 QoS/带宽，行为可预测；代价：MQTT 上看到的数据比 UART 侧实际到达晚最多 1 秒，V1 阶段可接受。

## 5. 组件设计

### 5.1 `src/mqtt/topic.hpp` + `.cpp`（纯逻辑，不依赖 libmosquitto）

```cpp
namespace mqtt {

/// 拼遥测发布topic："{topic_prefix}/{vendor_id}/telemetry"。
/// vendor_id理论上不会传空(上层已经用state_store.vendor_id.has_value()把关)，
/// 这里不做空值校验，纯字符串拼接。
std::string BuildTelemetryTopic(const std::string& topic_prefix, const std::string& vendor_id);

}  // namespace mqtt
```

不依赖 `mosquitto.h`，本机随时能编译/单测，不需要装 `libmosquitto-dev`。

### 5.2 `src/mqtt/mqtt_client.hpp` + `.cpp`（依赖 libmosquitto）

```cpp
namespace mqtt {

struct ConnectionOptions {
  std::string broker_host;
  int broker_port;
  std::string client_id;
  std::string username;
  std::string password;
  int keepalive_seconds;
};

class MqttClient {
 public:
  static std::optional<MqttClient> Open(const ConnectionOptions& options);
  ~MqttClient();

  MqttClient(MqttClient&&) noexcept;
  MqttClient& operator=(MqttClient&&) noexcept;
  MqttClient(const MqttClient&) = delete;
  MqttClient& operator=(const MqttClient&) = delete;

  /// 发布一条消息。qos/retain由调用方传入(来自config.json的mqtt.qos，
  /// retain固定传true，见第6节)。连没连由调用方先调IsConnected()把关，
  /// 这里不重复判断、不重试。
  bool Publish(const std::string& topic, const std::string& payload, int qos, bool retain);

  /// 读connected_标志，不主动问库要状态。
  bool IsConnected() const;

 private:
  explicit MqttClient(mosquitto* handle);
  mosquitto* handle_;
  std::shared_ptr<std::atomic<bool>> connected_;
};

}  // namespace mqtt
```

`Open()` 内部步骤：

1. `std::call_once` 保证进程级 `mosquitto_lib_init()` 只调一次。
2. `mosquitto_new(client_id.c_str(), /*clean_session=*/true, /*userdata=*/&connected_flag)`，失败（返回 `nullptr`）则整个 `Open()` 返回 `std::nullopt`。
3. 非空用户名才调 `mosquitto_username_pw_set()`（`username`/`password` 都是空字符串时不调，`config.example.json` 默认就是空）。
4. 注册 `on_connect`/`on_disconnect` 回调：只更新 `connected_` 标志、打一行日志，不做任何业务判断（业务判断在 `main.cpp` 里通过轮询 `IsConnected()` 做）。
5. `mosquitto_reconnect_delay_set()` 配置指数退避重连（起始延迟/最大延迟给固定值，不走配置文件，属于代码内部实现细节）。
6. `mosquitto_connect_async(broker_host, broker_port, keepalive_seconds)`。
7. `mosquitto_loop_start()` 起后台网络线程，交给 libmosquitto 自己管连接/重连/心跳。

`connected_` 用 `std::shared_ptr<std::atomic<bool>>`（不是裸 `std::atomic<bool>` 成员）——因为回调是 C 函数指针风格，`userdata` 传的是这个 `shared_ptr` 管理的裸指针，`MqttClient` 支持 move 之后底层 `atomic<bool>` 对象地址不能变，回调那边存的指针要一直有效。

析构：`mosquitto_loop_stop(/*force=*/true)` → `mosquitto_destroy()`。`mosquitto_lib_cleanup()` 不调（进程级资源，进程退出时 OS 回收，多实例场景下过早 cleanup 会影响其他还存活的实例——V1 只有一个 `MqttClient` 实例，但不主动调更安全）。

## 6. `main.cpp` 改动

```cpp
constexpr auto kTelemetryPublishInterval = std::chrono::seconds(1);
```

主循环外新增：

```cpp
std::optional<mqtt::MqttClient> mqtt_client;
std::string telemetry_topic;
auto last_telemetry_publish = std::chrono::steady_clock::now();
```

主循环每轮迭代新增两个判断（紧跟在现有心跳发送判断之后）：

```cpp
// 1. 连接判断：身份就绪且还没连过，才连一次
if (!mqtt_client) {
  auto snapshot = state_store.Snapshot();
  if (snapshot.vendor_id) {
    telemetry_topic = mqtt::BuildTelemetryTopic(app_config->mqtt.topic_prefix, *snapshot.vendor_id);
    mqtt_client = mqtt::MqttClient::Open({
        .broker_host = app_config->mqtt.broker_host,
        .broker_port = app_config->mqtt.broker_port,
        .client_id = app_config->mqtt.client_id,
        .username = app_config->mqtt.username,
        .password = app_config->mqtt.password,
        .keepalive_seconds = app_config->mqtt.keepalive_seconds,
    });
    if (mqtt_client) {
      std::cout << "MQTT连接中: broker=" << app_config->mqtt.broker_host
                << " topic=" << telemetry_topic << std::endl;
    } else {
      std::cerr << "MQTT客户端创建失败，下一轮重试" << std::endl;
    }
  }
}

// 2. 发布判断：已连接且到了1Hz节拍
auto now = std::chrono::steady_clock::now();
if (mqtt_client && mqtt_client->IsConnected() &&
    now - last_telemetry_publish >= kTelemetryPublishInterval) {
  std::string json_str = payload::ToJson(state_store.Snapshot(), app_config->identity.school_name).dump();
  if (!mqtt_client->Publish(telemetry_topic, json_str, app_config->mqtt.qos, /*retain=*/true)) {
    std::cerr << "MQTT发布失败，下个节拍重试" << std::endl;
  }
  last_telemetry_publish = now;
}
```

现有的逐帧 `LogJsonPayload` 打印调用保留不动，两条路径（打印 vs 发布）各自独立触发条件，互不影响。

## 7. 错误处理

- `Open()`失败（`mosquitto_new`/`loop_start`内部失败）：打日志，`mqtt_client`保持`nullopt`，下一轮主循环因为`!mqtt_client`仍为true自然再试一次，不需要单独的重试计数器。
- 连接建立后断开（网络抖动/broker重启）：libmosquitto后台线程按`mosquitto_reconnect_delay_set()`配置自动重连，`IsConnected()`降为false期间发布判断自动跳过，不发布也不报错刷屏；重连成功后自动恢复正常发布节奏。断线期间的数据不补发（第1节已明确不做）。
- `Publish()`返回false（比如网络恰好在发布瞬间断开）：打一行警告，不重试当前这条，下一个1Hz节拍带着更新后的数据自然重试。

## 8. 测试计划

- **`mqtt/topic.hpp`的`BuildTelemetryTopic()`**：`tests/test_mqtt_topic.cpp`，纯函数单测，覆盖正常`vendor_id`和空字符串输入（防御性，理论上不会发生，但测一下不炸）。CMake加对应`test_mqtt_topic`可执行文件/ctest target，跟现有测试模式一致。
- **`mqtt/mqtt_client.cpp`**：不写ctest单测——项目目前对"真连外部系统"的代码（如`uart::MavlinkLink`对真实串口）都没有自动化单测先例，这次维持同样的测试边界。改成人工验证步骤（见第9节）。
- **`main.cpp`新增的"连接判断/发布判断"过程式代码**：现有`LogTelemetry`/`LogExtension`/`LogJsonPayload`都没有单测（属于集成层，不是`cns_rpi_core`库的一部分），这次保持同样的边界，不新增。

## 9. 人工验证步骤（真机/开发机）

1. 装依赖：`sudo apt install mosquitto libmosquitto-dev`（本机既当broker又编译client，装一次够用）。
2. 起本地broker：`mosquitto`（默认监听`1883`，走`localhost`）。
3. `config.json`的`mqtt.broker_host`指向本地地址（`127.0.0.1`或实际WiFi IP，取决于测试机和broker是不是同一台）。
4. 另开一个终端订阅：`mosquitto_sub -h <broker_host> -t 'cns_rpi/+/telemetry' -v`。
5. 跑`cns_rpi`（真机接真实STM32，或本机接`tools/mavlink_sim/send_frames.py`模拟），确认：
   - 收到身份帧后日志打出"MQTT连接中"；
   - `mosquitto_sub`能收到JSON，topic里的`vendor_id`段跟`identity.vendor_id`字段值一致；
   - 重新执行一次`mosquitto_sub`（新订阅），立刻收到最后一条（验证retain生效）；
   - 手动重启mosquitto broker进程，观察`cns_rpi`能自动重连、恢复发布（不需要重启`cns_rpi`本身）。

**网络路径注意事项**：真机上5G模块（Quectel RM500U-CNV）已拨号时，`usb0`的默认路由metric（100）比`wlan0`（600）低，5G实际上比WiFi优先级更高。**这是期望的产品行为，不是要修的问题**——实地部署现场不一定有WiFi，5G优先保证了"不管有没有WiFi都能联网"，之前记在待定产品决策里的这一条已经拍板确认。`mqtt/`模块代码不关心走哪个网卡，这条路由优先级本身不需要`mqtt/`模块或`main.cpp`做任何处理。

仅本次M5验证阶段：因为5G模块里插的还是临时手机卡（不是最终物联卡），如果想让这轮验证明确走WiFi连本机/局域网broker，需要临时把5G模块拔了或不让它拨号——这只是验证期间的临时措施，跟上面"5G优先是期望行为"的产品结论不矛盾。真实物联卡到货后的联网切换测试留到M8。

## 10. CMake 改动

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
```

`cns_rpi_core`新增源文件`src/mqtt/topic.cpp`、`src/mqtt/mqtt_client.cpp`，链接`PkgConfig::MOSQUITTO`。`test_mqtt_topic`只依赖`topic.cpp`，不需要链接`PkgConfig::MOSQUITTO`（`topic.hpp`不包含`mosquitto.h`）。

trixie的`libmosquitto-dev`自带pkg-config文件，`pkg_check_modules`能直接找到，不需要手动`find_library`。
