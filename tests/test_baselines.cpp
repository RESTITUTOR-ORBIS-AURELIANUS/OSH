#include "../include/baselines.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <type_traits>

using Set = std::set<Page>;

// Independent finite-state dynamic program: enumerate every legal victim.
std::uint64_t brute_opt(const std::vector<Page>& trace, std::size_t capacity) {
    std::map<Set, std::uint64_t> states{{{}, 0}};
    for (Page page : trace) {
        std::map<Set, std::uint64_t> next;
        auto keep_best = [&](Set state, std::uint64_t cost) {
            auto it = next.find(state);
            if (it == next.end() || cost < it->second) next[std::move(state)] = cost;
        };
        for (const auto& state : states) {
            if (state.first.count(page)) keep_best(state.first, state.second);
            else if (state.first.size() < capacity) {
                Set s = state.first; s.insert(page); keep_best(s, state.second + 1);
            } else {
                for (Page victim : state.first) {
                    Set s = state.first; s.erase(victim); s.insert(page);
                    keep_best(s, state.second + 1);
                }
            }
        }
        states = std::move(next);
    }
    auto best = std::numeric_limits<std::uint64_t>::max();
    for (const auto& s : states) best = std::min(best, s.second);
    return best;
}

template<class P> std::uint64_t checked_run(const std::vector<Page>& trace, std::size_t capacity) {
    P policy(capacity);
    std::uint64_t faults = 0;
    Set previous;
    for (Page page : trace) {
        const bool miss = policy.access(page);
        faults += miss;
        const auto pages = policy.resident_pages();
        const Set current(pages.begin(), pages.end());
        assert(current.size() == pages.size());
        assert(current.size() <= capacity);
        assert(current.count(page));
        assert(miss == !previous.count(page));
        for (Page p : current) assert(previous.count(p) || p == page);
        if (!miss) assert(current == previous);
        if (previous.size() < capacity && miss) assert(current.size() == previous.size() + 1);
        if (previous.size() == capacity) assert(current.size() == capacity);
        previous = current;
        if constexpr (std::is_same<P, ArcPolicy>::value) {
            assert(policy.metadata_size() <= 2 * capacity);
            assert(policy.recency_target() <= capacity);
        }
    }
    return faults;
}

void all_policies(const std::vector<Page>& trace, std::size_t capacity, bool brute) {
    const auto opt = optimal_faults(trace, capacity);
    if (brute) assert(opt == brute_opt(trace, capacity));
    assert(checked_run<FifoPolicy>(trace, capacity) >= opt);
    assert(checked_run<LruPolicy>(trace, capacity) >= opt);
    assert(checked_run<ClockPolicy>(trace, capacity) >= opt);
    assert(checked_run<LfuPolicy>(trace, capacity) >= opt);
    assert(checked_run<MruPolicy>(trace, capacity) >= opt);
    assert(checked_run<ArcPolicy>(trace, capacity) >= opt);
}

int main() {
    const std::vector<Page> classic{7,0,1,2,0,3,0,4,2,3,0,3,2};
    assert(checked_run<FifoPolicy>(classic,3) == 10);
    assert(checked_run<LruPolicy>(classic,3) == 9);
    assert(checked_run<ClockPolicy>(classic,3) == 9);
    assert(optimal_faults(classic,3) == 7);
    const std::vector<Page> belady{1,2,3,4,1,2,5,1,2,3,4,5};
    assert(checked_run<FifoPolicy>(belady,3) == 9);
    assert(checked_run<FifoPolicy>(belady,4) == 10);
    for (std::size_t c = 1; c <= 5; ++c) all_policies({}, c, true);
    // Every trace over 3 pages through length 7, including capacity 1.
    std::uint64_t cases = 0;
    std::vector<Page> trace;
    std::function<void(unsigned)> enumerate = [&](unsigned remaining) {
        for (std::size_t c = 1; c <= 4; ++c) { all_policies(trace, c, true); ++cases; }
        if (!remaining) return;
        for (Page p = 0; p < 3; ++p) {
            trace.push_back(p); enumerate(remaining - 1); trace.pop_back();
        }
    };
    enumerate(7);
    std::mt19937_64 rng(20261028);
    for (unsigned i = 0; i < 300; ++i) {
        std::size_t c = 1 + rng() % 30;
        trace.clear();
        for (unsigned j = 0; j < 1000; ++j) {
            const Page p = (rng() % 5 == 0) ? rng() % 100 : rng() % (c + 3);
            trace.push_back(p);
        }
        all_policies(trace, c, false);
    }
    // High page values and long hit runs exercise tokens without page-index arrays,
    // and OPT heap compaction with repeated next-use entries.
    trace.assign(10000, std::numeric_limits<Page>::max());
    trace.insert(trace.end(), {0, 1, std::numeric_limits<Page>::max(), 0});
    all_policies(trace, 2, false);
    bool rejected = false;
    try { LruPolicy p(0); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    std::cout << "baselines: classic cases, " << cases
              << " exhaustive cases, 300 randomized invariant/lower-bound cases passed\n";
}
