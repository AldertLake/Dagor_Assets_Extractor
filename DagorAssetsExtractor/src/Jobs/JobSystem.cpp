#include "Jobs/JobSystem.h"

#include "Platform/ThreadComWic.h"

#include <algorithm>
#include <utility>

namespace DagorAssetsExtractor::Jobs
{
    namespace
    {
        constexpr size_t MaxConfiguredWorkers = 64;
        std::atomic<JobSystem*> g_defaultPool{ nullptr };
    }

    JobSystem::JobSystem(size_t workerCount)
        : m_workerCount(workerCount == 0
            ? ComputeDefaultWorkerCount()
            : std::clamp<size_t>(workerCount, 1, MaxWorkerCount()))
    {
    }

    JobSystem::~JobSystem()
    {
        JoinAll();

        std::vector<std::shared_ptr<WorkerSlot>> workers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            m_workersToRetire = 0;
            workers = std::move(m_workers);
        }

        m_workAvailable.notify_all();
        for (const std::shared_ptr<WorkerSlot>& slot : workers)
        {
            if (slot && slot->thread.joinable())
            {
                slot->thread.join();
            }
        }
    }

    size_t JobSystem::ComputeDefaultWorkerCount()
    {
        return MaxWorkerCount();
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

        workerCount = std::clamp<size_t>(workerCount, 1, MaxConfiguredWorkers);
        if (taskCount != 0)
        {
            workerCount = std::min(workerCount, taskCount);
        }

        return workerCount;
    }

    size_t JobSystem::MaxWorkerCount()
    {
        const size_t hardwareCount = static_cast<size_t>(std::thread::hardware_concurrency());
        return std::clamp<size_t>(hardwareCount == 0 ? 1 : hardwareCount, 1, MaxConfiguredWorkers);
    }

    JobSystem* JobSystem::GetDefaultPool() noexcept
    {
        return g_defaultPool.load(std::memory_order_acquire);
    }

    void JobSystem::SetDefaultPool(JobSystem* pool) noexcept
    {
        g_defaultPool.store(pool, std::memory_order_release);
    }

    void JobSystem::SetWorkerCount(size_t workerCount)
    {
        const size_t targetCount = workerCount == 0
            ? ComputeDefaultWorkerCount()
            : std::clamp<size_t>(workerCount, 1, MaxWorkerCount());

        std::lock_guard<std::mutex> lock(m_mutex);
        m_workerCount = targetCount;

        if (!m_workersStarted || m_stopping)
        {
            return;
        }

        if (targetCount > m_liveWorkers)
        {
            m_workersToRetire = 0;
            const size_t toSpawn = targetCount - m_liveWorkers;
            m_workers.reserve(m_workers.size() + toSpawn);
            for (size_t i = 0; i < toSpawn; ++i)
            {
                SpawnWorkerLocked();
            }
        }
        else if (targetCount < m_liveWorkers)
        {
            m_workersToRetire = m_liveWorkers - targetCount;
            m_workAvailable.notify_all();
        }
        else
        {
            m_workersToRetire = 0;
        }
    }

    size_t JobSystem::WorkerCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_workerCount;
    }

    void JobSystem::SpawnWorkerLocked()
    {
        auto slot = std::make_shared<WorkerSlot>();
        ++m_liveWorkers;
        slot->thread = std::thread([this, slot]() {
            WorkerLoop(slot);
        });
        m_workers.push_back(std::move(slot));
    }

    void JobSystem::EnsureWorkersStartedLocked()
    {
        if (m_workersStarted)
        {
            return;
        }

        m_workersStarted = true;
        m_workers.reserve(m_workerCount);
        for (size_t i = 0; i < m_workerCount; ++i)
        {
            SpawnWorkerLocked();
        }
    }

    void JobSystem::WorkerLoop(const std::shared_ptr<WorkerSlot>& slot)
    {
        Platform::ScopedWorkerComScope workerCom;

        for (;;)
        {
            PendingJob job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_workAvailable.wait(lock, [this]() {
                    return m_stopping
                        || m_workersToRetire > 0
                        || !m_highPriorityJobs.empty()
                        || !m_normalPriorityJobs.empty();
                });

                if (m_stopping && m_highPriorityJobs.empty() && m_normalPriorityJobs.empty())
                {
                    if (m_liveWorkers > 0)
                    {
                        --m_liveWorkers;
                    }
                    slot->exited.store(true, std::memory_order_release);
                    return;
                }

                if (m_workersToRetire > 0)
                {
                    --m_workersToRetire;
                    if (m_liveWorkers > 0)
                    {
                        --m_liveWorkers;
                    }
                    slot->exited.store(true, std::memory_order_release);
                    return;
                }

                if (!m_highPriorityJobs.empty())
                {
                    job = std::move(m_highPriorityJobs.front());
                    m_highPriorityJobs.pop_front();
                }
                else
                {
                    job = std::move(m_normalPriorityJobs.front());
                    m_normalPriorityJobs.pop_front();
                }

                ++m_activeJobs;
            }

            try
            {
                job.work();
            }
            catch (...)
            {
                // Job bodies should handle and report their own errors. This
                // guard keeps background exceptions from terminating the app.
            }

            bool becameIdle = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_activeJobs;
                becameIdle = m_highPriorityJobs.empty()
                    && m_normalPriorityJobs.empty()
                    && m_activeJobs == 0;
            }

            if (becameIdle)
            {
                m_idle.notify_all();
            }
        }
    }

    void JobSystem::Start(std::string name, std::function<void()> work, JobPriority priority)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        EnsureWorkersStartedLocked();

        PendingJob job;
        job.name = std::move(name);
        job.work = std::move(work);

        if (priority == JobPriority::High)
        {
            m_highPriorityJobs.push_back(std::move(job));
        }
        else
        {
            m_normalPriorityJobs.push_back(std::move(job));
        }

        m_workAvailable.notify_one();
    }

    void JobSystem::EnqueueNormalBatch(std::vector<PendingJob> jobs)
    {
        if (jobs.empty())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        EnsureWorkersStartedLocked();

        for (PendingJob& job : jobs)
        {
            m_normalPriorityJobs.push_back(std::move(job));
        }

        m_workAvailable.notify_all();
    }

    void JobSystem::JoinFinished()
    {
        std::vector<std::shared_ptr<WorkerSlot>> retired;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_workers.begin(); it != m_workers.end();)
            {
                if (*it && (*it)->exited.load(std::memory_order_acquire))
                {
                    retired.push_back(std::move(*it));
                    it = m_workers.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (const std::shared_ptr<WorkerSlot>& slot : retired)
        {
            if (slot && slot->thread.joinable())
            {
                slot->thread.join();
            }
        }
    }

    void JobSystem::JoinAll()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idle.wait(lock, [this]() {
            return m_highPriorityJobs.empty()
                && m_normalPriorityJobs.empty()
                && m_activeJobs == 0;
        });
    }

    size_t JobSystem::RunningCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_highPriorityJobs.size() + m_normalPriorityJobs.size() + m_activeJobs;
    }
}
