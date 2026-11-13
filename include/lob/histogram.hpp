// Fixed-size log-linear latency histogram (the HdrHistogram idea, minimal).
//
// Values below 64 get an exact bucket. Above that, each power of two is split
// into 32 equal sub-buckets, so the relative error of any reported percentile
// is at most 1/32 (about 3%). Recording is O(1) with no allocation, which
// matters because the recorder runs inside the measured loop.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace lob {

class Histogram {
public:
    void record(std::uint64_t v) noexcept {
        ++buckets_[index(v)];
        ++count_;
        sum_ += v;
        max_ = std::max(max_, v);
        min_ = std::min(min_, v);
    }
    void merge(const Histogram& o) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) buckets_[i] += o.buckets_[i];
        count_ += o.count_;
        sum_ += o.sum_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
    }
    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t max() const noexcept { return max_; }
    std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
    double mean() const noexcept { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0; }

    // Smallest bucket upper bound such that at least q of the samples are <= it. q in [0, 1].
    std::uint64_t percentile(double q) const noexcept {
        if (count_ == 0) return 0;
        const double want = q * static_cast<double>(count_);
        std::uint64_t target = static_cast<std::uint64_t>(want);
        if (static_cast<double>(target) < want) ++target;  // ceil
        target = std::clamp<std::uint64_t>(target, 1, count_);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += buckets_[i];
            if (seen >= target) return std::min(upper(i), max_);
        }
        return max_;
    }

    static constexpr std::size_t kBuckets = 64 + 58 * 32;

    static constexpr std::size_t index(std::uint64_t v) noexcept {
        if (v < 64) return static_cast<std::size_t>(v);
        const unsigned e = 63u - static_cast<unsigned>(std::countl_zero(v));  // >= 6
        return 64 + (e - 6) * 32 + static_cast<std::size_t>((v >> (e - 5)) & 31);
    }
    // Largest value that maps to bucket i.
    static constexpr std::uint64_t upper(std::size_t i) noexcept {
        if (i < 64) return i;
        const unsigned e = 6 + static_cast<unsigned>((i - 64) / 32);
        const std::uint64_t sub = (i - 64) % 32;
        const std::uint64_t width = 1ull << (e - 5);
        return (1ull << e) + sub * width + (width - 1);
    }

private:
    std::array<std::uint64_t, kBuckets> buckets_{};
    std::uint64_t count_ = 0, sum_ = 0, max_ = 0, min_ = ~0ull;
};

}  // namespace lob
