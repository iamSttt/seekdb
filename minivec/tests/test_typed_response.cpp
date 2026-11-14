// Direct executor complement to test_typed_response.py: prove out.empty(), not
// merely that the protocol wrapper omitted a failed result. Link engine sources
// except main.cpp. Adapted to 760f's four-argument parse_sql; no bulk flag exists.
#include "db.h"
#include "exec.h"
#include "sql.h"
#include "../third_party/json.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>

using namespace mvdb;
int main() {
  alarm(120);
  char path[]="/tmp/mvdb-typed-response-XXXXXX";assert(mkdtemp(path));
  {
    Database db;std::string error,out;int code=0;assert(db.open(path,&error));
    auto call=[&](const std::string& text) {
      Statement statement;assert(parse_sql(text,&statement,&error,&code));
      return execute_json(db,statement,&out,&error,&code,false);
    };
    // 30,000 codepoints fit VARCHAR while their 90,000 UTF-8 bytes ensure the
    // valid response prefix already exceeds 64 KiB before the later bad row.
    std::string prefix;prefix.reserve(90000);
    for(size_t i=0;i<30000;++i)prefix+="\xe4\xb8\xad";
    const std::string invalid[]={std::string("\xff",1),std::string("\xc0\x80",2),
      std::string("\xed\xa0\x80",3),std::string("\xe4\xb8",2),std::string("\xf4\x90\x80\x80",4),std::string("\x80",1)};
    // Preserve all original scalar cases, and exercise both sides of the
    // experimental projected-vector guard. Every path still sees a 90KB prefix.
    for(int dimension:{0,127,128}) {
      const std::string table=dimension ? "badvec"+std::to_string(dimension) : "bad";
      std::string vector="[";
      for(int i=0;i<dimension;++i) {if(i)vector+=',';vector+='0';}
      vector+=']';
      const std::string schema=dimension ? ",v VECTOR("+std::to_string(dimension)+")" : "";
      const std::string cell=dimension ? ","+vector : "";
      const std::string projection=dimension ? "id,s,v" : "id,s";
      assert(call("CREATE TABLE "+table+"(id BIGINT PRIMARY KEY,s VARCHAR(65535)"+schema+");"));
      assert(call("INSERT INTO "+table+" VALUES (1,'"+prefix+"'"+cell+"),(2,'valid'"+cell+"),(3,'after'"+cell+");"));
      auto columns=nlohmann::json::array({"id","s"});
      auto expected=nlohmann::json::array({3,"after"});
      if(dimension) {columns.push_back("v");expected.push_back(std::vector<double>(dimension,0.0));}
      for(const auto& bytes:invalid) {
        assert(call("UPDATE "+table+" SET s='"+bytes+"' WHERE id=2;"));
        for(const char* suffix:{" ORDER BY id LIMIT 10;",";"," ORDER BY id LIMIT 4096;"}) {
          out="previous response must also be cleared";
          assert(!call("SELECT "+projection+" FROM "+table+suffix));
          assert(out.empty() && code!=0 && error.find("UTF-8")!=std::string::npos);
          assert(call("SELECT "+projection+" FROM "+table+" WHERE id=3;"));
          const auto response=nlohmann::json::parse(out);
          assert(response.at("columns")==columns);
          assert(response.at("rows")==nlohmann::json::array({expected}));
          assert(error.empty() && code==0);
        }
      }
    }
  }
  std::filesystem::remove_all(path);
  std::cout<<"PASS 54 scalar/127d/128d mid-response UTF8 failures clear executor output and allow following success\n";
}
