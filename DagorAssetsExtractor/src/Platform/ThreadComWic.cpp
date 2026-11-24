#include "Platform/ThreadComWic.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <utility>

namespace DagorAssetsExtractor::Platform
{
    namespace
    {
        thread_local int tlsWorkerComDepth = 0;
        thread_local bool tlsWorkerComScopeActive = false;
        thread_local bool tlsWorkerComReady = false;
        thread_local bool tlsWorkerComDidInit = false;
        thread_local HRESULT tlsWorkerComHr = E_FAIL;
        thread_local IWICImagingFactory* tlsCachedWicFactory = nullptr;
    }

    ScopedWorkerComScope::ScopedWorkerComScope()
    {
        if (tlsWorkerComDepth == 0)
        {
            m_isRootScope = true;
            const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            tlsWorkerComHr = hr;
            tlsWorkerComDidInit = SUCCEEDED(hr);
            tlsWorkerComReady = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
            tlsWorkerComScopeActive = true;
        }

        ++tlsWorkerComDepth;
    }

    ScopedWorkerComScope::~ScopedWorkerComScope()
    {
        if (tlsWorkerComDepth > 0)
        {
            --tlsWorkerComDepth;
        }

        if (m_isRootScope && tlsWorkerComDepth == 0)
        {
            // Release any thread-local COM objects strictly before CoUninitialize().
            if (tlsCachedWicFactory != nullptr)
            {
                tlsCachedWicFactory->Release();
                tlsCachedWicFactory = nullptr;
            }

            if (tlsWorkerComDidInit)
            {
                ::CoUninitialize();
            }

            tlsWorkerComDidInit = false;
            tlsWorkerComScopeActive = false;
            tlsWorkerComReady = false;
            tlsWorkerComHr = E_FAIL;
        }
    }

    bool ScopedWorkerComScope::IsReady() const noexcept
    {
        return tlsWorkerComReady;
    }

    long ScopedWorkerComScope::ResultCode() const noexcept
    {
        return static_cast<long>(tlsWorkerComHr);
    }

    WicFactoryLease::WicFactoryLease()
    {
        if (tlsWorkerComScopeActive)
        {
            if (!tlsWorkerComReady)
            {
                m_resultCode = static_cast<long>(tlsWorkerComHr);
                return;
            }

            if (tlsCachedWicFactory == nullptr)
            {
                IWICImagingFactory* factory = nullptr;
                const HRESULT hr = ::CoCreateInstance(
                    CLSID_WICImagingFactory,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&factory));
                m_resultCode = static_cast<long>(hr);
                if (FAILED(hr))
                {
                    if (factory != nullptr)
                    {
                        factory->Release();
                    }
                    return;
                }
                tlsCachedWicFactory = factory;
            }
            else
            {
                m_resultCode = static_cast<long>(S_OK);
            }

            m_factory = tlsCachedWicFactory;
            m_owned = false;
            m_uninitCom = false;
            m_cachedWorkerFactory = true;
            return;
        }

        const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE)
        {
            m_resultCode = static_cast<long>(comHr);
            return;
        }

        const bool didInitCom = SUCCEEDED(comHr);
        IWICImagingFactory* factory = nullptr;
        const HRESULT hr = ::CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        m_resultCode = static_cast<long>(hr);
        if (FAILED(hr))
        {
            if (factory != nullptr)
            {
                factory->Release();
            }
            if (didInitCom)
            {
                ::CoUninitialize();
            }
            return;
        }

        m_factory = factory;
        m_owned = true;
        m_uninitCom = didInitCom;
        m_cachedWorkerFactory = false;
    }

    WicFactoryLease::~WicFactoryLease()
    {
        Release();
    }

    WicFactoryLease::WicFactoryLease(WicFactoryLease&& other) noexcept
        : m_factory(std::exchange(other.m_factory, nullptr))
        , m_resultCode(std::exchange(other.m_resultCode, 0))
        , m_owned(std::exchange(other.m_owned, false))
        , m_uninitCom(std::exchange(other.m_uninitCom, false))
        , m_cachedWorkerFactory(std::exchange(other.m_cachedWorkerFactory, false))
    {
    }

    WicFactoryLease& WicFactoryLease::operator=(WicFactoryLease&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_factory = std::exchange(other.m_factory, nullptr);
            m_resultCode = std::exchange(other.m_resultCode, 0);
            m_owned = std::exchange(other.m_owned, false);
            m_uninitCom = std::exchange(other.m_uninitCom, false);
            m_cachedWorkerFactory = std::exchange(other.m_cachedWorkerFactory, false);
        }
        return *this;
    }

    void WicFactoryLease::Release() noexcept
    {
        if (m_owned && m_factory != nullptr)
        {
            m_factory->Release();
        }
        m_factory = nullptr;
        m_owned = false;
        m_cachedWorkerFactory = false;

        if (m_uninitCom)
        {
            ::CoUninitialize();
            m_uninitCom = false;
        }
    }
}
