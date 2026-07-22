#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "cellular/cellular_snapshot.hpp"
#include "cellular/cellular_status.hpp"

TEST_CASE("RPICELL heartbeat packs online and diagnostic bits") {
  cellular::LinkStatus status{};
  status.interface_present = true;
  status.carrier_up = true;
  status.has_ip_address = true;
  status.has_default_route = true;
  status.online = true;

  CHECK(cellular::PackRpiCellularValue(status) == 0x1F);
}

TEST_CASE("RPICELL heartbeat can report offline while preserving diagnostics") {
  cellular::LinkStatus status{};
  status.interface_present = true;
  status.carrier_up = true;
  status.has_ip_address = false;
  status.has_default_route = false;
  status.online = false;

  CHECK(cellular::PackRpiCellularValue(status) == 0x06);
}

TEST_CASE("RPICELL heartbeat uses NAMED_VALUE_INT name") {
  cellular::LinkStatus status{};
  status.interface_present = true;
  status.carrier_up = true;
  status.has_ip_address = true;
  status.has_default_route = true;
  status.online = true;

  auto msg = cellular::BuildRpiCellularHeartbeat(status, 1, 191, 1234);
  mavlink_named_value_int_t decoded{};
  mavlink_msg_named_value_int_decode(&msg, &decoded);

  CHECK(static_cast<std::uint32_t>(msg.msgid) == MAVLINK_MSG_ID_NAMED_VALUE_INT);
  CHECK(std::string(decoded.name, decoded.name + 7) == "RPICELL");
  CHECK(decoded.time_boot_ms == 1234);
  CHECK(decoded.value == 0x1F);
}

TEST_CASE("RPICELL在线位由统一快照状态映射") {
  cellular::StatusSnapshot snapshot;
  snapshot.diagnostics = {
      .interface_present = true,
      .carrier_up = true,
      .has_ip_address = true,
      .has_default_route = true,
  };

  for (const auto state : {cellular::LinkState::kOnline, cellular::LinkState::kDegraded}) {
    snapshot.link_state = state;
    CHECK(cellular::PackRpiCellularValue(cellular::FromSnapshot(snapshot)) == 0x1F);
  }

  for (const auto state : {cellular::LinkState::kOffline, cellular::LinkState::kRecovering,
                           cellular::LinkState::kUnknown}) {
    snapshot.link_state = state;
    CHECK(cellular::PackRpiCellularValue(cellular::FromSnapshot(snapshot)) == 0x1E);
  }
}
