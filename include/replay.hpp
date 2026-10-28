#pragma once
#include "policy.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

// Experimental expert: use a repeated suffix's already-observed continuation as
// a *prediction*. Predicted pages are never loaded; every actual load is caused
// by access(page). No trace, trace length, or future request enters this class.
class HistoryReplayPolicy final : public Policy {
    struct Frequency {
        std::uint64_t last;
        double mass;
        std::list<Page>::iterator position;
    };
    std::size_t capacity_, history_limit_, min_context_, max_context_;
    std::uint64_t time_ = 0;
    double window_;
    std::vector<Page> history_, frames_;
    std::unordered_map<Page,std::deque<std::uint64_t>> occurrences_;
    std::unordered_map<Page,Frequency> stats_;
    std::unordered_set<Page> resident_;
    std::list<Page> recency_;

    std::size_t frequency_victim() const {
        std::size_t victim=0;
        double smallest=std::numeric_limits<double>::infinity();
        for(std::size_t i=0;i<frames_.size();++i) {
            const auto& f=stats_.at(frames_[i]);
            const double value=f.mass*std::exp(-double(time_-f.last)/window_);
            if(value<smallest || (value==smallest && f.last<stats_.at(frames_[victim]).last)) {
                smallest=value; victim=i;
            }
        }
        return victim;
    }
    std::size_t choose_victim(Page current) const {
        const auto occ=occurrences_.find(current);
        if(occ==occurrences_.end()) return frequency_victim();
        const std::uint64_t oldest=time_>=history_limit_?time_-history_limit_+1:0;
        std::size_t best_length=0;
        std::uint64_t best_position=0;
        unsigned checked=0;
        for(auto it=occ->second.rbegin();it!=occ->second.rend() && checked<64;++it,++checked) {
            const auto old=*it;
            if(old<oldest || old>=time_)continue;
            std::size_t n=1;
            while(n<max_context_ && n<=old-oldest && n<=time_-oldest &&
                  history_[(old-n)%history_limit_]==history_[(time_-n)%history_limit_]) ++n;
            if(n>best_length) { best_length=n; best_position=old; }
            if(n==max_context_)break;
        }
        if(best_length<min_context_) return frequency_victim();
        const auto period=time_-best_position;
        const std::uint64_t horizon=std::max<std::uint64_t>(period,4*capacity_);
        std::unordered_map<Page,std::uint64_t> next;
        next.reserve(capacity_*2);
        // Predict only from the part of that continuation that has happened.
        // A sufficiently long suffix match also allows repeating the observed
        // cycle, still a fallible prediction rather than privileged future data.
        const bool can_repeat=best_length>=std::min<std::uint64_t>(8,period);
        const auto length=can_repeat?horizon:std::min(horizon,period);
        for(std::uint64_t distance=1;distance<=length;++distance) {
            const auto source=best_position+1+(distance-1)%period;
            const Page p=history_[source%history_limit_];
            if(resident_.count(p))next.emplace(p,distance);
        }
        if(next.empty())return frequency_victim();
        std::size_t victim=frequency_victim();
        auto rank=[&](Page p) { auto it=next.find(p);return it==next.end()?horizon+1:it->second; };
        auto largest=rank(frames_[victim]);
        for(std::size_t i=0;i<frames_.size();++i) {
            auto r=rank(frames_[i]);
            if(r>largest) {victim=i;largest=r;}
        }
        return victim;
    }
public:
    explicit HistoryReplayPolicy(std::size_t capacity, std::size_t min_context=3,
                                 std::size_t max_context=32, double window_multiple=4.0)
      :capacity_(capacity),history_limit_(std::max<std::size_t>(1024,64*capacity)),
       min_context_(min_context),max_context_(max_context),
       window_(std::max(1.0,window_multiple*capacity)),history_(history_limit_) {
        if(!capacity || !min_context || max_context<min_context || !(window_multiple>0) || !std::isfinite(window_multiple))
            throw std::invalid_argument("invalid replay policy parameters");
        frames_.reserve(capacity);
    }
    bool access(Page page) override {
        if(time_>=history_limit_) {
            Page old=history_[time_%history_limit_];
            auto it=occurrences_.find(old);
            if(it!=occurrences_.end()) {
                it->second.pop_front();
                if(it->second.empty())occurrences_.erase(it);
            }
        }
        history_[time_%history_limit_]=page;
        auto st=stats_.find(page);
        if(st==stats_.end()) {
            recency_.push_front(page);
            stats_.emplace(page,Frequency{time_,1.0,recency_.begin()});
        }else{
            auto& f=st->second;
            f.mass=f.mass*std::exp(-double(time_-f.last)/window_)+1.0;
            f.last=time_;recency_.splice(recency_.begin(),recency_,f.position);
        }
        const bool fault=!resident_.count(page);
        if(fault){
            if(frames_.size()==capacity_) {
                const auto victim=choose_victim(page);
                resident_.erase(frames_[victim]);frames_[victim]=page;
            }else frames_.push_back(page);
            resident_.insert(page);
        }
        occurrences_[page].push_back(time_);
        while(stats_.size()>history_limit_) {
            auto it=recency_.end();
            do{--it;}while(resident_.count(*it));
            stats_.erase(*it);recency_.erase(it);
        }
        ++time_;
        return fault;
    }
    std::vector<Page> resident_pages()const override{return frames_;}
};
