CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Isrc -pthread
SRC := $(wildcard src/*.cpp)
BIN := minivec

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

clean:
	rm -f $(BIN)

.PHONY: all clean
