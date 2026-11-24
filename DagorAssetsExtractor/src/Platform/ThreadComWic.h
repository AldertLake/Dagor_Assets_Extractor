#pragma once

struct IWICImagingFactory;

namespace DagorAssetsExtractor::Platform
{
    class ScopedWorkerComScope
    {
    public:
        ScopedWorkerComScope();
        ~ScopedWorkerComScope();

        ScopedWorkerComScope(const ScopedWorkerComScope&) = delete;
        ScopedWorkerComScope& operator=(const ScopedWorkerComScope&) = delete;
        ScopedWorkerComScope(ScopedWorkerComScope&&) = delete;
        ScopedWorkerComScope& operator=(ScopedWorkerComScope&&) = delete;

        [[nodiscard]] bool IsReady() const noexcept;
        [[nodiscard]] long ResultCode() const noexcept;

    private:
        bool m_isRootScope = false;
    };

    class WicFactoryLease
    {
    public:
        WicFactoryLease();
        ~WicFactoryLease();

        WicFactoryLease(const WicFactoryLease&) = delete;
        WicFactoryLease& operator=(const WicFactoryLease&) = delete;
        WicFactoryLease(WicFactoryLease&& other) noexcept;
        WicFactoryLease& operator=(WicFactoryLease&& other) noexcept;

        [[nodiscard]] IWICImagingFactory* Get() const noexcept { return m_factory; }
        [[nodiscard]] bool IsReady() const noexcept { return m_factory != nullptr; }
        [[nodiscard]] long ResultCode() const noexcept { return m_resultCode; }
        [[nodiscard]] bool IsCachedWorkerFactory() const noexcept { return m_cachedWorkerFactory; }

    private:
        void Release() noexcept;

        IWICImagingFactory* m_factory = nullptr;
        long m_resultCode = 0;
        bool m_owned = false;
        bool m_uninitCom = false;
        bool m_cachedWorkerFactory = false;
    };
}
