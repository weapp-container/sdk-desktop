#include "internal.hpp"
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace weapp::desktop::detail {
Json encode(const Value& v) {
  return std::visit(
      [](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return nullptr;
        else if constexpr (std::is_same_v<T, Value::Array>) {
          Json a = Json::array();
          for (const auto& x : item) a.push_back(encode(x));
          return a;
        } else if constexpr (std::is_same_v<T, Value::Object>) {
          Json a = Json::object();
          for (const auto& [k, x] : item) a[k] = encode(x);
          return a;
        } else if constexpr (std::is_same_v<T, double>) {
          if (!std::isfinite(item))
            throw std::invalid_argument("business numbers must be finite");
          return item;
        } else
          return item;
      },
      v.data);
}
Value decode(const Json& j) {
  if (j.is_null()) return {};
  if (j.is_boolean()) return j.get<bool>();
  if (j.is_number()) return j.get<double>();
  if (j.is_string()) return j.get<std::string>();
  if (j.is_array()) {
    Value::Array a;
    for (const auto& x : j) a.push_back(decode(x));
    return a;
  }
  Value::Object a;
  for (auto i = j.begin(); i != j.end(); ++i) a[i.key()] = decode(i.value());
  return a;
}
} // namespace weapp::desktop::detail
