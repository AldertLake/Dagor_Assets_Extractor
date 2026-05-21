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
        Count
    };

    struct UiIcon
    {
        ID3D11ShaderResourceView* view = nullptr;
    };

    void InitializeUiAssets(ID3D11Device* device);
    void ShutdownUiAssets();
    const UiIcon* GetUiIcon(UiIconId id);
}
