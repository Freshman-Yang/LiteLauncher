#include "pch.h"
#include "ProcessHelper.h"
#include "Providers/Win32Provider.h"
#include <atomic>
#include <filesystem>
#include <shlobj.h>

namespace
{
struct AtomicFlagGuard
{
    std::atomic<bool> &flag;
    ~AtomicFlagGuard() noexcept
    {
        flag.store(false, std::memory_order_release);
    }
};

struct ActiveCountGuard
{
    std::atomic<int> &counter;
    ~ActiveCountGuard() noexcept
    {
        counter.fetch_sub(1, std::memory_order_acq_rel);
    }
};

bool TryAcquireActionSlot(std::atomic<ULONGLONG> &lastTick, std::atomic<int> &activeCount, ULONGLONG cooldownMs = 250,
                          int maxConcurrent = 4) noexcept
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = lastTick.load(std::memory_order_relaxed);
    do
    {
        if (now - last < cooldownMs)
            return false;
    } while (!lastTick.compare_exchange_weak(last, now, std::memory_order_acq_rel, std::memory_order_relaxed));

    if (activeCount.fetch_add(1, std::memory_order_acq_rel) >= maxConcurrent)
    {
        activeCount.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }

    return true;
}
} // namespace

// 1. Win32：以管理员身份运行（使用独立 STA 线程 + SEE_MASK_NOASYNC，防抖且不因 UAC 弹窗卡死后续操作）
void ProcessHelper::RunAsAdmin(const std::wstring &filePath)
{
    if (filePath.empty())
        return;

    static std::atomic<ULONGLONG> s_lastAdminTick{0};
    static std::atomic<int> s_activeAdminCount{0};
    if (!TryAcquireActionSlot(s_lastAdminTick, s_activeAdminCount))
        return;

    try
    {
        std::thread([filePath]() {
            ActiveCountGuard guard{s_activeAdminCount};
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

            std::wstring workDir;
            if (!EndsWithCaseInsensitive(filePath, L".lnk"))
            {
                std::error_code ec;
                std::filesystem::path p(filePath);
                if (p.has_parent_path() && std::filesystem::exists(p.parent_path(), ec))
                {
                    workDir = p.parent_path().wstring();
                }
            }

            SHELLEXECUTEINFOW sei = {sizeof(sei)};
            sei.fMask = SEE_MASK_NOASYNC; // 确保无消息泵的后台 STA 线程同步等待 Shell/DDE 执行完毕再销毁套间
            sei.lpVerb = L"runas";
            sei.lpFile = filePath.c_str();
            sei.lpDirectory = workDir.empty() ? nullptr : workDir.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);

            if (SUCCEEDED(hr))
                CoUninitialize();
        }).detach();
    }
    catch (...)
    {
        s_activeAdminCount.fetch_sub(1, std::memory_order_acq_rel);
    }
}

// 2. Win32：打开文件所在位置（若传入 .lnk 则在后台 STA 线程内自动解析真实目标）
void ProcessHelper::OpenFileLocation(const std::wstring &filePath)
{
    if (filePath.empty())
        return;

    static std::atomic<ULONGLONG> s_lastOpenLocTick{0};
    static std::atomic<int> s_activeOpenLocCount{0};
    if (!TryAcquireActionSlot(s_lastOpenLocTick, s_activeOpenLocCount))
        return;

    try
    {
        std::thread([filePath]() {
            ActiveCountGuard guard{s_activeOpenLocCount};
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

            std::wstring target = filePath;
            if (EndsWithCaseInsensitive(target, L".lnk"))
            {
                std::wstring resolved = Win32Provider::ResolveLnkTarget(target);
                if (!resolved.empty())
                {
                    target = std::move(resolved);
                }
            }

            PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(target.c_str());
            if (!pidl && target != filePath)
            {
                pidl = ILCreateFromPathW(filePath.c_str());
            }
            if (pidl)
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }

            if (SUCCEEDED(hr))
                CoUninitialize();
        }).detach();
    }
    catch (...)
    {
        s_activeOpenLocCount.fetch_sub(1, std::memory_order_acq_rel);
    }
}

// 3. UWP：在独立 MTA 线程中通过 AUMID 提取 PackageFamilyName 定向查找并静默卸载（消除 O(N) 跨进程异步枚举）
void ProcessHelper::UninstallUwpByAumid(const std::wstring &aumid, std::function<void()> onComplete)
{
    if (aumid.empty())
        return;

    static std::atomic<bool> s_isUninstalling{false};
    bool expected = false;
    if (!s_isUninstalling.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    try
    {
        std::thread([aumid, onComplete = std::move(onComplete)]() {
            AtomicFlagGuard guard{s_isUninstalling};
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            try
            {
                winrt::Windows::Management::Deployment::PackageManager pkgManager;
                bool uninstalled = false;

                // 标准 AUMID 格式为 "PackageFamilyName!AppId"
                // 直接提取 '!' 前的 PackageFamilyName 进行 O(1) 定向查询，免去全量枚举与 GetAppListEntriesAsync
                size_t bangPos = aumid.find(L'!');
                if (bangPos != std::wstring::npos && bangPos > 0)
                {
                    std::wstring familyName = aumid.substr(0, bangPos);
                    auto packages = pkgManager.FindPackagesForUser(L"", familyName);
                    for (auto const &package : packages)
                    {
                        if (package.IsFramework() || package.IsResourcePackage())
                            continue;
                        pkgManager.RemovePackageAsync(package.Id().FullName()).get();
                        uninstalled = true;
                        break;
                    }
                }

                // 兜底逻辑：仅当 AUMID 格式非标准（不含 '!'）时，才回退至遍历查询
                if (!uninstalled && bangPos == std::wstring::npos)
                {
                    auto packages = pkgManager.FindPackagesForUser(L"");
                    for (auto const &package : packages)
                    {
                        if (package.IsFramework() || package.IsResourcePackage())
                            continue;
                        bool matched = false;
                        try
                        {
                            auto entries = package.GetAppListEntriesAsync().get();
                            for (auto const &entry : entries)
                            {
                                if (entry && entry.AppUserModelId() == aumid)
                                {
                                    matched = true;
                                    break;
                                }
                            }
                        }
                        catch (...)
                        {
                        }

                        if (matched)
                        {
                            pkgManager.RemovePackageAsync(package.Id().FullName()).get();
                            break;
                        }
                    }
                }
            }
            catch (...)
            {
            }
            if (onComplete)
            {
                onComplete();
            }
            winrt::clear_factory_cache();
            CoFreeUnusedLibrariesEx(0, 0);
            winrt::uninit_apartment();
        }).detach();
    }
    catch (...)
    {
        s_isUninstalling.store(false, std::memory_order_release);
    }
}
