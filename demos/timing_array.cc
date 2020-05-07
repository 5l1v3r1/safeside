/*
 * Copyright 2020 Google LLC
 *
 * Licensed under both the 3-Clause BSD License and the GPLv2, found in the
 * LICENSE and LICENSE.GPL-2.0 files, respectively, in the root directory.
 *
 * SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
 */

#include "timing_array.h"

#include <stdlib.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "asm/measurereadlatency.h"
#include "instr.h"
#include "utils.h"

TimingArray::TimingArray() {
  // Explicitly initialize the elements of the array.
  //
  // It's not important what value we write as long as we force *something* to
  // be written to each element. Otherwise, the backing allocation could be a
  // range of zero-fill-on-demand (ZFOD), copy-on-write pages that all start
  // off mapped to the same physical page of zeros. Since the cache on modern
  // Intel CPUs is physically tagged, some elements might map to the same cache
  // line and we wouldn't observe a timing difference between reading accessed
  // and unaccessed elements.
  for (int i = 0; i < size(); ++i) {
    ElementAt(i) = -1;
  }

  // Init the first time through, then keep for later instances.
  static uint64_t threshold = FindCachedReadLatencyThreshold();
  cached_read_latency_threshold_ = threshold;
}

void TimingArray::FlushFromCache() {
  // We only need to flush the cache lines with elements on them.
  for (int i = 0; i < size(); ++i) {
    FlushDataCacheLineNoBarrier(&ElementAt(i));
  }

  // Wait for flushes to finish.
  MemoryAndSpeculationBarrier();
}

int TimingArray::FindFirstCachedElementIndexAfter(int start_after) {
  // Fail if element is out of bounds.
  if (start_after >= size()) {
    return -1;
  }

  // Start at the element after `start_after`, wrapping around until we've
  // found a cached element or tried every element.
  for (int i = 1; i <= size(); ++i) {
    int el = (start_after + i) % size();
    uint64_t read_latency = MeasureReadLatency(&ElementAt(el));
    if (read_latency <= cached_read_latency_threshold_) {
      return el;
    }
  }

  // Didn't find a cached element.
  return -1;
}

int TimingArray::FindFirstCachedElementIndex() {
  // Start "after" the last element, which means start at the first.
  return FindFirstCachedElementIndexAfter(size() - 1);
}

// Determines a threshold value (as returned from MeasureReadLatency) where:
//   - Reads _at or below_ the threshold _almost certainly_ came from cache
//   - Reads _above_ the threshold _probably_ came from memory
//
// There are a lot of ways to arrive at such a threshold value. Our current
// approach is, roughly:
//   1. Flush all array elements from cache.
//   2. Read all elements into cache, noting the fastest read.
//   3. Read all elements again, now from cache, noting the slowest read.
//      Take the midpoint of the "fastest uncached" and "slowest cached" and
//      call it a "candidate threshold".
//   4. Repeat (1)-(3) many times to get a distribution of candidate thresholds.
//   5. Choose a threshold from the low side of that distribution.
//
// In a previous attempt, we just collected the "slowest cached" distribution
// and sampled the threshold from there. That worked in many cases, but failed
// when the observed latency of cached reads could change over time -- for
// example, on laptops with aggressive dynamic frequency scaling.
//
// Now we also measure "fastest uncached" and use it to bias the threshold up.
// The hope is we add margin to more reliably recognize true positives without
// meaningfully increasing the likelihood of false postives.
uint64_t TimingArray::FindCachedReadLatencyThreshold() {
  const int iterations = 10000;

  // A few considerations for choosing the percentile:
  //   - Lower means fewer false positives
  //   - Too low and we see outliers from uncached reads that were "too fast",
  //     e.g. places where we failed to defeat the prefetcher
  //   - Too high and we see outliers from slow cached *or* uncached reads,
  //     e.g. measurements that were interrupted by context switches, or
  //     where a coscheduled hyperthread introduced cache contention.
  const int percentile = 10;

  // For testing, allow the threshold to be specified as an environment
  // variable.
  const char *threshold_from_env = getenv("CACHED_THRESHOLD");
  if (threshold_from_env) {
    return atoi(threshold_from_env);
  }

  std::vector<uint64_t> candidate_thresholds;
  for (int n = 0; n < iterations; ++n) {
    FlushFromCache();

    // We have to measure reads across all elements of the array, since that's
    // what the code that later *uses* our computed threshold will be doing.
    // Certain measurement artifacts only appear when reading many values:
    //   - TimingArray forces values onto different pages, which introduces TLB
    //     pressure. After re-reading ~64 elements of a 256-element array on an
    //     Intel Xeon processor, we see latency increase as we start hitting the
    //     L2 TLB. The 7-Zip LZMA benchmarks have some useful measurements on
    //     this: https://www.7-cpu.com/cpu/Skylake.html
    //   - Our first version of TimingArray didn't implement cache coloring and
    //     all elements were contending for the same cache set. As a result,
    //     after reading 256 values, we had evicted all but the first ~8 from
    //     the L1 cache. Our threshold computation took this into account. If we
    //     had just looped over reading one memory value, the computed threshold
    //     would have been too low to classify reads from L2 or L3 cache.

    uint64_t fastest_uncached = std::numeric_limits<uint64_t>::max();
    for (int i = 0; i < size(); ++i) {
      fastest_uncached =
          std::min(fastest_uncached, MeasureReadLatency(&ElementAt(i)));
    }

    uint64_t slowest_cached = std::numeric_limits<uint64_t>::min();  // aka 0
    for (int i = 0; i < size(); ++i) {
      slowest_cached =
          std::max(slowest_cached, MeasureReadLatency(&ElementAt(i)));
    }

    // The candidate threshold is the midpoint.
    candidate_thresholds.push_back((fastest_uncached + slowest_cached) / 2);
  }

  // Return the Nth% candidate threshold.
  std::sort(candidate_thresholds.begin(), candidate_thresholds.end());
  int index = (percentile / 100.0) * (iterations - 1);
  return candidate_thresholds[index];
}
