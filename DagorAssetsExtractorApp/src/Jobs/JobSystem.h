#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace DagorAssetsExtractor::Jobs
{
    class JobSystem
    {
    public:
        explicit JobSystem(size_t workerCount = 0);
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;
        JobSystem(JobSystem&&) = delete;
        JobSystem& operator=(JobSystem&&) = delete;

        static size_t ComputeWorkerCount(size_t taskCount = 0, size_t reserveThreads = 0);

        void Start(std::string name, std::function<void()> work);
        void JoinFinished();
        void JoinAll();
        size_t RunningCount() const;

        template <typename Func>
        static void RunBoundedWorkers(size_t taskCount, Func&& work, size_t reserveThreads = 0)
        {
            if (taskCount == 0)
            {
                return;
            }

            const size_t workerCount = ComputeWorkerCount(taskCount, reserveThreads);
            JobSystem jobs(workerCount);
            std::atomic_size_t nextIndex{ 0 };

            for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
            {
                jobs.Start("Worker_" + std::to_string(workerIndex), [&work, &nextIndex, taskCount, workerIndex]() {
                    for (;;)
                    {
                        const size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                        if (index >= taskCount)
                        {
                            break;
                        }

                        work(index, workerIndex);
                    }
                });
            }

            jobs.JoinAll();
        }

    private:
        struct Job
        {
            std::string name;
            std::function<void()> work;
            bool running = false;
            bool completed = false;
        };

        void EnsureWorkersStarted();
        void WorkerLoop();

        mutable std::mutex m_mutex;
        mutable std::condition_variable m_workAvailable;
        mutable std::condition_variable m_idle;
        std::vector<std::thread> m_workers;
        std::vector<std::shared_ptr<Job>> m_jobs;
        std::deque<std::shared_ptr<Job>> m_pendingJobs;
        size_t m_activeJobs = 0;
        size_t m_workerCount = 0;
        bool m_stopping = false;
    };
}
