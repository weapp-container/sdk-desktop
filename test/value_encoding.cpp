#include "internal.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using weapp::desktop::Value;
using weapp::desktop::detail::decode;
using weapp::desktop::detail::encode;
using weapp::desktop::detail::Json;

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void safe_integers() {
  for (double value : {0.0, 1.0, -1.0, 4294967295.0, -4294967295.0,
                       9007199254740991.0, -9007199254740991.0}) {
    const auto encoded = encode(Value(value));
    check(encoded.is_number_integer() && !encoded.is_number_float(),
          "safe integral Values must use the JSON integer variant");
    check(encoded.get<std::int64_t>() == static_cast<std::int64_t>(value),
          "safe integer value changed");
    const auto wire = Json::parse(encoded.dump());
    check(wire.is_number_integer() && !wire.is_number_float(),
          "safe integers must remain integer tokens on the wire");
  }
  check(encode(Value(1)).dump() == "1", "Value(int) emitted a floating token");
}

void fractions() {
  for (double value : {0.5, -1.25, 1e-9,
                       std::numeric_limits<double>::denorm_min()}) {
    const auto encoded = encode(Value(value));
    check(encoded.is_number_float(), "fraction lost its floating variant");
    check(encoded.get<double>() == value, "fraction changed");
  }
}

void unsafe_integrals() {
  for (double value : {9007199254740992.0, -9007199254740992.0, 1e100,
                       std::numeric_limits<double>::max()}) {
    const auto encoded = encode(Value(value));
    check(encoded.is_number_float(),
          "unsafe integral double must not be cast to an integer");
    check(encoded.get<double>() == value, "unsafe double changed");
  }
}

void negative_zero() {
  const auto encoded = encode(Value(-0.0));
  check(encoded.is_number_float() && std::signbit(encoded.get<double>()),
        "negative zero lost its sign or floating variant");
  check(encoded.dump() == "-0.0", "negative zero changed on the wire");
  const auto wire = Json::parse(encoded.dump());
  check(wire.is_number_float() && std::signbit(wire.get<double>()),
        "negative zero failed the JSON round trip");
  check(encode(Value(0.0)).is_number_integer(),
        "positive zero must be a JSON integer");
}

void nonfinite_numbers() {
  for (double value : {std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    bool rejected = false;
    try {
      encode(Value(value));
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()) == "business numbers must be finite";
    }
    check(rejected, "nonfinite Values must preserve the existing rejection");
  }
}

void strings() {
  const Value input(Value::Object{
      {"epoch", "18446744073709551615"},
      {"deadlineNs", "1793623018305000000"},
      {"numericLooking", "1.0"},
  });
  const auto encoded = encode(input);
  check(encoded["epoch"].is_string() &&
            encoded["epoch"] == "18446744073709551615",
        "epoch string changed");
  check(encoded["deadlineNs"].is_string() &&
            encoded["deadlineNs"] == "1793623018305000000",
        "deadline string changed");
  check(encoded["numericLooking"] == "1.0", "string was coerced to a number");
}

void nesting_and_frame_fields() {
  const Value input(Value::Object{
      {"schemaVersion", 1.0},
      {"hostComponentFrame",
       Value::Object{{"width", 1.0}, {"height", 2.0}, {"rowBytes", 4.0}}},
      {"values", Value::Array{1.0, -1.25, -0.0, true, nullptr, "9007199254740993"}},
  });
  const auto encoded = Json::parse(encode(input).dump());
  check(encoded["schemaVersion"].is_number_integer() &&
            encoded["schemaVersion"] == 1,
        "schemaVersion is not an integer token");
  for (const auto* name : {"width", "height", "rowBytes"})
    check(encoded["hostComponentFrame"][name].is_number_integer(),
          "frame dimensions must be integer tokens accepted by Rust u32");
  check(encoded["values"][0].is_number_integer() &&
            encoded["values"][1].is_number_float() &&
            std::signbit(encoded["values"][2].get<double>()) &&
            encoded["values"][3] == true && encoded["values"][4].is_null() &&
            encoded["values"][5] == "9007199254740993",
        "nested Values changed semantics");
  check(encode(decode(encoded)) == encoded,
        "decode then encode changed the nested JSON values");
  bool rejected = false;
  try {
    encode(Value::Array{Value::Object{
        {"invalid", std::numeric_limits<double>::infinity()}}});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "nested nonfinite Values must be rejected");
}
}  // namespace

int main() {
  try {
    safe_integers();
    fractions();
    unsafe_integrals();
    negative_zero();
    nonfinite_numbers();
    strings();
    nesting_and_frame_fields();
    std::cout << "Value JSON encoding: 7 groups passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Value JSON encoding failed: " << error.what() << '\n';
    return 1;
  }
}
