CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -pthread -Iinclude -Itests
SRC      := src/seat_hold.cpp

all: build/test_rules build/test_model build/test_stress

build/%: tests/%.cpp $(SRC) include/seat_hold.hpp tests/reference.hpp tests/fake_clock.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)

test: all
	./build/test_rules
	./build/test_model
	./build/test_stress

tsan:
	@mkdir -p build
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread -Iinclude -Itests -o build/test_stress_tsan tests/test_stress.cpp $(SRC)
	./build/test_stress_tsan 1 100

mutants:
	./scripts/mutants.sh

clean:
	rm -rf build
.PHONY: all test tsan mutants clean
