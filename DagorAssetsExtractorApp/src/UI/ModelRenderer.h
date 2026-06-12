#pragma once

#include "App/AppState.h"
#include <vector>
#include <cstdint>

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
            float panX,
            float panY,
            bool wireframe,
            const float bboxMin[3],
            const float bboxMax[3],
            uint32_t width,
            uint32_t height,
            const App::ModelPreviewLighting& lighting);

    private:
        bool CreateRenderTarget(uint32_t width, uint32_t height);
        void CleanupRenderTarget();

        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;

        // Model Buffers
        ID3D11Buffer* m_vertexBuffer = nullptr;
        ID3D11Buffer* m_indexBuffer = nullptr;
        uint32_t m_indexCount = 0;

        // Shaders & Pipeline state
        ID3D11VertexShader* m_vertexShader = nullptr;
        ID3D11PixelShader* m_pixelShader = nullptr;
        ID3D11InputLayout* m_inputLayout = nullptr;
        ID3D11Buffer* m_constantBuffer = nullptr;
        ID3D11RasterizerState* m_rasterizerStateSolid = nullptr;
        ID3D11RasterizerState* m_rasterizerStateWireframe = nullptr;
        ID3D11DepthStencilState* m_depthStencilState = nullptr;

        // Render target resources
        ID3D11Texture2D* m_renderTargetTexture = nullptr;
        ID3D11RenderTargetView* m_renderTargetView = nullptr;
        ID3D11ShaderResourceView* m_shaderResourceView = nullptr;
        ID3D11Texture2D* m_depthStencilTexture = nullptr;
        ID3D11DepthStencilView* m_depthStencilView = nullptr;

        uint32_t m_rtWidth = 0;
        uint32_t m_rtHeight = 0;
    };
}
