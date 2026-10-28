#pragma once
#include "policy.hpp"
#include <algorithm>
#include <cmath>
#include <list>
#include <stdexcept>
#include <unordered_map>

// Exponentially decayed frequency: each past reference contributes
// 2^(-(now-reference_time)/half_life). The page with the lowest sum is evicted.
// Ghost entries remember statistics only, never page contents.
class DecayPolicy : public Policy {
    struct Entry {
        double log_score = 0;
        std::uint64_t last = 0;
        bool resident = false;
        std::list<Page>::iterator position;
    };
    std::size_t capacity_, history_limit_;
    double half_life_;
    std::uint64_t now_ = 0;
    std::unordered_map<Page, Entry> history_;
    std::list<Page> recent_;
    std::vector<Page> cache_;
    double score(const Entry& e) const {
        return e.log_score - static_cast<double>(now_ - e.last) / half_life_;
    }
    void trim() {
        // Resident pages are never removed from the statistics directory.
        auto it = recent_.end();
        while (history_.size() > history_limit_ && it != recent_.begin()) {
            --it;
            if (history_.at(*it).resident) continue;
            history_.erase(*it);
            it = recent_.erase(it);
        }
    }
public:
    explicit DecayPolicy(std::size_t capacity, double half_life_factor = 16)
        : capacity_(capacity), history_limit_(std::max<std::size_t>(256, 32 * capacity)),
          half_life_(std::max(1.0, capacity * half_life_factor)) {
        if (!capacity || !(half_life_factor > 0)) throw std::invalid_argument("invalid decay capacity/half life");
        cache_.reserve(capacity);
    }
    bool access(Page page) override {
        ++now_;
        auto found = history_.find(page);
        bool fault = found == history_.end() || !found->second.resident;
        if (found == history_.end()) {
            recent_.push_front(page);
            Entry e; e.last = now_; e.position = recent_.begin();
            history_.emplace(page, e);
        } else {
            Entry& e = found->second;
            const double old = score(e);
            e.log_score = old < -60 ? 0 : std::log2(1.0 + std::exp2(old));
            e.last = now_;
            recent_.splice(recent_.begin(), recent_, e.position);
        }
        if (fault) {
            if (cache_.size() == capacity_) {
                std::size_t victim = 0;
                for (std::size_t i = 1; i < cache_.size(); ++i) {
                    const Entry &a = history_.at(cache_[i]), &b = history_.at(cache_[victim]);
                    const double sa = score(a), sb = score(b);
                    if (sa < sb || (sa == sb && a.last < b.last)) victim = i;
                }
                history_.at(cache_[victim]).resident = false;
                cache_[victim] = page;
            } else cache_.push_back(page);
            history_.at(page).resident = true;
        }
        trim();
        return fault;
    }
    std::vector<Page> resident_pages() const override { return cache_; }
};
