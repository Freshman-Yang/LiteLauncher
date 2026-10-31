#pragma once
#include "Core/ConfigManager.h"
#include "Core/SearchManager.h"
#include "IconManager.h"

// 自定义窗口消息：单实例唤醒与后台刷新完成通知
#define WM_SINGLE_INSTANCE_WAKEUP (WM_USER + 100)
#define WM_SEARCH_REFRESHED (WM_USER + 102)
constexpr wchar_t LAUNCHER_WINDOW_CLASS[] = L"LiteLauncher_MainWindow_Class";

class LauncherWindow
{
  public:
    LauncherWindow();
    ~LauncherWindow();

    // 创建并初始化主窗口
    bool Create(HINSTANCE hInstance);

    // 窗口显示与隐藏控制
    void ShowAndFocus();
    void HideWindow();

    // 动态切换窗口材质 (供设置窗口实时调用)
    void ApplyBackdrop(BackdropType type);

    // 动态响应界面语言切换 (供设置窗口实时调用，更新字体、排版与搜索缓存)
    void OnLanguageChanged();

    // 设置打开“设置窗口”的回调接口 (解耦 SettingsWindow)
    void SetOpenSettingsCallback(std::function<void()> callback)
    {
        m_openSettingsCallback = std::move(callback);
    }

    HWND GetHwnd() const
    {
        return m_hwnd;
    }

  private:
    // D2D 自绘右键菜单项结构
    struct ContextMenuItem
    {
        UINT cmdId;
        wchar_t glyph;
        std::wstring text;
    };

    // 窗口过程与 EDIT 控件子类化过程
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass,
                                             DWORD_PTR dwRefData);

    // Direct2D / DirectWrite / WIC 资源管理
    void CreateDeviceIndependentResources();
    void DiscardDeviceIndependentResources();
    void CreateDeviceResources();
    void DiscardDeviceResources();
    void UpdateDpiResources();
    void RebuildVisibleItemLayouts();
    void ClearVisibleItemLayouts();
    void OnPaint(const RECT &rcPaint);

    // 主题与视觉更新
    void UpdateTheme();
    void UpdateWindowSize();
    void UpdateSearchBoxLayout();
    void RebuildSearchTextLayout();

    // 核心搜索与交互逻辑
    void OnSearchTextChanged();
    void EnsureSelectedVisible();
    void LaunchSelectedItem();
    void ExecuteItem(const SearchResultItem &item);

    // D2D 现代右键菜单控制与命中测试
    void ShowContextMenu(int index, POINT ptClient);
    void CloseContextMenu();
    void ExecuteContextMenuCommand(UINT cmdId);
    int HitTestContextMenuItem(POINT ptClient) const;

    int HitTestItem(POINT ptClient) const;
    bool HitTestClearButton(POINT ptClient) const;

  private:
    HWND m_hwnd = nullptr;
    HWND m_hEdit = nullptr;
    HFONT m_hEditFont = nullptr;

    float m_dpiScale = 1.0f;
    UINT m_currentDpi = 0;
    UINT m_fontDpi = 0;
    AppLanguage m_fontLang = AppLanguage::ZhCN;
    int m_cachedEditHeight = 0;
    bool m_isDarkMode = true;

    // 搜索数据与列表状态
    std::wstring m_currentQuery;
    std::wstring m_imeCompString;
    int m_imeCursorPos = 0;
    std::wstring m_predictionSuffix; // 预测词的后半部分
    std::vector<SearchResultItem> m_results;
    int m_selectedIndex = -1;
    int m_hoveredIndex = -1;
    int m_scrollOffset = 0; // 滚动偏移项数 (0 ~ count - 5)
    bool m_isClearBtnHovered = false;
    bool m_isTrackingMouse = false;
    bool m_caretVisible = true;

    // D2D 自绘右键菜单状态
    bool m_isContextMenuVisible = false;
    int m_contextMenuTargetIndex = -1;
    int m_contextMenuHoveredIndex = -1;
    D2D1_RECT_F m_contextMenuRect = {};
    std::vector<ContextMenuItem> m_contextMenuItems;

    std::function<void()> m_openSettingsCallback;

    // Direct2D / DirectWrite 核心接口（在隐藏窗口时完整释放以清空字形图集与隔离字体缓存）
    winrt::com_ptr<ID2D1Factory> m_d2dFactory;
    winrt::com_ptr<IDWriteFactory> m_dwriteFactory;

    winrt::com_ptr<ID2D1HwndRenderTarget> m_renderTarget;

    // 画刷缓存
    winrt::com_ptr<ID2D1SolidColorBrush> m_textPrimaryBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_textSecondaryBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_textPredictionBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_separatorBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_itemSelectedBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_itemHoverBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_accentBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_selectionBgBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_menuBgBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_menuBorderBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_menuShadowBrush;

    // 文本格式 (随 DPI 缩放或唤醒按需重建)
    winrt::com_ptr<IDWriteTextFormat> m_searchFormat;
    winrt::com_ptr<IDWriteTextFormat> m_itemNameFormat;
    winrt::com_ptr<IDWriteTextFormat> m_itemPathFormat;
    winrt::com_ptr<IDWriteTextFormat> m_itemGlyphFormat;
    winrt::com_ptr<IDWriteTextFormat> m_clearBtnFormat;
    winrt::com_ptr<IDWriteTextFormat> m_menuTextFormat;
    winrt::com_ptr<IDWriteTextFormat> m_menuGlyphFormat;
    winrt::com_ptr<IDWriteInlineObject> m_pathEllipsis;
    winrt::com_ptr<IDWriteTextLayout> m_searchTextLayout;

    // 可见列表项 (最多 5 项) 的预排版 TextLayout 缓存，彻底消灭 OnPaint 每帧重复创建临时 Layout
    struct VisibleItemLayout
    {
        winrt::com_ptr<IDWriteTextLayout> nameLayout;
        winrt::com_ptr<IDWriteTextLayout> pathLayout;
    };
    VisibleItemLayout m_visibleLayouts[5];
};
