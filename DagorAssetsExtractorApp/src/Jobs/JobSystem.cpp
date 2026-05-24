#include "Jobs/JobSystem.h"

#include <algorithm>
#include <utility>

namespace DagorAssetsExtractor::Jobs
{
    JobSystem::JobSystem(size_t workerCount)
        : m_workerCount(workerCount == 0 ? ComputeWorkerCount() : std::max<size_t>(1, workerCount))
    {
    }

    JobSystem::~JobSystem()
    {
        JoinAll();

        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            workers = std::move(m_workers);
        }

        m_workAvailable.notify_all();
        for (std::thread& worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
    }

    size_t JobSystem::ComputeWorkerCount(size_t taskCount, size_t reserveThreads)
    {
        size_t workerCount = static_cast<size_t>(std::thread::hardware_concurrency());
        if (workerCount == 0)
        {
            workerCount = 1;
        }

        if (reserveThreads >= workerCount)
        {
            workerCount = 1;
        }
        else
        {
            workerCount -= reserveThreads;
        }

        workerCount = std::max<size_t>(1, workerCount);
        if (taskCount != 0)
        {
            workerCount = std::min(workerCount, taskCount);
        }

        return workerCount;
    }

    void JobSystem::EnsureWorkersStarted()
    {
        if (!m_workers.empty())
        {
            return;
        }

        m_workers.reserve(m_workerCount);
        for (size_t i = 0; i < m_workerCount; ++i)
        {
            m_workers.emplace_back([this]() {
                WorkerLoop();
            });
        }
    }

    void JobSystem::WorkerLoop()
    {
        for (;;)
        {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_workAvailable.wait(lock, [this]() {
                    return m_stopping || !m_pendingJobs.empty();
                });

                if (m_stopping && m_pendingJobs.empty())
                {
                    return;
                }

                job = std::move(m_pendingJobs.front());
                m_pendingJobs.pop_front();
                job->running = true;
                ++m_activeJobs;
            }

            try
            {
                job->work();
            }
            catch (...)
            {
                // Job bodies should handle and report their own errors. This
                // guard keeps background exceptions from terminating the app.
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                job->running = false;
                job->completed = true;
                --m_activeJobs;
            }

            m_idle.notify_all();
        }
    }

    void JobSystem::Start(std::string name, std::function<void()> work)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        EnsureWorkersStarted();

        auto job = std::make_shared<Job>();
        job->name = std::move(name);
        job->work = std::move(work);
        m_pendingJobs.push_back(job);
        m_jobs.push_back(std::move(job));
        m_workAvailable.notify_one();
    }

    void JobSystem::JoinFinished()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.erase(
            std::remove_if(m_jobs.begin(), m_jobs.end(), [](const std::shared_ptr<Job>& job) {
                return job && job->completed;
            }),
            m_jobs.end());
    }

    void JobSystem::JoinAll()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idle.wait(lock, [this]() {
            return m_pendingJobs.empty() && m_activeJobs == 0;
        });
        m_jobs.clear();
    }

    size_t JobSystem::RunningCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pendingJobs.size() + m_activeJobs;
    }
}
