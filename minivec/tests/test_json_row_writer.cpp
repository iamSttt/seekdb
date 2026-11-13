#include "json_row_writer.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>

int main() {
  using nlohmann::json;
  std::string output;
  mvdb::JsonRowWriter writer(output);
  for(int64_t value:{INT64_MIN,int64_t(-1),int64_t(0),int64_t(1),INT64_MAX}) {
    output.clear();writer.integer(value);assert(output==json(value).dump());
  }
  std::mt19937_64 rng(2026111303);
  for(unsigned i=0;i<10000;++i) {
    const uint64_t bits=rng();double value;std::memcpy(&value,&bits,sizeof(value));
    if(!std::isfinite(value))continue;
    output.clear();writer.number(value);assert(output==json(value).dump());
  }
  for(double value:{-0.0,0.0,std::numeric_limits<double>::denorm_min(),
                     std::numeric_limits<double>::min(),std::numeric_limits<double>::max()}) {
    output.clear();writer.number(value);assert(output==json(value).dump());
  }
  std::string controls;
  for(int i=0;i<128;++i)controls+=static_cast<char>(i);
  for(const std::string& value:{std::string(),controls,std::string("\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80")}) {
    output.clear();writer.text(value);assert(output==json(value).dump());
  }
  for(const std::string& value:{std::string("\xff"),std::string("\xc0\x80"),std::string("\xed\xa0\x80"),std::string("\xe4\xb8")}) {
    bool rejected=false;
    try {output.clear();writer.text(value);}catch(const json::exception&){rejected=true;}
    assert(rejected);
  }
  for(double value:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
    bool rejected=false;try{output.clear();writer.number(value);}catch(const std::runtime_error&){rejected=true;}
    assert(rejected);
  }
  std::vector<float> vector={-0.0f,0.0f,std::numeric_limits<float>::denorm_min(),
      std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),0.125f};
  output.clear();writer.value(mvdb::Value::ofVec(vector));assert(output==json(vector).dump());
  std::cout<<"PASS typed JSON matches pinned serializer, exact int64, float roundtrip, negative zero and strict UTF8\n";
}
