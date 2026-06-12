#include "UI/ModelRenderer.h"
#include "Platform/Platform.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <cmath>
#include <algorithm>
#include <stdexcept>

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
            };

            PS_INPUT VS(VS_INPUT input)
            {
                PS_INPUT output;
                output.pos = mul(float4(input.pos, 1.0f), mvp);
                output.normal = mul(input.normal, (float3x3)world);
                return output;
            }
        )";

        const char* g_pixelShaderSource = R"(
            cbuffer ConstantBuffer : register(b0)
            {
                row_major matrix mvp;
                row_major matrix world;
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
            };

            float4 PS(PS_INPUT input) : SV_Target
            {
                float3 N = normalize(input.normal);
                
                // Key Light direction
                float3 L1 = normalize(keyLightDir.xyz);
                float3 C1 = keyLightColor.rgb;
                float diff1 = max(0.0f, dot(N, L1));
                
                // Fill Light direction (opposite key light horizontally)
                float3 L2 = normalize(float3(-L1.x, 0.8f, -L1.z));
                float3 C2 = fillLightColor.rgb;
                float diff2 = max(0.0f, dot(N, L2));
                
                // View direction is along -Z in view space
                float3 V = normalize(float3(0.0f, 0.0f, 1.0f)); 
                float3 H1 = normalize(L1 + V);
                float spec1 = pow(max(0.0f, dot(N, H1)), specParams.y) * specParams.x;
                
                // Diffuse lighting contribution
                float3 diffuseColor = (diff1 * C1 * 0.70f + diff2 * C2 * 0.25f);
                float3 finalColor = baseColor.rgb * (diffuseColor + ambientColor.rgb) + float3(spec1, spec1, spec1) * C1;
                
                return float4(saturate(finalColor), 1.0f);
            }
        )";

        struct CBData
        {
            XMFLOAT4X4 mvp;
            XMFLOAT4X4 world;
            XMFLOAT4 keyLightDir;
            XMFLOAT4 keyLightColor;
            XMFLOAT4 fillLightColor;
            XMFLOAT4 ambientColor;
            XMFLOAT4 baseColor;
            XMFLOAT4 specParams;
        };

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
        m_device = device;
        m_context = context;

        if (m_device == nullptr || m_context == nullptr)
        {
            return false;
        }

        Shutdown();

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

        // Create Rasterizer States
        D3D11_RASTERIZER_DESC rastDesc = {};
        rastDesc.FillMode = D3D11_FILL_SOLID;
        rastDesc.CullMode = D3D11_CULL_BACK;
        rastDesc.FrontCounterClockwise = TRUE;
        rastDesc.DepthClipEnable = TRUE;
        m_device->CreateRasterizerState(&rastDesc, &m_rasterizerStateSolid);

        rastDesc.FillMode = D3D11_FILL_WIREFRAME;
        rastDesc.CullMode = D3D11_CULL_NONE;
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

        // Create Ground Grid Vertices (unit size centered at origin)
        std::vector<App::ModelPreviewVertex> gridVertices;
        int gridDivisions = 20;
        for (int i = 0; i <= gridDivisions; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(gridDivisions) - 0.5f;
            
            // X line (parallel to X axis)
            gridVertices.push_back({ { -0.5f, 0.0f, t }, { 0.0f, 1.0f, 0.0f } });
            gridVertices.push_back({ { 0.5f, 0.0f, t }, { 0.0f, 1.0f, 0.0f } });
            
            // Z line (parallel to Z axis)
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

        return true;
    }

    void ModelRenderer::Shutdown()
    {
        ClearModelData();
        CleanupRenderTarget();

        if (m_groundVertexBuffer) { m_groundVertexBuffer->Release(); m_groundVertexBuffer = nullptr; }
        if (m_vertexShader) { m_vertexShader->Release(); m_vertexShader = nullptr; }
        if (m_pixelShader) { m_pixelShader->Release(); m_pixelShader = nullptr; }
        if (m_inputLayout) { m_inputLayout->Release(); m_inputLayout = nullptr; }
        if (m_constantBuffer) { m_constantBuffer->Release(); m_constantBuffer = nullptr; }
        if (m_rasterizerStateSolid) { m_rasterizerStateSolid->Release(); m_rasterizerStateSolid = nullptr; }
        if (m_rasterizerStateWireframe) { m_rasterizerStateWireframe->Release(); m_rasterizerStateWireframe = nullptr; }
        if (m_depthStencilState) { m_depthStencilState->Release(); m_depthStencilState = nullptr; }
        if (m_depthStencilStateNoDepth) { m_depthStencilStateNoDepth->Release(); m_depthStencilStateNoDepth = nullptr; }
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

        m_indexCount = static_cast<uint32_t>(indices.size());
        return true;
    }

    void ModelRenderer::ClearModelData()
    {
        if (m_vertexBuffer) { m_vertexBuffer->Release(); m_vertexBuffer = nullptr; }
        if (m_indexBuffer) { m_indexBuffer->Release(); m_indexBuffer = nullptr; }
        m_indexCount = 0;
    }

    bool ModelRenderer::CreateRenderTarget(uint32_t width, uint32_t height)
    {
        CleanupRenderTarget();

        if (m_device == nullptr || width == 0 || height == 0)
        {
            return false;
        }

        // Create Render Target Texture
        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, &m_renderTargetTexture);
        if (FAILED(hr)) return false;

        hr = m_device->CreateRenderTargetView(m_renderTargetTexture, nullptr, &m_renderTargetView);
        if (FAILED(hr)) return false;

        hr = m_device->CreateShaderResourceView(m_renderTargetTexture, nullptr, &m_shaderResourceView);
        if (FAILED(hr)) return false;

        // Create Depth Stencil Texture
        D3D11_TEXTURE2D_DESC depthDesc = {};
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Usage = D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        hr = m_device->CreateTexture2D(&depthDesc, nullptr, &m_depthStencilTexture);
        if (FAILED(hr)) return false;

        hr = m_device->CreateDepthStencilView(m_depthStencilTexture, nullptr, &m_depthStencilView);
        if (FAILED(hr)) return false;

        m_rtWidth = width;
        m_rtHeight = height;
        return true;
    }

    void ModelRenderer::CleanupRenderTarget()
    {
        if (m_renderTargetTexture) { m_renderTargetTexture->Release(); m_renderTargetTexture = nullptr; }
        if (m_renderTargetView) { m_renderTargetView->Release(); m_renderTargetView = nullptr; }
        if (m_shaderResourceView) { m_shaderResourceView->Release(); m_shaderResourceView = nullptr; }
        if (m_depthStencilTexture) { m_depthStencilTexture->Release(); m_depthStencilTexture = nullptr; }
        if (m_depthStencilView) { m_depthStencilView->Release(); m_depthStencilView = nullptr; }
        m_rtWidth = 0;
        m_rtHeight = 0;
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
            // Set pipeline states
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

            // Apply world-space pan offsets directly to the rotation target (origin)
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

            // Compute camera offset from target
            XMVECTOR offsetVec = XMVectorSet(
                zoom * cosP * sinY,
                zoom * sinP,
                -zoom * cosP * cosY,
                0.0f
            );

            XMVECTOR cameraPos = target + offsetVec;

            // Compute up vector for lookat matrix
            XMVECTOR look = XMVector3Normalize(offsetVec);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(look, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
            XMVECTOR up = XMVector3Normalize(XMVector3Cross(right, look));

            // View and Projection matrices
            XMMATRIX view = XMMatrixLookAtLH(cameraPos, target, up);
            float aspect = static_cast<float>(width) / static_cast<float>(height);
            
            // Dynamic clipping planes based on camera distance
            float nearPlane = std::max(0.01f, zoom * 0.01f);
            float farPlane = std::max(10.0f, zoom * 10.0f);
            XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, nearPlane, farPlane);

            XMMATRIX world = XMMatrixIdentity();
            XMMATRIX mvp = world * view * proj;

            // Compute key light vector in world space
            float ly = lighting.lightYaw * XM_PI / 180.0f;
            float lp = lighting.lightPitch * XM_PI / 180.0f;
            XMVECTOR keyLight = XMVectorSet(
                cosf(lp) * sinf(ly),
                sinf(lp),
                -cosf(lp) * cosf(ly),
                0.0f
            );

            // Update constant buffer
            CBData cb;
            XMStoreFloat4x4(&cb.mvp, mvp);
            XMStoreFloat4x4(&cb.world, world);
            XMStoreFloat4(&cb.keyLightDir, keyLight);
            cb.keyLightColor = XMFLOAT4(lighting.keyColor[0], lighting.keyColor[1], lighting.keyColor[2], 1.0f);
            cb.fillLightColor = XMFLOAT4(lighting.fillColor[0], lighting.fillColor[1], lighting.fillColor[2], 1.0f);
            cb.ambientColor = XMFLOAT4(lighting.ambientColor[0], lighting.ambientColor[1], lighting.ambientColor[2], 1.0f);
            cb.baseColor = XMFLOAT4(lighting.baseColor[0], lighting.baseColor[1], lighting.baseColor[2], 1.0f);
            cb.specParams = XMFLOAT4(lighting.specIntensity, lighting.specPower, 0.0f, 0.0f);

            // Render Ground Grid if enabled
            if (lighting.showGround && m_groundVertexBuffer != nullptr && m_groundVertexCount > 0)
            {
                float sizeX = bboxMax[0] - bboxMin[0];
                float sizeZ = bboxMax[2] - bboxMin[2];
                float maxModelSize = std::max(sizeX, sizeZ);
                float gridScale = std::max(20.0f, maxModelSize * 5.0f);

                XMMATRIX groundWorld = XMMatrixScaling(gridScale, 1.0f, gridScale) * XMMatrixTranslation(centerX, bboxMin[1], centerZ);
                XMMATRIX groundMvp = groundWorld * view * proj;

                CBData cbGround;
                XMStoreFloat4x4(&cbGround.mvp, groundMvp);
                XMStoreFloat4x4(&cbGround.world, groundWorld);
                XMStoreFloat4(&cbGround.keyLightDir, keyLight);
                cbGround.keyLightColor = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                cbGround.fillLightColor = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
                cbGround.ambientColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
                cbGround.baseColor = XMFLOAT4(0.35f, 0.38f, 0.42f, 1.0f); // Slate gray grid lines
                cbGround.specParams = XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f);

                m_context->IASetVertexBuffers(0, 1, &m_groundVertexBuffer, &stride, &offset);
                m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                m_context->RSSetState(m_rasterizerStateSolid);

                m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cbGround, 0, 0);
                m_context->Draw(m_groundVertexCount, 0);

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
            }
            else if (renderMode == 2) // Solid + Wireframe
            {
                // First pass: Solid
                m_context->RSSetState(m_rasterizerStateSolid);
                m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cb, 0, 0);
                m_context->DrawIndexed(m_indexCount, 0, 0);

                // Second pass: Wireframe on top
                m_context->RSSetState(m_rasterizerStateWireframe);
                
                CBData cbWire = cb;
                float luminance = 0.2126f * cb.baseColor.x + 0.7152f * cb.baseColor.y + 0.0722f * cb.baseColor.z;
                if (luminance < 0.4f)
                {
                    cbWire.baseColor = XMFLOAT4(0.9f, 0.92f, 0.95f, 1.0f); // Light lines on dark solid
                }
                else
                {
                    cbWire.baseColor = XMFLOAT4(0.08f, 0.08f, 0.1f, 1.0f); // Dark lines on light solid
                }
                cbWire.specParams = XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f); // No specular on wireframe lines

                m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cbWire, 0, 0);
                m_context->DrawIndexed(m_indexCount, 0, 0);
            }
            else // Solid only
            {
                m_context->RSSetState(m_rasterizerStateSolid);
                m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cb, 0, 0);
                m_context->DrawIndexed(m_indexCount, 0, 0);
            }

            // Outline highlight pass: render hovered submesh in wireframe ALWAYS on top (yellow)
            if (hoveredSubmeshIndex >= 0 && hoveredSubmeshIndex < static_cast<int>(submeshes.size()))
            {
                const auto& submesh = submeshes[hoveredSubmeshIndex];
                if (submesh.indexCount > 0)
                {
                    m_context->RSSetState(m_rasterizerStateWireframe);
                    m_context->OMSetDepthStencilState(m_depthStencilStateNoDepth, 0);

                    CBData cbHighlight = cb;
                    cbHighlight.baseColor = XMFLOAT4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow
                    cbHighlight.specParams = XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f); // No specular on outline lines

                    m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &cbHighlight, 0, 0);
                    m_context->DrawIndexed(submesh.indexCount, submesh.startIndex, 0);
                }
            }
        }

        // Unbind target views to make the texture available as a shader resource for ImGui
        ID3D11RenderTargetView* nullRTV = nullptr;
        m_context->OMSetRenderTargets(1, &nullRTV, nullptr);

        return m_shaderResourceView;
    }

    std::string ModelRenderer::GetGPUName() const
    {
        if (!m_device) return "Unknown GPU";

        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))))
        {
            IDXGIAdapter* dxgiAdapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)))
            {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(dxgiAdapter->GetDesc(&desc)))
                {
                    char buffer[128];
                    int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buffer, sizeof(buffer), nullptr, nullptr);
                    dxgiAdapter->Release();
                    dxgiDevice->Release();
                    if (len > 0)
                    {
                        return std::string(buffer);
                    }
                    return "Unknown GPU";
                }
                dxgiAdapter->Release();
            }
            dxgiDevice->Release();
        }
        return "Unknown GPU";
    }

    void ModelRenderer::GetVRAMInfo(uint64_t& usageBytes, uint64_t& budgetBytes) const
    {
        usageBytes = 0;
        budgetBytes = 0;
        if (!m_device) return;

        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))))
        {
            IDXGIAdapter* dxgiAdapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)))
            {
                IDXGIAdapter3* dxgiAdapter3 = nullptr;
                if (SUCCEEDED(dxgiAdapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&dxgiAdapter3))))
                {
                    DXGI_QUERY_VIDEO_MEMORY_INFO memInfo;
                    if (SUCCEEDED(dxgiAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo)))
                    {
                        usageBytes = memInfo.CurrentUsage;
                        budgetBytes = memInfo.Budget;
                    }
                    dxgiAdapter3->Release();
                }
                else
                {
                    DXGI_ADAPTER_DESC desc;
                    if (SUCCEEDED(dxgiAdapter->GetDesc(&desc)))
                    {
                        budgetBytes = desc.DedicatedVideoMemory;
                        uint64_t vbSize = m_vertexBuffer ? sizeof(App::ModelPreviewVertex) * m_indexCount : 0;
                        uint64_t ibSize = m_indexBuffer ? sizeof(uint32_t) * m_indexCount : 0;
                        uint64_t rtSize = m_rtWidth * m_rtHeight * 4;
                        uint64_t dsSize = m_rtWidth * m_rtHeight * 4;
                        usageBytes = vbSize + ibSize + rtSize + dsSize;
                    }
                }
                dxgiAdapter->Release();
            }
            dxgiDevice->Release();
        }
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
