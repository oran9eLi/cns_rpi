/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * 收到 MAVLink 帧只更新 state::StateStore，不在逐帧路径输出业务日志；标准遥测
 * 解码失败时再尝试扩展帧和身份帧解码。日志格式和输出目标由 logging::Logger 独立负责，
 * main 仅在配置成功后创建并向业务模块传递同一实例。
 * 受控设备身份统一来自 OPEN_DRONE_ID_BASIC_ID.uas_id：主控箱被动接收，PX4 需要
 * 周期主动请求；树莓派自身不持有任何业务身份。身份就绪后才连接 broker，
 * 按固定节拍发布遥测，发布失败时记录警告；不把遥测 JSON 写入运行日志。
 * 注册状态在连接、重连和身份元数据变化时 retained 发布；异常断线由 Last Will 标记
 * offline，正常退出主动发布 offline。
 */

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cellular/cellular_status.hpp"
#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "config/startup_summary.hpp"
#include "config_command/config_store.hpp"
#include "config_command/command_parser.hpp"
#include "config_command/command_processor.hpp"
#include "config_command/config_updater.hpp"
#include "control_command/control_command.hpp"
#include "control_command/control_endpoint.hpp"
#include "control_command/control_transaction.hpp"
#include "latency/px4_latency.hpp"
#include "logging/logger.hpp"
#include "mqtt/mqtt_client.hpp"
#include "mqtt/topic.hpp"
#include "network/qgc_udp_bridge.hpp"
#include "payload/json_serializer.hpp"
#include "platform/systemd_watchdog.hpp"
#include "protocol/extension_decoder.hpp"
#include "protocol/identity.hpp"
#include "protocol/px4_identity.hpp"
#include "protocol/telemetry_decoder.hpp"
#include "registration/registration_payload.hpp"
#include "registration/registration_state.hpp"
#include "state/state_store.hpp"
#include "uart/mavlink_port_discovery.hpp"

namespace {

constexpr std::uint8_t kComponentId = MAV_COMP_ID_ONBOARD_COMPUTER;
constexpr auto kControlAckTimeout = std::chrono::seconds(2);
constexpr auto kDiscoveryProbeTimeout = std::chrono::milliseconds(300);
constexpr auto kDiscoveryRetryInterval = std::chrono::seconds(1);
constexpr auto kDiscoveryWarningInterval = std::chrono::seconds(10);
constexpr auto kMavlinkSilenceTimeout = std::chrono::seconds(10);
constexpr auto kPx4IdentityRequestInterval = std::chrono::seconds(2);
/// 身份未就绪的提示日志间隔。请求节拍是 2s，逐次告警会淹没日志。
constexpr auto kIdentityWarningInterval = std::chrono::seconds(10);

volatile std::sig_atomic_t g_exit_requested = 0;

void HandleExitSignal(int /*signal*/) { g_exit_requested = 1; }

/// RPi 自己的 HEARTBEAT：它不是飞控，所以 autopilot=MAV_AUTOPILOT_INVALID。
mavlink_message_t BuildHeartbeat(std::uint8_t system_id) {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(system_id, kComponentId, &msg, MAV_TYPE_ONBOARD_CONTROLLER,
                              MAV_AUTOPILOT_INVALID, /*base_mode=*/0, /*custom_mode=*/0,
                              MAV_STATE_ACTIVE);
  return msg;
}

registration::OnlineRegistration MakeOnlineRegistration(
    const state::TelemetryState& snapshot, const std::string& school_name) {
  return {
      .device_id = *snapshot.device_id,
      .device_type = *snapshot.device_type,
      .school_name = school_name,
      .dcdw_label = snapshot.dcdw_label,
      .product = snapshot.product,
      .version = snapshot.version,
  };
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleExitSignal);
  std::signal(SIGTERM, HandleExitSignal);

  // 配置路径必须由调用方显式给出绝对路径，不设默认值：systemd 拉起时的工作目录
  // 不确定，任何相对路径默认值都会在服务环境下静默指向错误的文件。
  std::string config_path;
  bool config_path_seen = false;
  std::vector<std::string_view> writer_arguments;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument.starts_with("--config-")) {
      writer_arguments.push_back(argument);
    } else if (!config_path_seen) {
      config_path = argument;
      config_path_seen = true;
    } else {
      std::cerr << "只能指定一个配置文件路径\n";
      return EXIT_FAILURE;
    }
  }
  if (!config_path_seen) {
    std::cerr << "用法: cns_rpi <配置文件绝对路径> [--config-writer=...] "
                 "[--config-helper=...]\n";
    return EXIT_FAILURE;
  }
  if (!std::filesystem::path(config_path).is_absolute()) {
    std::cerr << "配置文件路径必须是绝对路径: " << config_path << "\n";
    return EXIT_FAILURE;
  }
  auto writer_options = config_command::ParseWriterOptions(writer_arguments);
  if (!writer_options) {
    std::cerr << "配置写入启动参数非法: " << writer_options.error() << "\n";
    return EXIT_FAILURE;
  }

  auto app_config = config::LoadAppConfig(config_path);
  if (!app_config) {
    std::cerr << "读取配置失败: " << config_path << ": "
              << config::ConfigErrorMessage(app_config.error()) << "\n";
    return EXIT_FAILURE;
  }

  auto level = logging::ParseLevel(app_config->logging.level);
  if (!level) {
    std::cerr << "初始化日志失败: " << level.error() << '\n';
    return EXIT_FAILURE;
  }
  auto logger = logging::Logger::Create(
      {.minimum_level = *level,
       .file = app_config->logging.file,
       .max_file_size_bytes = app_config->logging.max_file_size_bytes},
      std::cout, std::cerr);
  if (!logger) {
    std::cerr << "初始化日志失败: " << logger.error() << '\n';
    return EXIT_FAILURE;
  }

  (*logger)->Info(config::BuildStartupSummary(*app_config));

  state::StateStore state_store;
  auto last_heartbeat = std::chrono::steady_clock::now();
  auto last_cellular_heartbeat = std::chrono::steady_clock::now();
  std::optional<mqtt::MqttClient> mqtt_client;
  registration::RegistrationState registration_state;
  std::optional<std::string> active_device_id;
  std::string registration_topic;
  std::string offline_payload;
  std::string telemetry_topic;
  std::string px4_realtime_topic;
  std::string px4_latency_probe_topic;
  std::string px4_latency_ack_topic;
  std::string config_set_topic;
  std::string config_ack_topic;
  std::string control_set_topic;
  std::string control_ack_topic;
  control_command::ControlTransaction control_transaction(kControlAckTimeout);
  std::optional<control_command::ControlledDeviceEndpoint> controlled_device;
  std::optional<uart::MavlinkLink> link;
  std::optional<network::QgcUdpBridge> qgc_udp_bridge;
  std::string active_serial_device;
  std::optional<mavlink_message_t> pending_first_message;
  bool ambiguous_device_type = false;
  uart::AsyncMavlinkDiscovery discovery;
  uart::DiscoveryLogLimiter discovery_log_limiter(kDiscoveryWarningInterval);
  // 两条限频器分开：身份未就绪和 uas_id 非法是两种完全不同的现场故障，
  // 共用一个限频器会让其中一种被另一种饿死(设计文档 §3.4)。
  uart::DiscoveryLogLimiter identity_wait_log_limiter(kIdentityWarningInterval);
  uart::DiscoveryLogLimiter uas_id_invalid_log_limiter(kIdentityWarningInterval);
  uart::MavlinkSilenceWatchdog silence_watchdog(kMavlinkSilenceTimeout);
  // systemd watchdog 只证明主循环还在转，不反映业务健康：串口断开和 MQTT 断连
  // 各有自己的恢复逻辑（且已验证能自愈），不应升级成整个进程重启。
  const auto watchdog_timeout = platform::WatchdogTimeout();
  std::optional<platform::WatchdogFeedTimer> watchdog_feed_timer;
  if (watchdog_timeout) {
    watchdog_feed_timer.emplace(platform::FeedIntervalFor(*watchdog_timeout));
    (*logger)->Info(
        "systemd watchdog 已启用，超时=" +
        std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(*watchdog_timeout)
                .count()) +
        "s");
  }
  auto next_discovery = std::chrono::steady_clock::now();
  bool ever_connected_to_device = false;
  bool restart_requested = false;
  auto last_telemetry_publish = std::chrono::steady_clock::now();
  auto last_px4_realtime_publish = std::chrono::steady_clock::now();
  std::uint64_t px4_realtime_sequence = 0;
  auto last_px4_basic_id_request =
      std::chrono::steady_clock::now() - kPx4IdentityRequestInterval;
  auto last_px4_version_request =
      std::chrono::steady_clock::now() - kPx4IdentityRequestInterval;
  // 第一次请求刚发出就报"PX4 未响应"是误报，至少要等一个请求周期落空。
  unsigned px4_basic_id_requests_sent = 0;
  auto next_qgc_start_attempt = std::chrono::steady_clock::now();

  auto close_mqtt_session = [&](bool publish_offline) {
    if (control_transaction.HasPending()) {
      (void)control_transaction.HandleLocalFailure(
          {.code = "device_link_unavailable",
           .message = "受控设备 MAVLink 链路不可用"});
      if (mqtt_client && mqtt_client->IsConnected() &&
          control_transaction.PendingAck() != nullptr &&
          !control_ack_topic.empty()) {
        if (mqtt_client->PublishAndWait(
                control_ack_topic, control_transaction.PendingAck()->dump(),
                app_config->mqtt.topics.control_ack.qos, /*retain=*/false,
                std::chrono::seconds(2))) {
          control_transaction.ConfirmAckPublished();
        }
      }
    }
    if (publish_offline && mqtt_client && mqtt_client->IsConnected() &&
        !registration_topic.empty() && !offline_payload.empty()) {
      (void)mqtt_client->PublishAndWait(
          registration_topic, offline_payload,
          app_config->mqtt.topics.registration.qos, /*retain=*/true,
          std::chrono::seconds(2));
    }
    mqtt_client.reset();
    registration_state = registration::RegistrationState{};
    active_device_id.reset();
    registration_topic.clear();
    telemetry_topic.clear();
    px4_realtime_topic.clear();
    px4_latency_probe_topic.clear();
    px4_latency_ack_topic.clear();
    config_set_topic.clear();
    config_ack_topic.clear();
    control_set_topic.clear();
    control_ack_topic.clear();
    offline_payload.clear();
    control_transaction =
        control_command::ControlTransaction(kControlAckTimeout);
  };

  auto mark_link_disconnected = [&] {
    close_mqtt_session(/*publish_offline=*/true);
    (*logger)->Error("受控设备串口断开: " + active_serial_device);
    link.reset();
    qgc_udp_bridge.reset();
    active_serial_device.clear();
    pending_first_message.reset();
    controlled_device.reset();
    ambiguous_device_type = false;
    state_store.ResetDeviceState();
    px4_basic_id_requests_sent = 0;
    silence_watchdog.Reset();
    next_discovery = std::chrono::steady_clock::now();
  };

  auto process_mavlink_message = [&](const mavlink_message_t& message,
                                     std::chrono::steady_clock::time_point now) {
    silence_watchdog.ObserveValidFrame(now);
    const auto classified =
        control_command::ClassifyControlledDeviceHeartbeat(
            message, app_config->device.mode);
    if (controlled_device && classified &&
        (classified->type != controlled_device->type ||
         classified->endpoint.system_id !=
             controlled_device->endpoint.system_id ||
         classified->endpoint.component_id !=
             controlled_device->endpoint.component_id)) {
      (*logger)->Error(
          "同一串口检测到冲突的受控设备特征，停止 MQTT 上报并等待检查接线");
      close_mqtt_session(/*publish_offline=*/true);
      state_store.ResetDeviceState();
      controlled_device.reset();
      qgc_udp_bridge.reset();
      ambiguous_device_type = true;
      return;
    }
    if (ambiguous_device_type) {
      return;
    }
    const bool endpoint_was_unknown = !controlled_device.has_value();
    controlled_device = control_command::ObserveControlledDeviceHeartbeat(
        message, controlled_device, app_config->device.mode);
    if (!controlled_device) {
      return;
    }
    if (endpoint_was_unknown) {
      state_store.UpdateControlledDevice(
          controlled_device->type, controlled_device->endpoint.system_id,
          controlled_device->endpoint.component_id);
      if (controlled_device->type == device::Type::kCnsBox) {
        state_store.UpdateDcdwLabel(
            protocol::FormatDcdwLabel(controlled_device->endpoint.system_id));
      }
      (*logger)->Info(
          "已识别受控设备: type=" +
          std::string(device::TypeName(controlled_device->type)) +
          " sysid=" +
          std::to_string(controlled_device->endpoint.system_id) +
          " compid=" +
          std::to_string(controlled_device->endpoint.component_id));
    }
    if (controlled_device->type == device::Type::kFlightController &&
        app_config->qgc_udp.enabled) {
      if (!qgc_udp_bridge && now >= next_qgc_start_attempt) {
        auto bridge = network::QgcUdpBridge::Open(
            {.lan_interfaces = app_config->qgc_udp.lan_interfaces,
             .listen_port =
                 static_cast<std::uint16_t>(app_config->qgc_udp.listen_port),
             .qgc_port =
                 static_cast<std::uint16_t>(app_config->qgc_udp.qgc_port),
             .discovery_interval =
                 app_config->qgc_udp.discovery_interval,
             .handover_idle = app_config->qgc_udp.handover_idle,
             .peer_timeout = app_config->qgc_udp.peer_timeout,
             .allow_commands = app_config->qgc_udp.allow_commands});
        if (bridge) {
          qgc_udp_bridge.emplace(std::move(*bridge));
          std::string qgc_interfaces;
          for (const auto& interface_name :
               app_config->qgc_udp.lan_interfaces) {
            if (!qgc_interfaces.empty()) {
              qgc_interfaces += ",";
            }
            qgc_interfaces += interface_name;
          }
          (*logger)->Info(
              "QGC UDP单控制权桥接已启动: listen=" +
              std::to_string(qgc_udp_bridge->LocalPort()) +
              " qgc=" + std::to_string(app_config->qgc_udp.qgc_port) +
              " interfaces=" + qgc_interfaces +
              " policy=" + app_config->qgc_udp.peer_policy +
              " handover_idle_ms=" +
              std::to_string(app_config->qgc_udp.handover_idle.count()) +
              " commands=" +
              (app_config->qgc_udp.allow_commands ? "enabled" : "disabled"));
        } else {
          (*logger)->Warn(
              "QGC UDP单控制权桥接启动失败: " +
              std::string(network::QgcUdpErrorMessage(bridge.error())));
          next_qgc_start_attempt = now + std::chrono::seconds(5);
        }
      }
      if (qgc_udp_bridge) {
        qgc_udp_bridge->ForwardFlightControllerMessage(message, now);
      }
    }

    if (!control_command::IsMessageFromControlledDevice(message,
                                                         *controlled_device)) {
      return;
    }

    const auto rpi_system_id =
        control_command::LearnedControlledSystemId(controlled_device);
    if (rpi_system_id && mqtt_client &&
        control_command::IsExpectedCommandAck(
            message, controlled_device->endpoint, *rpi_system_id,
            kComponentId)) {
      mavlink_command_ack_t ack{};
      mavlink_msg_command_ack_decode(&message, &ack);
      const auto ack_status = control_transaction.HandleMavlinkAck(
          ack.command, ack.result, ack.progress, ack.result_param2, now);
      const char* match =
          ack_status == control_command::MavlinkAckStatus::kFinal      ? "最终"
          : ack_status == control_command::MavlinkAckStatus::kInProgress ? "进行中"
                                                                         : "未匹配";
      (*logger)->Info("收到设备应答: mavlink_command=" + std::to_string(ack.command) +
                      " 结果=" + control_command::ResultCode(ack.result) + " (" + match + ")");
    }
    if (!protocol::DecodeAndStore(message, state_store)) {
      (void)protocol::DecodeExtensionAndStore(message, state_store);
    }

    // 主控箱和 PX4 统一从 Basic ID 取身份：uas_id 就是 device_id，不再按设备
    // 类型分叉，也不再用 AUTOPILOT_VERSION 的 uid/uid2 冒充受控设备身份。
    if (message.msgid != MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID) {
      return;
    }
    mavlink_open_drone_id_basic_id_t basic_id{};
    mavlink_msg_open_drone_id_basic_id_decode(&message, &basic_id);
    const std::string uas_id = protocol::ExtractUasId(basic_id.uas_id);
    if (!protocol::IsValidUasId(uas_id)) {
      if (uas_id_invalid_log_limiter.ShouldLog(now)) {
        (*logger)->Warn("受控设备 Basic ID 的 uas_id 非法，身份未就绪: \"" +
                        uas_id + "\"");
      }
      return;
    }
    switch (state_store.UpdateDeviceId(uas_id)) {
      case state::DeviceIdUpdate::kUnchanged:
        break;
      case state::DeviceIdUpdate::kAccepted:
        if (controlled_device->type == device::Type::kCnsBox) {
          if (auto product = protocol::CnsBoxProductFrom(uas_id)) {
            state_store.UpdateProduct(*product);
          } else {
            (*logger)->Warn(
                "主控箱 device_id 缺少 DCDWCNS1 产品前缀，检查固件身份配置: " +
                uas_id);
          }
        }
        (*logger)->Info("受控设备身份就绪: device_id=" + uas_id);
        break;
      case state::DeviceIdUpdate::kConflict:
        // 不静默切 topic：先让旧身份正确离线，再清空状态重新识别设备。
        (*logger)->Error("受控设备身份冲突，旧身份下线并重新识别: 新 uas_id=" +
                         uas_id);
        close_mqtt_session(/*publish_offline=*/true);
        state_store.ResetDeviceState();
        controlled_device.reset();
        qgc_udp_bridge.reset();
        px4_basic_id_requests_sent = 0;
        break;
    }
  };

  while (!g_exit_requested) {
    auto now = std::chrono::steady_clock::now();
    if (!link) {
      if (auto attempt = discovery.TryTakeResult()) {
        if (attempt->found) {
          active_serial_device = std::move(attempt->found->device);
          pending_first_message = attempt->found->first_message;
          link.emplace(std::move(attempt->found->link));
          silence_watchdog.ObserveValidFrame(std::chrono::steady_clock::now());
          if (ever_connected_to_device) {
            (*logger)->Info("已重新发现受控设备 MAVLink 串口，链路恢复: " +
                            active_serial_device);
          } else {
            (*logger)->Info("已发现受控设备 MAVLink 串口: " +
                            active_serial_device);
            ever_connected_to_device = true;
          }
        } else if (!g_exit_requested) {
          const auto discovery_finished = std::chrono::steady_clock::now();
          if (discovery_log_limiter.ShouldLog(discovery_finished)) {
            std::string warning = "尚未发现受控设备 MAVLink 串口，继续等待";
            if (!attempt->failures.empty()) {
              warning += "; 探测失败: " +
                         uart::FormatCandidateFailures(attempt->failures);
            }
            (*logger)->Warn(warning);
          }
          next_discovery = discovery_finished + kDiscoveryRetryInterval;
        }
      }

      now = std::chrono::steady_clock::now();
      if (!link && !discovery.IsRunning() && now >= next_discovery) {
        (void)discovery.Start(app_config->serial.device,
                              app_config->serial.baud,
                              kDiscoveryProbeTimeout);
      }
    }

    now = std::chrono::steady_clock::now();
    if (link) {
      if (pending_first_message) {
        const auto first_message = *pending_first_message;
        pending_first_message.reset();
        process_mavlink_message(first_message, now);
      } else {
        const auto serial_wait =
            (qgc_udp_bridge ||
             (app_config->px4_realtime.enabled && controlled_device &&
              controlled_device->type == device::Type::kFlightController))
                                     ? std::chrono::milliseconds(5)
                                     : std::chrono::milliseconds(100);
        auto received = link->ReceiveMessage(serial_wait);
        if (!received) {
          mark_link_disconnected();
        } else if (*received) {
          process_mavlink_message(**received, now);
        }
      }
      if (link && qgc_udp_bridge) {
        for (const auto& message :
             qgc_udp_bridge->PollIncoming(std::chrono::steady_clock::now())) {
          if (!link->SendMessage(message)) {
            mark_link_disconnected();
            break;
          }
        }
        while (qgc_udp_bridge) {
          const auto event = qgc_udp_bridge->TakePeerEvent();
          if (!event) {
            break;
          }
          if (event->type ==
              network::QgcUdpBridge::PeerEventType::kConnected) {
            (*logger)->Info("QGC控制端已锁定: " + event->endpoint);
          } else if (event->type ==
                     network::QgcUdpBridge::PeerEventType::kSwitched) {
            (*logger)->Info("QGC控制端已自动切换: " + event->endpoint);
          } else {
            (*logger)->Warn("QGC控制权已释放，恢复双入口竞选: " +
                            event->endpoint);
          }
        }
      }
      if (link && silence_watchdog.Expired(std::chrono::steady_clock::now())) {
        (*logger)->Error("连续10秒未收到合法MAVLink帧");
        mark_link_disconnected();
      }
    }

    now = std::chrono::steady_clock::now();
    const auto rpi_system_id =
        control_command::LearnedControlledSystemId(controlled_device);
    if (link && rpi_system_id &&
        now - last_heartbeat >= app_config->runtime.heartbeat_interval) {
      mavlink_message_t heartbeat = BuildHeartbeat(*rpi_system_id);
      if (!link->SendMessage(heartbeat)) {
        mark_link_disconnected();
      } else {
        last_heartbeat = now;
      }
    }

    if (link && rpi_system_id &&
        now - last_cellular_heartbeat >= app_config->cellular.heartbeat_interval) {
      const auto cellular_snapshot = cellular::ReadStatusSnapshot(
          app_config->cellular.status_snapshot_path,
          app_config->cellular.status_snapshot_max_age);
      const auto cellular_status = cellular::FromSnapshot(cellular_snapshot);
      const auto boot_ms = static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
      const auto heartbeat = cellular::BuildRpiCellularHeartbeat(
          cellular_status, *rpi_system_id, kComponentId, boot_ms);
      if (!link->SendMessage(heartbeat)) {
        mark_link_disconnected();
      } else {
        last_cellular_heartbeat = now;
      }
    }

    // PX4 在普通遥测链路上既不主动发 BASIC_ID 也不主动发 AUTOPILOT_VERSION，
    // 两者都要周期请求。两条请求的停止条件必须各自独立：device_id 来自 Basic ID，
    // 会先于 AUTOPILOT_VERSION 到达，若共用 !device_id 作为条件，产品与版本
    // 元数据将永远采集不到(设计文档 §3.2)。
    // 用 else if 错开发送：MAV_CMD_REQUEST_MESSAGE 的 COMMAND_ACK 不带被请求的
    // 消息号，同一轮连发会让应答无法归属到具体某条请求。
    const auto identity_snapshot = state_store.Snapshot();
    if (link && controlled_device &&
        controlled_device->type == device::Type::kFlightController &&
        rpi_system_id) {
      if (!identity_snapshot.device_id &&
          now - last_px4_basic_id_request >= kPx4IdentityRequestInterval) {
        const auto request = protocol::BuildMessageRequest(
            MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID, *rpi_system_id, kComponentId,
            controlled_device->endpoint.system_id,
            controlled_device->endpoint.component_id);
        if (!link->SendMessage(request)) {
          mark_link_disconnected();
        } else {
          last_px4_basic_id_request = now;
          ++px4_basic_id_requests_sent;
        }
      } else if (!identity_snapshot.autopilot_version_received &&
                 now - last_px4_version_request >= kPx4IdentityRequestInterval) {
        const auto request = protocol::BuildMessageRequest(
            MAVLINK_MSG_ID_AUTOPILOT_VERSION, *rpi_system_id, kComponentId,
            controlled_device->endpoint.system_id,
            controlled_device->endpoint.component_id);
        if (!link->SendMessage(request)) {
          mark_link_disconnected();
        } else {
          last_px4_version_request = now;
        }
      }
    }

    if (link && controlled_device && !identity_snapshot.device_id) {
      const char* waiting_reason = nullptr;
      if (controlled_device->type != device::Type::kFlightController) {
        waiting_reason = "等待受控设备 Basic ID，身份未就绪";
      } else if (px4_basic_id_requests_sent > 1) {
        waiting_reason =
            "PX4 未响应 Basic ID 请求，身份未就绪；确认 PX4 已配置 UAS ID";
      }
      if (waiting_reason != nullptr && identity_wait_log_limiter.ShouldLog(now)) {
        (*logger)->Warn(waiting_reason);
      }
    }

    auto mqtt_snapshot = state_store.Snapshot();
    if (mqtt_client && active_device_id && mqtt_snapshot.device_id &&
        *mqtt_snapshot.device_id != *active_device_id) {
      close_mqtt_session(/*publish_offline=*/true);
    }

    if (!mqtt_client) {
      auto snapshot = mqtt_snapshot;
      if (snapshot.device_id && snapshot.device_type) {
        if (!registration::IsValidDeviceIdentity(
                app_config->mqtt.connection.client_id_prefix,
                *snapshot.device_id)) {
          (*logger)->Warn(
              "device_id或MQTT Client ID前缀含非法字符，暂不连接MQTT");
        } else {
          const auto& topics = app_config->mqtt.topics;
          registration_topic = mqtt::BuildRegistrationTopic(
              topics.topic_namespace, *snapshot.device_id,
              topics.registration.suffix);
          telemetry_topic = mqtt::BuildTelemetryTopic(
              topics.topic_namespace, *snapshot.device_id,
              topics.telemetry.suffix);
          px4_realtime_topic = mqtt::BuildPx4RealtimeTopic(
              topics.topic_namespace, *snapshot.device_id);
          px4_latency_probe_topic = mqtt::BuildPx4LatencyProbeTopic(
              topics.topic_namespace, *snapshot.device_id);
          px4_latency_ack_topic = mqtt::BuildPx4LatencyAckTopic(
              topics.topic_namespace, *snapshot.device_id);
          config_set_topic = mqtt::BuildConfigSetTopic(
              topics.topic_namespace, *snapshot.device_id,
              topics.config_set.suffix);
          config_ack_topic = mqtt::BuildConfigAckTopic(
              topics.topic_namespace, *snapshot.device_id,
              topics.config_ack.suffix);
          control_set_topic = mqtt::BuildControlSetTopic(
              topics.topic_namespace, *snapshot.device_id,
              topics.control_set.suffix);
          control_ack_topic = mqtt::BuildControlAckTopic(
              topics.topic_namespace, *snapshot.device_id,
              topics.control_ack.suffix);
          offline_payload = registration::BuildOfflinePayload(
              *snapshot.device_id, *snapshot.device_type);
          mqtt_client = mqtt::MqttClient::Open({
            .broker_host = app_config->mqtt.connection.host,
            .broker_port = app_config->mqtt.connection.port,
            .client_id = registration::BuildClientId(
                app_config->mqtt.connection.client_id_prefix,
                *snapshot.device_id),
            .username = app_config->mqtt.auth.username,
            .password = app_config->mqtt.auth.password,
            .keepalive_seconds = app_config->mqtt.connection.keepalive_seconds,
            .reconnect_delay_seconds = app_config->mqtt.connection.reconnect.delay_seconds,
            .reconnect_delay_max_seconds =
                app_config->mqtt.connection.reconnect.delay_max_seconds,
            .will = {
                .topic = registration_topic,
                .payload = offline_payload,
                .qos = topics.registration.qos,
                .retain = true,
            },
            .subscriptions = [&]() {
              std::vector<std::pair<std::string, int>> subscriptions{
                  {config_set_topic, topics.config_set.qos},
                  {control_set_topic, topics.control_set.qos}};
              if (snapshot.device_type &&
                  *snapshot.device_type == device::Type::kFlightController) {
                subscriptions.emplace_back(px4_latency_probe_topic, 0);
              }
              return subscriptions;
            }(),
          }, **logger);
          if (mqtt_client) {
            active_device_id = *snapshot.device_id;
            (*logger)->Info("MQTT连接中: broker=" + app_config->mqtt.connection.host +
                            " topic=" + telemetry_topic);
            if (controlled_device &&
                controlled_device->type == device::Type::kFlightController &&
                app_config->px4_realtime.enabled) {
              (*logger)->Info(
                  "PX4实时遥测已启用: interval=" +
                  std::to_string(
                      app_config->px4_realtime.publish_interval.count()) +
                  "ms topic=" + px4_realtime_topic);
            }
          } else {
            (*logger)->Warn("MQTT客户端创建失败，下一轮重试");
          }
        }
      }
    }

    if (mqtt_client && active_device_id && mqtt_snapshot.device_id &&
        *active_device_id == *mqtt_snapshot.device_id) {
      auto snapshot = state_store.Snapshot();
      if (snapshot.device_id && snapshot.device_type) {
        const auto online_payload = registration::BuildOnlinePayload(
            MakeOnlineRegistration(snapshot,
                                   app_config->identity.school_name));
        if (registration_state.ShouldPublish(mqtt_client->IsConnected(), online_payload)) {
          const auto& registration_config = app_config->mqtt.topics.registration;
          if (mqtt_client->Publish(registration_topic, online_payload, registration_config.qos,
                                   /*retain=*/true)) {
            registration_state.MarkPublished(online_payload);
          } else {
            (*logger)->Warn("MQTT注册发布失败，下一轮重试");
          }
        }
      }
    }

    if (mqtt_client && active_device_id && mqtt_snapshot.device_id &&
        *active_device_id == *mqtt_snapshot.device_id) {
      if (auto message = mqtt_client->TryPopMessage()) {
        if (message->topic == px4_latency_probe_topic &&
            controlled_device &&
            controlled_device->type == device::Type::kFlightController) {
          const auto probe =
              latency::ParsePx4LatencyProbe(message->payload, *active_device_id);
          if (!probe) {
            (*logger)->Warn("丢弃非法PX4链路探测: " + probe.error());
          } else if (!mqtt_client->Publish(
                         px4_latency_ack_topic,
                         latency::BuildPx4LatencyAck(*probe).dump(),
                         /*qos=*/0, /*retain=*/false)) {
            (*logger)->Warn("PX4链路探测ACK发布失败");
          }
        } else if (message->topic == config_set_topic && !restart_requested) {
          config_command::CommandProcessResult result;
          auto parsed = config_command::ParseConfigCommand(message->payload);
          (*logger)->Info(
              "收到配置命令: command_id=" +
              (parsed ? parsed->command_id : std::string{"<解析失败>"}));
          if (!parsed) {
            result = config_command::ProcessConfigCommand(
                message->payload, nlohmann::json::object(),
                [](const nlohmann::json&) ->
                    std::expected<void, config_command::CommandError> {
                  return std::unexpected(config_command::CommandError{
                      .code = "config_write_failed", .message = "不可达的持久化分支"});
                });
          } else if (control_transaction.HasPending()) {
            result.ack = config_command::BuildRejectedAck(
                parsed->command_id,
                {.code = "control_command_busy",
                 .message = "存在未结束的飞控命令，暂不修改运行参数"});
          } else {
            auto current = config_command::LoadConfigJson(config_path);
            if (!current) {
              result.ack = config_command::BuildRejectedAck(parsed->command_id, current.error());
            } else {
              result = config_command::ProcessConfigCommand(
                  message->payload, *current,
                  [&](const nlohmann::json& candidate) {
                    return config_command::PersistConfig(*writer_options, config_path, candidate);
                  });
            }
          }

          (*logger)->Info(
              "配置命令回执: command_id=" + result.ack.value("command_id", std::string{}) +
              " status=" + result.ack.value("status", std::string{}));
          const bool acked = mqtt_client->PublishAndWait(
              config_ack_topic, result.ack.dump(), app_config->mqtt.topics.config_ack.qos,
              /*retain=*/false, std::chrono::seconds(2));
          if (!acked) {
            (*logger)->Warn("配置命令ACK发布失败或超时");
          }
          if (result.should_exit) {
            restart_requested = true;
          }
        } else if (message->topic == control_set_topic && !restart_requested) {
          auto command = control_command::Parse(message->payload);
          if (!command) {
            (*logger)->Warn("收到无法解析的飞控命令: " + command.error().code);
            const auto ack = control_command::BuildRejectedAck(
                command.error().command_id, command.error().command, command.error());
            (void)mqtt_client->Publish(control_ack_topic, ack.dump(),
                                       app_config->mqtt.topics.control_ack.qos,
                                       /*retain=*/false);
          } else if (!controlled_device ||
                     controlled_device->type != device::Type::kCnsBox) {
            const auto ack = control_command::BuildRejectedAck(
                command->command_id, command->command,
                {.code = "unsupported_device_type",
                 .message =
                     "当前命令属于主控箱私有协议，不能发送给 PX4 飞控"});
            (void)mqtt_client->Publish(
                control_ack_topic, ack.dump(),
                app_config->mqtt.topics.control_ack.qos, /*retain=*/false);
          } else {
            (*logger)->Info("收到飞控命令: command_id=" + command->command_id +
                            " command=" + command->command);
            const auto submission = control_transaction.Submit(*command, now);
            if (submission.ack) {
              (void)mqtt_client->Publish(control_ack_topic, submission.ack->dump(),
                                         app_config->mqtt.topics.control_ack.qos,
                                         /*retain=*/false);
            } else if (submission.should_send_to_mcu && !link) {
              (void)control_transaction.HandleLocalFailure(
                  {.code = "device_link_unavailable",
                   .message = "受控设备串口链路不可用"});
            } else if (submission.should_send_to_mcu && !rpi_system_id) {
              (void)control_transaction.HandleLocalFailure(
                  {.code = "device_identity_unknown",
                   .message = "尚未识别受控设备心跳"});
            } else if (submission.should_send_to_mcu) {
              const auto mavlink_message = control_command::EncodeCommandLong(
                  *command, *rpi_system_id, kComponentId,
                  controlled_device->endpoint.system_id,
                  controlled_device->endpoint.component_id);
              if (!link->SendMessage(mavlink_message)) {
                mark_link_disconnected();
              } else {
                const auto pending_ack = control_command::BuildPendingAck(*command);
                if (!mqtt_client->Publish(control_ack_topic, pending_ack.dump(),
                                          app_config->mqtt.topics.control_ack.qos,
                                          /*retain=*/false)) {
                  (*logger)->Warn("飞控命令执行中回执发布失败: command_id=" +
                                  command->command_id);
                }
              }
            }
          }
        } else {
          (*logger)->Warn("忽略非本设备命令topic: " + message->topic);
        }
      }
    }

    (void)control_transaction.CheckTimeout(now);
    if (mqtt_client && control_transaction.PendingAck() != nullptr) {
      const bool published = control_transaction.PendingAckIsFinal()
                                 ? mqtt_client->PublishAndWait(
                                       control_ack_topic,
                                       control_transaction.PendingAck()->dump(),
                                       app_config->mqtt.topics.control_ack.qos,
                                       /*retain=*/false, std::chrono::seconds(2))
                                 : mqtt_client->Publish(
                                       control_ack_topic,
                                       control_transaction.PendingAck()->dump(),
                                       app_config->mqtt.topics.control_ack.qos,
                                       /*retain=*/false);
      if (published) {
        control_transaction.ConfirmAckPublished();
      }
    }

    if (restart_requested && !control_transaction.HasPending()) {
      break;
    }

    if (mqtt_client && active_device_id && mqtt_snapshot.device_id &&
        *active_device_id == *mqtt_snapshot.device_id &&
        mqtt_client->IsConnected() &&
        now - last_telemetry_publish >= app_config->runtime.telemetry_publish_interval) {
      const std::string json_str =
          payload::ToJson(
              state_store.Snapshot(), app_config->identity.school_name,
              cellular::ReadStatusSnapshot(app_config->cellular.status_snapshot_path,
                                           app_config->cellular.status_snapshot_max_age)).dump();
      // retain=false：遥测是按节拍刷新的实时值，不是设备状态的权威存档。
      // retained 遥测会让新订阅者立刻收到最后一帧，却分不清那是实时数据还是
      // 设备掉电前的陈旧快照；设备是否在线由 registration topic 的 retained
      // online/offline 表达，不该由遥测兼任。
      if (!mqtt_client->Publish(telemetry_topic, json_str,
                                app_config->mqtt.topics.telemetry.qos,
                                /*retain=*/false)) {
        (*logger)->Warn("MQTT发布失败，下个节拍重试");
      }
      last_telemetry_publish = now;
    }

    if (app_config->px4_realtime.enabled && mqtt_client &&
        controlled_device &&
        controlled_device->type == device::Type::kFlightController &&
        active_device_id && mqtt_snapshot.device_id &&
        *active_device_id == *mqtt_snapshot.device_id &&
        mqtt_client->IsConnected() &&
        now - last_px4_realtime_publish >=
            app_config->px4_realtime.publish_interval) {
      const auto frame = payload::ToPx4RealtimeJson(
          state_store.Snapshot(), px4_realtime_sequence++);
      // 高频帧采用 QoS 0 且不 retain。失败时直接等下一帧，避免重传旧数据积累延迟。
      if (!mqtt_client->Publish(px4_realtime_topic, frame.dump(),
                                /*qos=*/0, /*retain=*/false)) {
        (*logger)->Warn("PX4实时遥测发布失败，丢弃当前帧");
      }
      last_px4_realtime_publish = now;
    }

    // 放在循环末尾：走到这里说明本轮迭代已完整跑完，没有卡在任何一步。
    if (watchdog_feed_timer &&
        watchdog_feed_timer->ShouldFeed(std::chrono::steady_clock::now())) {
      (void)platform::NotifyWatchdogAlive();
    }

    if (!link) {
      std::this_thread::sleep_for(discovery.IsRunning()
                                     ? std::chrono::milliseconds(10)
                                     : std::chrono::milliseconds(100));
    }
  }

  if (!restart_requested && mqtt_client && mqtt_client->IsConnected() &&
      !registration_topic.empty()) {
    if (!mqtt_client->PublishAndWait(registration_topic, offline_payload,
                                     app_config->mqtt.topics.registration.qos,
                                     /*retain=*/true, std::chrono::seconds(2))) {
      (*logger)->Warn("MQTT离线状态发布超时");
    }
  }
  return EXIT_SUCCESS;
}
