CXX ?= g++
FEATURE_FLAGS ?= -DMVDB_ENABLE_SPILL_FILTER -DMVDB_ENABLE_TYPED_JSON -DMVDB_TYPED_WIDE_VECTOR_ONLY
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Isrc -pthread
SRC := $(wildcard src/*.cpp)
BIN := minivec
TEST_BINS := test_sql test_db test_recovery test_query test_sort test_flat test_distance_neighbors test_distance_simd test_quantized_bound test_query_pool test_cross_table_spill test_index_budget test_resident_update test_payload_batch test_filter_paths test_spill_filter test_storage_index_budget test_json_row_writer test_typed_response
FEATURE_STAMP := test_feature_flags.stamp

all: $(BIN)

# A flags change, including explicit FEATURE_FLAGS=, must rebuild native/tests.
$(FEATURE_STAMP): FORCE_FEATURE_FLAGS
	@printf '%s\n' "$(FEATURE_FLAGS)" > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp
$(BIN) $(TEST_BINS): $(FEATURE_STAMP)
FORCE_FEATURE_FLAGS:

$(BIN): $(SRC) $(wildcard src/*.h) $(wildcard third_party/*.hpp)
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(SRC)

TEST_COMMON := src/db.cpp src/exec.cpp src/sql.cpp src/value.cpp src/flat_index.cpp
test_sql: tests/test_sql.cpp src/sql.cpp src/value.cpp
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(filter-out $(FEATURE_STAMP),$^)
test_db: tests/test_db.cpp src/db.cpp src/value.cpp src/flat_index.cpp
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(filter-out $(FEATURE_STAMP),$^)
test_recovery: tests/test_recovery_regressions.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(filter-out $(FEATURE_STAMP),$^)
test_query: tests/test_query_regressions.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(filter-out $(FEATURE_STAMP),$^)
test_sort: tests/test_sort.cpp src/result_sort.h
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ tests/test_sort.cpp
test_flat: tests/test_flat_index.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(filter-out $(FEATURE_STAMP),$^)
test_distance_neighbors: tests/test_distance_neighbors.cpp src/distance.h
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ tests/test_distance_neighbors.cpp
test_distance_simd: tests/test_distance_simd.cpp src/distance.h
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ tests/test_distance_simd.cpp
test_quantized_bound: tests/test_quantized_bound.cpp src/quantized_bound.h src/distance.h
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ tests/test_quantized_bound.cpp
test_query_pool: tests/test_query_pool_safety.cpp src/query_pool.h
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ tests/test_query_pool_safety.cpp -Wl,--wrap=pthread_create
test_cross_table_spill: tests/test_cross_table_spill.cpp src/db.cpp src/flat_index.cpp src/value.cpp
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(filter-out $(FEATURE_STAMP),$^)
test_index_budget: tests/test_index_budget.cpp src/flat_index.cpp
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -DMVDB_TEST_WRAP_PWRITE -o $@ $(filter-out $(FEATURE_STAMP),$^) -Wl,--wrap=pwrite
test_resident_update: tests/test_resident_update.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -DMVDB_TEST_WRAP_PWRITE -o $@ $(filter-out $(FEATURE_STAMP),$^) -Wl,--wrap=pwrite

test_payload_batch: tests/test_payload_batch.cpp src/db.cpp src/flat_index.cpp src/value.cpp
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -DMVDB_TEST_WRAP_PWRITE -o $@ $(filter-out $(FEATURE_STAMP),$^) -Wl,--wrap=pwrite

test_filter_paths: tests/test_filter_paths.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(filter-out $(FEATURE_STAMP),$^)
test_spill_filter: tests/test_spill_filter.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -DMVDB_SPILL_BYTES=1 -DMVDB_TEST_WRAP_PWRITE -o $@ $(filter-out $(FEATURE_STAMP),$^) -Wl,--wrap=pread -Wl,--wrap=pwrite
test_storage_index_budget: tests/test_storage_index_budget.cpp src/db.cpp src/flat_index.cpp src/value.cpp
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(filter-out $(FEATURE_STAMP),$^)
test_json_row_writer: tests/test_json_row_writer.cpp src/json_row_writer.h src/value.h third_party/json.hpp
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ tests/test_json_row_writer.cpp
test_typed_response: tests/test_typed_response.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) $(FEATURE_FLAGS) -o $@ $(filter-out $(FEATURE_STAMP),$^)

test: all $(TEST_BINS)
	./test_sql
	./test_db
	./test_recovery
	./test_query
	./test_sort
	./test_flat
	./test_distance_neighbors
	./test_distance_simd
	./test_quantized_bound
	./test_query_pool
	./test_query_pool --create-fails
	./test_cross_table_spill
	./test_index_budget
	./test_index_budget --ann-budget
	./test_resident_update
	./test_payload_batch
	./test_filter_paths
	./test_spill_filter
	./test_storage_index_budget
	./test_json_row_writer
	./test_typed_response
	MVDB_PROTOCOL=sql python3 tests/regressions.py
	python3 tests/json_protocol.py --binary ./minivec
	python3 tests/test_typed_response.py --binary ./minivec
	python3 tests/topk_bounds.py --binary ./minivec
	MVDB_QUERY_THREADS=2 python3 tests/topk_bounds.py --binary ./minivec

clean:
	rm -f $(BIN) $(TEST_BINS) $(FEATURE_STAMP) $(FEATURE_STAMP).tmp

.PHONY: all clean test FORCE_FEATURE_FLAGS
