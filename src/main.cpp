/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * M3a 阶段在 M2 双向收发闭环基础上接入遥测解码，M3b 阶段接入扩展帧解码，
 * M3c 阶段接入身份帧解码：收到帧先更新 DCDW 角色号(帧头 sysid)，再依次尝试
 * protocol::DecodeAndStore（标准遥测）和 protocol::DecodeExtensionAndStore
 * （NAMED_VALUE_INT/TUNNEL 扩展帧 + OPEN_DRONE_ID_* 身份帧），
 * 写入 state::StateStore -> 打印解码后的有意义字段做人工验证。
 * 启动时读一次 RPi 本机序列号(V1 过渡期权威键)。M5 阶段接入 MQTT 发布：
 * 身份就绪(state_store.vendor_id 有值)后才连接 broker，连上后按 1Hz 固定
 * 节奏发布遥测；注册扩展在连接/重连和身份元数据变化时发布 retained online，
 * 异常断线由 Last Will 标记 offline，正常退出主动发布 offline。
 */

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "common/mavlink.h"
#include "config/app_config.hpp"
#include "mqtt/mqtt_client.hpp"
#include "mqtt/topic.hpp"
#include "payload/json_serializer.hpp"
#include "protocol/extension_decoder.hpp"
#include "protocol/identity.hpp"
#include "protocol/telemetry_decoder.hpp"
#include "registration/registration_payload.hpp"
#include "registration/registration_state.hpp"
#include "state/state_store.hpp"
#include "uart/mavlink_link.hpp"

namespace {

constexpr std::uint8_t kSystemId = 1;
constexpr std::uint8_t kComponentId = MAV_COMP_ID_ONBOARD_COMPUTER;
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);
constexpr auto kTelemetryPublishInterval = std::chrono::seconds(1);

volatile std::sig_atomic_t g_exit_requested = 0;

void HandleExitSignal(int /*signal*/) { g_exit_requested = 1; }

/// RPi 自己的 HEARTBEAT：它不是飞控，所以 autopilot=MAV_AUTOPILOT_INVALID。
mavlink_message_t BuildHeartbeat() {
  mavlink_message_t msg{};
  mavlink_msg_heartbeat_pack(kSystemId, kComponentId, &msg, MAV_TYPE_ONBOARD_CONTROLLER,
                              MAV_AUTOPILOT_INVALID, /*base_mode=*/0, /*custom_mode=*/0,
                              MAV_STATE_ACTIVE);
  return msg;
}

/// 按刚解出来的这条帧的 msgid，打印 state_store 里对应字段的最新值——
/// 只是给人看的调试日志，不是解码逻辑本身（解码逻辑在 protocol::DecodeAndStore 里）。
void LogTelemetry(std::uint32_t msgid, const state::TelemetryState& snapshot) {
  switch (msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT:
      if (snapshot.heartbeat) {
        std::cout << "HEARTBEAT: type=" << static_cast<int>(snapshot.heartbeat->type)
                  << " system_status=" << static_cast<int>(snapshot.heartbeat->system_status)
                  << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_GPS_RAW_INT:
      if (snapshot.gps_raw_int) {
        std::cout << "GPS_RAW_INT: fix_type=" << static_cast<int>(snapshot.gps_raw_int->fix_type)
                  << " lat=" << snapshot.gps_raw_int->lat
                  << " lon=" << snapshot.gps_raw_int->lon << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_ATTITUDE:
      if (snapshot.attitude) {
        std::cout << "ATTITUDE: roll=" << snapshot.attitude->roll
                  << " pitch=" << snapshot.attitude->pitch
                  << " yaw=" << snapshot.attitude->yaw << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
      if (snapshot.global_position_int) {
        std::cout << "GLOBAL_POSITION_INT: lat=" << snapshot.global_position_int->lat
                  << " lon=" << snapshot.global_position_int->lon
                  << " relative_alt=" << snapshot.global_position_int->relative_alt << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_SYS_STATUS:
      if (snapshot.sys_status) {
        std::cout << "SYS_STATUS: voltage_battery=" << snapshot.sys_status->voltage_battery
                  << " battery_remaining="
                  << static_cast<int>(snapshot.sys_status->battery_remaining) << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_BATTERY_STATUS:
      for (std::size_t i = 0; i < snapshot.battery_status.size(); ++i) {
        if (snapshot.battery_status[i]) {
          std::cout << "BATTERY_STATUS[" << i
                    << "]: voltages[0]=" << snapshot.battery_status[i]->voltages[0]
                    << " battery_remaining="
                    << static_cast<int>(snapshot.battery_status[i]->battery_remaining)
                    << std::endl;
        }
      }
      break;
    case MAVLINK_MSG_ID_SCALED_PRESSURE:
      if (snapshot.scaled_pressure) {
        std::cout << "SCALED_PRESSURE: press_abs=" << snapshot.scaled_pressure->press_abs
                  << " temperature=" << snapshot.scaled_pressure->temperature << std::endl;
      }
      break;
    default:
      break;
  }
}

/// 跟 LogTelemetry 同样的定位：按扩展帧(M3b)/身份帧(M3c)的 msgid/内部语义
/// 打印 state_store 里对应字段的最新值，供真机人工验证；解码逻辑本身在
/// protocol::DecodeExtensionAndStore 里，这里只打印。
void LogExtension(std::uint32_t msgid, const state::TelemetryState& snapshot) {
  switch (msgid) {
    case MAVLINK_MSG_ID_NAMED_VALUE_INT:
      if (snapshot.module_status) {
        std::cout << "MODSTAT: [0]=" << static_cast<int>((*snapshot.module_status)[0])
                  << " [13]=" << static_cast<int>((*snapshot.module_status)[13]) << std::endl;
      }
      if (snapshot.motor_pwm) {
        std::cout << "MOTOR: duty=[" << static_cast<int>(snapshot.motor_pwm->duty_percent[0])
                  << "," << static_cast<int>(snapshot.motor_pwm->duty_percent[1]) << ","
                  << static_cast<int>(snapshot.motor_pwm->duty_percent[2]) << ","
                  << static_cast<int>(snapshot.motor_pwm->duty_percent[3])
                  << "] run_state=" << snapshot.motor_pwm->run_state
                  << " speed_level=" << static_cast<int>(snapshot.motor_pwm->speed_level)
                  << std::endl;
      }
      if (snapshot.gnss_sat) {
        std::cout << "GNSS_SAT: gps_visible=" << static_cast<int>(snapshot.gnss_sat->gps_visible)
                  << " gps_used=" << static_cast<int>(snapshot.gnss_sat->gps_used) << std::endl;
      }
      if (snapshot.env_humidity) {
        std::cout << "HUMIDITY: relative_humidity_x10=" << snapshot.env_humidity->relative_humidity_x10
                  << std::endl;
      }
      if (snapshot.lora_status) {
        std::cout << "LORASTAT: loss_rate_x10=" << snapshot.lora_status->loss_rate_x10
                  << " node_id=" << static_cast<int>(snapshot.lora_status->node_id)
                  << " present=" << snapshot.lora_status->present
                  << " link_state=" << static_cast<int>(snapshot.lora_status->link_state)
                  << std::endl;
      }
      if (snapshot.remote_id_status) {
        std::cout << "RIDSTAT: location_count=" << snapshot.remote_id_status->location_count
                  << " error_count=" << snapshot.remote_id_status->error_count
                  << " last_success_ms=" << snapshot.remote_id_status->last_success_ms
                  << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_TUNNEL:
      if (snapshot.alarm_table) {
        std::cout << "ALARM_TABLE: active_count=" << snapshot.alarm_table->active_count
                  << std::endl;
      }
      if (snapshot.message_log) {
        std::cout << "MESSAGE_LOG: latest_seq=" << snapshot.message_log->latest_seq
                  << " count=" << snapshot.message_log->count << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_BASIC_ID:
      if (snapshot.open_drone_id_basic_id) {
        std::cout << "OPEN_DRONE_ID_BASIC_ID: id_type="
                  << static_cast<int>(snapshot.open_drone_id_basic_id->id_type)
                  << " ua_type=" << static_cast<int>(snapshot.open_drone_id_basic_id->ua_type)
                  << std::endl;
      }
      if (snapshot.vendor_id) {
        std::cout << "vendor_id=" << *snapshot.vendor_id << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_LOCATION:
      if (snapshot.open_drone_id_location) {
        std::cout << "OPEN_DRONE_ID_LOCATION: lat=" << snapshot.open_drone_id_location->latitude
                  << " lon=" << snapshot.open_drone_id_location->longitude << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SYSTEM:
      if (snapshot.open_drone_id_system) {
        std::cout << "OPEN_DRONE_ID_SYSTEM: operator_lat="
                  << snapshot.open_drone_id_system->operator_latitude << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_OPERATOR_ID:
      if (snapshot.open_drone_id_operator_id) {
        std::cout << "OPEN_DRONE_ID_OPERATOR_ID: operator_id_type="
                  << static_cast<int>(snapshot.open_drone_id_operator_id->operator_id_type)
                  << std::endl;
      }
      break;
    case MAVLINK_MSG_ID_OPEN_DRONE_ID_SELF_ID:
      if (snapshot.open_drone_id_self_id) {
        std::cout << "OPEN_DRONE_ID_SELF_ID: description_type="
                  << static_cast<int>(snapshot.open_drone_id_self_id->description_type)
                  << std::endl;
      }
      break;
    default:
      break;
  }
}

/// 每处理完一帧调用一次，把当前完整快照序列化成JSON打印到控制台，
/// 供真机演示/联调时人工核对字段可读性——不是解码逻辑，纯打印。
void LogJsonPayload(const state::TelemetryState& state, const std::string& school_name) {
  std::cout << payload::ToJson(state, school_name).dump(2) << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleExitSignal);
  std::signal(SIGTERM, HandleExitSignal);

  const std::string config_path = argc > 1 ? argv[1] : "config/config.json";

  auto app_config = config::LoadAppConfig(config_path);
  if (!app_config) {
    std::cerr << "读取配置失败: " << config_path << ": "
              << config::ConfigErrorMessage(app_config.error()) << "\n";
    return EXIT_FAILURE;
  }

  auto link = uart::MavlinkLink::Open(app_config->serial.device, app_config->serial.baud);
  if (!link) {
    std::cerr << "打开串口失败: " << app_config->serial.device << "\n";
    return EXIT_FAILURE;
  }

  std::cout << "cns_rpi M3c 启动，串口=" << app_config->serial.device
            << " 波特率=" << app_config->serial.baud << std::endl;

  state::StateStore state_store;
  if (auto serial = protocol::ReadRpiSerial()) {
    state_store.UpdateRpiSerial(*serial);
  }
  auto last_heartbeat = std::chrono::steady_clock::now();
  std::optional<mqtt::MqttClient> mqtt_client;
  registration::RegistrationState registration_state;
  std::optional<std::string> active_vendor_id;
  std::string registration_topic;
  std::string offline_payload;
  std::string telemetry_topic;
  auto last_telemetry_publish = std::chrono::steady_clock::now();
  while (!g_exit_requested) {
    if (auto msg = link->ReceiveMessage()) {
      state_store.UpdateDcdwLabel(protocol::FormatDcdwLabel(msg->sysid));
      if (protocol::DecodeAndStore(*msg, state_store)) {
        LogTelemetry(msg->msgid, state_store.Snapshot());
      } else if (protocol::DecodeExtensionAndStore(*msg, state_store)) {
        LogExtension(msg->msgid, state_store.Snapshot());
      }
      LogJsonPayload(state_store.Snapshot(), app_config->identity.school_name);
    }

    auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat >= kHeartbeatInterval) {
      mavlink_message_t heartbeat = BuildHeartbeat();
      link->SendMessage(heartbeat);
      last_heartbeat = now;
    }

    auto mqtt_snapshot = state_store.Snapshot();
    if (mqtt_client && active_vendor_id && mqtt_snapshot.vendor_id &&
        *mqtt_snapshot.vendor_id != *active_vendor_id) {
      mqtt_client->PublishAndWait(registration_topic, offline_payload,
                                  app_config->mqtt.topics.registration.qos,
                                  /*retain=*/true, std::chrono::seconds(2));
      mqtt_client.reset();
      registration_state = registration::RegistrationState{};
      active_vendor_id.reset();
      registration_topic.clear();
      telemetry_topic.clear();
      offline_payload.clear();
    }

    if (!mqtt_client) {
      auto snapshot = mqtt_snapshot;
      if (snapshot.vendor_id) {
        if (!registration::IsValidDeviceIdentity(
                app_config->mqtt.connection.client_id_prefix, *snapshot.vendor_id)) {
          std::cerr << "vendor_id或MQTT Client ID前缀含非法字符，暂不连接MQTT" << std::endl;
          continue;
        }
        const auto& topics = app_config->mqtt.topics;
        registration_topic = mqtt::BuildRegistrationTopic(
            topics.topic_namespace, *snapshot.vendor_id, topics.registration.suffix);
        telemetry_topic = mqtt::BuildTelemetryTopic(
            topics.topic_namespace, *snapshot.vendor_id, topics.telemetry.suffix);
        offline_payload = registration::BuildOfflinePayload(*snapshot.vendor_id);
        mqtt_client = mqtt::MqttClient::Open({
            .broker_host = app_config->mqtt.connection.host,
            .broker_port = app_config->mqtt.connection.port,
            .client_id = registration::BuildClientId(
                app_config->mqtt.connection.client_id_prefix, *snapshot.vendor_id),
            .username = app_config->mqtt.auth.username,
            .password = app_config->mqtt.auth.password,
            .keepalive_seconds = app_config->mqtt.connection.keepalive_seconds,
            .will = {
                .topic = registration_topic,
                .payload = offline_payload,
                .qos = topics.registration.qos,
                .retain = true,
            },
        });
        if (mqtt_client) {
          active_vendor_id = *snapshot.vendor_id;
          std::cout << "MQTT连接中: broker=" << app_config->mqtt.connection.host
                    << " topic=" << telemetry_topic << std::endl;
        } else {
          std::cerr << "MQTT客户端创建失败，下一轮重试" << std::endl;
        }
      }
    }

    if (mqtt_client) {
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
            std::cerr << "MQTT注册发布失败，下一轮重试" << std::endl;
          }
        }
      }
    }

    if (mqtt_client && mqtt_client->IsConnected() &&
        now - last_telemetry_publish >= kTelemetryPublishInterval) {
      std::string json_str =
          payload::ToJson(state_store.Snapshot(), app_config->identity.school_name).dump();
      if (!mqtt_client->Publish(telemetry_topic, json_str,
                                app_config->mqtt.topics.telemetry.qos, /*retain=*/true)) {
        std::cerr << "MQTT发布失败，下个节拍重试" << std::endl;
      }
      last_telemetry_publish = now;
    }
  }

  if (mqtt_client && mqtt_client->IsConnected() && !registration_topic.empty()) {
    if (!mqtt_client->PublishAndWait(registration_topic, offline_payload,
                                     app_config->mqtt.topics.registration.qos,
                                     /*retain=*/true, std::chrono::seconds(2))) {
      std::cerr << "MQTT离线状态发布超时" << std::endl;
    }
  }
  return EXIT_SUCCESS;
}
