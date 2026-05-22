#include "UI/UI.h"
#include "UI/UiAssets.h"
#include "App/AppState.h"
#include "Platform/Platform.h"
#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/PackContents.h"
#include "DagorFormats/BlkReader.h"

#include "imgui.h"

#include <algorithm>
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
                    App::SaveSettingsWithLog(app);
                }
            }

            if (ImGui::MenuItem("Save Settings"))
            {
                App::SaveSettingsWithLog(app);
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
            const bool canLoadCache = app.validation.IsValid() && app.settings.indexCacheEnabled && !app.scan.running.load();
            ImGui::BeginDisabled(!canLoadCache);
            if (ImGui::MenuItem("Load Cached Index"))
            {
                App::LoadIndexCacheIntoApp(app);
            }
            ImGui::EndDisabled();

            const bool canScan = app.validation.IsValid() && !app.scan.running.load();
            ImGui::BeginDisabled(!canScan);
            if (ImGui::MenuItem("Rebuild Resource Index"))
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

            ImGui::BeginDisabled(!app.scan.running.load());
            if (ImGui::MenuItem("Cancel Scan"))
            {
                app.scan.cancelRequested.store(true);
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
        const ImVec2 rebuildButtonSize = CalcIconButtonSize("Rebuild Index", rebuildHasIcon);
        const ImVec2 cancelButtonSize = CalcButtonSize("Cancel");
        const float buttonsWidth = rebuildButtonSize.x + ImGui::GetStyle().ItemSpacing.x + cancelButtonSize.x;
        ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionAvail().x - buttonsWidth));
        ImGui::BeginDisabled(!app.validation.IsValid() || app.scan.running.load());
        if (DrawIconTextButton("##rebuild_index", UiIconId::Rebuild, "Rebuild Index"))
        {
            App::StartScan(app);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!app.scan.running.load());
        if (ImGui::Button("Cancel", cancelButtonSize))
        {
            app.scan.cancelRequested.store(true);
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
                App::SaveSettingsWithLog(app);
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
            App::SaveSettingsWithLog(app);
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

                const float tableHeight = std::clamp(ImGui::GetContentRegionAvail().y * 0.58f, 260.0f, 460.0f);
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
                                DrawInlineIconLabel(GetSubAssetIconId(entry, selected->type), entry.name.c_str());

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
            ImGui::BulletText("DDS texture extraction with reversed-mip reordering, decompression, and readable PNG/TGA export for BC1-BC5, common uncompressed, and RGBA float textures.");
        }
        if (selected->type == AssetType::Grp)
        {
            ImGui::BulletText("Binary static and dynamic model extraction to glTF 2.0 GLB / Wavefront OBJ.");
            ImGui::BulletText("GeomNodeTree skeleton metadata parsing for GRP inspection and skinned mesh exports.");
        }
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

        std::vector<LogEntry> logs;
        {
            std::lock_guard<std::mutex> lock(app.logMutex);
            logs = app.logs;
        }

        if (ImGui::BeginTable("Logs", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f)))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 82.0f);
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const LogEntry& entry : logs)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(entry.time.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(LevelColor(entry.level), "%s", LevelName(entry.level));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", entry.message.c_str());
            }

            ImGui::EndTable();
        }
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
        ImGui::TextUnformatted("Progress Log");
        if (ImGui::BeginTable("ProgressJobLogs", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f)))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 82.0f);
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const LogEntry& entry : snapshot.logs)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(entry.time.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(LevelColor(entry.level), "%s", LevelName(entry.level));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", entry.message.c_str());
            }
            ImGui::EndTable();
        }

        {
            std::lock_guard<std::mutex> lock(app.progressJob.mutex);
            app.progressJob.windowOpen = open || snapshot.running;
        }
        ImGui::End();
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

        static const char* textureFormats[] = { "DDS", "PNG", "TGA" };
        const std::string textureFormat = AssetIndex::ToLowerAscii(app.settings.textureExportFormat);
        int textureFormatIndex = 0;
        if (textureFormat == "png")
        {
            textureFormatIndex = 1;
        }
        else if (textureFormat == "tga")
        {
            textureFormatIndex = 2;
        }

        if (ImGui::Combo("Texture Format", &textureFormatIndex, textureFormats, IM_ARRAYSIZE(textureFormats)))
        {
            app.settings.textureExportFormat = textureFormats[textureFormatIndex];
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
        ImGui::TextDisabled("PNG/TGA currently exports the top mip for BC1-BC5 plus R8, R8G8, RGBA8, BGRA8, RGBA16F, and RGBA32F DDSx textures.");
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
            App::SaveSettingsWithLog(app);
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
                    App::SaveSettingsWithLog(app);
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
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float bottomHeight = std::min(190.0f, available.y * 0.32f);
        const float topHeight = std::max(200.0f, available.y - bottomHeight - spacing);
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

        const float splitterHeight = 6.0f;
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

        ImGui::BeginChild("BottomPanel", ImVec2(0.0f, 0.0f), true);
        DrawJobAndLogPanel(app);
        ImGui::EndChild();

        ImGui::End();

        DrawSettingsWindow(app);
        DrawAboutWindow(app);
        DrawFirstRunModal(app);
        DrawDynModelExportModal(app);
        DrawProgressJobWindow(app);
    }
}
