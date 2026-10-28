#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
using Page = std::uint64_t;
struct Policy {
    virtual ~Policy() = default;
    // Returns true exactly when this request faults. Only the current page is supplied.
    virtual bool access(Page page) = 0;
    virtual std::vector<Page> resident_pages() const = 0;
};
