#pragma once

/**
 * @file deferred_serial_request.hpp
 * @brief F407 端点待复核期间的单槽实验串口动作暂存。
 *
 * @details 只保存进程内请求，不持久化、不生成实际串口帧或 MQTT ACK。
 */

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "experiment/m1_request.hpp"
#include "experiment/pi_link_inspection.hpp"
#include "experiment/uart_echo_request.hpp"

namespace experiment {

using DeferredSerialRequest = std::variant<UartEchoRequest, M1Request>;
enum class DeferResult { kQueued, kDuplicate, kConflict, kFull };

class DeferredSerialRequestQueue {
 public:
  DeferResult Enqueue(DeferredSerialRequest request);
  /** @brief 槽内动作移交前始终拦截来信，避免重投绕开去重。 */
  bool ShouldIntercept(bool revalidation_pending) const;
  const DeferredSerialRequest* Front() const;
  std::optional<DeferredSerialRequest> Take();

 private:
  std::optional<DeferredSerialRequest> pending_;
};

/** @brief 同动作 ID 不同内容的冲突重投 ACK；原待发动作不被覆盖。 */
Publication BuildDeferredRejection(
    const DeferredSerialRequest& request, const std::string& topic_namespace,
    std::string_view error_code, std::chrono::system_clock::time_point now);

/** @brief 用原字段还原待发请求，移交既有事务解析、有效期和去重逻辑。 */
std::string SerializeDeferredRequest(const DeferredSerialRequest& request);

/** @brief 过期请求立即交原事务记终态；有效请求须身份就绪且串口空闲。 */
bool ShouldReplayDeferred(const DeferredSerialRequest& request,
                          std::chrono::system_clock::time_point now,
                          bool identity_ready, bool serial_idle,
                          bool mqtt_connected);

}  // namespace experiment
