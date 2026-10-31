#include "pch.h"
#include "Win32Provider.h"
#include "Core/ConfigManager.h"
#include "Utils/PinyinMatcher.h"
#include <Shlobj.h>
#include <filesystem>
#include <shobjidl.h>
#include <unordered_set>

namespace
{
// 零分配提取文件名（含扩展名）
inline std::wstring_view ExtractFilenameView(std::wstring_view path) noexcept
{
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring_view::npos) ? path : path.substr(pos + 1);
}

// 零分配提取纯文件名（不含扩展名）
inline std::wstring_view ExtractStemView(std::wstring_view path) noexcept
{
    std::wstring_view filename = ExtractFilenameView(path);
    size_t dot = filename.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0)
        return filename;
    return filename.substr(0, dot);
}

// 零分配提取扩展名（包含 '.'）
inline std::wstring_view ExtractExtensionView(std::wstring_view path) noexcept
{
    std::wstring_view filename = ExtractFilenameView(path);
    size_t dot = filename.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0)
        return {};
    return filename.substr(dot);
}

// 检查文件名是否命中卸载/删除黑名单（使用 string_view 零拷贝、大小写不敏感）
bool IsBlacklistedName(std::wstring_view stemName) noexcept
{
    static constexpr std::wstring_view nameBlacklist[] = {L"uninstall", L"卸载", L"删除", L"uninst", L"LiteLauncher"};
    for (const auto &kw : nameBlacklist)
    {
        if (FindSubstringCaseInsensitive(stemName, kw) >= 0)
            return true;
    }
    return false;
}

// 检查目标路径是否命中卸载程序或文档/网页黑名单（零堆内存分配）
bool IsBlacklistedTarget(std::wstring_view targetPath) noexcept
{
    if (targetPath.empty())
        return false;

    std::wstring_view filename = ExtractFilenameView(targetPath);
    std::wstring_view ext = ExtractExtensionView(filename);

    if (EqualsCaseInsensitive(filename, L"unins000.exe") || EqualsCaseInsensitive(filename, L"msiexec.exe") ||
        EqualsCaseInsensitive(filename, L"LiteLauncher.exe") ||
        FindSubstringCaseInsensitive(filename, L"unins") == 0 ||
        FindSubstringCaseInsensitive(filename, L"uninstall") >= 0)
    {
        return true;
    }

    if (!ext.empty())
    {
        static constexpr std::wstring_view blockedExts[] = {L".chm", L".hlp", L".pdf", L".txt",
                                                            L".url", L".html", L".htm"};
        for (const auto &bExt : blockedExts)
        {
            if (EqualsCaseInsensitive(ext, bExt))
                return true;
        }
    }

    return false;
}

// 判断是否为无意义的通用启动器名或系统/运行时宿主程序（用于自定义目录命名及 .lnk 英文关键词去噪）
bool IsGenericOrHostExeName(std::wstring_view stemView) noexcept
{
    static constexpr std::wstring_view genericAndHostNames[] = {
        // 通用启动器/配置器名称
        L"app", L"launcher", L"config", L"start", L"main", L"run", L"console", L"client",
        L"server", L"update", L"updater", L"helper", L"setup", L"install", L"installer",
        L"daemon", L"service", L"gui", L"ui", L"loader", L"bootstrap",
        // 系统宿主与跨平台运行时外壳（防止挂接在它们名下的快捷方式被污染）
        L"cmd", L"powershell", L"pwsh", L"conhost", L"rundll32", L"regsvr32", L"explorer",
        L"wscript", L"cscript", L"mshta", L"javaw", L"java", L"python", L"pythonw",
        L"node", L"electron", L"nw", L"chrome_proxy", L"msedge_proxy"
    };

    for (const auto &g : genericAndHostNames)
    {
        if (EqualsCaseInsensitive(stemView, g))
            return true;
    }
    return false;
}

// 为自定义目录下的通用名 .exe（如 bin\launcher.exe、config.exe）附加父目录名称，防止重名混淆或被吞
std::wstring FormatCustomExeDisplayName(std::wstring_view fullPath, std::wstring_view stemView)
{
    if (!IsGenericOrHostExeName(stemView))
    {
        return std::wstring(stemView);
    }

    // 向上回溯提取有意义的父文件夹名（若直接父目录是 bin/x64 等架构目录，则再往上取一层）
    size_t lastSlash = fullPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring_view::npos && lastSlash > 0)
    {
        std::wstring_view parentPath = fullPath.substr(0, lastSlash);
        std::wstring_view parentName = ExtractFilenameView(parentPath);

        if (EqualsCaseInsensitive(parentName, L"bin") || EqualsCaseInsensitive(parentName, L"x64") ||
            EqualsCaseInsensitive(parentName, L"x86") || EqualsCaseInsensitive(parentName, L"win64") ||
            EqualsCaseInsensitive(parentName, L"win32"))
        {
            size_t prevSlash = parentPath.find_last_of(L"\\/");
            if (prevSlash != std::wstring_view::npos && prevSlash > 0)
            {
                parentName = ExtractFilenameView(parentPath.substr(0, prevSlash));
            }
        }

        if (!parentName.empty() && parentName.back() != L':')
        {
            std::wstring combined;
            combined.reserve(parentName.size() + 3 + stemView.size());
            combined.append(parentName).append(L" (").append(stemView).push_back(L')');
            return combined;
        }
    }

    return std::wstring(stemView);
}

// 唯一目录硬件标识（VolumeSerialNumber + 64位 FileIndex），用于彻底阻断符号链接/Junction 环路与根目录爆炸
struct DirectoryIdentity
{
    DWORD VolumeSerialNumber;
    uint64_t FileIndex;

    bool operator==(const DirectoryIdentity &other) const noexcept
    {
        return VolumeSerialNumber == other.VolumeSerialNumber && FileIndex == other.FileIndex;
    }
};

struct DirectoryIdentityHasher
{
    size_t operator()(const DirectoryIdentity &id) const noexcept
    {
        uint64_t h = id.FileIndex ^ (static_cast<uint64_t>(id.VolumeSerialNumber) * 1099511628211ULL);
        return static_cast<size_t>(h);
    }
};

using VisitedDirSet = std::unordered_set<DirectoryIdentity, DirectoryIdentityHasher>;

// 解析目录真实句柄（自动追踪 Junction/Symlink 最终目标）并登记到已访问集合中
bool TryMarkDirectoryVisited(const wchar_t *dirPath, VisitedDirSet &visitedDirs) noexcept
{
    HANDLE hDir = CreateFileW(dirPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hDir == INVALID_HANDLE_VALUE)
        return false;

    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(hDir, &info) != FALSE;
    CloseHandle(hDir);

    if (!ok)
        return false;

    uint64_t fileIndex = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    return visitedDirs.insert({info.dwVolumeSerialNumber, fileIndex}).second;
}

// 安全且零冗余 I/O 的目录遍历器：
// 1. 普通文件夹直接利用 directory_entry 缓存属性放行，0 次额外 CreateFileW 系统调用；
// 2. 仅当遇到 ReparsePoint 目录（Symlink/Junction，如 Scoop 的 current 目录）或处于其子树中时，才启用 FileIndex 环路拦截。
template <typename Callback>
void TraverseDirectorySafe(const std::filesystem::path &rootDir, int currentDepth, int maxDepth,
                           VisitedDirSet &visitedDirs, bool inReparseTree, Callback &&onFile)
{
    if (currentDepth > maxDepth)
        return;

    std::error_code ec;
    auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator it(rootDir, options, ec);
    std::filesystem::directory_iterator end;
    if (ec)
        return;

    while (it != end)
    {
        if (SearchManager::IsShuttingDown())
            break;

        std::error_code entry_ec;
        const auto &entry = *it;

        // 直接复用 directory_entry 内部缓存的 WIN32_FIND_DATA 状态，零系统调用
        if (entry.is_directory(entry_ec) && !entry_ec)
        {
            if (currentDepth < maxDepth)
            {
                std::error_code sym_ec;
                // 在 MSVC 标准库中，entry.is_symlink() 直接检查缓存的 FILE_ATTRIBUTE_REPARSE_POINT 位，无内核调用开销
                bool isReparseDir = entry.is_symlink(sym_ec) && !sym_ec;
                bool needIdentityCheck = inReparseTree || isReparseDir;

                const auto &subDirPath = entry.path();
                bool canEnter = true;

                if (needIdentityCheck)
                {
                    canEnter = TryMarkDirectoryVisited(subDirPath.c_str(), visitedDirs);
                }

                if (canEnter)
                {
                    TraverseDirectorySafe(subDirPath, currentDepth + 1, maxDepth, visitedDirs,
                                          needIdentityCheck, onFile);
                }
            }
        }
        else if (!entry_ec)
        {
            // 允许普通文件及重解析点文件（如 OneDrive 快捷方式、wt.exe 应用执行别名）通过
            onFile(entry.path());
        }

        std::error_code inc_ec;
        it.increment(inc_ec);
        if (inc_ec)
            break;
    }
}
} // namespace

// 解析快捷方式真实路径（供右键“打开文件位置”使用）
std::wstring Win32Provider::ResolveLnkTarget(const std::wstring &lnkPath)
{
    winrt::com_ptr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))))
    {
        return L"";
    }
    auto persistFile = shellLink.try_as<IPersistFile>();
    if (!persistFile)
        return L"";
    if (FAILED(persistFile->Load(lnkPath.c_str(), STGM_READ)))
        return L"";

    // 移除 shellLink->Resolve(...)，防止指向断开的网络驱动器或休眠硬盘时引发 UI/后台线程阻塞
    WCHAR targetPath[MAX_PATH] = {0};
    if (SUCCEEDED(shellLink->GetPath(targetPath, MAX_PATH, nullptr, SLGP_RAWPATH)) && targetPath[0] != L'\0')
    {
        return ExpandEnvStrings(targetPath);
    }
    return L"";
}

// 扫描所有安装软件与自定义目录
std::vector<SearchResultItem> Win32Provider::ScanStartMenuApps()
{
    std::vector<SearchResultItem> items;
    items.reserve(256);

    auto pathConfigs = ConfigManager::GetClassifiedSearchPaths();
    std::wstring selfExePath = GetSelfExePath();

    std::unordered_set<uint64_t> seenTargetArgsHash;
    seenTargetArgsHash.reserve(256);

    winrt::com_ptr<IShellLinkW> shellLink;
    winrt::com_ptr<IPersistFile> persistFile;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shellLink.put()))))
    {
        persistFile = shellLink.try_as<IPersistFile>();
    }

    for (const auto &cfg : pathConfigs)
    {
        if (cfg.Path.empty())
            continue;

        VisitedDirSet visitedDirs;
        // 仅登记扫描起始根目录自身的 FileIndex（1 次轻量调用），防止子级软链接指回扫描起点形成环路
        TryMarkDirectoryVisited(cfg.Path.c_str(), visitedDirs);

        TraverseDirectorySafe(cfg.Path, 1, cfg.MaxDepth, visitedDirs, false,
                              [&](const std::filesystem::path &filePath) {
            const std::wstring &nativePath = filePath.native();
            std::wstring_view pathView(nativePath);

            // 规则1：系统默认开始菜单/桌面目录只扫描 .lnk
            if (cfg.IsDefaultLnkOnly)
            {
                if (!EndsWithCaseInsensitive(pathView, L".lnk"))
                    return;

                std::wstring_view stemView = ExtractStemView(pathView);
                if (stemView.empty() || IsBlacklistedName(stemView))
                    return;

                std::wstring targetPath;
                WCHAR szArgs[INFOTIPSIZE] = {0};
                std::wstring_view argsView;

                if (shellLink && persistFile && SUCCEEDED(persistFile->Load(nativePath.c_str(), STGM_READ)))
                {
                    WCHAR szTarget[MAX_PATH] = {0};
                    if (SUCCEEDED(shellLink->GetPath(szTarget, MAX_PATH, nullptr, SLGP_RAWPATH)) &&
                        szTarget[0] != L'\0')
                    {
                        targetPath = ExpandEnvStrings(szTarget);
                    }

                    if (SUCCEEDED(shellLink->GetArguments(szArgs, INFOTIPSIZE)) && szArgs[0] != L'\0')
                    {
                        argsView = szArgs;
                    }
                }

                if (!targetPath.empty())
                {
                    if (EqualsCaseInsensitive(targetPath, selfExePath))
                        return;
                    if (IsBlacklistedTarget(targetPath))
                        return;
                }

                if (!argsView.empty() &&
                    (FindSubstringCaseInsensitive(argsView, L"/x") >= 0 ||
                     FindSubstringCaseInsensitive(argsView, L"uninstall") >= 0))
                {
                    if (FindSubstringCaseInsensitive(targetPath, L"msiexec") >= 0)
                        return;
                }

                // 零分配计算 64 位大小写不敏感联合 Hash Key
                uint64_t hashKey = 0;
                if (targetPath.empty())
                {
                    hashKey = HashStringCaseInsensitiveFNV1a(pathView, HashStringFNV1a(L"lnk_fallback|"));
                }
                else
                {
                    hashKey = HashStringCaseInsensitiveFNV1a(
                        argsView, HashStringFNV1a(L"|", HashStringCaseInsensitiveFNV1a(targetPath)));
                }

                if (!seenTargetArgsHash.insert(hashKey).second)
                {
                    return;
                }

                SearchResultItem item;
                item.Name.assign(stemView);
                item.Pinyin = PinyinMatcher::GetCamelPinyin(item.Name);
                item.Type = ItemType::Win32;
                item.ActionPath = nativePath;

                // 提取目标程序的英文文件名（如 "微信.lnk" -> "WeChat.exe" -> "WeChat"）
                if (!targetPath.empty())
                {
                    std::wstring_view targetStem = ExtractStemView(targetPath);
                    if (!targetStem.empty() &&
                        !EqualsCaseInsensitive(targetStem, stemView) &&
                        !IsGenericOrHostExeName(targetStem))
                    {
                        item.ExtraKeywords.assign(targetStem);
                    }
                }

                item.TargetExePath = std::move(targetPath);
                items.push_back(std::move(item));
            }
            // 规则2：用户自定义目录只扫描 .exe
            else
            {
                if (!EndsWithCaseInsensitive(pathView, L".exe"))
                    return;

                std::wstring_view stemView = ExtractStemView(pathView);
                if (stemView.empty())
                    return;

                if (EqualsCaseInsensitive(pathView, selfExePath))
                    return;
                if (IsBlacklistedName(stemView) || IsBlacklistedTarget(pathView))
                    return;

                uint64_t hashKey = HashStringFNV1a(L"|", HashStringCaseInsensitiveFNV1a(pathView));
                if (!seenTargetArgsHash.insert(hashKey).second)
                {
                    return;
                }

                SearchResultItem item;
                item.Name = FormatCustomExeDisplayName(pathView, stemView);
                item.Pinyin = PinyinMatcher::GetCamelPinyin(item.Name);
                item.Type = ItemType::Win32;
                // 普通 .exe 的 TargetExePath 保持为空，消费端自动回退使用 ActionPath，避免双倍堆内存占用
                item.ActionPath = nativePath;

                items.push_back(std::move(item));
            }
        });
    }

    return items;
}
