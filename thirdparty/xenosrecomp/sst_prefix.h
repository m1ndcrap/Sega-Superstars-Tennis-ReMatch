// sst_rematch - prefix header for the vendored XenosRecomp translator

#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <fmt/core.h>

template <typename T>
static T byteSwap(T value)
{
  if constexpr (sizeof(T) == 1)
    return value;
  else if constexpr (sizeof(T) == 2)
    return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(value)));
  else if constexpr (sizeof(T) == 4)
    return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(value)));
  else if constexpr (sizeof(T) == 8)
    return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(value)));

  assert(false && "Unexpected byte size.");
  return value;
}

template <typename T>
struct be
{
  T value;

  T get() const
  {
    if constexpr (std::is_enum_v<T>)
      return T(byteSwap(std::underlying_type_t<T>(value)));
    else
      return byteSwap(value);
  }

  operator T() const { return get(); }
};
