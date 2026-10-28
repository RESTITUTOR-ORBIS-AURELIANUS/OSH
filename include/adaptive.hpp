#pragma once
#include "policy.hpp"
#include "baselines.hpp"
#include "decay.hpp"
#include "predictive.hpp"
#include "replay.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

// Adaptive expert replacement (AER).
// Discounted shadow faults select a leader. Switching requires a 0.5*C
// advantage to avoid repeatedly undoing earlier eviction choices.
// On an actual fault, evict ONLY a real resident absent from the leader's
// updated shadow set. Weighted expert votes break ties within that set.
// A stable leader can thus be approached without loading unrequested pages.
// Each expert has a SHADOW cache containing page IDs only. Real hits/faults
// are counted solely from cache_. Shadow contents can never be copied in.
class AdaptivePolicy : public Policy {
    std::size_t capacity_;
    double discount_, temperature_, switch_cost_;
    std::size_t leader_ = 0;
    std::uint64_t now_ = 0;
    std::vector<std::unique_ptr<Policy>> experts_;
    std::vector<double> losses_;
    std::unordered_map<Page, std::uint64_t> cache_;
public:
    explicit AdaptivePolicy(std::size_t capacity, double window_factor = 32,
                            double temperature = 4, double switch_cost = 0.5,
                            bool use_replay = true)
        : capacity_(capacity),
          discount_(std::exp2(-1.0 / std::max(64.0, capacity * window_factor))),
          temperature_(temperature), switch_cost_(switch_cost) {
        if (!capacity || !(window_factor > 0) || !(temperature > 0) || !(switch_cost >= 0) ||
            !std::isfinite(window_factor) || !std::isfinite(temperature) || !std::isfinite(switch_cost))
            throw std::invalid_argument("invalid adaptive parameter");
        experts_.emplace_back(new LruPolicy(capacity));
        experts_.emplace_back(new ArcPolicy(capacity));
        experts_.emplace_back(new MruPolicy(capacity));
        for (double factor : {2., 8., 32., 128.})
            experts_.emplace_back(new DecayPolicy(capacity, factor));
        for (double factor : {1., 4., 16.})
            experts_.emplace_back(new PredictivePolicy(capacity, factor));
        if (use_replay) experts_.emplace_back(new HistoryReplayPolicy(capacity, 1));
        losses_.resize(experts_.size());
    }
    bool access(Page page) override {
        ++now_;
        for (std::size_t j = 0; j < experts_.size(); ++j)
            losses_[j] = discount_ * losses_[j] + (experts_[j]->access(page) ? 1.0 : 0.0);
        const auto best_expert = static_cast<std::size_t>(std::min_element(losses_.begin(), losses_.end()) - losses_.begin());
        if (losses_[leader_] - losses_[best_expert] > std::max(1.0, switch_cost_ * capacity_)) leader_ = best_expert;
        auto found = cache_.find(page);
        if (found != cache_.end()) { found->second = now_; return false; }
        if (cache_.size() == capacity_) {
            const double best = *std::min_element(losses_.begin(), losses_.end());
            std::unordered_map<Page, double> support;
            support.reserve(capacity_ * 2);
            for (std::size_t j = 0; j < experts_.size(); ++j) {
                double weight = std::exp(-(losses_[j] - best) / temperature_);
                if (weight < 1e-9) continue;
                for (Page p : experts_[j]->resident_pages()) support[p] += weight;
            }
            const auto leader_pages = experts_[leader_]->resident_pages();
            std::unordered_set<Page> target(leader_pages.begin(), leader_pages.end());
            auto victim = cache_.begin();
            double weakest = std::numeric_limits<double>::infinity();
            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (target.count(it->first)) continue;
                const double vote = support[it->first];
                if (vote < weakest || (vote == weakest && it->second < victim->second)) {
                    weakest = vote; victim = it;
                }
            }
            cache_.erase(victim);
        }
        cache_[page] = now_; // Only the page actually demanded is loaded.
        return true;
    }
    std::vector<Page> resident_pages() const override {
        std::vector<Page> pages;
        pages.reserve(cache_.size());
        for (const auto& item : cache_) pages.push_back(item.first);
        return pages;
    }
};
