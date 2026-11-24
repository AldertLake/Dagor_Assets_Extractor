#include "UI/ModelRenderer.h"
#include "Platform/Platform.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace DagorAssetsExtractor::UI
{
    namespace
    {
        using namespace DirectX;

        const char* g_vertexShaderSource = R"(
            cbuffer ConstantBuffer : register(b0)
            {
                row_major matrix mvp;
                row_major matrix world;
                float4 cameraPos;
                float4 keyLightDir;
                float4 keyLightColor;
                float4 fillLightColor;
                float4 ambientColor;
                float4 baseColor;
                float4 specParams;
            };

            struct VS_INPUT
            {
                float3 pos : POSITION;
                float3 normal : NORMAL;
            };

            struct PS_INPUT
            {
                float4 pos : SV_POSITION;
                float3 normal : NORMAL;
                float3 worldPos : TEXCOORD0;
            };

            PS_INPUT VS(VS_INPUT input)
            {
                PS_INPUT output;
                float4 wp = mul(float4(input.pos, 1.0f), world);
                output.pos = mul(float4(input.pos, 1.0f), mvp);
                output.normal = mul(input.normal, (float3x3)world);
                output.worldPos = wp.xyz;
                return output;
            }
        )";

        const char* g_pixelShaderSource = R"(
            cbuffer ConstantBuffer : register(b0)
            {
                row_major matrix mvp;
                row_major matrix world;
                float4 cameraPos;
                float4 keyLightDir;
                float4 keyLightColor;
                float4 fillLightColor;
                float4 ambientColor;
                float4 baseColor;
                float4 specParams;
            };

            struct PS_INPUT
            {
                float4 pos : SV_POSITION;
                float3 normal : NORMAL;
                float3 worldPos : TEXCOORD0;
            };

            float4 PS(PS_INPUT input) : SV_Target
            {
                if (specParams.z > 0.5f)
                {
                    return float4(saturate(baseColor.rgb), 1.0f);
                }

                float nLen = length(input.normal);
                float3 N = nLen > 1e-5f ? (input.normal / nLen) : float3(0.0f, 1.0f, 0.0f);

                // Key Light direction
                float3 L1 = normalize(keyLightDir.xyz);
                float3 C1 = keyLightColor.rgb;
                float diff1 = max(0.0f, dot(N, L1));

                // Fill Light direction (opposite key light horizontally)
                float3 L2 = normalize(float3(-L1.x, 0.8f, -L1.z));
                float3 C2 = fillLightColor.rgb;
                float diff2 = max(0.0f, dot(N, L2));

                // World-space view vector from shaded surface point toward camera
                float3 toCam = cameraPos.xyz - input.worldPos;
                float vLen = length(toCam);
                float3 V = vLen > 1e-5f ? (toCam / vLen) : float3(0.0f, 0.0f, 1.0f);

                float3 H1 = normalize(L1 + V);
                float spec1 = pow(max(0.0f, dot(N, H1)), specParams.y) * specParams.x;

                // Soft hemisphere ambient & subtle Fresnel rim for silhouette clarity
                float3 hemiAmbient = lerp(ambientColor.rgb * 0.72f, ambientColor.rgb * 1.18f, N.y * 0.5f + 0.5f);
                float rim = pow(1.0f - max(0.0f, dot(N, V)), 3.0f) * specParams.w;

                float3 diffuseColor = diff1 * C1 * 0.72f + diff2 * C2 * 0.28f;
                float3 finalColor = baseColor.rgb * (diffuseColor + hemiAmbient) + float3(spec1, spec1, spec1) * C1 + rim * C2;

                return float4(saturate(finalColor), 1.0f);
            }
        )";

        struct CBData
        {
            XMFLOAT4X4 mvp;
            XMFLOAT4X4 world;
            XMFLOAT4 cameraPos;
            XMFLOAT4 keyLightDir;
            XMFLOAT4 keyLightColor;
            XMFLOAT4 fillLightColor;
            XMFLOAT4 ambientColor;
            XMFLOAT4 baseColor;
            XMFLOAT4 specParams; // x: specIntensity, y: specPower, z: unlitFlag (>0.5 unlit), w: rimIntensity
        };
        static_assert(sizeof(CBData) % 16 == 0, "CBData must be 16-byte aligned");

        HRESULT CompileShader(const char* source, LPCSTR entryPoint, LPCSTR shaderModel, ID3DBlob** blobOut)
        {
            DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
            shaderFlags |= D3DCOMPILE_DEBUG;
            shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
            ID3DBlob* errorBlob = nullptr;
            HRESULT hr = D3DCompile(
                source,
                strlen(source),
                nullptr,
                nullptr,
                nullptr,
                entryPoint,
                shaderModel,
                shaderFlags,
                0,
                blobOut,
                &errorBlob);

            if (FAILED(hr))
            {
                if (errorBlob)
                {
                    OutputDebugStringA(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
                    errorBlob->Release();
                }
                return hr;
            }

            if (errorBlob)
            {
                errorBlob->Release();
            }
            return S_OK;
        }
    }

    ModelRenderer::~ModelRenderer()
    {
        Shutdown();
    }

    bool ModelRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        Shutdown();

        m_device = device;
        m_context = context;

        if (m_device == nullptr || m_context == nullptr)
        {
            return false;
        }

        // Cache real DXGI GPU adapter name & VRAM query interface once
        {
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice)
            {
                IDXGIAdapter* dxgiAdapter = nullptr;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) && dxgiAdapter)
                {
                    DXGI_ADAPTER_DESC desc = {};
                    if (SUCCEEDED(dxgiAdapter->GetDesc(&desc)))
                    {
                        char buffer[256] = {};
                        const int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buffer, sizeof(buffer), nullptr, nullptr);
                        if (len > 0)
                        {
                            m_gpuName = buffer;
                            while (!m_gpuName.empty() && m_gpuName.back() == ' ')
                            {
                                m_gpuName.pop_back();
                            }
                        }
                        m_dedicatedVideoMemory = static_cast<uint64_t>(desc.DedicatedVideoMemory);
                    }

                    IDXGIAdapter3* adapter3 = nullptr;
                    if (SUCCEEDED(dxgiAdapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3)
                    {
                        m_dxgiAdapter3 = adapter3;
                    }
                    dxgiAdapter->Release();
                }
                dxgiDevice->Release();
            }
        }
        if (m_gpuName.empty())
        {
            m_gpuName = "Unknown GPU";
        }

        // Compile and create Vertex Shader
        ID3DBlob* vsBlob = nullptr;
        HRESULT hr = CompileShader(g_vertexShaderSource, "VS", "vs_4_0", &vsBlob);
        if (FAILED(hr))
        {
            return false;
        }

        hr = m_device->CreateVertexShader(
            vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(),
            nullptr,
            &m_vertexShader);

        if (FAILED(hr))
        {
            vsBlob->Release();
            return false;
        }

        // Create Input Layout
        D3D11_INPUT_ELEMENT_DESC inputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };

        hr = m_device->CreateInputLayout(
            inputElements,
            ARRAYSIZE(inputElements),
            vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(),
            &m_inputLayout);

        vsBlob->Release();
        if (FAILED(hr))
        {
            return false;
        }

        // Compile and create Pixel Shader
        ID3DBlob* psBlob = nullptr;
        hr = CompileShader(g_pixelShaderSource, "PS", "ps_4_0", &psBlob);
        if (FAILED(hr))
        {
            return false;
        }

        hr = m_device->CreatePixelShader(
            psBlob->GetBufferPointer(),
            psBlob->GetBufferSize(),
            nullptr,
            &m_pixelShader);

        psBlob->Release();
        if (FAILED(hr))
        {
            return false;
        }

        // Create Constant Buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(CBData);
        cbDesc.Usage = D3D11_USAGE_DEFAULT;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
        if (FAILED(hr))
        {
            return false;
        }

        // Create Rasterizer States (multisample-enabled for 4x MSAA)
        D3D11_RASTERIZER_DESC rastDesc = {};
        rastDesc.FillMode = D3D11_FILL_SOLID;
        rastDesc.CullMode = D3D11_CULL_BACK;
        rastDesc.FrontCounterClockwise = TRUE;
        rastDesc.DepthClipEnable = TRUE;
        rastDesc.MultisampleEnable = TRUE;
        rastDesc.AntialiasedLineEnable = TRUE;
        m_device->CreateRasterizerState(&rastDesc, &m_rasterizerStateSolid);

        rastDesc.FillMode = D3D11_FILL_WIREFRAME;
        rastDesc.CullMode = D3D11_CULL_NONE;
        rastDesc.DepthBias = -2;
        rastDesc.SlopeScaledDepthBias = -1.0f;
        m_device->CreateRasterizerState(&rastDesc, &m_rasterizerStateWireframe);

        // Create Depth Stencil State
        D3D11_DEPTH_STENCIL_DESC dsDesc = {};
        dsDesc.DepthEnable = TRUE;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        dsDesc.StencilEnable = FALSE;
        m_device->CreateDepthStencilState(&dsDesc, &m_depthStencilState);

        // Create Depth Stencil State with Depth disabled (always on top)
        dsDesc.DepthEnable = FALSE;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        m_device->CreateDepthStencilState(&dsDesc, &m_depthStencilStateNoDepth);

        // Create Ground Grid Vertices (unit size centered at origin, omitting center axes to avoid z-fighting)
        std::vector<App::ModelPreviewVertex> gridVertices;
        const int gridDivisions = 20;
        const int centerDivision = gridDivisions / 2;
        for (int i = 0; i <= gridDivisions; ++i)
        {
            if (i == centerDivision)
            {
                continue;
            }
            float t = static_cast<float>(i) / static_cast<float>(gridDivisions) - 0.5f;

            // Line parallel to X axis
            gridVertices.push_back({ { -0.5f, 0.0f, t }, { 0.0f, 1.0f, 0.0f } });
            gridVertices.push_back({ { 0.5f, 0.0f, t }, { 0.0f, 1.0f, 0.0f } });

            // Line parallel to Z axis
            gridVertices.push_back({ { t, 0.0f, -0.5f }, { 0.0f, 1.0f, 0.0f } });
            gridVertices.push_back({ { t, 0.0f, 0.5f }, { 0.0f, 1.0f, 0.0f } });
        }
        m_groundVertexCount = static_cast<uint32_t>(gridVertices.size());

        D3D11_BUFFER_DESC vtxDesc = {};
        vtxDesc.ByteWidth = static_cast<UINT>(sizeof(App::ModelPreviewVertex) * gridVertices.size());
        vtxDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vtxDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = gridVertices.data();

        hr = m_device->CreateBuffer(&vtxDesc, &initData, &m_groundVertexBuffer);
        if (FAILED(hr))
        {
            return false;
        }

        // Create Center Axis Line Vertices: [0..1] X axis, [2..3] Z axis
        const App::ModelPreviewVertex axisVertices[4] = {
            { { -0.5f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
            { {  0.5f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
            { {  0.0f, 0.0f, -0.5f }, { 0.0f, 1.0f, 0.0f } },
            { {  0.0f, 0.0f,  0.5f }, { 0.0f, 1.0f, 0.0f } }
        };
        D3D11_BUFFER_DESC axisDesc = {};
        axisDesc.ByteWidth = sizeof(axisVertices);
        axisDesc.Usage = D3D11_USAGE_IMMUTABLE;
        axisDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA axisData = {};
        axisData.pSysMem = axisVertices;
        if (FAILED(m_device->CreateBuffer(&axisDesc, &axisData, &m_axisLineVertexBuffer)))
        {
            m_axisLineVertexBuffer = nullptr;
        }

        // Create triple-buffered hardware GPU timestamp queries
        D3D11_QUERY_DESC disjointDesc = {};
        disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        D3D11_QUERY_DESC timestampDesc = {};
        timestampDesc.Query = D3D11_QUERY_TIMESTAMP;
        for (uint32_t i = 0; i < kGpuQueryRingSize; ++i)
        {
            if (FAILED(m_device->CreateQuery(&disjointDesc, &m_queryDisjoint[i])) ||
                FAILED(m_device->CreateQuery(&timestampDesc, &m_queryStart[i])) ||
                FAILED(m_device->CreateQuery(&timestampDesc, &m_queryEnd[i])))
            {
                if (m_queryDisjoint[i]) { m_queryDisjoint[i]->Release(); m_queryDisjoint[i] = nullptr; }
                if (m_queryStart[i]) { m_queryStart[i]->Release(); m_queryStart[i] = nullptr; }
                if (m_queryEnd[i]) { m_queryEnd[i]->Release(); m_queryEnd[i] = nullptr; }
            }
            m_queryPending[i] = false;
        }

        return true;
    }

    void ModelRenderer::Shutdown()
    {
        ClearModelData();
        CleanupRenderTarget();

        if (m_groundVertexBuffer) { m_groundVertexBuffer->Release(); m_groundVertexBuffer = nullptr; }
        m_groundVertexCount = 0;
        if (m_axisLineVertexBuffer) { m_axisLineVertexBuffer->Release(); m_axisLineVertexBuffer = nullptr; }
        if (m_vertexShader) { m_vertexShader->Release(); m_vertexShader = nullptr; }
        if (m_pixelShader) { m_pixelShader->Release(); m_pixelShader = nullptr; }
        if (m_inputLayout) { m_inputLayout->Release(); m_inputLayout = nullptr; }
        if (m_constantBuffer) { m_constantBuffer->Release(); m_constantBuffer = nullptr; }
        if (m_rasterizerStateSolid) { m_rasterizerStateSolid->Release(); m_rasterizerStateSolid = nullptr; }
        if (m_rasterizerStateWireframe) { m_rasterizerStateWireframe->Release(); m_rasterizerStateWireframe = nullptr; }
        if (m_depthStencilState) { m_depthStencilState->Release(); m_depthStencilState = nullptr; }
        if (m_depthStencilStateNoDepth) { m_depthStencilStateNoDepth->Release(); m_depthStencilStateNoDepth = nullptr; }

        for (uint32_t i = 0; i < kGpuQueryRingSize; ++i)
        {
            if (m_queryDisjoint[i]) { m_queryDisjoint[i]->Release(); m_queryDisjoint[i] = nullptr; }
            if (m_queryStart[i]) { m_queryStart[i]->Release(); m_queryStart[i] = nullptr; }
            if (m_queryEnd[i]) { m_queryEnd[i]->Release(); m_queryEnd[i] = nullptr; }
            m_queryPending[i] = false;
        }
        m_queryWriteIndex = 0;
        m_lastGpuTimeMs = 0.0f;
        m_gpuTimingValid = false;
        m_lastDrawCalls = 0;

        if (m_dxgiAdapter3) { m_dxgiAdapter3->Release(); m_dxgiAdapter3 = nullptr; }
        m_gpuName.clear();
        m_dedicatedVideoMemory = 0;
        m_device = nullptr;
        m_context = nullptr;
    }

    bool ModelRenderer::SetModelData(const std::vector<App::ModelPreviewVertex>& vertices, const std::vector<uint32_t>& indices)
    {
        ClearModelData();

        if (vertices.empty() || indices.empty() || m_device == nullptr)
        {
            return true;
        }

        // Create Vertex Buffer
        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.ByteWidth = static_cast<UINT>(sizeof(App::ModelPreviewVertex) * vertices.size());
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = vertices.data();

        HRESULT hr = m_device->CreateBuffer(&vbDesc, &vbData, &m_vertexBuffer);
        if (FAILED(hr))
        {
            return false;
        }

        // Create Index Buffer
        D3D11_BUFFER_DESC ibDesc = {};
        ibDesc.ByteWidth = static_cast<UINT>(sizeof(uint32_t) * indices.size());
        ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA ibData = {};
        ibData.pSysMem = indices.data();

        hr = m_device->CreateBuffer(&ibDesc, &ibData, &m_indexBuffer);
        if (FAILED(hr))
        {
            m_vertexBuffer->Release();
            m_vertexBuffer = nullptr;
            return false;
        }

        m_vertexCount = static_cast<uint32_t>(vertices.size());
        m_indexCount = static_cast<uint32_t>(indices.size());
        return true;
    }

    void ModelRenderer::ClearModelData()
    {
        if (m_vertexBuffer) { m_vertexBuffer->Release(); m_vertexBuffer = nullptr; }
        if (m_indexBuffer) { m_indexBuffer->Release(); m_indexBuffer = nullptr; }
        m_vertexCount = 0;
        m_indexCount = 0;
    }

    bool ModelRenderer::CreateRenderTarget(uint32_t width, uint32_t height)
    {
        CleanupRenderTarget();

        if (m_device == nullptr || width == 0 || height == 0)
        {
            return false;
        }

        UINT msaaQuality = 0;
        if (SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 4, &msaaQuality)) && msaaQuality > 0)
        {
            m_msaaSampleCount = 4;
        }
        else
        {
            m_msaaSampleCount = 1;
        }

        // Single-sample SRV texture (always used by ImGui::Image)
        D3D11_TEXTURE2D_DESC srvTexDesc = {};
        srvTexDesc.Width = width;
        srvTexDesc.Height = height;
        srvTexDesc.MipLevels = 1;
        srvTexDesc.ArraySize = 1;
        srvTexDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvTexDesc.SampleDesc.Count = 1;
        srvTexDesc.Usage = D3D11_USAGE_DEFAULT;
        srvTexDesc.BindFlags = (m_msaaSampleCount > 1)
            ? D3D11_BIND_SHADER_RESOURCE
            : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

        HRESULT hr = m_device->CreateTexture2D(&srvTexDesc, nullptr, &m_renderTargetTexture);
        if (FAILED(hr))
        {
            CleanupRenderTarget();
            return false;
        }

        hr = m_device->CreateShaderResourceView(m_renderTargetTexture, nullptr, &m_shaderResourceView);
        if (FAILED(hr))
        {
            CleanupRenderTarget();
            return false;
        }

        if (m_msaaSampleCount > 1)
        {
            D3D11_TEXTURE2D_DESC msaaDesc = {};
            msaaDesc.Width = width;
            msaaDesc.Height = height;
            msaaDesc.MipLevels = 1;
            msaaDesc.ArraySize = 1;
            msaaDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            msaaDesc.SampleDesc.Count = m_msaaSampleCount;
            msaaDesc.SampleDesc.Quality = 0;
            msaaDesc.Usage = D3D11_USAGE_DEFAULT;
            msaaDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

            hr = m_device->CreateTexture2D(&msaaDesc, nullptr, &m_msaaRenderTargetTexture);
            if (FAILED(hr))
            {
                CleanupRenderTarget();
                return false;
            }

            hr = m_device->CreateRenderTargetView(m_msaaRenderTargetTexture, nullptr, &m_renderTargetView);
            if (FAILED(hr))
            {
                CleanupRenderTarget();
                return false;
            }
        }
        else
        {
            hr = m_device->CreateRenderTargetView(m_renderTargetTexture, nullptr, &m_renderTargetView);
            if (FAILED(hr))
            {
                CleanupRenderTarget();
                return false;
            }
        }

        // Create Depth Stencil Texture matching render sample count
        D3D11_TEXTURE2D_DESC depthDesc = {};
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = m_msaaSampleCount;
        depthDesc.SampleDesc.Quality = 0;
        depthDesc.Usage = D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        hr = m_device->CreateTexture2D(&depthDesc, nullptr, &m_depthStencilTexture);
        if (FAILED(hr))
        {
            CleanupRenderTarget();
            return false;
        }

        hr = m_device->CreateDepthStencilView(m_depthStencilTexture, nullptr, &m_depthStencilView);
        if (FAILED(hr))
        {
            CleanupRenderTarget();
            return false;
        }

        m_rtWidth = width;
        m_rtHeight = height;
        return true;
    }

    void ModelRenderer::CleanupRenderTarget()
    {
        if (m_msaaRenderTargetTexture) { m_msaaRenderTargetTexture->Release(); m_msaaRenderTargetTexture = nullptr; }
        if (m_renderTargetTexture) { m_renderTargetTexture->Release(); m_renderTargetTexture = nullptr; }
        if (m_renderTargetView) { m_renderTargetView->Release(); m_renderTargetView = nullptr; }
        if (m_shaderResourceView) { m_shaderResourceView->Release(); m_shaderResourceView = nullptr; }
        if (m_depthStencilTexture) { m_depthStencilTexture->Release(); m_depthStencilTexture = nullptr; }
        if (m_depthStencilView) { m_depthStencilView->Release(); m_depthStencilView = nullptr; }
        m_msaaSampleCount = 1;
        m_rtWidth = 0;
        m_rtHeight = 0;
    }

    void ModelRenderer::PollCompletedGpuQueries()
    {
        if (!m_context)
        {
            return;
        }

        for (uint32_t offset = 0; offset < kGpuQueryRingSize; ++offset)
        {
            const uint32_t slot = (m_queryWriteIndex + offset) % kGpuQueryRingSize;
            if (!m_queryPending[slot] || !m_queryDisjoint[slot] || !m_queryStart[slot] || !m_queryEnd[slot])
            {
                continue;
            }

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData = {};
            const HRESULT hrDisjoint = m_context->GetData(
                m_queryDisjoint[slot],
                &disjointData,
                sizeof(disjointData),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hrDisjoint == S_FALSE)
            {
                continue;
            }

            UINT64 startTs = 0;
            UINT64 endTs = 0;
            const HRESULT hrStart = m_context->GetData(
                m_queryStart[slot],
                &startTs,
                sizeof(startTs),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT hrEnd = m_context->GetData(
                m_queryEnd[slot],
                &endTs,
                sizeof(endTs),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);

            if (hrStart == S_FALSE || hrEnd == S_FALSE)
            {
                continue;
            }

            m_queryPending[slot] = false;
            if (hrDisjoint == S_OK && hrStart == S_OK && hrEnd == S_OK &&
                !disjointData.Disjoint && disjointData.Frequency > 0 && endTs >= startTs)
            {
                const double ms = (static_cast<double>(endTs - startTs) * 1000.0) /
                    static_cast<double>(disjointData.Frequency);
                m_lastGpuTimeMs = static_cast<float>(ms);
                m_gpuTimingValid = true;
            }
        }
    }

    ID3D11ShaderResourceView* ModelRenderer::Render(
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
        int hoveredSubmeshIndex,
        const std::vector<App::ModelPreviewSubmesh>& submeshes)
    {
        if (m_device == nullptr || m_context == nullptr || width == 0 || height == 0)
        {
            return nullptr;
        }

        if (width != m_rtWidth || height != m_rtHeight)
        {
            if (!CreateRenderTarget(width, height))
            {
                return nullptr;
            }
        }

        PollCompletedGpuQueries();

        m_lastDrawCalls = 0;
        const uint32_t activeQuerySlot = m_queryWriteIndex;
        const bool beginGpuTimer = !m_queryPending[activeQuerySlot] &&
            m_queryDisjoint[activeQuerySlot] != nullptr &&
            m_queryStart[activeQuerySlot] != nullptr &&
            m_queryEnd[activeQuerySlot] != nullptr;

        if (beginGpuTimer)
        {
            m_context->Begin(m_queryDisjoint[activeQuerySlot]);
            m_context->End(m_queryStart[activeQuerySlot]);
        }

        // Unbind the SRV from all pixel shader slots to avoid D3D warnings about input/output bind conflicts
        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);

        // Clear views
        float clearColor[4] = { lighting.backgroundColor[0], lighting.backgroundColor[1], lighting.backgroundColor[2], 1.0f };
        m_context->ClearRenderTargetView(m_renderTargetView, clearColor);
        m_context->ClearDepthStencilView(m_depthStencilView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

        // Bind target views and viewport
        m_context->OMSetRenderTargets(1, &m_renderTargetView, m_depthStencilView);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(width);
        vp.Height = static_cast<float>(height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vp);

        if (m_vertexBuffer != nullptr && m_indexBuffer != nullptr && m_indexCount > 0)
        {
            m_context->IASetInputLayout(m_inputLayout);

            UINT stride = sizeof(App::ModelPreviewVertex);
            UINT offset = 0;
            m_context->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &offset);
            m_context->IASetIndexBuffer(m_indexBuffer, DXGI_FORMAT_R32_UINT, 0);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            m_context->VSSetShader(m_vertexShader, nullptr, 0);
            m_context->VSSetConstantBuffers(0, 1, &m_constantBuffer);

            m_context->PSSetShader(m_pixelShader, nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, &m_constantBuffer);

            m_context->OMSetDepthStencilState(m_depthStencilState, 0);

            // Matrix calculations using DirectXMath
            float centerX = (bboxMin[0] + bboxMax[0]) * 0.5f;
            float centerY = (bboxMin[1] + bboxMax[1]) * 0.5f;
            float centerZ = (bboxMin[2] + bboxMax[2]) * 0.5f;

            XMVECTOR target = XMVectorSet(
                centerX + panWorldX,
                centerY + panWorldY,
                centerZ + panWorldZ,
                1.0f
            );

            float theta = yaw * XM_PI / 180.0f;
            float phi = pitch * XM_PI / 180.0f;

            float cosP = cosf(phi);
            float sinP = sinf(phi);
            float cosY = cosf(theta);
            float sinY = sinf(theta);

            XMVECTOR offsetVec = XMVectorSet(
                zoom * cosP * sinY,
                zoom * sinP,
                -zoom * cosP * cosY,
                0.0f
            );

            XMVECTOR cameraPos = target + offsetVec;

            XMVECTOR look = XMVector3Normalize(offsetVec);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(look, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
            XMVECTOR up = XMVector3Normalize(XMVector3Cross(right, look));

            XMMATRIX view = XMMatrixLookAtLH(cameraPos, target, up);
            float aspect = static_cast<float>(width) / static_cast<float>(height);

            float nearPlane = std::max(0.01f, zoom * 0.01f);
            float farPlane = std::max(10.0f, zoom * 10.0f);
            XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, nearPlane, farPlane);

            XMMATRIX world = XMMatrixIdentity();
            XMMATRIX mvp = world * view * proj;

            float ly = lighting.lightYaw * XM_PI / 180.0f;
            float lp = lighting.lightPitch * XM_PI / 180.0f;
            XMVECTOR keyLight = XMVectorSet(
                cosf(lp) * sinf(ly),
                sinf(lp),
                -cosf(lp) * cosf(ly),
                0.0f
            );

            CBData cb = {};
            XMStoreFloat4x4(&cb.mvp, mvp);
            XMStoreFloat4x4(&cb.world, world);
            XMStoreFloat4(&cb.cameraPos, cameraPos);
            XMStoreFloat4(&cb.keyLightDir, keyLight);
            cb.keyLightColor = XMFLOAT4(lighting.keyColor[0], lighting.keyColor[1], lighting.keyColor[2], 1.0f);
            cb.fillLightColor = XMFLOAT4(lighting.fillColor[0], lighting.fillColor[1], lighting.fillColor[2], 1.0f);
            cb.ambientColor = XMFLOAT4(lighting.ambientColor[0], lighting.ambientColor[1], lighting.ambientColor[2], 1.0f);
            cb.baseColor = XMFLOAT4(lighting.baseColor[0], lighting.baseColor[1], lighting.baseColor[2], 1.0f);
            cb.specParams = XMFLOAT4(lighting.specIntensity, lighting.specPower, 0.0f, 0.22f);

            // Render Studio Ground Grid & Origin Axes if enabled
            if (lighting.showGround && m_groundVertexBuffer != nullptr && m_groundVertexCount > 0)
            {
                float sizeX = bboxMax[0] - bboxMin[0];
                float sizeZ = bboxMax[2] - bboxMin[2];
                float maxModelSize = std::max(sizeX, sizeZ);
                float gridScale = std::max(20.0f, maxModelSize * 5.0f);

                XMMATRIX groundWorld = XMMatrixScaling(gridScale, 1.0f, gridScale) * XMMatrixTranslation(centerX, bboxMin[1], centerZ);
                XMMATRIX groundMvp = groundWorld * view * proj;

                CBData cbGround = {};
                XMStoreFloat4x4(&cbGround.mvp, groundMvp);
                XMStoreFloat4x4(&cbGround.world, groundWorld);
                XMStoreFloat4(&cbGround.cameraPos, cameraPos);
                cbGround.baseColor = XMFLOAT4(0.24f, 0.27f, 0.31f, 1.0f);
                cbGround.specParams = XMFLOAT4(0.0f, 1.0f, 1.0f, 0.0f); // Unlit

                m_context->IASetVertexBuffers(0, 1, &m_groundVertexBuffer, &stride, &offset);
                m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                m_context->RSSetState(m_rasterizerStateSolid);

                m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cbGround, 0, 0);
                m_context->Draw(m_groundVertexCount, 0);
                ++m_lastDrawCalls;

                if (m_axisLineVertexBuffer != nullptr)
                {
                    m_context->IASetVertexBuffers(0, 1, &m_axisLineVertexBuffer, &stride, &offset);

                    // Center X axis line (subtle warm red)
                    cbGround.baseColor = XMFLOAT4(0.62f, 0.25f, 0.27f, 1.0f);
                    m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cbGround, 0, 0);
                    m_context->Draw(2, 0);
                    ++m_lastDrawCalls;

                    // Center Z axis line (subtle cool blue)
                    cbGround.baseColor = XMFLOAT4(0.25f, 0.44f, 0.68f, 1.0f);
                    m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cbGround, 0, 0);
                    m_context->Draw(2, 2);
                    ++m_lastDrawCalls;
                }

                // Restore model buffers & topology for drawing the model
                m_context->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &offset);
                m_context->IASetIndexBuffer(m_indexBuffer, DXGI_FORMAT_R32_UINT, 0);
                m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            }

            if (renderMode == 1) // Wireframe only
            {
                m_context->RSSetState(m_rasterizerStateWireframe);
                m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cb, 0, 0);
                m_context->DrawIndexed(m_indexCount, 0, 0);
                ++m_lastDrawCalls;
            }
            else if (renderMode == 2) // Solid + Wireframe
            {
                m_context->RSSetState(m_rasterizerStateSolid);
                m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cb, 0, 0);
                m_context->DrawIndexed(m_indexCount, 0, 0);
                ++m_lastDrawCalls;

                m_context->RSSetState(m_rasterizerStateWireframe);
                CBData cbWire = cb;
                float luminance = 0.2126f * cb.baseColor.x + 0.7152f * cb.baseColor.y + 0.0722f * cb.baseColor.z;
                if (luminance < 0.4f)
                {
                    cbWire.baseColor = XMFLOAT4(0.86f, 0.90f, 0.96f, 1.0f);
                }
                else
                {
                    cbWire.baseColor = XMFLOAT4(0.08f, 0.09f, 0.12f, 1.0f);
                }
                cbWire.specParams = XMFLOAT4(0.0f, 1.0f, 1.0f, 0.0f); // Unlit crisp wireframe overlay

                m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cbWire, 0, 0);
                m_context->DrawIndexed(m_indexCount, 0, 0);
                ++m_lastDrawCalls;
            }
            else // Solid only
            {
                m_context->RSSetState(m_rasterizerStateSolid);
                m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cb, 0, 0);
                m_context->DrawIndexed(m_indexCount, 0, 0);
                ++m_lastDrawCalls;
            }

            // Outline highlight pass: render hovered/selected submesh in crisp unlit wireframe always on top
            if (hoveredSubmeshIndex >= 0 && hoveredSubmeshIndex < static_cast<int>(submeshes.size()))
            {
                const auto& submesh = submeshes[hoveredSubmeshIndex];
                if (submesh.indexCount > 0)
                {
                    m_context->RSSetState(m_rasterizerStateWireframe);
                    m_context->OMSetDepthStencilState(m_depthStencilStateNoDepth, 0);

                    CBData cbHighlight = cb;
                    cbHighlight.baseColor = XMFLOAT4(1.0f, 0.86f, 0.20f, 1.0f); // Warm highlight gold
                    cbHighlight.specParams = XMFLOAT4(0.0f, 1.0f, 1.0f, 0.0f); // Unlit

                    m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cbHighlight, 0, 0);
                    m_context->DrawIndexed(submesh.indexCount, submesh.startIndex, 0);
                    ++m_lastDrawCalls;
                }
            }
        }

        // Unbind target views to resolve MSAA or make texture available as shader resource for ImGui
        ID3D11RenderTargetView* nullRTV = nullptr;
        m_context->OMSetRenderTargets(1, &nullRTV, nullptr);

        if (m_msaaSampleCount > 1 && m_msaaRenderTargetTexture != nullptr && m_renderTargetTexture != nullptr)
        {
            m_context->ResolveSubresource(
                m_renderTargetTexture,
                0,
                m_msaaRenderTargetTexture,
                0,
                DXGI_FORMAT_B8G8R8A8_UNORM);
        }

        if (beginGpuTimer)
        {
            m_context->End(m_queryEnd[activeQuerySlot]);
            m_context->End(m_queryDisjoint[activeQuerySlot]);
            m_queryPending[activeQuerySlot] = true;
            m_queryWriteIndex = (activeQuerySlot + 1) % kGpuQueryRingSize;
        }

        return m_shaderResourceView;
    }

    std::string ModelRenderer::GetGPUName() const
    {
        return m_gpuName.empty() ? std::string("Unknown GPU") : m_gpuName;
    }

    void ModelRenderer::GetVRAMInfo(uint64_t& usageBytes, uint64_t& budgetBytes) const
    {
        usageBytes = 0;
        budgetBytes = m_dedicatedVideoMemory;
        if (!m_device)
        {
            return;
        }

        if (m_dxgiAdapter3 != nullptr)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
            if (SUCCEEDED(m_dxgiAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo)))
            {
                usageBytes = memInfo.CurrentUsage;
                budgetBytes = memInfo.Budget;
                return;
            }
        }

        const uint64_t vbSize = m_vertexBuffer ? static_cast<uint64_t>(sizeof(App::ModelPreviewVertex)) * m_vertexCount : 0ull;
        const uint64_t ibSize = m_indexBuffer ? static_cast<uint64_t>(sizeof(uint32_t)) * m_indexCount : 0ull;
        const uint64_t pixelCount = static_cast<uint64_t>(m_rtWidth) * static_cast<uint64_t>(m_rtHeight);
        const uint64_t msaaFactor = static_cast<uint64_t>(std::max<uint32_t>(1, m_msaaSampleCount));
        const uint64_t rtSize = pixelCount * 4ull * (m_msaaSampleCount > 1 ? (msaaFactor + 1ull) : 1ull);
        const uint64_t dsSize = pixelCount * 4ull * msaaFactor;
        usageBytes = vbSize + ibSize + rtSize + dsSize;
    }

    namespace
    {
        ModelRenderer g_modelRenderer;
    }

    bool InitializeModelRenderer(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        return g_modelRenderer.Initialize(device, context);
    }

    void ShutdownModelRenderer()
    {
        g_modelRenderer.Shutdown();
    }

    ModelRenderer* GetModelRenderer()
    {
        return &g_modelRenderer;
    }
}
