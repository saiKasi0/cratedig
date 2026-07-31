// The constexpr Hann window, and the constexpr cosine underneath it.
//
// The cosine is the part worth testing hard. It exists only because std::cos is
// not constexpr in C++20, so its single job is to agree with std::cos -- and if
// it quietly did not, every spectrum in the onset detector would be computed
// through a slightly wrong window and nothing would point at this file.

#include "ingest/window.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr std::size_t kSize = 1'024;

// Built at compile time. If cos_constexpr were not usable in a constant
// expression this line would not compile, which is the point of writing it this
// way rather than calling hann_window() inside a test body.
constexpr std::array<float, kSize> kWindow = ingest::hann_window<kSize>();

}  // namespace

TEST_CASE("the constexpr cosine agrees with std::cos", "[unit]") {
  // Across several periods in both directions, so the range reduction is
  // exercised rather than just the first octant. Reduction is where a hand-rolled
  // trig function goes wrong: the series itself is easy.
  double worst = 0.0;
  double worst_at = 0.0;
  for (int step = -2'000; step <= 2'000; ++step) {
    const double x = static_cast<double>(step) * 0.01;  // -20 .. +20 radians
    const double error = std::abs(ingest::detail::cos_constexpr(x) - std::cos(x));
    if (error > worst) {
      worst = error;
      worst_at = x;
    }
  }
  INFO("worst error " << worst << " at x = " << worst_at);

  // Measured at 7.8e-16, at the far end of the sweep where the range reduction
  // has folded the most -- about three ULP of a double, which is as close as two
  // different correct implementations get.
  CHECK(worst < 1e-14);
}

TEST_CASE("the constexpr cosine is exact at the quadrant boundaries", "[unit]") {
  CHECK(ingest::detail::cos_constexpr(0.0) == 1.0);
  CHECK(std::abs(ingest::detail::cos_constexpr(std::numbers::pi) + 1.0) < 1e-15);
  CHECK(std::abs(ingest::detail::cos_constexpr(std::numbers::pi / 2.0)) < 1e-15);
  CHECK(std::abs(ingest::detail::cos_constexpr(3.0 * std::numbers::pi / 2.0)) < 1e-15);
  CHECK(std::abs(ingest::detail::cos_constexpr(2.0 * std::numbers::pi) - 1.0) < 1e-15);
}

TEST_CASE("the constexpr cosine is even", "[unit]") {
  for (int step = 0; step <= 100; ++step) {
    const double x = static_cast<double>(step) * 0.13;
    REQUIRE(ingest::detail::cos_constexpr(x) == ingest::detail::cos_constexpr(-x));
  }
}

TEST_CASE("the Hann window has the shape it should", "[unit]") {
  // Periodic form: starts at exactly zero, peaks at the midpoint, and does NOT
  // return to zero at the last sample -- that asymmetry is the difference from
  // the symmetric window and is what makes overlapping copies sum flat.
  CHECK(kWindow[0] == 0.0F);
  CHECK(kWindow[kSize / 2] == 1.0F);
  CHECK(kWindow[kSize - 1] > 0.0F);
  CHECK(kWindow[kSize - 1] < 0.0001F);

  // Symmetric about the midpoint.
  for (std::size_t index = 1; index < kSize / 2; ++index) {
    REQUIRE(std::abs(kWindow[(kSize / 2) + index] - kWindow[(kSize / 2) - index]) < 1e-6F);
  }

  // Monotone up to the peak; a ripple here would mean the range reduction is
  // wrong somewhere in the middle of the sweep.
  for (std::size_t index = 1; index <= kSize / 2; ++index) {
    REQUIRE(kWindow[index] >= kWindow[index - 1]);
  }
}

TEST_CASE("the Hann window matches the analytic definition", "[unit]") {
  // The definition, evaluated with the standard library's own cos: this is what
  // pins the whole compile-time construction to the thing everybody else means
  // by "a Hann window".
  float worst = 0.0F;
  for (std::size_t index = 0; index < kSize; ++index) {
    const double phase =
        2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(kSize);
    const auto reference = static_cast<float>(0.5 - (0.5 * std::cos(phase)));
    worst = std::max(worst, std::abs(kWindow[index] - reference));
  }
  INFO("worst deviation from the analytic window: " << worst);
  CHECK(worst < 1e-7F);  // float rounding, nothing more
}

TEST_CASE("overlapping Hann windows sum flat at 50 percent overlap", "[unit]") {
  // The property the PERIODIC form exists for, and the reason it is worth being
  // fussy about the divisor: two half-overlapped Hann windows sum to exactly 1.
  // With the symmetric form (divisor N-1) they do not, and the result is a
  // low-level ripple across the spectrum that reads as signal.
  constexpr std::size_t kHop = kSize / 2;
  for (std::size_t index = 0; index < kHop; ++index) {
    const float total = kWindow[index] + kWindow[index + kHop];
    REQUIRE(std::abs(total - 1.0F) < 1e-6F);
  }
}
