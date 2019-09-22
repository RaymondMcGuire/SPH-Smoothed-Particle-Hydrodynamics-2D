#ifndef _DETAIL_PARALLEL_INL_H_
#define _DETAIL_PARALLEL_INL_H_

#include "constants.h"
#include <algorithm>
#include <functional>
#include <future>
#include <vector>


// NOTE - This abstraction takes a lambda which should take captured
//        variables by *value* to ensure no captured references race
//        with the task itself.
template <typename TASK_T>
inline void schedule(TASK_T&& fcn) {
    fcn();
}

template <typename TASK_T>
using operator_return_t = typename std::result_of<TASK_T()>::type;

// NOTE - see above, same issues associated with schedule()
template <typename TASK_T>
inline auto async(TASK_T&& fcn) -> std::future<operator_return_t<TASK_T>> {
    using package_t = std::packaged_task<operator_return_t<TASK_T>()>;

    auto task = new package_t(std::forward<TASK_T>(fcn));
    auto future = task->get_future();

    schedule([=]() {
        (*task)();
        delete task;
    });

    return future;
}

// Adopted from:
// Radenski, A.
// Shared Memory, Message Passing, and Hybrid Merge Sorts for Standalone and
// Clustered SMPs. Proc PDPTA'11, the  2011 International Conference on Parallel
// and Distributed Processing Techniques and Applications, CSREA Press
// (H. Arabnia, Ed.), 2011, pp. 367 - 373.
template <typename RandomIterator, typename RandomIterator2,
          typename CompareFunction>
void merge(RandomIterator a, size_t size, RandomIterator2 temp,
           CompareFunction compareFunction) {
    size_t i1 = 0;
    size_t i2 = size / 2;
    size_t tempi = 0;

    while (i1 < size / 2 && i2 < size) {
        if (compareFunction(a[i1], a[i2])) {
            temp[tempi] = a[i1];
            i1++;
        } else {
            temp[tempi] = a[i2];
            i2++;
        }
        tempi++;
    }

    while (i1 < size / 2) {
        temp[tempi] = a[i1];
        i1++;
        tempi++;
    }

    while (i2 < size) {
        temp[tempi] = a[i2];
        i2++;
        tempi++;
    }

    // Copy sorted temp array into main array, a
    parallelFor(kZeroSize, size, [&](size_t i) { a[i] = temp[i]; });
}

template <typename RandomIterator, typename RandomIterator2,
          typename CompareFunction>
void parallelMergeSort(RandomIterator a, size_t size, RandomIterator2 temp,
                       unsigned int numThreads,
                       CompareFunction compareFunction) {
    if (numThreads == 1) {
        std::sort(a, a + size, compareFunction);
    } else if (numThreads > 1) {
        std::vector<std::future<void>> pool;
        pool.reserve(2);

        auto launchRange = [compareFunction](RandomIterator begin, size_t k2,
                                             RandomIterator2 temp,
                                             unsigned int numThreads) {
            parallelMergeSort(begin, k2, temp, numThreads, compareFunction);
        };

        pool.emplace_back(internal::async(
            [=]() { launchRange(a, size / 2, temp, numThreads / 2); }));

        pool.emplace_back(internal::async([=]() {
            launchRange(a + size / 2, size - size / 2, temp + size / 2,
                        numThreads - numThreads / 2);
        }));

        // Wait for jobs to finish
        for (auto& f : pool) {
            if (f.valid()) {
                f.wait();
            }
        }

        merge(a, size, temp, compareFunction);
    }
}

template <typename RandomIterator, typename T>
void parallelFill(const RandomIterator& begin, const RandomIterator& end,
                  const T& value, ExecutionPolicy policy) {
    auto diff = end - begin;
    if (diff <= 0) {
        return;
    }

    size_t size = static_cast<size_t>(diff);
    parallelFor(kZeroSize, size, [begin, value](size_t i) { begin[i] = value; },
                policy);
}

// Adopted from http://ideone.com/Z7zldb
template <typename IndexType, typename Function>
void parallelFor(IndexType start, IndexType end, const Function& func,
                 ExecutionPolicy policy) {
    if (start > end) {
        return;
    }

    if (policy == ExecutionPolicy::kParallel) {
#pragma omp parallel for
        for (auto i = start; i < end; ++i) {

            func(i);
        }
    } else {
        for (auto i = start; i < end; ++i) {
            func(i);
        }
    }
}

template <typename IndexType, typename Function>
void parallelRangeFor(IndexType start, IndexType end, const Function& func,
                      ExecutionPolicy policy) {
    if (start > end) {
        return;
    }

    // Estimate number of threads in the pool
    unsigned int numThreadsHint = maxNumberOfThreads();
    const unsigned int numThreads =
        (policy == ExecutionPolicy::kParallel)
            ? (numThreadsHint == 0u ? 8u : numThreadsHint)
            : 1;

    // Size of a slice for the range functions
    IndexType n = end - start + 1;
    IndexType slice =
        (IndexType)std::round(n / static_cast<double>(numThreads));
    slice = std::max(slice, IndexType(1));

    // Create pool and launch jobs
    std::vector<std::future<void>> pool;
    pool.reserve(numThreads);
    IndexType i1 = start;
    IndexType i2 = std::min(start + slice, end);
    for (unsigned int i = 0; i + 1 < numThreads && i1 < end; ++i) {
        pool.emplace_back(internal::async([=]() { func(i1, i2); }));
        i1 = i2;
        i2 = std::min(i2 + slice, end);
    }
    if (i1 < end) {
        pool.emplace_back(internal::async([=]() { func(i1, end); }));
    }

    // Wait for jobs to finish
    for (auto& f : pool) {
        if (f.valid()) {
            f.wait();
        }
    }
}

template <typename IndexType, typename Function>
void parallelFor(IndexType beginIndexX, IndexType endIndexX,
                 IndexType beginIndexY, IndexType endIndexY,
                 const Function& function, ExecutionPolicy policy) {
    parallelFor(beginIndexY, endIndexY,
                [&](IndexType j) {
                    for (IndexType i = beginIndexX; i < endIndexX; ++i) {
                        function(i, j);
                    }
                },
                policy);
}

template <typename IndexType, typename Function>
void parallelRangeFor(IndexType beginIndexX, IndexType endIndexX,
                      IndexType beginIndexY, IndexType endIndexY,
                      const Function& function, ExecutionPolicy policy) {
    parallelRangeFor(beginIndexY, endIndexY,
                     [&](IndexType jBegin, IndexType jEnd) {
                         function(beginIndexX, endIndexX, jBegin, jEnd);
                     },
                     policy);
}

template <typename IndexType, typename Function>
void parallelFor(IndexType beginIndexX, IndexType endIndexX,
                 IndexType beginIndexY, IndexType endIndexY,
                 IndexType beginIndexZ, IndexType endIndexZ,
                 const Function& function, ExecutionPolicy policy) {
    parallelFor(beginIndexZ, endIndexZ,
                [&](IndexType k) {
                    for (IndexType j = beginIndexY; j < endIndexY; ++j) {
                        for (IndexType i = beginIndexX; i < endIndexX; ++i) {
                            function(i, j, k);
                        }
                    }
                },
                policy);
}

template <typename IndexType, typename Function>
void parallelRangeFor(IndexType beginIndexX, IndexType endIndexX,
                      IndexType beginIndexY, IndexType endIndexY,
                      IndexType beginIndexZ, IndexType endIndexZ,
                      const Function& function, ExecutionPolicy policy) {
    parallelRangeFor(beginIndexZ, endIndexZ,
                     [&](IndexType kBegin, IndexType kEnd) {
                         function(beginIndexX, endIndexX, beginIndexY,
                                  endIndexY, kBegin, kEnd);
                     },
                     policy);
}

template <typename IndexType, typename Value, typename Function,
          typename Reduce>
Value parallelReduce(IndexType start, IndexType end, const Value& identity,
                     const Function& func, const Reduce& reduce,
                     ExecutionPolicy policy) {
    if (start > end) {
        return identity;
    }
    // Estimate number of threads in the pool
    unsigned int numThreadsHint = maxNumberOfThreads();
    const unsigned int numThreads =
        (policy == ExecutionPolicy::kParallel)
            ? (numThreadsHint == 0u ? 8u : numThreadsHint)
            : 1;

    // Size of a slice for the range functions
    IndexType n = end - start + 1;
    IndexType slice =
        (IndexType)std::round(n / static_cast<double>(numThreads));
    slice = std::max(slice, IndexType(1));

    // Results
    std::vector<Value> results(numThreads, identity);

    // [Helper] Inner loop
    auto launchRange = [&](IndexType k1, IndexType k2, unsigned int tid) {
        results[tid] = func(k1, k2, identity);
    };

    // Create pool and launch jobs
    std::vector<std::future<void>> pool;
    pool.reserve(numThreads);
    IndexType i1 = start;
    IndexType i2 = std::min(start + slice, end);
    unsigned int tid = 0;
    for (; tid + 1 < numThreads && i1 < end; ++tid) {
        pool.emplace_back(internal::async([=]() { launchRange(i1, i2, tid); }));
        i1 = i2;
        i2 = std::min(i2 + slice, end);
    }
    if (i1 < end) {
        pool.emplace_back(
            internal::async([=]() { launchRange(i1, end, tid); }));
    }

    // Wait for jobs to finish
    for (auto& f : pool) {
        if (f.valid()) {
            f.wait();
        }
    }

    // Gather
    Value finalResult = identity;
    for (const Value& val : results) {
        finalResult = reduce(val, finalResult);
    }

    return finalResult;
}

template <typename RandomIterator, typename CompareFunction>
void parallelSort(RandomIterator begin, RandomIterator end,
                  CompareFunction compareFunction, ExecutionPolicy policy) {
    if (end < begin) {
        return;
    }

    size_t size = static_cast<size_t>(end - begin);

    typedef
        typename std::iterator_traits<RandomIterator>::value_type value_type;
    std::vector<value_type> temp(size);

    // Estimate number of threads in the pool
    unsigned int numThreadsHint = maxNumberOfThreads();
    const unsigned int numThreads =
        (policy == ExecutionPolicy::kParallel)
            ? (numThreadsHint == 0u ? 8u : numThreadsHint)
            : 1;

    internal::parallelMergeSort(begin, size, temp.begin(), numThreads,
                                compareFunction);
}

template <typename RandomIterator>
void parallelSort(RandomIterator begin, RandomIterator end,
                  ExecutionPolicy policy) {
    parallelSort(
        begin, end,
        std::less<typename std::iterator_traits<RandomIterator>::value_type>(),
        policy);
}

#endif  // _DETAIL_PARALLEL_INL_H_
