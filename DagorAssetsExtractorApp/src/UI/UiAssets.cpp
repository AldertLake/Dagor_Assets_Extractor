#include "UI/UiAssets.h"

#include "Platform/Platform.h"

#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace DagorAssetsExtractor::UI
{
    namespace
    {
        namespace fs = std::filesystem;
        using Microsoft::WRL::ComPtr;

        constexpr size_t kIconCount = static_cast<size_t>(UiIconId::Count);

        struct OwnedUiIcon
        {
            ComPtr<ID3D11ShaderResourceView> view;
        };

        std::array<OwnedUiIcon, kIconCount> g_ownedIcons;
        std::array<UiIcon, kIconCount> g_publicIcons;
        ComPtr<ID3D11ShaderResourceView> g_texturePreviewView;
        UiTexture g_texturePreview;
        constexpr size_t kTextureMapPreviewCount = 8;
        std::array<ComPtr<ID3D11ShaderResourceView>, kTextureMapPreviewCount>
            g_textureMapPreviewViews;
        std::array<UiTexture, kTextureMapPreviewCount> g_textureMapPreviews;
        ID3D11Device* g_device = nullptr;

        size_t ToIndex(UiIconId id)
        {
            return static_cast<size_t>(id);
        }

        void PublishIcon(UiIconId id)
        {
            const size_t index = ToIndex(id);
            g_publicIcons[index].view = g_ownedIcons[index].view.Get();
        }

        void ClearIcons()
        {
            for (size_t index = 0; index < kIconCount; ++index)
            {
                g_ownedIcons[index].view.Reset();
                g_publicIcons[index].view = nullptr;
            }
        }

        bool CreateTextureFromBgra(
            const uint8_t* pixels,
            uint32_t width,
            uint32_t height,
            uint32_t stride,
            bool srgb,
            ComPtr<ID3D11ShaderResourceView>& outView)
        {
            if (g_device == nullptr || pixels == nullptr || width == 0 || height == 0 ||
                stride < width * 4u)
            {
                return false;
            }

            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = width;
            textureDesc.Height = height;
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = srgb
                ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                : DXGI_FORMAT_B8G8R8A8_UNORM;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA subresource = {};
            subresource.pSysMem = pixels;
            subresource.SysMemPitch = stride;

            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(g_device->CreateTexture2D(&textureDesc, &subresource, &texture)))
            {
                return false;
            }

            if (FAILED(g_device->CreateShaderResourceView(texture.Get(), nullptr, &outView)))
            {
                return false;
            }

            return true;
        }

        bool CreateTextureFromBgra(
            const std::vector<uint8_t>& pixels,
            uint32_t width,
            uint32_t height,
            ComPtr<ID3D11ShaderResourceView>& outView)
        {
            return CreateTextureFromBgra(
                pixels.data(),
                width,
                height,
                width * 4u,
                false,
                outView);
        }

        bool LoadPngTexture(const fs::path& path, ComPtr<ID3D11ShaderResourceView>& outView)
        {
            ComPtr<IWICImagingFactory> factory;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            {
                return false;
            }

            ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(factory->CreateDecoderFromFilename(
                    path.wstring().c_str(),
                    nullptr,
                    GENERIC_READ,
                    WICDecodeMetadataCacheOnLoad,
                    &decoder)))
            {
                return false;
            }

            ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, &frame)))
            {
                return false;
            }

            ComPtr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(&converter)))
            {
                return false;
            }

            if (FAILED(converter->Initialize(
                    frame.Get(),
                    GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone,
                    nullptr,
                    0.0,
                    WICBitmapPaletteTypeCustom)))
            {
                return false;
            }

            UINT width = 0;
            UINT height = 0;
            if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
            {
                return false;
            }

            std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
            const UINT stride = width * 4;
            if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
            {
                return false;
            }

            return CreateTextureFromBgra(pixels, width, height, outView);
        }

        void CreateCheckerFallback(ComPtr<ID3D11ShaderResourceView>& outView)
        {
            std::vector<uint8_t> pixels(16u * 16u * 4u, 0);
            for (uint32_t y = 0; y < 16; ++y)
            {
                for (uint32_t x = 0; x < 16; ++x)
                {
                    const bool magenta = ((x / 4u) + (y / 4u)) % 2u == 0u;
                    const size_t offset = (static_cast<size_t>(y) * 16u + x) * 4u;
                    pixels[offset + 0] = magenta ? 0xFFu : 0x20u;
                    pixels[offset + 1] = 0x00u;
                    pixels[offset + 2] = magenta ? 0xFFu : 0x20u;
                    pixels[offset + 3] = 0xFFu;
                }
            }

            CreateTextureFromBgra(pixels, 16, 16, outView);
        }

        bool LoadUiTextureByName(const char* filename, UiIconId targetId, UiIconId fallbackId = UiIconId::Invalid)
        {
            const fs::path path = Platform::GetUiAssetPath() / Platform::Utf8ToPath(filename);

            ComPtr<ID3D11ShaderResourceView> view;
            if (!LoadPngTexture(path, view))
            {
                const size_t fallbackIndex = ToIndex(fallbackId);
                if (fallbackIndex < kIconCount && g_ownedIcons[fallbackIndex].view)
                {
                    g_ownedIcons[ToIndex(targetId)].view = g_ownedIcons[fallbackIndex].view;
                    PublishIcon(targetId);
                    return true;
                }
                return false;
            }

            const size_t index = ToIndex(targetId);
            g_ownedIcons[index].view = std::move(view);
            PublishIcon(targetId);
            return true;
        }
    }

    void InitializeUiAssets(ID3D11Device* device)
    {
        g_device = device;
        ClearIcons();

        if (g_device == nullptr)
        {
            return;
        }

        LoadUiTextureByName("Invalid.png", UiIconId::Invalid);
        if (!g_ownedIcons[ToIndex(UiIconId::Invalid)].view)
        {
            CreateCheckerFallback(g_ownedIcons[ToIndex(UiIconId::Invalid)].view);
            PublishIcon(UiIconId::Invalid);
        }

        LoadUiTextureByName("GameInstall.png", UiIconId::GameInstall);
        LoadUiTextureByName("ContentBrowser.png", UiIconId::ContentBrowser);
        LoadUiTextureByName("Log.png", UiIconId::Log);
        LoadUiTextureByName("Inspect.png", UiIconId::Inspect);
        LoadUiTextureByName("Rebuild.png", UiIconId::Rebuild);
        LoadUiTextureByName("Github.png", UiIconId::Github);
        LoadUiTextureByName("Physics.png", UiIconId::Physics);
        LoadUiTextureByName("Texture.png", UiIconId::Texture);
        LoadUiTextureByName("Collision.png", UiIconId::Collision);
        LoadUiTextureByName("DynModel.png", UiIconId::DynModel);
        LoadUiTextureByName("GeomNodeTree.png", UiIconId::GeomNodeTree);
        LoadUiTextureByName("RendInst.png", UiIconId::RendInst);
        LoadUiTextureByName("Anim2Data.png", UiIconId::Anim2Data);
        LoadUiTextureByName("Character.png", UiIconId::Character);
        LoadUiTextureByName("AnimGraph.png", UiIconId::AnimGraph);
        LoadUiTextureByName("patreon.png", UiIconId::Patreon);
    }

    void ShutdownUiAssets()
    {
        ClearTexturePreview();
        ClearTextureMapPreviews();
        ClearIcons();
        g_device = nullptr;
    }

    const UiIcon* GetUiIcon(UiIconId id)
    {
        const size_t index = ToIndex(id);
        if (index >= g_publicIcons.size())
        {
            return nullptr;
        }

        const UiIcon* icon = &g_publicIcons[index];
        if (icon->view != nullptr)
        {
            return icon;
        }

        return &g_publicIcons[ToIndex(UiIconId::Invalid)];
    }

    bool UpdateTexturePreview(
        const uint8_t* bgraPixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        bool srgb)
    {
        ComPtr<ID3D11ShaderResourceView> view;
        if (!CreateTextureFromBgra(bgraPixels, width, height, stride, srgb, view))
        {
            return false;
        }

        g_texturePreviewView = std::move(view);
        g_texturePreview.view = g_texturePreviewView.Get();
        g_texturePreview.width = width;
        g_texturePreview.height = height;
        return true;
    }

    void ClearTexturePreview()
    {
        g_texturePreviewView.Reset();
        g_texturePreview = {};
        ClearTextureMapPreviews();
    }

    const UiTexture* GetTexturePreview()
    {
        return g_texturePreview.view != nullptr ? &g_texturePreview : nullptr;
    }

    bool UpdateTextureMapPreview(
        uint32_t mapKind,
        const uint8_t* bgraPixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride)
    {
        const size_t index = static_cast<size_t>(mapKind);
        if (index >= kTextureMapPreviewCount)
        {
            return false;
        }

        ComPtr<ID3D11ShaderResourceView> view;
        if (!CreateTextureFromBgra(
                bgraPixels,
                width,
                height,
                stride,
                false,
                view))
        {
            return false;
        }

        g_textureMapPreviewViews[index] = std::move(view);
        g_textureMapPreviews[index].view = g_textureMapPreviewViews[index].Get();
        g_textureMapPreviews[index].width = width;
        g_textureMapPreviews[index].height = height;
        return true;
    }

    void ClearTextureMapPreviews()
    {
        for (size_t index = 0; index < kTextureMapPreviewCount; ++index)
        {
            g_textureMapPreviewViews[index].Reset();
            g_textureMapPreviews[index] = {};
        }
    }

    const UiTexture* GetTextureMapPreview(uint32_t mapKind)
    {
        const size_t index = static_cast<size_t>(mapKind);
        if (index >= kTextureMapPreviewCount ||
            g_textureMapPreviews[index].view == nullptr)
        {
            return nullptr;
        }
        return &g_textureMapPreviews[index];
    }
}
