#include <windows.h>
#include <d3d11.h>

#include "App/AppState.h"
#include "Platform/Platform.h"
#include "UI/UI.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    using DagorAssetsExtractor::App::AppState;
    using DagorAssetsExtractor::App::LogLevel;

    ID3D11Device* GDevice = nullptr;
    ID3D11DeviceContext* GDeviceContext = nullptr;
    IDXGISwapChain* GSwapChain = nullptr;
    ID3D11RenderTargetView* GMainRenderTargetView = nullptr;

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(GSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        {
            GDevice->CreateRenderTargetView(backBuffer, nullptr, &GMainRenderTargetView);
            backBuffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (GMainRenderTargetView != nullptr)
        {
            GMainRenderTargetView->Release();
            GMainRenderTargetView = nullptr;
        }
    }

    bool CreateDeviceD3D(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.BufferDesc.Width = 0;
        swapChainDesc.BufferDesc.Height = 0;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.OutputWindow = hwnd;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        UINT createDeviceFlags = 0;
#if defined(_DEBUG)
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        constexpr D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0
        };

        D3D_FEATURE_LEVEL featureLevel;
        const HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &swapChainDesc,
            &GSwapChain,
            &GDevice,
            &featureLevel,
            &GDeviceContext);

        if (FAILED(result))
        {
            return false;
        }

        CreateRenderTarget();
        return true;
    }

    void CleanupDeviceD3D()
    {
        CleanupRenderTarget();

        if (GSwapChain != nullptr)
        {
            GSwapChain->Release();
            GSwapChain = nullptr;
        }

        if (GDeviceContext != nullptr)
        {
            GDeviceContext->Release();
            GDeviceContext = nullptr;
        }

        if (GDevice != nullptr)
        {
            GDevice->Release();
            GDevice = nullptr;
        }
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return TRUE;
    }

    switch (msg)
    {
    case WM_SIZE:
        if (GDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            GSwapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0U) == SC_KEYMENU)
        {
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comInitialized = SUCCEEDED(comResult);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"DagorAssetsExtractorWindowClass";

    if (RegisterClassExW(&windowClass) == 0)
    {
        if (comInitialized)
        {
            CoUninitialize();
        }
        return 1;
    }

    HWND hwnd = CreateWindowW(
        windowClass.lpszClassName,
        L"Dagor Assets Extractor",
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        1420,
        880,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (hwnd == nullptr)
    {
        UnregisterClassW(windowClass.lpszClassName, instance);
        if (comInitialized)
        {
            CoUninitialize();
        }
        return 1;
    }

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, instance);
        if (comInitialized)
        {
            CoUninitialize();
        }
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(GDevice, GDeviceContext);

    AppState app;
    app.hwnd = hwnd;
    app.settingsPath = DagorAssetsExtractor::Platform::GetSettingsPath();
    app.indexCachePath = DagorAssetsExtractor::Platform::GetIndexCachePath();
    app.descriptorLinksCachePath = DagorAssetsExtractor::Platform::GetDescriptorLinksCachePath();
    
    // We access the settings load from App module
    DagorAssetsExtractor::App::LoadSettings(app.settings, app.settingsPath);
    app.validation = DagorAssetsExtractor::AssetIndex::ValidateGameRoot(app.settings.gameRoot);
    DagorAssetsExtractor::App::SyncSettingsBuffers(app);
    app.firstRunPromptOpen = !app.validation.IsValid();

    DagorAssetsExtractor::App::AddLog(app, LogLevel::Info, "Application started.");
    DagorAssetsExtractor::App::AddLog(app, LogLevel::Info, "Settings path: " + DagorAssetsExtractor::Platform::PathToUtf8(app.settingsPath));
    DagorAssetsExtractor::App::AddLog(app, LogLevel::Info, "Index cache path: " + DagorAssetsExtractor::Platform::PathToUtf8(app.indexCachePath));
    DagorAssetsExtractor::App::AddLog(app, LogLevel::Info, "Descriptor-link cache path: " + DagorAssetsExtractor::Platform::PathToUtf8(app.descriptorLinksCachePath));
    
    if (app.validation.IsValid())
    {
        DagorAssetsExtractor::App::AddLog(app, LogLevel::Info, "Loaded saved game folder: " + DagorAssetsExtractor::Platform::PathToUtf8(app.settings.gameRoot));
        DagorAssetsExtractor::App::LoadIndexCacheIntoApp(app);
    }
    else
    {
        DagorAssetsExtractor::App::AddLog(app, LogLevel::Warning, "No valid War Thunder folder is configured.");
        DagorAssetsExtractor::App::SetCacheStatus(app.scan, "No valid game folder", false);
    }

    bool running = true;
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };

    while (running)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);

            if (msg.message == WM_QUIT)
            {
                running = false;
            }
        }

        if (!running)
        {
            break;
        }

        DagorAssetsExtractor::App::JoinFinishedBackgroundJobs(app);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DagorAssetsExtractor::UI::DrawMainShell(app, running);

        ImGui::Render();
        GDeviceContext->OMSetRenderTargets(1, &GMainRenderTargetView, nullptr);
        GDeviceContext->ClearRenderTargetView(GMainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        GSwapChain->Present(1, 0);
    }

    DagorAssetsExtractor::App::StopScan(app);
    DagorAssetsExtractor::App::WaitForBackgroundJobs(app);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(windowClass.lpszClassName, instance);

    if (comInitialized)
    {
        CoUninitialize();
    }

    return 0;
}
