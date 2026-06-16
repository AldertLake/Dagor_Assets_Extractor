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

    struct BlkPreviewCache
    {
        std::wstring path;
        bool loaded = false;
        std::string error;
        size_t propertyCount = 0;
        size_t blockCount = 0;
        std::vector<std::string> lines;
    };

    size_t CountBlkPropertiesRecursive(const DagorFormats::BlkBlock& block)
    {
        size_t count = block.properties.size();
        for (const DagorFormats::BlkBlock& child : block.children)
        {
            count += CountBlkPropertiesRecursive(child);
        }
        return count;
    }

    size_t CountBlkBlocksRecursive(const DagorFormats::BlkBlock& block)
    {
        size_t count = block.children.size();
        for (const DagorFormats::BlkBlock& child : block.children)
        {
            count += CountBlkBlocksRecursive(child);
        }
        return count;
    }

    std::string MakeBlkPropertyLine(const DagorFormats::BlkProperty& property, int depth)
    {
        std::string line(static_cast<size_t>(depth) * 2, ' ');
        line += property.name;
        if (!property.type.empty())
        {
            line += ':';
            line += property.type;
        }
        line += " = ";
        line += property.quoted ? "\"" : "";
        line += property.value;
        line += property.quoted ? "\"" : "";

        std::replace(line.begin(), line.end(), '\n', ' ');
        std::replace(line.begin(), line.end(), '\r', ' ');
        if (line.size() > 180)
        {
            line.resize(177);
            line += "...";
        }
        return line;
    }

    void CollectBlkPreviewLines(const DagorFormats::BlkBlock& block, BlkPreviewCache& preview, int depth = 0)
    {
        if (preview.lines.size() >= 48)
        {
            return;
        }

        for (const DagorFormats::BlkProperty& property : block.properties)
        {
            if (preview.lines.size() >= 48)
            {
                return;
            }
            preview.lines.push_back(MakeBlkPropertyLine(property, depth));
        }

        for (const DagorFormats::BlkBlock& child : block.children)
        {
            if (preview.lines.size() >= 48)
            {
                return;
            }

            std::string line(static_cast<size_t>(depth) * 2, ' ');
            line += child.name;
            if (!child.type.empty())
            {
                line += ':';
                line += child.type;
            }
            line += " {";
            preview.lines.push_back(std::move(line));

            CollectBlkPreviewLines(child, preview, depth + 1);

            if (preview.lines.size() >= 48)
            {
                return;
            }

            preview.lines.push_back(std::string(static_cast<size_t>(depth) * 2, ' ') + "}");
        }
    }

    BlkPreviewCache LoadBlkPreview(const std::filesystem::path& path)
    {
        constexpr uintmax_t MaxPreviewBytes = 1024 * 1024;

        BlkPreviewCache preview;
        preview.path = path.wstring();
        preview.loaded = true;

        std::error_code ec;
        const uintmax_t fileSize = std::filesystem::file_size(path, ec);
        if (ec)
        {
            preview.error = "Could not read BLK size: " + ec.message();
            return preview;
        }

        if (fileSize > MaxPreviewBytes)
        {
            preview.error = "BLK file is larger than the 1 MB preview limit.";
            return preview;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            preview.error = "Could not open BLK file.";
            return preview;
        }

        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        try
        {
            DagorFormats::BlkReader reader(text);
            reader.Parse();

            const DagorFormats::BlkBlock& root = reader.GetRoot();
            preview.propertyCount = CountBlkPropertiesRecursive(root);
            preview.blockCount = CountBlkBlocksRecursive(root);
            CollectBlkPreviewLines(root, preview);
        }
        catch (const std::exception& exception)
        {
            preview.error = std::string("BLK parse error: ") + exception.what();
        }

        return preview;
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

            if (ImGui::MenuItem("Save Settings"))
            {
                SaveSettingsWithImGui(app);
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
        app.descriptorUiGrpMatches.clear();
        app.descriptorUiDxpMatches.clear();

        if (!links)
        {
            return;
        }

        for (size_t linkIndex = 0; linkIndex < links->size(); ++linkIndex)
        {
            const AssetIndex::DescriptorResourceLink& link = (*links)[linkIndex];
            if (selected.type == AssetType::Descriptor && link.descriptorPath == selected.path)
            {
                app.descriptorUiDescriptorMatches.push_back(linkIndex);
            }
            else if (selected.type == AssetType::Grp && link.matchedGrpPath && *link.matchedGrpPath == selected.path)
            {
                app.descriptorUiGrpMatches.push_back(linkIndex);
            }
            else if (selected.type == AssetType::Dxp)
            {
                for (size_t textureIndex = 0; textureIndex < link.textureLinks.size(); ++textureIndex)
                {
                    const AssetIndex::DescriptorTextureLink& textureLink = link.textureLinks[textureIndex];
                    if (textureLink.matchedDxpPath && *textureLink.matchedDxpPath == selected.path)
                    {
                        app.descriptorUiDxpMatches.push_back({ linkIndex, textureIndex });
                    }
                }
            }
        }
    }

    void DrawLeftPanel(AppState& app, const std::vector<ResourceRecord>& records)
    {
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

        uint32_t counts[5] = {};
        for (const ResourceRecord& record : records)
        {
            switch (record.type)
            {
            case AssetType::Vromfs:
                ++counts[0];
                break;
            case AssetType::Grp:
                ++counts[1];
                break;
            case AssetType::Dxp:
                ++counts[2];
                break;
            case AssetType::Descriptor:
                ++counts[3];
                break;
            case AssetType::Blk:
                ++counts[4];
                break;
            default:
                break;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Discovered");
        ImGui::BulletText("VROMFS: %u", counts[0]);
        ImGui::BulletText("GRP: %u", counts[1]);
        ImGui::BulletText("DXP: %u", counts[2]);
        ImGui::BulletText("Descriptors: %u", counts[3]);
        ImGui::BulletText("BLK: %u", counts[4]);
        ImGui::Spacing();
        ImGui::TextWrapped("Cache: %s", App::GetCacheStatus(app.scan).c_str());
        ImGui::TextWrapped("BLK Links: %s", App::GetBlkResourceLinkStatus(app).c_str());
        ImGui::TextWrapped("Desc Links: %s", App::GetDescriptorResourceLinkStatus(app).c_str());

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
            for (size_t i = 0; i < records.size(); ++i)
            {
                if (RecordPassesTypeFilter(app, records[i].type) && RecordPassesSearch(records[i], search))
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

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(app.filteredIndices.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    ImGui::PushID(row);
                    const ResourceRecord& record = records[app.filteredIndices[row]];
                    const std::wstring nativePath = record.path.wstring();
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
                        std::string pathText = Platform::PathToUtf8(record.path.parent_path());
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
                    const std::string name = Platform::PathToUtf8(record.path.filename());
                    ImGui::TextUnformatted(name.c_str());

                    ImGui::TableSetColumnIndex(2);
                    DrawWrappedText(
                        Platform::PathToUtf8(record.path.parent_path()));

                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(FormatBytes(record.sizeBytes).c_str());
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

        for (const ResourceRecord& record : records)
        {
            if (record.path.wstring() == app.selectedPath)
            {
                return record;
            }
        }

        return std::nullopt;
    }

    void DrawBlkPreview(const ResourceRecord& selected)
    {
        static BlkPreviewCache preview;

        const std::wstring selectedPath = selected.path.wstring();
        if (preview.path != selectedPath)
        {
            preview = {};
            preview.path = selectedPath;
        }

        ImGui::TextUnformatted("BLK Preview");
        ImGui::BeginDisabled(preview.loaded);
        if (ImGui::Button("Load BLK Preview", ImVec2(-1.0f, 0.0f)))
        {
            preview = LoadBlkPreview(selected.path);
        }
        ImGui::EndDisabled();

        if (!preview.loaded)
        {
            ImGui::TextWrapped("Click to parse a small text BLK preview. Binary/packed BLK variants are not decoded yet.");
            return;
        }

        if (!preview.error.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.49f, 0.42f, 1.0f), "Parse Error");
            ImGui::TextWrapped("%s", preview.error.c_str());
            return;
        }

        ImGui::Text("Properties: %zu", preview.propertyCount);
        ImGui::SameLine();
        ImGui::Text("Blocks: %zu", preview.blockCount);

        if (ImGui::BeginTable("BlkPreviewLines", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 180.0f)))
        {
            ImGui::TableSetupColumn("Top-level and nested entries", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(preview.lines.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(preview.lines[static_cast<size_t>(row)].c_str());
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

        std::vector<const AssetIndex::BlkResourceLink*> rows;
        for (const auto& link : *links)
        {
            if (link.blkPath == selected.path)
            {
                rows.push_back(&link);
            }
        }

        if (rows.empty())
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

            for (const auto* row : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                DrawWrappedText(row->resourceName);

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(AssetIndex::AssetTypeName(row->expectedType));

                ImGui::TableSetColumnIndex(2);
                if (row->matchedPath)
                {
                    DrawWrappedText(Platform::PathToUtf8(*row->matchedPath));
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.49f, 0.42f, 1.0f), "Unresolved");
                }

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u", row->line);
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

            const size_t resolvedGrpCount = static_cast<size_t>(std::count_if(
                app.descriptorUiDescriptorMatches.begin(),
                app.descriptorUiDescriptorMatches.end(),
                [&](size_t linkIndex) { return (*links)[linkIndex].matchedGrpPath.has_value(); }));

            ImGui::Text("Resources in this descriptor: %zu", app.descriptorUiDescriptorMatches.size());
            ImGui::Text("Resolved GRP matches: %zu", resolvedGrpCount);

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

                for (size_t rowIndex = 0; rowIndex < app.descriptorUiDescriptorMatches.size(); ++rowIndex)
                {
                    const AssetIndex::DescriptorResourceLink& link = (*links)[app.descriptorUiDescriptorMatches[rowIndex]];
                    const size_t resolvedTextureCount = static_cast<size_t>(std::count_if(
                        link.textureLinks.begin(),
                        link.textureLinks.end(),
                        [](const auto& texLink) { return texLink.matchedDxpPath.has_value(); }));

                    ImGui::PushID(static_cast<int>(rowIndex));
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    DrawWrappedText(link.resourceName);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("0x%X", link.resId);

                    ImGui::TableSetColumnIndex(2);
                    if (link.matchedGrpPath)
                    {
                        DrawWrappedText(
                            link.matchedGrpEntryName
                                ? *link.matchedGrpEntryName
                                : "Root");
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
                        DrawWrappedText(link.matchedGrpClassName);
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

                    for (const auto& texLink : primaryLink.textureLinks)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%u", texLink.textureIndex);

                        ImGui::TableSetColumnIndex(1);
                        DrawWrappedText(texLink.textureName);

                        ImGui::TableSetColumnIndex(2);
                        if (texLink.matchedDxpPath)
                        {
                            DrawWrappedText(
                                Platform::PathToUtf8(
                                    texLink.matchedDxpPath->filename()));
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
                            DrawWrappedText(*texLink.matchedDxpEntryName);
                        }
                        else
                        {
                            ImGui::TextDisabled("-");
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

                    for (const size_t linkIndex : app.descriptorUiGrpMatches)
                    {
                        const AssetIndex::DescriptorResourceLink& match = (*links)[linkIndex];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        DrawWrappedText(match.resourceName);

                        ImGui::TableSetColumnIndex(1);
                        DrawWrappedText(
                            match.matchedGrpEntryName
                                ? *match.matchedGrpEntryName
                                : "Root");

                        ImGui::TableSetColumnIndex(2);
                        DrawWrappedText(
                            Platform::PathToUtf8(
                                match.descriptorPath.filename()));
                        if (ImGui::IsItemHovered())
                        {
                            DrawWrappedTooltip(
                                Platform::PathToUtf8(match.descriptorPath));
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

                    for (const auto& matchIndex : app.descriptorUiDxpMatches)
                    {
                        const AssetIndex::DescriptorResourceLink& match = (*links)[matchIndex.first];
                        const AssetIndex::DescriptorTextureLink& textureLink = match.textureLinks[matchIndex.second];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        DrawWrappedText(
                            textureLink.matchedDxpEntryName
                                ? *textureLink.matchedDxpEntryName
                                : "Root");

                        ImGui::TableSetColumnIndex(1);
                        DrawWrappedText(match.resourceName);

                        ImGui::TableSetColumnIndex(2);
                        DrawWrappedText(
                            Platform::PathToUtf8(
                                match.descriptorPath.filename()));
                        if (ImGui::IsItemHovered())
                        {
                            DrawWrappedTooltip(
                                Platform::PathToUtf8(match.descriptorPath));
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
                const bool isParsing = app.packContentsParsing.load();
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
            DrawBlkPreview(*selected);
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
        if (ImGui::Button("Clear"))
        {
            logger.ClearUI();
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy All"))
        {
            auto logs = logger.GetSnapshot();
            std::string allLogs;
            for (const auto& entry : logs)
            {
                allLogs += entry.formatted + "\n";
            }
            ImGui::SetClipboardText(allLogs.c_str());
        }

        ImGui::Separator();

        const bool childVisible = ImGui::BeginChild(id, ImVec2(0.0f, height), true, ImGuiWindowFlags_HorizontalScrollbar);
        if (childVisible)
        {
            auto logs = logger.GetSnapshot();
            for (size_t i = 0; i < logs.size(); ++i)
            {
                const auto& entry = logs[i];
                ImGui::PushID(static_cast<int>(i));
                
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

        bool open = snapshot.windowOpen;
        ImGui::SetNextWindowSize(ImVec2(620.0f, 360.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(snapshot.title.empty() ? "Operation Progress" : snapshot.title.c_str(), &open))
        {
            {
                std::lock_guard<std::mutex> lock(app.progressJob.mutex);
                app.progressJob.windowOpen = open || snapshot.running;
            }
            ImGui::End();
            return;
        }

        ImGui::TextWrapped("%s", snapshot.status.empty() ? "Working..." : snapshot.status.c_str());
        if (!snapshot.currentItem.empty())
        {
            DrawWrappedDisabledText(snapshot.currentItem);
        }

        const float fraction = snapshot.total > 0
            ? std::clamp(static_cast<float>(snapshot.current) / static_cast<float>(snapshot.total), 0.0f, 1.0f)
            : 0.0f;
        std::string overlay;
        if (snapshot.total > 0)
        {
            overlay = std::to_string(snapshot.current) + " / " + std::to_string(snapshot.total);
        }
        ImGui::ProgressBar(snapshot.total > 0 ? fraction : 0.0f, ImVec2(-1.0f, 0.0f), overlay.empty() ? nullptr : overlay.c_str());

        ImGui::BeginDisabled(!snapshot.running || snapshot.cancelRequested);
        if (ImGui::Button(snapshot.cancelRequested ? "Cancel Requested" : "Cancel", ImVec2(130.0f, 0.0f)))
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

        ImGui::SameLine();
        if (!snapshot.running && ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
        {
            open = false;
        }

        ImGui::Separator();
        DrawLogPanel(app.logger, "ProgressJobLogs");

        {
            std::lock_guard<std::mutex> lock(app.progressJob.mutex);
            app.progressJob.windowOpen = open || snapshot.running;
        }
        ImGui::End();
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

        std::string title = "Settings###SettingsWindow";
        if (!ImGui::IsPopupOpen(title.c_str()))
        {
            ImGui::OpenPopup(title.c_str());
        }

        ImGui::SetNextWindowSize(ImVec2(800.0f, 500.0f), ImGuiCond_FirstUseEver);
        bool open = app.settingsWindowOpen;
        if (ImGui::BeginPopupModal(title.c_str(), &open, ImGuiWindowFlags_NoCollapse))
        {
            const ImGuiTableFlags layoutFlags =
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("SettingsLayout", 2, layoutFlags))
            {
                ImGui::TableSetupColumn("Categories", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::BeginChild("SettingsCategories", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - 10.0f), false);
                
                static int selectedCategory = 0;
                if (DrawInlineIconSelectable(UiIconId::GameInstall, "Game Installation", selectedCategory == 0))
                {
                    selectedCategory = 0;
                }
                ImGui::Spacing();
                if (DrawInlineIconSelectable(UiIconId::Texture, "Asset Export", selectedCategory == 1))
                {
                    selectedCategory = 1;
                }
                ImGui::Spacing();
                if (DrawInlineIconSelectable(UiIconId::Rebuild, "Performance", selectedCategory == 2))
                {
                    selectedCategory = 2;
                }
                ImGui::Spacing();
                if (DrawInlineIconSelectable(UiIconId::Log, "Storage & Paths", selectedCategory == 3))
                {
                    selectedCategory = 3;
                }
                
                ImGui::EndChild();

                ImGui::TableSetColumnIndex(1);
                ImGui::BeginChild("SettingsContent", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - 10.0f), false);
                
                if (selectedCategory == 0)
                {
                    DrawPanelTitle(UiIconId::GameInstall, "Game Installation");
                    ImGui::Separator();
                    ImGui::Spacing();
                    
                    ImGui::TextUnformatted("War Thunder Folder:");
                    ImGui::SetNextItemWidth(-120.0f);
                    ImGui::InputText("##GameRoot", app.gameRootBuffer.data(), app.gameRootBuffer.size());
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##GameRoot", ImVec2(100.0f, 0.0f)))
                    {
                        if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose War Thunder Folder", app.settings.gameRoot))
                        {
                            App::SetGameRoot(app, *selected);
                        }
                    }
                    
                    ImGui::Spacing();
                    if (ImGui::Button("Validate Game Folder", ImVec2(180.0f, 0.0f)))
                    {
                        App::ApplyGameRootBuffer(app);
                    }
                    
                    ImGui::Spacing();
                    DrawValidationSummary(app.validation);
                    
                    ImGui::Separator();
                    ImGui::Spacing();
                    if (ImGui::Checkbox("Enable index cache", &app.settings.indexCacheEnabled))
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
                            "Saves a scanned directory file list cache in the storage directory "
                            "to speed up future application launches.");
                    }
                }
                else if (selectedCategory == 1)
                {
                    DrawPanelTitle(UiIconId::Texture, "Asset Export");
                    ImGui::Separator();
                    ImGui::Spacing();
                    
                    ImGui::TextUnformatted("Export Output Folder:");
                    ImGui::SetNextItemWidth(-120.0f);
                    ImGui::InputText("##ExportRoot", app.exportRootBuffer.data(), app.exportRootBuffer.size());
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##ExportRoot", ImVec2(100.0f, 0.0f)))
                    {
                        if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose Export Folder", app.settings.exportRoot))
                        {
                            app.settings.exportRoot = *selected;
                            App::SyncSettingsBuffers(app);
                        }
                    }
                    
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    
                    // Force texture format to PNG, since GLB is the only supported format and requires PNG textures.
                    app.settings.textureExportFormat = "PNG";
                    static const char* textureFormats[] = { "DDS", "PNG", "TGA" };
                    int textureFormatIndex = 1; // "PNG"

                    ImGui::BeginDisabled(true);
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::Combo("Texture Format", &textureFormatIndex, textureFormats, IM_ARRAYSIZE(textureFormats)))
                    {
                        app.settings.textureExportFormat = textureFormats[textureFormatIndex];
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered())
                    {
                        DrawWrappedTooltip("GLB export format requires textures to be in PNG/JPEG format.");
                    }
                    
                    int maxLods = app.settings.staticModelMaxLods;
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::InputInt("Max RendInst LODs", &maxLods))
                    {
                        maxLods = std::clamp(maxLods, 1, 16);
                        app.settings.staticModelMaxLods = maxLods;
                    }
                    if (ImGui::IsItemHovered())
                    {
                        DrawWrappedTooltip(
                            "Sets the maximum number of Levels of Detail (LODs) to export "
                            "for static models (RendInst).");
                    }
                    
                    ImGui::Spacing();
                    ImGui::Checkbox("Export textures with models", &app.settings.exportTexturesWithModels);
                    
                    ImGui::Checkbox("Embed textures inside GLB", &app.settings.embedTexturesInGlb);
                    if (ImGui::IsItemHovered())
                    {
                        DrawWrappedTooltip(
                            "Stores PNG/JPEG material textures as GLB bufferViews instead "
                            "of external image URIs.");
                    }
                    
                    ImGui::Checkbox(
                        "Convert War Thunder packed _n to Normal + ORM",
                        &app.settings.convertWarThunderPackedMaps);
                    if (ImGui::IsItemHovered())
                    {
                        DrawWrappedTooltip(
                            "Enabled: reconstructs a standard tangent-space normal texture and "
                            "generates glTF ORM from War Thunder's packed _n channels. Disabled: "
                            "preserves the native packed image in an optional custom GLB extension; "
                            "standard viewers will not shade from those packed channels automatically.");
                    }
                    
                    ImGui::Checkbox("Export JSON manifest metadata alongside assets", &app.settings.exportJsonManifest);
                    ImGui::Checkbox("Fallback to DDS when PNG/TGA conversion is unavailable", &app.settings.preserveOriginalDDS);
                }
                else if (selectedCategory == 2)
                {
                    DrawPanelTitle(UiIconId::Rebuild, "Performance");
                    ImGui::Separator();
                    ImGui::Spacing();
                    
                    int maxLoadedPackContents = static_cast<int>(app.settings.maxLoadedPackContents);
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::InputInt("Max Loaded Containers", &maxLoadedPackContents))
                    {
                        maxLoadedPackContents = std::clamp(maxLoadedPackContents, 1, 256);
                        app.settings.maxLoadedPackContents = static_cast<size_t>(maxLoadedPackContents);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        DrawWrappedTooltip(
                            "When loaded pack contents exceed this number, the cache unloads "
                            "old parsed containers and keeps the current inspector target.");
                    }
                    
                    ImGui::Spacing();
                    if (ImGui::Checkbox("Enable index cache##Performance", &app.settings.indexCacheEnabled))
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
                    
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();
                    
                    DrawWrappedDisabledText(
                        "Oodle-compressed DDSx textures use the bundled oo2core_6_win64.dll "
                        "shipped with the app build.");
                    ImGui::Spacing();
                    DrawWrappedDisabledText(
                        "PNG/TGA exports the top mip directly for BC1-BC5 plus common "
                        "uncompressed/float DDSx textures, then retries unsupported DXGI "
                        "formats through DirectXTex.");
                }
                else if (selectedCategory == 3)
                {
                    DrawPanelTitle(UiIconId::Log, "Storage & Paths");
                    ImGui::Separator();
                    ImGui::Spacing();
                    
                    if (ImGui::BeginTable("StoragePathsTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("File Type", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                        
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted("Settings file");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", Platform::PathToUtf8(app.settingsPath).c_str());
                        ImGui::TableSetColumnIndex(2);
                        if (ImGui::SmallButton("Show in Explorer##Settings"))
                        {
                            Platform::ShowPathInExplorer(app.settingsPath);
                        }
                        
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted("Index cache");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", Platform::PathToUtf8(app.indexCachePath).c_str());
                        ImGui::TableSetColumnIndex(2);
                        if (ImGui::SmallButton("Show in Explorer##Index"))
                        {
                            Platform::ShowPathInExplorer(app.indexCachePath);
                        }
                        
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted("Descriptor-link cache");
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", Platform::PathToUtf8(app.descriptorLinksCachePath).c_str());
                        ImGui::TableSetColumnIndex(2);
                        if (ImGui::SmallButton("Show in Explorer##Desc"))
                        {
                            Platform::ShowPathInExplorer(app.descriptorLinksCachePath);
                        }
                        
                        ImGui::EndTable();
                    }
                }
                
                ImGui::EndChild();
                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("Save Settings", ImVec2(120.0f, 0.0f)))
            {
                app.settings.exportRoot = Platform::Utf8ToPath(app.exportRootBuffer.data());
                App::SetGameRoot(app, Platform::Utf8ToPath(app.gameRootBuffer.data()));
                SaveSettingsWithImGui(app);
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                open = false;
                ImGui::CloseCurrentPopup();
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

        if (!ImGui::Begin("About Dagor Assets Extractor", &app.aboutWindowOpen))
        {
            ImGui::End();
            return;
        }

        ImGui::TextWrapped("Dagor Assets Extractor is a read-only desktop browser and exporter for War Thunder Dagor resource packs.");
        ImGui::Separator();
        DrawWrappedBulletText(
            "Scans the local game install and builds a searchable content browser.");
        DrawWrappedBulletText(
            "Inspects GRP, DXP, VROMFS, descriptor, and BLK data without modifying "
            "game files.");
        DrawWrappedBulletText(
            "Exports readable textures and models for modding workflows.");
        ImGui::Spacing();
        if (DrawIconTextButton("##about_github", UiIconId::Github, "Open Source on GitHub"))
        {
            Platform::OpenUrl(L"https://github.com/AldertLake/Dagor_Assets_Extractor");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Credit: AldertLake");
        ImGui::End();
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

        ImGui::OpenPopup("First Run Setup");
        if (ImGui::BeginPopupModal("First Run Setup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("Choose the local War Thunder installation folder to start building the read-only resource index.");
            ImGui::Spacing();
            ImGui::InputText("Game Root", app.gameRootBuffer.data(), app.gameRootBuffer.size());
            ImGui::SameLine();
            if (ImGui::Button("Browse"))
            {
                if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose War Thunder Folder", app.settings.gameRoot))
                {
                    App::SetGameRoot(app, *selected);
                }
            }

            DrawValidationSummary(app.validation);
            ImGui::Separator();

            ImGui::BeginDisabled(!app.validation.IsValid());
            if (ImGui::Button("Use Folder", ImVec2(120.0f, 0.0f)))
            {
                App::ApplyGameRootBuffer(app);
                if (app.validation.IsValid())
                {
                    SaveSettingsWithImGui(app);
                    app.firstRunPromptOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    App::AddLog(app, LogLevel::Warning, "The selected folder did not pass validation.");
                }
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Validate", ImVec2(100.0f, 0.0f)))
            {
                App::ApplyGameRootBuffer(app);
            }

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
        const App::TexturePreviewSnapshot& preview)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.06f, 0.072f, 1.0f));
        ImGui::BeginChild(
            "TexturePreviewCanvas",
            ImVec2(0.0f, 0.0f),
            true,
            ImGuiWindowFlags_HorizontalScrollbar);

        if (previewTexture == nullptr || previewTexture->view == nullptr)
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
            ImGui::EndChild();
            ImGui::PopStyleColor();
            return;
        }

        const bool canvasHovered =
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        if (canvasHovered &&
            ImGui::GetIO().MouseWheel != 0.0f)
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
        if (app.texturePreview.checkerboard)
        {
            constexpr float tileSize = 16.0f;
            const ImVec2 windowMin = ImGui::GetWindowPos();
            const ImVec2 windowMax(
                windowMin.x + ImGui::GetWindowSize().x,
                windowMin.y + ImGui::GetWindowSize().y);
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
        const Export::TextureMapAnalysis mapAnalysis =
            Export::AnalyzeTextureMaps(preview.entryName);

        ImGui::SetNextWindowSize(ImVec2(1080.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 480.0f), ImVec2(FLT_MAX, FLT_MAX));
        std::string title = "Texture Viewer - " + preview.entryName + "###TexturePreviewWindow";
        bool open = app.texturePreview.windowOpen;

        if (!ImGui::IsPopupOpen(title.c_str()))
        {
            ImGui::OpenPopup(title.c_str());
        }

        ImGuiIO& io = ImGui::GetIO();
        const bool previousMoveFromTitleBarOnly =
            io.ConfigWindowsMoveFromTitleBarOnly;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        const bool viewerVisible = ImGui::BeginPopupModal(title.c_str(), &open);
        io.ConfigWindowsMoveFromTitleBarOnly = previousMoveFromTitleBarOnly;

        if (viewerVisible)
        {
            DrawPanelTitle(UiIconId::Texture, "Texture Viewer");
            ImGui::SameLine();
            DrawWrappedDisabledText(
                Platform::PathToUtf8(preview.packPath.filename()));
            ImGui::Separator();

            const ImGuiTableFlags layoutFlags =
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("TextureViewerLayout", 2, layoutFlags))
            {
                ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.72f);
                ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed, 320.0f);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::BeginChild("TextureViewerMain", ImVec2(0.0f, 0.0f), false);
                if (ImGui::Button("Fit to Window"))
                {
                    app.texturePreview.fitToWindow = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("100%"))
                {
                    app.texturePreview.fitToWindow = false;
                    app.texturePreview.zoom = 1.0f;
                }
                ImGui::SameLine();
                if (ImGui::Button("-"))
                {
                    app.texturePreview.fitToWindow = false;
                    app.texturePreview.zoom =
                        std::max(0.05f, app.texturePreview.zoom / 1.25f);
                }
                ImGui::SameLine();
                if (ImGui::Button("+"))
                {
                    app.texturePreview.fitToWindow = false;
                    app.texturePreview.zoom =
                        std::min(16.0f, app.texturePreview.zoom * 1.25f);
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130.0f);
                ImGui::BeginDisabled(app.texturePreview.fitToWindow);
                ImGui::SliderFloat(
                    "##TextureZoom",
                    &app.texturePreview.zoom,
                    0.05f,
                    16.0f,
                    "%.2fx",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::Checkbox("Alpha Grid", &app.texturePreview.checkerboard);

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
                else
                {
                    DrawWrappedDisabledText(
                        "Mouse wheel zooms. Left-drag pans images larger than the viewport.");
                }

                DrawTexturePreviewCanvas(app, GetTexturePreview(), preview);
                ImGui::EndChild();

                ImGui::TableSetColumnIndex(1);
                ImGui::BeginChild("TextureViewerDetails", ImVec2(0.0f, 0.0f), false);
                ImGui::TextUnformatted("Texture Details");
                ImGui::Separator();

                if (ImGui::CollapsingHeader(
                        "Image",
                        ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (ImGui::BeginTable(
                            "TextureImageDetails",
                            2,
                            ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 92.0f);
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
                        ImGui::EndTable();
                    }
                }

                if (ImGui::CollapsingHeader(
                        "Detected Maps",
                        ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (mapAnalysis.layout ==
                        Export::TextureContentLayout::WarThunderPackedNormalPbr)
                    {
                        const ImVec4 modeColor = app.settings.convertWarThunderPackedMaps
                            ? ImVec4(0.38f, 0.72f, 1.0f, 1.0f)
                            : ImVec4(0.95f, 0.76f, 0.32f, 1.0f);
                        DrawWrappedColoredText(
                            modeColor,
                            app.settings.convertWarThunderPackedMaps
                                ? "GLB mode: generate standard Normal + ORM textures."
                                : "GLB mode: preserve native packed _n channels in the optional DAGOR_material_war_thunder_packed_n extension.");
                        DrawWrappedDisabledText(
                            app.settings.convertWarThunderPackedMaps
                                ? "Instant map previews and individual-map export use the same channel conversion as model export."
                                : "Instant map previews and individual-map export remain available. Generic glTF viewers ignore the native packed extension unless they implement its channel contract.");
                        ImGui::Spacing();
                    }

                    const ImVec4 detectionColor =
                        mapAnalysis.confidence == Export::TextureDetectionConfidence::StrongNameBased
                            ? ImVec4(0.48f, 0.86f, 0.58f, 1.0f)
                            : mapAnalysis.confidence == Export::TextureDetectionConfidence::NameBased
                                ? ImVec4(0.95f, 0.76f, 0.32f, 1.0f)
                                : ImVec4(0.72f, 0.74f, 0.80f, 1.0f);
                    DrawWrappedColoredText(detectionColor, mapAnalysis.summary);
                    ImGui::TextWrapped("%s", mapAnalysis.caution.c_str());

                    if (preview.loading)
                    {
                        ImGui::Spacing();
                        DrawWrappedDisabledText(
                            "Map previews will be generated after the source texture is decoded.");
                    }
                    else if (!preview.error.empty())
                    {
                        ImGui::Spacing();
                        ImGui::PushStyleColor(
                            ImGuiCol_ChildBg,
                            ImVec4(0.20f, 0.075f, 0.065f, 0.72f));
                        ImGui::BeginChild(
                            "TextureMapDecodeError",
                            ImVec2(0.0f, 0.0f),
                            ImGuiChildFlags_Borders |
                                ImGuiChildFlags_AutoResizeY);
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.49f, 0.42f, 1.0f),
                            "Map previews unavailable");
                        ImGui::TextWrapped(
                            "The source texture is invalid or could not be decoded. "
                            "No maps can be previewed or exported.");
                        DrawWrappedDisabledText(preview.error);
                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }
                    else if (mapAnalysis.maps.empty())
                    {
                        ImGui::Spacing();
                        DrawWrappedDisabledText(
                            "No individual-map export is offered because the channel layout "
                            "could not be identified safely.");
                    }
                    else
                    {
                        ImGui::Spacing();
                        if (ImGui::SmallButton("Select All"))
                        {
                            for (const Export::DetectedTextureMap& map :
                                 mapAnalysis.maps)
                            {
                                const UiTexture* mapTexture =
                                    GetTextureMapPreview(
                                        static_cast<uint32_t>(map.kind));
                                if (mapTexture != nullptr &&
                                    mapTexture->view != nullptr)
                                {
                                    app.texturePreview.selectedMapMask |=
                                        1u << static_cast<uint32_t>(map.kind);
                                }
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear Selection"))
                        {
                            app.texturePreview.selectedMapMask = 0;
                        }
                        ImGui::Spacing();
                        constexpr float tileSpacing = 7.0f;
                        const float availableWidth = ImGui::GetContentRegionAvail().x;
                        const int columnCount = std::clamp(
                            static_cast<int>((availableWidth + tileSpacing) / 92.0f),
                            2,
                            3);
                        const float tileSize = std::max(
                            72.0f,
                            (availableWidth -
                                tileSpacing * static_cast<float>(columnCount - 1)) /
                                static_cast<float>(columnCount));

                        for (size_t mapIndex = 0;
                             mapIndex < mapAnalysis.maps.size();
                             ++mapIndex)
                        {
                            const Export::DetectedTextureMap& map =
                                mapAnalysis.maps[mapIndex];
                            if (mapIndex % static_cast<size_t>(columnCount) != 0)
                            {
                                ImGui::SameLine(0.0f, tileSpacing);
                            }

                            ImGui::PushID(static_cast<int>(map.kind));
                            const UiTexture* mapTexture = GetTextureMapPreview(
                                static_cast<uint32_t>(map.kind));
                            const bool mapPreviewAvailable =
                                mapTexture != nullptr &&
                                mapTexture->view != nullptr;
                            const ImVec2 tileMin = ImGui::GetCursorScreenPos();
                            ImGui::InvisibleButton(
                                "##TextureMapTile",
                                ImVec2(tileSize, tileSize),
                                ImGuiButtonFlags_MouseButtonLeft |
                                    ImGuiButtonFlags_MouseButtonRight);
                            const bool hovered = ImGui::IsItemHovered();
                            const uint32_t selectionBit =
                                1u << static_cast<uint32_t>(map.kind);
                            if (mapPreviewAvailable &&
                                ImGui::IsItemClicked(ImGuiMouseButton_Left))
                            {
                                app.texturePreview.selectedMapMask ^=
                                    selectionBit;
                            }
                            else if (
                                mapPreviewAvailable &&
                                ImGui::IsItemClicked(ImGuiMouseButton_Right) &&
                                (app.texturePreview.selectedMapMask &
                                    selectionBit) == 0)
                            {
                                app.texturePreview.selectedMapMask |=
                                    selectionBit;
                            }

                            const bool selected =
                                (app.texturePreview.selectedMapMask &
                                    selectionBit) != 0;
                            const ImVec2 tileMax(
                                tileMin.x + tileSize,
                                tileMin.y + tileSize);
                            ImDrawList* drawList = ImGui::GetWindowDrawList();
                            drawList->AddRectFilled(
                                tileMin,
                                tileMax,
                                hovered
                                    ? IM_COL32(50, 56, 68, 255)
                                    : IM_COL32(34, 39, 48, 255),
                                4.0f);

                            if (mapPreviewAvailable)
                            {
                                const float imagePadding = 5.0f;
                                const float contentSize =
                                    std::max(1.0f, tileSize - imagePadding * 2.0f);
                                const float imageScale = std::min(
                                    contentSize /
                                        static_cast<float>(mapTexture->width),
                                    contentSize /
                                        static_cast<float>(mapTexture->height));
                                const ImVec2 imageSize(
                                    std::max(
                                        1.0f,
                                        static_cast<float>(mapTexture->width) *
                                            imageScale),
                                    std::max(
                                        1.0f,
                                        static_cast<float>(mapTexture->height) *
                                            imageScale));
                                const ImVec2 imageMin(
                                    tileMin.x + (tileSize - imageSize.x) * 0.5f,
                                    tileMin.y + (tileSize - imageSize.y) * 0.5f);
                                drawList->AddImage(
                                    reinterpret_cast<ImTextureID>(
                                        mapTexture->view),
                                    imageMin,
                                    ImVec2(
                                        imageMin.x + imageSize.x,
                                        imageMin.y + imageSize.y));
                            }
                            else
                            {
                                const ImVec2 textSize =
                                    ImGui::CalcTextSize("Unavailable");
                                drawList->AddText(
                                    ImVec2(
                                        tileMin.x +
                                            (tileSize - textSize.x) * 0.5f,
                                        tileMin.y +
                                            (tileSize - textSize.y) * 0.5f),
                                    IM_COL32(148, 154, 166, 255),
                                    "Unavailable");
                            }

                            drawList->AddRect(
                                tileMin,
                                tileMax,
                                selected
                                    ? IM_COL32(55, 142, 255, 255)
                                    : hovered
                                        ? IM_COL32(118, 135, 162, 255)
                                        : IM_COL32(75, 83, 98, 255),
                                4.0f,
                                0,
                                selected ? 3.0f : 1.0f);

                            if (hovered)
                            {
                                ImGui::BeginTooltip();
                                ImGui::TextUnformatted(map.label.c_str());
                                DrawWrappedDisabledText(map.channels);
                                if (!mapPreviewAvailable)
                                {
                                    DrawWrappedColoredText(
                                        ImVec4(1.0f, 0.62f, 0.42f, 1.0f),
                                        "Thumbnail generation failed.");
                                }
                                ImGui::EndTooltip();
                            }

                            if (ImGui::BeginPopupContextItem("TextureMapDetails"))
                            {
                                ImGui::TextUnformatted(map.label.c_str());
                                ImGui::Separator();
                                ImGui::Text("Type: %s", map.label.c_str());
                                ImGui::Text(
                                    "Channels: %s",
                                    map.channels.c_str());
                                ImGui::Text(
                                    "Source size: %u x %u",
                                    preview.sourceWidth,
                                    preview.sourceHeight);
                                if (mapTexture != nullptr)
                                {
                                    ImGui::Text(
                                        "Thumbnail: %u x %u",
                                        mapTexture->width,
                                        mapTexture->height);
                                }
                                else
                                {
                                    ImGui::TextColored(
                                        ImVec4(1.0f, 0.62f, 0.42f, 1.0f),
                                        "Preview: Unavailable");
                                }
                                ImGui::Text(
                                    "Generated: %s",
                                    map.derived ? "Yes" : "No");
                                const std::string outputSuffix =
                                    std::string(
                                        Export::TextureMapKindFileSuffix(
                                            map.kind)) +
                                    ".png";
                                ImGui::Text(
                                    "Output suffix: %s",
                                    outputSuffix.c_str());
                                ImGui::Text(
                                    "Source format: %s",
                                    preview.format.empty()
                                        ? "Unknown"
                                        : preview.format.c_str());
                                ImGui::Text(
                                    "Mip levels: %u",
                                    static_cast<unsigned>(preview.mipCount));
                                ImGui::Text(
                                    "Data space: %s",
                                    map.kind == Export::TextureMapKind::BaseColor
                                        ? "sRGB color"
                                        : "Linear material data");
                                ImGui::EndPopup();
                            }
                            ImGui::PopID();
                        }

                        ImGui::Spacing();
                        const bool exportBusy = app.exportProgress.running.load();
                        std::vector<Export::TextureMapKind> selectedMaps;
                        selectedMaps.reserve(mapAnalysis.maps.size());
                        for (const Export::DetectedTextureMap& map :
                             mapAnalysis.maps)
                        {
                            const uint32_t selectionBit =
                                1u << static_cast<uint32_t>(map.kind);
                            const UiTexture* mapTexture =
                                GetTextureMapPreview(
                                    static_cast<uint32_t>(map.kind));
                            if ((app.texturePreview.selectedMapMask &
                                    selectionBit) != 0 &&
                                mapTexture != nullptr &&
                                mapTexture->view != nullptr)
                            {
                                selectedMaps.push_back(map.kind);
                            }
                        }
                        ImGui::TextDisabled(
                            "Selected: %zu map%s",
                            selectedMaps.size(),
                            selectedMaps.size() == 1 ? "" : "s");
                        ImGui::BeginDisabled(
                            !currentEntry.has_value() ||
                            selectedMaps.empty() ||
                            exportBusy);
                        const std::string exportLabel =
                            selectedMaps.size() == 1
                                ? "Export Selected Map"
                                : "Export Selected Maps (" +
                                    std::to_string(selectedMaps.size()) + ")";
                        if (ImGui::Button(
                                exportLabel.c_str(),
                                ImVec2(-1.0f, 0.0f)) &&
                            currentEntry &&
                            !selectedMaps.empty())
                        {
                            App::RequestTextureMapsExport(
                                app,
                                preview.packPath,
                                *currentEntry,
                                std::move(selectedMaps));
                        }
                        ImGui::EndDisabled();
                        if (exportBusy)
                        {
                            ImGui::TextDisabled("Another export is currently running.");
                        }
                    }
                }

                if (ImGui::CollapsingHeader(
                        "Storage",
                        ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (ImGui::BeginTable(
                            "TextureStorageDetails",
                            2,
                            ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 92.0f);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                        DrawTextureDetailRow(
                            "DXP pack",
                            Platform::PathToUtf8(preview.packPath.filename()));
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

                if (ImGui::CollapsingHeader(
                        "Decode Status",
                        ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (preview.loading)
                    {
                        ImGui::TextColored(
                            ImVec4(0.72f, 0.84f, 1.0f, 1.0f),
                            "Decoding in background");
                    }
                    else if (!preview.error.empty())
                    {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.49f, 0.42f, 1.0f),
                            "Decode failed");
                        ImGui::TextWrapped("%s", preview.error.c_str());
                    }
                    else
                    {
                        ImGui::TextColored(
                            ImVec4(0.48f, 0.86f, 0.58f, 1.0f),
                            "Ready");
                    }

                    if (!preview.warnings.empty())
                    {
                        ImGui::Spacing();
                        ImGui::TextDisabled(
                            "%zu decode note%s",
                            preview.warnings.size(),
                            preview.warnings.size() == 1 ? "" : "s");
                        for (const std::string& warning : preview.warnings)
                        {
                            DrawWrappedBulletText(warning);
                        }
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::BeginDisabled(!currentEntry.has_value());
                if (DrawIconTextButton("##ExportTexture", UiIconId::Export, "Export Texture", ImVec2(-1.0f, 0.0f)) &&
                    currentEntry)
                {
                    App::RequestTextureExport(app, preview.packPath, *currentEntry);
                }
                if (DrawIconTextButton("##ReloadPreview", UiIconId::Rebuild, "Reload Preview", ImVec2(-1.0f, 0.0f)) &&
                    currentEntry)
                {
                    ClearTexturePreview();
                    app.texturePreview.fitToWindow = true;
                    app.texturePreview.zoom = 1.0f;
                    App::RequestTexturePreview(app, preview.packPath, *currentEntry);
                }
                ImGui::EndDisabled();
                if (DrawIconTextButton("##ShowDxpInExplorer", UiIconId::ContentBrowser, "Show DXP in Explorer", ImVec2(-1.0f, 0.0f)))
                {
                    Platform::ShowPathInExplorer(preview.packPath);
                }
                ImGui::EndChild();

                ImGui::EndTable();
            }
            ImGui::EndPopup();
        }

        app.texturePreview.windowOpen = open;
    }

    void DrawModelPreviewPopup(AppState& app)
    {
        if (!app.modelPreview.popupOpen)
        {
            return;
        }

        // Check if there is pending preview data to upload
        std::shared_ptr<App::ModelPreviewData> pendingData = App::TakePendingModelPreview(app);
        if (pendingData)
        {
            ModelRenderer* renderer = GetModelRenderer();
            if (renderer)
            {
                if (renderer->SetModelData(pendingData->vertices, pendingData->indices))
                {
                    app.modelPreview.vertexCount = static_cast<uint32_t>(pendingData->vertices.size());
                    app.modelPreview.indexCount = static_cast<uint32_t>(pendingData->indices.size());
                    app.modelPreview.grpClassId = pendingData->grpClassId;
                    app.modelPreview.subMeshCount = pendingData->subMeshCount;
                    app.modelPreview.fileSize = pendingData->fileSize;
                    std::copy(std::begin(pendingData->bboxMin), std::end(pendingData->bboxMin), std::begin(app.modelPreview.bboxMin));
                    std::copy(std::begin(pendingData->bboxMax), std::end(pendingData->bboxMax), std::begin(app.modelPreview.bboxMax));
                    app.modelPreview.submeshes = std::move(pendingData->submeshes);
                    app.modelPreview.hoveredSubmeshIndex = -1;
                    app.modelPreview.vertices = std::move(pendingData->vertices);
                    app.modelPreview.indices = std::move(pendingData->indices);
                    app.modelPreview.bones = std::move(pendingData->bones);

                    // Auto-fit camera zoom
                    float dx = app.modelPreview.bboxMax[0] - app.modelPreview.bboxMin[0];
                    float dy = app.modelPreview.bboxMax[1] - app.modelPreview.bboxMin[1];
                    float dz = app.modelPreview.bboxMax[2] - app.modelPreview.bboxMin[2];
                    float bboxSize = std::sqrt(dx * dx + dy * dy + dz * dz);
                    app.modelPreview.zoom = bboxSize > 0.0f ? bboxSize * 1.5f : 5.0f;
                    app.modelPreview.panWorldX = 0.0f;
                    app.modelPreview.panWorldY = 0.0f;
                    app.modelPreview.panWorldZ = 0.0f;
                    app.modelPreview.yaw = 45.0f;
                    app.modelPreview.pitch = 30.0f;
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

        ImGui::SetNextWindowSize(ImVec2(950.0f, 600.0f), ImGuiCond_FirstUseEver);
        std::string popupName = "Model Preview - " + app.modelPreview.entryName + "###ModelPreviewModal";
        
        static bool wasOpen = false;
        if (!wasOpen)
        {
            ImGui::OpenPopup(popupName.c_str());
            wasOpen = true;
        }

        bool isOpen = true;
        if (ImGui::BeginPopupModal(popupName.c_str(), &isOpen))
        {
            if (app.modelPreview.loading)
            {
                ImGui::Text("Loading model geometry from package...");
            }
            else if (!app.modelPreview.error.empty())
            {
                DrawWrappedColoredText(
                    ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "Error: " + app.modelPreview.error);
            }
            else if (app.modelPreview.vertexCount == 0)
            {
                ImGui::Text("No model loaded.");
            }
            else
            {
                // Render details panel and viewport side-by-side
                float detailPanelWidth = 280.0f;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float contentWidth = ImGui::GetContentRegionAvail().x;
                float viewportWidth = std::max(100.0f, contentWidth - detailPanelWidth - spacing);

                // Viewport Child Window
                ImGui::BeginChild("3DViewportChild", ImVec2(viewportWidth, 0.0f), true);
                {
                    ImVec2 viewportSize = ImGui::GetContentRegionAvail();
                    // Leave space for buttons at the bottom of viewport child window
                    float controlsHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
                    viewportSize.y -= controlsHeight;
                    if (viewportSize.y < 50.0f) viewportSize.y = 50.0f;

                    ModelRenderer* renderer = GetModelRenderer();
                    ID3D11ShaderResourceView* srv = nullptr;
                    if (renderer)
                    {
                        // Mouse Raycasting and Hover Detection (only when right-click is held to save performance)
                        ImGuiIO& io = ImGui::GetIO();
                        bool rightMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                        int bestSubmeshIndex = -1;

                        if (rightMouseDown)
                        {
                            ImVec2 imagePos = ImGui::GetCursorScreenPos();
                            ImVec2 mousePos = io.MousePos;

                            float mx = mousePos.x - imagePos.x;
                            float my = mousePos.y - imagePos.y;
                            bool mouseInside = (mx >= 0.0f && mx < viewportSize.x && my >= 0.0f && my < viewportSize.y);

                            if (mouseInside && !app.modelPreview.vertices.empty())
                            {
                                using namespace DirectX;
                                float bboxMin[3];
                                float bboxMax[3];
                                std::copy(std::begin(app.modelPreview.bboxMin), std::end(app.modelPreview.bboxMin), bboxMin);
                                std::copy(std::begin(app.modelPreview.bboxMax), std::end(app.modelPreview.bboxMax), bboxMax);

                                float centerX = (bboxMin[0] + bboxMax[0]) * 0.5f;
                                float centerY = (bboxMin[1] + bboxMax[1]) * 0.5f;
                                float centerZ = (bboxMin[2] + bboxMax[2]) * 0.5f;

                                XMVECTOR target = XMVectorSet(
                                    centerX + app.modelPreview.panWorldX,
                                    centerY + app.modelPreview.panWorldY,
                                    centerZ + app.modelPreview.panWorldZ,
                                    1.0f
                                );

                                float theta = app.modelPreview.yaw * XM_PI / 180.0f;
                                float phi = app.modelPreview.pitch * XM_PI / 180.0f;

                                float cosP = cosf(phi);
                                float sinP = sinf(phi);
                                float cosY = cosf(theta);
                                float sinY = sinf(theta);

                                XMVECTOR offsetVec = XMVectorSet(
                                    app.modelPreview.zoom * cosP * sinY,
                                    app.modelPreview.zoom * sinP,
                                    -app.modelPreview.zoom * cosP * cosY,
                                    0.0f
                                );

                                XMVECTOR cameraPos = target + offsetVec;
                                XMVECTOR look = XMVector3Normalize(offsetVec);
                                XMVECTOR right = XMVector3Normalize(XMVector3Cross(look, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
                                XMVECTOR up = XMVector3Normalize(XMVector3Cross(right, look));

                                XMMATRIX view = XMMatrixLookAtLH(cameraPos, target, up);
                                float aspect = static_cast<float>(viewportSize.x) / static_cast<float>(viewportSize.y);
                                float nearPlane = std::max(0.01f, app.modelPreview.zoom * 0.01f);
                                float farPlane = std::max(10.0f, app.modelPreview.zoom * 10.0f);
                                XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, nearPlane, farPlane);
                                XMMATRIX viewProj = view * proj;

                                XMMATRIX invViewProj = XMMatrixInverse(nullptr, viewProj);

                                float ndcX = (2.0f * mx) / viewportSize.x - 1.0f;
                                float ndcY = 1.0f - (2.0f * my) / viewportSize.y;

                                XMVECTOR ndcNear = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
                                XMVECTOR ndcFar = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);

                                XMVECTOR worldNear = XMVector3TransformCoord(ndcNear, invViewProj);
                                XMVECTOR worldFar = XMVector3TransformCoord(ndcFar, invViewProj);

                                XMVECTOR rayDir = XMVector3Normalize(worldFar - worldNear);

                                Ray ray;
                                ray.origin = worldNear;
                                ray.direction = rayDir;

                                static auto lastLogTime = std::chrono::steady_clock::now();
                                auto now = std::chrono::steady_clock::now();
                                bool shouldLog = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count() >= 1;
                                if (shouldLog)
                                {
                                    lastLogTime = now;
                                    App::AddLog(app, App::LogLevel::Info, "Raycast check: mouse=" + std::to_string(mx) + "," + std::to_string(my) +
                                        " submeshes=" + std::to_string(app.modelPreview.submeshes.size()) +
                                        " vertices=" + std::to_string(app.modelPreview.vertices.size()) +
                                        " ndc=(" + std::to_string(ndcX) + "," + std::to_string(ndcY) + ")");
                                }

                                float bestT = FLT_MAX;

                                for (size_t subIdx = 0; subIdx < app.modelPreview.submeshes.size(); ++subIdx)
                                {
                                    const auto& submesh = app.modelPreview.submeshes[subIdx];
                                    float tNearBox = 0.0f;
                                    if (RayAABBIntersection(ray, submesh.bboxMin, submesh.bboxMax, tNearBox))
                                    {
                                        if (shouldLog)
                                        {
                                            App::AddLog(app, App::LogLevel::Info, "Box check hit: " + submesh.name + " tNear=" + std::to_string(tNearBox));
                                        }

                                        if (tNearBox < bestT)
                                        {
                                            bool hitTriangle = false;
                                            float submeshBestT = FLT_MAX;

                                            for (uint32_t i = 0; i + 2 < submesh.indexCount; i += 3)
                                            {
                                                uint32_t idx0 = app.modelPreview.indices[submesh.startIndex + i];
                                                uint32_t idx1 = app.modelPreview.indices[submesh.startIndex + i + 1];
                                                uint32_t idx2 = app.modelPreview.indices[submesh.startIndex + i + 2];

                                                if (idx0 < app.modelPreview.vertices.size() &&
                                                    idx1 < app.modelPreview.vertices.size() &&
                                                    idx2 < app.modelPreview.vertices.size())
                                                {
                                                    XMVECTOR v0 = XMVectorSet(app.modelPreview.vertices[idx0].pos[0], app.modelPreview.vertices[idx0].pos[1], app.modelPreview.vertices[idx0].pos[2], 1.0f);
                                                    XMVECTOR v1 = XMVectorSet(app.modelPreview.vertices[idx1].pos[0], app.modelPreview.vertices[idx1].pos[1], app.modelPreview.vertices[idx1].pos[2], 1.0f);
                                                    XMVECTOR v2 = XMVectorSet(app.modelPreview.vertices[idx2].pos[0], app.modelPreview.vertices[idx2].pos[1], app.modelPreview.vertices[idx2].pos[2], 1.0f);

                                                    float tTriangle = 0.0f;
                                                    if (RayTriangleIntersection(ray, v0, v1, v2, tTriangle))
                                                    {
                                                        hitTriangle = true;
                                                        if (tTriangle < submeshBestT)
                                                        {
                                                            submeshBestT = tTriangle;
                                                        }
                                                    }
                                                }
                                            }

                                            if (hitTriangle && submeshBestT < bestT)
                                            {
                                                bestT = submeshBestT;
                                                bestSubmeshIndex = static_cast<int>(subIdx);
                                                if (shouldLog)
                                                {
                                                    App::AddLog(app, App::LogLevel::Info, "New best hit: " + submesh.name + " t=" + std::to_string(bestT));
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        app.modelPreview.hoveredSubmeshIndex = bestSubmeshIndex;

                        auto start = std::chrono::high_resolution_clock::now();

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
                            app.modelPreview.hoveredSubmeshIndex,
                            app.modelPreview.submeshes
                        );

                        auto end = std::chrono::high_resolution_clock::now();
                        std::chrono::duration<float, std::milli> duration = end - start;
                        float latency = duration.count();
                        app.modelPreview.renderLatencyMs = latency;

                        app.modelPreview.fpsHistory[app.modelPreview.statOffset] = io.Framerate;
                        app.modelPreview.latencyHistory[app.modelPreview.statOffset] = latency;
                        app.modelPreview.frameTimeHistory[app.modelPreview.statOffset] = io.DeltaTime * 1000.0f;
                        app.modelPreview.statOffset = (app.modelPreview.statOffset + 1) % 90;
                    }

                    if (srv)
                    {
                        ImVec2 startCursorPos = ImGui::GetCursorPos();
                        ImGui::Image(reinterpret_cast<ImTextureID>(srv), viewportSize);

                        // Capture the correct viewport image rect in screen space right after rendering the Image.
                        // This prevents later ImGui elements (like overlays) from overriding GetItemRectMin().
                        ImVec2 imageMin = ImGui::GetItemRectMin();
                        ImVec2 imageMax = ImGui::GetItemRectMax();

                        { // Scope for projection and bone overlay drawing to prevent redefinition conflicts below
                            // Compute camera/projection vectors once for viewport overlays
                            using namespace DirectX;
                            float bboxMin[3];
                            float bboxMax[3];
                            std::copy(std::begin(app.modelPreview.bboxMin), std::end(app.modelPreview.bboxMin), bboxMin);
                            std::copy(std::begin(app.modelPreview.bboxMax), std::end(app.modelPreview.bboxMax), bboxMax);

                            float centerX = (bboxMin[0] + bboxMax[0]) * 0.5f;
                            float centerY = (bboxMin[1] + bboxMax[1]) * 0.5f;
                            float centerZ = (bboxMin[2] + bboxMax[2]) * 0.5f;

                            XMVECTOR target = XMVectorSet(
                                centerX + app.modelPreview.panWorldX,
                                centerY + app.modelPreview.panWorldY,
                                centerZ + app.modelPreview.panWorldZ,
                                1.0f
                            );

                            float theta = app.modelPreview.yaw * XM_PI / 180.0f;
                            float phi = app.modelPreview.pitch * XM_PI / 180.0f;

                            float cosP = cosf(phi);
                            float sinP = sinf(phi);
                            float cosY = cosf(theta);
                            float sinY = sinf(theta);

                            XMVECTOR offsetVec = XMVectorSet(
                                app.modelPreview.zoom * cosP * sinY,
                                app.modelPreview.zoom * sinP,
                                -app.modelPreview.zoom * cosP * cosY,
                                0.0f
                            );

                            XMVECTOR cameraPos = target + offsetVec;
                            XMVECTOR look = XMVector3Normalize(offsetVec);
                            XMVECTOR right = XMVector3Normalize(XMVector3Cross(look, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
                            XMVECTOR up = XMVector3Normalize(XMVector3Cross(right, look));

                            XMMATRIX view = XMMatrixLookAtLH(cameraPos, target, up);
                            float aspect = static_cast<float>(viewportSize.x) / static_cast<float>(viewportSize.y);
                            float nearPlane = std::max(0.01f, app.modelPreview.zoom * 0.01f);
                            float farPlane = std::max(10.0f, app.modelPreview.zoom * 10.0f);
                            XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, nearPlane, farPlane);
                            XMMATRIX viewProj = view * proj;

                            // Project and draw the submesh hover name label in the center of the outline (yellow)
                            bool rightMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                            if (rightMouseDown &&
                                app.modelPreview.hoveredSubmeshIndex >= 0 && 
                                app.modelPreview.hoveredSubmeshIndex < static_cast<int>(app.modelPreview.submeshes.size()))
                            {
                                const auto& submesh = app.modelPreview.submeshes[app.modelPreview.hoveredSubmeshIndex];
                                
                                float submeshCenterX = (submesh.bboxMin[0] + submesh.bboxMax[0]) * 0.5f;
                                float submeshCenterY = (submesh.bboxMin[1] + submesh.bboxMax[1]) * 0.5f;
                                float submeshCenterZ = (submesh.bboxMin[2] + submesh.bboxMax[2]) * 0.5f;

                                XMVECTOR center3D = XMVectorSet(submeshCenterX, submeshCenterY, submeshCenterZ, 1.0f);
                                XMVECTOR cameraToCenter = center3D - cameraPos;
                                XMVECTOR dotProduct = XMVector3Dot(cameraToCenter, look);

                                if (XMVectorGetX(dotProduct) < 0.0f)
                                {
                                    XMVECTOR projected = XMVector3TransformCoord(center3D, viewProj);
                                    float projX = XMVectorGetX(projected);
                                    float projY = XMVectorGetY(projected);

                                    if (projX >= -1.1f && projX <= 1.1f && projY >= -1.1f && projY <= 1.1f)
                                    {
                                        float screenX = imageMin.x + (projX + 1.0f) * 0.5f * viewportSize.x;
                                        float screenY = imageMin.y + (1.0f - projY) * 0.5f * viewportSize.y;

                                        ImDrawList* drawList = ImGui::GetWindowDrawList();
                                        std::string partName = submesh.name;
                                        ImGui::SetWindowFontScale(1.3f);
                                        ImVec2 textSize = ImGui::CalcTextSize(partName.c_str());
                                        ImVec2 textPos = ImVec2(screenX - textSize.x * 0.5f, screenY - textSize.y * 0.5f);

                                        // Shadow
                                        drawList->AddText(ImVec2(textPos.x + 1.0f, textPos.y + 1.0f), IM_COL32(0, 0, 0, 255), partName.c_str());
                                        // Text in white
                                        drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), partName.c_str());
                                        ImGui::SetWindowFontScale(1.0f);
                                    }
                                }
                            }

                            // Project and draw the bone armature overlay (cyan lines and green joint circles) if enabled
                            if (app.modelPreview.showBones && !app.modelPreview.bones.empty())
                            {
                                std::vector<ImVec2> boneScreenCoords(app.modelPreview.bones.size());
                                std::vector<bool> boneValid(app.modelPreview.bones.size(), false);

                                for (size_t i = 0; i < app.modelPreview.bones.size(); ++i)
                                {
                                    const auto& bone = app.modelPreview.bones[i];
                                    XMVECTOR center3D = XMVectorSet(bone.worldPos[0], bone.worldPos[1], bone.worldPos[2], 1.0f);
                                    XMVECTOR cameraToCenter = center3D - cameraPos;
                                    XMVECTOR dotProduct = XMVector3Dot(cameraToCenter, look);

                                    if (XMVectorGetX(dotProduct) < 0.0f)
                                    {
                                        XMVECTOR projected = XMVector3TransformCoord(center3D, viewProj);
                                        float projX = XMVectorGetX(projected);
                                        float projY = XMVectorGetY(projected);

                                        if (projX >= -2.0f && projX <= 2.0f && projY >= -2.0f && projY <= 2.0f)
                                        {
                                            boneScreenCoords[i].x = imageMin.x + (projX + 1.0f) * 0.5f * viewportSize.x;
                                            boneScreenCoords[i].y = imageMin.y + (1.0f - projY) * 0.5f * viewportSize.y;
                                            boneValid[i] = true;
                                        }
                                    }
                                }

                                ImDrawList* drawList = ImGui::GetWindowDrawList();

                                // 1. Draw parent-child bone line connections
                                for (size_t i = 0; i < app.modelPreview.bones.size(); ++i)
                                {
                                    const auto& bone = app.modelPreview.bones[i];
                                    if (bone.parentIndex != 0xFFFFu && bone.parentIndex < app.modelPreview.bones.size())
                                    {
                                        if (boneValid[i] && boneValid[bone.parentIndex])
                                        {
                                            drawList->AddLine(boneScreenCoords[i], boneScreenCoords[bone.parentIndex], IM_COL32(0, 255, 255, 180), 2.0f);
                                        }
                                    }
                                }

                                // 2. Draw joint circle nodes
                                for (size_t i = 0; i < app.modelPreview.bones.size(); ++i)
                                {
                                    if (boneValid[i])
                                    {
                                        drawList->AddCircleFilled(boneScreenCoords[i], 3.0f, IM_COL32(0, 255, 0, 220));
                                    }
                                }
                            }
                        }

                        // Draw transparent performance overlay on top-left of the viewport image
                        ImVec2 oldCursorPos = ImGui::GetCursorPos();
                        ImVec2 localPos = ImVec2(imageMin.x - ImGui::GetWindowPos().x + ImGui::GetScrollX(),
                                                 imageMin.y - ImGui::GetWindowPos().y + ImGui::GetScrollY());

                        ImGui::SetCursorPos(ImVec2(localPos.x + 8.0f, localPos.y + 8.0f));
                        ImGui::BeginGroup();
                        {
                            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.11f, 0.75f));
                            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.27f, 0.30f, 0.60f));
                            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 5.0f));

                            if (ImGui::BeginChild("PerfOverlay", ImVec2(300.0f, 220.0f), true, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                            {
                                ImGuiIO& io = ImGui::GetIO();
                                
                                const UiIcon* debugIcon = GetUiIcon(UiIconId::Debug);
                                if (debugIcon && debugIcon->view != nullptr)
                                {
                                    ImGui::Image(reinterpret_cast<ImTextureID>(debugIcon->view), ImVec2(14.0f, 14.0f));
                                    ImGui::SameLine(0.0f, 6.0f);
                                }
                                ImGui::TextDisabled("PERFORMANCE OVERLAY");
                                
                                ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "FPS: %.1f", io.Framerate);
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "GPU: %.2f ms", app.modelPreview.renderLatencyMs);
                                ImGui::SameLine();
                                ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "CPU: %.1f ms", io.DeltaTime * 1000.0f);

                                float maxMs = 0.01f;
                                float maxFps = 0.01f;
                                for (int i = 0; i < 90; ++i)
                                {
                                    if (app.modelPreview.frameTimeHistory[i] > maxMs) maxMs = app.modelPreview.frameTimeHistory[i];
                                    if (app.modelPreview.latencyHistory[i] > maxMs) maxMs = app.modelPreview.latencyHistory[i];
                                    if (app.modelPreview.fpsHistory[i] > maxFps) maxFps = app.modelPreview.fpsHistory[i];
                                }
                                maxMs *= 1.15f;
                                maxFps *= 1.15f;

                                ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                                ImVec2 canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 70.0f);
                                ImDrawList* drawList = ImGui::GetWindowDrawList();

                                drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(15, 17, 20, 230));
                                drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(50, 55, 62, 255));

                                for (int i = 1; i < 4; ++i)
                                {
                                    float gridY = canvasPos.y + (canvasSize.y / 4.0f) * i;
                                    drawList->AddLine(ImVec2(canvasPos.x, gridY), ImVec2(canvasPos.x + canvasSize.x, gridY), IM_COL32(40, 45, 50, 100));
                                }

                                // 1. CPU Frame Time (Blue)
                                for (int i = 0; i < 89; ++i)
                                {
                                    int idx0 = (app.modelPreview.statOffset + i) % 90;
                                    int idx1 = (app.modelPreview.statOffset + i + 1) % 90;

                                    float val0 = app.modelPreview.frameTimeHistory[idx0];
                                    float val1 = app.modelPreview.frameTimeHistory[idx1];

                                    float x0 = canvasPos.x + (static_cast<float>(i) / 89.0f) * canvasSize.x;
                                    float x1 = canvasPos.x + (static_cast<float>(i + 1) / 89.0f) * canvasSize.x;

                                    float y0 = canvasPos.y + canvasSize.y - (val0 / maxMs) * canvasSize.y;
                                    float y1 = canvasPos.y + canvasSize.y - (val1 / maxMs) * canvasSize.y;

                                    drawList->AddQuadFilled(
                                        ImVec2(x0, canvasPos.y + canvasSize.y),
                                        ImVec2(x0, y0),
                                        ImVec2(x1, y1),
                                        ImVec2(x1, canvasPos.y + canvasSize.y),
                                        IM_COL32(65, 105, 225, 45)
                                    );
                                }

                                // 2. GPU Render Latency (Cyan)
                                for (int i = 0; i < 89; ++i)
                                {
                                    int idx0 = (app.modelPreview.statOffset + i) % 90;
                                    int idx1 = (app.modelPreview.statOffset + i + 1) % 90;

                                    float val0 = app.modelPreview.latencyHistory[idx0];
                                    float val1 = app.modelPreview.latencyHistory[idx1];

                                    float x0 = canvasPos.x + (static_cast<float>(i) / 89.0f) * canvasSize.x;
                                    float x1 = canvasPos.x + (static_cast<float>(i + 1) / 89.0f) * canvasSize.x;

                                    float y0 = canvasPos.y + canvasSize.y - (val0 / maxMs) * canvasSize.y;
                                    float y1 = canvasPos.y + canvasSize.y - (val1 / maxMs) * canvasSize.y;

                                    drawList->AddQuadFilled(
                                        ImVec2(x0, canvasPos.y + canvasSize.y),
                                        ImVec2(x0, y0),
                                        ImVec2(x1, y1),
                                        ImVec2(x1, canvasPos.y + canvasSize.y),
                                        IM_COL32(0, 255, 255, 75)
                                    );
                                }

                                // 3. FPS Line (Yellow)
                                for (int i = 0; i < 89; ++i)
                                {
                                    int idx0 = (app.modelPreview.statOffset + i) % 90;
                                    int idx1 = (app.modelPreview.statOffset + i + 1) % 90;

                                    float val0 = app.modelPreview.fpsHistory[idx0];
                                    float val1 = app.modelPreview.fpsHistory[idx1];

                                    float x0 = canvasPos.x + (static_cast<float>(i) / 89.0f) * canvasSize.x;
                                    float x1 = canvasPos.x + (static_cast<float>(i + 1) / 89.0f) * canvasSize.x;

                                    float y0 = canvasPos.y + canvasSize.y - (val0 / maxFps) * canvasSize.y;
                                    float y1 = canvasPos.y + canvasSize.y - (val1 / maxFps) * canvasSize.y;

                                    drawList->AddLine(
                                        ImVec2(x0, y0),
                                        ImVec2(x1, y1),
                                        IM_COL32(255, 215, 0, 240),
                                        1.8f
                                    );
                                }

                                ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y + 4.0f));

                                std::string gpuName = renderer->GetGPUName();
                                uint64_t vramUsage = 0;
                                uint64_t vramBudget = 0;
                                renderer->GetVRAMInfo(vramUsage, vramBudget);

                                float usageMb = static_cast<float>(vramUsage) / (1024.0f * 1024.0f);
                                float budgetMb = static_cast<float>(vramBudget) / (1024.0f * 1024.0f);

                                if (gpuName.length() > 38)
                                {
                                    gpuName = gpuName.substr(0, 35) + "...";
                                }

                                ImGui::TextDisabled("GPU: %s", gpuName.c_str());

                                char vramText[64];
                                if (vramBudget > 0)
                                {
                                    float fraction = usageMb / budgetMb;
                                    snprintf(vramText, sizeof(vramText), "VRAM: %.1f / %.0f MB (%.1f%%)", usageMb, budgetMb, fraction * 100.0f);
                                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.72f, 0.35f, 0.85f));
                                    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 15.0f), vramText);
                                    ImGui::PopStyleColor();
                                }
                                else
                                {
                                    snprintf(vramText, sizeof(vramText), "VRAM Allocation: %.1f MB", usageMb);
                                    ImGui::ProgressBar(0.0f, ImVec2(-1.0f, 15.0f), vramText);
                                }

                                uint32_t triangleCount = app.modelPreview.indexCount / 3;
                                float throughput = (static_cast<float>(triangleCount) * io.Framerate) / 1000000.0f;
                                ImGui::Text("Throughput: %.2f Mtris/sec (%u tris)", throughput, triangleCount);

                                ImGui::EndChild();
                            }
                            ImGui::PopStyleVar(2);
                            ImGui::PopStyleColor(2);
                        }
                        ImGui::EndGroup();

                        // Project and draw 3D Axis Gizmo in bottom left of the viewport image
                        ImDrawList* drawList = ImGui::GetWindowDrawList();

                        float gizmoSize = 30.0f;
                        ImVec2 gizmoCenter = ImVec2(imageMin.x + gizmoSize + 15.0f, imageMax.y - gizmoSize - 15.0f);

                        // Compute camera vectors for orientation projection
                        float theta = app.modelPreview.yaw * 3.14159265f / 180.0f;
                        float phi = app.modelPreview.pitch * 3.14159265f / 180.0f;

                        float cosP = cosf(phi);
                        float sinP = sinf(phi);
                        float cosY = cosf(theta);
                        float sinY = sinf(theta);

                        // Look vector (from target to camera)
                        float lookX = cosP * sinY;
                        float lookY = sinP;
                        float lookZ = -cosP * cosY;

                        // Right vector: cross(look, (0,1,0))
                        float rightX = -lookZ;
                        float rightY = 0.0f;
                        float rightZ = lookX;
                        float rightLen = sqrtf(rightX * rightX + rightZ * rightZ);
                        if (rightLen > 1e-5f)
                        {
                            rightX /= rightLen;
                            rightZ /= rightLen;
                        }

                        // Up vector: cross(right, look)
                        float upX = rightY * lookZ - rightZ * lookY;
                        float upY = rightZ * lookX - rightX * lookZ;
                        float upZ = rightX * lookY - rightY * lookX;
                        float upLen = sqrtf(upX * upX + upY * upY + upZ * upZ);
                        if (upLen > 1e-5f)
                        {
                            upX /= upLen;
                            upY /= upLen;
                            upZ /= upLen;
                        }

                        // Projection: projects 3D coords onto screen space relative to center
                        auto ProjectAxis = [&](float x, float y, float z) -> ImVec2
                        {
                            float sx = x * rightX + y * rightY + z * rightZ;
                            float sy = x * upX + y * upY + z * upZ;
                            return ImVec2(sx * gizmoSize, -sy * gizmoSize); // Invert Y
                        };

                        ImVec2 endX = ImVec2(gizmoCenter.x + ProjectAxis(1.0f, 0.0f, 0.0f).x, gizmoCenter.y + ProjectAxis(1.0f, 0.0f, 0.0f).y);
                        ImVec2 endY = ImVec2(gizmoCenter.x + ProjectAxis(0.0f, 1.0f, 0.0f).x, gizmoCenter.y + ProjectAxis(0.0f, 1.0f, 0.0f).y);
                        ImVec2 endZ = ImVec2(gizmoCenter.x + ProjectAxis(0.0f, 0.0f, 1.0f).x, gizmoCenter.y + ProjectAxis(0.0f, 0.0f, 1.0f).y);

                        // Back-to-front depth sort (Z-sorting relative to camera look vector)
                        struct AxisDraw
                        {
                            ImVec2 end;
                            ImU32 color;
                            const char* label;
                            float depth;
                        };
                        AxisDraw axes[3] = {
                            { endX, IM_COL32(255, 64, 64, 255), "X", lookX }, // X (Red)
                            { endY, IM_COL32(64, 255, 64, 255), "Y", lookY }, // Y (Green)
                            { endZ, IM_COL32(64, 128, 255, 255), "Z", lookZ }  // Z (Blue)
                        };
                        std::sort(std::begin(axes), std::end(axes), [](const AxisDraw& a, const AxisDraw& b) {
                            return a.depth > b.depth;
                        });

                        for (const auto& axis : axes)
                        {
                            drawList->AddLine(gizmoCenter, axis.end, axis.color, 2.0f);
                            drawList->AddCircleFilled(axis.end, 7.5f, axis.color);
                            ImVec2 textSize = ImGui::CalcTextSize(axis.label);
                            drawList->AddText(ImVec2(axis.end.x - textSize.x * 0.5f, axis.end.y - textSize.y * 0.5f), IM_COL32(255, 255, 255, 255), axis.label);
                        }

                        // Overlay the invisible button exactly on top of the image to handle drag inputs and prevent window movement conflict
                        ImGui::SetCursorPos(startCursorPos);
                        ImGui::InvisibleButton("ViewportInteract", viewportSize);

                        const bool isHovered = ImGui::IsItemHovered();
                        const bool isActive = ImGui::IsItemActive();
                        ImGuiIO& io = ImGui::GetIO();

                        // Scroll Zoom
                        if (isHovered && io.MouseWheel != 0.0f)
                        {
                            float zoomSpeed = std::max(0.1f, app.modelPreview.zoom * 0.15f);
                            app.modelPreview.zoom -= io.MouseWheel * zoomSpeed;
                            app.modelPreview.zoom = std::max(0.01f, app.modelPreview.zoom);
                        }

                        // Middle Mouse Button Drag State
                        static bool draggingMiddle = false;
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && isHovered) draggingMiddle = true;
                        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) draggingMiddle = false;

                        if (draggingMiddle)
                        {
                            if (io.KeyShift) // Pan in world-space using camera basis vectors
                            {
                                float panSpeed = std::max(0.001f, app.modelPreview.zoom * 0.002f);
                                float dx = -io.MouseDelta.x * panSpeed;
                                float dy = io.MouseDelta.y * panSpeed;
                                app.modelPreview.panWorldX += rightX * dx + upX * dy;
                                app.modelPreview.panWorldY += rightY * dx + upY * dy;
                                app.modelPreview.panWorldZ += rightZ * dx + upZ * dy;
                            }
                            else if (io.KeyCtrl) // Zoom
                            {
                                float zoomSpeed = std::max(0.1f, app.modelPreview.zoom * 0.005f);
                                app.modelPreview.zoom -= io.MouseDelta.y * zoomSpeed;
                                app.modelPreview.zoom = std::max(0.01f, app.modelPreview.zoom);
                            }
                            else // Orbit/Rotate
                            {
                                app.modelPreview.yaw -= io.MouseDelta.x * 0.5f;
                                app.modelPreview.pitch += io.MouseDelta.y * 0.5f;
                                app.modelPreview.pitch = std::clamp(app.modelPreview.pitch, -89.0f, 89.0f);
                            }
                        }
                        // Left Mouse Button Fallback Drag (Trackpad support, intercepts click so parent window doesn't move)
                        else if (isActive)
                        {
                            if (io.KeyShift) // Pan in world-space using camera basis vectors
                            {
                                float panSpeed = std::max(0.001f, app.modelPreview.zoom * 0.002f);
                                float dx = -io.MouseDelta.x * panSpeed;
                                float dy = io.MouseDelta.y * panSpeed;
                                app.modelPreview.panWorldX += rightX * dx + upX * dy;
                                app.modelPreview.panWorldY += rightY * dx + upY * dy;
                                app.modelPreview.panWorldZ += rightZ * dx + upZ * dy;
                            }
                            else // Orbit/Rotate
                            {
                                app.modelPreview.yaw -= io.MouseDelta.x * 0.5f;
                                app.modelPreview.pitch += io.MouseDelta.y * 0.5f;
                                app.modelPreview.pitch = std::clamp(app.modelPreview.pitch, -89.0f, 89.0f);
                            }
                        }

                        ImGui::SetCursorPos(oldCursorPos);
                    }
                    else
                    {
                        ImGui::Text("D3D11 Viewport rendering unavailable");
                    }

                    // Viewport controls toolbar
                    ImGui::Separator();
                    
                    // Render mode dropdown combo
                    ImGui::Text("Render Mode");
                    ImGui::SameLine();
                    const char* renderModes[] = { "Solid", "Wireframe", "Solid + Wireframe" };
                    ImGui::SetNextItemWidth(130.0f);
                    ImGui::Combo("##RenderMode", &app.modelPreview.renderMode, renderModes, IM_ARRAYSIZE(renderModes));
                    
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    
                    // Show Armature (always Text first then toggle)
                    ImGui::Text("Show Armature");
                    ImGui::SameLine();
                    ImGui::BeginDisabled(app.modelPreview.bones.empty());
                    ImGui::Checkbox("##ShowArmature", &app.modelPreview.showBones);
                    ImGui::EndDisabled();
                    
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    
                    // Ground Grid (always Text first then toggle)
                    ImGui::Text("Ground Grid");
                    ImGui::SameLine();
                    ImGui::Checkbox("##GroundGrid", &app.modelPreview.lighting.showGround);
                    
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                    
                    if (ImGui::Button("Reset Camera"))
                    {
                        app.modelPreview.yaw = 45.0f;
                        app.modelPreview.pitch = 30.0f;
                        app.modelPreview.panWorldX = 0.0f;
                        app.modelPreview.panWorldY = 0.0f;
                        app.modelPreview.panWorldZ = 0.0f;
                        float dx = app.modelPreview.bboxMax[0] - app.modelPreview.bboxMin[0];
                        float dy = app.modelPreview.bboxMax[1] - app.modelPreview.bboxMin[1];
                        float dz = app.modelPreview.bboxMax[2] - app.modelPreview.bboxMin[2];
                        float bboxSize = std::sqrt(dx * dx + dy * dy + dz * dz);
                        app.modelPreview.zoom = bboxSize > 0.0f ? bboxSize * 1.5f : 5.0f;
                    }

                    // Input navigation map/legend
                    ImGui::SameLine();
                    DrawWrappedDisabledText(
                        "Middle Drag: Orbit | Shift + Middle Drag: Pan | Scroll: Zoom");
                }
                ImGui::EndChild();

                ImGui::SameLine();

                // Detail Panel Child Window
                ImGui::BeginChild("DetailPanelChild", ImVec2(0.0f, 0.0f), true);
                {
                    UiIconId iconId = UiIconId::Invalid;
                    std::string className = AssetIndex::DescribeGrpClassId(app.modelPreview.grpClassId);
                    if (className == "DynModel")
                    {
                        iconId = UiIconId::DynModel;
                    }
                    else if (className == "RendInst")
                    {
                        iconId = UiIconId::RendInst;
                    }
                    
                    DrawPanelTitle(iconId, "Preview Model");
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::CollapsingHeader("MODEL DETAILS", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::Spacing();
                        DrawWrappedText("Name: " + app.modelPreview.entryName);
                        DrawWrappedText(
                            "Class: " +
                            AssetIndex::DescribeGrpClassId(
                                app.modelPreview.grpClassId));
                        ImGui::Text("File Size: %s", FormatBytes(app.modelPreview.fileSize).c_str());
                        ImGui::Text("Submeshes: %u", app.modelPreview.subMeshCount);
                        ImGui::Spacing();
                    }

                    if (ImGui::CollapsingHeader("GEOMETRY STATS", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::Spacing();
                        ImGui::Text("Vertices: %u", app.modelPreview.vertexCount);
                        ImGui::Text("Indices:  %u", app.modelPreview.indexCount);
                        ImGui::Text("Triangles: %u", app.modelPreview.indexCount / 3);
                        ImGui::Spacing();
                    }

                    if (ImGui::CollapsingHeader("BOUNDING BOX", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::Spacing();
                        float sizeX = app.modelPreview.bboxMax[0] - app.modelPreview.bboxMin[0];
                        float sizeY = app.modelPreview.bboxMax[1] - app.modelPreview.bboxMin[1];
                        float sizeZ = app.modelPreview.bboxMax[2] - app.modelPreview.bboxMin[2];

                        ImGui::Text("Min: (%.3f, %.3f, %.3f)", app.modelPreview.bboxMin[0], app.modelPreview.bboxMin[1], app.modelPreview.bboxMin[2]);
                        ImGui::Text("Max: (%.3f, %.3f, %.3f)", app.modelPreview.bboxMax[0], app.modelPreview.bboxMax[1], app.modelPreview.bboxMax[2]);
                        ImGui::Text("Size: %.3f x %.3f x %.3f", sizeX, sizeY, sizeZ);
                        ImGui::Spacing();
                    }

                    ImGui::Spacing();
                    if (ImGui::CollapsingHeader("LIGHTING & SCENE", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::Spacing();
                        ImGui::TextDisabled("Light Angles");
                        ImGui::SliderFloat("Light Yaw", &app.modelPreview.lighting.lightYaw, -180.0f, 180.0f, "%.0f deg");
                        ImGui::SliderFloat("Light Pitch", &app.modelPreview.lighting.lightPitch, -90.0f, 90.0f, "%.0f deg");

                        ImGui::Spacing();
                        ImGui::TextDisabled("Specular Settings");
                        ImGui::SliderFloat("Glossiness", &app.modelPreview.lighting.specPower, 1.0f, 128.0f, "%.0f");
                        ImGui::SliderFloat("Intensity", &app.modelPreview.lighting.specIntensity, 0.0f, 2.0f, "%.2f");

                        ImGui::Spacing();
                        ImGui::TextDisabled("Colors");
                        ImGui::ColorEdit3("Key Light", app.modelPreview.lighting.keyColor);
                        ImGui::ColorEdit3("Fill Light", app.modelPreview.lighting.fillColor);
                        ImGui::ColorEdit3("Ambient", app.modelPreview.lighting.ambientColor);
                        ImGui::ColorEdit3("Clay Color", app.modelPreview.lighting.baseColor);
                        ImGui::ColorEdit3("Background", app.modelPreview.lighting.backgroundColor);

                        ImGui::Spacing();
                        if (ImGui::Button("Reset Lighting", ImVec2(-1.0f, 0.0f)))
                        {
                            app.modelPreview.lighting = App::ModelPreviewLighting();
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Button("Export to GLB", ImVec2(-1.0f, 0.0f)))
                    {
                        constexpr uint32_t kDynModelClassId = 0xB4B7D9C4u;
                        const bool isDynModel = (app.modelPreview.grpClassId == kDynModelClassId);
                        if (isDynModel)
                        {
                            App::OpenDynModelExportModalByName(app, app.modelPreview.packPath, app.modelPreview.entryName);
                        }
                        else
                        {
                            App::RequestModelExportByName(app, app.modelPreview.packPath, app.modelPreview.entryName);
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndPopup();
        }

        if (!isOpen)
        {
            app.modelPreview.popupOpen = false;
            wasOpen = false;
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
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
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
        ImGui::PopStyleVar(1);

        ImGui::BeginChild("InspectorPanel", ImVec2(0.0f, 0.0f), true);
        DrawInspector(app, records);
        ImGui::EndChild();

        ImGui::EndChild();

        // Horizontal splitter between Top Area and Bottom Panel
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
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
        ImGui::PopStyleVar(1);

        ImGui::BeginChild("BottomPanel", ImVec2(0.0f, 0.0f), true);
        DrawJobAndLogPanel(app);
        ImGui::EndChild();

        ImGui::End();

        DrawSettingsWindow(app);
        DrawAboutWindow(app);
        DrawFirstRunModal(app);
        DrawDynModelExportModal(app);
        DrawProgressJobWindow(app);
        DrawRebuildProgressModal(app);
        DrawBlkRebuildProgressModal(app);
        DrawModelPreviewPopup(app);
        DrawTexturePreviewWindow(app);
        DrawExportProgressModal(app);
    }
}
