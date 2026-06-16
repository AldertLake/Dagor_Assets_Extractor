#include <windows.h>
#include <d3d11.h>

#include "App/AppState.h"
#include "Platform/Platform.h"
#include "UI/UI.h"
#include "UI/ModelRenderer.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

namespace
{
    using DagorAssetsExtractor::App::AppState;
    using DagorAssetsExtractor::App::LogLevel;

    ID3D11Device* GDevice = nullptr;
    ID3D11DeviceContext* GDeviceContext = nullptr;
    IDXGISwapChain* GSwapChain = nullptr;
    ID3D11RenderTargetView* GMainRenderTargetView = nullptr;

    HICON LoadWindowIcon(const std::filesystem::path& path, int width, int height)
    {
        const std::wstring filePath = path.wstring();
        HICON icon = reinterpret_cast<HICON>(LoadImageW(
            nullptr,
            filePath.c_str(),
            IMAGE_ICON,
            width,
            height,
            LR_LOADFROMFILE));

        if (icon == nullptr)
        {
            icon = LoadIconW(nullptr, IDI_APPLICATION);
        }

        return icon;
    }

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
    const std::filesystem::path uiAssetPath = DagorAssetsExtractor::Platform::GetUiAssetPath();
    windowClass.hIcon = LoadWindowIcon(uiAssetPath / L"DAE.ico", GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    windowClass.hIconSm = LoadWindowIcon(uiAssetPath / L"DAE.ico", GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

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

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(windowClass.hIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(windowClass.hIconSm));

    COLORREF darkColor = RGB(36, 36, 36);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &darkColor, sizeof(darkColor));

    COLORREF whiteColor = RGB(255, 255, 255);
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &whiteColor, sizeof(whiteColor));

    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

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

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    
    std::string fontPath = DagorAssetsExtractor::Platform::PathToUtf8(DagorAssetsExtractor::Platform::GetUiAssetPath() / L"Roboto-Medium.ttf");
    io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f);

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(GDevice, GDeviceContext);
    DagorAssetsExtractor::UI::InitializeUiAssets(GDevice);
    DagorAssetsExtractor::UI::InitializeModelRenderer(GDevice, GDeviceContext);

    AppState app;
    app.hwnd = hwnd;
    app.settingsPath = DagorAssetsExtractor::Platform::GetSettingsPath();
    app.indexCachePath = DagorAssetsExtractor::Platform::GetIndexCachePath();
    app.descriptorLinksCachePath = DagorAssetsExtractor::Platform::GetDescriptorLinksCachePath();
    
    // We access the settings load from App module
    DagorAssetsExtractor::App::LoadSettings(app.settings, app.settingsPath);
    if (const std::string imguiIniData = DagorAssetsExtractor::App::LoadImGuiSettings(app.settingsPath); !imguiIniData.empty())
    {
        ImGui::LoadIniSettingsFromMemory(imguiIniData.c_str(), imguiIniData.size());
    }
    DagorAssetsExtractor::App::ApplySettingsToUiState(app);
    app.validation = DagorAssetsExtractor::AssetIndex::ValidateGameRoot(app.settings.gameRoot);
    DagorAssetsExtractor::App::SyncSettingsBuffers(app);
    app.firstRunPromptOpen = !app.validation.IsValid();

    app.logger.Initialize(DagorAssetsExtractor::Platform::GetExecutableDirectory());

    DagorAssetsExtractor::App::AddLog(app, LogLevel::Info, "Application started.");
    DagorAssetsExtractor::App::AddLog(app, LogLevel::Info, "Portable cache folder: " + DagorAssetsExtractor::Platform::PathToUtf8(DagorAssetsExtractor::Platform::GetCacheRootPath()));
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

        if (io.WantSaveIniSettings)
        {
            size_t iniSize = 0;
            const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
            std::string error;
            if (DagorAssetsExtractor::App::SaveSharedSettings(app.settings, app.settingsPath, std::string_view(iniData, iniSize), error))
            {
                io.WantSaveIniSettings = false;
            }
            else
            {
                DagorAssetsExtractor::App::AddLog(app, LogLevel::Error, error);
            }
        }

        ImGui::Render();
        GDeviceContext->OMSetRenderTargets(1, &GMainRenderTargetView, nullptr);
        GDeviceContext->ClearRenderTargetView(GMainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        GSwapChain->Present(1, 0);
    }

    DagorAssetsExtractor::App::StopScan(app);
    DagorAssetsExtractor::App::WaitForBackgroundJobs(app);

    {
        size_t iniSize = 0;
        const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
        std::string error;
        if (!DagorAssetsExtractor::App::SaveSharedSettings(app.settings, app.settingsPath, std::string_view(iniData, iniSize), error))
        {
            DagorAssetsExtractor::App::AddLog(app, LogLevel::Error, error);
        }
        else
        {
            io.WantSaveIniSettings = false;
        }
    }

    DagorAssetsExtractor::UI::ShutdownModelRenderer();
    DagorAssetsExtractor::UI::ShutdownUiAssets();
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
