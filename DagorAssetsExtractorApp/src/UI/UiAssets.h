#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace DagorAssetsExtractor::UI
{
    enum class UiIconId : uint32_t
    {
        Invalid = 0,
        GameInstall,
        ContentBrowser,
        Log,
        Inspect,
        Rebuild,
        Github,
        Physics,
        Texture,
        Collision,
        DynModel,
        GeomNodeTree,
        RendInst,
        Anim2Data,
        Character,
        AnimGraph,
        Patreon,
        Count
    };

    struct UiIcon
    {
        ID3D11ShaderResourceView* view = nullptr;
    };

    struct UiTexture
    {
        ID3D11ShaderResourceView* view = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    void InitializeUiAssets(ID3D11Device* device);
    void ShutdownUiAssets();
    const UiIcon* GetUiIcon(UiIconId id);
    bool UpdateTexturePreview(
        const uint8_t* bgraPixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        bool srgb);
    void ClearTexturePreview();
    const UiTexture* GetTexturePreview();
    bool UpdateTextureMapPreview(
        uint32_t mapKind,
        const uint8_t* bgraPixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride);
    void ClearTextureMapPreviews();
    const UiTexture* GetTextureMapPreview(uint32_t mapKind);
}
