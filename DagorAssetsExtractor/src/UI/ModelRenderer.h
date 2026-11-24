#pragma once

#include "App/AppState.h"
#include <vector>
#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11Buffer;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;
struct ID3D11RasterizerState;
struct ID3D11DepthStencilState;
struct ID3D11Query;
struct IDXGIAdapter3;

namespace DagorAssetsExtractor::UI
{
    class ModelRenderer;

    bool InitializeModelRenderer(ID3D11Device* device, ID3D11DeviceContext* context);
    void ShutdownModelRenderer();
    ModelRenderer* GetModelRenderer();

    class ModelRenderer
    {
    public:
        ModelRenderer() = default;
        ~ModelRenderer();

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        bool SetModelData(const std::vector<App::ModelPreviewVertex>& vertices, const std::vector<uint32_t>& indices);
        void ClearModelData();

        ID3D11ShaderResourceView* Render(
            float yaw,
            float pitch,
            float zoom,
            float panWorldX,
            float panWorldY,
            float panWorldZ,
            int renderMode,
            const float bboxMin[3],
            const float bboxMax[3],
            uint32_t width,
            uint32_t height,
            const App::ModelPreviewLighting& lighting,
            int hoveredSubmeshIndex = -1,
            const std::vector<App::ModelPreviewSubmesh>& submeshes = {});

        std::string GetGPUName() const;
        void GetVRAMInfo(uint64_t& usageBytes, uint64_t& budgetBytes) const;
        float GetLastGpuTimeMs() const { return m_lastGpuTimeMs; }
        bool HasHardwareGpuTiming() const { return m_gpuTimingValid; }
        uint32_t GetLastDrawCallCount() const { return m_lastDrawCalls; }
        uint32_t GetMsaaSampleCount() const { return m_msaaSampleCount; }

    private:
        bool CreateRenderTarget(uint32_t width, uint32_t height);
        void CleanupRenderTarget();
        void PollCompletedGpuQueries();

        static constexpr uint32_t kGpuQueryRingSize = 3;

        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        // Model Buffers
        ID3D11Buffer* m_vertexBuffer = nullptr;
        ID3D11Buffer* m_indexBuffer = nullptr;
        uint32_t m_vertexCount = 0;
        uint32_t m_indexCount = 0;

        // Ground Grid & Origin Axis Buffers
        ID3D11Buffer* m_groundVertexBuffer = nullptr;
        uint32_t m_groundVertexCount = 0;
        ID3D11Buffer* m_axisLineVertexBuffer = nullptr;

        // Shaders & Pipeline state
        ID3D11VertexShader* m_vertexShader = nullptr;
        ID3D11PixelShader* m_pixelShader = nullptr;
        ID3D11InputLayout* m_inputLayout = nullptr;
        ID3D11Buffer* m_constantBuffer = nullptr;
        ID3D11RasterizerState* m_rasterizerStateSolid = nullptr;
        ID3D11RasterizerState* m_rasterizerStateWireframe = nullptr;
        ID3D11DepthStencilState* m_depthStencilState = nullptr;
        ID3D11DepthStencilState* m_depthStencilStateNoDepth = nullptr;

        // Render target resources (4x MSAA + single-sample SRV resolve target when supported)
        ID3D11Texture2D* m_msaaRenderTargetTexture = nullptr;
        ID3D11Texture2D* m_renderTargetTexture = nullptr;
        ID3D11RenderTargetView* m_renderTargetView = nullptr;
        ID3D11ShaderResourceView* m_shaderResourceView = nullptr;
        ID3D11Texture2D* m_depthStencilTexture = nullptr;
        ID3D11DepthStencilView* m_depthStencilView = nullptr;
        uint32_t m_msaaSampleCount = 1;

        uint32_t m_rtWidth = 0;
        uint32_t m_rtHeight = 0;

        // Cached DXGI adapter & GPU telemetry
        IDXGIAdapter3* m_dxgiAdapter3 = nullptr;
        std::string m_gpuName;
        uint64_t m_dedicatedVideoMemory = 0;
        ID3D11Query* m_queryDisjoint[kGpuQueryRingSize] = {};
        ID3D11Query* m_queryStart[kGpuQueryRingSize] = {};
        ID3D11Query* m_queryEnd[kGpuQueryRingSize] = {};
        bool m_queryPending[kGpuQueryRingSize] = {};
        uint32_t m_queryWriteIndex = 0;
        float m_lastGpuTimeMs = 0.0f;
        bool m_gpuTimingValid = false;
        uint32_t m_lastDrawCalls = 0;
    };
}
