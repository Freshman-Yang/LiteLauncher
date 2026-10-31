#pragma once
#include "ConfigManager.h"

namespace I18n
{
inline bool IsEnglish(AppLanguage lang = ConfigManager::GetLanguage()) noexcept
{
    return lang == AppLanguage::EnUS;
}

constexpr const wchar_t *GetLocaleName(AppLanguage lang = ConfigManager::GetLanguage()) noexcept
{
    return (lang == AppLanguage::EnUS) ? L"en-US" : L"zh-CN";
}

constexpr const wchar_t *GetUiFontName(AppLanguage lang = ConfigManager::GetLanguage()) noexcept
{
    return (lang == AppLanguage::EnUS) ? L"Segoe UI" : L"Microsoft YaHei UI";
}

constexpr const wchar_t *Select(const wchar_t *zh, const wchar_t *en,
                                AppLanguage lang = ConfigManager::GetLanguage()) noexcept
{
    return (lang == AppLanguage::EnUS) ? en : zh;
}

// 内置指令中英双语常量
constexpr const wchar_t kCmdSettingsZh[] = L"设置 LiteLauncher";
constexpr const wchar_t kCmdSettingsEn[] = L"Settings LiteLauncher";

constexpr const wchar_t kCmdRefreshZh[] = L"刷新 LiteLauncher";
constexpr const wchar_t kCmdRefreshEn[] = L"Refresh LiteLauncher";

constexpr const wchar_t kCmdExitZh[] = L"退出 LiteLauncher";
constexpr const wchar_t kCmdExitEn[] = L"Exit LiteLauncher";

// 主窗口：全局热键冲突提示与右键菜单文案
inline const wchar_t *HotkeyFailTitle() noexcept
{
    return Select(L"LiteLauncher 提示", L"LiteLauncher Notice");
}

inline const wchar_t *HotkeyFailMsg() noexcept
{
    return Select(L"全局快捷键 Alt+Space 注册失败，可能已被其它程序占用！",
                  L"Failed to register global hotkey Alt+Space. It may already be in use by another application!");
}

inline const wchar_t *MenuRunAsAdmin() noexcept
{
    return Select(L"以管理员身份运行", L"Run as administrator");
}

inline const wchar_t *MenuOpenLocation() noexcept
{
    return Select(L"打开文件所在位置", L"Open file location");
}

inline const wchar_t *MenuUninstallUwp() noexcept
{
    return Select(L"卸载此应用", L"Uninstall this app");
}

// 设置窗口：标题与各控件静态文案
inline const wchar_t *SettingsWindowTitle() noexcept
{
    return Select(L"设置 - LiteLauncher", L"Settings - LiteLauncher");
}

inline const wchar_t *SettingsLabelBackdrop() noexcept
{
    return Select(L"窗口材质 (皮肤)", L"Window Backdrop");
}

inline const wchar_t *SettingsLabelLanguage() noexcept
{
    return Select(L"语言 / Language", L"Language");
}

inline const wchar_t *BackdropOptionPureBlur() noexcept
{
    return Select(L"透明模糊 (默认)", L"Pure Blur (Default)");
}

inline const wchar_t *BackdropOptionAcrylic() noexcept
{
    return Select(L"经典亚克力 (Acrylic)", L"Acrylic");
}

inline const wchar_t *BackdropOptionMica() noexcept
{
    return Select(L"云母 (Mica)", L"Mica");
}

inline const wchar_t *BackdropOptionMicaAlt() noexcept
{
    return Select(L"云母深色 (Mica Alt)", L"Mica Alt");
}

inline const wchar_t *SettingsCheckAutorun() noexcept
{
    return Select(L"开机自动启动", L"Launch at startup");
}

inline const wchar_t *SettingsLabelPaths() noexcept
{
    return Select(L"应用程序检索路径 (每行一个路径)", L"Application Search Paths (One path per line)");
}

inline const wchar_t *SettingsButtonRestore() noexcept
{
    return Select(L"恢复默认路径", L"Restore Defaults");
}

inline const wchar_t *SettingsLabelDesc() noexcept
{
    return Select(
        L"说明：系统默认路径仅扫描 .lnk 快捷方式（桌面1层，开始菜单4层）；新增的自定义路径仅扫描 .exe 文件（最大深度2层）。",
        L"Note: Default system paths scan .lnk shortcuts only (Desktop: 1 level, Start Menu: 4 levels); custom paths scan .exe files only (max depth: 2 levels).");
}

inline const wchar_t *SettingsButtonSave() noexcept
{
    return Select(L"保存并刷新索引", L"Save & Rebuild Index");
}

inline const wchar_t *SettingsButtonCancel() noexcept
{
    return Select(L"关闭", L"Close");
}
} // namespace I18n
