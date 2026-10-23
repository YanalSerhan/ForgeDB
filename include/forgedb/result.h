// Result<T>: either a value of type T or a non-OK Status.
//
// The same idea as C++23 std::expected<T, Status>, written for C++20. A function
// that can fail and produces a value returns Result<T>; one that only succeeds or
// fails returns Status.
//
//   Result<std::uint64_t> ParseNumber(std::string_view text);
//
//   Result<std::uint64_t> r = ParseNumber("42");
//   if (!r.ok()) return r.status();
//   use(*r);
#pragma once

#include <concepts>
#include <type_traits>
#include <utility>
#include <variant>

#include "forgedb/check.h"
#include "forgedb/status.h"

namespace forgedb {

template <typename T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<std::remove_cv_t<T>, Status>,
                "Result<Status> makes no sense; return Status.");
  static_assert(!std::is_reference_v<T>,
                "Result<T&> is not supported; return a pointer or a value.");

 public:
  // From a value. Implicit (when T is implicitly constructible from U) so a
  // function returning Result<T> can simply `return value;`.
  template <typename U = T>
    requires(std::constructible_from<T, U &&> && !std::same_as<std::remove_cvref_t<U>, Result> &&
             !std::same_as<std::remove_cvref_t<U>, Status>)
  explicit(!std::is_convertible_v<U&&, T>)
      Result(U&& value)  // NOLINT(bugprone-forwarding-reference-overload)
      : data_(std::in_place_index<kValueIndex>, std::forward<U>(value)) {}

  // From an error. Implicit, so a function can simply `return status;`.
  // Passing an OK Status is a bug: it is caught by FORGEDB_DCHECK in debug
  // builds and turned into an Internal error otherwise, never into a "value".
  Result(Status status)  // NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
      : data_(std::in_place_index<kStatusIndex>, std::move(status)) {
    FORGEDB_DCHECK(!std::get_if<kStatusIndex>(&data_)->ok());
    if (std::get_if<kStatusIndex>(&data_)->ok()) {
      data_.template emplace<kStatusIndex>(
          Status::Internal("Result constructed from an OK Status"));
    }
  }

  [[nodiscard]] bool ok() const noexcept { return data_.index() == kValueIndex; }

  // OK if this holds a value, otherwise the error.
  [[nodiscard]] Status status() const& {
    return ok() ? Status::OK() : *std::get_if<kStatusIndex>(&data_);
  }
  [[nodiscard]] Status status() && {
    return ok() ? Status::OK() : std::move(*std::get_if<kStatusIndex>(&data_));
  }

  // The value. Calling these on an error is a bug, so it is checked in every
  // build type (FORGEDB_CHECK): continuing would read garbage.
  [[nodiscard]] T& value() & {
    FORGEDB_CHECK(ok());
    return *std::get_if<kValueIndex>(&data_);
  }
  [[nodiscard]] const T& value() const& {
    FORGEDB_CHECK(ok());
    return *std::get_if<kValueIndex>(&data_);
  }
  // Returns by value (moved out) rather than T&&, so `auto&& v = F().value();`
  // can't dangle once the temporary Result is destroyed.
  [[nodiscard]] T value() && {
    FORGEDB_CHECK(ok());
    return std::move(*std::get_if<kValueIndex>(&data_));
  }

  [[nodiscard]] T& operator*() & { return value(); }
  [[nodiscard]] const T& operator*() const& { return value(); }
  [[nodiscard]] T operator*() && { return std::move(*this).value(); }
  [[nodiscard]] T* operator->() { return &value(); }
  [[nodiscard]] const T* operator->() const { return &value(); }

 private:
  static constexpr std::size_t kStatusIndex = 0;
  static constexpr std::size_t kValueIndex = 1;

  std::variant<Status, T> data_;
};

}  // namespace forgedb

// FORGEDB_ASSIGN_OR_RETURN(lhs, expr): evaluates `expr` (a Result<T>); on error
// returns its Status from the current function, otherwise assigns the value:
//
//   FORGEDB_ASSIGN_OR_RETURN(auto file, fs.NewWritableFile(path));
//
// `lhs` is a declaration such as `auto file`, so it cannot be parenthesized.
// NOLINTBEGIN(cppcoreguidelines-macro-usage,bugprone-macro-parentheses)
#define FORGEDB_DETAIL_CONCAT_INNER(a, b) a##b
#define FORGEDB_DETAIL_CONCAT(a, b) FORGEDB_DETAIL_CONCAT_INNER(a, b)
#define FORGEDB_ASSIGN_OR_RETURN(lhs, expr) \
  FORGEDB_DETAIL_ASSIGN_OR_RETURN(FORGEDB_DETAIL_CONCAT(forgedb_result_, __LINE__), lhs, expr)
#define FORGEDB_DETAIL_ASSIGN_OR_RETURN(result, lhs, expr) \
  auto result = (expr);                                    \
  if (!result.ok()) {                                      \
    return std::move(result).status();                     \
  }                                                        \
  lhs = std::move(result).value()
// NOLINTEND(cppcoreguidelines-macro-usage,bugprone-macro-parentheses)
