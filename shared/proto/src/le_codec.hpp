#pragma once
/**
 * @file le_codec.hpp
 * @brief Private little-endian byte writer and reader shared by the proto wire encoders (engine and FC telemetry).
 *
 * Not part of the public API. Serialization is byte-wise and independent of host endianness. Standard C++20 only.
 */
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace astrosim::proto::detail {

/// @brief Appends little-endian primitives to a byte buffer, independent of host endianness.
class Writer {
 public:
  /// @param size Expected final size, used to reserve capacity.
  explicit Writer(std::size_t size) { bytes_.reserve(size); }

  /// @brief Appends one byte.
  /// @param v Value.
  void u8(std::uint8_t v) { bytes_.push_back(static_cast<std::byte>(v)); }
  /// @brief Appends a u16, little-endian.
  /// @param v Value.
  void u16(std::uint16_t v) { le(v, 2); }
  /// @brief Appends a u32, little-endian.
  /// @param v Value.
  void u32(std::uint32_t v) { le(v, 4); }
  /// @brief Appends an IEEE 754 binary32, little-endian.
  /// @param v Value.
  void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
  /// @brief Appends an IEEE 754 binary64, little-endian.
  /// @param v Value.
  void f64(double v) { le(std::bit_cast<std::uint64_t>(v), 8); }

  /// @brief Writes every element of @p a using the member function @p put.
  /// @param a Elements to write, in order.
  /// @param put Member function that writes one element.
  template <std::size_t N, typename F>
  void array(const std::array<F, N>& a, void (Writer::*put)(F)) {
    for (F v : a) (this->*put)(v);
  }

  /// @return The accumulated bytes (the writer is left empty).
  std::vector<std::byte> take() { return std::move(bytes_); }

 private:
  void le(std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  std::vector<std::byte> bytes_;
};

/// @brief Reads little-endian primitives from a byte span.
/// @pre The caller has verified the span holds every byte that will be read.
class Reader {
 public:
  /// @param data Bytes to read from; must outlive the reader.
  explicit Reader(std::span<const std::byte> data) : data_(data) {}

  /// @return The next byte.
  std::uint8_t u8() { return static_cast<std::uint8_t>(data_[pos_++]); }
  /// @return The next little-endian u16.
  std::uint16_t u16() { return static_cast<std::uint16_t>(le(2)); }
  /// @return The next little-endian u32.
  std::uint32_t u32() { return static_cast<std::uint32_t>(le(4)); }
  /// @return The next little-endian binary32.
  float f32() { return std::bit_cast<float>(u32()); }
  /// @return The next little-endian binary64.
  double f64() { return std::bit_cast<double>(le(8)); }

  /// @brief Fills @p a with consecutive binary32 values.
  /// @param a Destination array.
  template <std::size_t N>
  void floats(std::array<float, N>& a) {
    for (float& v : a) v = f32();
  }
  /// @brief Fills @p a with consecutive binary64 values.
  /// @param a Destination array.
  template <std::size_t N>
  void doubles(std::array<double, N>& a) {
    for (double& v : a) v = f64();
  }

 private:
  std::uint64_t le(int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i);
    return v;
  }
  std::span<const std::byte> data_;
  std::size_t pos_ = 0;
};

}  // namespace astrosim::proto::detail
