/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * 收到 MAVLink 帧只更新 state::StateStore，不在逐帧路径输出业务日志；标准遥测
 * 解码失败时再尝试扩展帧和身份帧解码。日志格式和输出目标由 logging::Logger 独立负责，
 * main 仅在配置成功后创建并向业务模块传递同一实例。
 * 启动时读取 RPi 本机序列号作为 V1 过渡期权威键；设备身份就绪后连接 broker，
 * 按固定节拍发布遥测，并在 Publish 成功后记录同一份紧凑 JSON，失败时只记录警告。
 * 注册状态在连接、重连和身份元数据变化时 retained 发布；异常断线由 Last Will 标记
 * offline，正常退出主动发布 offline。
 */

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
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
#include "config_command/config_store.hpp"
#include "config_command/command_parser.hpp"
#include "config_command/command_processor.hpp"
#include "config_command/config_updater.hpp"
#include "control_command/control_command.hpp"
#include "control_command/control_endpoint.hpp"
#include "control_command/control_transaction.hpp"
#include "logging/logger.hpp"
#include "mqtt/mqtt_client.hpp"
#include "mqtt/topic.hpp"
#include "payload/json_serializer.hpp"
#include "platform/systemd_watchdog.hpp"
#include "protocol/extension_decoder.hpp"
#include "protocol/identity.hpp"
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

  (*logger)->Info("cns_rpi M3c 启动，串口配置=" + app_config->serial.device +
                  " 波特率=" + std::to_string(app_config->serial.baud));

  state::StateStore state_store;
  if (auto serial = protocol::ReadRpiSerial()) {
    state_store.UpdateRpiSerial(*serial);
  }
  auto last_heartbeat = std::chrono::steady_clock::now();
  auto last_cellular_heartbeat = std::chrono::steady_clock::now();
  std::optional<mqtt::MqttClient> mqtt_client;
  registration::RegistrationState registration_state;
  std::optional<std::string> active_vendor_id;
  std::string registration_topic;
  std::string offline_payload;
  std::string telemetry_topic;
  std::string config_set_topic;
  std::string config_ack_topic;
  std::string control_set_topic;
  std::string control_ack_topic;
  control_command::ControlTransaction control_transaction(kControlAckTimeout);
  std::optional<control_command::MavlinkEndpoint> stm32_endpoint;
  std::optional<uart::MavlinkLink> link;
  std::string active_serial_device;
  std::optional<mavlink_message_t> pending_first_message;
  uart::AsyncMavlinkDiscovery discovery;
  uart::DiscoveryLogLimiter discovery_log_limiter(kDiscoveryWarningInterval);
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
  bool ever_connected_to_stm32 = false;
  bool restart_requested = false;
  auto last_telemetry_publish = std::chrono::steady_clock::now();

  auto mark_link_disconnected = [&] {
    (*logger)->Error("STM32串口断开: " + active_serial_device);
    link.reset();
    active_serial_device.clear();
    pending_first_message.reset();
    stm32_endpoint.reset();
    silence_watchdog.Reset();
    next_discovery = std::chrono::steady_clock::now();
    if (control_transaction.HasPending()) {
      (void)control_transaction.HandleLocalFailure(
          {.code = "stm32_link_unavailable", .message = "STM32串口链路不可用"});
    }
  };

  auto process_mavlink_message = [&](const mavlink_message_t& message,
                                     std::chrono::steady_clock::time_point now) {
    silence_watchdog.ObserveValidFrame(now);
    stm32_endpoint =
        control_command::ObserveFlightControllerHeartbeat(message, stm32_endpoint);
    const auto rpi_system_id = control_command::LearnedSystemId(stm32_endpoint);
    if (rpi_system_id && mqtt_client &&
        control_command::IsExpectedCommandAck(message, *stm32_endpoint, *rpi_system_id,
                                              kComponentId)) {
      mavlink_command_ack_t ack{};
      mavlink_msg_command_ack_decode(&message, &ack);
      (void)control_transaction.HandleMavlinkAck(
          ack.command, ack.result, ack.progress, ack.result_param2, now);
    }
    state_store.UpdateDcdwLabel(protocol::FormatDcdwLabel(message.sysid));
    if (!protocol::DecodeAndStore(message, state_store)) {
      (void)protocol::DecodeExtensionAndStore(message, state_store);
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
          if (ever_connected_to_stm32) {
            (*logger)->Info("已重新发现STM32 MAVLink串口，链路恢复: " +
                            active_serial_device);
          } else {
            (*logger)->Info("已发现STM32 MAVLink串口: " + active_serial_device);
            ever_connected_to_stm32 = true;
          }
        } else if (!g_exit_requested) {
          const auto discovery_finished = std::chrono::steady_clock::now();
          if (discovery_log_limiter.ShouldLog(discovery_finished)) {
            std::string warning = "尚未发现STM32 MAVLink串口，继续等待";
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
        auto received = link->ReceiveMessage();
        if (!received) {
          mark_link_disconnected();
        } else if (*received) {
          process_mavlink_message(**received, now);
        }
      }
      if (link && silence_watchdog.Expired(std::chrono::steady_clock::now())) {
        (*logger)->Error("连续10秒未收到合法MAVLink帧");
        mark_link_disconnected();
      }
    }

    now = std::chrono::steady_clock::now();
    const auto rpi_system_id = control_command::LearnedSystemId(stm32_endpoint);
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

    auto mqtt_snapshot = state_store.Snapshot();
    if (mqtt_client && active_vendor_id && mqtt_snapshot.vendor_id &&
        *mqtt_snapshot.vendor_id != *active_vendor_id) {
      if (control_transaction.HasPending()) {
        (void)control_transaction.HandleLocalFailure(
            {.code = "device_identity_changed", .message = "设备身份发生变化，取消当前控制事务"});
      } else {
        control_transaction.ResetIdentityScope();
        mqtt_client->PublishAndWait(registration_topic, offline_payload,
                                    app_config->mqtt.topics.registration.qos,
                                    /*retain=*/true, std::chrono::seconds(2));
        mqtt_client.reset();
        registration_state = registration::RegistrationState{};
        active_vendor_id.reset();
        registration_topic.clear();
        telemetry_topic.clear();
        config_set_topic.clear();
        config_ack_topic.clear();
        control_set_topic.clear();
        control_ack_topic.clear();
        offline_payload.clear();
      }
    }

    if (!mqtt_client) {
      auto snapshot = mqtt_snapshot;
      if (snapshot.vendor_id) {
        if (!registration::IsValidDeviceIdentity(
                app_config->mqtt.connection.client_id_prefix, *snapshot.vendor_id)) {
          (*logger)->Warn("vendor_id或MQTT Client ID前缀含非法字符，暂不连接MQTT");
        } else {
          const auto& topics = app_config->mqtt.topics;
          registration_topic = mqtt::BuildRegistrationTopic(
              topics.topic_namespace, *snapshot.vendor_id, topics.registration.suffix);
          telemetry_topic = mqtt::BuildTelemetryTopic(
              topics.topic_namespace, *snapshot.vendor_id, topics.telemetry.suffix);
          config_set_topic = mqtt::BuildConfigSetTopic(
              topics.topic_namespace, *snapshot.vendor_id, topics.config_set.suffix);
          config_ack_topic = mqtt::BuildConfigAckTopic(
              topics.topic_namespace, *snapshot.vendor_id, topics.config_ack.suffix);
          control_set_topic = mqtt::BuildControlSetTopic(
              topics.topic_namespace, *snapshot.vendor_id, topics.control_set.suffix);
          control_ack_topic = mqtt::BuildControlAckTopic(
              topics.topic_namespace, *snapshot.vendor_id, topics.control_ack.suffix);
          offline_payload = registration::BuildOfflinePayload(*snapshot.vendor_id);
          mqtt_client = mqtt::MqttClient::Open({
            .broker_host = app_config->mqtt.connection.host,
            .broker_port = app_config->mqtt.connection.port,
            .client_id = registration::BuildClientId(
                app_config->mqtt.connection.client_id_prefix, *snapshot.vendor_id),
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
            .subscriptions = {{config_set_topic, topics.config_set.qos},
                              {control_set_topic, topics.control_set.qos}},
          }, **logger);
          if (mqtt_client) {
            active_vendor_id = *snapshot.vendor_id;
            (*logger)->Info("MQTT连接中: broker=" + app_config->mqtt.connection.host +
                            " topic=" + telemetry_topic);
          } else {
            (*logger)->Warn("MQTT客户端创建失败，下一轮重试");
          }
        }
      }
    }

    if (mqtt_client && active_vendor_id && mqtt_snapshot.vendor_id &&
        *active_vendor_id == *mqtt_snapshot.vendor_id) {
      auto snapshot = state_store.Snapshot();
      if (snapshot.vendor_id) {
        const auto online_payload = registration::BuildOnlinePayload({
            .vendor_id = *snapshot.vendor_id,
            .school_name = app_config->identity.school_name,
            .dcdw_label = snapshot.dcdw_label,
        });
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

    if (mqtt_client && active_vendor_id && mqtt_snapshot.vendor_id &&
        *active_vendor_id == *mqtt_snapshot.vendor_id) {
      if (auto message = mqtt_client->TryPopMessage()) {
        if (message->topic == config_set_topic && !restart_requested) {
          config_command::CommandProcessResult result;
          auto parsed = config_command::ParseConfigCommand(message->payload);
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
            const auto ack = control_command::BuildRejectedAck(
                command.error().command_id, command.error().command, command.error());
            (void)mqtt_client->Publish(control_ack_topic, ack.dump(),
                                       app_config->mqtt.topics.control_ack.qos,
                                       /*retain=*/false);
          } else {
            const auto submission = control_transaction.Submit(*command, now);
            if (submission.ack) {
              (void)mqtt_client->Publish(control_ack_topic, submission.ack->dump(),
                                         app_config->mqtt.topics.control_ack.qos,
                                         /*retain=*/false);
            } else if (submission.should_send_to_mcu && !link) {
              (void)control_transaction.HandleLocalFailure(
                  {.code = "stm32_link_unavailable", .message = "STM32串口链路不可用"});
            } else if (submission.should_send_to_mcu && !rpi_system_id) {
              (void)control_transaction.HandleLocalFailure(
                  {.code = "stm32_identity_unknown", .message = "尚未收到STM32心跳"});
            } else if (submission.should_send_to_mcu) {
              const auto mavlink_message = control_command::EncodeCommandLong(
                  *command, *rpi_system_id, kComponentId, stm32_endpoint->system_id,
                  stm32_endpoint->component_id);
              if (!link->SendMessage(mavlink_message)) {
                mark_link_disconnected();
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

    if (mqtt_client && active_vendor_id && mqtt_snapshot.vendor_id &&
        *active_vendor_id == *mqtt_snapshot.vendor_id && mqtt_client->IsConnected() &&
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
      if (mqtt_client->Publish(telemetry_topic, json_str,
                               app_config->mqtt.topics.telemetry.qos, /*retain=*/false)) {
        logging::LogPublishedTelemetry(**logger, json_str);
      } else {
        (*logger)->Warn("MQTT发布失败，下个节拍重试");
      }
      last_telemetry_publish = now;
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
