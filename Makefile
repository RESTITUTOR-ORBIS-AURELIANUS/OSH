CXX ?= c++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -Wpedantic

.PHONY: all test verify benchmark submission clean
all: pager

pager: pager.cpp include/policy.hpp include/baselines.hpp include/decay.hpp include/predictive.hpp include/adaptive.hpp include/replay.hpp
	$(CXX) $(CXXFLAGS) pager.cpp -o pager

test: pager
	$(CXX) $(CXXFLAGS) tests/test_baselines.cpp -o tests/test_baselines
	./tests/test_baselines
	$(CXX) $(CXXFLAGS) tests/test_online.cpp -o tests/test_online
	./tests/test_online
	python3 tests/test_cli.py --binary ./pager
	python3 scripts/verify_release.py --binary ./pager

verify: pager
	python3 scripts/verify_release.py --binary ./pager

benchmark: pager
	python3 tests/generate_traces.py
	python3 scripts/benchmark.py --include-holdout --plots

submission:
	python3 scripts/amalgamate.py

clean:
	rm -f pager pager.exe tests/test_baselines tests/test_online
