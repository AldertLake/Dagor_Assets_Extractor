#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace DagorAssetsExtractor::Jobs
{
    enum class JobPriority
    {
        Normal = 0,
        High = 1
    };

    class JobSystem
    {
    public:
        explicit JobSystem(size_t workerCount = 0);
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;
        JobSystem(JobSystem&&) = delete;
        JobSystem& operator=(JobSystem&&) = delete;

        static size_t ComputeDefaultWorkerCount();
        static size_t ComputeWorkerCount(size_t taskCount = 0, size_t reserveThreads = 0);
        static size_t MaxWorkerCount();

        static JobSystem* GetDefaultPool() noexcept;
        static void SetDefaultPool(JobSystem* pool) noexcept;

        void SetWorkerCount(size_t workerCount);
        [[nodiscard]] size_t WorkerCount() const;

        void Start(std::string name, std::function<void()> work, JobPriority priority = JobPriority::Normal);
        void JoinFinished();
        void JoinAll();
        [[nodiscard]] size_t RunningCount() const;

        template <typename Func>
        void ParallelFor(size_t count, size_t grainSize, Func&& fn)
        {
            if (count == 0)
            {
                return;
            }

            const size_t grain = std::max<size_t>(1, grainSize);
            const size_t chunkCount = (count + grain - 1) / grain;
            const size_t configuredWorkers = WorkerCount();

            auto invokeAt = [&](size_t itemIndex, size_t workerSlot) {
                if constexpr (std::is_invocable_v<Func&, size_t, size_t>)
                {
                    fn(itemIndex, workerSlot);
                }
                else
                {
                    (void)workerSlot;
                    fn(itemIndex);
                }
            };

            if (chunkCount <= 1 || configuredWorkers <= 1)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    invokeAt(i, 0);
                }
                return;
            }

            const size_t helperCount = std::min(configuredWorkers, chunkCount) - 1;
            struct ParallelForState
            {
                std::atomic<size_t> nextIndex{ 0 };
                std::mutex mutex;
                std::condition_variable cv;
                size_t activeHelpers = 0;
                bool closed = false;
                std::exception_ptr firstException;
            };

            auto state = std::make_shared<ParallelForState>();
            auto* fnPtr = std::addressof(fn);

            auto runRangeLoop = [&](size_t workerSlot) {
                for (;;)
                {
                    const size_t start = state->nextIndex.fetch_add(grain, std::memory_order_relaxed);
                    if (start >= count)
                    {
                        break;
                    }

                    const size_t end = std::min(start + grain, count);
                    for (size_t i = start; i < end; ++i)
                    {
                        if constexpr (std::is_invocable_v<Func&, size_t, size_t>)
                        {
                            (*fnPtr)(i, workerSlot);
                        }
                        else
                        {
                            (void)workerSlot;
                            (*fnPtr)(i);
                        }
                    }
                }
            };

            std::vector<PendingJob> helpers;
            helpers.reserve(helperCount);
            for (size_t h = 0; h < helperCount; ++h)
            {
                const size_t helperSlot = h + 1;
                PendingJob job;
                job.name = "ParallelForHelper";
                job.work = [state, fnPtr, count, grain, helperSlot]() {
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        if (state->closed)
                        {
                            return;
                        }
                        ++state->activeHelpers;
                    }

                    std::exception_ptr captured;
                    try
                    {
                        for (;;)
                        {
                            const size_t start = state->nextIndex.fetch_add(grain, std::memory_order_relaxed);
                            if (start >= count)
                            {
                                break;
                            }

                            const size_t end = std::min(start + grain, count);
                            for (size_t i = start; i < end; ++i)
                            {
                                if constexpr (std::is_invocable_v<Func&, size_t, size_t>)
                                {
                                    (*fnPtr)(i, helperSlot);
                                }
                                else
                                {
                                    (void)helperSlot;
                                    (*fnPtr)(i);
                                }
                            }
                        }
                    }
                    catch (...)
                    {
                        captured = std::current_exception();
                        state->nextIndex.store(count, std::memory_order_relaxed);
                    }

                    bool notifyCaller = false;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        if (captured && !state->firstException)
                        {
                            state->firstException = captured;
                        }
                        --state->activeHelpers;
                        notifyCaller = state->closed && state->activeHelpers == 0;
                    }
                    if (notifyCaller)
                    {
                        state->cv.notify_all();
                    }
                };
                helpers.push_back(std::move(job));
            }

            EnqueueNormalBatch(std::move(helpers));

            std::exception_ptr callerException;
            try
            {
                runRangeLoop(0);
            }
            catch (...)
            {
                callerException = std::current_exception();
                state->nextIndex.store(count, std::memory_order_relaxed);
            }

            std::exception_ptr rethrowException;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->closed = true;
                state->cv.wait(lock, [&]() {
                    return state->activeHelpers == 0;
                });
                rethrowException = state->firstException ? state->firstException : callerException;
            }

            if (rethrowException)
            {
                std::rethrow_exception(rethrowException);
            }
        }

        template <typename Func>
        static void RunParallelFor(size_t count, size_t grainSize, Func&& fn)
        {
            if (count == 0)
            {
                return;
            }

            if (JobSystem* defaultPool = GetDefaultPool(); defaultPool != nullptr)
            {
                defaultPool->ParallelFor(count, grainSize, std::forward<Func>(fn));
                return;
            }

            JobSystem fallbackPool(ComputeWorkerCount(count, 0));
            fallbackPool.ParallelFor(count, grainSize, std::forward<Func>(fn));
        }

        template <typename Func>
        static void RunBoundedWorkers(size_t taskCount, Func&& work, size_t reserveThreads = 0)
        {
            if (taskCount == 0)
            {
                return;
            }

            if (JobSystem* defaultPool = GetDefaultPool(); defaultPool != nullptr)
            {
                defaultPool->ParallelFor(taskCount, 1, std::forward<Func>(work));
                return;
            }

            JobSystem fallbackPool(ComputeWorkerCount(taskCount, reserveThreads));
            fallbackPool.ParallelFor(taskCount, 1, std::forward<Func>(work));
        }

    private:
        struct PendingJob
        {
            std::string name;
            std::function<void()> work;
        };

        struct WorkerSlot
        {
            std::thread thread;
            std::atomic_bool exited{ false };
        };

        void EnsureWorkersStartedLocked();
        void SpawnWorkerLocked();
        void WorkerLoop(const std::shared_ptr<WorkerSlot>& slot);
        void EnqueueNormalBatch(std::vector<PendingJob> jobs);

        mutable std::mutex m_mutex;
        mutable std::condition_variable m_workAvailable;
        mutable std::condition_variable m_idle;
        std::vector<std::shared_ptr<WorkerSlot>> m_workers;
        std::deque<PendingJob> m_highPriorityJobs;
        std::deque<PendingJob> m_normalPriorityJobs;
        size_t m_activeJobs = 0;
        size_t m_workerCount = 1;
        size_t m_liveWorkers = 0;
        size_t m_workersToRetire = 0;
        bool m_workersStarted = false;
        bool m_stopping = false;
    };

    class ScopedDefaultJobSystem
    {
    public:
        explicit ScopedDefaultJobSystem(JobSystem& pool) noexcept
            : m_previous(JobSystem::GetDefaultPool())
        {
            JobSystem::SetDefaultPool(&pool);
        }

        ~ScopedDefaultJobSystem()
        {
            JobSystem::SetDefaultPool(m_previous);
        }

        ScopedDefaultJobSystem(const ScopedDefaultJobSystem&) = delete;
        ScopedDefaultJobSystem& operator=(const ScopedDefaultJobSystem&) = delete;
        ScopedDefaultJobSystem(ScopedDefaultJobSystem&&) = delete;
        ScopedDefaultJobSystem& operator=(ScopedDefaultJobSystem&&) = delete;

    private:
        JobSystem* m_previous = nullptr;
    };
}
