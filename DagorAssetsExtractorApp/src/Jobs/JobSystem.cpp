#include "Jobs/JobSystem.h"

#include <utility>

namespace DagorAssetsExtractor::Jobs
{
    JobSystem::~JobSystem()
    {
        JoinAll();
    }

    void JobSystem::Start(std::string name, std::function<void()> work)
    {
        auto completed = std::make_shared<std::atomic_bool>(false);
        std::thread worker([completed, work = std::move(work)]() mutable {
            try
            {
                work();
            }
            catch (...)
            {
                // Job bodies should handle and report their own errors. This
                // guard keeps background exceptions from terminating the app.
            }

            completed->store(true, std::memory_order_release);
        });

        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.push_back(Job{ std::move(name), std::move(worker), std::move(completed) });
    }

    void JobSystem::JoinFinished()
    {
        std::vector<std::thread> finished;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_jobs.begin();
            while (it != m_jobs.end())
            {
                if (it->completed && it->completed->load(std::memory_order_acquire))
                {
                    finished.push_back(std::move(it->thread));
                    it = m_jobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (std::thread& thread : finished)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
    }

    void JobSystem::JoinAll()
    {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (Job& job : m_jobs)
            {
                threads.push_back(std::move(job.thread));
            }
            m_jobs.clear();
        }

        for (std::thread& thread : threads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
    }

    size_t JobSystem::RunningCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        size_t count = 0;
        for (const Job& job : m_jobs)
        {
            if (!job.completed || !job.completed->load(std::memory_order_acquire))
            {
                ++count;
            }
        }

        return count;
    }
}
