/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#pragma once

#include <cstdint>
#include <string_view>

#include <folly/hash/Hash.h>

// XXH_INLINE_ALL causes hashes to be non-deterministic with the linux conda build, needs investigating
// https://github.com/man-group/ArcticDB/issues/1268
#if defined(ARCTICDB_USING_CONDA) && defined(__linux__)
#define XXH_STATIC_LINKING_ONLY
#else
// xxhash does some raw pointer manipulation that are safe, but compilers view as violating array bounds
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>
#if defined(ARCTICDB_USING_CONDA) && defined(__linux__)
#undef XXH_STATIC_LINKING_ONLY
#else
#undef XXH_INLINE_ALL
#pragma GCC diagnostic pop
#endif

namespace arcticdb {

using HashedValue = XXH64_hash_t;
constexpr std::size_t DEFAULT_SEED = 0x42;

template<class T, std::size_t seed = DEFAULT_SEED>
HashedValue hash(T *d, std::size_t count) {
    return XXH64(reinterpret_cast<const void *>(d), count * sizeof(T), seed);
}

// size argument to XXH64 being compile-time constant improves performance
template<class T, std::size_t seed = DEFAULT_SEED>
HashedValue hash(T *d) {
    return XXH64(reinterpret_cast<const void *>(d), sizeof(T), seed);
}

inline HashedValue hash(std::string_view sv) {
    return hash(sv.data(), sv.size());
}

class HashAccum {
  public:
    explicit HashAccum(HashedValue seed = DEFAULT_SEED) {
        reset(seed);
    }

    void reset(HashedValue seed = DEFAULT_SEED) {
        XXH64_reset(&state_, seed);
    }

    template<typename T>
    void operator()(T *d, std::size_t count = 1) {
        XXH64_update(&state_, d, sizeof(T) * count);
    }

    [[nodiscard]] HashedValue digest() const {
        return XXH64_digest(&state_);
    }
  private:
    XXH64_state_t state_ = XXH64_state_t{};
};

} // namespace arcticdb
