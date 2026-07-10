#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "registration/registration_state.hpp"

TEST_CASE("连接上升沿和内容变化触发注册") {
  registration::RegistrationState state;
  CHECK_FALSE(state.ShouldPublish(false, "v1"));
  CHECK(state.ShouldPublish(true, "v1"));
  state.MarkPublished("v1");
  CHECK_FALSE(state.ShouldPublish(true, "v1"));
  CHECK(state.ShouldPublish(true, "v2"));
  state.MarkPublished("v2");
  CHECK_FALSE(state.ShouldPublish(false, "v2"));
  CHECK(state.ShouldPublish(true, "v2"));
}

TEST_CASE("发布失败时后续循环继续重试") {
  registration::RegistrationState state;
  CHECK(state.ShouldPublish(true, "v1"));
  CHECK(state.ShouldPublish(true, "v1"));
}
