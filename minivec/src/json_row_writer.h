#pragma once
#include "value.h"
#include "../third_party/json.hpp"
#include <charconv>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace mvdb {
// Typed row serialization, using the pinned JSON library's exact floating-point
// formatting and strict UTF-8 escaping. No per-row/cell JSON array is built.
class JsonRowWriter {
 public:
  explicit JsonRowWriter(std::string& output):output_(output) {}
  void integer(int64_t value) {
    char buffer[32];
    const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);
    if(result.ec!=std::errc{})throw std::runtime_error("integer result encoding failed");
    output_.append(buffer,result.ptr);
  }
  void number(double value) {
    if(!std::isfinite(value))throw std::runtime_error("non-finite numeric result");
    char buffer[64];
    const char* end=nlohmann::detail::to_chars(buffer,buffer+sizeof(buffer),value);
    output_.append(buffer,static_cast<size_t>(end-buffer));
  }
  void text(const std::string& value) {
    if(!escaping_)escaping_.reset(new Serializer(nlohmann::detail::output_adapter<char>(output_),' '));
    output_+='"';escaping_->dump_escaped(value,false);output_+='"';
  }
  void value(const Value& item) {
    switch(item.type) {
      case Type::BIGINT: integer(item.i);return;
      case Type::VARCHAR: text(item.s);return;
      case Type::VECTOR:
        output_+='[';
        for(size_t i=0;i<item.v.size();++i) {if(i)output_+=',';number(static_cast<double>(item.v[i]));}
        output_+=']';return;
    }
    throw std::runtime_error("invalid result type");
  }
 private:
  using Serializer=nlohmann::detail::serializer<nlohmann::json>;
  std::string& output_;
  std::unique_ptr<Serializer> escaping_;
};
} // namespace mvdb
