#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <iterator>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace
{
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
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

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
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"DagorAssetsExtractorWindowClass";

    if (RegisterClassExW(&windowClass) == 0)
    {
        return 1;
    }

    HWND hwnd = CreateWindowW(
        windowClass.lpszClassName,
        L"Dagor Assets Extractor",
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        1280,
        800,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (hwnd == nullptr)
    {
        UnregisterClassW(windowClass.lpszClassName, instance);
        return 1;
    }

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, instance);
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

    bool showDemoWindow = true;
    bool running = true;
    float clearColor[4] = { 0.12f, 0.14f, 0.18f, 1.0f };

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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Dagor Assets Extractor");
        ImGui::TextUnformatted("ImGui test window is running.");
        ImGui::Separator();
        ImGui::Checkbox("Show ImGui Demo Window", &showDemoWindow);
        ImGui::ColorEdit3("Clear Color", clearColor);
        ImGui::Text("Window size: %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Text("Frame time: %.3f ms", 1000.0f / io.Framerate);
        ImGui::Text("FPS: %.1f", io.Framerate);
        ImGui::End();

        if (showDemoWindow)
        {
            ImGui::ShowDemoWindow(&showDemoWindow);
        }

        ImGui::Render();
        GDeviceContext->OMSetRenderTargets(1, &GMainRenderTargetView, nullptr);
        GDeviceContext->ClearRenderTargetView(GMainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        GSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(windowClass.lpszClassName, instance);

    return 0;
}
