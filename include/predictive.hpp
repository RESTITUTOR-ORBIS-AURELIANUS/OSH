#pragma once
#include "policy.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

// A past-only reuse predictor. The metadata directory contains page identities and
// statistics, never page contents; it does not enlarge the resident frame set.
class PredictivePolicy final : public Policy {
    struct History {
        std::uint64_t last = 0;
        double frequency = 1.0;   // exponentially decayed reference count
        double gap = 0.0;         // most recent observed reuse interval
        unsigned stable = 0;     // consecutive similar reuse intervals
        std::list<Page>::iterator position;
    };
    std::size_t capacity_, history_limit_;
    double window_;
    bool use_intervals_;
    std::uint64_t clock_ = 0;
    std::unordered_map<Page, History> history_;
    std::list<Page> recent_;
    std::vector<Page> frames_;
    std::unordered_set<Page> resident_;

    double delay(const History& h) const {
        const double age = static_cast<double>(clock_ - h.last);
        const double mass = h.frequency * std::exp(-age / window_);
        double estimate = window_ / std::max(mass, 1e-12);
        if (use_intervals_ && h.stable >= 2 && h.gap > 0.0 &&
            age <= h.gap * 1.25) {
            // Two matching observed gaps establish a provisional periodic model.
            // After it misses its deadline, fall back to the frequency estimate.
            const double remaining = std::max(1.0, h.gap - age);
            const double confidence = h.stable >= 3 ? 1.0 : 0.75;
            estimate = confidence * remaining + (1.0 - confidence) * estimate;
        }
        return estimate;
    }

    void trim_history() {
        // Resident metadata is always retained. At most C resident entries can
        // obstruct removal from the oldest end of this bounded directory.
        while (history_.size() > history_limit_) {
            auto it = recent_.end();
            do { --it; } while (resident_.count(*it));
            history_.erase(*it);
            recent_.erase(it);
        }
    }
public:
    explicit PredictivePolicy(std::size_t capacity, double window_multiple = 8.0,
                              bool use_intervals = true)
        : capacity_(capacity), history_limit_(std::max<std::size_t>(512, capacity * 64)),
          window_(std::max(1.0, window_multiple * capacity)),
          use_intervals_(use_intervals) {
        if (!capacity || !(window_multiple > 0.0) || !std::isfinite(window_multiple))
            throw std::invalid_argument("positive capacity and finite decay window required");
        frames_.reserve(capacity);
        resident_.reserve(capacity * 2);
        history_.reserve(history_limit_ + 1);
    }

    bool access(Page page) override {
        ++clock_;
        auto it = history_.find(page);
        if (it == history_.end()) {
            recent_.push_front(page);
            History h;
            h.last = clock_;
            h.position = recent_.begin();
            it = history_.emplace(page, h).first;
        } else {
            History& h = it->second;
            const double gap = static_cast<double>(clock_ - h.last);
            h.frequency = h.frequency * std::exp(-gap / window_) + 1.0;
            if (h.gap > 0.0 && std::abs(gap - h.gap) <= std::max(1.0, h.gap * 0.10))
                h.stable = std::min(32u, h.stable + 1);
            else
                h.stable = 1;
            h.gap = gap;
            h.last = clock_;
            recent_.splice(recent_.begin(), recent_, h.position);
        }
        const bool fault = !resident_.count(page);
        if (fault) {
            if (frames_.size() == capacity_) {
                std::size_t victim = 0;
                double largest = -1.0;
                for (std::size_t i = 0; i < frames_.size(); ++i) {
                    const History& h = history_.at(frames_[i]);
                    const double d = delay(h);
                    const History& old = history_.at(frames_[victim]);
                    if (d > largest || (d == largest && h.last < old.last)) {
                        largest = d;
                        victim = i;
                    }
                }
                resident_.erase(frames_[victim]);
                frames_[victim] = page;
            } else {
                frames_.push_back(page);
            }
            resident_.insert(page);
        }
        trim_history();
        return fault;
    }
    std::vector<Page> resident_pages() const override { return frames_; }
};
