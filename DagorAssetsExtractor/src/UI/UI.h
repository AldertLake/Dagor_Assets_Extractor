#pragma once

#include "App/AppState.h"

struct ID3D11Device;

namespace DagorAssetsExtractor::UI
{
    void InitializeUiAssets(ID3D11Device* device);
    void ShutdownUiAssets();
    void DrawMainShell(App::AppState& app, bool& running);
}
