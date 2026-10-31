#include "pch.h"
#include "Core/ConfigManager.h"
#include "UI/LauncherWindow.h"
#include "UI/SettingsWindow.h"

constexpr wchar_t SINGLE_INSTANCE_MUTEX[] = L"Local\\LiteLauncher_SingleInstance_Mutex";

namespace
{
struct FindInstanceContext
{
    std::wstring SelfExePath;
    std::wstring_view SelfFileName;
    HWND FoundHwnd = nullptr;
    DWORD FoundPid = 0;
};

// 提取路径中的纯文件名部分
constexpr std::wstring_view ExtractFileName(std::wstring_view path) noexcept
{
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring_view::npos) ? path : path.substr(pos + 1);
}

// 查找并校验已运行实例的主窗口（核验窗口类名与所属进程映像名，兼顾防欺骗与管理员权限/目录迁移唤醒）
HWND FindVerifiedLauncherWindow(DWORD *outPid) noexcept
{
    FindInstanceContext ctx;
    ctx.SelfExePath = GetSelfExePath();
    if (ctx.SelfExePath.empty())
        return nullptr;
    ctx.SelfFileName = ExtractFileName(ctx.SelfExePath);

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            wchar_t className[64]{};
            if (GetClassNameW(hwnd, className, _countof(className)) <= 0 ||
                wcscmp(className, LAUNCHER_WINDOW_CLASS) != 0)
            {
                return TRUE; // 继续枚举
            }

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == 0 || pid == GetCurrentProcessId())
            {
                return TRUE;
            }

            auto *pCtx = reinterpret_cast<FindInstanceContext *>(lParam);
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc)
            {
                wchar_t procPath[MAX_PATH * 2]{};
                DWORD pathLen = _countof(procPath);
                bool match = false;
                if (QueryFullProcessImageNameW(hProc, 0, procPath, &pathLen))
                {
                    std::wstring_view targetPath(procPath, pathLen);
                    std::wstring_view targetFileName = ExtractFileName(targetPath);

                    // 优先比对完整路径；若用户移动了目录或覆盖更新，则比对 EXE 文件名（如 LiteLauncher.exe）
                    match = (CompareStringOrdinal(targetPath.data(), static_cast<int>(targetPath.size()),
                                                  pCtx->SelfExePath.c_str(), static_cast<int>(pCtx->SelfExePath.size()),
                                                  TRUE) == CSTR_EQUAL) ||
                            (!targetFileName.empty() &&
                             CompareStringOrdinal(targetFileName.data(), static_cast<int>(targetFileName.size()),
                                                  pCtx->SelfFileName.data(), static_cast<int>(pCtx->SelfFileName.size()),
                                                  TRUE) == CSTR_EQUAL);
                }
                CloseHandle(hProc);

                if (match)
                {
                    pCtx->FoundHwnd = hwnd;
                    pCtx->FoundPid = pid;
                    return FALSE; // 找到合法主实例窗口，停止枚举
                }
            }
            else if (GetLastError() == ERROR_ACCESS_DENIED)
            {
                // 若已运行的实例处于更高权限（如管理员模式）导致 OpenProcess 拒绝访问，
                // 鉴于专有窗口类名 LiteLauncher_MainWindow_Class 已校验通过，优雅放行唤醒
                pCtx->FoundHwnd = hwnd;
                pCtx->FoundPid = pid;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (outPid)
    {
        *outPid = ctx.FoundPid;
    }
    return ctx.FoundHwnd;
}
} // namespace

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE /*hPrevInstance*/, _In_ LPWSTR /*lpCmdLine*/,
                    _In_ int /*nCmdShow*/)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    // 单实例检测与极速唤醒拦截
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, SINGLE_INSTANCE_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // 查找已运行实例的主窗口句柄。
        // 若主实例刚抢占 Mutex 尚在初始化（窗口未创建完毕），进行短暂轮询重试（最多 20 * 15ms = 300ms），消除唤醒竞态
        HWND hExistingWnd = nullptr;
        DWORD dwProcessId = 0;
        for (int retry = 0; retry < 20; ++retry)
        {
            hExistingWnd = FindVerifiedLauncherWindow(&dwProcessId);
            if (hExistingWnd != nullptr)
            {
                break;
            }
            Sleep(15);
        }

        if (hExistingWnd && dwProcessId != 0)
        {
            // 将前台焦点设置权限授予已核验身份的主实例进程，确保其调用 SetForegroundWindow 成功
            AllowSetForegroundWindow(dwProcessId);

            // 发送异步唤醒/隐藏切换消息
            PostMessageW(hExistingWnd, WM_SINGLE_INSTANCE_WAKEUP, 0, 0);
        }

        if (hMutex)
        {
            CloseHandle(hMutex);
        }
        return 0; // 极速退出，不加载任何图形与业务资源
    }

    // 确保高 DPI 感知（Per-Monitor V2），防止 Direct2D 与 GDI 字体模糊
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 初始化 STA COM 环境（供 WIC、Direct2D、DirectWrite 及 Shell API 使用）
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // 初始化通用控件库（供 EDIT 搜索框及 SettingsWindow 现代控件使用）
    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    // 加载本地配置文件
    ConfigManager::LoadConfig();

    // 创建主窗口并绑定设置窗口回调（默认不调用 ShowWindow，实现纯粹的静默启动常驻）
    LauncherWindow launcher;
    if (!launcher.Create(hInstance))
    {
        if (hMutex)
        {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
        return -1;
    }

    // 绑定打开“设置窗口”的回调，并支持下拉框实时预览切换窗口材质与界面语言热更新
    launcher.SetOpenSettingsCallback([&launcher]() {
        SettingsWindow::Show(
            [&launcher](int backdropIndex) { launcher.ApplyBackdrop(static_cast<BackdropType>(backdropIndex)); },
            [&launcher]() { launcher.OnLanguageChanged(); });
    });

    // 标准 Win32 消息循环
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        // 仅当消息所属窗口归属于 SettingsWindow 时才分发给 IsDialogMessageW，防止拦截主窗口按键
        HWND hSettings = SettingsWindow::GetHwnd();
        if (hSettings && IsWindow(hSettings) && (msg.hwnd == hSettings || IsChild(hSettings, msg.hwnd)))
        {
            if (IsDialogMessageW(hSettings, &msg))
            {
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 退出清理
    SettingsWindow::Close();

    if (hMutex)
    {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam);
}
