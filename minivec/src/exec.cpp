// exec.cpp — statement execution + MySQL-style table rendering
#include "exec.h"
#include "result_sort.h"
#include "distance.h"
#include "quantized_bound.h"
#include "query_pool.h"
#include "../third_party/json.hpp"
#include <limits>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace mvdb {

std::string format_double(double v) {
  if (std::isnan(v)) return "NULL";
  if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), v);
  std::string s(buf, res.ptr);
  // MySQL prints 1 as "1", 1.5 as "1.5" — to_chars shortest already does that.
  return s;
}

namespace {

using Json = nlohmann::json;

// Above this Top-K the per-half heaps would cost more than the scan they save.
constexpr size_t kFanoutTopKMax = 1024;

enum class OutputMode { Tsv, TsvStatus, Table };

OutputMode output_mode() {
  static const OutputMode mode = [] {
    const char* value = std::getenv("MVDB_OUT");
    if (value && std::strcmp(value, "tsv-status") == 0) return OutputMode::TsvStatus;
    if (value && std::strcmp(value, "table") == 0) return OutputMode::Table;
    return OutputMode::Tsv;
  }();
  return mode;
}

bool tsv_mode() { return output_mode() != OutputMode::Table; }

std::string render_text(const std::string& text) {
  if (output_mode() != OutputMode::TsvStatus) return text;
  std::string escaped;
  escaped.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '\\': escaped += "\\\\"; break;
      case '\t': escaped += "\\t"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\0': escaped += "\\0"; break;
      default: escaped += c; break;
    }
  }
  return escaped;
}

void emit_affected_rows(int64_t n, std::string* out, bool json_response) {
  if (json_response) {
    *out += "{\"affected_rows\":" + std::to_string(n) + "}";
    return;
  }
  if (output_mode() == OutputMode::Tsv) return;
  *out += "Query OK, " + std::to_string(n) + " row" + (n == 1 ? "" : "s") + " affected\n";
}

Json json_value(const Value& value) {
  switch (value.type) {
    case Type::BIGINT: return value.i;
    case Type::VARCHAR: return value.s;
    case Type::VECTOR:
      for (const float component : value.v) {
        if (!std::isfinite(component)) throw std::runtime_error("non-finite vector result");
      }
      return value.v;
  }
  throw std::runtime_error("invalid result type");
}

std::string render_value(const Value& v) {
  switch (v.type) {
    case Type::BIGINT:
      return std::to_string(v.i);
    case Type::VARCHAR:
      return render_text(v.s);
    case Type::VECTOR: {
      std::string s = "[";
      for (size_t i = 0; i < v.v.size(); ++i) {
        if (i) s += ",";
        s += format_double(v.v[i]);
      }
      s += "]";
      return s;
    }
  }
  return "";
}

bool is_numeric_type(Type t) { return t == Type::BIGINT; }

void render_table(const std::vector<std::string>& headers,
                  const std::vector<std::vector<std::string>>& rows,
                  const std::vector<bool>& right_align, std::string* out) {
  size_t ncol = headers.size();
  std::vector<size_t> w(ncol, 0);
  for (size_t c = 0; c < ncol; ++c) w[c] = headers[c].size();
  for (const auto& r : rows)
    for (size_t c = 0; c < ncol && c < r.size(); ++c) w[c] = std::max(w[c], r[c].size());

  auto border = [&]() {
    std::string s = "+";
    for (size_t c = 0; c < ncol; ++c) {
      s += std::string(w[c] + 2, '-');
      s += "+";
    }
    s += "\n";
    return s;
  };

  *out += border();
  *out += "|";
  for (size_t c = 0; c < ncol; ++c) {
    *out += " ";
    *out += headers[c];
    *out += std::string(w[c] - headers[c].size(), ' ');
    *out += " |";
  }
  *out += "\n";
  *out += border();
  for (const auto& r : rows) {
    *out += "|";
    for (size_t c = 0; c < ncol; ++c) {
      const std::string cell = c < r.size() ? r[c] : std::string();
      *out += " ";
      const bool num = right_align[c];
      const size_t pad = w[c] > cell.size() ? w[c] - cell.size() : 0;
      if (num) {
        *out += std::string(pad, ' ');
        *out += cell;
      } else {
        *out += cell;
        *out += std::string(pad, ' ');
      }
      *out += " |";
    }
    *out += "\n";
  }
  *out += border();
}

}  // namespace

static bool execute_impl(Database& db, const Statement& st, std::string* out, std::string* err,
                         int* code, const OutputSink& sink, bool json_response, bool ann_mode) {
  *code = 0;
  switch (st.kind) {
    case Statement::EMPTY:
      if (json_response) emit_affected_rows(0, out, true);
      return true;

    case Statement::CREATE_TABLE: {
      if (!db.create_table(st.table, st.columns, err, code)) {
        if (*code == 1050 && st.if_not_exists == "IF NOT EXISTS") { *code = 0; *err = ""; emit_affected_rows(0, out, json_response); return true; }
        return false;
      }
      emit_affected_rows(0, out, json_response);
      return true;
    }
    case Statement::DROP_TABLE: {
      if (!db.drop_table(st.table, err, code)) {
        if (*code == 1146 && st.if_not_exists == "IF EXISTS") { *code = 0; *err = ""; emit_affected_rows(0, out, json_response); return true; }
        return false;
      }
      emit_affected_rows(0, out, json_response);
      return true;
    }
    case Statement::SHOW_TABLES: {
      auto names = db.table_names();
      std::sort(names.begin(), names.end());
      if (json_response) {
        Json rows = Json::array();
        for (const auto& name : names) rows.push_back(Json::array({name}));
        *out = Json{{"columns", Json::array({"Tables"})}, {"rows", std::move(rows)}}.dump();
        return true;
      }
      std::vector<std::vector<std::string>> rows;
      for (auto& n : names) rows.push_back({n});
      if (tsv_mode()) {
        *out += "Tables\n";
        for (auto& r : rows) { *out += render_text(r[0]); *out += '\n'; }
      } else {
        render_table({"Tables"}, rows, {false}, out);
        *out += std::to_string(rows.size()) + " row" + (rows.size() == 1 ? "" : "s") + " in set\n";
      }
      return true;
    }
    case Statement::BEGIN_TXN: {
      if (!db.begin(err, code)) return false;
      emit_affected_rows(0, out, json_response);
      return true;
    }
    case Statement::COMMIT_TXN: {
      if (!db.commit(err, code)) return false;
      emit_affected_rows(0, out, json_response);
      return true;
    }
    case Statement::ROLLBACK_TXN: {
      if (!db.rollback(err, code)) return false;
      emit_affected_rows(0, out, json_response);
      return true;
    }
    case Statement::CREATE_INDEX: {
      if (!db.create_vector_index(st.table, st.index_column, st.index_name, err, code)) return false;
      emit_affected_rows(0, out, json_response);
      return true;
    }
    case Statement::INSERT: {
      Table* t = db.table(st.table);
      if (!t) {
        *err = "Table '" + st.table + "' doesn't exist";
        *code = 1146;
        return false;
      }
      const TableSchema& sch = t->schema;
      // Resolve an explicit column list into schema order.
      std::vector<int> pos;  // schema index for each written value, or -1
      if (!st.columns.empty()) {
        for (auto& c : st.columns) {
          int idx = sch.find(c.name);
          if (idx < 0) {
            *err = "Unknown column '" + c.name + "' in 'field list'";
            *code = 1054;
            return false;
          }
          if (std::find(pos.begin(), pos.end(), idx) != pos.end()) {
            *err = "Duplicate column in INSERT"; *code = 1110; return false;
          }
          pos.push_back(idx);
        }
      } else {
        for (size_t i = 0; i < sch.cols.size(); ++i) pos.push_back((int)i);
        if (!st.rows.empty() && st.rows[0].size() != sch.cols.size()) {
          *err = "Column count doesn't match value count at row 1";
          *code = 1136;
          return false;
        }
      }
      int64_t n = 0;
      for (auto& row : st.rows) {
        if (row.size() != pos.size()) {
          *err = "Column count doesn't match value count at row " + std::to_string(n + 1);
          *code = 1136;
          return false;
        }
        if (pos.size() == sch.cols.size() && st.columns.empty()) {
          if (!db.insert(st.table, row, err, code)) {
            return false;
          }
        } else {
          std::vector<Value> full(sch.cols.size());
          std::vector<uint8_t> filled(sch.cols.size(), 0);
          for (size_t k = 0; k < pos.size(); ++k) {
            full[pos[k]] = row[k];
            filled[pos[k]] = 1;
          }
          for (size_t k = 0; k < full.size(); ++k) {
            if (!filled[k]) {
              *err = "Field '" + sch.cols[k].name + "' doesn't have a default value";
              *code = 1364;
                  return false;
            }
          }
          if (!db.insert(st.table, full, err, code)) {
            return false;
          }
        }
        ++n;
      }
      emit_affected_rows(n, out, json_response);
      return true;
    }
    case Statement::UPDATE: {
      int64_t n = db.update_rows(st.table, st.conds, st.sets, err, code);
      if (*code) return false;
      emit_affected_rows(n, out, json_response);
      return true;
    }
    case Statement::DELETE_ROWS: {
      int64_t n = db.delete_rows(st.table, st.conds, err, code);
      if (*code) return false;
      emit_affected_rows(n, out, json_response);
      return true;
    }
    case Statement::SELECT: {
      Table* t = db.table(st.table);
      if (!t) {
        *err = "Table '" + st.table + "' doesn't exist";
        *code = 1146;
        return false;
      }
      const TableSchema& sch = t->schema;

      // Resolve projection.
      struct Item {
        int col;                 // schema column index, -1 for distance
        bool is_distance;
        std::string header;
        const std::vector<float>* qvec;
      };
      std::vector<Item> items;
      bool star = st.items.empty();
      if (star) {
        for (size_t c = 0; c < sch.cols.size(); ++c)
          items.push_back({(int)c, false, sch.cols[c].name, nullptr});
      } else {
        for (auto& it : st.items) {
          if (it.is_distance) {
            int col = sch.find(it.column);
            if (col < 0) {
              *err = "Unknown column '" + it.column + "' in 'field list'";
              *code = 1054;
              return false;
            }
            if (sch.cols[col].type != Type::VECTOR) {
              *err = "L2_DISTANCE() requires a VECTOR column";
              *code = 1235;
              return false;
            }
            if ((int)it.query.size() != sch.cols[col].len) {
              *err = "Vector dimension mismatch";
              *code = 1264;
              return false;
            }
            static thread_local std::vector<float> dummy;
            items.push_back({col, true, it.alias.empty() ? "L2_DISTANCE(" + it.column + ")" : it.alias, nullptr});
            items.back().qvec = &it.query;
          } else {
            int col = sch.find(it.column);
            if (col < 0) {
              *err = "Unknown column '" + it.column + "' in 'field list'";
              *code = 1054;
              return false;
            }
            items.push_back({col, false, it.alias.empty() ? it.column : it.alias, nullptr});
          }
        }
      }

      if (!validate_conditions(*t, st.conds, err, code)) return false;

      // Resolve ordering separately from projection: SQL may order by a column
      // or distance expression that is not in the selected fields.
      Item order{-1, false, "", nullptr};
      const bool ordered = !st.order_by.empty();
      if (ordered) {
        bool found = false;
        if (st.order_is_expression) {
          order.col = sch.find(st.order_expression.column);
          order.is_distance = true;
          order.qvec = &st.order_expression.query;
          found = order.col >= 0;
        } else {
          for (const auto& item : items) {
            if (item.header == st.order_by) { order = item; found = true; break; }
          }
          if (!found) { order.col = sch.find(st.order_by); found = order.col >= 0; }
        }
        if (!found) { *err = "Unknown column '" + st.order_by + "' in 'order clause'"; *code = 1054; return false; }
        if (order.is_distance && (sch.cols[order.col].type != Type::VECTOR ||
            order.qvec->size() != static_cast<size_t>(sch.cols[order.col].len))) {
          *err = "Vector dimension or type mismatch"; *code = 1264; return false;
        }
        if (!order.is_distance && sch.cols[order.col].type == Type::VECTOR) {
          *err = "ORDER BY VECTOR requires a distance expression"; *code = 1235; return false;
        }
      }
      auto distance2 = [](const std::vector<float>& a, const std::vector<float>& b) {
        // Subtraction must also be double: finite float32 endpoints can differ
        // by more than FLT_MAX. Exact mode never relies on float overflow.
        return distance_squared(a.data(), b.data(), a.size());
      };
      struct Candidate {
        int64_t pk = 0;
        int64_t integer = 0;
        double distance = 0;
        std::string text;
      };
      auto better = [&](const auto& a, const auto& b) {
        int cmp = 0;
        if (order.is_distance) cmp = a.distance < b.distance ? -1 : (a.distance > b.distance ? 1 : 0);
        else if (ordered && sch.cols[order.col].type == Type::BIGINT)
          cmp = a.integer < b.integer ? -1 : (a.integer > b.integer ? 1 : 0);
        else if (ordered) cmp = utf8_compare(a.text, b.text);
        if (cmp == 0) return a.pk < b.pk; // always ASC, even for ORDER BY ... DESC
        return st.order_desc ? cmp > 0 : cmp < 0;
      };
      auto begin_json_rows = [&] {
        Json columns = Json::array();
        for (const auto& item : items) columns.push_back(item.header);
        *out += "{\"columns\":" + columns.dump() + ",\"rows\":[";
      };
      auto projected_distance = [&](const Row& row, const Item& item, const Candidate* candidate) {
        // Only an alias resolved to this exact AST expression shares the
        // ranking key. Other distance projections still evaluate themselves.
        const bool same_expression = candidate && order.is_distance &&
            item.col == order.col && item.qvec == order.qvec;
        const double squared = same_expression ? candidate->distance
            : distance2(row.vals[item.col].v, *item.qvec);
        return std::sqrt(squared);
      };
      // Reuse only the array storage within this SELECT. dump() still produces
      // an independent string before the next row clears the cells, preserving
      // the reference serializer's floating-point and UTF-8 error semantics.
      Json row_cells;
      if (json_response) {
        row_cells = Json::array();
        row_cells.get_ref<Json::array_t&>().reserve(items.size());
      }
      auto encode_json_row = [&](const Row& row, const Candidate* candidate = nullptr) {
        row_cells.clear();
        for (const auto& item : items) {
          const Value& value = row.vals[item.col];
          if (!item.is_distance) {
            row_cells.push_back(json_value(value));
          } else {
            const double distance = projected_distance(row, item, candidate);
            if (!std::isfinite(distance)) throw std::runtime_error("non-finite L2 distance result");
            row_cells.push_back(distance);
          }
        }
        return row_cells.dump();
      };
      bool first_json_row = true;
      auto append_json_row = [&](const std::string& row) {
        if (!first_json_row) *out += ',';
        *out += row;
        first_json_row = false;
      };
      // Streaming output and external sort for large result sets. The normal
      // TopK path below keeps keys only for the current K winners.
      const bool wide_keys = ordered && !order.is_distance &&
          sch.cols[order.col].type == Type::VARCHAR && st.limit > 0 &&
          static_cast<uint64_t>(st.limit) > (8u<<20)/(static_cast<uint64_t>(sch.cols[order.col].len)*4+1);
      if ((json_response || (sink && tsv_mode())) &&
          (st.limit < 0 || st.limit >= 4096 || wide_keys)) {
        if (json_response) {
          begin_json_rows();
        } else {
          for (size_t j = 0; j < items.size(); ++j) {
            if (j) *out += '\t';
            *out += render_text(items[j].header);
          }
          *out += '\n';
        }
        auto emit = [&](const std::string& row) {
          if (json_response) {
            append_json_row(row);
          } else {
            *out += row;
            if (out->size() >= 65536) {
              sink(*out);
              out->clear();
            }
          }
        };
        auto render = [&](const Row& row) {
          if (json_response) return encode_json_row(row);
          std::string text;
          for(size_t j=0;j<items.size();++j) {
            if(j)text+='\t';
            const auto& item=items[j];
            text += item.is_distance ? format_double(std::sqrt(distance2(row.vals[item.col].v,*item.qvec)))
                                     : render_value(row.vals[item.col]);
          }
          text+='\n';return text;
        };
        const size_t limit=st.limit<0 ? std::numeric_limits<size_t>::max() : static_cast<size_t>(st.limit);
        if(ordered) {
          ResultSorter sorter(db.data_directory(),better);
          db.scan(st.table,st.conds,[&](const Row& row){
            SortRow c; c.pk=row.vals[sch.pk_idx].i;
            if(order.is_distance)c.distance=distance2(row.vals[order.col].v,*order.qvec);
            else if(sch.cols[order.col].type==Type::BIGINT)c.integer=row.vals[order.col].i;
            else c.text=row.vals[order.col].s;
            c.payload=render(row);sorter.add(std::move(c));return true;
          });
          sorter.emit(limit,emit);
        } else {
          size_t count=0;
          db.scan(st.table,st.conds,[&](const Row& row){emit(render(row));return ++count<limit;});
        }
        if (json_response) *out += "]}";
        return true;
      }
      std::vector<Candidate> result;
      const bool bounded = st.limit >= 0;
      const size_t k = bounded ? static_cast<size_t>(st.limit) : 0;
      auto offer = [&](Candidate c) {
        if (!bounded || result.size() < k) {
          result.push_back(std::move(c));
          if (bounded && ordered) std::push_heap(result.begin(), result.end(), better);
        } else if (ordered && better(c, result.front())) {
          std::pop_heap(result.begin(), result.end(), better);
          result.back() = std::move(c);
          std::push_heap(result.begin(), result.end(), better);
        }
        return ordered || !bounded || result.size() < k;
      };
      if (!bounded || k != 0) {
        if (ordered && order.is_distance) {
          // This statement does not change the floating-point control state.
          // Resolve the CPU kernel and conservative coefficients once per query.
          const DistanceBoundContext bound_context(order.qvec->size());
          auto consider=[&](int64_t pk,const float* v,size_t dim){
            // Reject only candidates proven strictly farther than the current
            // exact boundary. Ties and uncertain bounds still use exact keys.
            if (bounded && !st.order_desc && result.size()==k &&
                bound_context.definitely_greater(v,order.qvec->data(),result.front().distance))
              return true;
            Candidate c;c.pk=pk;c.distance=distance_squared(v,order.qvec->data(),dim);
            return offer(std::move(c));
          };
          FlatIndex::ResidentView view;
          FlatIndex* index=ann_mode ? db.resident_vector_index(st.table) : nullptr;
          if(st.conds.empty() && db.resident_vector_view(st.table,&view)) {
            // One immutable view for the statement removes two indirect
            // callbacks per row. Mutations invalidate the owning index.
            std::vector<uint32_t> probed;
            const uint32_t* ordinals=nullptr;
            size_t ordinal_count=0;
            // The approximate candidate set only narrows which rows are visited;
            // ranking inside it is the same exact float path, so the recall
            // contract is the probe width alone. A probe that cannot beat the
            // full scan reports failure and leaves the exact scan in place.
            const bool approximate=index &&
                index->ann_probe(order.qvec->data(),0,&probed,&ordinals,&ordinal_count);
            // Intra-query fan-out over two contiguous halves of the ordinal
            // span. A half owns its top-K heap, its own certification bound and
            // its own mutable threshold cache - neither bound object may be
            // shared between threads - so the halves touch no shared state and
            // the merge below is their only meeting point. Their union holds the
            // K best rows under (exact distance, primary key), a total order, so
            // the winner set cannot depend on how the span was cut.
            const bool fanout=!approximate && bounded && !st.order_desc && k>=1 &&
                k<=kFanoutTopKMax && query_fanout_wanted(view.count);
            if(fanout) {
              std::vector<Candidate> halves[2];
              halves[0].reserve(k);halves[1].reserve(k);
              auto keep=[&](std::vector<Candidate>& heap,Candidate c) {
                if(heap.size()<k) {
                  heap.push_back(std::move(c));
                  std::push_heap(heap.begin(),heap.end(),better);
                } else if(better(c,heap.front())) {
                  std::pop_heap(heap.begin(),heap.end(),better);
                  heap.back()=std::move(c);
                  std::push_heap(heap.begin(),heap.end(),better);
                }
              };
              const size_t dimension=view.dimension;
              const float* query=order.qvec->data();
              std::function<void(unsigned)> scan_half=[&](unsigned worker) {
                const size_t middle=view.count/2;
                const size_t first=worker?middle:0;
                const size_t last=worker?view.count:middle;
                if(first>=last)return;
                // One bound object per half: both cache their current threshold.
                const DistanceBoundContext bound(dimension);
                const QuantizedQueryBound quantized(query,dimension,
                    view.quantized ? view.quantization_step : 0);
                std::vector<Candidate>& heap=halves[worker];
                for(size_t i=first;i<last;++i) {
                  // Integer codes are only a certified rejection filter, taken
                  // against this half's own k-th distance. Every uncertain row
                  // follows the existing float bound and exact kernel.
                  if(quantized.enabled() && heap.size()==k &&
                     quantized.definitely_greater(view.quantized+i*dimension,heap.front().distance))
                    continue;
                  const float* row=view.rows[i];
                  if(heap.size()==k && bound.definitely_greater(row,query,heap.front().distance))
                    continue;
                  Candidate c;c.pk=view.keys[i];c.distance=distance_squared(row,query,dimension);
                  keep(heap,std::move(c));
                }
              };
              auto run_half=+[](void* ctx,unsigned worker) {
                (*static_cast<std::function<void(unsigned)>*>(ctx))(worker);
              };
              query_fanout_run(&scan_half,run_half,true);
              for(unsigned worker=0;worker<2;++worker)
                for(auto& candidate:halves[worker])offer(std::move(candidate));
            } else {
              const QuantizedQueryBound quantized(order.qvec->data(),view.dimension,
                  bounded && !st.order_desc && view.quantized ? view.quantization_step : 0);
              auto scan_ordinal=[&](size_t i) {
                // Integer codes are only a certified rejection filter. Every
                // uncertain row follows the existing float bound/exact path.
                if(quantized.enabled() && result.size()==k &&
                   quantized.definitely_greater(view.quantized+i*view.dimension,result.front().distance))
                  return true;
                return consider(view.keys[i],view.rows[i],view.dimension);
              };
              if(approximate) {
                for(size_t j=0;j<ordinal_count;++j)
                  if(!scan_ordinal(ordinals[j]))break;
              } else if(quantized.enabled()) {
                for(size_t i=0;i<view.count;++i)
                  if(!scan_ordinal(i))break;
              } else {
                for(size_t i=0;i<view.count;++i)
                  if(!consider(view.keys[i],view.rows[i],view.dimension))break;
              }
            }
          } else {
            // Filtered search: the same certified int8 rejection, with the
            // threshold kept current from the winning heap. Before the heap is
            // full the threshold is infinite, so nothing is rejected.
            FlatIndex::ResidentView filtered_view;
            const bool resident=db.resident_vector_view(st.table,&filtered_view) &&
                filtered_view.quantized && bounded && !st.order_desc;
            std::unique_ptr<QuantizedQueryBound> filtered_bound;
            if(resident) filtered_bound.reset(new QuantizedQueryBound(order.qvec->data(),
                filtered_view.dimension,filtered_view.quantization_step));
            const bool filtered_reject=resident && filtered_bound->enabled();
            double threshold=std::numeric_limits<double>::infinity();
            auto consider_filtered=[&](int64_t pk,const float* v,size_t dim) {
              if(bounded && !st.order_desc && result.size()==k &&
                 bound_context.definitely_greater(v,order.qvec->data(),result.front().distance))
                return true;
              Candidate c;c.pk=pk;c.distance=distance_squared(v,order.qvec->data(),dim);
              const bool more=offer(std::move(c));
              if(result.size()==k) threshold=result.front().distance;
              return more;
            };
            db.scan_vectors(st.table,st.conds,consider_filtered,
                filtered_reject ? filtered_bound.get() : nullptr,
                filtered_reject ? &threshold : nullptr);
          }
        } else db.scan(st.table, st.conds, [&](const Row& r) {
          Candidate c;c.pk=r.vals[sch.pk_idx].i;
          if(ordered) {
            if(sch.cols[order.col].type==Type::BIGINT)c.integer=r.vals[order.col].i;
            else c.text=r.vals[order.col].s;
          }
          return offer(std::move(c));
        });
      }
      if (ordered) std::sort(result.begin(), result.end(), better);
      // Candidates retain only keys, not entire rows. Retrieve the winning
      // projection by PK after selection, including on the spilled table path.
      std::vector<std::vector<std::string>> cells;
      const bool stream = !json_response && sink && tsv_mode();
      if (json_response) {
        begin_json_rows();
      } else if (stream) {
        for(size_t j=0;j<items.size();++j){if(j)*out+='\t';*out+=render_text(items[j].header);}
        *out+='\n';
      } else cells.reserve(result.size());
      for (const auto& c : result) {
        // Selection and projection share one statement and the same table.
        // Keep spill-aware row reads without compiling another PK predicate.
        const auto position = t->pk_pos.find(c.pk);
        if (position == t->pk_pos.end()) continue;
        const size_t slot = position->second;
        if (slot >= t->rows.size() || t->dead[slot]) continue;
        Row scratch;
        const Row& r = t->read_row(slot, scratch);
        if (json_response) {
          append_json_row(encode_json_row(r, &c));
          continue;
        }
        std::vector<std::string> row;
        if(!stream)row.reserve(items.size());
        for (size_t j=0;j<items.size();++j) {
          const auto& item=items[j];
          std::string cell=item.is_distance ? format_double(projected_distance(r, item, &c))
                                            : render_value(r.vals[item.col]);
          if(stream){if(j)*out+='\t';*out+=cell;}
          else row.push_back(std::move(cell));
        }
        if(stream){*out+='\n';if(out->size()>=65536){sink(*out);out->clear();}}
        else cells.push_back(std::move(row));
      }
      if (json_response) {
        *out += "]}";
        return true;
      }
      if(stream)return true;
      std::vector<bool> align(items.size(), false);
      for (size_t c = 0; c < items.size(); ++c)
        align[c] = items[c].is_distance || is_numeric_type(sch.cols[items[c].col].type);
      std::vector<std::string> headers;
      for (auto& it : items) headers.push_back(it.header);
      if (tsv_mode()) {
        for (size_t c = 0; c < headers.size(); ++c) {
          if (c) *out += '\t';
          *out += render_text(headers[c]);
        }
        *out += '\n';
        for (auto& row : cells) {
          for (size_t c = 0; c < row.size(); ++c) {
            if (c) *out += '\t';
            *out += row[c];
          }
          *out += '\n';
        }
      } else {
        render_table(headers, cells, align, out);
        *out += std::to_string(cells.size()) + " row" + (cells.size() == 1 ? "" : "s") + " in set\n";
      }
      return true;
    }
    default:
      *err = "Unsupported statement";
      *code = 1235;
      return false;
  }
}

static bool execute_statement(Database& db, const Statement& st, std::string* out,
                              std::string* err, int* code, const OutputSink& sink,
                              bool json_response, bool ann_mode) {
  // Calibrate the fan-out handshake off the query path: this runs on the very
  // first statement of the process, long before any timed query.
  query_pool_startup_probe();
  const bool mutation = st.kind == Statement::INSERT || st.kind == Statement::UPDATE ||
      st.kind == Statement::DELETE_ROWS || st.kind == Statement::CREATE_TABLE ||
      st.kind == Statement::DROP_TABLE || st.kind == Statement::CREATE_INDEX;
  if (!mutation) return execute_impl(db, st, out, err, code, sink, json_response, ann_mode);
  if (!db.begin_statement(err, code)) return false;
  bool ok;
  try {
    ok = execute_impl(db, st, out, err, code, sink, json_response, ann_mode);
  } catch (...) {
    if (json_response) db.end_statement(false, err, code);
    throw;
  }
  const bool ended = db.end_statement(ok, err, code);
  if (!ok || !ended) out->clear();
  return ok && ended;
}

bool execute(Database& db, const Statement& st, std::string* out, std::string* err,
             int* code, const OutputSink& sink) {
  return execute_statement(db, st, out, err, code, sink, false, false);
}

bool execute_json(Database& db, const Statement& st, std::string* out, std::string* err, int* code,
                  bool ann_mode) {
  out->clear();
  err->clear();
  *code = 0;
  try {
    if (execute_statement(db, st, out, err, code, {}, true, ann_mode)) return true;
  } catch (const std::exception& ex) {
    if (*code == 0) {
      *code = 1105;
      *err = ex.what();
    }
  }
  out->clear();
  return false;
}

}  // namespace mvdb
