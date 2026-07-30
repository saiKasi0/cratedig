#include <bit>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("toolchain sanity", "[unit]") {
  STATIC_CHECK(__cplusplus >= 202002L);
  STATIC_CHECK(sizeof(void*) == 8);
  STATIC_CHECK(sizeof(float) == 4);
  // IEEE-754 single precision underpins the determinism contract.
  STATIC_CHECK(std::bit_cast<std::uint32_t>(1.0f) == 0x3f800000u);
}
