// Small self-implemented PRNG (xorshift64*). Deterministic and identical on every
// compiler/standard library, unlike std::*_distribution (implementation-defined).
#pragma once
#include <cstdint>

namespace lobtest {

class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL) {
        if (s_ == 0) s_ = 0x2545F4914F6CDD1DULL;
        for (int i = 0; i < 4; ++i) next();  // decorrelate nearby seeds
    }
    std::uint64_t next() {
        s_ ^= s_ >> 12; s_ ^= s_ << 25; s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1DULL;
    }
    // Uniform in [0, n). Multiply-shift; negligible bias for small n.
    std::uint32_t below(std::uint32_t n) {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(next() >> 32) * n) >> 32);
    }
    // Uniform in [lo, hi] inclusive.
    std::int32_t range(std::int32_t lo, std::int32_t hi) {
        return lo + static_cast<std::int32_t>(below(static_cast<std::uint32_t>(hi - lo + 1)));
    }
    // True with probability pct/100.
    bool percent(std::uint32_t pct) { return below(100) < pct; }
    // Bernoulli with probability p16/65536 (integer math: no libm, fully portable).
    bool bern16(std::uint32_t p16) { return (next() >> 48) < p16; }
    // Number of failures before the first success, capped. Mean ~ (1-p)/p.
    std::uint32_t geometric(std::uint32_t p16, std::uint32_t cap) {
        std::uint32_t k = 0;
        while (k < cap && !bern16(p16)) ++k;
        return k;
    }
private:
    std::uint64_t s_;
};

} // namespace lobtest
