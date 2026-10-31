#include "pch.h"
#include "SettingsWindow.h"
#include "Core/ConfigManager.h"
#include "Core/I18n.h"
#include "Core/SearchManager.h"
#include "Assets/resource.h"
#include <sstream>
#include <uxtheme.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

constexpr wchar_t SETTINGS_WND_CLASS[] = L"LiteLauncher_SettingsWindowClass";

void SettingsWindow::RegisterWindowClass(HINSTANCE hInstance)
{
    if (s_classRegistered)
        return;

    WNDCLASSEXW wcex = {sizeof(WNDCLASSEXW)};
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr; // 在 WM_ERASEBKGND / WM_CTLCOLORSTATIC 中统一管理纯白背景
    wcex.lpszClassName = SETTINGS_WND_CLASS;

    RegisterClassExW(&wcex);
    s_classRegistered = true;
}

void SettingsWindow::Show(std::function<void(int)> onBackdropChanged, std::function<void()> onLanguageChanged)
{
    if (onBackdropChanged)
    {
        s_onBackdropChanged = std::move(onBackdropChanged);
    }
    if (onLanguageChanged)
    {
        s_onLanguageChanged = std::move(onLanguageChanged);
    }

    // 如果窗口已存在，直接激活到前台
    if (s_hwnd && IsWindow(s_hwnd))
    {
        if (IsIconic(s_hwnd))
        {
            ShowWindow(s_hwnd, SW_RESTORE);
        }
        SetForegroundWindow(s_hwnd);
        return;
    }

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    RegisterWindowClass(hInstance);

    // 获取鼠标所在显示器或主显示器的工作区与初始 DPI
    POINT pt = {0, 0};
    GetCursorPos(&pt);
    HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(MONITORINFO)};
    GetMonitorInfoW(hMonitor, &mi);

    // 先创建一个隐藏窗口以获取准确的窗口 DPI
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    s_hwnd = CreateWindowExW(0, SETTINGS_WND_CLASS, I18n::SettingsWindowTitle(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                             620, 500, nullptr, nullptr, hInstance, nullptr);

    if (!s_hwnd)
        return;

    // 按当前 DPI 计算 620x500 缩放尺寸并居中到屏幕
    UINT dpi = GetDpiForWindow(s_hwnd);
    float scale = dpi / 96.0f;
    int width = static_cast<int>(620 * scale);
    int height = static_cast<int>(500 * scale);

    int screenW = mi.rcWork.right - mi.rcWork.left;
    int screenH = mi.rcWork.bottom - mi.rcWork.top;
    int x = mi.rcWork.left + (screenW - width) / 2;
    int y = mi.rcWork.top + (screenH - height) / 2;

    SetWindowPos(s_hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    ShowWindow(s_hwnd, SW_SHOW);
    UpdateWindow(s_hwnd);
    SetForegroundWindow(s_hwnd);
}

void SettingsWindow::Close()
{
    if (s_hwnd && IsWindow(s_hwnd))
    {
        DestroyWindow(s_hwnd);
    }
}

void SettingsWindow::CreateControls(HWND hwnd)
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // 1. 窗口材质 (皮肤) 与界面语言下拉框、开机自动启动复选框
    s_lblBackdrop = CreateWindowExW(0, WC_STATICW, I18n::SettingsLabelBackdrop(), WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                    hwnd, nullptr, hInst, nullptr);

    s_cmbBackdrop =
        CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 0,
                        0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CMB_BACKDROP)), hInst, nullptr);

    s_lblLanguage = CreateWindowExW(0, WC_STATICW, I18n::SettingsLabelLanguage(), WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                    hwnd, nullptr, hInst, nullptr);

    s_cmbLanguage =
        CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 0,
                        0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CMB_LANGUAGE)), hInst, nullptr);

    SendMessageW(s_cmbLanguage, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"简体中文"));
    SendMessageW(s_cmbLanguage, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));

    s_chkAutorun = CreateWindowExW(0, WC_BUTTONW, I18n::SettingsCheckAutorun(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHK_AUTORUN)), hInst, nullptr);

    // 2. 检索路径配置标题与恢复默认按钮
    s_lblPaths = CreateWindowExW(0, WC_STATICW, I18n::SettingsLabelPaths(), WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0,
                                 0, 0, 0, hwnd, nullptr, hInst, nullptr);

    s_btnRestore = CreateWindowExW(0, WC_BUTTONW, I18n::SettingsButtonRestore(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_RESTORE)), hInst, nullptr);

    // 说明文本
    s_lblDesc = CreateWindowExW(0, WC_STATICW, I18n::SettingsLabelDesc(), WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                nullptr, hInst, nullptr);

    // 多行路径编辑框
    s_edtPaths =
        CreateWindowExW(0, WC_EDITW, L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                            ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
                        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDT_PATHS)), hInst, nullptr);

    // 解除默认文本长度限制
    SendMessageW(s_edtPaths, EM_SETLIMITTEXT, 0, 0);

    // 3. 底部保存与关闭按钮
    s_btnSave = CreateWindowExW(0, WC_BUTTONW, I18n::SettingsButtonSave(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SAVE)), hInst, nullptr);

    s_btnCancel =
        CreateWindowExW(0, WC_BUTTONW, I18n::SettingsButtonCancel(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CANCEL)), hInst, nullptr);

    RefreshLocalizedTexts(hwnd);
}

void SettingsWindow::RefreshLocalizedTexts(HWND hwnd)
{
    SetWindowTextW(hwnd, I18n::SettingsWindowTitle());
    if (s_lblBackdrop)
        SetWindowTextW(s_lblBackdrop, I18n::SettingsLabelBackdrop());
    if (s_lblLanguage)
        SetWindowTextW(s_lblLanguage, I18n::SettingsLabelLanguage());
    if (s_chkAutorun)
        SetWindowTextW(s_chkAutorun, I18n::SettingsCheckAutorun());
    if (s_lblPaths)
        SetWindowTextW(s_lblPaths, I18n::SettingsLabelPaths());
    if (s_btnRestore)
        SetWindowTextW(s_btnRestore, I18n::SettingsButtonRestore());
    if (s_lblDesc)
        SetWindowTextW(s_lblDesc, I18n::SettingsLabelDesc());
    if (s_btnSave)
        SetWindowTextW(s_btnSave, I18n::SettingsButtonSave());
    if (s_btnCancel)
        SetWindowTextW(s_btnCancel, I18n::SettingsButtonCancel());

    if (s_cmbBackdrop)
    {
        int curBackdrop = static_cast<int>(SendMessageW(s_cmbBackdrop, CB_GETCURSEL, 0, 0));
        if (curBackdrop < 0 || curBackdrop > 3)
        {
            curBackdrop = static_cast<int>(ConfigManager::GetBackdrop());
        }
        SendMessageW(s_cmbBackdrop, CB_RESETCONTENT, 0, 0);
        SendMessageW(s_cmbBackdrop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(I18n::BackdropOptionPureBlur()));
        SendMessageW(s_cmbBackdrop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(I18n::BackdropOptionAcrylic()));
        SendMessageW(s_cmbBackdrop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(I18n::BackdropOptionMica()));
        SendMessageW(s_cmbBackdrop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(I18n::BackdropOptionMicaAlt()));
        SendMessageW(s_cmbBackdrop, CB_SETCURSEL, curBackdrop, 0);
    }
}

void SettingsWindow::UpdateFontsAndLayout(HWND hwnd, UINT dpi)
{
    float scale = dpi / 96.0f;
    auto S = [scale](int val) -> int { return static_cast<int>(val * scale + 0.5f); };

    // 清理旧字体
    if (s_fontTitle)
    {
        DeleteObject(s_fontTitle);
        s_fontTitle = nullptr;
    }
    if (s_fontNormal)
    {
        DeleteObject(s_fontNormal);
        s_fontNormal = nullptr;
    }
    if (s_fontDesc)
    {
        DeleteObject(s_fontDesc);
        s_fontDesc = nullptr;
    }
    if (s_fontCode)
    {
        DeleteObject(s_fontCode);
        s_fontCode = nullptr;
    }

    // 创建匹配当前 DPI 与语言环境的 UI 字体
    const wchar_t *uiFontName = I18n::GetUiFontName();
    auto createFont = [](int pixelHeight, int weight, const wchar_t *faceName) -> HFONT {
        return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faceName);
    };

    s_fontTitle = createFont(S(16), FW_SEMIBOLD, uiFontName);
    s_fontNormal = createFont(S(14), FW_NORMAL, uiFontName);
    s_fontDesc = createFont(S(12), FW_NORMAL, uiFontName);
    s_fontCode = createFont(S(13), FW_NORMAL, uiFontName);

    // 应用字体到所有子控件
    SendMessageW(s_lblBackdrop, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontTitle), TRUE);
    SendMessageW(s_cmbBackdrop, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontNormal), TRUE);
    SendMessageW(s_lblLanguage, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontTitle), TRUE);
    SendMessageW(s_cmbLanguage, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontNormal), TRUE);
    SendMessageW(s_chkAutorun, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontNormal), TRUE);
    SendMessageW(s_lblPaths, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontTitle), TRUE);
    SendMessageW(s_btnRestore, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontNormal), TRUE);
    SendMessageW(s_lblDesc, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontDesc), TRUE);
    SendMessageW(s_edtPaths, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontCode), TRUE);
    SendMessageW(s_btnSave, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontNormal), TRUE);
    SendMessageW(s_btnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(s_fontNormal), TRUE);

    // 调整下拉框项高度以适应 DPI
    SendMessageW(s_cmbBackdrop, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), S(24));
    SendMessageW(s_cmbBackdrop, CB_SETITEMHEIGHT, 0, S(22));
    SendMessageW(s_cmbLanguage, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), S(24));
    SendMessageW(s_cmbLanguage, CB_SETITEMHEIGHT, 0, S(22));

    // 获取客户区尺寸并计算布局 (Padding=24, RowSpacing=16)
    RECT rcClient = {0};
    GetClientRect(hwnd, &rcClient);
    int clientW = rcClient.right - rcClient.left;
    int clientH = rcClient.bottom - rcClient.top;
    if (clientW <= 0 || clientH <= 0)
        return;

    int pad = S(24);
    int contentW = clientW - pad * 2;
    int y = pad;

    // 1. 皮肤材质选择区域、界面语言选择区域与开机自启复选框（三列并排对齐）
    int cmbBackdropW = S(200);
    int cmbLangW = S(160);
    int colGap = S(20);
    int langX = pad + cmbBackdropW + colGap;
    int chkX = langX + cmbLangW + colGap;
    int chkW = (std::max)(S(130), contentW - (chkX - pad));

    SetWindowPos(s_lblBackdrop, nullptr, pad, y, cmbBackdropW, S(24), SWP_NOZORDER);
    SetWindowPos(s_lblLanguage, nullptr, langX, y, cmbLangW, S(24), SWP_NOZORDER);
    y += S(24) + S(8);

    // CBS_DROPDOWNLIST 的高度包含下拉列表弹出框高度
    SetWindowPos(s_cmbBackdrop, nullptr, pad, y, cmbBackdropW, S(200), SWP_NOZORDER);
    SetWindowPos(s_cmbLanguage, nullptr, langX, y, cmbLangW, S(200), SWP_NOZORDER);
    SetWindowPos(s_chkAutorun, nullptr, chkX, y, chkW, S(30), SWP_NOZORDER);
    y += S(32) + S(16);

    // 2. 检索路径标题与右侧“恢复默认路径”按钮
    int btnRestoreW = S(136);
    int btnRestoreH = S(30);
    SetWindowPos(s_lblPaths, nullptr, pad, y, contentW - btnRestoreW - S(12), btnRestoreH, SWP_NOZORDER);
    SetWindowPos(s_btnRestore, nullptr, pad + contentW - btnRestoreW, y, btnRestoreW, btnRestoreH, SWP_NOZORDER);
    y += btnRestoreH + S(8);

    // 说明文本（预留双行高度防止截断）
    int descH = S(38);
    SetWindowPos(s_lblDesc, nullptr, pad, y, contentW, descH, SWP_NOZORDER);
    y += descH + S(8);

    // 3. 底部按钮区域尺寸
    int btnBottomH = S(32);
    int btnSaveW = S(156);
    int btnCancelW = S(92);
    int btnSpacing = S(12);
    int bottomY = clientH - pad - btnBottomH;

    // 中间多行文本框占据剩余全部垂直空间
    int editH = bottomY - S(16) - y;
    if (editH < S(60))
        editH = S(60);
    SetWindowPos(s_edtPaths, nullptr, pad, y, contentW, editH, SWP_NOZORDER);

    // 右下角按钮排列
    int cancelX = clientW - pad - btnCancelW;
    int saveX = cancelX - btnSpacing - btnSaveW;
    SetWindowPos(s_btnSave, nullptr, saveX, bottomY, btnSaveW, btnBottomH, SWP_NOZORDER);
    SetWindowPos(s_btnCancel, nullptr, cancelX, bottomY, btnCancelW, btnBottomH, SWP_NOZORDER);

    InvalidateRect(hwnd, nullptr, TRUE);
}

void SettingsWindow::LoadCurrentSettings()
{
    s_isInitializing = true;

    // 同步当前皮肤材质选项
    int backdropIdx = static_cast<int>(ConfigManager::GetBackdrop());
    SendMessageW(s_cmbBackdrop, CB_SETCURSEL, backdropIdx, 0);

    // 同步当前语言选项
    if (s_cmbLanguage)
    {
        int langIdx = (ConfigManager::GetLanguage() == AppLanguage::EnUS) ? 1 : 0;
        SendMessageW(s_cmbLanguage, CB_SETCURSEL, langIdx, 0);
    }

    // 同步开机自动启动状态
    if (s_chkAutorun)
    {
        DWORD dataSize = 0;
        LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                      L"LiteLauncher", RRF_RT_REG_SZ, nullptr, nullptr, &dataSize);
        bool isAutorun = (status == ERROR_SUCCESS && dataSize > sizeof(wchar_t));
        SendMessageW(s_chkAutorun, BM_SETCHECK, isAutorun ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    // 同步当前检索路径列表
    auto paths = ConfigManager::GetSearchPaths();
    std::wstring joinedPaths;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        joinedPaths += paths[i];
        if (i + 1 < paths.size())
        {
            joinedPaths += L"\r\n";
        }
    }
    SetWindowTextW(s_edtPaths, joinedPaths.c_str());

    s_isInitializing = false;
}

void SettingsWindow::ApplyTheme(HWND hwnd)
{
    s_isDarkMode = IsSystemAppsDarkMode();

    // 1. 启用 Win32 原生通用控件深色主题支持 (uxtheme 未导出序号 135 & 136)
    if (HMODULE hUxtheme = GetModuleHandleW(L"uxtheme.dll"))
    {
        using fnSetPreferredAppMode = int(WINAPI *)(int); // 1 = AllowDark, 2 = ForceDark
        using fnFlushMenuThemes = void(WINAPI *)();
        auto setPreferredAppMode = reinterpret_cast<fnSetPreferredAppMode>(GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135)));
        auto flushMenuThemes = reinterpret_cast<fnFlushMenuThemes>(GetProcAddress(hUxtheme, MAKEINTRESOURCEA(136)));
        if (setPreferredAppMode)
            setPreferredAppMode(s_isDarkMode ? 2 : 0);
        if (flushMenuThemes)
            flushMenuThemes();
    }

    // 2. 同步 DWM 沉浸式暗色标题栏与边框 (同时兼容 Win10 1809 的 19 与 Win10 2004+/Win11 的 20)
    BOOL useDark = s_isDarkMode ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark))))
    {
        DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
    }

    // 3. 重建背景画刷与编辑框底色画刷
    if (s_bgBrush)
    {
        DeleteObject(s_bgBrush);
        s_bgBrush = nullptr;
    }
    if (s_editBgBrush)
    {
        DeleteObject(s_editBgBrush);
        s_editBgBrush = nullptr;
    }

    // 深色模式：主背景 #1E1E1E，输入框 #2B2B2B；浅色模式：主背景 #FFFFFF，输入框 #FAFAFA
    s_bgBrush = CreateSolidBrush(s_isDarkMode ? RGB(30, 30, 30) : RGB(255, 255, 255));
    s_editBgBrush = CreateSolidBrush(s_isDarkMode ? RGB(43, 43, 43) : RGB(250, 250, 250));

    // 4. 为各子控件应用对应的 Explorer 深浅色视觉样式
    const wchar_t *btnTheme = s_isDarkMode ? L"DarkMode_Explorer" : L"Explorer";
    const wchar_t *cmbTheme = s_isDarkMode ? L"DarkMode_CFD" : L"CFD";

    if (s_cmbBackdrop) SetWindowTheme(s_cmbBackdrop, cmbTheme, nullptr);
    if (s_cmbLanguage) SetWindowTheme(s_cmbLanguage, cmbTheme, nullptr);
    if (s_chkAutorun)  SetWindowTheme(s_chkAutorun, btnTheme, nullptr);
    if (s_btnRestore)  SetWindowTheme(s_btnRestore, btnTheme, nullptr);
    if (s_edtPaths)    SetWindowTheme(s_edtPaths, btnTheme, nullptr);
    if (s_btnSave)      SetWindowTheme(s_btnSave, btnTheme, nullptr);
    if (s_btnCancel)    SetWindowTheme(s_btnCancel, btnTheme, nullptr);

    InvalidateRect(hwnd, nullptr, TRUE);
}

void SettingsWindow::OnBackdropSelectionChanged()
{
    if (s_isInitializing)
        return;

    int idx = static_cast<int>(SendMessageW(s_cmbBackdrop, CB_GETCURSEL, 0, 0));
    if (idx >= 0 && idx <= 3)
    {
        ConfigManager::SetBackdrop(static_cast<BackdropType>(idx));
        if (s_onBackdropChanged)
        {
            s_onBackdropChanged(idx);
        }
    }
}

void SettingsWindow::OnLanguageSelectionChanged()
{
    if (s_isInitializing)
        return;

    int idx = static_cast<int>(SendMessageW(s_cmbLanguage, CB_GETCURSEL, 0, 0));
    if (idx == 0 || idx == 1)
    {
        AppLanguage newLang = (idx == 1) ? AppLanguage::EnUS : AppLanguage::ZhCN;
        if (newLang != ConfigManager::GetLanguage())
        {
            // 1. 立即保存语言配置
            ConfigManager::SetLanguage(newLang);

            // 2. 立即原地刷新设置窗口自身的所有控件文案（并保持材质下拉框等控件原有的选中状态）
            s_isInitializing = true;
            RefreshLocalizedTexts(s_hwnd);
            s_isInitializing = false;
            UpdateFontsAndLayout(s_hwnd, GetDpiForWindow(s_hwnd));

            // 3. 立即触发后台重新构建搜索缓存并通知主窗口刷新以应用新语言的字体与搜索结果
            if (s_onLanguageChanged)
            {
                s_onLanguageChanged();
            }
            else
            {
                SearchManager::RequestRescanAsync();
            }
        }
    }
}

void SettingsWindow::OnAutorunToggle()
{
    if (s_isInitializing || !s_chkAutorun)
        return;

    constexpr const wchar_t *kRunSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t *kValueName = L"LiteLauncher";

    bool checked = (SendMessageW(s_chkAutorun, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (checked)
    {
        std::wstring exePath = GetSelfExePath();
        if (!exePath.empty())
        {
            std::wstring quotedPath = L"\"" + exePath + L"\"";
            RegSetKeyValueW(HKEY_CURRENT_USER, kRunSubKey, kValueName, REG_SZ, quotedPath.c_str(),
                            static_cast<DWORD>((quotedPath.size() + 1) * sizeof(wchar_t)));
        }
    }
    else
    {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunSubKey, kValueName);
    }
}

void SettingsWindow::OnRestoreDefaultPathsClick()
{
    auto defaults = ConfigManager::GetDefaultPaths();
    std::wstring joined;
    for (size_t i = 0; i < defaults.size(); ++i)
    {
        joined += defaults[i];
        if (i + 1 < defaults.size())
        {
            joined += L"\r\n";
        }
    }
    SetWindowTextW(s_edtPaths, joined.c_str());
}

void SettingsWindow::OnSaveClick()
{
    int len = GetWindowTextLengthW(s_edtPaths);
    std::wstring rawText;
    if (len > 0)
    {
        rawText.resize(static_cast<size_t>(len));
        int copied = GetWindowTextW(s_edtPaths, rawText.data(), len + 1);
        rawText.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    }

    std::wstringstream ss(rawText);
    std::wstring line;
    std::vector<std::wstring> newPaths;

    while (std::getline(ss, line, L'\n'))
    {
        // 清理首尾空白字符与双引号（防止用户“复制为路径”带入引号）
        while (!line.empty() &&
               (line.back() == L' ' || line.back() == L'\t' || line.back() == L'\r' || line.back() == L'"'))
        {
            line.pop_back();
        }
        while (!line.empty() && (line.front() == L' ' || line.front() == L'\t' || line.front() == L'"'))
        {
            line.erase(line.begin());
        }
        if (!line.empty())
        {
            newPaths.push_back(line);
        }
    }

    if (newPaths.empty())
    {
        newPaths = ConfigManager::GetDefaultPaths();
    }

    ConfigManager::SetSearchPaths(newPaths);

    SearchManager::RequestRescanAsync();

    DestroyWindow(s_hwnd);
}

void SettingsWindow::OnCancelClick()
{
    DestroyWindow(s_hwnd);
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE: {
        // 分别加载标准尺寸的大图标 (32x32) 和小图标 (16x16)，避免左上角小图标模糊/锯齿
        HICON hBigIcon =
            static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
        HICON hSmallIcon =
            static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (hBigIcon)
        {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hBigIcon));
        }
        if (hSmallIcon)
        {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmallIcon));
        }

        CreateControls(hwnd);
        ApplyTheme(hwnd); // 在创建控件后立即应用深浅色主题与 DWM 标题栏属性
        UpdateFontsAndLayout(hwnd, GetDpiForWindow(hwnd));
        LoadCurrentSettings();
        return 0;
    }

    case WM_SETTINGCHANGE: {
        // 监听系统深浅色主题实时切换（防御第三方程序错误广播整数/ATOM 伪指针 <= 0xFFFF）
        if (lParam && !IS_INTRESOURCE(lParam) &&
            wcsncmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet", _countof(L"ImmersiveColorSet")) == 0)
        {
            if (IsSystemAppsDarkMode() != s_isDarkMode)
            {
                ApplyTheme(hwnd);
            }
        }
        break;
    }
        
    case WM_SIZE: {
        if (wParam != SIZE_MINIMIZED)
        {
            UpdateFontsAndLayout(hwnd, GetDpiForWindow(hwnd));
        }
        return 0;
    }

    case WM_DPICHANGED: {
        UINT newDpi = HIWORD(wParam);
        RECT *const prcNewWindow = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(hwnd, nullptr, prcNewWindow->left, prcNewWindow->top, prcNewWindow->right - prcNewWindow->left,
                     prcNewWindow->bottom - prcNewWindow->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateFontsAndLayout(hwnd, newDpi);
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, s_bgBrush ? s_bgBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);
        SetBkMode(hdc, TRANSPARENT);
        if (hCtrl == s_lblDesc)
        {
            SetTextColor(hdc, s_isDarkMode ? RGB(166, 166, 166) : RGB(102, 102, 102));
        }
        else
        {
            SetTextColor(hdc, s_isDarkMode ? RGB(240, 240, 240) : RGB(26, 26, 26));
        }
        return reinterpret_cast<LRESULT>(s_bgBrush ? s_bgBrush : GetStockObject(WHITE_BRUSH));
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, s_isDarkMode ? RGB(43, 43, 43) : RGB(250, 250, 250));
        SetTextColor(hdc, s_isDarkMode ? RGB(240, 240, 240) : RGB(26, 26, 26));
        return reinterpret_cast<LRESULT>(s_editBgBrush ? s_editBgBrush : GetStockObject(WHITE_BRUSH));
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_CMB_BACKDROP && code == CBN_SELCHANGE)
        {
            OnBackdropSelectionChanged();
            return 0;
        }
        if (id == IDC_CMB_LANGUAGE && code == CBN_SELCHANGE)
        {
            OnLanguageSelectionChanged();
            return 0;
        }
        if (code == BN_CLICKED)
        {
            switch (id)
            {
            case IDC_CHK_AUTORUN:
                OnAutorunToggle();
                return 0;
            case IDC_BTN_RESTORE:
                OnRestoreDefaultPathsClick();
                return 0;
            case IDC_BTN_SAVE:
                OnSaveClick();
                return 0;
            case IDC_BTN_CANCEL:
            case IDCANCEL:
                OnCancelClick();
                return 0;
            }
        }
        break;
    }

    case WM_CLOSE: {
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY: {
        // 彻底释放所有 GDI 资源与窗口句柄引用
        if (s_fontTitle)
        {
            DeleteObject(s_fontTitle);
            s_fontTitle = nullptr;
        }
        if (s_fontNormal)
        {
            DeleteObject(s_fontNormal);
            s_fontNormal = nullptr;
        }
        if (s_fontDesc)
        {
            DeleteObject(s_fontDesc);
            s_fontDesc = nullptr;
        }
        if (s_fontCode)
        {
            DeleteObject(s_fontCode);
            s_fontCode = nullptr;
        }
        if (s_bgBrush)
        {
            DeleteObject(s_bgBrush);
            s_bgBrush = nullptr;
        }
        if (s_editBgBrush)
        {
            DeleteObject(s_editBgBrush);
            s_editBgBrush = nullptr;
        }

        s_lblBackdrop = nullptr;
        s_cmbBackdrop = nullptr;
        s_lblLanguage = nullptr;
        s_cmbLanguage = nullptr;
        s_chkAutorun = nullptr;
        s_lblPaths = nullptr;
        s_btnRestore = nullptr;
        s_lblDesc = nullptr;
        s_edtPaths = nullptr;
        s_btnSave = nullptr;
        s_btnCancel = nullptr;
        s_hwnd = nullptr;
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
