#pragma once

#include <atomic>
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
        JobSystem() = default;
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;
        JobSystem(JobSystem&&) = delete;
        JobSystem& operator=(JobSystem&&) = delete;

        void Start(std::string name, std::function<void()> work);
        void JoinFinished();
        void JoinAll();
        size_t RunningCount() const;

    private:
        struct Job
        {
            std::string name;
            std::thread thread;
            std::shared_ptr<std::atomic_bool> completed;
        };

        mutable std::mutex m_mutex;
        std::vector<Job> m_jobs;
    };
}
