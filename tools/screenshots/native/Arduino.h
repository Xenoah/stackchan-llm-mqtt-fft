#pragma once
// Minimal host adapters used only by the documentation renderer.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
using std::max;
using std::min;
using std::isnan;

class String {
  std::string value_;
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}
  String(std::string value) : value_(std::move(value)) {}
  template<class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
  String(T value) : value_(std::to_string(value)) {}
  operator const char*() const { return value_.c_str(); }
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  bool isEmpty() const { return value_.empty(); }
  void reserve(size_t size) { value_.reserve(size); }
  char operator[](size_t index) const { return value_[index]; }
  void remove(size_t index, size_t count = std::string::npos) { value_.erase(index, count); }
  String substring(size_t start) const { return value_.substr(start); }
  String& operator+=(const String& other) { value_ += other.value_; return *this; }
  String& operator+=(char other) { value_ += other; return *this; }
  friend String operator+(const String& a, const String& b) { return a.value_ + b.value_; }
  friend String operator+(const char* a, const String& b) { return String(a) + b; }
  friend String operator+(const String& a, const char* b) { return a + String(b); }
  friend bool operator==(const String& a, const String& b) { return a.value_ == b.value_; }
};
inline uint32_t millis() { return 10000; }
inline void delay(unsigned long) {}
