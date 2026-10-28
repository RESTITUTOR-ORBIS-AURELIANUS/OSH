#include "../include/adaptive.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <unordered_map>

using Factory = std::function<std::unique_ptr<Policy>(std::size_t)>;
using ResidentSet = std::set<Page>;
struct Step {
    bool fault;
    ResidentSet residents;
    bool operator==(const Step& other) const {
        return fault == other.fault && residents == other.residents;
    }
};

// Test the actual cache, independently of the policy's internal counters and
// experts: every new resident must be the currently requested page.
std::uint64_t check_trace(const Factory& factory, std::size_t capacity,
                          const std::vector<Page>& trace,
                          std::vector<Step>* log = nullptr) {
    auto policy = factory(capacity);
    assert(policy->resident_pages().empty());
    ResidentSet before;
    std::uint64_t faults = 0;
    for (Page requested : trace) {
        const bool fault = policy->access(requested);
        const auto pages = policy->resident_pages();
        const ResidentSet after(pages.begin(), pages.end());
        assert(fault == (before.count(requested) == 0));
        assert(after.size() == pages.size());
        assert(after.size() <= capacity);
        assert(after.count(requested));
        if (!fault) {
            assert(after == before); // A hit cannot swap in some shadow page.
        } else {
            assert(after.size() == std::min(capacity, before.size() + 1));
            for (Page page : after) assert(page == requested || before.count(page));
            std::size_t removed = 0;
            for (Page page : before) removed += !after.count(page);
            assert(removed == (before.size() == capacity ? 1u : 0u));
        }
        if (log) log->push_back({fault, after});
        faults += fault;
        before = after;
    }
    return faults;
}

// Multiplication by an odd number plus an offset is a bijection modulo 2^64.
// This prevents assumptions about small/dense/ordered page IDs from passing.
Page rename_page(Page page) {
    return page * UINT64_C(0xd1342543de82ef95) + UINT64_C(0x9e3779b97f4a7c15);
}

int main() {
    std::vector<std::pair<std::string, Factory>> factories{
        {"adaptive", [](std::size_t c) { return std::make_unique<AdaptivePolicy>(c); }},
        {"predict", [](std::size_t c) { return std::make_unique<PredictivePolicy>(c); }},
        {"decay", [](std::size_t c) { return std::make_unique<DecayPolicy>(c); }},
        {"adaptive-sharp", [](std::size_t c) { return std::make_unique<AdaptivePolicy>(c, 1., .1); }},
        {"predict-no-intervals", [](std::size_t c) { return std::make_unique<PredictivePolicy>(c, 4., false); }},
        {"decay-short", [](std::size_t c) { return std::make_unique<DecayPolicy>(c, .25); }}
    };
    const std::vector<std::size_t> capacities{1,2,5,10,20,50,100};
    std::vector<std::vector<Page>> traces;
    traces.push_back({});
    traces.push_back({0});
    traces.push_back({7,0,1,2,0,3,0,4,2,3,0,3,2});
    traces.push_back(std::vector<Page>(3000, std::numeric_limits<Page>::max()));
    std::vector<Page> unique, cyclic, phases, mixed;
    for (Page i = 0; i < 20000; ++i) unique.push_back(rename_page(i));
    for (Page i = 0; i < 8000; ++i) cyclic.push_back(i % 101);
    for (Page i = 0; i < 8000; ++i) phases.push_back((i / 1000) * 10000 + i % 17);
    std::mt19937_64 rng(20261028);
    for (Page i = 0; i < 18000; ++i) {
        // Keep hot metadata alive while enough distinct cold pages force every
        // default ghost-directory limit (up to 6400 at C=100) to be exercised.
        if (i < 1000 || i % 4 == 0) mixed.push_back(rng() % 7);
        else mixed.push_back(i + 100000);
    }
    traces.push_back(std::move(unique));
    traces.push_back(std::move(cyclic));
    traces.push_back(std::move(phases));
    traces.push_back(std::move(mixed));
    std::size_t checks = 0;
    for (const auto& named : factories) {
        for (std::size_t c : capacities) {
            for (const auto& trace : traces) {
                const auto faults = check_trace(named.second, c, trace);
                assert(faults >= optimal_faults(trace, c));
                ++checks;
            }
        }
        bool rejected = false;
        try { auto bad = named.second(0); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
        std::cout << named.first << ": empty/one-frame/long-stream invariants passed\n";
    }
    // Deterministic independently generated trials; exact states, not merely
    // aggregate fault counts, are compared under renaming and prefix replay.
    for (unsigned trial = 0; trial < 150; ++trial) {
        const std::size_t c = 1 + rng() % 20;
        const std::size_t prefix_size = 80 + rng() % 100;
        std::vector<Page> a, b, renamed;
        for (std::size_t i = 0; i < prefix_size; ++i) a.push_back(rng() % (c + 11));
        b = a;
        for (unsigned i = 0; i < 100; ++i) {
            a.push_back(rng() % (c + 11));
            b.push_back(1000000 + rng() % 57);
        }
        for (Page p : a) renamed.push_back(rename_page(p));
        for (const auto& named : factories) {
            std::vector<Step> la, lb, lr;
            const auto faults = check_trace(named.second, c, a, &la);
            check_trace(named.second, c, b, &lb);
            check_trace(named.second, c, renamed, &lr);
            assert(faults >= optimal_faults(a, c));
            for (std::size_t i = 0; i < prefix_size; ++i) assert(la[i] == lb[i]);
            for (std::size_t i = 0; i < a.size(); ++i) {
                ResidentSet mapped;
                for (Page p : la[i].residents) mapped.insert(rename_page(p));
                assert(lr[i].fault == la[i].fault);
                assert(lr[i].residents == mapped);
            }
            ++checks;
        }
    }
    std::cout << "online policies: " << checks
              << " scenario/configuration checks passed (including 900 prefix and renaming trials)\n";
}
