CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Isrc -pthread
SRC := $(wildcard src/*.cpp)
BIN := minivec

all: $(BIN)

$(BIN): $(SRC) $(wildcard src/*.h) $(wildcard third_party/*.hpp)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

TEST_COMMON := src/db.cpp src/exec.cpp src/sql.cpp src/value.cpp src/flat_index.cpp
test_sql: tests/test_sql.cpp src/sql.cpp src/value.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^
test_db: tests/test_db.cpp src/db.cpp src/value.cpp src/flat_index.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^
test_recovery: tests/test_recovery_regressions.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) -o $@ $^
test_vector: tests/test_vector.cpp src/vector.cpp src/value.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^
test_query: tests/test_query_regressions.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) -o $@ $^
test_sort: tests/test_sort.cpp src/result_sort.h
	$(CXX) $(CXXFLAGS) -o $@ tests/test_sort.cpp
test_flat: tests/test_flat_index.cpp $(TEST_COMMON)
	$(CXX) $(CXXFLAGS) -o $@ $^
test_distance_neighbors: tests/test_distance_neighbors.cpp src/distance.h
	$(CXX) $(CXXFLAGS) -o $@ tests/test_distance_neighbors.cpp
test: all test_sql test_db test_recovery test_vector test_query test_sort test_flat test_distance_neighbors
	./test_sql
	./test_db
	./test_recovery
	./test_vector quick
	./test_query
	./test_sort
	./test_flat
	./test_distance_neighbors
	MVDB_PROTOCOL=sql python3 tests/regressions.py

clean:
	rm -f $(BIN)

.PHONY: all clean test
