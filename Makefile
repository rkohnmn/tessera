# ─────────────────────────────────────────────────────────────────────────────
#  Tessera — convenience build for environments without CMake.
#  The CMake build is canonical (see README); this Makefile exists so a fresh
#  Linux/macOS checkout can produce ./build/tessera with a single `make`.
# ─────────────────────────────────────────────────────────────────────────────
VERSION   := 1.0.0
CXX       ?= g++
# -O3 -DNDEBUG to match the canonical CMake Release build, so the numbers in
# docs/BENCHMARKS.md describe the binary this Makefile produces too. At -O2 the
# parallel indexer runs about 2x slower (the inner scan loop does not inline),
# which is exactly the operation the benchmarks lead with.
CXXFLAGS  ?= -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Wshadow
CPPFLAGS  += -Isrc -DTESSERA_VERSION=\"$(VERSION)\"
LDFLAGS   += -pthread

SRC       := $(shell find src -name '*.cpp' | sort)
OBJ       := $(patsubst src/%.cpp,build/obj/%.o,$(SRC))
DEP       := $(OBJ:.o=.d)
TEST_SRC  := $(shell find tests -name '*.cpp' | sort)
TEST_OBJ  := $(patsubst tests/%.cpp,build/test-obj/%.o,$(TEST_SRC))

.PHONY: all clean test check run

all: build/tessera

build/tessera: $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c $< -o $@

build/test-obj/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -Itests -Isrc -MMD -MP -c $< -o $@

build/tessera_tests: $(TEST_OBJ) $(filter-out build/obj/main.o,$(OBJ))
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test check: build/tessera_tests
	@mkdir -p build
	./build/tessera_tests

run: build/tessera
	./build/tessera

clean:
	rm -rf build

-include $(DEP)
