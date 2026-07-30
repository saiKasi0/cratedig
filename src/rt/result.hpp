#ifndef CRATEDIG_RT_RESULT_HPP
#define CRATEDIG_RT_RESULT_HPP

#include <cassert>
#include <type_traits>
#include <utility>

namespace rt {

// std::expected-style value-or-error type for the exception-free targets
// (src/rt/ and src/engine/ compile with -fno-exceptions; see CLAUDE.md).
//
// M0 restricts T and E to trivially copyable, trivially destructible types.
// That covers everything the real-time lane needs — indices, frame counts, small
// enums — and keeps the whole type a plain union with a flag: no destructor to
// run, no exception safety to reason about, usable in constexpr. Widening this
// to non-trivial types means writing the destructor/move machinery, which is
// deliberately deferred until a caller actually needs it.

template <typename E>
struct Err {
  E value;
};

template <typename E>
Err(E) -> Err<E>;

template <typename T, typename E>
class [[nodiscard]] Result {
  static_assert(std::is_trivially_copyable_v<T>, "Result<T, E>: T must be trivially copyable");
  static_assert(std::is_trivially_destructible_v<T>,
                "Result<T, E>: T must be trivially destructible");
  static_assert(std::is_trivially_copyable_v<E>, "Result<T, E>: E must be trivially copyable");
  static_assert(std::is_trivially_destructible_v<E>,
                "Result<T, E>: E must be trivially destructible");

 public:
  using ValueType = T;
  using ErrorType = E;

  constexpr Result(T value) noexcept  // NOLINT(google-explicit-constructor)
      : m_value(value), m_has_value(true) {}

  constexpr Result(Err<E> error) noexcept  // NOLINT(google-explicit-constructor)
      : m_error(error.value), m_has_value(false) {}

  [[nodiscard]] constexpr bool ok() const noexcept { return m_has_value; }

  constexpr explicit operator bool() const noexcept { return m_has_value; }

  // Precondition: ok(). Checked in debug builds only — there is no exception to
  // throw and no sensible real-time recovery from asking for the wrong branch.
  [[nodiscard]] constexpr const T& value() const noexcept {
    assert(m_has_value && "Result::value() called on an error result");
    return m_value;
  }

  // Precondition: !ok().
  [[nodiscard]] constexpr const E& error() const noexcept {
    assert(!m_has_value && "Result::error() called on a value result");
    return m_error;
  }

  [[nodiscard]] constexpr T value_or(T fallback) const noexcept {
    return m_has_value ? m_value : fallback;
  }

 private:
  union {
    T m_value;
    E m_error;
  };

  bool m_has_value;
};

}  // namespace rt

#endif  // CRATEDIG_RT_RESULT_HPP
