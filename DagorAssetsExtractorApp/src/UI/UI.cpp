#include "UI/UI.h"
#include "UI/UiAssets.h"
#include "UI/ModelRenderer.h"
#include "App/AppState.h"
#include "Platform/Platform.h"
#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/PackContents.h"
#include "DagorFormats/BlkReader.h"

#include "imgui.h"

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

    bool DrawIconTextButton(const char* id, UiIconId iconId, const char* label)
    {
        const UiIcon* icon = GetUiIcon(iconId);
        const bool hasIcon = icon != nullptr && icon->view != nullptr;
        const float iconSize = hasIcon ? GetInlineIconSize() : 0.0f;
        const ImVec2 size = CalcIconButtonSize(label, hasIcon);
        const bool pressed = ImGui::Button(id, size);

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        if (hasIcon)
        {
            const float iconY = min.y + (size.y - iconSize) * 0.5f;
            drawList->AddImage(
                reinterpret_cast<ImTextureID>(icon->view),
                ImVec2(min.x + style.FramePadding.x, iconY),
                ImVec2(min.x + style.FramePadding.x + iconSize, iconY + iconSize));
        }

        const float textX = min.x + style.FramePadding.x + (hasIcon ? iconSize + kButtonIconSpacing : 0.0f);
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

    void DrawValidationSummary(const AssetIndex::ValidationResult& validation)
    {
        ImGui::TextColored(validation.IsValid() ? ImVec4(0.43f, 0.86f, 0.55f, 1.0f) : ImVec4(1.0f, 0.49f, 0.42f, 1.0f),
            validation.IsValid() ? "Valid War Thunder folder" : "No valid game folder");

        for (const std::string& message : validation.messages)
        {
            ImGui::BulletText("%s", message.c_str());
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

    void DrawHeader(AppState& app)
    {
        ImGui::TextUnformatted("Dagor Assets Extractor");
        ImGui::SameLine();
        ImGui::TextColored(
            app.validation.IsValid() ? ImVec4(0.43f, 0.86f, 0.55f, 1.0f) : ImVec4(1.0f, 0.49f, 0.42f, 1.0f),
            app.validation.IsValid() ? "Ready" : "Game folder required");

        const UiIcon* rebuildIcon = GetUiIcon(UiIconId::Rebuild);
        const bool rebuildHasIcon = rebuildIcon != nullptr && rebuildIcon->view != nullptr;
        const ImVec2 rebuildButtonSize = CalcIconButtonSize("Rebuild Resources", rebuildHasIcon);
        const UiIcon* patreonIcon = GetUiIcon(UiIconId::Patreon);
        const bool patreonHasIcon = patreonIcon != nullptr && patreonIcon->view != nullptr;
        const ImVec2 patreonButtonSize = CalcIconButtonSize("Donate", patreonHasIcon);

        const float buttonsWidth = rebuildButtonSize.x + ImGui::GetStyle().ItemSpacing.x + patreonButtonSize.x;
        ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionAvail().x - buttonsWidth));

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
        DrawPanelTitle(UiIconId::GameInstall, "Game Install");
        ImGui::Separator();
        if (app.settings.gameRoot.empty())
        {
            ImGui::TextWrapped("No folder selected.");
        }
        else
        {
            ImGui::TextWrapped("%s", Platform::PathToUtf8(app.settings.gameRoot).c_str());
        }

        if (ImGui::Button("Choose Folder", ImVec2(-1.0f, 0.0f)))
        {
            if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose War Thunder Folder", app.settings.gameRoot))
            {
                App::SetGameRoot(app, *selected);
                SaveSettingsWithImGui(app);
            }
        }

        ImGui::Spacing();
        DrawValidationSummary(app.validation);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Filters");
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
    }

    void DrawContentBrowser(AppState& app, const std::shared_ptr<const std::vector<ResourceRecord>>& recordsPtr)
    {
        DrawPanelTitle(UiIconId::ContentBrowser, "Content Browser");
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

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(AssetIndex::AssetTypeName(record.type));

                    ImGui::TableSetColumnIndex(1);
                    const std::string name = Platform::PathToUtf8(record.path.filename());
                    ImGuiSelectableFlags selectableFlags = ImGuiSelectableFlags_SpanAllColumns;
                    if (ImGui::Selectable(name.c_str(), selected, selectableFlags))
                    {
                        app.selectedPath = nativePath;
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(Platform::PathToUtf8(record.path.parent_path()).c_str());

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
                ImGui::TextUnformatted(row->resourceName.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(AssetIndex::AssetTypeName(row->expectedType));

                ImGui::TableSetColumnIndex(2);
                if (row->matchedPath)
                {
                    ImGui::TextUnformatted(Platform::PathToUtf8(*row->matchedPath).c_str());
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
            ImGui::SetTooltip("Descriptor links are already built globally.");
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
                    ImGui::TextUnformatted(link.resourceName.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("0x%X", link.resId);

                    ImGui::TableSetColumnIndex(2);
                    if (link.matchedGrpPath)
                    {
                        ImGui::TextUnformatted(link.matchedGrpEntryName ? link.matchedGrpEntryName->c_str() : "Root");
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", Platform::PathToUtf8(*link.matchedGrpPath).c_str());
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
                        ImGui::SetTooltip("%zu resolved texture pack matches out of %zu descriptor texture references.",
                            resolvedTextureCount, link.textureLinks.size());
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
                            ImGui::SetTooltip("This descriptor resource is not matched to any GRP entry yet.");
                        }
                        else if (!link.matchedGrpEntryName)
                        {
                            ImGui::SetTooltip("The matched GRP entry name is missing.");
                        }
                        else
                        {
                            std::string tooltip = AssetIndex::GetUnsupportedExportTooltip(link.matchedGrpClassId, link.matchedGrpClassName);
                            ImGui::SetTooltip("%s", tooltip.c_str());
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            const AssetIndex::DescriptorResourceLink& primaryLink = (*links)[app.descriptorUiDescriptorMatches.front()];
            ImGui::Text("Primary resource preview: %s", primaryLink.resourceName.c_str());
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
                        ImGui::TextUnformatted(texLink.textureName.c_str());

                        ImGui::TableSetColumnIndex(2);
                        if (texLink.matchedDxpPath)
                        {
                            ImGui::TextUnformatted(Platform::PathToUtf8(texLink.matchedDxpPath->filename()).c_str());
                            if (ImGui::IsItemHovered())
                            {
                                ImGui::SetTooltip("%s", Platform::PathToUtf8(*texLink.matchedDxpPath).c_str());
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
                        ImGui::TextUnformatted(match.resourceName.c_str());

                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(match.matchedGrpEntryName ? match.matchedGrpEntryName->c_str() : "Root");

                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(Platform::PathToUtf8(match.descriptorPath.filename()).c_str());
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", Platform::PathToUtf8(match.descriptorPath).c_str());
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
                        ImGui::TextUnformatted(textureLink.matchedDxpEntryName ? textureLink.matchedDxpEntryName->c_str() : "Root");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(match.resourceName.c_str());

                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(Platform::PathToUtf8(match.descriptorPath.filename()).c_str());
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", Platform::PathToUtf8(match.descriptorPath).c_str());
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
        ImGui::Text("Name: %s", Platform::PathToUtf8(selected->path.filename()).c_str());
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
                const bool hasActions = (isDxp || isGrp);

                int columnCount = 3;
                if (isDxp) columnCount = 5;      // Name, Offset, Size, Format, Actions
                else if (isGrp) columnCount = 5; // Name, Offset, Size, Class, Actions
                else if (isDesc) columnCount = 4; // Resource Name, ID, Class, Deps

                ImGuiTableFlags subFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;

                const float tableHeight =
                    std::clamp(ImGui::GetContentRegionAvail().y * 0.58f, 260.0f, 460.0f);
                if (ImGui::BeginTable("SubAssets", columnCount, subFlags, ImVec2(0.0f, tableHeight)))
                {
                    if (isDesc)
                    {
                        ImGui::TableSetupColumn("Resource Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Resource ID", ImGuiTableColumnFlags_WidthFixed, 88.0f);
                        ImGui::TableSetupColumn("Class Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn("Deps", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    }
                    else
                    {
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        if (isDxp)
                        {
                            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                        }
                        else if (isGrp)
                        {
                            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                        }
                        if (hasActions)
                        {
                            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 100.0f);
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
                            ImGui::TableNextRow();

                            if (isDesc)
                            {
                                ImGui::TableSetColumnIndex(0);
                                DrawInlineIconLabel(GetSubAssetIconId(entry, selected->type), entry.name.c_str());

                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("0x%X", entry.resId);

                                ImGui::TableSetColumnIndex(2);
                                if (!entry.formatInfo.empty())
                                {
                                    ImGui::TextUnformatted(entry.formatInfo.c_str());
                                }
                                else
                                {
                                    ImGui::TextDisabled("N/A");
                                }

                                ImGui::TableSetColumnIndex(3);
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
                                            ImGui::BulletText("%s", entry.textureNames[t].c_str());
                                        }
                                        if (entry.textureNames.size() > 8) ImGui::BulletText("...and %zu more", entry.textureNames.size() - 8);
                                    }
                                    
                                    if (!entry.materialNames.empty())
                                    {
                                        ImGui::Separator();
                                        ImGui::TextUnformatted("Materials:");
                                        for (size_t m = 0; m < std::min(entry.materialNames.size(), size_t(8)); ++m)
                                        {
                                            ImGui::BulletText("%s", entry.materialNames[m].c_str());
                                        }
                                        if (entry.materialNames.size() > 8) ImGui::BulletText("...and %zu more", entry.materialNames.size() - 8);
                                    }
                                    ImGui::EndTooltip();
                                }
                            }
                            else
                            {
                                ImGui::TableSetColumnIndex(0);
                                if (isDxp)
                                {
                                    const bool isSelectedTexture =
                                        texturePreview.packPath == selected->path &&
                                        texturePreview.entryName == entry.name &&
                                        texturePreview.entryOffset == entry.offset;
                                    if (ImGui::Selectable(entry.name.c_str(), isSelectedTexture))
                                    {
                                        ClearTexturePreview();
                                        app.texturePreview.fitToWindow = true;
                                        app.texturePreview.zoom = 1.0f;
                                        app.texturePreview.windowOpen = true;
                                        App::RequestTexturePreview(app, selected->path, entry);
                                        texturePreview = App::SnapshotTexturePreview(app);
                                    }
                                    if (ImGui::IsItemHovered())
                                    {
                                        ImGui::SetTooltip("Select to preview %s", entry.name.c_str());
                                    }
                                }
                                else if (isGrp && (AssetIndex::CanExportGrpEntryAsStaticModel(entry) || AssetIndex::CanExportGrpEntryAsSkinnedModel(entry)))
                                {
                                    const bool isSelectedModel =
                                        app.modelPreview.packPath == selected->path &&
                                        app.modelPreview.entryName == entry.name;
                                    if (DrawInlineIconSelectable(GetSubAssetIconId(entry, selected->type), entry.name.c_str(), isSelectedModel))
                                    {
                                        App::RequestModelPreview(app, selected->path, entry);
                                    }
                                    if (ImGui::IsItemHovered())
                                    {
                                        ImGui::SetTooltip("Select to preview 3D model %s", entry.name.c_str());
                                    }
                                }
                                else
                                {
                                    DrawInlineIconLabel(GetSubAssetIconId(entry, selected->type), entry.name.c_str());
                                }

                                ImGui::TableSetColumnIndex(1);
                                ImGui::Text("0x%X", entry.offset);

                                ImGui::TableSetColumnIndex(2);
                                ImGui::TextUnformatted(FormatBytes(entry.size).c_str());

                                if (isDxp || isGrp)
                                {
                                    ImGui::TableSetColumnIndex(3);
                                    if (!entry.formatInfo.empty())
                                    {
                                        ImGui::TextUnformatted(entry.formatInfo.c_str());
                                    }
                                    else
                                    {
                                        ImGui::TextDisabled("N/A");
                                    }
                                }
                                
                                if (hasActions)
                                {
                                    ImGui::TableSetColumnIndex((isDxp || isGrp) ? 4 : 3);
                                    
                                    const bool exportSupported = AssetIndex::IsExportSupported(entry, selected->type);
                                    if (exportSupported)
                                    {
                                        std::string btnLabel = "Export##" + entry.name;
                                        const bool canExportSkinned = AssetIndex::CanExportGrpEntryAsSkinnedModel(entry);
                                        
                                        if (ImGui::Button(btnLabel.c_str()))
                                        {
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
                                    }
                                    else
                                    {
                                        ImGui::AlignTextToFramePadding();
                                        ImGui::TextUnformatted("Source");
                                        if (ImGui::IsItemHovered())
                                        {
                                            if (isGrp)
                                            {
                                                std::string tooltip = AssetIndex::GetUnsupportedExportTooltip(entry);
                                                ImGui::SetTooltip("%s", tooltip.c_str());
                                            }
                                            else if (isDxp)
                                            {
                                                ImGui::SetTooltip("Direct export from the list is not supported for textures. Use 'Batch Export All' above.");
                                            }
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
        ImGui::BulletText("Discovery and metadata indexing.");
        if (isContainer)
        {
            ImGui::BulletText("Sub-asset listing (lazy loaded on demand).");
        }
        if (selected->type == AssetType::Blk)
        {
            ImGui::BulletText("Text BLK preview for resource relationship metadata.");
        }
        if (selected->type == AssetType::Dxp)
        {
            ImGui::BulletText("DDS texture extraction with reversed-mip reordering, zlib/zstd/Oodle/LZMA decompression, and readable PNG/TGA export including DirectXTex fallback for BC6H/BC7-class textures.");
        }
        if (selected->type == AssetType::Grp)
        {
            ImGui::BulletText("Binary static and dynamic model extraction to glTF 2.0 GLB / Wavefront OBJ.");
            ImGui::BulletText("GeomNodeTree skeleton metadata parsing for GRP inspection and skinned mesh exports.");
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
        ImGui::Text("Scan: %s", App::GetScanStatus(app.scan).c_str());
        ImGui::SameLine();
        ImGui::Text("Visited: %llu", static_cast<unsigned long long>(app.scan.filesVisited.load()));
        ImGui::SameLine();
        ImGui::Text("Found: %llu", static_cast<unsigned long long>(app.scan.recordsFound.load()));
        ImGui::Text("Cache: %s", App::GetCacheStatus(app.scan).c_str());
        ImGui::SameLine();
        ImGui::Text("BLK Links: %s", App::GetBlkResourceLinkStatus(app).c_str());
        ImGui::SameLine();
        ImGui::Text("Desc Links: %s", App::GetDescriptorResourceLinkStatus(app).c_str());

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
            ImGui::TextDisabled("%s", snapshot.currentItem.c_str());
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
            ImGui::TextUnformatted(status.c_str());

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
            ImGui::TextColored(ImVec4(0.72f, 0.84f, 1.0f, 1.0f), "%s", title.empty() ? "Exporting Assets" : title.c_str());
            ImGui::Spacing();

            if (isRunning)
            {
                ImGui::TextWrapped("%s", status.empty() ? "Exporting..." : status.c_str());
                if (!currentItem.empty())
                {
                    ImGui::TextDisabled("Current: %s", currentItem.c_str());
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

        if (!ImGui::Begin("Settings", &app.settingsWindowOpen))
        {
            ImGui::End();
            return;
        }

        ImGui::TextUnformatted("Game");
        ImGui::InputText("Game Root", app.gameRootBuffer.data(), app.gameRootBuffer.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse##GameRoot"))
        {
            if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose War Thunder Folder", app.settings.gameRoot))
            {
                App::SetGameRoot(app, *selected);
            }
        }

        if (ImGui::Button("Validate Game Folder"))
        {
            App::ApplyGameRootBuffer(app);
        }

        ImGui::Spacing();
        DrawValidationSummary(app.validation);

        ImGui::Separator();
        ImGui::TextUnformatted("Export Defaults");
        ImGui::InputText("Export Root", app.exportRootBuffer.data(), app.exportRootBuffer.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse##ExportRoot"))
        {
            if (const auto selected = Platform::BrowseForFolder(static_cast<HWND>(app.hwnd), L"Choose Export Folder", app.settings.exportRoot))
            {
                app.settings.exportRoot = *selected;
                App::SyncSettingsBuffers(app);
            }
        }

        // Force texture format to PNG, since GLB is the only supported format and requires PNG textures.
        app.settings.textureExportFormat = "PNG";

        static const char* textureFormats[] = { "DDS", "PNG", "TGA" };
        int textureFormatIndex = 1; // "PNG"

        ImGui::BeginDisabled(true);
        if (ImGui::Combo("Texture Format", &textureFormatIndex, textureFormats, IM_ARRAYSIZE(textureFormats)))
        {
            app.settings.textureExportFormat = textureFormats[textureFormatIndex];
        }
        ImGui::EndDisabled();

        int maxLods = app.settings.staticModelMaxLods;
        if (ImGui::InputInt("Max RendInst LODs", &maxLods))
        {
            maxLods = std::clamp(maxLods, 1, 16);
            app.settings.staticModelMaxLods = maxLods;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Sets the maximum number of Levels of Detail (LODs) to export for static models (RendInst).");
        }

        ImGui::Checkbox("Export textures with models", &app.settings.exportTexturesWithModels);
        ImGui::Checkbox("Embed textures inside GLB", &app.settings.embedTexturesInGlb);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Stores PNG/JPEG material textures as GLB bufferViews instead of external image URIs.");
        }
        ImGui::Checkbox("Export JSON manifest metadata alongside assets", &app.settings.exportJsonManifest);
        ImGui::Checkbox("Fallback to DDS when PNG/TGA conversion is unavailable", &app.settings.preserveOriginalDDS);
        ImGui::TextDisabled("Oodle-compressed DDSx textures use the bundled oo2core_6_win64.dll shipped with the app build.");
        ImGui::TextDisabled("PNG/TGA exports the top mip directly for BC1-BC5 plus common uncompressed/float DDSx textures, then retries unsupported DXGI formats through DirectXTex.");
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
        int maxLoadedPackContents = static_cast<int>(app.settings.maxLoadedPackContents);
        if (ImGui::InputInt("Max loaded containers", &maxLoadedPackContents))
        {
            maxLoadedPackContents = std::clamp(maxLoadedPackContents, 1, 256);
            app.settings.maxLoadedPackContents = static_cast<size_t>(maxLoadedPackContents);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("When loaded pack contents exceed this number, the cache unloads old parsed containers and keeps the current inspector target.");
        }

        ImGui::Separator();
        if (ImGui::Button("Save Settings"))
        {
            app.settings.exportRoot = Platform::Utf8ToPath(app.exportRootBuffer.data());
            App::SetGameRoot(app, Platform::Utf8ToPath(app.gameRootBuffer.data()));
            SaveSettingsWithImGui(app);
        }
        ImGui::SameLine();
        ImGui::Text("Settings file: %s", Platform::PathToUtf8(app.settingsPath).c_str());
        ImGui::Text("Index cache: %s", Platform::PathToUtf8(app.indexCachePath).c_str());
        ImGui::Text("Descriptor-link cache: %s", Platform::PathToUtf8(app.descriptorLinksCachePath).c_str());

        ImGui::End();
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
        ImGui::BulletText("Scans the local game install and builds a searchable content browser.");
        ImGui::BulletText("Inspects GRP, DXP, VROMFS, descriptor, and BLK data without modifying game files.");
        ImGui::BulletText("Exports readable textures and models for modding workflows.");
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

        ImGui::Text("Model: %s", pending.entry.name.c_str());
        ImGui::Spacing();

        ImGui::Checkbox("Include textures", &pending.includeTextures);

        ImGui::Checkbox("Include skeleton / rig", &pending.includeSkeletonRig);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Exports GeomNodeTree bones and binds skinned meshes when a matching skeleton is found.");
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
            ImGui::SetTooltip("Exports matched Anim2Data ACL tracks as glTF animation channels. Requires skeleton / rig export so curves can target bones.");
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

    void DrawTexturePreviewCanvas(AppState& app, const UiTexture* previewTexture)
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
            const char* message = "Select a texture from a DXP pack to preview it.";
            const ImVec2 textSize = ImGui::CalcTextSize(message);
            ImGui::SetCursorPos(ImVec2(
                ImGui::GetCursorPosX() + std::max(0.0f, (available.x - textSize.x) * 0.5f),
                ImGui::GetCursorPosY() + std::max(0.0f, (available.y - textSize.y) * 0.5f)));
            ImGui::TextDisabled("%s", message);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            return;
        }

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
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

        ImGui::SetNextWindowSize(ImVec2(1080.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f, 480.0f), ImVec2(FLT_MAX, FLT_MAX));
        std::string title = "Texture Viewer - " + preview.entryName + "###TexturePreviewWindow";
        bool open = app.texturePreview.windowOpen;

        if (ImGui::Begin(title.c_str(), &open))
        {
            DrawPanelTitle(UiIconId::Texture, "Texture Viewer");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Platform::PathToUtf8(preview.packPath.filename()).c_str());
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
                    ImGui::TextDisabled(
                        "Mouse wheel zooms. Scrollbars navigate images larger than the viewport.");
                }

                DrawTexturePreviewCanvas(app, GetTexturePreview());
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
                            ImGui::BulletText("%s", warning.c_str());
                        }
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::BeginDisabled(!currentEntry.has_value());
                if (ImGui::Button("Export Texture", ImVec2(-1.0f, 0.0f)) &&
                    currentEntry)
                {
                    App::RequestTextureExport(app, preview.packPath, *currentEntry);
                }
                if (ImGui::Button("Reload Preview", ImVec2(-1.0f, 0.0f)) &&
                    currentEntry)
                {
                    ClearTexturePreview();
                    app.texturePreview.fitToWindow = true;
                    app.texturePreview.zoom = 1.0f;
                    App::RequestTexturePreview(app, preview.packPath, *currentEntry);
                }
                ImGui::EndDisabled();
                if (ImGui::Button("Show DXP in Explorer", ImVec2(-1.0f, 0.0f)))
                {
                    Platform::ShowPathInExplorer(preview.packPath);
                }
                ImGui::EndChild();

                ImGui::EndTable();
            }
        }
        ImGui::End();

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

                    // Auto-fit camera zoom
                    float dx = app.modelPreview.bboxMax[0] - app.modelPreview.bboxMin[0];
                    float dy = app.modelPreview.bboxMax[1] - app.modelPreview.bboxMin[1];
                    float dz = app.modelPreview.bboxMax[2] - app.modelPreview.bboxMin[2];
                    float bboxSize = std::sqrt(dx * dx + dy * dy + dz * dz);
                    app.modelPreview.zoom = bboxSize > 0.0f ? bboxSize * 1.5f : 5.0f;
                    app.modelPreview.panX = 0.0f;
                    app.modelPreview.panY = 0.0f;
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
        std::string windowTitle = "Model Preview - " + app.modelPreview.entryName + "###ModelPreviewWindow";

        bool isOpen = true;
        if (ImGui::Begin(windowTitle.c_str(), &isOpen))
        {
            if (app.modelPreview.loading)
            {
                ImGui::Text("Loading model geometry from package...");
            }
            else if (!app.modelPreview.error.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", app.modelPreview.error.c_str());
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
                        auto start = std::chrono::high_resolution_clock::now();

                        srv = renderer->Render(
                            app.modelPreview.yaw,
                            app.modelPreview.pitch,
                            app.modelPreview.zoom,
                            app.modelPreview.panX,
                            app.modelPreview.panY,
                            app.modelPreview.wireframe,
                            app.modelPreview.bboxMin,
                            app.modelPreview.bboxMax,
                            static_cast<uint32_t>(viewportSize.x),
                            static_cast<uint32_t>(viewportSize.y),
                            app.modelPreview.lighting
                        );

                        auto end = std::chrono::high_resolution_clock::now();
                        std::chrono::duration<float, std::milli> duration = end - start;
                        float latency = duration.count();
                        app.modelPreview.renderLatencyMs = latency;

                        ImGuiIO& io = ImGui::GetIO();
                        app.modelPreview.frameTimes[app.modelPreview.statOffset] = io.Framerate;
                        app.modelPreview.latencyTimes[app.modelPreview.statOffset] = latency;
                        app.modelPreview.statOffset = (app.modelPreview.statOffset + 1) % 90;
                    }

                    if (srv)
                    {
                        ImGui::Image(reinterpret_cast<ImTextureID>(srv), viewportSize);

                        // Draw transparent performance overlay on top-left of the viewport image
                        ImVec2 oldCursorPos = ImGui::GetCursorPos();
                        ImVec2 localPos = ImVec2(ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x + ImGui::GetScrollX(),
                                                 ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y + ImGui::GetScrollY());

                        ImGui::SetCursorPos(ImVec2(localPos.x + 8.0f, localPos.y + 8.0f));
                        ImGui::BeginGroup();
                        {
                            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.11f, 0.75f));
                            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.27f, 0.30f, 0.60f));
                            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
                            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 5.0f));

                            if (ImGui::BeginChild("PerfOverlay", ImVec2(170.0f, 112.0f), true, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar))
                            {
                                ImGuiIO& io = ImGui::GetIO();
                                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "FPS: %.1f", io.Framerate);
                                ImGui::SameLine();
                                ImGui::TextDisabled("(%.1fms)", io.DeltaTime * 1000.0f);

                                // Micro FPS graph
                                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.13f, 0.15f, 0.4f));
                                float maxFps = 0.0f;
                                for (float f : app.modelPreview.frameTimes) if (f > maxFps) maxFps = f;
                                ImGui::PlotLines("##MicroFPS", app.modelPreview.frameTimes, 90, app.modelPreview.statOffset, nullptr, 0.0f, maxFps * 1.1f, ImVec2(-1.0f, 22.0f));

                                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Render: %.2f ms", app.modelPreview.renderLatencyMs);

                                // Micro Latency graph
                                float maxLatency = 0.0f;
                                for (float l : app.modelPreview.latencyTimes) if (l > maxLatency) maxLatency = l;
                                ImGui::PlotLines("##MicroLatency", app.modelPreview.latencyTimes, 90, app.modelPreview.statOffset, nullptr, 0.0f, maxLatency * 1.1f, ImVec2(-1.0f, 22.0f));

                                ImGui::PopStyleColor(); // FrameBg
                                ImGui::EndChild();
                            }
                            ImGui::PopStyleVar(2);
                            ImGui::PopStyleColor(2);
                        }
                        ImGui::EndGroup();
                        ImGui::SetCursorPos(oldCursorPos);

                        // Handle mouse inputs over the image
                        const bool isHovered = ImGui::IsItemHovered();
                        ImGuiIO& io = ImGui::GetIO();

                        if (isHovered && io.MouseWheel != 0.0f)
                        {
                            float zoomSpeed = std::max(0.1f, app.modelPreview.zoom * 0.15f);
                            app.modelPreview.zoom -= io.MouseWheel * zoomSpeed;
                            app.modelPreview.zoom = std::max(0.01f, app.modelPreview.zoom);
                        }

                        static bool draggingLeft = false;
                        static bool draggingRight = false;
                        static bool draggingMiddle = false;

                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && isHovered) draggingLeft = true;
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && isHovered) draggingRight = true;
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && isHovered) draggingMiddle = true;

                        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) draggingLeft = false;
                        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) draggingRight = false;
                        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) draggingMiddle = false;

                        if (draggingLeft || draggingRight)
                        {
                            if (io.KeyShift)
                            {
                                float panSpeed = std::max(0.001f, app.modelPreview.zoom * 0.002f);
                                app.modelPreview.panX -= io.MouseDelta.x * panSpeed;
                                app.modelPreview.panY += io.MouseDelta.y * panSpeed;
                            }
                            else
                            {
                                app.modelPreview.yaw -= io.MouseDelta.x * 0.5f;
                                app.modelPreview.pitch += io.MouseDelta.y * 0.5f;
                                app.modelPreview.pitch = std::clamp(app.modelPreview.pitch, -89.0f, 89.0f);
                            }
                        }
                        else if (draggingMiddle)
                        {
                            float panSpeed = std::max(0.001f, app.modelPreview.zoom * 0.002f);
                            app.modelPreview.panX -= io.MouseDelta.x * panSpeed;
                            app.modelPreview.panY += io.MouseDelta.y * panSpeed;
                        }
                    }
                    else
                    {
                        ImGui::Text("D3D11 Viewport rendering unavailable");
                    }

                    // Viewport controls toolbar
                    ImGui::Separator();
                    ImGui::Checkbox("Wireframe", &app.modelPreview.wireframe);
                    ImGui::SameLine();
                    if (ImGui::Button("Reset Camera"))
                    {
                        app.modelPreview.yaw = 45.0f;
                        app.modelPreview.pitch = 30.0f;
                        app.modelPreview.panX = 0.0f;
                        app.modelPreview.panY = 0.0f;
                        float dx = app.modelPreview.bboxMax[0] - app.modelPreview.bboxMin[0];
                        float dy = app.modelPreview.bboxMax[1] - app.modelPreview.bboxMin[1];
                        float dz = app.modelPreview.bboxMax[2] - app.modelPreview.bboxMin[2];
                        float bboxSize = std::sqrt(dx * dx + dy * dy + dz * dz);
                        app.modelPreview.zoom = bboxSize > 0.0f ? bboxSize * 1.5f : 5.0f;
                    }
                }
                ImGui::EndChild();

                ImGui::SameLine();

                // Detail Panel Child Window
                ImGui::BeginChild("DetailPanelChild", ImVec2(0.0f, 0.0f), true);
                {
                    ImGui::TextDisabled("MODEL DETAILS");
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Name: %s", app.modelPreview.entryName.c_str());
                    ImGui::Text("Class: %s", AssetIndex::DescribeGrpClassId(app.modelPreview.grpClassId).c_str());
                    ImGui::Text("File Size: %s", FormatBytes(app.modelPreview.fileSize).c_str());
                    ImGui::Text("Submeshes: %u", app.modelPreview.subMeshCount);

                    ImGui::Spacing();
                    ImGui::TextDisabled("GEOMETRY STATS");
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Vertices: %u", app.modelPreview.vertexCount);
                    ImGui::Text("Indices:  %u", app.modelPreview.indexCount);
                    ImGui::Text("Triangles: %u", app.modelPreview.indexCount / 3);

                    ImGui::Spacing();
                    ImGui::TextDisabled("BOUNDING BOX");
                    ImGui::Separator();
                    ImGui::Spacing();

                    float sizeX = app.modelPreview.bboxMax[0] - app.modelPreview.bboxMin[0];
                    float sizeY = app.modelPreview.bboxMax[1] - app.modelPreview.bboxMin[1];
                    float sizeZ = app.modelPreview.bboxMax[2] - app.modelPreview.bboxMin[2];

                    ImGui::Text("Min: (%.3f, %.3f, %.3f)", app.modelPreview.bboxMin[0], app.modelPreview.bboxMin[1], app.modelPreview.bboxMin[2]);
                    ImGui::Text("Max: (%.3f, %.3f, %.3f)", app.modelPreview.bboxMax[0], app.modelPreview.bboxMax[1], app.modelPreview.bboxMax[2]);
                    ImGui::Text("Size: %.3f x %.3f x %.3f", sizeX, sizeY, sizeZ);

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
        }
        ImGui::End();

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
        DrawHeader(app);

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
        DrawExportProgressModal(app);
        DrawTexturePreviewWindow(app);
        DrawModelPreviewPopup(app);
    }
}
