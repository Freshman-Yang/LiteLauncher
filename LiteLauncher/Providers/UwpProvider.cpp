#include "pch.h"
#include "UwpProvider.h"
#include "Utils/PinyinMatcher.h"

using namespace winrt;
using namespace Windows::Management::Deployment;

namespace
{
// 大小写不敏感的前缀匹配（零堆分配）
inline bool StartsWithCaseInsensitive(std::wstring_view str, std::wstring_view prefix) noexcept
{
    if (str.size() < prefix.size())
        return false;
    return FindStringOrdinal(FIND_FROMSTART, str.data(), static_cast<int>(prefix.size()), prefix.data(),
                             static_cast<int>(prefix.size()), TRUE) == 0;
}

struct UwpBuiltinAliasEntry
{
    std::wstring_view PackagePrefix;
    const wchar_t *Aliases;
};

// Windows 常见内置 UWP 应用静态英文名与命令别名表（直接挂载 const wchar_t* 指针，零堆内存分配）
constexpr UwpBuiltinAliasEntry kBuiltinUwpAliases[] = {
    {L"Microsoft.Paint", L"mspaint paint pbrush"},
    {L"Microsoft.WindowsCalculator", L"calc calculator"},
    {L"Microsoft.ScreenSketch", L"snip snippingtool screenshot"},
    {L"Microsoft.WindowsTerminal", L"wt terminal cmd powershell console"},
    {L"Microsoft.WindowsNotepad", L"notepad"},
    {L"Microsoft.WindowsAlarms", L"clock alarm timer"},
    {L"windows.immersivecontrolpanel", L"settings control ms-settings"},
    {L"Microsoft.SecHealthUI", L"defender security windowsdefender"},
    {L"Microsoft.Windows.Photos", L"photos picture image"},
    {L"Microsoft.WindowsCamera", L"camera webcam"},
    {L"Microsoft.WindowsSoundRecorder", L"recorder soundrecorder voicerecorder"},
    {L"Microsoft.WindowsStore", L"store msstore wsreset"},
    {L"Microsoft.MicrosoftStickyNotes", L"stickynotes notes"},
    {L"Microsoft.Todos", L"todo task"},
    {L"Microsoft.BingWeather", L"weather"},
    {L"Microsoft.ZuneMusic", L"music mediaplayer"},
    {L"Microsoft.ZuneVideo", L"video movies"},
    {L"Microsoft.WindowsFeedbackHub", L"feedback"},
    {L"Microsoft.WindowsMaps", L"maps"},
    {L"Microsoft.GetHelp", L"gethelp help"},
    {L"Microsoft.PowerAutomateDesktop", L"powerautomate flow"},
    {L"Microsoft.OutlookForWindows", L"outlook mail"},
    {L"MicrosoftCorporationII.QuickAssist", L"quickassist"},
    {L"Microsoft.GamingApp", L"xbox gaming"},
    {L"Microsoft.XboxGamingOverlay", L"gamebar xbox"}
};

// 匹配内置 UWP 静态别名指针（零分配）
const wchar_t *MatchBuiltinUwpAliases(std::wstring_view pkgName) noexcept
{
    for (const auto &entry : kBuiltinUwpAliases)
    {
        if (StartsWithCaseInsensitive(pkgName, entry.PackagePrefix))
        {
            return entry.Aliases;
        }
    }
    return nullptr;
}

// 从包名（'_' 之前的部分）剥离 Microsoft. / Windows. 等通用厂商前缀，提取核心英文名
std::wstring_view ExtractCleanUwpPackageName(std::wstring_view pkgName) noexcept
{
    static constexpr std::wstring_view redundantPrefixes[] = {
        L"MicrosoftCorporationII.",
        L"Microsoft.",
        L"Windows."
    };

    // 循环剥离（例如处理 "Microsoft.Windows.Photos" -> 剥离 "Microsoft." 再剥离 "Windows." -> "Photos"）
    bool stripped = true;
    while (stripped)
    {
        stripped = false;
        for (const auto &prefix : redundantPrefixes)
        {
            if (StartsWithCaseInsensitive(pkgName, prefix) && pkgName.size() > prefix.size())
            {
                pkgName.remove_prefix(prefix.size());
                stripped = true;
                break;
            }
        }
    }
    return pkgName;
}
} // namespace

std::vector<SearchResultItem> UwpProvider::ScanUwpApps()
{
    std::vector<SearchResultItem> uwpApps;
    uwpApps.reserve(64);

    try
    {
        PackageManager pkgManager;
        auto packages = pkgManager.FindPackagesForUser(L"");
        for (auto const &package : packages)
        {
            if (SearchManager::IsShuttingDown())
                break;
            if (package.IsFramework() || package.IsResourcePackage() || package.IsStub())
                continue;
            try
            {
                auto appEntries = package.GetAppListEntriesAsync().get();
                for (auto const &entry : appEntries)
                {
                    if (!entry)
                        continue;

                    try
                    {
                        auto displayName = entry.DisplayInfo().DisplayName();
                        auto aumid = entry.AppUserModelId();
                        if (displayName.empty() || aumid.empty())
                            continue;

                        SearchResultItem item;
                        item.Name.assign(displayName.data(), displayName.size());
                        item.Pinyin = PinyinMatcher::GetCamelPinyin(item.Name);
                        item.Type = ItemType::UWP;
                        item.ActionPath.assign(aumid.data(), aumid.size()); // 仅保存字符串 AUMID

                        // 提取 AUMID 中 '_' 之前的包名部分（零 I/O）
                        std::wstring_view aumidView(item.ActionPath);
                        size_t underscorePos = aumidView.find(L'_');
                        std::wstring_view rawPkgName =
                            (underscorePos != std::wstring_view::npos) ? aumidView.substr(0, underscorePos) : aumidView;

                        if (!rawPkgName.empty())
                        {
                            // 1. 挂载静态内置应用别名表（如 mspaint, calc, wt 等，零堆分配）
                            item.Keywords = MatchBuiltinUwpAliases(rawPkgName);

                            // 2. 提取去前缀后的包名作为基础英文检索词（如 WindowsTerminal）
                            std::wstring_view cleanPkgName = ExtractCleanUwpPackageName(rawPkgName);
                            if (!cleanPkgName.empty() && !EqualsCaseInsensitive(cleanPkgName, item.Name))
                            {
                                item.ExtraKeywords.assign(cleanPkgName);
                            }
                        }

                        uwpApps.push_back(std::move(item));
                    }
                    catch (...)
                    {
                    }
                }
            }
            catch (...)
            {
            }
        }
    }
    catch (...)
    {
        OutputDebugStringW(L"[LiteLauncher] ScanUwpApps Failed!\n");
    }
    return uwpApps;
}
