#include "UI/UI.h"
#include "UI/UiAssets.h"
#include "UI/ModelRenderer.h"
#include "App/AppState.h"
#include <DirectXMath.h>
#include "Platform/Platform.h"
#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/PackContents.h"
#include "DagorFormats/BlkReader.h"
#include "Export/MaterialTextureResolver.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

namespace DagorAssetsExtractor::UI
{
    using App::AppState;
    using App::LogLevel;
    using App::LogEntry;
    using AssetIndex::ResourceRecord;
    using AssetIndex::AssetType;

    struct Ray
    {
        DirectX::XMVECTOR origin;
        DirectX::XMVECTOR direction;
    };

    static bool RayAABBIntersection(const Ray& ray, const float bboxMin[3], const float bboxMax[3], float& tNearOut)
    {
        using namespace DirectX;
        XMFLOAT3 origin, dir;
        XMStoreFloat3(&origin, ray.origin);
        XMStoreFloat3(&dir, ray.direction);

        float tMin = -FLT_MAX;
        float tMax = FLT_MAX;

        float originArr[3] = { origin.x, origin.y, origin.z };
        float dirArr[3] = { dir.x, dir.y, dir.z };

        for (int i = 0; i < 3; ++i)
        {
            float o = originArr[i];
            float d = dirArr[i];
            float minVal = bboxMin[i];
            float maxVal = bboxMax[i];

            if (std::fabs(d) < 1e-6f)
            {
                if (o < minVal || o > maxVal)
                    return false;
            }
            else
            {
                float invD = 1.0f / d;
                float t1 = (minVal - o) * invD;
                float t2 = (maxVal - o) * invD;

                if (t1 > t2) std::swap(t1, t2);

                tMin = std::max(tMin, t1);
                tMax = std::min(tMax, t2);

                if (tMin > tMax || tMax < 0.0f)
                    return false;
            }
        }

        tNearOut = std::max(0.0f, tMin);
        return true;
    }

    static bool RayTriangleIntersection(const Ray& ray, const DirectX::XMVECTOR& v0, const DirectX::XMVECTOR& v1, const DirectX::XMVECTOR& v2, float& tOut)
    {
        using namespace DirectX;
        XMVECTOR edge1 = v1 - v0;
        XMVECTOR edge2 = v2 - v0;
        XMVECTOR h = XMVector3Cross(ray.direction, edge2);
        XMVECTOR aVec = XMVector3Dot(edge1, h);
        float a = XMVectorGetX(aVec);

        if (a > -1e-6f && a < 1e-6f)
            return false;

        float f = 1.0f / a;
        XMVECTOR s = ray.origin - v0;
        XMVECTOR uVec = XMVector3Dot(s, h) * f;
        float u = XMVectorGetX(uVec);

        if (u < 0.0f || u > 1.0f)
            return false;

        XMVECTOR q = XMVector3Cross(s, edge1);
        XMVECTOR vVec = XMVector3Dot(ray.direction, q) * f;
        float v = XMVectorGetX(vVec);

        if (v < 0.0f || u + v > 1.0f)
            return false;

        XMVECTOR tVec = XMVector3Dot(edge2, q) * f;
        float t = XMVectorGetX(tVec);

        if (t > 1e-6f)
        {
            tOut = t;
            return true;
        }

        return false;
    }

    void SaveSettingsWithImGui(AppState& app)
    {
        size_t iniSize = 0;
        const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
        App::SaveSettingsWithLog(app, std::string_view(iniData, iniSize));
    }

    std::string FormatBytes(uintmax_t bytes)
    {
        const char* units[] = { "B", "KB", "MB", "GB" };
        double value = static_cast<double>(bytes);
        size_t unit = 0;
        while (value >= 1024.0 && unit + 1 < std::size(units))
        {
            value /= 1024.0;
            ++unit;
        }

        std::ostringstream stream;
        if (unit == 0)
        {
            stream << static_cast<uintmax_t>(value) << ' ' << units[unit];
        }
        else
        {
            stream << std::fixed << std::setprecision(1) << value << ' ' << units[unit];
        }
        return stream.str();
    }

    ImVec4 LevelColor(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Warning:
            return ImVec4(1.0f, 0.74f, 0.27f, 1.0f);
        case LogLevel::Error:
            return ImVec4(1.0f, 0.38f, 0.34f, 1.0f);
        default:
            return ImVec4(0.72f, 0.84f, 1.0f, 1.0f);
        }
    }

    const char* LevelName(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Warning:
            return "Warning";
        case LogLevel::Error:
            return "Error";
        default:
            return "Info";
        }
    }

    constexpr float kPanelIconSpacing = 6.0f;
    constexpr float kRowIconSize = 16.0f;
    constexpr float kButtonIconSpacing = 6.0f;

    float GetInlineIconSize()
    {
        return std::max(12.0f, ImGui::GetTextLineHeight());
    }

    ImVec2 CalcButtonSize(const char* label)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        return ImVec2(
            textSize.x + style.FramePadding.x * 2.0f,
            textSize.y + style.FramePadding.y * 2.0f);
    }

    ImVec2 CalcIconButtonSize(const char* label, bool hasIcon)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const float iconSize = hasIcon ? GetInlineIconSize() : 0.0f;
        const float width = textSize.x + style.FramePadding.x * 2.0f + (hasIcon ? iconSize + kButtonIconSpacing : 0.0f);
        const float height = std::max(textSize.y, iconSize) + style.FramePadding.y * 2.0f;
        return ImVec2(width, height);
    }

    void DrawPanelTitle(UiIconId iconId, const char* title)
    {
        const UiIcon* icon = GetUiIcon(iconId);
        ImGui::BeginGroup();
        ImGui::AlignTextToFramePadding();
        if (icon && icon->view != nullptr)
        {
            const float iconSize = GetInlineIconSize();
            const ImVec2 textSize = ImGui::CalcTextSize(title);
            const ImGuiStyle& style = ImGui::GetStyle();
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float iconY = cursor.y + style.FramePadding.y + ((textSize.y - iconSize) * 0.5f);
            ImGui::GetWindowDrawList()->AddImage(
                reinterpret_cast<ImTextureID>(icon->view),
                ImVec2(cursor.x, iconY),
                ImVec2(cursor.x + iconSize, iconY + iconSize));
            ImGui::Dummy(ImVec2(iconSize, iconSize));
            ImGui::SameLine(0.0f, kPanelIconSpacing);
        }
        ImGui::TextUnformatted(title);
        ImGui::EndGroup();
    }

    void DrawInlineIconLabel(UiIconId iconId, const char* label)
    {
        const UiIcon* icon = GetUiIcon(iconId);
        if (icon && icon->view != nullptr)
        {
            ImGui::Image(reinterpret_cast<ImTextureID>(icon->view), ImVec2(kRowIconSize, kRowIconSize));
            ImGui::SameLine(0.0f, 6.0f);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
    }

    bool DrawInlineIconSelectable(UiIconId iconId, const char* label, bool selected)
    {
        const UiIcon* icon = GetUiIcon(iconId);
        if (icon && icon->view != nullptr)
        {
            ImGui::Image(reinterpret_cast<ImTextureID>(icon->view), ImVec2(kRowIconSize, kRowIconSize));
            ImGui::SameLine(0.0f, 6.0f);
        }
        return ImGui::Selectable(label, selected);
    }

    bool DrawIconTextButton(const char* id, UiIconId iconId, const char* label, const ImVec2& sizeArg = ImVec2(0.0f, 0.0f))
    {
        const UiIcon* icon = GetUiIcon(iconId);
        const bool hasIcon = icon != nullptr && icon->view != nullptr;
        const float iconSize = hasIcon ? GetInlineIconSize() : 0.0f;
        const ImVec2 calculatedSize = CalcIconButtonSize(label, hasIcon);

        ImVec2 size = sizeArg;
        if (size.x == 0.0f) size.x = calculatedSize.x;
        else if (size.x < 0.0f) size.x = ImGui::GetContentRegionAvail().x;

        if (size.y == 0.0f) size.y = calculatedSize.y;

        const bool pressed = ImGui::Button(id, size);

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        const float contentWidth = (hasIcon ? iconSize + kButtonIconSpacing : 0.0f) + textSize.x;
        const float startX = min.x + (size.x - contentWidth) * 0.5f;

        if (hasIcon)
        {
            const float iconY = min.y + (size.y - iconSize) * 0.5f;
            drawList->AddImage(
                reinterpret_cast<ImTextureID>(icon->view),
                ImVec2(startX, iconY),
                ImVec2(startX + iconSize, iconY + iconSize));
        }

        const float textX = startX + (hasIcon ? iconSize + kButtonIconSpacing : 0.0f);
        const float textY = min.y + (size.y - textSize.y) * 0.5f;
        drawList->AddText(ImVec2(textX, textY), ImGui::GetColorU32(ImGuiCol_Text), label);
        return pressed;
    }

    UiIconId GetSubAssetIconId(const AssetIndex::SubAssetRecord& entry, AssetType containerType)
    {
        if (containerType == AssetType::Dxp)
        {
            return UiIconId::Texture;
        }

        std::string typeToken = AssetIndex::ToLowerAscii(entry.formatInfo);
        if (typeToken.empty())
        {
            typeToken = AssetIndex::ToLowerAscii(entry.name);
        }

        const auto contains = [&](const char* needle) -> bool
        {
            return typeToken.find(needle) != std::string::npos;
        };

        if (contains("physobj"))
        {
            return UiIconId::Physics;
        }
        if (contains("collision"))
        {
            return UiIconId::Collision;
        }
        if (contains("dynmodel"))
        {
            return UiIconId::DynModel;
        }
        if (contains("geomnodetree"))
        {
            return UiIconId::GeomNodeTree;
        }
        if (contains("rendinst"))
        {
            return UiIconId::RendInst;
        }
        if (contains("anim2data"))
        {
            return UiIconId::Anim2Data;
        }
        if (contains("character"))
        {
            return UiIconId::Character;
        }
        if (contains("animgraph"))
        {
            return UiIconId::AnimGraph;
        }
        if (contains("texture") || contains("tex"))
        {
            return UiIconId::Texture;
        }

        return UiIconId::Invalid;
    }

    void DrawWrappedText(std::string_view text)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
    }

    void DrawWrappedDisabledText(std::string_view text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        DrawWrappedText(text);
        ImGui::PopStyleColor();
    }

    void DrawWrappedColoredText(const ImVec4& color, std::string_view text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        DrawWrappedText(text);
        ImGui::PopStyleColor();
    }

    void DrawWrappedBulletText(std::string_view text)
    {
        ImGui::Bullet();
        ImGui::SameLine();
        DrawWrappedText(text);
    }

    void DrawWrappedTooltip(std::string_view text)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    void DrawValidationSummary(const AssetIndex::ValidationResult& validation)
    {
        ImGui::TextColored(validation.IsValid() ? ImVec4(0.43f, 0.86f, 0.55f, 1.0f) : ImVec4(1.0f, 0.49f, 0.42f, 1.0f),
            validation.IsValid() ? "Valid War Thunder folder" : "No valid game folder");

        for (const std::string& message : validation.messages)
        {
            DrawWrappedBulletText(message);
        }
    }

    void DrawMenuBar(AppState& app, bool& running)
    {
        if (!ImGui::BeginMenuBar())
        {
            return;
        }

        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Choose Game Folder"))
            {
                if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose War Thunder Folder", app.settings.gameRoot))
                {
                    App::SetGameRoot(app, *selected);
                    SaveSettingsWithImGui(app);
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
            {
                running = false;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Settings", nullptr, &app.settingsWindowOpen);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools"))
        {
            const bool canScan = app.validation.IsValid() && !app.scan.running.load();
            ImGui::BeginDisabled(!canScan);
            if (ImGui::MenuItem("Rebuild Resources"))
            {
                App::StartScan(app);
            }
            ImGui::EndDisabled();

            const bool canRebuildBlk = app.validation.IsValid() && app.scan.completed.load() && !App::IsBlkResourceLinksLoading(app);
            ImGui::BeginDisabled(!canRebuildBlk);
            if (ImGui::MenuItem("Rebuild BLK Relationships"))
            {
                App::RequestBlkResourceLinks(app, App::SnapshotRecords(app.scan));
            }
            ImGui::EndDisabled();

            const bool canRebuildDesc = app.validation.IsValid() && app.scan.completed.load() && !App::IsDescriptorResourceLinksLoading(app);
            ImGui::BeginDisabled(!canRebuildDesc);
            if (ImGui::MenuItem("Rebuild Descriptor Relationships"))
            {
                App::RequestDescriptorResourceLinks(app, App::SnapshotRecords(app.scan));
            }
            ImGui::EndDisabled();

            const bool canClearCache = !app.scan.running.load() && !App::IsBlkResourceLinksLoading(app) && !App::IsDescriptorResourceLinksLoading(app);
            ImGui::BeginDisabled(!canClearCache);
            if (ImGui::MenuItem("Invalidate & Clear Cache"))
            {
                App::InvalidateAndClearCache(app);
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About"))
            {
                app.aboutWindowOpen = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }



    bool RecordPassesTypeFilter(const AppState& app, AssetType type)
    {
        switch (type)
        {
        case AssetType::Vromfs:
            return app.showVromfs;
        case AssetType::Grp:
            return app.showGrp;
        case AssetType::Dxp:
            return app.showDxp;
        case AssetType::Descriptor:
            return app.showDescriptors;
        case AssetType::Blk:
            return app.showBlk;
        default:
            return true;
        }
    }

    bool RecordPassesSearch(const ResourceRecord& record, const std::string& search)
    {
        if (search.empty())
        {
            return true;
        }

        const std::string haystack = AssetIndex::ToLowerAscii(Platform::PathToUtf8(record.path));
        return haystack.find(search) != std::string::npos;
    }

    bool SubAssetPassesSearch(const AssetIndex::SubAssetRecord& entry, const std::string& search)
    {
        if (search.empty())
        {
            return true;
        }

        std::string haystack = entry.name + " " + entry.formatInfo + " " +
            entry.texFormat + " " + entry.texCompression + " " + entry.skeletonRootName;
        for (const std::string& textureName : entry.textureNames)
        {
            haystack += " " + textureName;
        }
        for (const std::string& materialName : entry.materialNames)
        {
            haystack += " " + materialName;
        }

        return AssetIndex::ToLowerAscii(std::move(haystack)).find(search) != std::string::npos;
    }

    void EnsureResourceUiCache(AppState& app, const std::shared_ptr<const std::vector<ResourceRecord>>& recordsPtr)
    {
        if (app.resourceUiCache.sourceRecords == recordsPtr)
        {
            return;
        }

        app.resourceUiCache.sourceRecords = recordsPtr;
        app.resourceUiCache.typeCounts.fill(0);
        app.resourceUiCache.rows.clear();
        app.resourceUiCache.indexByNativePath.clear();

        if (!recordsPtr)
        {
            return;
        }

        const std::vector<ResourceRecord>& records = *recordsPtr;
        app.resourceUiCache.rows.reserve(records.size());
        app.resourceUiCache.indexByNativePath.reserve(records.size());

        for (size_t i = 0; i < records.size(); ++i)
        {
            const ResourceRecord& record = records[i];
            switch (record.type)
            {
            case AssetType::Vromfs: ++app.resourceUiCache.typeCounts[0]; break;
            case AssetType::Grp: ++app.resourceUiCache.typeCounts[1]; break;
            case AssetType::Dxp: ++app.resourceUiCache.typeCounts[2]; break;
            case AssetType::Descriptor: ++app.resourceUiCache.typeCounts[3]; break;
            case AssetType::Blk: ++app.resourceUiCache.typeCounts[4]; break;
            default: break;
            }

            AppState::ResourceUiRow row;
            row.nativePath = record.path.wstring();
            row.fileNameUtf8 = Platform::PathToUtf8(record.path.filename());
            row.parentDirUtf8 = Platform::PathToUtf8(record.path.parent_path());
            row.sizeFormatted = FormatBytes(record.sizeBytes);
            row.searchHaystackLower = AssetIndex::ToLowerAscii(Platform::PathToUtf8(record.path));

            app.resourceUiCache.indexByNativePath.try_emplace(row.nativePath, i);
            app.resourceUiCache.rows.push_back(std::move(row));
        }
    }

    void RebuildDescriptorUiMatches(
        AppState& app,
        const std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>>& links,
        const ResourceRecord& selected)
    {
        if (app.descriptorUiLinks == links &&
            app.descriptorUiSelectedPath == selected.path.wstring() &&
            app.descriptorUiSelectedType == selected.type)
        {
            return;
        }

        app.descriptorUiLinks = links;
        app.descriptorUiSelectedPath = selected.path.wstring();
        app.descriptorUiSelectedType = selected.type;
        app.descriptorUiDescriptorMatches.clear();
        app.descriptorUiResolvedGrpCount = 0;
        app.descriptorUiGrpMatches.clear();
        app.descriptorUiDxpMatches.clear();

        if (!links)
        {
            return;
        }

        auto finalizeDescriptorMatchCounts = [&]() {
            app.descriptorUiResolvedGrpCount = static_cast<size_t>(std::count_if(
                app.descriptorUiDescriptorMatches.begin(),
                app.descriptorUiDescriptorMatches.end(),
                [&](size_t linkIndex) { return (*links)[linkIndex].matchedGrpPath.has_value(); }));
        };

        if (const auto runtimeIndex = App::SnapshotGlobalAssetIndex(app))
        {
            const std::wstring selectedKey = AssetIndex::GlobalAssetIndex::MakePathKey(selected.path);
            if (selected.type == AssetType::Descriptor)
            {
                if (const auto it = runtimeIndex->linksByDescriptorPath.find(selectedKey);
                    it != runtimeIndex->linksByDescriptorPath.end())
                {
                    app.descriptorUiDescriptorMatches = it->second;
                    finalizeDescriptorMatchCounts();
                    return;
                }
            }
            else if (selected.type == AssetType::Grp)
            {
                if (const auto it = runtimeIndex->linksByGrpPath.find(selectedKey);
                    it != runtimeIndex->linksByGrpPath.end())
                {
                    app.descriptorUiGrpMatches = it->second;
                    return;
                }
            }
            else if (selected.type == AssetType::Dxp)
            {
                if (const auto it = runtimeIndex->linksByDxpPath.find(selectedKey);
                    it != runtimeIndex->linksByDxpPath.end())
                {
                    app.descriptorUiDxpMatches = it->second;
                    return;
                }
            }
        }

        const std::wstring selectedKey = AssetIndex::GlobalAssetIndex::MakePathKey(selected.path);
        for (size_t linkIndex = 0; linkIndex < links->size(); ++linkIndex)
        {
            const AssetIndex::DescriptorResourceLink& link = (*links)[linkIndex];
            if (selected.type == AssetType::Descriptor &&
                AssetIndex::GlobalAssetIndex::MakePathKey(link.descriptorPath) == selectedKey)
            {
                app.descriptorUiDescriptorMatches.push_back(linkIndex);
            }
            else if (selected.type == AssetType::Grp &&
                     link.matchedGrpPath &&
                     AssetIndex::GlobalAssetIndex::MakePathKey(*link.matchedGrpPath) == selectedKey)
            {
                app.descriptorUiGrpMatches.push_back(linkIndex);
            }
            else if (selected.type == AssetType::Dxp)
            {
                for (size_t textureIndex = 0; textureIndex < link.textureLinks.size(); ++textureIndex)
                {
                    const AssetIndex::DescriptorTextureLink& textureLink = link.textureLinks[textureIndex];
                    if (textureLink.matchedDxpPath &&
                        AssetIndex::GlobalAssetIndex::MakePathKey(*textureLink.matchedDxpPath) == selectedKey)
                    {
                        app.descriptorUiDxpMatches.push_back({ linkIndex, textureIndex });
                    }
                }
            }
        }

        finalizeDescriptorMatchCounts();
    }

    void DrawLeftPanel(AppState& app, const std::vector<ResourceRecord>& records)
    {
        (void)records;
        DrawPanelTitle(UiIconId::ContentBrowser, "Resource Filters");
        ImGui::Separator();
        bool filterChanged = false;
        filterChanged |= ImGui::Checkbox("VROMFS", &app.showVromfs);
        filterChanged |= ImGui::Checkbox("GRP", &app.showGrp);
        filterChanged |= ImGui::Checkbox("DXP", &app.showDxp);
        filterChanged |= ImGui::Checkbox("Descriptors", &app.showDescriptors);
        filterChanged |= ImGui::Checkbox("BLK", &app.showBlk);
        if (filterChanged)
        {
            SaveSettingsWithImGui(app);
        }

        const auto& counts = app.resourceUiCache.typeCounts;

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Discovered");
        if (ImGui::BeginTable(
                "DiscoveredCountsTable",
                2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableHeadersRow();

            const struct
            {
                const char* label;
                uint32_t count;
            } rows[] = {
                { "VROMFS", counts[0] },
                { "GRP", counts[1] },
                { "DXP", counts[2] },
                { "Descriptors", counts[3] },
                { "BLK", counts[4] },
            };
            for (const auto& row : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(row.label);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", row.count);
            }
            ImGui::EndTable();
        }

        if (!app.validation.IsValid())
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Game folder required.");
            DrawWrappedDisabledText("Please configure and validate your game folder in Settings.");
        }
    }

    void DrawContentBrowser(AppState& app, const std::shared_ptr<const std::vector<ResourceRecord>>& recordsPtr)
    {
        DrawPanelTitle(UiIconId::ContentBrowser, "Content Browser");

        const UiIcon* rebuildIcon = GetUiIcon(UiIconId::Rebuild);
        const bool rebuildHasIcon = rebuildIcon != nullptr && rebuildIcon->view != nullptr;
        const ImVec2 rebuildButtonSize = CalcIconButtonSize("Rebuild Resources", rebuildHasIcon);
        
        const UiIcon* patreonIcon = GetUiIcon(UiIconId::Patreon);
        const bool patreonHasIcon = patreonIcon != nullptr && patreonIcon->view != nullptr;
        const ImVec2 patreonButtonSize = CalcIconButtonSize("Donate", patreonHasIcon);
        
        const float buttonsWidth = rebuildButtonSize.x + ImGui::GetStyle().ItemSpacing.x + patreonButtonSize.x;
        
        ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionMax().x - buttonsWidth));
        
        if (DrawIconTextButton("##donate", UiIconId::Patreon, "Donate"))
        {
            Platform::OpenUrl(L"https://www.patreon.com/AldertLake");
        }
        
        ImGui::SameLine();
        ImGui::BeginDisabled(!app.validation.IsValid() || app.scan.running.load());
        if (DrawIconTextButton("##rebuild_resources", UiIconId::Rebuild, "Rebuild Resources"))
        {
            App::StartScan(app);
        }
        ImGui::EndDisabled();

        ImGui::Separator();

        ImGui::SetNextItemWidth(-1.0f);
        bool searchChanged = ImGui::InputTextWithHint("##Search", "Search discovered packs and descriptors", app.searchBuffer.data(), app.searchBuffer.size());
        const std::string search = AssetIndex::ToLowerAscii(std::string(app.searchBuffer.data()));

        static bool lastShowVromfs = app.showVromfs;
        static bool lastShowGrp = app.showGrp;
        static bool lastShowDxp = app.showDxp;
        static bool lastShowDescriptors = app.showDescriptors;
        static bool lastShowBlk = app.showBlk;

        bool filtersChanged = (lastShowVromfs != app.showVromfs || lastShowGrp != app.showGrp || lastShowDxp != app.showDxp || lastShowDescriptors != app.showDescriptors || lastShowBlk != app.showBlk);

        const std::vector<ResourceRecord>& records = *recordsPtr;

        if (recordsPtr != app.lastRecords || searchChanged || filtersChanged || app.lastSearch != search)
        {
            app.lastRecords = recordsPtr;
            app.lastSearch = search;
            lastShowVromfs = app.showVromfs;
            lastShowGrp = app.showGrp;
            lastShowDxp = app.showDxp;
            lastShowDescriptors = app.showDescriptors;
            lastShowBlk = app.showBlk;

            app.filteredIndices.clear();
            const bool useRowCache = app.resourceUiCache.sourceRecords == recordsPtr &&
                                     app.resourceUiCache.rows.size() == records.size();
            for (size_t i = 0; i < records.size(); ++i)
            {
                if (!RecordPassesTypeFilter(app, records[i].type))
                {
                    continue;
                }
                const bool matchesSearch = search.empty() ||
                    (useRowCache
                        ? (app.resourceUiCache.rows[i].searchHaystackLower.find(search) != std::string::npos)
                        : RecordPassesSearch(records[i], search));
                if (matchesSearch)
                {
                    app.filteredIndices.push_back(i);
                }
            }
        }

        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("ResourceRecords", 4, flags, ImVec2(0.0f, -24.0f)))
        {
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.40f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.60f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 88.0f);
            ImGui::TableHeadersRow();

            const bool useRowCache = app.resourceUiCache.sourceRecords == recordsPtr &&
                                     app.resourceUiCache.rows.size() == records.size();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(app.filteredIndices.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    ImGui::PushID(row);
                    const size_t recordIndex = app.filteredIndices[static_cast<size_t>(row)];
                    const ResourceRecord& record = records[recordIndex];
                    const std::wstring nativePath = useRowCache
                        ? app.resourceUiCache.rows[recordIndex].nativePath
                        : record.path.wstring();
                    const std::string& nameText = useRowCache
                        ? app.resourceUiCache.rows[recordIndex].fileNameUtf8
                        : Platform::PathToUtf8(record.path.filename());
                    const std::string& pathText = useRowCache
                        ? app.resourceUiCache.rows[recordIndex].parentDirUtf8
                        : Platform::PathToUtf8(record.path.parent_path());
                    const std::string& sizeText = useRowCache
                        ? app.resourceUiCache.rows[recordIndex].sizeFormatted
                        : FormatBytes(record.sizeBytes);
                    const bool selected = nativePath == app.selectedPath;

                    float col2Width = 0.0f;
                    if (ImGuiTable* table = ImGui::GetCurrentTable())
                    {
                        if (2 < table->ColumnsCount)
                        {
                            col2Width = table->Columns[2].WorkMaxX - table->Columns[2].WorkMinX;
                        }
                    }
                    float cellPaddingY = ImGui::GetStyle().CellPadding.y;
                    float rowHeight = ImGui::GetTextLineHeight() + cellPaddingY * 2.0f;
                    if (col2Width > 0.0f)
                    {
                        ImVec2 pathTextSize = ImGui::CalcTextSize(pathText.c_str(), nullptr, false, col2Width);
                        rowHeight = std::max(rowHeight, pathTextSize.y + cellPaddingY * 2.0f);
                    }

                    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                    
                    ImGui::TableSetColumnIndex(0);
                    ImVec2 cursorReset = ImGui::GetCursorPos();
                    if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight - cellPaddingY * 2.0f)))
                    {
                        app.selectedPath = nativePath;
                    }
                    ImGui::SetCursorPos(cursorReset);
                    ImGui::TextUnformatted(AssetIndex::AssetTypeName(record.type));

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(nameText.c_str());

                    ImGui::TableSetColumnIndex(2);
                    DrawWrappedText(pathText);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(sizeText.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }

        ImGui::Text("%zu matching records.", app.filteredIndices.size());
    }

    std::optional<ResourceRecord> FindSelectedRecord(const AppState& app, const std::vector<ResourceRecord>& records)
    {
        if (app.selectedPath.empty())
        {
            return std::nullopt;
        }

        if (app.resourceUiCache.sourceRecords.get() == &records)
        {
            const auto it = app.resourceUiCache.indexByNativePath.find(app.selectedPath);
            if (it != app.resourceUiCache.indexByNativePath.end() && it->second < records.size())
            {
                return records[it->second];
            }
            return std::nullopt;
        }

        for (const ResourceRecord& record : records)
        {
            if (record.path.wstring() == app.selectedPath)
            {
                return record;
            }
        }

        return std::nullopt;
    }

    void DrawBlkPreview(AppState& app, const ResourceRecord& selected)
    {
        const bool isLoading = App::IsBlkPreviewLoading(app, selected.path);
        const std::shared_ptr<const App::BlkPreview> preview = App::SnapshotBlkPreview(app, selected.path);
        const bool loaded = preview && preview->loaded;

        ImGui::TextUnformatted("BLK Preview");
        ImGui::BeginDisabled(isLoading || loaded);
        if (ImGui::Button("Load BLK Preview", ImVec2(-1.0f, 0.0f)))
        {
            App::RequestBlkPreview(app, selected.path);
        }
        ImGui::EndDisabled();

        if (isLoading)
        {
            ImGui::TextColored(ImVec4(0.72f, 0.84f, 1.0f, 1.0f), "Loading BLK preview...");
            return;
        }

        if (!loaded)
        {
            ImGui::TextWrapped("Click to parse a small text BLK preview asynchronously. Binary/packed BLK variants are not decoded.");
            return;
        }

        if (!preview->error.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.49f, 0.42f, 1.0f), "Parse Error");
            ImGui::TextWrapped("%s", preview->error.c_str());
            return;
        }

        ImGui::Text("Properties: %zu", preview->propertyCount);
        ImGui::SameLine();
        ImGui::Text("Blocks: %zu", preview->blockCount);

        if (ImGui::BeginTable("BlkPreviewLines", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 180.0f)))
        {
            ImGui::TableSetupColumn("Top-level and nested entries", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(preview->lines.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(preview->lines[static_cast<size_t>(row)].c_str());
                }
            }

            ImGui::EndTable();
        }
    }

    std::string OptionalStringValue(const std::optional<std::string>& value)
    {
        if (!value)
        {
            return "Missing";
        }
        return *value;
    }

    void DrawBlkResourceLinks(AppState& app, const ResourceRecord& selected)
    {
        const std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> links = App::SnapshotBlkResourceLinks(app);
        const bool isLoading = App::IsBlkResourceLinksLoading(app);

        ImGui::TextUnformatted("BLK Resource Links");
        ImGui::BeginDisabled(isLoading || !app.scan.completed.load());
        if (ImGui::Button("Refresh Links##blk", ImVec2(110.0f, 0.0f)))
        {
            App::RequestBlkResourceLinks(app, App::SnapshotRecords(app.scan));
        }
        ImGui::EndDisabled();

        if (isLoading)
        {
            ImGui::TextColored(ImVec4(0.72f, 0.84f, 1.0f, 1.0f), "Building BLK links...");
            return;
        }

        if (!links)
        {
            ImGui::TextWrapped("Click Refresh Links to parse all BLK files and index their referenced assets.");
            return;
        }

        const std::wstring selectedNativePath = selected.path.wstring();
        if (app.blkUiLinks != links || app.blkUiSelectedPath != selectedNativePath)
        {
            app.blkUiLinks = links;
            app.blkUiSelectedPath = selectedNativePath;
            app.blkUiMatchedIndices.clear();
            for (size_t i = 0; i < links->size(); ++i)
            {
                if ((*links)[i].blkPath == selected.path)
                {
                    app.blkUiMatchedIndices.push_back(i);
                }
            }
        }

        if (app.blkUiMatchedIndices.empty())
        {
            ImGui::TextWrapped("No resource links found in this BLK.");
            return;
        }

        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("BlkLinksTable", 4, flags, ImVec2(0.0f, 200.0f)))
        {
            ImGui::TableSetupColumn("Resource Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("Expected Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Matched Path", ImGuiTableColumnFlags_WidthStretch, 0.50f);
            ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(app.blkUiMatchedIndices.size()));
            while (clipper.Step())
            {
                for (int rowIdx = clipper.DisplayStart; rowIdx < clipper.DisplayEnd; ++rowIdx)
                {
                    const AssetIndex::BlkResourceLink& row = (*links)[app.blkUiMatchedIndices[static_cast<size_t>(rowIdx)]];
                    ImGui::PushID(rowIdx);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(row.resourceName.c_str());
                    if (ImGui::IsItemHovered())
                    {
                        DrawWrappedTooltip(row.resourceName);
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(AssetIndex::AssetTypeName(row.expectedType));

                    ImGui::TableSetColumnIndex(2);
                    if (row.matchedPath)
                    {
                        const std::string matchedUtf8 = Platform::PathToUtf8(*row.matchedPath);
                        ImGui::TextUnformatted(matchedUtf8.c_str());
                        if (ImGui::IsItemHovered())
                        {
                            DrawWrappedTooltip(matchedUtf8);
                        }
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.49f, 0.42f, 1.0f), "Unresolved");
                    }

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%u", row.line);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    void DrawDescriptorResourceLinks(AppState& app, const ResourceRecord& selected)
    {
        const std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>> links = App::SnapshotDescriptorResourceLinks(app);
        const bool isLoading = App::IsDescriptorResourceLinksLoading(app);
        const bool hasGlobalLinks = App::HasGlobalDescriptorResourceLinks(app);

        ImGui::TextUnformatted("Descriptor Resource Links");
        ImGui::BeginDisabled(isLoading || !app.scan.completed.load() || hasGlobalLinks);
        if (ImGui::Button("Build Selected Links##desc", ImVec2(160.0f, 0.0f)))
        {
            App::RequestDescriptorResourceLinksForSelected(app, App::SnapshotRecords(app.scan), selected);
        }
        if (hasGlobalLinks && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            DrawWrappedTooltip("Descriptor links are already built globally.");
        }
        ImGui::EndDisabled();

        if (isLoading)
        {
            ImGui::TextColored(ImVec4(0.72f, 0.84f, 1.0f, 1.0f), "Building descriptor relationships...");
            return;
        }

        if (!links)
        {
            ImGui::TextWrapped("Build selected links for this asset, or use Tools > Rebuild Descriptor Relationships for a full global cache.");
            return;
        }

        RebuildDescriptorUiMatches(app, links, selected);

        if (selected.type == AssetType::Descriptor)
        {
            if (app.descriptorUiDescriptorMatches.empty())
            {
                ImGui::TextWrapped("No relationships found for this descriptor.");
                return;
            }

            ImGui::Text("Resources in this descriptor: %zu", app.descriptorUiDescriptorMatches.size());
            ImGui::Text("Resolved GRP matches: %zu", app.descriptorUiResolvedGrpCount);

            ImGuiTableFlags resourceTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
            if (ImGui::BeginTable("DescriptorMatchesTable", 6, resourceTableFlags, ImVec2(0.0f, 210.0f)))
            {
                ImGui::TableSetupColumn("Resource Name", ImGuiTableColumnFlags_WidthStretch, 0.28f);
                ImGui::TableSetupColumn("Resource ID", ImGuiTableColumnFlags_WidthFixed, 88.0f);
                ImGui::TableSetupColumn("GRP Entry", ImGuiTableColumnFlags_WidthStretch, 0.28f);
                ImGui::TableSetupColumn("GRP Class", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Textures", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableHeadersRow();

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(app.descriptorUiDescriptorMatches.size()));
                while (clipper.Step())
                {
                    for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
                    {
                        const AssetIndex::DescriptorResourceLink& link = (*links)[app.descriptorUiDescriptorMatches[static_cast<size_t>(rowIndex)]];
                        const size_t resolvedTextureCount = static_cast<size_t>(std::count_if(
                            link.textureLinks.begin(),
                            link.textureLinks.end(),
                            [](const auto& texLink) { return texLink.matchedDxpPath.has_value(); }));

                        ImGui::PushID(rowIndex);
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(link.resourceName.c_str());
                        if (ImGui::IsItemHovered())
                        {
                            DrawWrappedTooltip(link.resourceName);
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("0x%X", link.resId);

                        ImGui::TableSetColumnIndex(2);
                        if (link.matchedGrpPath)
                        {
                            const char* entryLabel = link.matchedGrpEntryName
                                ? link.matchedGrpEntryName->c_str()
                                : "Root";
                            ImGui::TextUnformatted(entryLabel);
                            if (ImGui::IsItemHovered())
                            {
                                DrawWrappedTooltip(
                                    Platform::PathToUtf8(*link.matchedGrpPath));
                            }
                        }
                        else
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.49f, 0.42f, 1.0f), "Unresolved");
                        }

                        ImGui::TableSetColumnIndex(3);
                        if (!link.matchedGrpClassName.empty())
                        {
                            ImGui::TextUnformatted(link.matchedGrpClassName.c_str());
                        }
                        else if (link.matchedGrpPath)
                        {
                            ImGui::TextDisabled("Unknown");
                        }
                        else
                        {
                            ImGui::TextDisabled("-");
                        }

                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text("%zu/%zu", resolvedTextureCount, link.textureLinks.size());
                        if (ImGui::IsItemHovered())
                        {
                            DrawWrappedTooltip(
                                std::to_string(resolvedTextureCount) +
                                " resolved texture pack matches out of " +
                                std::to_string(link.textureLinks.size()) +
                                " descriptor texture references.");
                        }

                        ImGui::TableSetColumnIndex(5);
                        const bool hasMatchedGrpEntry = link.matchedGrpPath.has_value() && link.matchedGrpEntryName.has_value();
                        const bool canExportUnifiedModel = hasMatchedGrpEntry && (link.matchedGrpEntryCanExportStaticModel || link.matchedGrpEntryCanExportSkinnedModel);
                        ImGui::BeginDisabled(!canExportUnifiedModel);
                        if (ImGui::Button("Export Model"))
                        {
                            if (link.matchedGrpEntryCanExportSkinnedModel)
                            {
                                App::OpenDynModelExportModalByName(app, *link.matchedGrpPath, *link.matchedGrpEntryName);
                            }
                            else
                            {
                                App::RequestModelExportByName(app, *link.matchedGrpPath, *link.matchedGrpEntryName);
                            }
                        }
                        if (!canExportUnifiedModel && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            if (!link.matchedGrpPath)
                            {
                                DrawWrappedTooltip(
                                    "This descriptor resource is not matched to any GRP entry yet.");
                            }
                            else if (!link.matchedGrpEntryName)
                            {
                                DrawWrappedTooltip(
                                    "The matched GRP entry name is missing.");
                            }
                            else
                            {
                                std::string tooltip = AssetIndex::GetUnsupportedExportTooltip(link.matchedGrpClassId, link.matchedGrpClassName);
                                DrawWrappedTooltip(tooltip);
                            }
                        }
                        ImGui::EndDisabled();
                        ImGui::PopID();
                    }
                }

                ImGui::EndTable();
            }

            const AssetIndex::DescriptorResourceLink& primaryLink = (*links)[app.descriptorUiDescriptorMatches.front()];
            DrawWrappedText(
                "Primary resource preview: " + primaryLink.resourceName);
            ImGui::Text("Textures: %zu (Resolved: %zu)", primaryLink.textureLinks.size(),
                std::count_if(primaryLink.textureLinks.begin(), primaryLink.textureLinks.end(), [](const auto& t) { return t.matchedDxpPath.has_value(); }));
            ImGui::Text("Materials: %zu", primaryLink.materialNames.size());

            if (!primaryLink.textureLinks.empty())
            {
                ImGui::TextUnformatted("Texture Dependencies:");
                ImGuiTableFlags tblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
                if (ImGui::BeginTable("DescTexTable", 4, tblFlags, ImVec2(0.0f, 140.0f)))
                {
                    ImGui::TableSetupColumn("Idx", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                    ImGui::TableSetupColumn("Texture Name", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                    ImGui::TableSetupColumn("Matched DXP", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                    ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthStretch, 0.15f);
                    ImGui::TableHeadersRow();

                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(primaryLink.textureLinks.size()));
                    while (clipper.Step())
                    {
                        for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
                        {
                            const auto& texLink = primaryLink.textureLinks[static_cast<size_t>(rowIndex)];
                            ImGui::PushID(rowIndex);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("%u", texLink.textureIndex);

                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(texLink.textureName.c_str());
                            if (ImGui::IsItemHovered())
                            {
                                DrawWrappedTooltip(texLink.textureName);
                            }

                            ImGui::TableSetColumnIndex(2);
                            if (texLink.matchedDxpPath)
                            {
                                const std::string dxpName = Platform::PathToUtf8(texLink.matchedDxpPath->filename());
                                ImGui::TextUnformatted(dxpName.c_str());
                                if (ImGui::IsItemHovered())
                                {
                                    DrawWrappedTooltip(
                                        Platform::PathToUtf8(
                                            *texLink.matchedDxpPath));
                                }
                            }
                            else
                            {
                                ImGui::TextColored(ImVec4(1.0f, 0.49f, 0.42f, 1.0f), "Unresolved DXP");
                            }

                            ImGui::TableSetColumnIndex(3);
                            if (texLink.matchedDxpEntryName)
                            {
                                ImGui::TextUnformatted(texLink.matchedDxpEntryName->c_str());
                                if (ImGui::IsItemHovered())
                                {
                                    DrawWrappedTooltip(*texLink.matchedDxpEntryName);
                                }
                            }
                            else
                            {
                                ImGui::TextDisabled("-");
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
        else if (selected.type == AssetType::Grp)
        {
            ImGui::Text("Descriptor resources resolving to this GRP: %zu", app.descriptorUiGrpMatches.size());
            if (!app.descriptorUiGrpMatches.empty())
            {
                ImGuiTableFlags tblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
                if (ImGui::BeginTable("GrpMatchesTable", 3, tblFlags, ImVec2(0.0f, 150.0f)))
                {
                    ImGui::TableSetupColumn("Resource Name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
                    ImGui::TableSetupColumn("GRP Entry", ImGuiTableColumnFlags_WidthStretch, 0.3f);
                    ImGui::TableSetupColumn("Descriptor File", ImGuiTableColumnFlags_WidthStretch, 0.3f);
                    ImGui::TableHeadersRow();

                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(app.descriptorUiGrpMatches.size()));
                    while (clipper.Step())
                    {
                        for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
                        {
                            const size_t linkIndex = app.descriptorUiGrpMatches[static_cast<size_t>(rowIndex)];
                            const AssetIndex::DescriptorResourceLink& match = (*links)[linkIndex];
                            ImGui::PushID(rowIndex);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(match.resourceName.c_str());
                            if (ImGui::IsItemHovered())
                            {
                                DrawWrappedTooltip(match.resourceName);
                            }

                            ImGui::TableSetColumnIndex(1);
                            const char* entryName = match.matchedGrpEntryName
                                ? match.matchedGrpEntryName->c_str()
                                : "Root";
                            ImGui::TextUnformatted(entryName);
                            if (ImGui::IsItemHovered())
                            {
                                DrawWrappedTooltip(entryName);
                            }

                            ImGui::TableSetColumnIndex(2);
                            const std::string descName = Platform::PathToUtf8(match.descriptorPath.filename());
                            ImGui::TextUnformatted(descName.c_str());
                            if (ImGui::IsItemHovered())
                            {
                                DrawWrappedTooltip(
                                    Platform::PathToUtf8(match.descriptorPath));
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
        else if (selected.type == AssetType::Dxp)
        {
            ImGui::Text("Descriptor textures resolving to this DXP: %zu", app.descriptorUiDxpMatches.size());
            if (!app.descriptorUiDxpMatches.empty())
            {
                ImGuiTableFlags tblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
                if (ImGui::BeginTable("DxpMatchesTable", 3, tblFlags, ImVec2(0.0f, 150.0f)))
                {
                    ImGui::TableSetupColumn("Texture Entry", ImGuiTableColumnFlags_WidthStretch, 0.4f);
                    ImGui::TableSetupColumn("Resource Name", ImGuiTableColumnFlags_WidthStretch, 0.3f);
                    ImGui::TableSetupColumn("Descriptor File", ImGuiTableColumnFlags_WidthStretch, 0.3f);
                    ImGui::TableHeadersRow();

                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(app.descriptorUiDxpMatches.size()));
                    while (clipper.Step())
                    {
                        for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex)
                        {
                            const auto& matchIndex = app.descriptorUiDxpMatches[static_cast<size_t>(rowIndex)];
                            const AssetIndex::DescriptorResourceLink& match = (*links)[matchIndex.first];
                            const AssetIndex::DescriptorTextureLink& textureLink = match.textureLinks[matchIndex.second];
                            ImGui::PushID(rowIndex);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            const char* texEntryName = textureLink.matchedDxpEntryName
                                ? textureLink.matchedDxpEntryName->c_str()
                                : "Root";
                            ImGui::TextUnformatted(texEntryName);
                            if (ImGui::IsItemHovered())
                            {
                                DrawWrappedTooltip(texEntryName);
                            }

                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(match.resourceName.c_str());
                            if (ImGui::IsItemHovered())
                            {
                                DrawWrappedTooltip(match.resourceName);
                            }

                            ImGui::TableSetColumnIndex(2);
                            const std::string descName = Platform::PathToUtf8(match.descriptorPath.filename());
                            ImGui::TextUnformatted(descName.c_str());
                            if (ImGui::IsItemHovered())
                            {
                                DrawWrappedTooltip(
                                    Platform::PathToUtf8(match.descriptorPath));
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    void DrawInspector(AppState& app, const std::vector<ResourceRecord>& records)
    {
        DrawPanelTitle(UiIconId::Inspect, "Inspector");
        ImGui::Separator();

        const std::optional<ResourceRecord> selected = FindSelectedRecord(app, records);
        App::TexturePreviewSnapshot texturePreview = App::SnapshotTexturePreview(app);

        if (!selected)
        {
            ImGui::TextWrapped("Select a discovered resource pack, descriptor, or BLK file to inspect its metadata.");
            return;
        }

        ImGui::Text("Type: %s", AssetIndex::AssetTypeName(selected->type));
        DrawWrappedText(
            "Name: " + Platform::PathToUtf8(selected->path.filename()));
        ImGui::Text("Size: %s", FormatBytes(selected->sizeBytes).c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("Full Path");
        ImGui::TextWrapped("%s", Platform::PathToUtf8(selected->path).c_str());
        ImGui::Separator();

        // Container sub-asset inspection (VROMFS, GRP, DXP, and Descriptor)
        const bool isContainer = (selected->type == AssetType::Vromfs || selected->type == AssetType::Dxp || selected->type == AssetType::Grp || selected->type == AssetType::Descriptor);
        if (isContainer)
        {
            const auto cachedContents = app.packContentsCache.Get(selected->path);

            if (!cachedContents)
            {
                // Not yet parsed — show load button
                const bool isParsing = app.packContentsCache.IsParsing(selected->path);
                ImGui::BeginDisabled(isParsing);
                if (ImGui::Button("Load Contents", ImVec2(-1.0f, 0.0f)))
                {
                    App::RequestPackContents(app, selected->path, selected->type);
                }
                ImGui::EndDisabled();

                if (isParsing)
                {
                    ImGui::TextColored(ImVec4(0.72f, 0.84f, 1.0f, 1.0f), "Parsing...");
                }
                else
                {
                    ImGui::TextWrapped("Click to parse this container and view its sub-assets.");
                }
            }
            else if (!cachedContents->error.empty())
            {
                // Parse failed
                ImGui::TextColored(ImVec4(1.0f, 0.49f, 0.42f, 1.0f), "Parse Error");
                ImGui::TextWrapped("%s", cachedContents->error.c_str());
            }
            else
            {
                // Parsed successfully — show sub-assets
                ImGui::Text("Container: %s", cachedContents->containerType.c_str());
                ImGui::Text("Sub-assets: %zu", cachedContents->entries.size());
                
                if (selected->type == AssetType::Dxp)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Batch Export All##dxp"))
                    {
                        App::RequestBatchTextureExport(app, selected->path, cachedContents);
                    }
                }
                ImGui::SetNextItemWidth(-1.0f);
                const bool subAssetSearchChanged = ImGui::InputTextWithHint(
                    "##SubAssetSearch",
                    "Search loaded sub-assets by name, class, texture, or material",
                    app.subAssetSearchBuffer.data(),
                    app.subAssetSearchBuffer.size());
                const std::string subAssetSearch = AssetIndex::ToLowerAscii(std::string(app.subAssetSearchBuffer.data()));

                if (app.subAssetFilterPath != selected->path.wstring() ||
                    app.lastSubAssetSearch != subAssetSearch ||
                    app.lastSubAssetCount != cachedContents->entries.size() ||
                    subAssetSearchChanged)
                {
                    app.subAssetFilterPath = selected->path.wstring();
                    app.lastSubAssetSearch = subAssetSearch;
                    app.lastSubAssetCount = cachedContents->entries.size();
                    app.filteredSubAssetIndices.clear();
                    app.filteredSubAssetIndices.reserve(cachedContents->entries.size());

                    for (size_t i = 0; i < cachedContents->entries.size(); ++i)
                    {
                        if (SubAssetPassesSearch(cachedContents->entries[i], subAssetSearch))
                        {
                            app.filteredSubAssetIndices.push_back(i);
                        }
                    }
                }

                ImGui::Text("%zu matching sub-assets.", app.filteredSubAssetIndices.size());
                ImGui::Separator();

                const bool isDxp = (selected->type == AssetType::Dxp);
                const bool isGrp = (selected->type == AssetType::Grp);
                const bool isDesc = (selected->type == AssetType::Descriptor);

                int columnCount = 3;
                if (isDxp || isGrp || isDesc) columnCount = 4;

                ImGuiTableFlags subFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

                const float tableHeight =
                    std::clamp(ImGui::GetContentRegionAvail().y * 0.58f, 260.0f, 460.0f);
                if (ImGui::BeginTable("SubAssets", columnCount, subFlags, ImVec2(0.0f, tableHeight)))
                {
                    if (isDesc)
                    {
                        ImGui::TableSetupColumn("Resource Name", ImGuiTableColumnFlags_WidthStretch, 0.60f);
                        ImGui::TableSetupColumn("Resource ID", ImGuiTableColumnFlags_WidthFixed, 88.0f);
                        ImGui::TableSetupColumn("Class Type", ImGuiTableColumnFlags_WidthStretch, 0.40f);
                        ImGui::TableSetupColumn("Deps", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    }
                    else
                    {
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.60f);
                        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        if (isDxp)
                        {
                            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch, 0.40f);
                        }
                        else if (isGrp)
                        {
                            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 0.40f);
                        }
                    }
                    ImGui::TableHeadersRow();

                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(app.filteredSubAssetIndices.size()));
                    while (clipper.Step())
                    {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                        {
                            ImGui::PushID(row);
                            const size_t entryIndex = app.filteredSubAssetIndices[static_cast<size_t>(row)];
                            const AssetIndex::SubAssetRecord& entry = cachedContents->entries[entryIndex];
                            float cellPaddingY = ImGui::GetStyle().CellPadding.y;
                            float rowHeight = ImGui::GetTextLineHeight() + cellPaddingY * 2.0f;
                            rowHeight = std::max(rowHeight, kRowIconSize + cellPaddingY * 2.0f);

                            if (isDesc)
                            {
                                float col2Width = 0.0f;
                                if (ImGuiTable* table = ImGui::GetCurrentTable())
                                {
                                    if (2 < table->ColumnsCount)
                                    {
                                        col2Width = table->Columns[2].WorkMaxX - table->Columns[2].WorkMinX;
                                    }
                                }
                                if (col2Width > 0.0f && !entry.formatInfo.empty())
                                {
                                    ImVec2 wrapSize = ImGui::CalcTextSize(entry.formatInfo.c_str(), nullptr, false, col2Width);
                                    rowHeight = std::max(rowHeight, wrapSize.y + cellPaddingY * 2.0f);
                                }
                            }
                            else
                            {
                                float col3Width = 0.0f;
                                if (ImGuiTable* table = ImGui::GetCurrentTable())
                                {
                                    if (3 < table->ColumnsCount)
                                    {
                                        col3Width = table->Columns[3].WorkMaxX - table->Columns[3].WorkMinX;
                                    }
                                }
                                if (col3Width > 0.0f && !entry.formatInfo.empty())
                                {
                                    ImVec2 wrapSize = ImGui::CalcTextSize(entry.formatInfo.c_str(), nullptr, false, col3Width);
                                    rowHeight = std::max(rowHeight, wrapSize.y + cellPaddingY * 2.0f);
                                }
                            }

                            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);

                            ImGui::TableSetColumnIndex(0);
                            ImVec2 cursorReset = ImGui::GetCursorPos();
                            bool isSelected = false;
                            if (!isDesc)
                            {
                                const bool isSelectableDxp = isDxp;
                                const bool isSelectableGrp = isGrp && (AssetIndex::CanExportGrpEntryAsStaticModel(entry) || AssetIndex::CanExportGrpEntryAsSkinnedModel(entry));
                                if (isSelectableDxp)
                                {
                                    isSelected = texturePreview.packPath == selected->path &&
                                                 texturePreview.entryName == entry.name &&
                                                 texturePreview.entryOffset == entry.offset;
                                }
                                else if (isSelectableGrp)
                                {
                                    isSelected = app.modelPreview.packPath == selected->path &&
                                                 app.modelPreview.entryName == entry.name;
                                }
                            }
                            
                            // Style Selectable to be fully transparent, we draw custom highlights with TableSetBgColor
                            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                            ImGui::Selectable("##subrow", isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight - cellPaddingY * 2.0f));
                            const bool isHovered = ImGui::IsItemHovered();
                            ImGui::PopStyleColor(3);

                            if (isSelected)
                            {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header));
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_Header));
                            }
                            else if (isHovered)
                            {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_HeaderHovered));
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_HeaderHovered));
                            }

                            const bool supportsPreview = isDxp || (isGrp && (AssetIndex::CanExportGrpEntryAsStaticModel(entry) || AssetIndex::CanExportGrpEntryAsSkinnedModel(entry)));
                            const bool exportSupported = AssetIndex::IsExportSupported(entry, selected->type);

                            // Right-click context menu (only if asset supports preview or export)
                            if ((isDxp || isGrp) && (supportsPreview || exportSupported) && ImGui::BeginPopupContextItem("subrow_context"))
                            {
                                ImGui::TextDisabled("Asset: %s", entry.name.c_str());
                                ImGui::Separator();
                                auto DrawContextMenuItem = [](UiIconId iconId, const char* label, bool enabled) -> bool {
                                    const UiIcon* icon = GetUiIcon(iconId);
                                    ImVec2 startPos = ImGui::GetCursorPos();
                                    float rowHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
                                    
                                    // Empty selectable spanning full width of the popup
                                    bool pressed = ImGui::Selectable(
                                        (std::string("##selectable_") + label).c_str(), 
                                        false, 
                                        (enabled ? 0 : ImGuiSelectableFlags_Disabled) | ImGuiSelectableFlags_AllowOverlap, 
                                        ImVec2(0, rowHeight));
                                    
                                    ImVec2 endPos = ImGui::GetCursorPos();
                                    
                                    // Draw content on top of selectable background
                                    if (icon && icon->view != nullptr)
                                    {
                                        if (!enabled) ImGui::BeginDisabled();
                                        ImGui::SetCursorPos(ImVec2(startPos.x + ImGui::GetStyle().FramePadding.x, startPos.y + (rowHeight - 16.0f) * 0.5f));
                                        ImGui::Image(reinterpret_cast<ImTextureID>(icon->view), ImVec2(16.0f, 16.0f));
                                        if (!enabled) ImGui::EndDisabled();
                                        
                                        if (!enabled) ImGui::BeginDisabled();
                                        ImGui::SetCursorPos(ImVec2(startPos.x + ImGui::GetStyle().FramePadding.x + 16.0f + 6.0f, startPos.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f));
                                        ImGui::TextUnformatted(label);
                                        if (!enabled) ImGui::EndDisabled();
                                    }
                                    else
                                    {
                                        if (!enabled) ImGui::BeginDisabled();
                                        ImGui::SetCursorPos(ImVec2(startPos.x + ImGui::GetStyle().FramePadding.x, startPos.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f));
                                        ImGui::TextUnformatted(label);
                                        if (!enabled) ImGui::EndDisabled();
                                    }
                                    
                                    ImGui::SetCursorPos(endPos);
                                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                                    return pressed;
                                };

                                if (DrawContextMenuItem(UiIconId::Preview, "Preview", supportsPreview))
                                {
                                    if (isDxp)
                                    {
                                        ClearTexturePreview();
                                        app.texturePreview.fitToWindow = true;
                                        app.texturePreview.zoom = 1.0f;
                                        app.texturePreview.windowOpen = true;
                                        App::RequestTexturePreview(app, selected->path, entry);
                                        texturePreview = App::SnapshotTexturePreview(app);
                                    }
                                    else if (isGrp)
                                    {
                                        App::RequestModelPreview(app, selected->path, entry);
                                    }
                                }

                                std::string exportLabel = "Export";
                                if (isDxp)
                                {
                                    exportLabel = "Export (as " + app.settings.textureExportFormat + ")";
                                }
                                else if (isGrp)
                                {
                                    exportLabel = "Export (as GLB)";
                                }

                                if (DrawContextMenuItem(UiIconId::Export, exportLabel.c_str(), exportSupported))
                                {
                                    const bool canExportSkinned = AssetIndex::CanExportGrpEntryAsSkinnedModel(entry);
                                    if (isDxp)
                                    {
                                        App::RequestTextureExport(app, selected->path, entry);
                                    }
                                    else if (isGrp)
                                    {
                                        if (canExportSkinned)
                                        {
                                            App::OpenDynModelExportModal(app, selected->path, entry);
                                        }
                                        else
                                        {
                                            App::RequestModelExport(app, selected->path, entry);
                                        }
                                    }
                                }

                                ImGui::EndPopup();
                            }

                            // Double click trigger to preview
                            if (isHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            {
                                if (supportsPreview)
                                {
                                    if (isDxp)
                                    {
                                        ClearTexturePreview();
                                        app.texturePreview.fitToWindow = true;
                                        app.texturePreview.zoom = 1.0f;
                                        app.texturePreview.windowOpen = true;
                                        App::RequestTexturePreview(app, selected->path, entry);
                                        texturePreview = App::SnapshotTexturePreview(app);
                                    }
                                    else if (isGrp)
                                    {
                                        App::RequestModelPreview(app, selected->path, entry);
                                    }
                                }
                            }

                            // Hover tooltip helper to show user they can right-click
                            if (isHovered && !ImGui::IsPopupOpen("subrow_context"))
                            {
                                if (supportsPreview || exportSupported)
                                {
                                    ImGui::BeginTooltip();
                                    ImGui::TextDisabled("Right-click to Preview/Export");
                                    ImGui::EndTooltip();
                                }
                            }
                            
                            ImGui::SetCursorPos(cursorReset);

                            if (isDesc)
                            {
                                // COLUMN 0: Resource Name
                                {
                                    float centering_offset_y = (rowHeight - cellPaddingY * 2.0f - std::max(16.0f, ImGui::GetTextLineHeight())) * 0.5f;
                                    if (centering_offset_y > 0.0f)
                                    {
                                        ImGui::SetCursorPosY(cursorReset.y + centering_offset_y);
                                    }
                                    DrawInlineIconLabel(GetSubAssetIconId(entry, selected->type), entry.name.c_str());
                                }

                                // COLUMN 1: Resource ID
                                ImGui::TableSetColumnIndex(1);
                                {
                                    float cellTopY = ImGui::GetCursorPosY();
                                    float centering_offset_y = (rowHeight - cellPaddingY * 2.0f - ImGui::GetTextLineHeight()) * 0.5f;
                                    if (centering_offset_y > 0.0f)
                                    {
                                        ImGui::SetCursorPosY(cellTopY + centering_offset_y);
                                    }
                                    ImGui::Text("0x%X", entry.resId);
                                }

                                // COLUMN 2: Class Type (Format info)
                                ImGui::TableSetColumnIndex(2);
                                {
                                    float cellTopY = ImGui::GetCursorPosY();
                                    float textHeight = ImGui::GetTextLineHeight();
                                    float col2Width = 0.0f;
                                    if (ImGuiTable* table = ImGui::GetCurrentTable())
                                    {
                                        if (2 < table->ColumnsCount)
                                        {
                                            col2Width = table->Columns[2].WorkMaxX - table->Columns[2].WorkMinX;
                                        }
                                    }
                                    if (col2Width > 0.0f && !entry.formatInfo.empty())
                                    {
                                        ImVec2 wrapSize = ImGui::CalcTextSize(entry.formatInfo.c_str(), nullptr, false, col2Width);
                                        textHeight = wrapSize.y;
                                    }
                                    float centering_offset_y = (rowHeight - cellPaddingY * 2.0f - textHeight) * 0.5f;
                                    if (centering_offset_y > 0.0f)
                                    {
                                        ImGui::SetCursorPosY(cellTopY + centering_offset_y);
                                    }
                                    if (!entry.formatInfo.empty())
                                    {
                                        DrawWrappedText(entry.formatInfo);
                                    }
                                    else
                                    {
                                        ImGui::TextDisabled("N/A");
                                    }
                                }

                                // COLUMN 3: Deps
                                ImGui::TableSetColumnIndex(3);
                                {
                                    float cellTopY = ImGui::GetCursorPosY();
                                    float centering_offset_y = (rowHeight - cellPaddingY * 2.0f - ImGui::GetTextLineHeight()) * 0.5f;
                                    if (centering_offset_y > 0.0f)
                                    {
                                        ImGui::SetCursorPosY(cellTopY + centering_offset_y);
                                    }
                                    const size_t depCount = entry.dependencyIds.size();
                                    const size_t texCount = entry.textureNames.size();
                                    const size_t matCount = entry.materialNames.size();
                                    ImGui::Text("%zu links", depCount + texCount + matCount);
                                    
                                    if (ImGui::IsItemHovered())
                                    {
                                        ImGui::BeginTooltip();
                                        ImGui::Text("Dependency Count: %zu", depCount);
                                        ImGui::Text("Textures: %zu", texCount);
                                        ImGui::Text("Materials: %zu", matCount);
                                        
                                        if (!entry.textureNames.empty())
                                        {
                                            ImGui::Separator();
                                            ImGui::TextUnformatted("Textures:");
                                            for (size_t t = 0; t < std::min(entry.textureNames.size(), size_t(8)); ++t)
                                            {
                                                DrawWrappedBulletText(entry.textureNames[t]);
                                            }
                                            if (entry.textureNames.size() > 8) ImGui::BulletText("...and %zu more", entry.textureNames.size() - 8);
                                        }
                                        
                                        if (!entry.materialNames.empty())
                                        {
                                            ImGui::Separator();
                                            ImGui::TextUnformatted("Materials:");
                                            for (size_t m = 0; m < std::min(entry.materialNames.size(), size_t(8)); ++m)
                                            {
                                                DrawWrappedBulletText(entry.materialNames[m]);
                                            }
                                            if (entry.materialNames.size() > 8) ImGui::BulletText("...and %zu more", entry.materialNames.size() - 8);
                                        }
                                        ImGui::EndTooltip();
                                    }
                                }
                            }
                            else
                            {
                                // COLUMN 0: Name
                                {
                                    float centering_offset_y = (rowHeight - cellPaddingY * 2.0f - std::max(16.0f, ImGui::GetTextLineHeight())) * 0.5f;
                                    if (centering_offset_y > 0.0f)
                                    {
                                        ImGui::SetCursorPosY(cursorReset.y + centering_offset_y);
                                    }
                                    DrawInlineIconLabel(GetSubAssetIconId(entry, selected->type), entry.name.c_str());
                                }

                                // COLUMN 1: Offset
                                ImGui::TableSetColumnIndex(1);
                                {
                                    float cellTopY = ImGui::GetCursorPosY();
                                    float centering_offset_y = (rowHeight - cellPaddingY * 2.0f - ImGui::GetTextLineHeight()) * 0.5f;
                                    if (centering_offset_y > 0.0f)
                                    {
                                        ImGui::SetCursorPosY(cellTopY + centering_offset_y);
                                    }
                                    ImGui::Text("0x%X", entry.offset);
                                }

                                // COLUMN 2: Size
                                ImGui::TableSetColumnIndex(2);
                                {
                                    float cellTopY = ImGui::GetCursorPosY();
                                    float centering_offset_y = (rowHeight - cellPaddingY * 2.0f - ImGui::GetTextLineHeight()) * 0.5f;
                                    if (centering_offset_y > 0.0f)
                                    {
                                        ImGui::SetCursorPosY(cellTopY + centering_offset_y);
                                    }
                                    ImGui::TextUnformatted(FormatBytes(entry.size).c_str());
                                }

                                // COLUMN 3: Format / Class
                                if (isDxp || isGrp)
                                {
                                    ImGui::TableSetColumnIndex(3);
                                    {
                                        float cellTopY = ImGui::GetCursorPosY();
                                        float textHeight = ImGui::GetTextLineHeight();
                                        float col3Width = 0.0f;
                                        if (ImGuiTable* table = ImGui::GetCurrentTable())
                                        {
                                            if (3 < table->ColumnsCount)
                                            {
                                                col3Width = table->Columns[3].WorkMaxX - table->Columns[3].WorkMinX;
                                            }
                                        }
                                        if (col3Width > 0.0f && !entry.formatInfo.empty())
                                        {
                                            ImVec2 wrapSize = ImGui::CalcTextSize(entry.formatInfo.c_str(), nullptr, false, col3Width);
                                            textHeight = wrapSize.y;
                                        }
                                        float centering_offset_y = (rowHeight - cellPaddingY * 2.0f - textHeight) * 0.5f;
                                        if (centering_offset_y > 0.0f)
                                        {
                                            ImGui::SetCursorPosY(cellTopY + centering_offset_y);
                                        }
                                        if (!entry.formatInfo.empty())
                                        {
                                            DrawWrappedText(entry.formatInfo);
                                        }
                                        else
                                        {
                                            ImGui::TextDisabled("N/A");
                                        }
                                    }
                                }
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }

            }

            ImGui::Separator();
        }

        if (selected->type == AssetType::Blk)
        {
            DrawBlkPreview(app, *selected);
            ImGui::Separator();
            DrawBlkResourceLinks(app, *selected);
            ImGui::Separator();
        }
        else if (selected->type == AssetType::Descriptor || selected->type == AssetType::Grp || selected->type == AssetType::Dxp)
        {
            DrawDescriptorResourceLinks(app, *selected);
            ImGui::Separator();
        }

        ImGui::TextUnformatted("Current Support");
        DrawWrappedBulletText("Discovery and metadata indexing.");
        if (isContainer)
        {
            DrawWrappedBulletText("Sub-asset listing (lazy loaded on demand).");
        }
        if (selected->type == AssetType::Blk)
        {
            DrawWrappedBulletText("Text BLK preview for resource relationship metadata.");
        }
        if (selected->type == AssetType::Dxp)
        {
            DrawWrappedBulletText(
                "DDS texture extraction with reversed-mip reordering, "
                "zlib/zstd/Oodle/LZMA decompression, and readable PNG/TGA export "
                "including DirectXTex fallback for BC6H/BC7-class textures.");
        }
        if (selected->type == AssetType::Grp)
        {
            DrawWrappedBulletText(
                "Binary static and dynamic model extraction to glTF 2.0 GLB / "
                "Wavefront OBJ.");
            DrawWrappedBulletText(
                "GeomNodeTree skeleton metadata parsing for GRP inspection and "
                "skinned mesh exports.");
        }
    }

    void DrawLogPanel(App::LogSystem& logger, const char* id = "LogsPanel", float height = 0.0f)
    {
        static const App::LogSystem* cachedLogger = nullptr;
        static uint64_t cachedVersion = UINT64_MAX;
        static std::vector<App::LogEntry> cachedLogs;

        const uint64_t currentVersion = logger.GetVersion();
        if (cachedLogger != &logger || cachedVersion != currentVersion)
        {
            cachedLogs = logger.GetSnapshot();
            cachedLogger = &logger;
            cachedVersion = currentVersion;
        }

        if (ImGui::Button("Clear"))
        {
            logger.ClearUI();
            cachedLogs.clear();
            cachedVersion = logger.GetVersion();
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy All"))
        {
            std::string allLogs;
            for (const auto& entry : cachedLogs)
            {
                allLogs += entry.formatted + "\n";
            }
            ImGui::SetClipboardText(allLogs.c_str());
        }

        ImGui::Separator();

        const bool childVisible = ImGui::BeginChild(id, ImVec2(0.0f, height), true, ImGuiWindowFlags_HorizontalScrollbar);
        if (childVisible)
        {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(cachedLogs.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const auto& entry = cachedLogs[static_cast<size_t>(row)];
                    ImGui::PushID(row);

                    ImGui::TextColored(LevelColor(entry.level), "%s", entry.formatted.c_str());

                    if (ImGui::BeginPopupContextItem("LogContext"))
                    {
                        if (ImGui::MenuItem("Copy Message"))
                        {
                            ImGui::SetClipboardText(entry.message.c_str());
                        }
                        if (ImGui::MenuItem("Copy Full Line"))
                        {
                            ImGui::SetClipboardText(entry.formatted.c_str());
                        }
                        ImGui::EndPopup();
                    }

                    ImGui::PopID();
                }
            }

            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            {
                ImGui::SetScrollHereY(1.0f);
            }
        }

        ImGui::EndChild();
    }

    void DrawJobAndLogPanel(AppState& app)
    {
        DrawPanelTitle(UiIconId::Log, "Jobs and Logs");
        ImGui::Separator();
        DrawWrappedText(
            "Scan: " + App::GetScanStatus(app.scan) +
            " | Visited: " + std::to_string(app.scan.filesVisited.load()) +
            " | Found: " + std::to_string(app.scan.recordsFound.load()));
        DrawWrappedText(
            "Cache: " + App::GetCacheStatus(app.scan) +
            " | BLK Links: " + App::GetBlkResourceLinkStatus(app) +
            " | Desc Links: " + App::GetDescriptorResourceLinkStatus(app));

        ImGui::Separator();

        DrawLogPanel(app.logger, "GlobalLogs");
    }

    void DrawProgressJobWindow(AppState& app)
    {
        App::ProgressJobSnapshot snapshot = App::SnapshotProgressJob(app);
        if (!snapshot.windowOpen && !snapshot.running)
        {
            return;
        }

        const std::string modalTitle =
            (snapshot.title.empty() ? "Operation Progress" : snapshot.title) + "###ProgressJobModal";
        if (!ImGui::IsPopupOpen(modalTitle.c_str()))
        {
            ImGui::OpenPopup(modalTitle.c_str());
        }

        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        const float targetWidth = std::min(640.0f, std::max(360.0f, mainViewport->WorkSize.x - 48.0f));
        const float maxHeight = std::max(240.0f, mainViewport->WorkSize.y - 48.0f);
        ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(targetWidth, 0.0f), ImVec2(targetWidth, maxHeight));

        bool open = true;
        bool* pOpen = snapshot.running ? nullptr : &open;
        if (ImGui::BeginPopupModal(
                modalTitle.c_str(),
                pOpen,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextWrapped("%s", snapshot.status.empty() ? "Working..." : snapshot.status.c_str());

            if (!snapshot.currentItem.empty())
            {
                DrawWrappedDisabledText("Current: " + snapshot.currentItem);
            }

            const float fraction = snapshot.total > 0
                ? std::clamp(static_cast<float>(snapshot.current) / static_cast<float>(snapshot.total), 0.0f, 1.0f)
                : (snapshot.running ? 0.0f : 1.0f);
            std::string overlay;
            if (snapshot.total > 0)
            {
                overlay = std::to_string(snapshot.current) + " / " + std::to_string(snapshot.total);
            }
            else if (snapshot.running)
            {
                overlay = "Working...";
            }
            else
            {
                overlay = "Done";
            }
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 20.0f), overlay.c_str());

            ImGui::Separator();
            const float logHeight = std::clamp(mainViewport->WorkSize.y * 0.30f, 120.0f, 220.0f);
            DrawLogPanel(app.logger, "ProgressJobLogs", logHeight);

            ImGui::Separator();
            if (snapshot.running)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.14f, 0.14f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.18f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.10f, 0.10f, 1.0f));
                ImGui::BeginDisabled(snapshot.cancelRequested);
                if (ImGui::Button(snapshot.cancelRequested ? "Canceling..." : "Cancel", ImVec2(130.0f, 0.0f)))
                {
                    if (App::IsDescriptorResourceLinksLoading(app))
                    {
                        App::RequestDescriptorResourceLinkCancel(app);
                    }
                    else
                    {
                        App::RequestProgressJobCancel(app);
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopStyleColor(3);
            }
            else
            {
                if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) || !open)
                {
                    std::lock_guard<std::mutex> lock(app.progressJob.mutex);
                    app.progressJob.windowOpen = false;
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::EndPopup();
        }
        else if (!open && !snapshot.running)
        {
            std::lock_guard<std::mutex> lock(app.progressJob.mutex);
            app.progressJob.windowOpen = false;
        }
    }

    void DrawRebuildProgressModal(AppState& app)
    {
        static bool wasRunning = false;
        bool isRunning = app.scan.running.load();

        if (isRunning && !wasRunning)
        {
            ImGui::OpenPopup("Rebuild Resources Progress");
        }
        wasRunning = isRunning;

        ImGui::SetNextWindowSize(ImVec2(400, 150), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Rebuild Resources Progress", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
        {
            std::string status = App::GetScanStatus(app.scan);
            DrawWrappedText(status);

            ImGui::Spacing();
            ImGui::Text("Files visited: %llu", app.scan.filesVisited.load());
            ImGui::Text("Records found: %llu", app.scan.recordsFound.load());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            bool cancelRequested = app.scan.cancelRequested.load();
            ImGui::BeginDisabled(cancelRequested);
            if (ImGui::Button(cancelRequested ? "Canceling..." : "Cancel", ImVec2(120, 0)))
            {
                app.scan.cancelRequested.store(true);
            }
            ImGui::EndDisabled();

            if (!isRunning)
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void DrawBlkRebuildProgressModal(AppState& app)
    {
        static bool wasRunning = false;
        bool isRunning = false;
        std::string status;
        {
            std::lock_guard<std::mutex> lock(app.blkLinks.mutex);
            isRunning = app.blkLinks.loading;
            status = app.blkLinks.status;
        }

        if (isRunning && !wasRunning)
        {
            ImGui::OpenPopup("Rebuild BLK Progress");
        }
        wasRunning = isRunning;

        ImGui::SetNextWindowSize(ImVec2(400, 120), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Rebuild BLK Progress", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextUnformatted("Rebuilding BLK Relationships...");
            ImGui::Spacing();
            ImGui::TextWrapped("%s", status.c_str());

            if (!isRunning)
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void DrawExportProgressModal(AppState& app)
    {
        bool isRunning = false;
        bool cancelRequested = false;
        std::string title;
        std::string status;
        std::string currentItem;
        std::string outputDir;
        uint64_t current = 0;
        uint64_t total = 0;

        {
            std::lock_guard<std::mutex> lock(app.exportProgress.mutex);
            isRunning = app.exportProgress.running.load();
            cancelRequested = app.exportProgress.cancelRequested.load();
            title = app.exportProgress.title;
            status = app.exportProgress.status;
            currentItem = app.exportProgress.currentItem;
            outputDir = app.exportProgress.outputDir;
            current = app.exportProgress.current;
            total = app.exportProgress.total;
        }

        static bool popupOpened = false;
        static size_t lastLogCount = 0;

        if (isRunning)
        {
            if (!popupOpened)
            {
                ImGui::OpenPopup("Export Progress");
                popupOpened = true;
                lastLogCount = 0;
            }
        }

        ImGui::SetNextWindowSize(ImVec2(520.0f, 350.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Export Progress", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse))
        {
            DrawWrappedColoredText(
                ImVec4(0.72f, 0.84f, 1.0f, 1.0f),
                title.empty() ? "Exporting Assets" : title);
            ImGui::Spacing();

            if (isRunning)
            {
                ImGui::TextWrapped("%s", status.empty() ? "Exporting..." : status.c_str());
                if (!currentItem.empty())
                {
                    DrawWrappedDisabledText("Current: " + currentItem);
                }
                else
                {
                    ImGui::Spacing();
                }
            }
            else
            {
                if (cancelRequested)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Export canceled.");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Export complete!");
                }
                ImGui::Spacing();
            }

            ImGui::Spacing();

            const float fraction = isRunning
                ? (total > 0 ? std::clamp(static_cast<float>(current) / static_cast<float>(total), 0.0f, 1.0f) : 0.0f)
                : (cancelRequested ? (total > 0 ? std::clamp(static_cast<float>(current) / static_cast<float>(total), 0.0f, 1.0f) : 0.0f) : 1.0f);

            std::string overlay;
            if (isRunning)
            {
                if (total > 0)
                {
                    overlay = std::to_string(current) + " / " + std::to_string(total);
                }
                else
                {
                    overlay = "Working...";
                }
            }
            else
            {
                if (cancelRequested)
                {
                    overlay = "Canceled (" + std::to_string(current) + " / " + std::to_string(total) + ")";
                }
                else
                {
                    overlay = "Success (" + std::to_string(total) + " / " + std::to_string(total) + ")";
                }
            }

            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 24.0f), overlay.c_str());

            ImGui::Spacing();
            ImGui::TextUnformatted("Export Log:");
            DrawLogPanel(app.logger, "ExportLogsChild", 120.0f);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (isRunning)
            {
                ImGui::BeginDisabled(cancelRequested);
                if (ImGui::Button(cancelRequested ? "Canceling..." : "Cancel Export", ImVec2(120.0f, 0.0f)))
                {
                    std::lock_guard<std::mutex> lock(app.exportProgress.mutex);
                    app.exportProgress.cancelRequested.store(true);
                }
                ImGui::EndDisabled();
            }
            else
            {
                if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
                {
                    ImGui::CloseCurrentPopup();
                }

                if (!outputDir.empty())
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Show in Explorer", ImVec2(140.0f, 0.0f)))
                    {
                        Platform::ShowPathInExplorer(outputDir);
                    }
                }
            }

            ImGui::EndPopup();
        }
        else
        {
            popupOpened = false;
        }
    }

    void DrawSettingsWindow(AppState& app)
    {
        if (!app.settingsWindowOpen)
        {
            return;
        }

        const std::string title = "Settings###SettingsWindow";
        if (!ImGui::IsPopupOpen(title.c_str()))
        {
            ImGui::OpenPopup(title.c_str());
        }

        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(880.0f, 560.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 460.0f), ImVec2(FLT_MAX, FLT_MAX));

        bool open = app.settingsWindowOpen;
        if (ImGui::BeginPopupModal(title.c_str(), &open, ImGuiWindowFlags_NoCollapse))
        {
            const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 10.0f;
            const ImGuiTableFlags layoutFlags =
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchProp;

            if (ImGui::BeginTable("SettingsLayout", 2, layoutFlags, ImVec2(0.0f, -footerHeight)))
            {
                ImGui::TableSetupColumn("Categories", ImGuiTableColumnFlags_WidthFixed, 195.0f);
                ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();

                static int selectedCategory = 0;

                ImGui::TableSetColumnIndex(0);
                if (ImGui::BeginChild("SettingsCategories", ImVec2(0.0f, 0.0f), false))
                {
                    if (DrawInlineIconSelectable(UiIconId::GameInstall, "Game Installation", selectedCategory == 0))
                    {
                        selectedCategory = 0;
                    }
                    ImGui::Spacing();
                    if (DrawInlineIconSelectable(UiIconId::Export, "Asset Export", selectedCategory == 1))
                    {
                        selectedCategory = 1;
                    }
                    ImGui::Spacing();
                    if (DrawInlineIconSelectable(UiIconId::Rebuild, "Performance & Cache", selectedCategory == 2))
                    {
                        selectedCategory = 2;
                    }
                    ImGui::Spacing();
                    if (DrawInlineIconSelectable(UiIconId::Log, "Storage & Paths", selectedCategory == 3))
                    {
                        selectedCategory = 3;
                    }
                }
                ImGui::EndChild();

                ImGui::TableSetColumnIndex(1);
                if (ImGui::BeginChild("SettingsContent", ImVec2(0.0f, 0.0f), false))
                {
                        if (selectedCategory == 0)
                        {
                            DrawPanelTitle(UiIconId::GameInstall, "Game Installation");
                            ImGui::Separator();
                            ImGui::Spacing();

                            ImGui::TextUnformatted("War Thunder Folder");
                            const float actionW = 76.0f;
                            const float spacingX = ImGui::GetStyle().ItemSpacing.x;
                            ImGui::SetNextItemWidth(-(actionW * 2.0f + spacingX * 2.0f));
                            ImGui::InputText("##GameRoot", app.gameRootBuffer.data(), app.gameRootBuffer.size());
                            ImGui::SameLine();
                            if (ImGui::Button("Browse##GameRoot", ImVec2(actionW, 0.0f)))
                            {
                                if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose War Thunder Folder", app.settings.gameRoot))
                                {
                                    App::SetGameRoot(app, *selected);
                                }
                            }
                            ImGui::SameLine();
                            ImGui::BeginDisabled(app.settings.gameRoot.empty());
                            if (ImGui::Button("Open##OpenGameRoot", ImVec2(actionW, 0.0f)))
                            {
                                Platform::ShowPathInExplorer(app.settings.gameRoot);
                            }
                            ImGui::EndDisabled();

                            ImGui::Spacing();
                            const bool scanRunning = app.scan.running.load();
                            ImGui::BeginDisabled(app.rootValidationInFlight.load());
                            if (ImGui::Button("Validate Folder", ImVec2(150.0f, 0.0f)))
                            {
                                App::ApplyGameRootBuffer(app);
                            }
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            ImGui::BeginDisabled(!app.validation.IsValid() || scanRunning);
                            if (ImGui::Button(scanRunning ? "Scanning..." : "Rescan Resources", ImVec2(150.0f, 0.0f)))
                            {
                                App::StartScan(app);
                            }
                            ImGui::EndDisabled();

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                            DrawValidationSummary(app.validation);
                        }
                        else if (selectedCategory == 1)
                        {
                            DrawPanelTitle(UiIconId::Export, "Asset Export");
                            ImGui::Separator();
                            ImGui::Spacing();

                            ImGui::TextUnformatted("Export Output Folder");
                            const float actionW = 76.0f;
                            const float spacingX = ImGui::GetStyle().ItemSpacing.x;
                            ImGui::SetNextItemWidth(-(actionW * 2.0f + spacingX * 2.0f));
                            ImGui::InputText("##ExportRoot", app.exportRootBuffer.data(), app.exportRootBuffer.size());
                            ImGui::SameLine();
                            if (ImGui::Button("Browse##ExportRoot", ImVec2(actionW, 0.0f)))
                            {
                                if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose Export Folder", app.settings.exportRoot))
                                {
                                    app.settings.exportRoot = *selected;
                                    App::SyncSettingsBuffers(app);
                                }
                            }
                            ImGui::SameLine();
                            ImGui::BeginDisabled(app.settings.exportRoot.empty());
                            if (ImGui::Button("Open##OpenExportRoot", ImVec2(actionW, 0.0f)))
                            {
                                Platform::ShowPathInExplorer(app.settings.exportRoot);
                            }
                            ImGui::EndDisabled();

                            ImGui::Spacing();
                            if (ImGui::CollapsingHeader("Standalone & Batch Texture Export", ImGuiTreeNodeFlags_DefaultOpen))
                            {
                                static const char* textureFormats[] = { "PNG", "DDS", "TGA" };
                                const std::string fmtLower = AssetIndex::ToLowerAscii(app.settings.textureExportFormat);
                                int textureFormatIndex = 0;
                                if (fmtLower == "dds") textureFormatIndex = 1;
                                else if (fmtLower == "tga") textureFormatIndex = 2;

                                ImGui::SetNextItemWidth(160.0f);
                                if (ImGui::Combo("Texture Format", &textureFormatIndex, textureFormats, IM_ARRAYSIZE(textureFormats)))
                                {
                                    app.settings.textureExportFormat = textureFormats[textureFormatIndex];
                                }
                                DrawWrappedDisabledText(
                                    "Applies to standalone and batch texture exports. "
                                    "GLB model exports always encode material textures as PNG.");

                                const bool isDdsFormat = (textureFormatIndex == 1);
                                ImGui::BeginDisabled(isDdsFormat);
                                ImGui::Checkbox("Fallback to DDS when PNG/TGA conversion is unavailable", &app.settings.preserveOriginalDDS);
                                ImGui::EndDisabled();

                                ImGui::Checkbox("Export JSON manifest metadata alongside assets", &app.settings.exportJsonManifest);
                            }

                            ImGui::Spacing();
                            if (ImGui::CollapsingHeader("3D Model (GLB) Export", ImGuiTreeNodeFlags_DefaultOpen))
                            {
                                int maxLods = std::clamp(app.settings.staticModelMaxLods, 1, 16);
                                ImGui::SetNextItemWidth(160.0f);
                                if (ImGui::SliderInt("Max RendInst LODs", &maxLods, 1, 16))
                                {
                                    app.settings.staticModelMaxLods = maxLods;
                                }
                                if (ImGui::IsItemHovered())
                                {
                                    DrawWrappedTooltip(
                                        "Sets the maximum number of Levels of Detail (LODs) to export for static models (RendInst).");
                                }

                                ImGui::Spacing();
                                ImGui::Checkbox("Export textures with models", &app.settings.exportTexturesWithModels);

                                ImGui::Indent(16.0f);
                                ImGui::BeginDisabled(!app.settings.exportTexturesWithModels);
                                ImGui::Checkbox("Embed textures inside GLB", &app.settings.embedTexturesInGlb);
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                {
                                    DrawWrappedTooltip(
                                        "Encodes PNG material textures directly into the GLB binary chunk instead of external files.");
                                }

                                ImGui::Checkbox(
                                    "Convert War Thunder packed _n to Normal + ORM",
                                    &app.settings.convertWarThunderPackedMaps);
                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                {
                                    DrawWrappedTooltip(
                                        "Enabled: reconstructs tangent-space Normal + glTF ORM textures from packed _n maps. "
                                        "Disabled: preserves native packed _n channels in the optional DAGOR_material_war_thunder_packed_n extension.");
                                }
                                ImGui::EndDisabled();
                                ImGui::Unindent(16.0f);
                            }
                        }
                        else if (selectedCategory == 2)
                        {
                            DrawPanelTitle(UiIconId::Rebuild, "Performance & Cache");
                            ImGui::Separator();
                            ImGui::Spacing();

                            if (ImGui::CollapsingHeader("Threading & Memory Cache", ImGuiTreeNodeFlags_DefaultOpen))
                            {
                                const int maxWorkerThreads = static_cast<int>(Jobs::JobSystem::MaxWorkerCount());
                                int workerThreadCount = static_cast<int>(
                                    std::clamp<size_t>(app.settings.workerThreadCount, 0u, static_cast<size_t>(maxWorkerThreads)));
                                ImGui::SetNextItemWidth(180.0f);
                                if (ImGui::SliderInt(
                                        "Worker Threads",
                                        &workerThreadCount,
                                        0,
                                        maxWorkerThreads,
                                        workerThreadCount == 0 ? "Auto" : "%d"))
                                {
                                    app.settings.workerThreadCount = static_cast<size_t>(workerThreadCount);
                                    app.backgroundJobs.SetWorkerCount(app.settings.workerThreadCount);
                                }
                                if (ImGui::IsItemHovered())
                                {
                                    DrawWrappedTooltip(
                                        "Background worker threads used for scanning, indexing, and exporting. "
                                        "Auto uses all available CPU cores.");
                                }
                                ImGui::SameLine();
                                const unsigned int hwCores = std::thread::hardware_concurrency();
                                ImGui::TextDisabled(
                                    "(Active: %zu / %u cores)",
                                    app.backgroundJobs.WorkerCount(),
                                    hwCores);

                                int maxLoadedPackContents = static_cast<int>(std::clamp<size_t>(app.settings.maxLoadedPackContents, 1u, 256u));
                                ImGui::SetNextItemWidth(180.0f);
                                if (ImGui::SliderInt("Max Loaded Packs", &maxLoadedPackContents, 1, 256))
                                {
                                    app.settings.maxLoadedPackContents = static_cast<size_t>(maxLoadedPackContents);
                                }
                                if (ImGui::IsItemHovered())
                                {
                                    DrawWrappedTooltip(
                                        "Maximum parsed containers kept in memory (capped by a 256 MB LRU budget).");
                                }

                                const size_t cachedPacks = app.packContentsCache.Size();
                                const size_t cachedBytes = app.packContentsCache.TotalEstimatedBytes();
                                ImGui::TextDisabled(
                                    "In memory: %zu container%s (%s / 256 MB budget)",
                                    cachedPacks,
                                    cachedPacks == 1 ? "" : "s",
                                    FormatBytes(cachedBytes).c_str());
                                ImGui::SameLine();
                                ImGui::BeginDisabled(cachedPacks == 0);
                                if (ImGui::SmallButton("Unload Cached Packs"))
                                {
                                    app.packContentsCache.Clear();
                                }
                                ImGui::EndDisabled();
                            }

                            ImGui::Spacing();
                            if (ImGui::CollapsingHeader("Persistent Disk Cache", ImGuiTreeNodeFlags_DefaultOpen))
                            {
                                if (ImGui::Checkbox("Enable persistent index & descriptor-link caches", &app.settings.indexCacheEnabled))
                                {
                                    if (app.settings.indexCacheEnabled)
                                    {
                                        App::LoadIndexCacheIntoApp(app);
                                    }
                                    else
                                    {
                                        App::SetCacheStatus(app.scan, "Disabled", false);
                                    }
                                }
                                if (ImGui::IsItemHovered())
                                {
                                    DrawWrappedTooltip(
                                        "Stores fast per-root DAEC binary caches (index.bin & links.bin) for sub-second startup.");
                                }

                                ImGui::TextDisabled("Status: %s", App::GetCacheStatus(app.scan).c_str());
                                ImGui::SameLine();
                                const bool busyWithCache = app.scan.running.load() ||
                                    App::IsBlkResourceLinksLoading(app) ||
                                    App::IsDescriptorResourceLinksLoading(app);
                                ImGui::BeginDisabled(busyWithCache);
                                if (ImGui::SmallButton("Clear Disk Caches"))
                                {
                                    App::InvalidateAndClearCache(app);
                                }
                                ImGui::EndDisabled();
                            }

                            ImGui::Spacing();
                            if (ImGui::CollapsingHeader("Runtime Capabilities", ImGuiTreeNodeFlags_DefaultOpen))
                            {
                                if (ImGui::BeginTable(
                                        "RuntimeCapsTable",
                                        2,
                                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                                {
                                    ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthFixed, 155.0f);
                                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);

                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);
                                    ImGui::TextDisabled("Oodle Codec");
                                    ImGui::TableSetColumnIndex(1);
                                    if (App::IsOodleAvailable(app))
                                    {
                                        ImGui::TextColored(ImVec4(0.45f, 0.86f, 0.56f, 1.0f), "Available (oo2core_6_win64.dll)");
                                    }
                                    else
                                    {
                                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.42f, 1.0f), "Unavailable (oo2core_6_win64.dll missing)");
                                    }

                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);
                                    ImGui::TextDisabled("Zstd / LZMA / Zlib");
                                    ImGui::TableSetColumnIndex(1);
                                    ImGui::TextUnformatted("Built-in");

                                    ModelRenderer* renderer = GetModelRenderer();
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);
                                    ImGui::TextDisabled("3D Viewport");
                                    ImGui::TableSetColumnIndex(1);
                                    if (renderer)
                                    {
                                        ImGui::Text(
                                            "DirectX 11 (%ux MSAA) · %s",
                                            renderer->GetMsaaSampleCount(),
                                            renderer->GetGPUName().c_str());
                                    }
                                    else
                                    {
                                        ImGui::TextDisabled("Unavailable");
                                    }

                                    ImGui::EndTable();
                                }
                            }
                        }
                        else if (selectedCategory == 3)
                        {
                            DrawPanelTitle(UiIconId::Log, "Storage & Paths");
                            ImGui::Separator();
                            ImGui::Spacing();

                            if (ImGui::BeginTable("StoragePathsTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                            {
                                ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 145.0f);
                                ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
                                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 118.0f);

                                auto DrawPathRow = [](const char* rowId, const char* label, const std::filesystem::path& path)
                                {
                                    const std::string utf8 = Platform::PathToUtf8(path);
                                    ImGui::PushID(rowId);
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);
                                    ImGui::TextDisabled("%s", label);
                                    ImGui::TableSetColumnIndex(1);
                                    ImGui::TextWrapped("%s", utf8.empty() ? "Not configured" : utf8.c_str());
                                    ImGui::TableSetColumnIndex(2);
                                    ImGui::BeginDisabled(utf8.empty());
                                    if (ImGui::SmallButton("Open"))
                                    {
                                        Platform::ShowPathInExplorer(path);
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("Copy"))
                                    {
                                        ImGui::SetClipboardText(utf8.c_str());
                                    }
                                    ImGui::EndDisabled();
                                    ImGui::PopID();
                                };

                                DrawPathRow("Settings", "Settings file", app.settingsPath);
                                DrawPathRow("Index", "Index cache", app.indexCachePath);
                                DrawPathRow("Desc", "Descriptor cache", app.descriptorLinksCachePath);
                                DrawPathRow("Export", "Export folder", app.settings.exportRoot);
                                DrawPathRow("Log", "Session log", app.logger.GetLogFilePath());

                                ImGui::EndTable();
                            }
                        }
                }
                ImGui::EndChild();

                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Reset Defaults", ImVec2(130.0f, 0.0f)))
            {
                const std::filesystem::path keepGameRoot = app.settings.gameRoot;
                const std::filesystem::path keepExportRoot = app.settings.exportRoot;
                app.settings = App::AppSettings();
                app.settings.gameRoot = keepGameRoot;
                app.settings.exportRoot = keepExportRoot;
                app.backgroundJobs.SetWorkerCount(app.settings.workerThreadCount);
                App::SyncSettingsBuffers(app);
                SaveSettingsWithImGui(app);
            }
            if (ImGui::IsItemHovered())
            {
                DrawWrappedTooltip("Resets export and performance options to defaults while keeping your Game and Export folders.");
            }

            const float saveBtnW = 126.0f;
            const float closeBtnW = 92.0f;
            const float footerSpacing = ImGui::GetStyle().ItemSpacing.x;
            const float rightFooterX = ImGui::GetWindowContentRegionMax().x - (saveBtnW + closeBtnW + footerSpacing);
            if (rightFooterX > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f)
            {
                ImGui::SameLine();
                ImGui::SetCursorPosX(rightFooterX);
            }
            else
            {
                ImGui::SameLine();
            }

            if (ImGui::Button("Close", ImVec2(closeBtnW, 0.0f)))
            {
                open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.78f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.53f, 0.88f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.38f, 0.68f, 1.0f));
            if (ImGui::Button("Save & Close", ImVec2(saveBtnW, 0.0f)))
            {
                open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);

            if (!open)
            {
                app.settings.exportRoot = Platform::Utf8ToPath(app.exportRootBuffer.data());
                const std::filesystem::path enteredGameRoot = Platform::Utf8ToPath(app.gameRootBuffer.data());
                if (enteredGameRoot != app.settings.gameRoot)
                {
                    App::SetGameRoot(app, enteredGameRoot);
                }
                else
                {
                    App::SyncSettingsBuffers(app);
                }
                SaveSettingsWithImGui(app);
            }

            ImGui::EndPopup();
        }

        app.settingsWindowOpen = open;
    }

    void DrawAboutWindow(AppState& app)
    {
        if (!app.aboutWindowOpen)
        {
            return;
        }

        const std::string aboutTitle = "About Dagor Assets Extractor###AboutModal";
        if (!ImGui::IsPopupOpen(aboutTitle.c_str()))
        {
            ImGui::OpenPopup(aboutTitle.c_str());
        }

        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        const float targetWidth = std::min(560.0f, std::max(320.0f, mainViewport->WorkSize.x - 48.0f));
        ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(targetWidth, 0.0f), ImVec2(targetWidth, FLT_MAX));
        bool open = app.aboutWindowOpen;
        if (ImGui::BeginPopupModal(aboutTitle.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse))
        {
            DrawPanelTitle(UiIconId::GameInstall, "Dagor Assets Extractor");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextWrapped("Extracts textures and models from War Thunder game files.");
            ImGui::Spacing();
            ImGui::TextWrapped("For educational purposes only. The author takes no responsibility for how this software is used.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (DrawIconTextButton("##about_github", UiIconId::Github, "Open Source on GitHub"))
            {
                Platform::OpenUrl(L"https://github.com/AldertLake/Dagor_Assets_Extractor");
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        app.aboutWindowOpen = open;
    }

    void DrawDynModelExportModal(AppState& app)
    {
        if (!app.pendingDynModelExport)
        {
            return;
        }

        App::PendingDynModelExport& pending = *app.pendingDynModelExport;
        if (pending.openRequested)
        {
            ImGui::OpenPopup("Export Dynamic Model");
            pending.openRequested = false;
        }

        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Export Dynamic Model", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        DrawWrappedText("Model: " + pending.entry.name);
        ImGui::Spacing();

        ImGui::Checkbox("Include textures", &pending.includeTextures);

        ImGui::Checkbox("Include skeleton / rig", &pending.includeSkeletonRig);
        if (ImGui::IsItemHovered())
        {
            DrawWrappedTooltip(
                "Exports GeomNodeTree bones and binds skinned meshes when a "
                "matching skeleton is found.");
        }

        if (!pending.includeSkeletonRig)
        {
            pending.includeAnimations = false;
        }

        ImGui::BeginDisabled(!pending.includeSkeletonRig);
        ImGui::Checkbox("Include animations", &pending.includeAnimations);
        const bool animationHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        ImGui::EndDisabled();
        if (animationHovered)
        {
            DrawWrappedTooltip(
                "Exports matched Anim2Data ACL tracks as glTF animation channels. "
                "Requires skeleton / rig export so curves can target bones.");
        }

        ImGui::Separator();

        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)))
        {
            app.pendingDynModelExport.reset();
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        ImGui::SameLine();
        if (ImGui::Button("Export", ImVec2(110.0f, 0.0f)))
        {
            const App::PendingDynModelExport confirmed = pending;
            app.pendingDynModelExport.reset();
            ImGui::CloseCurrentPopup();
            App::RequestModelExport(app, confirmed.packPath, confirmed.entry, confirmed.includeTextures, confirmed.includeSkeletonRig, confirmed.includeAnimations);
            ImGui::EndPopup();
            return;
        }

        ImGui::EndPopup();
    }

    void DrawFirstRunModal(AppState& app)
    {
        if (!app.firstRunPromptOpen)
        {
            return;
        }

        const char* modalTitle = "Select Game Installation###FirstRunModal";
        if (!ImGui::IsPopupOpen(modalTitle))
        {
            ImGui::OpenPopup(modalTitle);
        }

        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        const float targetWidth = std::min(620.0f, std::max(340.0f, mainViewport->WorkSize.x - 48.0f));
        ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(targetWidth, 0.0f), ImVec2(targetWidth, FLT_MAX));
        if (ImGui::BeginPopupModal(modalTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextWrapped("Choose the local War Thunder installation folder to start building the read-only resource index.");
            ImGui::Spacing();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Game Root");
            ImGui::SameLine();
            const float browseWidth = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseWidth - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::InputText(
                    "##FirstRunGameRoot",
                    app.gameRootBuffer.data(),
                    app.gameRootBuffer.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue))
            {
                App::ApplyGameRootBuffer(app);
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse"))
            {
                if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose War Thunder Folder", app.settings.gameRoot))
                {
                    App::SetGameRoot(app, *selected);
                }
            }

            const std::filesystem::path enteredRoot = Platform::Utf8ToPath(app.gameRootBuffer.data());
            const bool bufferMatchesCurrentRoot =
                Platform::CanonicalizePathUtf8(enteredRoot) == Platform::CanonicalizePathUtf8(app.settings.gameRoot);
            const bool validationInFlight = app.rootValidationInFlight.load();
            const bool canUseCurrentFolder = !validationInFlight && bufferMatchesCurrentRoot && app.validation.IsValid();

            if (validationInFlight)
            {
                ImGui::TextDisabled("Validating game folder...");
            }
            else
            {
                DrawValidationSummary(app.validation);
            }
            ImGui::Separator();

            ImGui::BeginDisabled(!canUseCurrentFolder);
            if (ImGui::Button("Use Folder", ImVec2(120.0f, 0.0f)))
            {
                SaveSettingsWithImGui(app);
                app.firstRunPromptOpen = false;
                // A completed scan automatically builds BLK relationships (descriptors stay manual).
                if (!app.scan.running.load() && !app.scan.completed.load())
                {
                    App::StartScan(app);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(validationInFlight);
            if (ImGui::Button("Validate", ImVec2(100.0f, 0.0f)))
            {
                App::ApplyGameRootBuffer(app);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Skip For Now", ImVec2(120.0f, 0.0f)))
            {
                app.firstRunPromptOpen = false;
                App::AddLog(app, LogLevel::Warning, "First-run setup skipped. A game folder is still required before indexing.");
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void UploadPendingTexturePreview(AppState& app)
    {
        std::shared_ptr<App::TexturePreviewImage> pending =
            App::TakePendingTexturePreview(app);
        if (!pending)
        {
            return;
        }

        ClearTexturePreview();
        ClearTextureMapPreviews();
        if (!UpdateTexturePreview(
                pending->bgraPixels.data(),
                pending->width,
                pending->height,
                pending->stride,
                pending->srgb))
        {
            App::SetTexturePreviewUploadError(
                app,
                pending->generation,
                "D3D11 could not create the preview texture.");
            app.texturePreview.uploadedGeneration = pending->generation;
            return;
        }

        for (const App::TexturePreviewImage::MapPreview& mapPreview :
             pending->mapPreviews)
        {
            UpdateTextureMapPreview(
                static_cast<uint32_t>(mapPreview.kind),
                mapPreview.bgraPixels.data(),
                mapPreview.width,
                mapPreview.height,
                mapPreview.stride);
        }

        if (!pending->mapPreviews.empty())
        {
            app.texturePreview.selectedMapMask =
                1u << static_cast<uint32_t>(pending->mapPreviews.front().kind);
        }
        else
        {
            app.texturePreview.selectedMapMask = 0;
        }
        app.texturePreview.uploadedGeneration = pending->generation;
    }

    namespace
    {
        bool DrawSegmentedButton(const char* label, bool active, const ImVec2& size = ImVec2(0.0f, 0.0f))
        {
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.44f, 0.72f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.52f, 0.82f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.38f, 0.64f, 1.0f));
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.23f, 0.26f, 0.31f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.19f, 0.22f, 0.27f, 1.0f));
            }
            const bool clicked = ImGui::Button(label, size);
            ImGui::PopStyleColor(3);
            return clicked;
        }
    }

    void DrawTextureDetailRow(const char* label, const std::string& value)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextWrapped("%s", value.empty() ? "N/A" : value.c_str());
    }

    void DrawTexturePreviewCanvas(
        AppState& app,
        const UiTexture* previewTexture,
        const App::TexturePreviewSnapshot& preview,
        float* effectiveScaleOut = nullptr)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.06f, 0.072f, 1.0f));
        const bool childOpen = ImGui::BeginChild(
            "TexturePreviewCanvas",
            ImVec2(0.0f, 0.0f),
            true,
            ImGuiWindowFlags_HorizontalScrollbar);

        if (childOpen)
        {
            if (previewTexture == nullptr || previewTexture->view == nullptr ||
                previewTexture->width == 0 || previewTexture->height == 0)
            {
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const char* message = preview.loading
                    ? "Decoding texture preview..."
                    : !preview.error.empty()
                        ? "Texture preview is unavailable."
                        : "No texture preview is available.";
                const ImVec2 textSize = ImGui::CalcTextSize(message);
                ImGui::SetCursorPos(ImVec2(
                    ImGui::GetCursorPosX() + std::max(0.0f, (available.x - textSize.x) * 0.5f),
                    ImGui::GetCursorPosY() + std::max(0.0f, (available.y - textSize.y) * 0.5f)));
                ImGui::TextDisabled("%s", message);
            }
            else
            {
                const bool canvasHovered =
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
                if (canvasHovered && ImGui::GetIO().MouseWheel != 0.0f)
                {
                    app.texturePreview.fitToWindow = false;
                    app.texturePreview.zoom = std::clamp(
                        app.texturePreview.zoom *
                            std::pow(1.15f, ImGui::GetIO().MouseWheel),
                        0.05f,
                        16.0f);
                }

                const ImVec2 available = ImGui::GetContentRegionAvail();
                float scale = app.texturePreview.zoom;
                if (app.texturePreview.fitToWindow)
                {
                    const float fitX = std::max(1.0f, available.x - 24.0f) /
                        static_cast<float>(previewTexture->width);
                    const float fitY = std::max(1.0f, available.y - 24.0f) /
                        static_cast<float>(previewTexture->height);
                    scale = std::min(1.0f, std::min(fitX, fitY));
                }
                if (effectiveScaleOut != nullptr)
                {
                    *effectiveScaleOut = scale;
                }

                const ImVec2 imageSize(
                    std::max(1.0f, previewTexture->width * scale),
                    std::max(1.0f, previewTexture->height * scale));
                const float offsetX = std::max(12.0f, (available.x - imageSize.x) * 0.5f);
                const float offsetY = std::max(12.0f, (available.y - imageSize.y) * 0.5f);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);

                const ImVec2 imageMin = ImGui::GetCursorScreenPos();
                const ImVec2 imageMax(imageMin.x + imageSize.x, imageMin.y + imageSize.y);
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 windowMin = ImGui::GetWindowPos();
                const ImVec2 windowMax(
                    windowMin.x + ImGui::GetWindowSize().x,
                    windowMin.y + ImGui::GetWindowSize().y);

                if (app.texturePreview.checkerboard)
                {
                    constexpr float tileSize = 16.0f;
                    const float visibleMinX = std::max(imageMin.x, windowMin.x);
                    const float visibleMinY = std::max(imageMin.y, windowMin.y);
                    const float visibleMaxX = std::min(imageMax.x, windowMax.x);
                    const float visibleMaxY = std::min(imageMax.y, windowMax.y);
                    const int firstX = std::max(0, static_cast<int>((visibleMinX - imageMin.x) / tileSize));
                    const int firstY = std::max(0, static_cast<int>((visibleMinY - imageMin.y) / tileSize));
                    const int lastX = std::max(
                        firstX,
                        static_cast<int>(std::ceil((visibleMaxX - imageMin.x) / tileSize)));
                    const int lastY = std::max(
                        firstY,
                        static_cast<int>(std::ceil((visibleMaxY - imageMin.y) / tileSize)));

                    for (int y = firstY; y < lastY; ++y)
                    {
                        for (int x = firstX; x < lastX; ++x)
                        {
                            const ImU32 color = ((x + y) & 1) == 0
                                ? IM_COL32(68, 72, 82, 255)
                                : IM_COL32(43, 47, 55, 255);
                            drawList->AddRectFilled(
                                ImVec2(imageMin.x + x * tileSize, imageMin.y + y * tileSize),
                                ImVec2(
                                    std::min(imageMax.x, imageMin.x + (x + 1) * tileSize),
                                    std::min(imageMax.y, imageMin.y + (y + 1) * tileSize)),
                                color);
                        }
                    }
                }

                ImGui::Image(
                    reinterpret_cast<ImTextureID>(previewTexture->view),
                    imageSize);
                drawList->AddRect(
                    imageMin,
                    imageMax,
                    IM_COL32(120, 128, 143, 255));

                if (canvasHovered)
                {
                    ImGui::SetMouseCursor(
                        ImGui::IsMouseDown(ImGuiMouseButton_Left)
                            ? ImGuiMouseCursor_Hand
                            : ImGuiMouseCursor_ResizeAll);
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
                    {
                        const ImVec2 dragDelta = ImGui::GetIO().MouseDelta;
                        ImGui::SetScrollX(ImGui::GetScrollX() - dragDelta.x);
                        ImGui::SetScrollY(ImGui::GetScrollY() - dragDelta.y);
                    }
                }

                // Unobtrusive bottom-left canvas navigation hint pill
                const char* hintText = "Wheel: Zoom  ·  Drag: Pan  ·  F / 0: Fit  ·  1: 100%  ·  G: Alpha Grid";
                const ImVec2 hintSize = ImGui::CalcTextSize(hintText);
                constexpr float padX = 9.0f;
                constexpr float padY = 4.0f;
                if (available.x > hintSize.x + padX * 2.0f + 24.0f && available.y > 56.0f)
                {
                    const ImVec2 pillMin(windowMin.x + 10.0f, windowMax.y - hintSize.y - padY * 2.0f - 10.0f);
                    const ImVec2 pillMax(pillMin.x + hintSize.x + padX * 2.0f, pillMin.y + hintSize.y + padY * 2.0f);
                    drawList->AddRectFilled(pillMin, pillMax, IM_COL32(14, 16, 20, 155), 0.0f);
                    drawList->AddText(ImVec2(pillMin.x + padX, pillMin.y + padY), IM_COL32(180, 188, 202, 210), hintText);
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void DrawTexturePreviewWindow(AppState& app)
    {
        UploadPendingTexturePreview(app);
        if (!app.texturePreview.windowOpen)
        {
            return;
        }

        App::TexturePreviewSnapshot preview = App::SnapshotTexturePreview(app);
        if (preview.entryName.empty())
        {
            app.texturePreview.windowOpen = false;
            return;
        }
        if (app.texturePreview.uploadedGeneration != preview.generation)
        {
            ClearTexturePreview();
            ClearTextureMapPreviews();
            app.texturePreview.selectedMapMask = 0;
            app.texturePreview.activePreviewMapKind = -1;
            app.texturePreview.uploadedGeneration = preview.generation;
        }

        std::optional<AssetIndex::SubAssetRecord> currentEntry;
        if (const auto contents = app.packContentsCache.Get(preview.packPath))
        {
            const auto entryIt = std::find_if(
                contents->entries.begin(),
                contents->entries.end(),
                [&](const AssetIndex::SubAssetRecord& entry) {
                    return entry.offset == preview.entryOffset &&
                        entry.name == preview.entryName;
                });
            if (entryIt != contents->entries.end())
            {
                currentEntry = *entryIt;
            }
        }
        if (!currentEntry.has_value() && !preview.sourceEntry.name.empty() && preview.sourceEntry.size > 0)
        {
            currentEntry = preview.sourceEntry;
        }

        const Export::TextureMapAnalysis mapAnalysis =
            Export::AnalyzeTextureMaps(preview.entryName);

        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        const ImVec2 viewportSize = mainViewport->WorkSize;
        const ImVec2 defaultSize(
            std::clamp(viewportSize.x * 0.88f, 960.0f, 1600.0f),
            std::clamp(viewportSize.y * 0.88f, 640.0f, 1000.0f));

        const std::string title = "Texture Viewer - " + preview.entryName + "###TexturePreviewModal";
        if (!ImGui::IsPopupOpen(title.c_str()))
        {
            ImGui::OpenPopup(title.c_str());
        }

        ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(defaultSize, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(860.0f, 540.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (app.texturePreview.focusRequested)
        {
            ImGui::SetNextWindowFocus();
            app.texturePreview.focusRequested = false;
        }

        bool open = app.texturePreview.windowOpen;

        ImGuiIO& io = ImGui::GetIO();
        const bool previousMoveFromTitleBarOnly = io.ConfigWindowsMoveFromTitleBarOnly;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        const bool viewerVisible = ImGui::BeginPopupModal(title.c_str(), &open, ImGuiWindowFlags_NoCollapse);
        io.ConfigWindowsMoveFromTitleBarOnly = previousMoveFromTitleBarOnly;

        if (viewerVisible)
        {
            const std::string packFileName = Platform::PathToUtf8(preview.packPath.filename());
            bool exportBusy = false;
            {
                std::lock_guard<std::mutex> lock(app.exportProgress.mutex);
                exportBusy = app.exportProgress.running.load();
            }

            // Studio Header Bar
            ImGui::BeginGroup();
            {
                DrawPanelTitle(UiIconId::Texture, preview.entryName.c_str());
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("|  %s", packFileName.empty() ? "DXP" : packFileName.c_str());
                if (preview.sourceWidth > 0 && preview.sourceHeight > 0)
                {
                    ImGui::SameLine(0.0f, 10.0f);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(
                        ImVec4(0.62f, 0.72f, 0.84f, 1.0f),
                        "%ux%u · %s · %s · %u mips",
                        preview.sourceWidth,
                        preview.sourceHeight,
                        preview.format.empty() ? "Unknown" : preview.format.c_str(),
                        preview.srgb ? "sRGB" : "Linear",
                        static_cast<unsigned>(preview.mipCount));
                }
                if (preview.loading)
                {
                    ImGui::SameLine(0.0f, 10.0f);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "● Decoding");
                }
                else if (!preview.error.empty())
                {
                    ImGui::SameLine(0.0f, 10.0f);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.42f, 1.0f), "● Failed");
                }
            }
            ImGui::EndGroup();

            const float reloadBtnWidth = CalcIconButtonSize("Reload", GetUiIcon(UiIconId::Rebuild) != nullptr).x + 8.0f;
            const float exportBtnWidth = CalcIconButtonSize("Export Texture", GetUiIcon(UiIconId::Export) != nullptr).x + 12.0f;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float totalRightWidth = reloadBtnWidth + spacing + exportBtnWidth;
            const float rightStartX = ImGui::GetWindowContentRegionMax().x - totalRightWidth;
            if (rightStartX > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f)
            {
                ImGui::SameLine();
                ImGui::SetCursorPosX(rightStartX);
            }
            else
            {
                ImGui::SameLine();
            }

            ImGui::BeginDisabled(!currentEntry.has_value());
            if (DrawIconTextButton("##ReloadPreview", UiIconId::Rebuild, "Reload", ImVec2(reloadBtnWidth, 0.0f)) &&
                currentEntry)
            {
                ClearTexturePreview();
                ClearTextureMapPreviews();
                app.texturePreview.fitToWindow = true;
                app.texturePreview.zoom = 1.0f;
                app.texturePreview.activePreviewMapKind = -1;
                App::RequestTexturePreview(app, preview.packPath, *currentEntry);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            const bool exportOriginalDisabled = exportBusy || !currentEntry.has_value();
            ImGui::BeginDisabled(exportOriginalDisabled);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.78f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.53f, 0.88f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.38f, 0.68f, 1.0f));
            if (DrawIconTextButton("##ExportTexture", UiIconId::Export, "Export Texture", ImVec2(exportBtnWidth, 0.0f)) &&
                currentEntry)
            {
                App::RequestTextureExport(app, preview.packPath, *currentEntry);
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (exportBusy)
                {
                    DrawWrappedTooltip("Another export operation is currently running.");
                }
                else
                {
                    DrawWrappedTooltip("Export original source texture (" + app.settings.textureExportFormat + ").");
                }
            }

            ImGui::Separator();

            const ImGuiTableFlags layoutFlags =
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("TextureViewerLayout", 2, layoutFlags))
            {
                ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.72f);
                ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed, 340.0f);
                ImGui::TableNextRow();

                // Column 0: Canvas + Clean Toolbar
                ImGui::TableSetColumnIndex(0);
                if (ImGui::BeginChild("TextureViewerMain", ImVec2(0.0f, 0.0f), false))
                {
                    const UiTexture* activeCanvasTexture = GetTexturePreview();
                    const Export::DetectedTextureMap* activeChannelMap = nullptr;
                    if (app.texturePreview.activePreviewMapKind >= 0)
                    {
                        const Export::TextureMapKind activeKind =
                            static_cast<Export::TextureMapKind>(app.texturePreview.activePreviewMapKind);
                        const UiTexture* channelTex = GetTextureMapPreview(
                            static_cast<uint32_t>(activeKind));
                        if (channelTex != nullptr && channelTex->view != nullptr)
                        {
                            for (const auto& candidate : mapAnalysis.maps)
                            {
                                if (candidate.kind == activeKind)
                                {
                                    activeCanvasTexture = channelTex;
                                    activeChannelMap = &candidate;
                                    break;
                                }
                            }
                        }
                        if (activeChannelMap == nullptr)
                        {
                            app.texturePreview.activePreviewMapKind = -1;
                        }
                    }

                    // Compute current effective scale cleanly for toolbar readout
                    float effectiveScale = app.texturePreview.zoom;
                    if (app.texturePreview.fitToWindow && activeCanvasTexture != nullptr &&
                        activeCanvasTexture->width > 0 && activeCanvasTexture->height > 0)
                    {
                        const ImVec2 approxAvail = ImGui::GetContentRegionAvail();
                        const float fitX = std::max(1.0f, approxAvail.x - 24.0f) /
                            static_cast<float>(activeCanvasTexture->width);
                        const float fitY = std::max(1.0f, approxAvail.y - 68.0f) /
                            static_cast<float>(activeCanvasTexture->height);
                        effectiveScale = std::min(1.0f, std::min(fitX, fitY));
                    }

                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

                    if (DrawSegmentedButton("Fit", app.texturePreview.fitToWindow))
                    {
                        app.texturePreview.fitToWindow = true;
                    }
                    ImGui::SameLine();
                    const bool is100 = !app.texturePreview.fitToWindow &&
                        std::fabs(app.texturePreview.zoom - 1.0f) < 0.01f;
                    if (DrawSegmentedButton("100%", is100))
                    {
                        app.texturePreview.fitToWindow = false;
                        app.texturePreview.zoom = 1.0f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("-"))
                    {
                        app.texturePreview.fitToWindow = false;
                        app.texturePreview.zoom = std::max(0.05f, app.texturePreview.zoom / 1.25f);
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(110.0f);
                    if (app.texturePreview.fitToWindow)
                    {
                        float displayZoom = effectiveScale;
                        ImGui::BeginDisabled(true);
                        ImGui::SliderFloat(
                            "##TextureZoom",
                            &displayZoom,
                            0.05f,
                            16.0f,
                            "Fit (%.2fx)",
                            ImGuiSliderFlags_Logarithmic);
                        ImGui::EndDisabled();
                    }
                    else
                    {
                        ImGui::SliderFloat(
                            "##TextureZoom",
                            &app.texturePreview.zoom,
                            0.05f,
                            16.0f,
                            "%.2fx",
                            ImGuiSliderFlags_Logarithmic);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("+"))
                    {
                        app.texturePreview.fitToWindow = false;
                        app.texturePreview.zoom = std::min(16.0f, app.texturePreview.zoom * 1.25f);
                    }

                    ImGui::SameLine(0.0f, 10.0f);
                    ImGui::TextDisabled("|");
                    ImGui::SameLine(0.0f, 10.0f);

                    if (DrawSegmentedButton("Alpha Grid", app.texturePreview.checkerboard))
                    {
                        app.texturePreview.checkerboard = !app.texturePreview.checkerboard;
                    }

                    if (activeChannelMap != nullptr)
                    {
                        ImGui::SameLine(0.0f, 10.0f);
                        ImGui::TextDisabled("|");
                        ImGui::SameLine(0.0f, 8.0f);
                        ImGui::TextColored(
                            ImVec4(0.45f, 0.78f, 1.0f, 1.0f),
                            "Channel: %s",
                            activeChannelMap->label.c_str());
                        ImGui::SameLine(0.0f, 6.0f);
                        if (ImGui::SmallButton("Show Source"))
                        {
                            app.texturePreview.activePreviewMapKind = -1;
                        }
                    }

                    ImGui::PopStyleVar(1);

                    if (preview.loading)
                    {
                        const float phase =
                            0.15f + 0.7f * static_cast<float>((std::sin(ImGui::GetTime() * 3.0) + 1.0) * 0.5);
                        ImGui::ProgressBar(phase, ImVec2(-1.0f, 0.0f), "Decoding texture...");
                    }
                    else if (!preview.error.empty())
                    {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.49f, 0.42f, 1.0f),
                            "Preview unavailable: %s",
                            preview.error.c_str());
                    }

                    // Keyboard shortcuts when Texture Viewer is focused
                    if (!io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                    {
                        if (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_0, false))
                        {
                            app.texturePreview.fitToWindow = true;
                        }
                        else if (ImGui::IsKeyPressed(ImGuiKey_1, false))
                        {
                            app.texturePreview.fitToWindow = false;
                            app.texturePreview.zoom = 1.0f;
                        }
                        else if (ImGui::IsKeyPressed(ImGuiKey_G, false))
                        {
                            app.texturePreview.checkerboard = !app.texturePreview.checkerboard;
                        }
                        else if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))
                        {
                            app.texturePreview.fitToWindow = false;
                            app.texturePreview.zoom = std::min(16.0f, app.texturePreview.zoom * 1.25f);
                        }
                        else if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))
                        {
                            app.texturePreview.fitToWindow = false;
                            app.texturePreview.zoom = std::max(0.05f, app.texturePreview.zoom / 1.25f);
                        }
                        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && app.texturePreview.activePreviewMapKind >= 0)
                        {
                            app.texturePreview.activePreviewMapKind = -1;
                        }
                    }

                    DrawTexturePreviewCanvas(app, activeCanvasTexture, preview);
                }
                ImGui::EndChild();

                // Column 1: Clean Tabbed Inspector (Details | Channels)
                ImGui::TableSetColumnIndex(1);
                if (ImGui::BeginChild("TextureViewerDetails", ImVec2(0.0f, 0.0f), false))
                {
                    if (ImGui::BeginTabBar("TextureInspectorTabs"))
                    {
                        if (ImGui::BeginTabItem("Details"))
                        {
                            ImGui::Spacing();
                            if (ImGui::CollapsingHeader("Image", ImGuiTreeNodeFlags_DefaultOpen))
                            {
                                if (ImGui::BeginTable(
                                        "TextureImageDetails",
                                        2,
                                        ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_BordersInnerH |
                                            ImGuiTableFlags_SizingStretchProp))
                                {
                                    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 96.0f);
                                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                                    DrawTextureDetailRow("Name", preview.entryName);
                                    DrawTextureDetailRow(
                                        "Dimensions",
                                        std::to_string(preview.sourceWidth) + " x " +
                                            std::to_string(preview.sourceHeight));
                                    const double aspect = preview.sourceHeight == 0
                                        ? 0.0
                                        : static_cast<double>(preview.sourceWidth) /
                                            static_cast<double>(preview.sourceHeight);
                                    std::ostringstream aspectText;
                                    aspectText << std::fixed << std::setprecision(3) << aspect << ":1";
                                    DrawTextureDetailRow("Aspect", aspectText.str());
                                    DrawTextureDetailRow(
                                        "Mip levels",
                                        std::to_string(static_cast<unsigned>(preview.mipCount)));
                                    DrawTextureDetailRow(
                                        "Format",
                                        preview.format.empty() ? "Unknown" : preview.format);
                                    DrawTextureDetailRow(
                                        "Color space",
                                        preview.srgb ? "sRGB" : "Linear / unspecified");
                                    DrawTextureDetailRow(
                                        "Detected role",
                                        mapAnalysis.summary.empty() ? "Unclassified" : mapAnalysis.summary);
                                    ImGui::EndTable();
                                }
                            }

                            ImGui::Spacing();
                            if (ImGui::CollapsingHeader("Storage", ImGuiTreeNodeFlags_DefaultOpen))
                            {
                                if (ImGui::BeginTable(
                                        "TextureStorageDetails",
                                        2,
                                        ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_BordersInnerH |
                                            ImGuiTableFlags_SizingStretchProp))
                                {
                                    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 96.0f);
                                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                                    DrawTextureDetailRow(
                                        "DXP pack",
                                        packFileName);
                                    DrawTextureDetailRow(
                                        "Compression",
                                        preview.compression.empty() ? "None / unknown" : preview.compression);
                                    std::ostringstream offsetText;
                                    offsetText << "0x" << std::uppercase << std::hex << preview.entryOffset;
                                    DrawTextureDetailRow("Offset", offsetText.str());
                                    DrawTextureDetailRow("Packed size", FormatBytes(preview.sourceSize));
                                    const uint64_t decodedBytes =
                                        static_cast<uint64_t>(preview.sourceWidth) *
                                        static_cast<uint64_t>(preview.sourceHeight) * 4ull;
                                    DrawTextureDetailRow("BGRA preview", FormatBytes(decodedBytes));
                                    ImGui::EndTable();
                                }
                            }

                            if (preview.loading || !preview.error.empty())
                            {
                                ImGui::Spacing();
                                if (ImGui::CollapsingHeader("Decode Status", ImGuiTreeNodeFlags_DefaultOpen))
                                {
                                    if (ImGui::BeginTable(
                                            "TextureDecodeDetails",
                                            2,
                                            ImGuiTableFlags_RowBg |
                                                ImGuiTableFlags_BordersInnerH |
                                                ImGuiTableFlags_SizingStretchProp))
                                    {
                                        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 96.0f);
                                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0);
                                        ImGui::TextDisabled("Status");
                                        ImGui::TableSetColumnIndex(1);
                                        if (preview.loading)
                                        {
                                            ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "Decoding...");
                                        }
                                        else
                                        {
                                            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.42f, 1.0f), "Failed");
                                        }
                                        ImGui::EndTable();
                                    }

                                    if (!preview.error.empty())
                                    {
                                        ImGui::Spacing();
                                        DrawWrappedColoredText(
                                            ImVec4(1.0f, 0.49f, 0.42f, 1.0f),
                                            preview.error);
                                    }
                                }
                            }

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                            if (DrawIconTextButton("##ShowDxpInExplorer", UiIconId::ContentBrowser, "Show DXP in Explorer", ImVec2(-1.0f, 0.0f)))
                            {
                                Platform::ShowPathInExplorer(preview.packPath);
                            }

                            ImGui::EndTabItem();
                        }

                        char channelsTabLabel[48];
                        if (mapAnalysis.maps.size() > 1)
                        {
                            snprintf(channelsTabLabel, sizeof(channelsTabLabel), "Channels (%zu)###ChannelsTab", mapAnalysis.maps.size());
                        }
                        else
                        {
                            snprintf(channelsTabLabel, sizeof(channelsTabLabel), "Channels###ChannelsTab");
                        }

                        if (ImGui::BeginTabItem(channelsTabLabel))
                        {
                            ImGui::Spacing();

                            if (preview.loading)
                            {
                                DrawWrappedDisabledText(
                                    "Channel previews will appear once the source texture finishes decoding.");
                            }
                            else if (!preview.error.empty())
                            {
                                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.075f, 0.065f, 0.72f));
                                if (ImGui::BeginChild(
                                        "TextureMapDecodeError",
                                        ImVec2(0.0f, 0.0f),
                                        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY))
                                {
                                    ImGui::TextColored(ImVec4(1.0f, 0.49f, 0.42f, 1.0f), "Channels unavailable");
                                    DrawWrappedDisabledText("Source texture decode failed. Fix or reload the preview first.");
                                }
                                ImGui::EndChild();
                                ImGui::PopStyleColor();
                            }
                            else if (mapAnalysis.maps.empty())
                            {
                                DrawWrappedDisabledText(
                                    "No packed channels were detected from this texture name. "
                                    "Use Export Texture in the header bar to export the image.");
                            }
                            else
                            {
                                const bool showChannelBatchExport =
                                    mapAnalysis.maps.size() > 1 || mapAnalysis.maps[0].derived;

                                if (DrawSegmentedButton("Source Texture (Full)", app.texturePreview.activePreviewMapKind < 0, ImVec2(-1.0f, 0.0f)))
                                {
                                    app.texturePreview.activePreviewMapKind = -1;
                                }

                                ImGui::Spacing();
                                DrawWrappedDisabledText(
                                    showChannelBatchExport
                                        ? "Click thumbnail to inspect in canvas. Check cards to extract PNG channels."
                                        : "Click thumbnail to inspect in canvas, or right-click for channel export.");

                                std::vector<Export::TextureMapKind> selectedMaps;
                                if (showChannelBatchExport)
                                {
                                    selectedMaps.reserve(mapAnalysis.maps.size());
                                    for (const Export::DetectedTextureMap& map : mapAnalysis.maps)
                                    {
                                        const uint32_t selectionBit = 1u << static_cast<uint32_t>(map.kind);
                                        const UiTexture* mapTexture = GetTextureMapPreview(static_cast<uint32_t>(map.kind));
                                        if ((app.texturePreview.selectedMapMask & selectionBit) != 0 &&
                                            mapTexture != nullptr &&
                                            mapTexture->view != nullptr)
                                        {
                                            selectedMaps.push_back(map.kind);
                                        }
                                    }

                                    ImGui::Spacing();
                                    if (ImGui::SmallButton("Select All"))
                                    {
                                        for (const Export::DetectedTextureMap& map : mapAnalysis.maps)
                                        {
                                            const UiTexture* mapTexture = GetTextureMapPreview(static_cast<uint32_t>(map.kind));
                                            if (mapTexture != nullptr && mapTexture->view != nullptr)
                                            {
                                                app.texturePreview.selectedMapMask |= 1u << static_cast<uint32_t>(map.kind);
                                            }
                                        }
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton("Clear"))
                                    {
                                        app.texturePreview.selectedMapMask = 0;
                                    }
                                    ImGui::SameLine();
                                    char selBuf[32];
                                    snprintf(selBuf, sizeof(selBuf), "%zu selected", selectedMaps.size());
                                    const float selW = ImGui::CalcTextSize(selBuf).x;
                                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - selW));
                                    ImGui::TextDisabled("%s", selBuf);
                                }
                                ImGui::Spacing();

                                constexpr float cardSpacing = 8.0f;
                                const float availW = ImGui::GetContentRegionAvail().x;
                                const float cardW = std::max(100.0f, (availW - cardSpacing) * 0.5f);
                                const float thumbH = std::clamp(cardW * 0.78f, 82.0f, 124.0f);
                                constexpr float footerH = 26.0f;

                                for (size_t mapIndex = 0; mapIndex < mapAnalysis.maps.size(); ++mapIndex)
                                {
                                    const Export::DetectedTextureMap& map = mapAnalysis.maps[mapIndex];
                                    if ((mapIndex % 2) != 0)
                                    {
                                        ImGui::SameLine(0.0f, cardSpacing);
                                    }

                                    ImGui::PushID(static_cast<int>(map.kind));
                                    const UiTexture* mapTexture = GetTextureMapPreview(static_cast<uint32_t>(map.kind));
                                    const bool mapPreviewAvailable =
                                        mapTexture != nullptr && mapTexture->view != nullptr;
                                    const uint32_t selectionBit = 1u << static_cast<uint32_t>(map.kind);
                                    const bool isChecked = (app.texturePreview.selectedMapMask & selectionBit) != 0;
                                    const bool isViewing =
                                        app.texturePreview.activePreviewMapKind == static_cast<int>(map.kind);

                                    ImGui::BeginGroup();
                                    const ImVec2 cardMin = ImGui::GetCursorScreenPos();
                                    const ImVec2 cardMax(cardMin.x + cardW, cardMin.y + thumbH + footerH);
                                    ImDrawList* drawList = ImGui::GetWindowDrawList();

                                    // Thumbnail interactive button
                                    ImGui::InvisibleButton(
                                        "##TextureMapThumb",
                                        ImVec2(cardW, thumbH),
                                        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                                    const bool thumbHovered = ImGui::IsItemHovered();

                                    if (mapPreviewAvailable && ImGui::IsItemClicked(ImGuiMouseButton_Left))
                                    {
                                        app.texturePreview.activePreviewMapKind =
                                            isViewing ? -1 : static_cast<int>(map.kind);
                                    }

                                    // Draw card frame & thumbnail
                                    drawList->AddRectFilled(
                                        cardMin,
                                        cardMax,
                                        thumbHovered ? IM_COL32(42, 48, 58, 255) : IM_COL32(28, 32, 39, 255),
                                        0.0f);

                                    if (mapPreviewAvailable)
                                    {
                                        const float imgPad = 5.0f;
                                        const float maxW = std::max(1.0f, cardW - imgPad * 2.0f);
                                        const float maxH = std::max(1.0f, thumbH - imgPad * 2.0f);
                                        const float imgScale = std::min(
                                            maxW / static_cast<float>(mapTexture->width),
                                            maxH / static_cast<float>(mapTexture->height));
                                        const ImVec2 imgSize(
                                            std::max(1.0f, static_cast<float>(mapTexture->width) * imgScale),
                                            std::max(1.0f, static_cast<float>(mapTexture->height) * imgScale));
                                        const ImVec2 imgMin(
                                            cardMin.x + (cardW - imgSize.x) * 0.5f,
                                            cardMin.y + (thumbH - imgSize.y) * 0.5f);
                                        drawList->AddImage(
                                            reinterpret_cast<ImTextureID>(mapTexture->view),
                                            imgMin,
                                            ImVec2(imgMin.x + imgSize.x, imgMin.y + imgSize.y));

                                        if (isViewing)
                                        {
                                            const char* viewingTag = "VIEWING";
                                            const ImVec2 tagSz = ImGui::CalcTextSize(viewingTag);
                                            const ImVec2 tagMin(cardMin.x + 5.0f, cardMin.y + 5.0f);
                                            const ImVec2 tagMax(tagMin.x + tagSz.x + 8.0f, tagMin.y + tagSz.y + 4.0f);
                                            drawList->AddRectFilled(tagMin, tagMax, IM_COL32(36, 128, 224, 235), 0.0f);
                                            drawList->AddText(ImVec2(tagMin.x + 4.0f, tagMin.y + 2.0f), IM_COL32(255, 255, 255, 255), viewingTag);
                                        }
                                    }
                                    else
                                    {
                                        const ImVec2 unavailableSz = ImGui::CalcTextSize("Unavailable");
                                        drawList->AddText(
                                            ImVec2(
                                                cardMin.x + (cardW - unavailableSz.x) * 0.5f,
                                                cardMin.y + (thumbH - unavailableSz.y) * 0.5f),
                                            IM_COL32(148, 154, 166, 255),
                                            "Unavailable");
                                    }

                                    // Context menu on thumbnail right-click
                                    if (ImGui::BeginPopupContextItem("TextureMapDetails"))
                                    {
                                        ImGui::TextUnformatted(map.label.c_str());
                                        ImGui::Separator();
                                        if (mapPreviewAvailable)
                                        {
                                            if (ImGui::MenuItem(isViewing ? "Return to Source Texture" : "Preview in Main Canvas"))
                                            {
                                                app.texturePreview.activePreviewMapKind =
                                                    isViewing ? -1 : static_cast<int>(map.kind);
                                            }
                                        }
                                        ImGui::BeginDisabled(!currentEntry.has_value() || exportBusy || !mapPreviewAvailable);
                                        if (ImGui::MenuItem("Export This Channel (PNG)") && currentEntry)
                                        {
                                            App::RequestTextureMapsExport(
                                                app,
                                                preview.packPath,
                                                *currentEntry,
                                                { map.kind });
                                        }
                                        ImGui::EndDisabled();
                                        ImGui::Separator();
                                        ImGui::TextDisabled("Channels: %s", map.channels.c_str());
                                        const std::string outputSuffix =
                                            std::string(Export::TextureMapKindFileSuffix(map.kind)) + ".png";
                                        ImGui::TextDisabled("Suffix: %s", outputSuffix.c_str());
                                        ImGui::TextDisabled("Size: %u x %u", preview.sourceWidth, preview.sourceHeight);
                                        ImGui::TextDisabled(
                                            "Space: %s",
                                            map.kind == Export::TextureMapKind::BaseColor ? "sRGB color" : "Linear data");
                                        ImGui::EndPopup();
                                    }

                                    if (thumbHovered)
                                    {
                                        ImGui::BeginTooltip();
                                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
                                        ImGui::TextUnformatted(map.label.c_str());
                                        ImGui::TextDisabled("%s", map.channels.c_str());
                                        const std::string tipSuffix =
                                            std::string("Output: *") +
                                            std::string(Export::TextureMapKindFileSuffix(map.kind)) + ".png";
                                        ImGui::TextDisabled("%s", tipSuffix.c_str());
                                        if (mapPreviewAvailable)
                                        {
                                            ImGui::TextColored(
                                                ImVec4(0.45f, 0.78f, 1.0f, 1.0f),
                                                "%s",
                                                isViewing ? "Click to return to source texture" : "Click to inspect in canvas · Right-click for actions");
                                        }
                                        ImGui::PopTextWrapPos();
                                        ImGui::EndTooltip();
                                    }

                                    // Footer Strip with clear map label (+ checkbox when multi-channel extraction applies)
                                    drawList->AddLine(
                                        ImVec2(cardMin.x + 1.0f, cardMin.y + thumbH),
                                        ImVec2(cardMax.x - 1.0f, cardMin.y + thumbH),
                                        IM_COL32(60, 66, 78, 200));

                                    ImGui::SetCursorScreenPos(ImVec2(cardMin.x + 6.0f, cardMin.y + thumbH + 3.0f));
                                    if (showChannelBatchExport)
                                    {
                                        bool checkVal = isChecked;
                                        ImGui::BeginDisabled(!mapPreviewAvailable);
                                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
                                        if (ImGui::Checkbox("##ChannelSelect", &checkVal))
                                        {
                                            if (checkVal)
                                            {
                                                app.texturePreview.selectedMapMask |= selectionBit;
                                            }
                                            else
                                            {
                                                app.texturePreview.selectedMapMask &= ~selectionBit;
                                            }
                                        }
                                        ImGui::PopStyleVar();
                                        ImGui::EndDisabled();
                                        ImGui::SameLine(0.0f, 5.0f);
                                    }

                                    // Fit label cleanly inside card width
                                    const float labelMaxX = cardMax.x - 6.0f;
                                    const float textStartX = ImGui::GetCursorScreenPos().x;
                                    const float maxLabelW = std::max(24.0f, labelMaxX - textStartX);
                                    std::string clippedLabel = map.label;
                                    if (ImGui::CalcTextSize(clippedLabel.c_str()).x > maxLabelW)
                                    {
                                        while (clippedLabel.size() > 3 &&
                                               ImGui::CalcTextSize((clippedLabel + "...").c_str()).x > maxLabelW)
                                        {
                                            clippedLabel.pop_back();
                                        }
                                        clippedLabel += "...";
                                    }
                                    ImGui::TextColored(
                                        isViewing ? ImVec4(0.55f, 0.85f, 1.0f, 1.0f) : ImVec4(0.92f, 0.94f, 0.97f, 1.0f),
                                        "%s",
                                        clippedLabel.c_str());

                                    // Draw crisp card border on top
                                    const ImU32 borderCol = isViewing
                                        ? IM_COL32(72, 168, 255, 255)
                                        : isChecked
                                            ? IM_COL32(76, 198, 122, 240)
                                            : thumbHovered
                                                ? IM_COL32(124, 140, 168, 240)
                                                : IM_COL32(72, 80, 94, 210);
                                    drawList->AddRect(
                                        cardMin,
                                        cardMax,
                                        borderCol,
                                        0.0f,
                                        0,
                                        (isViewing || isChecked) ? 2.0f : 1.0f);

                                    // Advance group bounds accurately to footer bottom
                                    ImGui::SetCursorScreenPos(ImVec2(cardMin.x, cardMax.y));
                                    ImGui::Dummy(ImVec2(cardW, 2.0f));
                                    ImGui::EndGroup();
                                    ImGui::PopID();
                                }

                                if (showChannelBatchExport)
                                {
                                    ImGui::Spacing();
                                    ImGui::Separator();
                                    ImGui::Spacing();
                                    ImGui::BeginDisabled(!currentEntry.has_value() || selectedMaps.empty() || exportBusy);
                                    const std::string extractLabel = selectedMaps.empty()
                                        ? "Check channels above to extract PNGs"
                                        : selectedMaps.size() == 1
                                            ? "Extract 1 Channel (PNG)"
                                            : "Extract " + std::to_string(selectedMaps.size()) + " Channels (PNG)";
                                    if (ImGui::Button(extractLabel.c_str(), ImVec2(-1.0f, 0.0f)) &&
                                        currentEntry && !selectedMaps.empty())
                                    {
                                        App::RequestTextureMapsExport(
                                            app,
                                            preview.packPath,
                                            *currentEntry,
                                            std::move(selectedMaps));
                                    }
                                    ImGui::EndDisabled();
                                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    {
                                        DrawWrappedTooltip(
                                            "Extracts split grayscale/normal PNG files for the checked channels. "
                                            "Use Export Texture in the header bar to save the unsplit source texture.");
                                    }
                                }
                            }

                            ImGui::EndTabItem();
                        }

                        ImGui::EndTabBar();
                    }
                }
                ImGui::EndChild();

                ImGui::EndTable();
            }

            DrawExportProgressModal(app);
            if (!open)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        app.texturePreview.windowOpen = open;
        if (!open)
        {
            app.texturePreview.activePreviewMapKind = -1;
        }
    }

    namespace
    {
        void ResetModelCamera(AppState& app)
        {
            const float dx = app.modelPreview.bboxMax[0] - app.modelPreview.bboxMin[0];
            const float dy = app.modelPreview.bboxMax[1] - app.modelPreview.bboxMin[1];
            const float dz = app.modelPreview.bboxMax[2] - app.modelPreview.bboxMin[2];
            const float bboxSize = std::sqrt(dx * dx + dy * dy + dz * dz);
            app.modelPreview.zoom = bboxSize > 0.0f ? bboxSize * 1.5f : 5.0f;
            app.modelPreview.panWorldX = 0.0f;
            app.modelPreview.panWorldY = 0.0f;
            app.modelPreview.panWorldZ = 0.0f;
            app.modelPreview.yaw = 45.0f;
            app.modelPreview.pitch = 30.0f;
        }

        void ApplyModelLightingPreset(App::ModelPreviewLighting& lighting, int presetIndex)
        {
            const bool keepGround = lighting.showGround;
            lighting = App::ModelPreviewLighting();
            lighting.showGround = keepGround;
            switch (presetIndex)
            {
            case 1: // Clay Sculpt
                lighting.lightYaw = 35.0f;
                lighting.lightPitch = 52.0f;
                lighting.keyColor[0] = 0.98f; lighting.keyColor[1] = 0.94f; lighting.keyColor[2] = 0.88f;
                lighting.fillColor[0] = 0.58f; lighting.fillColor[1] = 0.66f; lighting.fillColor[2] = 0.78f;
                lighting.ambientColor[0] = 0.24f; lighting.ambientColor[1] = 0.22f; lighting.ambientColor[2] = 0.21f;
                lighting.baseColor[0] = 0.76f; lighting.baseColor[1] = 0.68f; lighting.baseColor[2] = 0.61f;
                lighting.specIntensity = 0.18f;
                lighting.specPower = 18.0f;
                lighting.backgroundColor[0] = 0.13f; lighting.backgroundColor[1] = 0.13f; lighting.backgroundColor[2] = 0.15f;
                break;
            case 2: // Dark Rim
                lighting.lightYaw = -125.0f;
                lighting.lightPitch = 32.0f;
                lighting.keyColor[0] = 0.88f; lighting.keyColor[1] = 0.94f; lighting.keyColor[2] = 1.00f;
                lighting.fillColor[0] = 0.95f; lighting.fillColor[1] = 0.68f; lighting.fillColor[2] = 0.42f;
                lighting.ambientColor[0] = 0.10f; lighting.ambientColor[1] = 0.12f; lighting.ambientColor[2] = 0.15f;
                lighting.baseColor[0] = 0.56f; lighting.baseColor[1] = 0.60f; lighting.baseColor[2] = 0.66f;
                lighting.specIntensity = 0.75f;
                lighting.specPower = 56.0f;
                lighting.backgroundColor[0] = 0.07f; lighting.backgroundColor[1] = 0.08f; lighting.backgroundColor[2] = 0.10f;
                break;
            case 3: // High Key
                lighting.lightYaw = 28.0f;
                lighting.lightPitch = 60.0f;
                lighting.keyColor[0] = 1.00f; lighting.keyColor[1] = 0.99f; lighting.keyColor[2] = 0.96f;
                lighting.fillColor[0] = 0.78f; lighting.fillColor[1] = 0.84f; lighting.fillColor[2] = 0.92f;
                lighting.ambientColor[0] = 0.32f; lighting.ambientColor[1] = 0.34f; lighting.ambientColor[2] = 0.38f;
                lighting.baseColor[0] = 0.68f; lighting.baseColor[1] = 0.70f; lighting.baseColor[2] = 0.74f;
                lighting.specIntensity = 0.38f;
                lighting.specPower = 28.0f;
                lighting.backgroundColor[0] = 0.17f; lighting.backgroundColor[1] = 0.19f; lighting.backgroundColor[2] = 0.22f;
                break;
            case 0:
            default:
                break;
            }
        }
    }

    void DrawModelPreviewPopup(AppState& app)
    {
        if (!app.modelPreview.popupOpen)
        {
            return;
        }

        // Upload pending async model payload on UI thread
        std::shared_ptr<App::ModelPreviewData> pendingData = App::TakePendingModelPreview(app);
        if (pendingData)
        {
            ModelRenderer* renderer = GetModelRenderer();
            if (renderer)
            {
                if (renderer->SetModelData(pendingData->vertices, pendingData->indices))
                {
                    app.modelPreview.sourceEntry = pendingData->sourceEntry;
                    app.modelPreview.isDynModel = pendingData->isDynModel;
                    app.modelPreview.vertexCount = static_cast<uint32_t>(pendingData->vertices.size());
                    app.modelPreview.indexCount = static_cast<uint32_t>(pendingData->indices.size());
                    app.modelPreview.grpClassId = pendingData->grpClassId;
                    app.modelPreview.subMeshCount = pendingData->subMeshCount;
                    app.modelPreview.fileSize = pendingData->fileSize;
                    std::copy(std::begin(pendingData->bboxMin), std::end(pendingData->bboxMin), std::begin(app.modelPreview.bboxMin));
                    std::copy(std::begin(pendingData->bboxMax), std::end(pendingData->bboxMax), std::begin(app.modelPreview.bboxMax));
                    app.modelPreview.submeshes = std::move(pendingData->submeshes);
                    app.modelPreview.hoveredSubmeshIndex = -1;
                    app.modelPreview.selectedSubmeshIndex = -1;
                    app.modelPreview.listHoveredSubmeshIndex = -1;
                    app.modelPreview.partSearchBuffer[0] = '\0';
                    app.modelPreview.vertices = std::move(pendingData->vertices);
                    app.modelPreview.indices = std::move(pendingData->indices);
                    app.modelPreview.bones = std::move(pendingData->bones);
                    ResetModelCamera(app);
                }
                else
                {
                    App::SetModelPreviewUploadError(app, pendingData->generation, "Failed to upload model buffers to GPU.");
                }
            }
            else
            {
                App::SetModelPreviewUploadError(app, pendingData->generation, "Model renderer not initialized.");
            }
        }

        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        const ImVec2 defaultSize(
            std::clamp(mainViewport->WorkSize.x * 0.88f, 960.0f, 1600.0f),
            std::clamp(mainViewport->WorkSize.y * 0.88f, 640.0f, 1000.0f));

        const std::string windowTitle = "Model Viewer - " + app.modelPreview.entryName + "###ModelPreviewModal";
        if (!ImGui::IsPopupOpen(windowTitle.c_str()))
        {
            ImGui::OpenPopup(windowTitle.c_str());
        }

        ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(defaultSize, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(860.0f, 540.0f), ImVec2(FLT_MAX, FLT_MAX));
        if (app.modelPreview.focusRequested)
        {
            ImGui::SetNextWindowFocus();
            app.modelPreview.focusRequested = false;
        }

        bool isOpen = true;

        ImGuiIO& io = ImGui::GetIO();
        const bool previousMoveFromTitleBarOnly = io.ConfigWindowsMoveFromTitleBarOnly;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        const bool windowVisible = ImGui::BeginPopupModal(windowTitle.c_str(), &isOpen, ImGuiWindowFlags_NoCollapse);
        io.ConfigWindowsMoveFromTitleBarOnly = previousMoveFromTitleBarOnly;

        if (windowVisible)
        {
            // Determine asset type & pack metadata for studio header
            constexpr uint32_t kDynModelClassId = 0xB4B7D9C4u;
            const bool isDynModel = app.modelPreview.isDynModel ||
                (app.modelPreview.grpClassId == kDynModelClassId) ||
                AssetIndex::CanExportGrpEntryAsSkinnedModel(app.modelPreview.sourceEntry);
            const std::string className = app.modelPreview.grpClassId != 0
                ? AssetIndex::DescribeGrpClassId(app.modelPreview.grpClassId)
                : (isDynModel ? "DynModel" : "Model");

            UiIconId headerIcon = UiIconId::Invalid;
            if (isDynModel || className == "DynModel")
            {
                headerIcon = UiIconId::DynModel;
            }
            else if (className == "RendInst")
            {
                headerIcon = UiIconId::RendInst;
            }

            const std::string packFileName = Platform::PathToUtf8(app.modelPreview.packPath.filename());
            const char* exportLabel = isDynModel ? "Export Model..." : "Export GLB";
            const float exportButtonWidth = CalcIconButtonSize(exportLabel, GetUiIcon(UiIconId::Export) != nullptr).x + 12.0f;
            bool exportBusy = false;
            {
                std::lock_guard<std::mutex> lock(app.exportProgress.mutex);
                exportBusy = app.exportProgress.running.load();
            }

            // Studio Header Bar
            ImGui::BeginGroup();
            {
                DrawPanelTitle(headerIcon, app.modelPreview.entryName.empty() ? "Model Viewer" : app.modelPreview.entryName.c_str());
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("|  %s", className.c_str());
                if (!packFileName.empty())
                {
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("(%s)", packFileName.c_str());
                }
                if (app.modelPreview.vertexCount > 0)
                {
                    const uint32_t tris = app.modelPreview.indexCount / 3;
                    ImGui::SameLine(0.0f, 10.0f);
                    ImGui::AlignTextToFramePadding();
                    if (!app.modelPreview.bones.empty())
                    {
                        ImGui::TextColored(
                            ImVec4(0.62f, 0.72f, 0.84f, 1.0f),
                            "%u tris · %u parts · %zu bones",
                            tris,
                            app.modelPreview.subMeshCount,
                            app.modelPreview.bones.size());
                    }
                    else
                    {
                        ImGui::TextColored(
                            ImVec4(0.62f, 0.72f, 0.84f, 1.0f),
                            "%u tris · %u parts",
                            tris,
                            app.modelPreview.subMeshCount);
                    }
                }
            }
            ImGui::EndGroup();

            const float headerRightX = ImGui::GetWindowContentRegionMax().x - exportButtonWidth;
            if (headerRightX > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f)
            {
                ImGui::SameLine();
                ImGui::SetCursorPosX(headerRightX);
            }
            else
            {
                ImGui::SameLine();
            }

            const bool exportDisabled = exportBusy || app.modelPreview.loading || app.modelPreview.vertexCount == 0;
            ImGui::BeginDisabled(exportDisabled);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.78f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.53f, 0.88f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.38f, 0.68f, 1.0f));
            if (DrawIconTextButton("##ModelViewerExportGlb", UiIconId::Export, exportLabel, ImVec2(exportButtonWidth, 0.0f)))
            {
                if (isDynModel)
                {
                    if (app.modelPreview.sourceEntry.size > 0 && !app.modelPreview.sourceEntry.name.empty())
                    {
                        App::OpenDynModelExportModal(app, app.modelPreview.packPath, app.modelPreview.sourceEntry);
                    }
                    else
                    {
                        App::OpenDynModelExportModalByName(app, app.modelPreview.packPath, app.modelPreview.entryName);
                    }
                }
                else
                {
                    if (app.modelPreview.sourceEntry.size > 0 && !app.modelPreview.sourceEntry.name.empty())
                    {
                        App::RequestModelExport(app, app.modelPreview.packPath, app.modelPreview.sourceEntry);
                    }
                    else
                    {
                        App::RequestModelExportByName(app, app.modelPreview.packPath, app.modelPreview.entryName);
                    }
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            if (exportBusy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                DrawWrappedTooltip("Another export operation is currently running.");
            }

            // Top Viewport Toolbar
            ImGui::BeginDisabled(app.modelPreview.loading || app.modelPreview.vertexCount == 0);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

            if (ImGui::Button("Reset View"))
            {
                ResetModelCamera(app);
            }
            if (ImGui::IsItemHovered())
            {
                DrawWrappedTooltip("Reset camera framing (F / Home)");
            }

            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextDisabled("|");
            ImGui::SameLine(0.0f, 8.0f);

            ImGui::SetNextItemWidth(112.0f);
            ImGui::Combo("##RenderMode", &app.modelPreview.renderMode, "Solid\0Wireframe\0Solid + Wire\0");
            if (ImGui::IsItemHovered())
            {
                DrawWrappedTooltip("Viewport shading mode (W to cycle)");
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(114.0f);
            if (ImGui::Combo("##LightingPreset", &app.modelPreview.lightingPreset, "Studio\0Clay Sculpt\0Dark Rim\0High Key\0"))
            {
                ApplyModelLightingPreset(app.modelPreview.lighting, app.modelPreview.lightingPreset);
            }
            if (ImGui::IsItemHovered())
            {
                DrawWrappedTooltip("Viewport lighting preset");
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(104.0f);
            if (ImGui::BeginCombo("##CameraPreset", "Camera"))
            {
                struct CameraPresetItem
                {
                    const char* name;
                    float yaw;
                    float pitch;
                };
                constexpr CameraPresetItem kCameraPresets[] = {
                    { "Iso", 45.0f, 30.0f },
                    { "Front", 0.0f, 0.0f },
                    { "Back", 180.0f, 0.0f },
                    { "Left", -90.0f, 0.0f },
                    { "Right", 90.0f, 0.0f },
                    { "Top", 0.0f, 89.0f },
                    { "Bottom", 0.0f, -89.0f },
                };
                for (const CameraPresetItem& preset : kCameraPresets)
                {
                    if (ImGui::Selectable(preset.name, false))
                    {
                        app.modelPreview.yaw = preset.yaw;
                        app.modelPreview.pitch = preset.pitch;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
            {
                DrawWrappedTooltip("Snap camera to a preset angle");
            }

            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextDisabled("|");
            ImGui::SameLine(0.0f, 8.0f);

            if (DrawSegmentedButton("Grid", app.modelPreview.lighting.showGround))
            {
                app.modelPreview.lighting.showGround = !app.modelPreview.lighting.showGround;
            }
            if (ImGui::IsItemHovered())
            {
                DrawWrappedTooltip("Toggle ground grid & origin axes (G)");
            }

            ImGui::SameLine();
            const bool noBones = app.modelPreview.bones.empty();
            ImGui::BeginDisabled(noBones);
            if (DrawSegmentedButton("Bones", !noBones && app.modelPreview.showBones))
            {
                app.modelPreview.showBones = !app.modelPreview.showBones;
            }
            const bool bonesHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            ImGui::EndDisabled();
            if (bonesHovered)
            {
                DrawWrappedTooltip(noBones ? "No skeleton bone data in this model." : "Toggle skeleton rig overlay");
            }

            ImGui::SameLine();
            if (DrawSegmentedButton("Stats", app.modelPreview.showPerfOverlay))
            {
                app.modelPreview.showPerfOverlay = !app.modelPreview.showPerfOverlay;
            }
            if (ImGui::IsItemHovered())
            {
                DrawWrappedTooltip("Toggle live GPU, VRAM & FPS telemetry overlay");
            }

            char camReadout[64];
            snprintf(camReadout, sizeof(camReadout), "Yaw %.0f deg · Pitch %.0f deg", app.modelPreview.yaw, app.modelPreview.pitch);
            const float readoutW = ImGui::CalcTextSize(camReadout).x;
            const float readoutX = ImGui::GetWindowContentRegionMax().x - readoutW;
            if (readoutX > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 14.0f)
            {
                ImGui::SameLine();
                ImGui::SetCursorPosX(readoutX);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", camReadout);
            }

            ImGui::PopStyleVar(1);
            ImGui::EndDisabled();
            ImGui::Separator();

            if (app.modelPreview.loading)
            {
                ImGui::Spacing();
                const float phase =
                    0.15f + 0.7f * static_cast<float>((std::sin(ImGui::GetTime() * 3.0) + 1.0) * 0.5);
                ImGui::ProgressBar(phase, ImVec2(-1.0f, 0.0f), "Loading model geometry from package...");
            }
            else if (!app.modelPreview.error.empty())
            {
                ImGui::Spacing();
                DrawWrappedColoredText(
                    ImVec4(1.0f, 0.45f, 0.42f, 1.0f),
                    "Error loading model: " + app.modelPreview.error);
            }
            else if (app.modelPreview.vertexCount == 0)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("No model geometry loaded.");
            }
            else
            {
                const ImGuiTableFlags layoutFlags =
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_BordersInnerV |
                    ImGuiTableFlags_SizingStretchProp;

                if (ImGui::BeginTable("ModelViewerLayout", 2, layoutFlags))
                {
                    ImGui::TableSetupColumn("Viewport", ImGuiTableColumnFlags_WidthStretch, 0.74f);
                    ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthFixed, 310.0f);
                    ImGui::TableNextRow();

                    // Column 0: Edge-to-Edge 3D Viewport
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                    const bool viewportChildOpen = ImGui::BeginChild(
                        "3DViewportChild",
                        ImVec2(0.0f, 0.0f),
                        true,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    ImGui::PopStyleVar(1);

                    if (viewportChildOpen)
                    {
                        ImVec2 viewportSize = ImGui::GetContentRegionAvail();
                        if (viewportSize.x < 64.0f) viewportSize.x = 64.0f;
                        if (viewportSize.y < 64.0f) viewportSize.y = 64.0f;

                        const ImVec2 viewportScreenPos = ImGui::GetCursorScreenPos();
                        const ImVec2 gizmoCenter = ImVec2(
                            viewportScreenPos.x + viewportSize.x - 48.0f,
                            viewportScreenPos.y + 48.0f);

                        // Precompute camera vectors once for raycast, overlays, gizmo, and interactions
                        using namespace DirectX;
                        const float centerX = (app.modelPreview.bboxMin[0] + app.modelPreview.bboxMax[0]) * 0.5f;
                        const float centerY = (app.modelPreview.bboxMin[1] + app.modelPreview.bboxMax[1]) * 0.5f;
                        const float centerZ = (app.modelPreview.bboxMin[2] + app.modelPreview.bboxMax[2]) * 0.5f;

                        const XMVECTOR target = XMVectorSet(
                            centerX + app.modelPreview.panWorldX,
                            centerY + app.modelPreview.panWorldY,
                            centerZ + app.modelPreview.panWorldZ,
                            1.0f);

                        const float theta = app.modelPreview.yaw * XM_PI / 180.0f;
                        const float phi = app.modelPreview.pitch * XM_PI / 180.0f;
                        const float cosP = cosf(phi);
                        const float sinP = sinf(phi);
                        const float cosY = cosf(theta);
                        const float sinY = sinf(theta);

                        const XMVECTOR offsetVec = XMVectorSet(
                            app.modelPreview.zoom * cosP * sinY,
                            app.modelPreview.zoom * sinP,
                            -app.modelPreview.zoom * cosP * cosY,
                            0.0f);
                        const XMVECTOR cameraPos = target + offsetVec;
                        const XMVECTOR look = XMVector3Normalize(offsetVec);
                        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(look, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
                        const XMVECTOR up = XMVector3Normalize(XMVector3Cross(right, look));

                        XMFLOAT3 look3 = {}, right3 = {}, up3 = {};
                        XMStoreFloat3(&look3, look);
                        XMStoreFloat3(&right3, right);
                        XMStoreFloat3(&up3, up);

                        const XMMATRIX view = XMMatrixLookAtLH(cameraPos, target, up);
                        const float aspect = viewportSize.x / viewportSize.y;
                        const float nearPlane = std::max(0.01f, app.modelPreview.zoom * 0.01f);
                        const float farPlane = std::max(10.0f, app.modelPreview.zoom * 10.0f);
                        const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, nearPlane, farPlane);
                        const XMMATRIX viewProj = view * proj;

                        // Submesh RMB Raycast
                        const bool rightMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                        int raycastSubmeshIndex = -1;
                        if (rightMouseDown && !app.modelPreview.vertices.empty())
                        {
                            const float mx = io.MousePos.x - viewportScreenPos.x;
                            const float my = io.MousePos.y - viewportScreenPos.y;
                            const bool mouseInside =
                                mx >= 0.0f && mx < viewportSize.x &&
                                my >= 0.0f && my < viewportSize.y;

                            if (mouseInside)
                            {
                                const XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);
                                const float ndcX = (2.0f * mx) / viewportSize.x - 1.0f;
                                const float ndcY = 1.0f - (2.0f * my) / viewportSize.y;

                                const XMVECTOR worldNear = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
                                const XMVECTOR worldFar = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);

                                Ray ray;
                                ray.origin = worldNear;
                                ray.direction = XMVector3Normalize(worldFar - worldNear);

                                float bestT = FLT_MAX;
                                for (size_t subIdx = 0; subIdx < app.modelPreview.submeshes.size(); ++subIdx)
                                {
                                    const auto& submesh = app.modelPreview.submeshes[subIdx];
                                    float tNearBox = 0.0f;
                                    if (!RayAABBIntersection(ray, submesh.bboxMin, submesh.bboxMax, tNearBox) || tNearBox >= bestT)
                                    {
                                        continue;
                                    }

                                    bool hitTriangle = false;
                                    float submeshBestT = FLT_MAX;
                                    for (uint32_t i = 0; i + 2 < submesh.indexCount; i += 3)
                                    {
                                        const uint32_t idx0 = app.modelPreview.indices[submesh.startIndex + i];
                                        const uint32_t idx1 = app.modelPreview.indices[submesh.startIndex + i + 1];
                                        const uint32_t idx2 = app.modelPreview.indices[submesh.startIndex + i + 2];
                                        if (idx0 < app.modelPreview.vertices.size() &&
                                            idx1 < app.modelPreview.vertices.size() &&
                                            idx2 < app.modelPreview.vertices.size())
                                        {
                                            const XMVECTOR v0 = XMVectorSet(app.modelPreview.vertices[idx0].pos[0], app.modelPreview.vertices[idx0].pos[1], app.modelPreview.vertices[idx0].pos[2], 1.0f);
                                            const XMVECTOR v1 = XMVectorSet(app.modelPreview.vertices[idx1].pos[0], app.modelPreview.vertices[idx1].pos[1], app.modelPreview.vertices[idx1].pos[2], 1.0f);
                                            const XMVECTOR v2 = XMVectorSet(app.modelPreview.vertices[idx2].pos[0], app.modelPreview.vertices[idx2].pos[1], app.modelPreview.vertices[idx2].pos[2], 1.0f);
                                            float tTriangle = 0.0f;
                                            if (RayTriangleIntersection(ray, v0, v1, v2, tTriangle) && tTriangle < submeshBestT)
                                            {
                                                hitTriangle = true;
                                                submeshBestT = tTriangle;
                                            }
                                        }
                                    }

                                    if (hitTriangle && submeshBestT < bestT)
                                    {
                                        bestT = submeshBestT;
                                        raycastSubmeshIndex = static_cast<int>(subIdx);
                                    }
                                }
                            }
                        }

                        app.modelPreview.hoveredSubmeshIndex = raycastSubmeshIndex;
                        int effectiveHighlight = raycastSubmeshIndex;
                        if (effectiveHighlight < 0)
                        {
                            effectiveHighlight = (app.modelPreview.listHoveredSubmeshIndex >= 0)
                                ? app.modelPreview.listHoveredSubmeshIndex
                                : app.modelPreview.selectedSubmeshIndex;
                        }

                        ModelRenderer* renderer = GetModelRenderer();
                        ID3D11ShaderResourceView* srv = nullptr;
                        if (renderer)
                        {
                            const auto submitStart = std::chrono::high_resolution_clock::now();
                            srv = renderer->Render(
                                app.modelPreview.yaw,
                                app.modelPreview.pitch,
                                app.modelPreview.zoom,
                                app.modelPreview.panWorldX,
                                app.modelPreview.panWorldY,
                                app.modelPreview.panWorldZ,
                                app.modelPreview.renderMode,
                                app.modelPreview.bboxMin,
                                app.modelPreview.bboxMax,
                                static_cast<uint32_t>(viewportSize.x),
                                static_cast<uint32_t>(viewportSize.y),
                                app.modelPreview.lighting,
                                effectiveHighlight,
                                app.modelPreview.submeshes);
                            const auto submitEnd = std::chrono::high_resolution_clock::now();
                            const float submitMs = std::chrono::duration<float, std::milli>(submitEnd - submitStart).count();

                            app.modelPreview.cpuSubmitMs = submitMs;
                            app.modelPreview.gpuTimingHardware = renderer->HasHardwareGpuTiming();
                            const float gpuMs = app.modelPreview.gpuTimingHardware
                                ? renderer->GetLastGpuTimeMs()
                                : submitMs;
                            app.modelPreview.renderLatencyMs = gpuMs;
                            app.modelPreview.lastDrawCalls = renderer->GetLastDrawCallCount();
                            app.modelPreview.msaaSampleCount = renderer->GetMsaaSampleCount();

                            app.modelPreview.fpsHistory[app.modelPreview.statOffset] = io.Framerate;
                            app.modelPreview.latencyHistory[app.modelPreview.statOffset] = gpuMs;
                            app.modelPreview.gpuTimeHistory[app.modelPreview.statOffset] = gpuMs;
                            app.modelPreview.frameTimeHistory[app.modelPreview.statOffset] = io.DeltaTime * 1000.0f;
                            app.modelPreview.statOffset = (app.modelPreview.statOffset + 1) % 90;

                            const double nowSec = ImGui::GetTime();
                            if (app.modelPreview.lastVramPollTime < 0.0 || (nowSec - app.modelPreview.lastVramPollTime) >= 0.25)
                            {
                                renderer->GetVRAMInfo(app.modelPreview.cachedVramUsage, app.modelPreview.cachedVramBudget);
                                app.modelPreview.lastVramPollTime = nowSec;
                            }
                        }

                        if (srv)
                        {
                            ImGui::Image(reinterpret_cast<ImTextureID>(srv), viewportSize);
                            const ImVec2 imageMin = ImGui::GetItemRectMin();
                            const ImVec2 imageMax = ImGui::GetItemRectMax();
                            ImDrawList* drawList = ImGui::GetWindowDrawList();

                            // Bone Armature Overlay
                            if (app.modelPreview.showBones && !app.modelPreview.bones.empty())
                            {
                                std::vector<ImVec2> boneScreenCoords(app.modelPreview.bones.size());
                                std::vector<bool> boneValid(app.modelPreview.bones.size(), false);

                                for (size_t i = 0; i < app.modelPreview.bones.size(); ++i)
                                {
                                    const auto& bone = app.modelPreview.bones[i];
                                    const XMVECTOR center3D = XMVectorSet(bone.worldPos[0], bone.worldPos[1], bone.worldPos[2], 1.0f);
                                    const XMVECTOR dotProduct = XMVector3Dot(center3D - cameraPos, look);
                                    if (XMVectorGetX(dotProduct) < 0.0f)
                                    {
                                        const XMVECTOR projected = XMVector3TransformCoord(center3D, viewProj);
                                        const float projX = XMVectorGetX(projected);
                                        const float projY = XMVectorGetY(projected);
                                        if (projX >= -2.0f && projX <= 2.0f && projY >= -2.0f && projY <= 2.0f)
                                        {
                                            boneScreenCoords[i].x = imageMin.x + (projX + 1.0f) * 0.5f * viewportSize.x;
                                            boneScreenCoords[i].y = imageMin.y + (1.0f - projY) * 0.5f * viewportSize.y;
                                            boneValid[i] = true;
                                        }
                                    }
                                }

                                for (size_t i = 0; i < app.modelPreview.bones.size(); ++i)
                                {
                                    const auto& bone = app.modelPreview.bones[i];
                                    if (bone.parentIndex != 0xFFFFu && bone.parentIndex < app.modelPreview.bones.size())
                                    {
                                        if (boneValid[i] && boneValid[bone.parentIndex])
                                        {
                                            drawList->AddLine(boneScreenCoords[i], boneScreenCoords[bone.parentIndex], IM_COL32(72, 224, 240, 185), 1.8f);
                                        }
                                    }
                                }

                                for (size_t i = 0; i < app.modelPreview.bones.size(); ++i)
                                {
                                    if (boneValid[i])
                                    {
                                        drawList->AddCircleFilled(boneScreenCoords[i], 3.0f, IM_COL32(115, 245, 145, 230));
                                    }
                                }
                            }

                            // Crisp Submesh Highlight Label Pill (when RMB inspecting or Parts tab hover/selection)
                            if (effectiveHighlight >= 0 && effectiveHighlight < static_cast<int>(app.modelPreview.submeshes.size()))
                            {
                                const auto& submesh = app.modelPreview.submeshes[effectiveHighlight];
                                const XMVECTOR subCenter = XMVectorSet(
                                    (submesh.bboxMin[0] + submesh.bboxMax[0]) * 0.5f,
                                    (submesh.bboxMin[1] + submesh.bboxMax[1]) * 0.5f,
                                    (submesh.bboxMin[2] + submesh.bboxMax[2]) * 0.5f,
                                    1.0f);

                                if (XMVectorGetX(XMVector3Dot(subCenter - cameraPos, look)) < 0.0f)
                                {
                                    const XMVECTOR projected = XMVector3TransformCoord(subCenter, viewProj);
                                    const float projX = XMVectorGetX(projected);
                                    const float projY = XMVectorGetY(projected);
                                    if (projX >= -1.05f && projX <= 1.05f && projY >= -1.05f && projY <= 1.05f)
                                    {
                                        const float screenX = imageMin.x + (projX + 1.0f) * 0.5f * viewportSize.x;
                                        const float screenY = imageMin.y + (1.0f - projY) * 0.5f * viewportSize.y;
                                        char labelBuf[256];
                                        snprintf(
                                            labelBuf,
                                            sizeof(labelBuf),
                                            "%s · %u tris",
                                            submesh.name.c_str(),
                                            submesh.indexCount / 3);
                                        const ImVec2 textSize = ImGui::CalcTextSize(labelBuf);
                                        const float padX = 9.0f;
                                        const float padY = 4.0f;
                                        const ImVec2 pillMin(
                                            std::clamp(screenX - textSize.x * 0.5f - padX, imageMin.x + 8.0f, std::max(imageMin.x + 8.0f, imageMax.x - textSize.x - padX * 2.0f - 8.0f)),
                                            std::clamp(screenY - textSize.y * 0.5f - padY, imageMin.y + 8.0f, std::max(imageMin.y + 8.0f, imageMax.y - textSize.y - padY * 2.0f - 8.0f)));
                                        const ImVec2 pillMax(pillMin.x + textSize.x + padX * 2.0f, pillMin.y + textSize.y + padY * 2.0f);
                                        drawList->AddRectFilled(pillMin, pillMax, IM_COL32(16, 19, 24, 228), 5.0f);
                                        drawList->AddRect(pillMin, pillMax, IM_COL32(255, 218, 60, 215), 5.0f, 0, 1.2f);
                                        drawList->AddText(ImVec2(pillMin.x + padX, pillMin.y + padY), IM_COL32(250, 250, 252, 255), labelBuf);
                                    }
                                }
                            }

                            // Base Viewport Interaction Layer
                            ImGui::SetCursorScreenPos(imageMin);
                            ImGui::InvisibleButton("ViewportInteract", viewportSize);
                            const bool viewportHovered = ImGui::IsItemHovered();
                            const bool viewportActive = ImGui::IsItemActive();

                            // Simple Top-Right 3D Direction Indicator
                            struct GizmoAxisItem
                            {
                                ImVec2 end;
                                ImVec2 delta;
                                float depth;
                                ImU32 color;
                                const char* label;
                            };

                            constexpr float axisLen = 28.0f;
                            auto ProjectAxisEnd = [&](float ax, float ay, float az, ImVec2& deltaOut) -> ImVec2
                            {
                                const float sx = ax * right3.x + ay * right3.y + az * right3.z;
                                const float sy = ax * up3.x + ay * up3.y + az * up3.z;
                                deltaOut = ImVec2(sx * axisLen, -sy * axisLen);
                                return ImVec2(gizmoCenter.x + deltaOut.x, gizmoCenter.y + deltaOut.y);
                            };

                            ImVec2 dx{}, dy{}, dz{};
                            GizmoAxisItem gizmoAxes[3] = {
                                { ProjectAxisEnd(1.0f, 0.0f, 0.0f, dx), dx, look3.x, IM_COL32(226,  82,  82, 255), "X" },
                                { ProjectAxisEnd(0.0f, 1.0f, 0.0f, dy), dy, look3.y, IM_COL32( 86, 198,  96, 255), "Y" },
                                { ProjectAxisEnd(0.0f, 0.0f, 1.0f, dz), dz, look3.z, IM_COL32( 78, 146, 245, 255), "Z" }
                            };

                            std::sort(std::begin(gizmoAxes), std::end(gizmoAxes), [](const GizmoAxisItem& a, const GizmoAxisItem& b) {
                                return a.depth < b.depth;
                            });

                            drawList->AddCircleFilled(gizmoCenter, 2.5f, IM_COL32(190, 198, 212, 180));
                            for (const GizmoAxisItem& axis : gizmoAxes)
                            {
                                drawList->AddLine(gizmoCenter, axis.end, axis.color, 2.0f);
                                const float len = std::sqrt(axis.delta.x * axis.delta.x + axis.delta.y * axis.delta.y);
                                const float nx = len > 1e-3f ? axis.delta.x / len : 0.0f;
                                const float ny = len > 1e-3f ? axis.delta.y / len : 0.0f;
                                const ImVec2 labelCenter(axis.end.x + nx * 9.0f, axis.end.y + ny * 9.0f);
                                const ImVec2 ts = ImGui::CalcTextSize(axis.label);
                                drawList->AddText(
                                    ImVec2(labelCenter.x - ts.x * 0.5f, labelCenter.y - ts.y * 0.5f),
                                    axis.color,
                                    axis.label);
                            }

                            // Scroll Zoom
                            if (viewportHovered && io.MouseWheel != 0.0f)
                            {
                                const float zoomSpeed = std::max(0.1f, app.modelPreview.zoom * 0.15f);
                                app.modelPreview.zoom = std::max(0.01f, app.modelPreview.zoom - io.MouseWheel * zoomSpeed);
                            }

                            // Middle Mouse Button Drag State
                            static bool draggingMiddle = false;
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && viewportHovered)
                            {
                                draggingMiddle = true;
                            }
                            if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                            {
                                draggingMiddle = false;
                            }

                            if (draggingMiddle)
                            {
                                if (io.KeyShift)
                                {
                                    const float panSpeed = std::max(0.001f, app.modelPreview.zoom * 0.002f);
                                    const float dxPan = -io.MouseDelta.x * panSpeed;
                                    const float dyPan = io.MouseDelta.y * panSpeed;
                                    app.modelPreview.panWorldX += right3.x * dxPan + up3.x * dyPan;
                                    app.modelPreview.panWorldY += right3.y * dxPan + up3.y * dyPan;
                                    app.modelPreview.panWorldZ += right3.z * dxPan + up3.z * dyPan;
                                }
                                else if (io.KeyCtrl)
                                {
                                    const float zoomSpeed = std::max(0.1f, app.modelPreview.zoom * 0.005f);
                                    app.modelPreview.zoom = std::max(0.01f, app.modelPreview.zoom - io.MouseDelta.y * zoomSpeed);
                                }
                                else
                                {
                                    app.modelPreview.yaw -= io.MouseDelta.x * 0.5f;
                                    app.modelPreview.pitch = std::clamp(app.modelPreview.pitch + io.MouseDelta.y * 0.5f, -89.0f, 89.0f);
                                }
                            }
                            else if (viewportActive)
                            {
                                if (io.KeyShift)
                                {
                                    const float panSpeed = std::max(0.001f, app.modelPreview.zoom * 0.002f);
                                    const float dxPan = -io.MouseDelta.x * panSpeed;
                                    const float dyPan = io.MouseDelta.y * panSpeed;
                                    app.modelPreview.panWorldX += right3.x * dxPan + up3.x * dyPan;
                                    app.modelPreview.panWorldY += right3.y * dxPan + up3.y * dyPan;
                                    app.modelPreview.panWorldZ += right3.z * dxPan + up3.z * dyPan;
                                }
                                else
                                {
                                    app.modelPreview.yaw -= io.MouseDelta.x * 0.5f;
                                    app.modelPreview.pitch = std::clamp(app.modelPreview.pitch + io.MouseDelta.y * 0.5f, -89.0f, 89.0f);
                                }
                            }

                            // Keyboard Shortcuts when Model Viewer is active and not typing
                            if (!io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                            {
                                if (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_Home, false))
                                {
                                    ResetModelCamera(app);
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_G, false))
                                {
                                    app.modelPreview.lighting.showGround = !app.modelPreview.lighting.showGround;
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_W, false))
                                {
                                    app.modelPreview.renderMode = (app.modelPreview.renderMode + 1) % 3;
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_1, false))
                                {
                                    app.modelPreview.yaw = 0.0f;
                                    app.modelPreview.pitch = 0.0f;
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_3, false))
                                {
                                    app.modelPreview.yaw = 90.0f;
                                    app.modelPreview.pitch = 0.0f;
                                }
                                else if (ImGui::IsKeyPressed(ImGuiKey_7, false))
                                {
                                    app.modelPreview.yaw = 0.0f;
                                    app.modelPreview.pitch = 89.0f;
                                }
                            }

                            // Optional Sleek Hardware Performance Telemetry HUD (Top-Left, toggled via "Stats" button)
                            if (app.modelPreview.showPerfOverlay)
                            {
                                ImGui::SetCursorScreenPos(ImVec2(imageMin.x + 12.0f, imageMin.y + 12.0f));
                                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.07f, 0.095f, 0.86f));
                                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.34f, 0.72f));
                                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f, 8.0f));

                                if (ImGui::BeginChild(
                                        "PerfOverlay",
                                        ImVec2(276.0f, 200.0f),
                                        true,
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                                {
                                    const UiIcon* debugIcon = GetUiIcon(UiIconId::Debug);
                                    if (debugIcon && debugIcon->view != nullptr)
                                    {
                                        ImGui::Image(reinterpret_cast<ImTextureID>(debugIcon->view), ImVec2(13.0f, 13.0f));
                                        ImGui::SameLine(0.0f, 5.0f);
                                    }
                                    ImGui::TextDisabled("GPU TELEMETRY");
                                    char msaaText[24];
                                    snprintf(msaaText, sizeof(msaaText), "%ux MSAA", app.modelPreview.msaaSampleCount);
                                    const float msaaW = ImGui::CalcTextSize(msaaText).x;
                                    ImGui::SameLine();
                                    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - msaaW));
                                    ImGui::TextDisabled("%s", msaaText);

                                    std::string gpuName = renderer ? renderer->GetGPUName() : "Unknown GPU";
                                    if (gpuName.length() > 36)
                                    {
                                        gpuName = gpuName.substr(0, 33) + "...";
                                    }
                                    ImGui::TextUnformatted(gpuName.c_str());

                                    ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.25f, 1.0f), "FPS: %.0f", io.Framerate);
                                    ImGui::SameLine(0.0f, 10.0f);
                                    ImGui::TextColored(
                                        ImVec4(0.30f, 0.88f, 0.96f, 1.0f),
                                        "%s: %.2f ms",
                                        app.modelPreview.gpuTimingHardware ? "GPU" : "Submit",
                                        app.modelPreview.renderLatencyMs);
                                    ImGui::SameLine(0.0f, 10.0f);
                                    ImGui::TextColored(ImVec4(0.45f, 0.68f, 1.0f, 1.0f), "Frame: %.1f ms", io.DeltaTime * 1000.0f);

                                    float maxMs = 1.0f;
                                    for (int i = 0; i < 90; ++i)
                                    {
                                        maxMs = std::max(maxMs, app.modelPreview.frameTimeHistory[i]);
                                        maxMs = std::max(maxMs, app.modelPreview.gpuTimeHistory[i]);
                                    }
                                    maxMs *= 1.15f;

                                    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                                    const ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, 54.0f);
                                    ImDrawList* hudDraw = ImGui::GetWindowDrawList();

                                    hudDraw->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(12, 14, 18, 230), 0.0f);
                                    hudDraw->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(48, 54, 64, 255), 0.0f);

                                    for (int g = 1; g < 3; ++g)
                                    {
                                        const float gy = canvasPos.y + (canvasSize.y / 3.0f) * static_cast<float>(g);
                                        hudDraw->AddLine(ImVec2(canvasPos.x, gy), ImVec2(canvasPos.x + canvasSize.x, gy), IM_COL32(45, 50, 60, 110));
                                    }

                                    for (int i = 0; i < 89; ++i)
                                    {
                                        const int idx0 = (app.modelPreview.statOffset + i) % 90;
                                        const int idx1 = (app.modelPreview.statOffset + i + 1) % 90;
                                        const float x0 = canvasPos.x + (static_cast<float>(i) / 89.0f) * canvasSize.x;
                                        const float x1 = canvasPos.x + (static_cast<float>(i + 1) / 89.0f) * canvasSize.x;

                                        const float f0 = std::clamp(app.modelPreview.frameTimeHistory[idx0] / maxMs, 0.0f, 1.0f);
                                        const float f1 = std::clamp(app.modelPreview.frameTimeHistory[idx1] / maxMs, 0.0f, 1.0f);
                                        hudDraw->AddQuadFilled(
                                            ImVec2(x0, canvasPos.y + canvasSize.y),
                                            ImVec2(x0, canvasPos.y + canvasSize.y * (1.0f - f0)),
                                            ImVec2(x1, canvasPos.y + canvasSize.y * (1.0f - f1)),
                                            ImVec2(x1, canvasPos.y + canvasSize.y),
                                            IM_COL32(75, 130, 235, 45));

                                        const float g0 = std::clamp(app.modelPreview.gpuTimeHistory[idx0] / maxMs, 0.0f, 1.0f);
                                        const float g1 = std::clamp(app.modelPreview.gpuTimeHistory[idx1] / maxMs, 0.0f, 1.0f);
                                        const float gy0 = canvasPos.y + canvasSize.y * (1.0f - g0);
                                        const float gy1 = canvasPos.y + canvasSize.y * (1.0f - g1);
                                        hudDraw->AddQuadFilled(
                                            ImVec2(x0, canvasPos.y + canvasSize.y),
                                            ImVec2(x0, gy0),
                                            ImVec2(x1, gy1),
                                            ImVec2(x1, canvasPos.y + canvasSize.y),
                                            IM_COL32(45, 220, 240, 65));
                                        hudDraw->AddLine(ImVec2(x0, gy0), ImVec2(x1, gy1), IM_COL32(75, 232, 250, 220), 1.4f);
                                    }

                                    char peakBuf[24];
                                    snprintf(peakBuf, sizeof(peakBuf), "%.1f ms", maxMs);
                                    const ImVec2 peakSize = ImGui::CalcTextSize(peakBuf);
                                    hudDraw->AddText(
                                        ImVec2(canvasPos.x + canvasSize.x - peakSize.x - 4.0f, canvasPos.y + 2.0f),
                                        IM_COL32(165, 175, 190, 190),
                                        peakBuf);

                                    ImGui::Dummy(canvasSize);
                                    ImGui::Spacing();

                                    const float usageMb = static_cast<float>(app.modelPreview.cachedVramUsage) / (1024.0f * 1024.0f);
                                    const float budgetMb = static_cast<float>(app.modelPreview.cachedVramBudget) / (1024.0f * 1024.0f);
                                    char vramText[64];
                                    if (budgetMb > 0.0f)
                                    {
                                        const float fraction = std::clamp(usageMb / budgetMb, 0.0f, 1.0f);
                                        snprintf(vramText, sizeof(vramText), "VRAM: %.0f / %.0f MB (%.0f%%)", usageMb, budgetMb, fraction * 100.0f);
                                        ImVec4 barCol(0.22f, 0.72f, 0.40f, 0.88f);
                                        if (fraction >= 0.88f) barCol = ImVec4(0.86f, 0.28f, 0.26f, 0.90f);
                                        else if (fraction >= 0.70f) barCol = ImVec4(0.88f, 0.66f, 0.22f, 0.90f);
                                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
                                        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 16.0f), vramText);
                                        ImGui::PopStyleColor();
                                    }
                                    else
                                    {
                                        snprintf(vramText, sizeof(vramText), "VRAM Usage: %.1f MB", usageMb);
                                        ImGui::ProgressBar(0.0f, ImVec2(-1.0f, 16.0f), vramText);
                                    }

                                    const uint32_t triangleCount = app.modelPreview.indexCount / 3;
                                    ImGui::TextDisabled(
                                        "Draws: %u  ·  %u tris  ·  %u verts",
                                        app.modelPreview.lastDrawCalls,
                                        triangleCount,
                                        app.modelPreview.vertexCount);
                                }
                                ImGui::EndChild();
                                ImGui::PopStyleVar(1);
                                ImGui::PopStyleColor(2);
                            }

                            // Unobtrusive Bottom-Left Controls Hint Pill
                            {
                                const char* hintText = "Drag: Orbit  ·  Shift+Drag: Pan  ·  Wheel: Zoom  ·  RMB: Inspect Part  ·  F: Reset";
                                const ImVec2 hintSize = ImGui::CalcTextSize(hintText);
                                const float padX = 9.0f;
                                const float padY = 4.0f;
                                if (viewportSize.x > hintSize.x + padX * 2.0f + 24.0f && viewportSize.y > 60.0f)
                                {
                                    const ImVec2 pillMin(imageMin.x + 12.0f, imageMax.y - hintSize.y - padY * 2.0f - 10.0f);
                                    const ImVec2 pillMax(pillMin.x + hintSize.x + padX * 2.0f, pillMin.y + hintSize.y + padY * 2.0f);
                                    drawList->AddRectFilled(pillMin, pillMax, IM_COL32(14, 16, 20, 155), 0.0f);
                                    drawList->AddText(ImVec2(pillMin.x + padX, pillMin.y + padY), IM_COL32(180, 188, 202, 210), hintText);
                                }
                            }
                        }
                        else
                        {
                            ImGui::SetCursorPos(ImVec2(14.0f, 14.0f));
                            ImGui::TextDisabled("D3D11 viewport rendering unavailable.");
                        }
                    }
                    ImGui::EndChild();

                    // Column 1: Tabbed Studio Inspector
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::BeginChild("ModelInspectorChild", ImVec2(0.0f, 0.0f), false))
                    {
                        if (ImGui::BeginTabBar("ModelInspectorTabs"))
                        {
                            bool partsTabOpen = false;
                            if (ImGui::BeginTabItem("Overview"))
                            {
                                ImGui::Spacing();
                                if (ImGui::CollapsingHeader("Asset", ImGuiTreeNodeFlags_DefaultOpen))
                                {
                                    if (ImGui::BeginTable(
                                            "ModelAssetDetails",
                                            2,
                                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                                    {
                                        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                                        DrawTextureDetailRow("Name", app.modelPreview.entryName);
                                        DrawTextureDetailRow("Class", className);
                                        DrawTextureDetailRow("GRP Pack", packFileName);
                                        DrawTextureDetailRow("Payload", FormatBytes(app.modelPreview.fileSize));
                                        ImGui::EndTable();
                                    }
                                }

                                ImGui::Spacing();
                                if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen))
                                {
                                    if (ImGui::BeginTable(
                                            "ModelGeoDetails",
                                            2,
                                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                                    {
                                        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                                        DrawTextureDetailRow("Vertices", std::to_string(app.modelPreview.vertexCount));
                                        DrawTextureDetailRow("Triangles", std::to_string(app.modelPreview.indexCount / 3));
                                        DrawTextureDetailRow("Indices", std::to_string(app.modelPreview.indexCount));
                                        DrawTextureDetailRow("Submeshes", std::to_string(app.modelPreview.subMeshCount));
                                        DrawTextureDetailRow("Bones", std::to_string(app.modelPreview.bones.size()));
                                        ImGui::EndTable();
                                    }
                                }

                                ImGui::Spacing();
                                if (ImGui::CollapsingHeader("Bounds", ImGuiTreeNodeFlags_DefaultOpen))
                                {
                                    const float sizeX = app.modelPreview.bboxMax[0] - app.modelPreview.bboxMin[0];
                                    const float sizeY = app.modelPreview.bboxMax[1] - app.modelPreview.bboxMin[1];
                                    const float sizeZ = app.modelPreview.bboxMax[2] - app.modelPreview.bboxMin[2];
                                    char buf[96];
                                    if (ImGui::BeginTable(
                                            "ModelBoundsDetails",
                                            2,
                                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                                    {
                                        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                                        snprintf(buf, sizeof(buf), "%.2f x %.2f x %.2f m", sizeX, sizeY, sizeZ);
                                        DrawTextureDetailRow("Size", buf);
                                        snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", app.modelPreview.bboxMin[0], app.modelPreview.bboxMin[1], app.modelPreview.bboxMin[2]);
                                        DrawTextureDetailRow("Min", buf);
                                        snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", app.modelPreview.bboxMax[0], app.modelPreview.bboxMax[1], app.modelPreview.bboxMax[2]);
                                        DrawTextureDetailRow("Max", buf);
                                        ImGui::EndTable();
                                    }
                                }

                                ImGui::Spacing();
                                ImGui::Separator();
                                ImGui::Spacing();
                                if (DrawIconTextButton("##ShowGrpInExplorer", UiIconId::ContentBrowser, "Show GRP in Explorer", ImVec2(-1.0f, 0.0f)))
                                {
                                    Platform::ShowPathInExplorer(app.modelPreview.packPath);
                                }

                                ImGui::EndTabItem();
                            }

                            if (ImGui::BeginTabItem("Parts"))
                            {
                                partsTabOpen = true;
                                app.modelPreview.listHoveredSubmeshIndex = -1;

                                ImGui::Spacing();
                                char partsHeader[64];
                                snprintf(partsHeader, sizeof(partsHeader), "Mesh Parts (%zu)", app.modelPreview.submeshes.size());
                                if (ImGui::CollapsingHeader(partsHeader, ImGuiTreeNodeFlags_DefaultOpen))
                                {
                                    ImGui::SetNextItemWidth(app.modelPreview.selectedSubmeshIndex >= 0 ? -78.0f : -1.0f);
                                    ImGui::InputTextWithHint(
                                        "##PartFilter",
                                        "Filter submeshes...",
                                        app.modelPreview.partSearchBuffer,
                                        sizeof(app.modelPreview.partSearchBuffer));
                                    if (app.modelPreview.selectedSubmeshIndex >= 0)
                                    {
                                        ImGui::SameLine();
                                        if (ImGui::Button("Clear", ImVec2(-1.0f, 0.0f)))
                                        {
                                            app.modelPreview.selectedSubmeshIndex = -1;
                                        }
                                    }

                                    const std::string partFilterLower = AssetIndex::ToLowerAscii(app.modelPreview.partSearchBuffer);
                                    std::vector<size_t> filteredSubmeshes;
                                    filteredSubmeshes.reserve(app.modelPreview.submeshes.size());
                                    for (size_t i = 0; i < app.modelPreview.submeshes.size(); ++i)
                                    {
                                        if (partFilterLower.empty() ||
                                            AssetIndex::ToLowerAscii(app.modelPreview.submeshes[i].name).find(partFilterLower) != std::string::npos)
                                        {
                                            filteredSubmeshes.push_back(i);
                                        }
                                    }

                                    ImGui::TextDisabled(
                                        "Showing %zu / %zu parts (hover or click to highlight)",
                                        filteredSubmeshes.size(),
                                        app.modelPreview.submeshes.size());

                                    const float submeshTableHeight = app.modelPreview.bones.empty() ? -4.0f : std::min(260.0f, ImGui::GetContentRegionAvail().y * 0.55f);
                                    if (ImGui::BeginTable(
                                            "ModelPartsTable",
                                            2,
                                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                            ImVec2(0.0f, submeshTableHeight)))
                                    {
                                        ImGui::TableSetupScrollFreeze(0, 1);
                                        ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthStretch);
                                        ImGui::TableSetupColumn("Tris", ImGuiTableColumnFlags_WidthFixed, 58.0f);
                                        ImGui::TableHeadersRow();

                                        ImGuiListClipper clipper;
                                        clipper.Begin(static_cast<int>(filteredSubmeshes.size()));
                                        while (clipper.Step())
                                        {
                                            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                                            {
                                                const size_t subIdx = filteredSubmeshes[static_cast<size_t>(row)];
                                                const auto& submesh = app.modelPreview.submeshes[subIdx];
                                                const bool isSelected = (app.modelPreview.selectedSubmeshIndex == static_cast<int>(subIdx));

                                                ImGui::PushID(static_cast<int>(subIdx));
                                                ImGui::TableNextRow();
                                                ImGui::TableSetColumnIndex(0);
                                                const std::string displayName = submesh.name.empty() ? ("Part #" + std::to_string(subIdx)) : submesh.name;
                                                if (ImGui::Selectable(displayName.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns))
                                                {
                                                    app.modelPreview.selectedSubmeshIndex = isSelected ? -1 : static_cast<int>(subIdx);
                                                }
                                                if (ImGui::IsItemHovered())
                                                {
                                                    app.modelPreview.listHoveredSubmeshIndex = static_cast<int>(subIdx);
                                                }
                                                ImGui::TableSetColumnIndex(1);
                                                ImGui::TextDisabled("%u", submesh.indexCount / 3);
                                                ImGui::PopID();
                                            }
                                        }
                                        clipper.End();
                                        ImGui::EndTable();
                                    }
                                }

                                if (!app.modelPreview.bones.empty())
                                {
                                    ImGui::Spacing();
                                    char boneHeader[64];
                                    snprintf(boneHeader, sizeof(boneHeader), "Skeleton Bones (%zu)", app.modelPreview.bones.size());
                                    if (ImGui::CollapsingHeader(boneHeader, ImGuiTreeNodeFlags_DefaultOpen))
                                    {
                                        ImGui::SetNextItemWidth(-1.0f);
                                        ImGui::InputTextWithHint(
                                            "##BoneFilter",
                                            "Filter bones...",
                                            app.modelPreview.boneSearchBuffer,
                                            sizeof(app.modelPreview.boneSearchBuffer));

                                        const std::string boneFilterLower = AssetIndex::ToLowerAscii(app.modelPreview.boneSearchBuffer);
                                        std::vector<size_t> filteredBones;
                                        filteredBones.reserve(app.modelPreview.bones.size());
                                        for (size_t i = 0; i < app.modelPreview.bones.size(); ++i)
                                        {
                                            if (boneFilterLower.empty() ||
                                                AssetIndex::ToLowerAscii(app.modelPreview.bones[i].name).find(boneFilterLower) != std::string::npos)
                                            {
                                                filteredBones.push_back(i);
                                            }
                                        }

                                        ImGui::TextDisabled(
                                            "Showing %zu / %zu bones",
                                            filteredBones.size(),
                                            app.modelPreview.bones.size());

                                        if (ImGui::BeginTable(
                                                "ModelBonesTable",
                                                2,
                                                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                                ImVec2(0.0f, -4.0f)))
                                        {
                                            ImGui::TableSetupScrollFreeze(0, 1);
                                            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 34.0f);
                                            ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthStretch);
                                            ImGui::TableHeadersRow();

                                            ImGuiListClipper boneClipper;
                                            boneClipper.Begin(static_cast<int>(filteredBones.size()));
                                            while (boneClipper.Step())
                                            {
                                                for (int row = boneClipper.DisplayStart; row < boneClipper.DisplayEnd; ++row)
                                                {
                                                    const size_t boneIdx = filteredBones[static_cast<size_t>(row)];
                                                    const auto& bone = app.modelPreview.bones[boneIdx];
                                                    ImGui::TableNextRow();
                                                    ImGui::TableSetColumnIndex(0);
                                                    ImGui::TextDisabled("%zu", boneIdx);
                                                    ImGui::TableSetColumnIndex(1);
                                                    ImGui::TextUnformatted(bone.name.empty() ? "<unnamed>" : bone.name.c_str());
                                                }
                                            }
                                            boneClipper.End();
                                            ImGui::EndTable();
                                        }
                                    }
                                }

                                ImGui::EndTabItem();
                            }

                            if (!partsTabOpen)
                            {
                                app.modelPreview.listHoveredSubmeshIndex = -1;
                            }

                            if (ImGui::BeginTabItem("Scene"))
                            {
                                ImGui::Spacing();
                                if (ImGui::CollapsingHeader("Light & Surface", ImGuiTreeNodeFlags_DefaultOpen))
                                {
                                    ImGui::SliderFloat("Light Yaw", &app.modelPreview.lighting.lightYaw, -180.0f, 180.0f, "%.0f deg");
                                    ImGui::SliderFloat("Light Pitch", &app.modelPreview.lighting.lightPitch, -90.0f, 90.0f, "%.0f deg");
                                    ImGui::SliderFloat("Glossiness", &app.modelPreview.lighting.specPower, 1.0f, 128.0f, "%.0f");
                                    ImGui::SliderFloat("Specular", &app.modelPreview.lighting.specIntensity, 0.0f, 2.0f, "%.2f");
                                }

                                ImGui::Spacing();
                                if (ImGui::CollapsingHeader("Colors & Backdrop", ImGuiTreeNodeFlags_DefaultOpen))
                                {
                                    ImGui::ColorEdit3("Key Light", app.modelPreview.lighting.keyColor);
                                    ImGui::ColorEdit3("Fill Light", app.modelPreview.lighting.fillColor);
                                    ImGui::ColorEdit3("Ambient", app.modelPreview.lighting.ambientColor);
                                    ImGui::ColorEdit3("Model Clay", app.modelPreview.lighting.baseColor);
                                    ImGui::ColorEdit3("Background", app.modelPreview.lighting.backgroundColor);
                                    ImGui::Checkbox("Show Ground Grid", &app.modelPreview.lighting.showGround);
                                }

                                ImGui::Spacing();
                                if (ImGui::Button("Reset Scene", ImVec2(-1.0f, 0.0f)))
                                {
                                    app.modelPreview.lightingPreset = 0;
                                    app.modelPreview.lighting = App::ModelPreviewLighting();
                                }

                                ImGui::EndTabItem();
                            }

                            ImGui::EndTabBar();
                        }
                    }
                    ImGui::EndChild();

                    ImGui::EndTable();
                }
            }

            DrawDynModelExportModal(app);
            DrawExportProgressModal(app);
            if (!isOpen)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (!isOpen)
        {
            app.modelPreview.popupOpen = false;
            App::ClearModelPreview(app);
            ModelRenderer* renderer = GetModelRenderer();
            if (renderer)
            {
                renderer->ClearModelData();
            }
        }
    }

    void DrawMainShell(AppState& app, bool& running)
    {
        App::JoinFinishedScan(app);
        App::JoinFinishedBackgroundJobs(app);

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_MenuBar;

        ImGui::Begin("DagorAssetsExtractorMain", nullptr, flags);
        DrawMenuBar(app, running);

        const std::shared_ptr<const std::vector<ResourceRecord>> recordsPtr = App::SnapshotRecords(app.scan);
        EnsureResourceUiCache(app, recordsPtr);
        const std::vector<ResourceRecord>& records = *recordsPtr;
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float splitterHeight = 6.0f;

        if (app.bottomPanelHeight < 0.0f)
        {
            app.bottomPanelHeight = std::min(190.0f, available.y * 0.32f);
        }

        const float minBottomHeight = 100.0f;
        const float maxBottomHeight = available.y - 200.0f;
        app.bottomPanelHeight = std::clamp(app.bottomPanelHeight, minBottomHeight, std::max(minBottomHeight, maxBottomHeight));

        const float topHeight = std::max(200.0f, available.y - app.bottomPanelHeight - splitterHeight - ImGui::GetStyle().ItemSpacing.y);
        const float leftWidth = 290.0f;
        // Initialize inspectorHeight if not set
        if (app.inspectorHeight < 0.0f)
        {
            app.inspectorHeight = topHeight * 0.54f;
        }

        // Clamp inspector height to stay within reasonable bounds
        const float minInspectorHeight = 180.0f;
        const float maxInspectorHeight = topHeight - 180.0f;
        app.inspectorHeight = std::clamp(app.inspectorHeight, minInspectorHeight, std::max(minInspectorHeight, maxInspectorHeight));


        ImGui::BeginChild("LeftPanel", ImVec2(leftWidth, topHeight), true);
        DrawLeftPanel(app, records);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("MainStackPanel", ImVec2(0.0f, topHeight), false);


        const float contentBrowserHeight = topHeight - app.inspectorHeight - splitterHeight - ImGui::GetStyle().ItemSpacing.y;

        ImGui::BeginChild("ContentBrowserPanel", ImVec2(0.0f, contentBrowserHeight), true);
        DrawContentBrowser(app, recordsPtr);
        ImGui::EndChild();

        // Horizontal splitter between ContentBrowserPanel and InspectorPanel
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Separator]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_SeparatorActive]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_SeparatorActive]);

        ImGui::Button("##inspector_splitter", ImVec2(-1.0f, splitterHeight));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (ImGui::IsItemActive())
        {
            app.inspectorHeight -= ImGui::GetIO().MouseDelta.y;
        }

        ImGui::PopStyleColor(3);

        ImGui::BeginChild("InspectorPanel", ImVec2(0.0f, 0.0f), true);
        DrawInspector(app, records);
        ImGui::EndChild();

        ImGui::EndChild();

        // Horizontal splitter between Top Area and Bottom Panel
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Separator]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_SeparatorActive]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_SeparatorActive]);

        ImGui::Button("##bottom_splitter", ImVec2(-1.0f, splitterHeight));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (ImGui::IsItemActive())
        {
            app.bottomPanelHeight -= ImGui::GetIO().MouseDelta.y;
        }

        ImGui::PopStyleColor(3);

        ImGui::BeginChild("BottomPanel", ImVec2(0.0f, 0.0f), true);
        DrawJobAndLogPanel(app);
        ImGui::EndChild();

        ImGui::End();

        DrawSettingsWindow(app);
        DrawAboutWindow(app);
        DrawFirstRunModal(app);
        if (!app.modelPreview.popupOpen)
        {
            DrawDynModelExportModal(app);
        }
        DrawProgressJobWindow(app);
        DrawRebuildProgressModal(app);
        DrawBlkRebuildProgressModal(app);
        DrawModelPreviewPopup(app);
        DrawTexturePreviewWindow(app);
        if (!app.texturePreview.windowOpen && !app.modelPreview.popupOpen)
        {
            DrawExportProgressModal(app);
        }
    }
}
