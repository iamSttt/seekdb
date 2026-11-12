// Direct executor complement to test_typed_response.py: prove out.empty(), not
// merely that the protocol wrapper omitted a failed result. Link engine sources
// except main.cpp, once with and once without MVDB_ENABLE_TYPED_JSON.
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
      Statement statement;assert(parse_sql(text,&statement,&error,&code,false));
      return execute_json(db,statement,&out,&error,&code,false);
    };
    assert(call("CREATE TABLE bad(id BIGINT PRIMARY KEY,s VARCHAR(80000));"));
    assert(call("INSERT INTO bad VALUES (1,'"+std::string(70000,'z')+"'),(2,'valid'),(3,'after');"));
    const std::string invalid[]={std::string("\xff",1),std::string("\xc0\x80",2),
      std::string("\xed\xa0\x80",3),std::string("\xe4\xb8",2),std::string("\xf4\x90\x80\x80",4),std::string("\x80",1)};
    for(const auto& bytes:invalid) {
      assert(call("UPDATE bad SET s='"+bytes+"' WHERE id=2;"));
      for(const char* suffix:{" ORDER BY id LIMIT 10;",";"," ORDER BY id LIMIT 4096;"}) {
        out="previous response must also be cleared";
        assert(!call(std::string("SELECT id,s FROM bad")+suffix));
        assert(out.empty() && code!=0 && error.find("UTF-8")!=std::string::npos);
        assert(call("SELECT id,s FROM bad WHERE id=3;"));
        const auto response=nlohmann::json::parse(out);
        assert(response.at("columns")==nlohmann::json::array({"id","s"}));
        assert(response.at("rows")==nlohmann::json::array({nlohmann::json::array({3,"after"})}));
        assert(error.empty() && code==0);
      }
    }
  }
  std::filesystem::remove_all(path);
  std::cout<<"PASS 18 mid-response UTF8 failures clear executor output and allow following success\n";
}
