#pragma once
#include "policy.hpp"
#include <algorithm>
#include <deque>
#include <list>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// All online policies start empty, load the requested page on a fault, and
// evict at most one resident page. Ghost records contain metadata, not pages.
inline std::size_t checked_capacity(std::size_t capacity) {
    if (capacity == 0) throw std::invalid_argument("frame count must be positive");
    return capacity;
}

class FifoPolicy final : public Policy {
    std::size_t capacity_;
    std::deque<Page> fifo_;
    std::unordered_set<Page> resident_;
public:
    explicit FifoPolicy(std::size_t capacity) : capacity_(checked_capacity(capacity)) {}
    bool access(Page page) override {
        if (resident_.count(page)) return false;
        if (fifo_.size() == capacity_) {
            resident_.erase(fifo_.front());
            fifo_.pop_front();
        }
        fifo_.push_back(page);
        resident_.insert(page);
        return true;
    }
    std::vector<Page> resident_pages() const override { return {fifo_.begin(), fifo_.end()}; }
};

// Front is most recently used; back is least recently used.
class LruPolicy final : public Policy {
    std::size_t capacity_;
    std::list<Page> order_;
    std::unordered_map<Page, std::list<Page>::iterator> positions_;
public:
    explicit LruPolicy(std::size_t capacity) : capacity_(checked_capacity(capacity)) {}
    bool access(Page page) override {
        auto it = positions_.find(page);
        if (it != positions_.end()) {
            order_.splice(order_.begin(), order_, it->second);
            return false;
        }
        if (order_.size() == capacity_) {
            positions_.erase(order_.back());
            order_.pop_back();
        }
        order_.push_front(page);
        positions_[page] = order_.begin();
        return true;
    }
    std::vector<Page> resident_pages() const override { return {order_.begin(), order_.end()}; }
};

class MruPolicy final : public Policy {
    std::size_t capacity_;
    std::list<Page> order_;
    std::unordered_map<Page, std::list<Page>::iterator> positions_;
public:
    explicit MruPolicy(std::size_t capacity) : capacity_(checked_capacity(capacity)) {}
    bool access(Page page) override {
        auto it = positions_.find(page);
        if (it != positions_.end()) {
            order_.splice(order_.begin(), order_, it->second);
            return false;
        }
        if (order_.size() == capacity_) {
            positions_.erase(order_.front());
            order_.pop_front();
        }
        order_.push_front(page);
        positions_[page] = order_.begin();
        return true;
    }
    std::vector<Page> resident_pages() const override { return {order_.begin(), order_.end()}; }
};

// Circular implementation of Second Chance. A newly loaded page has R = 1.
class ClockPolicy final : public Policy {
    std::size_t capacity_, hand_ = 0;
    std::vector<Page> frames_;
    std::vector<unsigned char> referenced_;
    std::unordered_map<Page, std::size_t> positions_;
public:
    explicit ClockPolicy(std::size_t capacity) : capacity_(checked_capacity(capacity)) {}
    bool access(Page page) override {
        auto it = positions_.find(page);
        if (it != positions_.end()) {
            referenced_[it->second] = 1;
            return false;
        }
        if (frames_.size() < capacity_) {
            positions_[page] = frames_.size();
            frames_.push_back(page);
            referenced_.push_back(1);
        } else {
            while (referenced_[hand_]) {
                referenced_[hand_] = 0;
                hand_ = (hand_ + 1) % capacity_;
            }
            positions_.erase(frames_[hand_]);
            frames_[hand_] = page;
            referenced_[hand_] = 1;
            positions_[page] = hand_;
            hand_ = (hand_ + 1) % capacity_;
        }
        return true;
    }
    std::vector<Page> resident_pages() const override { return frames_; }
};

// Historical LFU: counts include previous visits before eviction. Ties use LRU.
// This intentionally keeps O(U) counters for U distinct pages in the stream;
// unlike ARC, it is not a bounded-history policy and can retain stale popularity.
class LfuPolicy final : public Policy {
    std::size_t capacity_;
    std::uint64_t time_ = 0;
    struct Record { std::uint64_t count = 0, last = 0; };
    std::unordered_map<Page, Record> history_;
    std::unordered_set<Page> resident_;
public:
    explicit LfuPolicy(std::size_t capacity) : capacity_(checked_capacity(capacity)) {}
    bool access(Page page) override {
        auto& record = history_[page];
        ++record.count;
        record.last = ++time_;
        if (resident_.count(page)) return false;
        if (resident_.size() == capacity_) {
            auto victim = resident_.begin();
            for (auto it = resident_.begin(); it != resident_.end(); ++it) {
                const auto& a = history_.at(*it);
                const auto& b = history_.at(*victim);
                if (a.count < b.count || (a.count == b.count && a.last < b.last)) victim = it;
            }
            resident_.erase(victim);
        }
        resident_.insert(page);
        return true;
    }
    std::vector<Page> resident_pages() const override { return {resident_.begin(), resident_.end()}; }
};

// ARC: two resident LRU lists T1/T2 and two nonresident ghost lists B1/B2.
// Megiddo and Modha, FAST 2003, Figure 4 (integer target/ratio variant):
// https://www.usenix.org/legacy/event/fast03/tech/full_papers/megiddo/megiddo.pdf
// p is the adaptive integer target for T1. Total metadata never exceeds 2*C.
class ArcPolicy final : public Policy {
    struct Queue {
        std::list<Page> order;
        std::unordered_map<Page, std::list<Page>::iterator> positions;
        std::size_t size() const { return order.size(); }
        bool contains(Page page) const { return positions.count(page) != 0; }
        void erase(Page page) {
            auto it = positions.find(page);
            if (it == positions.end()) throw std::logic_error("ARC queue membership invariant");
            order.erase(it->second);
            positions.erase(it);
        }
        void push(Page page) {
            order.push_front(page);
            positions.emplace(page, order.begin());
        }
        Page pop_lru() {
            if (order.empty()) throw std::logic_error("ARC empty victim queue");
            Page page = order.back();
            erase(page);
            return page;
        }
    };
    std::size_t capacity_, p_ = 0;
    Queue t1_, t2_, b1_, b2_;
    void replace(Page requested) {
        if (t1_.size() != 0 &&
            (t1_.size() > p_ || (b2_.contains(requested) && t1_.size() == p_))) {
            b1_.push(t1_.pop_lru());
        } else {
            b2_.push(t2_.pop_lru());
        }
    }
public:
    explicit ArcPolicy(std::size_t capacity) : capacity_(checked_capacity(capacity)) {}
    bool access(Page page) override {
        if (t1_.contains(page)) {
            t1_.erase(page);
            t2_.push(page);
            return false;
        }
        if (t2_.contains(page)) {
            t2_.erase(page);
            t2_.push(page);
            return false;
        }
        if (b1_.contains(page)) {
            const auto delta = std::max<std::size_t>(1, b2_.size() / b1_.size());
            p_ = std::min(capacity_, p_ + delta);
            replace(page);
            b1_.erase(page);
            t2_.push(page);
        } else if (b2_.contains(page)) {
            const auto delta = std::max<std::size_t>(1, b1_.size() / b2_.size());
            p_ = delta >= p_ ? 0 : p_ - delta;
            replace(page);
            b2_.erase(page);
            t2_.push(page);
        } else {
            if (t1_.size() + b1_.size() == capacity_) {
                if (t1_.size() < capacity_) {
                    b1_.pop_lru();
                    replace(page);
                } else {
                    t1_.pop_lru();
                }
            } else {
                const auto total = t1_.size() + t2_.size() + b1_.size() + b2_.size();
                if (total >= capacity_) {
                    if (total == 2 * capacity_) b2_.pop_lru();
                    replace(page);
                }
            }
            t1_.push(page);
        }
        return true;
    }
    std::vector<Page> resident_pages() const override {
        std::vector<Page> result(t1_.order.begin(), t1_.order.end());
        result.insert(result.end(), t2_.order.begin(), t2_.order.end());
        return result;
    }
    std::size_t metadata_size() const { return t1_.size() + t2_.size() + b1_.size() + b2_.size(); }
    std::size_t recency_target() const { return p_; }
};

// Offline Belady/MIN lower bound. This function alone reads future references.
// Precomputing next uses avoids a repeated forward scan. A max heap picks the
// resident page whose next access is farthest away (or never occurs again).
inline std::uint64_t optimal_faults(const std::vector<Page>& trace, std::size_t capacity) {
    checked_capacity(capacity);
    const std::size_t never = trace.size();
    std::vector<std::size_t> next_use(trace.size(), never);
    std::unordered_map<Page, std::size_t> next;
    for (std::size_t i = trace.size(); i-- > 0;) {
        auto it = next.find(trace[i]);
        if (it != next.end()) next_use[i] = it->second;
        next[trace[i]] = i;
    }
    // Lazy entries are checked against the resident page's current next use.
    using Entry = std::pair<std::size_t, Page>;
    std::priority_queue<Entry> heap;
    std::unordered_map<Page, std::size_t> resident;
    std::uint64_t faults = 0;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        const Page page = trace[i];
        if (!resident.count(page)) {
            ++faults;
            if (resident.size() == capacity) {
                for (;;) {
                    const auto candidate = heap.top();
                    auto it = resident.find(candidate.second);
                    if (it != resident.end() && it->second == candidate.first) break;
                    heap.pop();
                }
                resident.erase(heap.top().second);
                heap.pop();
            }
        }
        resident[page] = next_use[i];
        heap.emplace(next_use[i], page);
        // Prevent stale heap entries from growing to O(N) on long hit runs.
        if (heap.size() > 4 * capacity + 64) {
            std::priority_queue<Entry> compact;
            for (const auto& pair : resident) compact.emplace(pair.second, pair.first);
            heap.swap(compact);
        }
    }
    return faults;
}
