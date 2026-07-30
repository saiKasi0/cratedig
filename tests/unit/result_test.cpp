#include "rt/result.hpp"

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

namespace {

enum class DecodeError : std::uint8_t { kUnsupportedCodec, kTruncated };

using FrameResult = rt::Result<std::size_t, DecodeError>;

}  // namespace

TEST_CASE("Result carries a value", "[unit]") {
  const FrameResult result{std::size_t{512}};

  CHECK(result.ok());
  CHECK(static_cast<bool>(result));
  CHECK(result.value() == 512);
  CHECK(result.value_or(0) == 512);
}

TEST_CASE("Result carries an error", "[unit]") {
  const FrameResult result{rt::Err{DecodeError::kTruncated}};

  CHECK_FALSE(result.ok());
  CHECK_FALSE(static_cast<bool>(result));
  CHECK(result.error() == DecodeError::kTruncated);
  CHECK(result.value_or(64) == 64);
}

TEST_CASE("Result is usable at compile time", "[unit]") {
  // The exception-free lane relies on this: a Result must be constant-evaluable
  // so error handling can live in constexpr helpers.
  constexpr FrameResult kOk{std::size_t{1024}};
  constexpr FrameResult kFailed{rt::Err{DecodeError::kUnsupportedCodec}};

  STATIC_CHECK(kOk.ok());
  STATIC_CHECK(kOk.value() == 1024);
  STATIC_CHECK(!kFailed.ok());
  STATIC_CHECK(kFailed.error() == DecodeError::kUnsupportedCodec);
  STATIC_CHECK(kFailed.value_or(7) == 7);
}

TEST_CASE("Result stays small and trivial", "[unit]") {
  // A Result must be cheap enough to return across the real-time boundary by
  // value; if it ever grows a destructor it can no longer be one.
  STATIC_CHECK(std::is_trivially_copyable_v<FrameResult>);
  STATIC_CHECK(std::is_trivially_destructible_v<FrameResult>);
  STATIC_CHECK(sizeof(FrameResult) <= 2 * sizeof(std::size_t));
}
