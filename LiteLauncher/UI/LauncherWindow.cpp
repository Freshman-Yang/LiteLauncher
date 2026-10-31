#include "pch.h"
#include "LauncherWindow.h"
#include "Core/I18n.h"
#include "Utils/ProcessHelper.h"
#include "Assets/resource.h"
#include <imm.h>
#include <shellscalingapi.h>
#include <windowsx.h>

// 兼容旧版 Windows SDK 的 DWM 枚举定义
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

// 未公开但极其稳定的 Windows 10/11 毛玻璃 API 定义
enum ACCENT_STATE
{
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_INVALID_STATE = 5
};

struct ACCENT_POLICY
{
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor; // AABBGGRR
    DWORD AnimationId;
};

struct WINDOWCOMPOSITIONATTRIBDATA
{
    DWORD Attrib;
    PVOID pvData;
    SIZE_T cbData;
};

typedef BOOL(WINAPI *pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA *);

namespace
{
constexpr int kWindowWidthDip = 700;
constexpr int kSearchBarHeightDip = 80;
constexpr int kMaxExpandedHeightDip = 457; // 80 + 1 + 16 + 5 * 72
constexpr int kMaxVisibleItems = 5;
constexpr int kItemStepDip = 72; // 64 height + 4 top + 4 bottom

// D2D 现代右键菜单布局参数 (DIP)
constexpr float kMenuWidthDip = 232.0f;
constexpr float kMenuPaddingDip = 6.0f;
constexpr float kMenuItemHeightDip = 36.0f;

// 右键菜单命令 ID
constexpr UINT IDM_RUN_AS_ADMIN = 1001;
constexpr UINT IDM_OPEN_LOCATION = 1002;
constexpr UINT IDM_UNINSTALL_UWP = 1003;
} // namespace

LauncherWindow::LauncherWindow() = default;

LauncherWindow::~LauncherWindow()
{
    if (m_hEditFont)
    {
        DeleteObject(m_hEditFont);
        m_hEditFont = nullptr;
    }
    DiscardDeviceResources();
    DiscardDeviceIndependentResources();
}

void LauncherWindow::CreateDeviceIndependentResources()
{
    if (!m_d2dFactory)
    {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.put());
    }
    if (!m_dwriteFactory)
    {
        // 使用 DWRITE_FACTORY_TYPE_ISOLATED 将字体文件映射、字形表与排版缓存严格隔离在本实例内部，
        // 确保隐藏窗口释放 m_dwriteFactory 时能够 100% 释放进程内 DirectWrite 字体/字形缓存，绝不驻留全局单例
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(m_dwriteFactory.put()));
    }
}

void LauncherWindow::DiscardDeviceIndependentResources()
{
    ClearVisibleItemLayouts();
    m_searchTextLayout = nullptr;
    m_pathEllipsis = nullptr;
    m_searchFormat = nullptr;
    m_itemNameFormat = nullptr;
    m_itemPathFormat = nullptr;
    m_itemGlyphFormat = nullptr;
    m_clearBtnFormat = nullptr;
    m_menuTextFormat = nullptr;
    m_menuGlyphFormat = nullptr;
    m_dwriteFactory = nullptr;
    m_d2dFactory = nullptr;
}

bool LauncherWindow::Create(HINSTANCE hInstance)
{
    IconManager::Initialize();

    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = LauncherWindow::WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = nullptr;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = LAUNCHER_WINDOW_CLASS;

    // 直接从 EXE 内置资源加载图标
    wc.hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm =
        static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));

    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    DWORD style = WS_OVERLAPPED | WS_THICKFRAME | WS_CAPTION;

    // 预先获取当前鼠标所在显示器（回退主显示器）的 DPI 与工作区，一步到位计算窗口物理尺寸与位置
    POINT ptCursor = {0, 0};
    GetCursorPos(&ptCursor);
    HMONITOR hMonitor = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTOPRIMARY);

    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
    {
        dpiX = 96;
    }
    m_currentDpi = dpiX;
    m_dpiScale = m_currentDpi / 96.0f;

    int width = static_cast<int>(kWindowWidthDip * m_dpiScale);
    int initialHeight = static_cast<int>(kSearchBarHeightDip * m_dpiScale);
    int maxExpandedHeight = static_cast<int>(kMaxExpandedHeightDip * m_dpiScale);

    MONITORINFO monitorInfo = {sizeof(MONITORINFO)};
    GetMonitorInfoW(hMonitor, &monitorInfo);

    int screenWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    int screenHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;

    int posX = monitorInfo.rcWork.left + (screenWidth - width) / 2;
    int posY = monitorInfo.rcWork.top + (screenHeight - maxExpandedHeight) / 2;

    m_hwnd = CreateWindowExW(exStyle, LAUNCHER_WINDOW_CLASS, L"LiteLauncher", style, posX, posY, width,
                             initialHeight, nullptr, nullptr, hInstance, this);

    if (!m_hwnd)
        return false;

    // 强制应用 Windows 11 标准 8px 圆角
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));

    // 触发一次 SWP_FRAMECHANGED 使 WM_NCCALCSIZE 无边框客户区立即生效（不改变位置和大小，不触发多余 WM_SIZE）
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // 创建原生 Win32 EDIT 子控件（用于支持中文输入法 IME 候选框跟随、光标与快捷键操作）
    m_hEdit = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 100,
                              40, m_hwnd, reinterpret_cast<HMENU>(101), hInstance, nullptr);

    SetLayeredWindowAttributes(m_hEdit, 0, 0, LWA_ALPHA);

    SendMessageW(m_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(0, 0));
    SetWindowSubclass(m_hEdit, EditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    // 初始化 DPI 字体、主题色及背景材质
    UpdateDpiResources();
    UpdateTheme();
    ApplyBackdrop(ConfigManager::GetBackdrop());
    UpdateSearchBoxLayout();

    // 注册全局快捷键 Alt + Space，启用 MOD_NOREPEAT 防止长按热键消息轰炸导致渲染震荡
    if (!RegisterHotKey(m_hwnd, 1, MOD_ALT | MOD_NOREPEAT, VK_SPACE))
    {
        MessageBoxW(nullptr, I18n::HotkeyFailMsg(), I18n::HotkeyFailTitle(), MB_ICONWARNING | MB_OK);
    }

    SearchManager::RequestRescanAsync();

    return true;
}

void LauncherWindow::UpdateDpiResources()
{
    if (!m_hwnd)
        return;
    if (m_currentDpi == 0)
    {
        m_currentDpi = GetDpiForWindow(m_hwnd);
    }
    m_dpiScale = m_currentDpi / 96.0f;

    const AppLanguage currentLang = ConfigManager::GetLanguage();
    const wchar_t *uiFontName = I18n::GetUiFontName(currentLang);
    const wchar_t *localeName = I18n::GetLocaleName(currentLang);

    // 当原生 EDIT 字体尚未创建、DPI 变化或界面语言变化时重建 GDI HFONT
    if (!m_hEditFont || m_fontDpi != m_currentDpi || m_fontLang != currentLang)
    {
        if (m_hEditFont)
            DeleteObject(m_hEditFont);
        int fontHeight = -static_cast<int>(28.0f * m_dpiScale + 0.5f);
        m_hEditFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                  uiFontName);
        m_fontDpi = m_currentDpi;
        m_fontLang = currentLang;
        m_cachedEditHeight = static_cast<int>(38.0f * m_dpiScale);
        if (m_hEdit)
        {
            SendMessageW(m_hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hEditFont), TRUE);
            SendMessageW(m_hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(0, 0));
            if (m_hEditFont)
            {
                HDC hdc = GetDC(m_hEdit);
                if (hdc)
                {
                    HGDIOBJ oldFont = SelectObject(hdc, m_hEditFont);
                    TEXTMETRICW tm{};
                    if (GetTextMetricsW(hdc, &tm))
                    {
                        m_cachedEditHeight = tm.tmHeight;
                    }
                    SelectObject(hdc, oldFont);
                    ReleaseDC(m_hEdit, hdc);
                }
            }
        }
    }

    CreateDeviceIndependentResources();
    if (!m_dwriteFactory)
        return;

    ClearVisibleItemLayouts();
    m_searchTextLayout = nullptr;
    m_searchFormat = nullptr;
    m_itemNameFormat = nullptr;
    m_itemPathFormat = nullptr;
    m_pathEllipsis = nullptr;

    m_dwriteFactory->CreateTextFormat(uiFontName, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 28.0f * m_dpiScale, localeName, m_searchFormat.put());
    if (m_searchFormat)
    {
        m_searchFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_searchFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    m_dwriteFactory->CreateTextFormat(uiFontName, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 16.0f * m_dpiScale, localeName,
                                      m_itemNameFormat.put());
    if (m_itemNameFormat)
    {
        m_itemNameFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_itemNameFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    m_dwriteFactory->CreateTextFormat(uiFontName, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 12.0f * m_dpiScale, localeName,
                                      m_itemPathFormat.put());
    if (m_itemPathFormat)
    {
        m_itemPathFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_itemPathFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        m_dwriteFactory->CreateEllipsisTrimmingSign(m_itemPathFormat.get(), m_pathEllipsis.put());
        m_itemPathFormat->SetTrimming(&trimming, m_pathEllipsis.get());
    }

    // 重建图标字体格式 (自动检测 Win11 Segoe Fluent Icons / Win10 Segoe MDL2 Assets)
    m_itemGlyphFormat = nullptr;
    m_clearBtnFormat = nullptr;

    const wchar_t *iconFontName = L"Segoe MDL2 Assets";
    winrt::com_ptr<IDWriteFontCollection> fontCollection;
    if (SUCCEEDED(m_dwriteFactory->GetSystemFontCollection(fontCollection.put())))
    {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fontCollection->FindFamilyName(L"Segoe Fluent Icons", &index, &exists)) && exists)
        {
            iconFontName = L"Segoe Fluent Icons";
        }
    }

    // 列表项图标字体 (24 DIP，水平+垂直居中对齐)
    m_dwriteFactory->CreateTextFormat(iconFontName, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 24.0f * m_dpiScale, localeName,
                                      m_itemGlyphFormat.put());
    if (m_itemGlyphFormat)
    {
        m_itemGlyphFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_itemGlyphFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // 搜索框右侧清空按钮 'X' 图标字体 (12 DIP，水平+垂直居中对齐)
    m_dwriteFactory->CreateTextFormat(iconFontName, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 12.0f * m_dpiScale, localeName,
                                      m_clearBtnFormat.put());
    if (m_clearBtnFormat)
    {
        m_clearBtnFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_clearBtnFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // 重建右键菜单文本与图标字体格式
    m_menuTextFormat = nullptr;
    m_menuGlyphFormat = nullptr;

    m_dwriteFactory->CreateTextFormat(uiFontName, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 14.0f * m_dpiScale, localeName,
                                      m_menuTextFormat.put());
    if (m_menuTextFormat)
    {
        m_menuTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_menuTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    m_dwriteFactory->CreateTextFormat(iconFontName, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 15.0f * m_dpiScale, localeName,
                                      m_menuGlyphFormat.put());
    if (m_menuGlyphFormat)
    {
        m_menuGlyphFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_menuGlyphFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    RebuildSearchTextLayout();
    RebuildVisibleItemLayouts();
}

void LauncherWindow::ClearVisibleItemLayouts()
{
    for (auto &slot : m_visibleLayouts)
    {
        slot.nameLayout = nullptr;
        slot.pathLayout = nullptr;
    }
}

void LauncherWindow::RebuildVisibleItemLayouts()
{
    ClearVisibleItemLayouts();
    if (!m_dwriteFactory || !m_itemNameFormat || !m_itemPathFormat)
        return;
    if (m_results.empty() || m_currentQuery.empty())
        return;

    float s = m_dpiScale;
    float winW = kWindowWidthDip * s;
    float itemRight = winW - 12.0f * s;
    float textLeft = 76.0f * s;
    float textRight = itemRight - 12.0f * s;
    float maxTextW = (std::max)(10.0f, textRight - textLeft);
    float nameH = 26.0f * s;
    float pathH = 20.0f * s;

    int totalCount = static_cast<int>(m_results.size());
    int visibleCount = (std::min)(totalCount, kMaxVisibleItems);

    for (int i = 0; i < visibleCount; ++i)
    {
        int itemIdx = m_scrollOffset + i;
        if (itemIdx < 0 || itemIdx >= totalCount)
            break;

        const auto &item = m_results[itemIdx];
        if (!item.Name.empty())
        {
            m_dwriteFactory->CreateTextLayout(item.Name.c_str(), static_cast<UINT32>(item.Name.length()),
                                              m_itemNameFormat.get(), maxTextW, nameH,
                                              m_visibleLayouts[i].nameLayout.put());
        }
        if (!item.ActionPath.empty())
        {
            m_dwriteFactory->CreateTextLayout(item.ActionPath.c_str(), static_cast<UINT32>(item.ActionPath.length()),
                                              m_itemPathFormat.get(), maxTextW, pathH,
                                              m_visibleLayouts[i].pathLayout.put());
        }
    }
}

void LauncherWindow::RebuildSearchTextLayout()
{
    m_searchTextLayout = nullptr;
    if (!m_dwriteFactory || !m_searchFormat)
        return;
    if (m_currentQuery.empty() && m_imeCompString.empty())
        return;

    float s = m_dpiScale;
    float winW = kWindowWidthDip * s;
    float searchBarH = kSearchBarHeightDip * s;
    float textLeft = 72.0f * s;
    float textRight = winW - (m_currentQuery.empty() ? 24.0f : 64.0f) * s;
    float maxTextW = (std::max)(10.0f, textRight - textLeft);

    // 将正在组字的拼音串插入到当前光标位置，或将行内预测词拼接到末尾，交由单个 TextLayout 统一排版
    std::wstring displayStr = m_currentQuery;
    DWORD selStart = 0, selEnd = 0;
    if (m_hEdit)
    {
        SendMessageW(m_hEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
    }
    UINT32 insertPos = (std::min)(static_cast<UINT32>(selEnd), static_cast<UINT32>(displayStr.length()));
    UINT32 queryAndImeLen = static_cast<UINT32>(displayStr.length());

    if (!m_imeCompString.empty())
    {
        displayStr.insert(insertPos, m_imeCompString);
        queryAndImeLen = static_cast<UINT32>(displayStr.length());
    }
    else if (!m_predictionSuffix.empty() && !m_currentQuery.empty())
    {
        displayStr.append(m_predictionSuffix);
    }

    m_dwriteFactory->CreateTextLayout(displayStr.c_str(), static_cast<UINT32>(displayStr.length()),
                                      m_searchFormat.get(), maxTextW, searchBarH, m_searchTextLayout.put());

    if (m_searchTextLayout)
    {
        // 给正在输入的拼音加上下划线指示
        if (!m_imeCompString.empty())
        {
            DWRITE_TEXT_RANGE compRange = {insertPos, static_cast<UINT32>(m_imeCompString.length())};
            m_searchTextLayout->SetUnderline(TRUE, compRange);
        }
        // 给预测词后缀区间单独绑定半透明预测词画刷，实现零临时分配的一体化绘制
        else if (!m_predictionSuffix.empty() && !m_currentQuery.empty() && m_textPredictionBrush)
        {
            DWRITE_TEXT_RANGE predRange = {queryAndImeLen, static_cast<UINT32>(m_predictionSuffix.length())};
            m_searchTextLayout->SetDrawingEffect(m_textPredictionBrush.get(), predRange);
        }
    }
}

void LauncherWindow::UpdateSearchBoxLayout()
{
    if (!m_hwnd || !m_hEdit)
        return;

    int editLeft = static_cast<int>(72.0f * m_dpiScale);
    int editRightMargin = static_cast<int>((m_currentQuery.empty() ? 24.0f : 64.0f) * m_dpiScale);
    int maxEditWidth = static_cast<int>(kWindowWidthDip * m_dpiScale) - editLeft - editRightMargin;

    int editHeight = m_cachedEditHeight > 0 ? m_cachedEditHeight : static_cast<int>(38.0f * m_dpiScale);
    int editTop = (static_cast<int>(kSearchBarHeightDip * m_dpiScale) - editHeight) / 2;
    SetWindowPos(m_hEdit, nullptr, editLeft, editTop, maxEditWidth, editHeight, SWP_NOZORDER | SWP_NOACTIVATE);

    RebuildSearchTextLayout();
}

void LauncherWindow::UpdateTheme()
{
    m_isDarkMode = IsSystemAppsDarkMode();

    if (m_hwnd)
    {
        BOOL useDarkMode = m_isDarkMode ? TRUE : FALSE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
    }

    // 若 D2D RenderTarget 已存在，同步更新所有画刷颜色
    if (m_renderTarget)
    {
        if (m_isDarkMode)
        {
            m_textPrimaryBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
            m_textSecondaryBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f));
            m_textPredictionBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f));
            m_separatorBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
            m_itemSelectedBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f));
            m_itemHoverBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f));
            m_accentBrush->SetColor(D2D1::ColorF(0.38f, 0.80f, 1.0f, 1.0f));
            m_selectionBgBrush->SetColor(D2D1::ColorF(0.38f, 0.80f, 1.0f, 0.35f));
            m_menuBgBrush->SetColor(D2D1::ColorF(0.17f, 0.17f, 0.19f, 0.96f));
            m_menuBorderBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
            m_menuShadowBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f));
        }
        else
        {
            m_textPrimaryBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.90f));
            m_textSecondaryBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f));
            m_textPredictionBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f));
            m_separatorBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f));
            m_itemSelectedBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f));
            m_itemHoverBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.03f));
            m_accentBrush->SetColor(D2D1::ColorF(0.0f, 0.40f, 0.75f, 1.0f));
            m_selectionBgBrush->SetColor(D2D1::ColorF(0.0f, 0.40f, 0.75f, 0.35f));
            m_menuBgBrush->SetColor(D2D1::ColorF(0.98f, 0.98f, 0.99f, 0.97f));
            m_menuBorderBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f));
            m_menuShadowBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f));
        }
    }

    if (m_hEdit)
        InvalidateRect(m_hEdit, nullptr, TRUE);
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void LauncherWindow::ApplyBackdrop(BackdropType type)
{
    if (!m_hwnd)
        return;

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    auto setWca =
        reinterpret_cast<pfnSetWindowCompositionAttribute>(GetProcAddress(hUser32, "SetWindowCompositionAttribute"));

    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    if (type == BackdropType::PureBlur)
    {
        // 关闭 Win11 原生系统材质
        int backdropNone = 1; // DWMSBT_NONE
        DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropNone, sizeof(backdropNone));

        // 开启 DWM 透明通道
        HRGN hRgn = CreateRectRgn(0, 0, -1, -1);
        DWM_BLURBEHIND bb = {0};
        bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
        bb.fEnable = TRUE;
        bb.hRgnBlur = hRgn;
        DwmEnableBlurBehindWindow(m_hwnd, &bb);
        DeleteObject(hRgn);

        // 纯粹无色模糊：State = 3, Color = 0
        if (setWca)
        {
            ACCENT_POLICY policy = {};
            policy.AccentState = ACCENT_ENABLE_BLURBEHIND;
            policy.AccentFlags = 0;
            policy.GradientColor = 0x00000000;
            WINDOWCOMPOSITIONATTRIBDATA data = {19, &policy, sizeof(policy)};
            setWca(m_hwnd, &data);
        }
    }
    else
    {
        // 关闭 BlurBehind 和 WCA 模糊，把控制权交还给 Win11 DWM
        DWM_BLURBEHIND bb = {0};
        bb.dwFlags = DWM_BB_ENABLE;
        bb.fEnable = FALSE;
        DwmEnableBlurBehindWindow(m_hwnd, &bb);

        if (setWca)
        {
            ACCENT_POLICY policy = {};
            policy.AccentState = ACCENT_DISABLED;
            WINDOWCOMPOSITIONATTRIBDATA data = {19, &policy, sizeof(policy)};
            setWca(m_hwnd, &data);
        }

        // 直接应用 Win11 原生材质：3 = Acrylic, 2 = Mica, 4 = MicaAlt
        int dwmBackdrop = 2; // 默认 Mica (DWMSBT_MAINWINDOW)
        if (type == BackdropType::Acrylic)
            dwmBackdrop = 3; // DWMSBT_TRANSIENTWINDOW
        else if (type == BackdropType::MicaAlt)
            dwmBackdrop = 4; // DWMSBT_TABBEDWINDOW

        DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &dwmBackdrop, sizeof(dwmBackdrop));
    }

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void LauncherWindow::OnLanguageChanged()
{
    if (!m_hwnd)
        return;

    CloseContextMenu();
    UpdateDpiResources();
    UpdateSearchBoxLayout();
    SearchManager::RequestRescanAsync(m_hwnd, WM_SEARCH_REFRESHED);

    if (IsWindowVisible(m_hwnd))
    {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void LauncherWindow::CreateDeviceResources()
{
    if (m_renderTarget || !m_hwnd)
        return;

    CreateDeviceIndependentResources();
    if (!m_d2dFactory)
        return;

    if (!m_searchFormat)
    {
        UpdateDpiResources();
    }

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    // 启用 Premultiplied Alpha 透明混合，确保 DWM 毛玻璃完美透出
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f // 统一使用物理像素坐标 (DIP * m_dpiScale)
    );

    HRESULT hr = m_d2dFactory->CreateHwndRenderTarget(rtProps, D2D1::HwndRenderTargetProperties(m_hwnd, size),
                                                      m_renderTarget.put());

    if (SUCCEEDED(hr))
    {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), m_textPrimaryBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.65f), m_textSecondaryBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.35f), m_textPredictionBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.08f), m_separatorBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.09f), m_itemSelectedBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.05f), m_itemHoverBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.80f, 1.0f, 1.0f), m_accentBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.80f, 1.0f, 0.35f), m_selectionBgBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.17f, 0.17f, 0.19f, 0.96f), m_menuBgBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.14f), m_menuBorderBrush.put());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.28f), m_menuShadowBrush.put());

        UpdateTheme();

        // RenderTarget 重建后刷新一次 Layout 以确保预测词 DrawingEffect 绑定最新画刷
        RebuildSearchTextLayout();
        RebuildVisibleItemLayouts();
    }
}

void LauncherWindow::DiscardDeviceResources()
{
    IconManager::OnRenderTargetDestroyed();
    ClearVisibleItemLayouts();
    m_searchTextLayout = nullptr;
    m_renderTarget = nullptr;
    m_textPrimaryBrush = nullptr;
    m_textSecondaryBrush = nullptr;
    m_textPredictionBrush = nullptr;
    m_separatorBrush = nullptr;
    m_itemSelectedBrush = nullptr;
    m_itemHoverBrush = nullptr;
    m_accentBrush = nullptr;
    m_selectionBgBrush = nullptr;
    m_menuBgBrush = nullptr;
    m_menuBorderBrush = nullptr;
    m_menuShadowBrush = nullptr;
}

void LauncherWindow::OnPaint(const RECT &rcPaint)
{
    if (!IsWindowVisible(m_hwnd))
        return;

    CreateDeviceResources();
    if (!m_renderTarget)
        return;

    m_renderTarget->BeginDraw();

    // 将物理像素的脏矩形转换为 D2D 浮点矩形并推入轴对齐裁剪区
    // 注：在 PushAxisAlignedClip 之后调用 Clear(0,0,0,0) 只会清除 clipRect 范围内的像素，
    // 绝不会破坏剪裁区外（如下方搜索结果列表）的已有像素，同时确保局部重绘区完美透出底层 DWM 毛玻璃材质。
    D2D1_RECT_F clipRect = D2D1::RectF(static_cast<float>(rcPaint.left), static_cast<float>(rcPaint.top),
                                       static_cast<float>(rcPaint.right), static_cast<float>(rcPaint.bottom));
    m_renderTarget->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // D2D 裁剪区内全透明清屏
    m_renderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    float s = m_dpiScale;
    float winW = kWindowWidthDip * s;
    float searchBarH = kSearchBarHeightDip * s;

    // 只有当脏矩形涉及顶部搜索栏区域 (clipRect.top < searchBarH) 时才执行搜索栏渲染与 DrawCall 提交
    if (clipRect.top < searchBarH)
    {
        // 绘制顶部左侧 32x32 AppIcon.ico (Margin: 24, 0, 16, 0)，直接共享 IconManager 的内置应用图标纹理
        D2D1_RECT_F appIconRect =
            D2D1::RectF(24.0f * s, (searchBarH - 32.0f * s) * 0.5f, (24.0f + 32.0f) * s, (searchBarH + 32.0f * s) * 0.5f);
        if (ID2D1Bitmap *pAppIcon = IconManager::GetCommandD2DBitmap(m_renderTarget.get()))
        {
            m_renderTarget->DrawBitmap(pAppIcon, appIconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        // 纯 Direct2D 绘制搜索框文本、行内预测词、选中高亮与光标（零临时 Layout 分配）
        if (m_dwriteFactory && m_searchFormat)
        {
            float textLeft = 72.0f * s;
            float textRight = winW - (m_currentQuery.empty() ? 24.0f : 64.0f) * s;
            float maxTextW = (std::max)(10.0f, textRight - textLeft);

            // 复用缓存的 m_searchTextLayout 一次性测量并绘制已输入文本、行内预测词、选区背景与光标
            if (!m_searchTextLayout && (!m_currentQuery.empty() || !m_imeCompString.empty()))
            {
                RebuildSearchTextLayout();
            }
            if (m_searchTextLayout)
            {
                DWORD selStart = 0, selEnd = 0;
                if (m_hEdit)
                {
                    SendMessageW(m_hEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart),
                                 reinterpret_cast<LPARAM>(&selEnd));
                }

                // 若有全选/部分选中文本，绘制选区背景（使用独立画刷，避免修改主强调色画刷透明度）
                if (selStart != selEnd && !m_currentQuery.empty() && m_imeCompString.empty())
                {
                    DWORD sPos = (std::min)(selStart, selEnd);
                    DWORD ePos = (std::max)(selStart, selEnd);
                    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                    DWRITE_HIT_TEST_METRICS htm1{}, htm2{};
                    m_searchTextLayout->HitTestTextPosition(sPos, FALSE, &x1, &y1, &htm1);
                    m_searchTextLayout->HitTestTextPosition(ePos, FALSE, &x2, &y2, &htm2);

                    D2D1_RECT_F selRect =
                        D2D1::RectF(textLeft + x1, (searchBarH - htm1.height) * 0.5f, textLeft + (std::min)(x2, maxTextW),
                                    (searchBarH + htm1.height) * 0.5f);
                    if (m_selectionBgBrush)
                    {
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(selRect, 4.0f * s, 4.0f * s),
                                                             m_selectionBgBrush.get());
                    }
                }

                // 绘制已输入的文字（包含正在组字的拼音，以及通过 SetDrawingEffect 着色的行内预测词）
                if (!m_currentQuery.empty() || !m_imeCompString.empty())
                {
                    m_renderTarget->DrawTextLayout(D2D1::Point2F(textLeft, 0.0f), m_searchTextLayout.get(),
                                                   m_textPrimaryBrush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }

                // 绘制闪烁的输入光标（加上 m_imeCursorPos 偏移，使光标准确停在拼音右侧！）
                if (m_caretVisible && GetFocus() == m_hEdit)
                {
                    float caretX = 0, caretY = 0;
                    DWRITE_HIT_TEST_METRICS htm{};
                    UINT32 caretCharPos = selEnd + static_cast<UINT32>(m_imeCursorPos);
                    m_searchTextLayout->HitTestTextPosition(caretCharPos, FALSE, &caretX, &caretY, &htm);
                    float caretH = htm.height > 0 ? htm.height : 32.0f * s;
                    float caretTop = (searchBarH - caretH) * 0.5f;
                    float cx = textLeft + (std::min)(caretX, maxTextW);

                    m_renderTarget->DrawLine(D2D1::Point2F(cx, caretTop), D2D1::Point2F(cx, caretTop + caretH),
                                             m_textPrimaryBrush.get(), 1.8f * s);
                }
            }
            else if (m_caretVisible && GetFocus() == m_hEdit)
            {
                // 空文本且无拼音组字时，无需实例化 IDWriteTextLayout，直接根据字号计算默认光标高度并绘制
                float fontSize = m_searchFormat->GetFontSize();
                float caretH = (fontSize > 0.0f) ? (fontSize * (32.0f / 28.0f)) : (32.0f * s);
                float caretTop = (searchBarH - caretH) * 0.5f;

                m_renderTarget->DrawLine(D2D1::Point2F(textLeft, caretTop), D2D1::Point2F(textLeft, caretTop + caretH),
                                         m_textPrimaryBrush.get(), 1.8f * s);
            }
        }

        // 使用图标字库绘制清空 'X' 按钮 (0xE711)
        if (!m_currentQuery.empty())
        {
            float btnRight = winW - 20.0f * s;
            float btnLeft = btnRight - 36.0f * s;
            float btnTop = (searchBarH - 36.0f * s) * 0.5f;
            float btnBottom = btnTop + 36.0f * s;
            D2D1_RECT_F btnRect = D2D1::RectF(btnLeft, btnTop, btnRight, btnBottom);

            if (m_isClearBtnHovered)
            {
                float centerX = (btnLeft + btnRight) * 0.5f;
                float centerY = (btnTop + btnBottom) * 0.5f;
                D2D1_ROUNDED_RECT hoverCircle = D2D1::RoundedRect(
                    D2D1::RectF(centerX - 14.0f * s, centerY - 14.0f * s, centerX + 14.0f * s, centerY + 14.0f * s),
                    14.0f * s, 14.0f * s);
                m_renderTarget->FillRoundedRectangle(hoverCircle, m_itemHoverBrush.get());
            }

            if (m_clearBtnFormat)
            {
                const wchar_t clearGlyph = 0xE711; // Cancel 'X' 图标码点
                m_renderTarget->DrawText(&clearGlyph, 1, m_clearBtnFormat.get(), btnRect, m_textSecondaryBrush.get());
            }
        }
    }

    // 绘制分割线与搜索结果列表
    // 只有当脏矩形涉及分割线及以下区域 (clipRect.bottom > searchBarH) 时才执行列表渲染与 DrawCall 提交
    if (!m_results.empty() && !m_currentQuery.empty() && clipRect.bottom > searchBarH)
    {
        // 1px 分割线 (Margin: 24, 0, 24, 0)
        D2D1_RECT_F sepRect = D2D1::RectF(24.0f * s, searchBarH, winW - 24.0f * s, searchBarH + 1.0f * s);
        m_renderTarget->FillRectangle(sepRect, m_separatorBrush.get());

        // 列表起始 Y 坐标: 80(搜索栏) + 1(分割线) + 4(ListView Top Margin) = 85 DIP
        float listStartY = 85.0f * s;
        int totalCount = static_cast<int>(m_results.size());
        int visibleCount = (std::min)(totalCount, kMaxVisibleItems);

        for (int i = 0; i < visibleCount; ++i)
        {
            int itemIdx = m_scrollOffset + i;
            if (itemIdx >= totalCount)
                break;

            const auto &item = m_results[itemIdx];

            // 每项步进 72 DIP (上下外边距各 4 DIP，实际高亮框高 64 DIP，左右外边距 12 DIP)
            float slotTop = listStartY + (i * kItemStepDip) * s;
            float itemTop = slotTop + 4.0f * s;
            float itemBottom = itemTop + 64.0f * s;
            float itemLeft = 12.0f * s;
            float itemRight = winW - 12.0f * s;

            D2D1_RECT_F itemRect = D2D1::RectF(itemLeft, itemTop, itemRight, itemBottom);
            D2D1_ROUNDED_RECT roundedItem = D2D1::RoundedRect(itemRect, 6.0f * s, 6.0f * s);

            // 绘制选中态 / 悬停态背景圆角矩形
            if (itemIdx == m_selectedIndex)
            {
                m_renderTarget->FillRoundedRectangle(roundedItem, m_itemSelectedBrush.get());

                // 绘制 Win11 风格左侧 3px 垂直高亮指示条
                D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                    D2D1::RectF(itemLeft, itemTop + 18.0f * s, itemLeft + 3.0f * s, itemBottom - 18.0f * s), 1.5f * s,
                    1.5f * s);
                m_renderTarget->FillRoundedRectangle(pill, m_accentBrush.get());
            }
            else if (itemIdx == m_hoveredIndex)
            {
                m_renderTarget->FillRoundedRectangle(roundedItem, m_itemHoverBrush.get());
            }

            // 绘制左侧 36x36 应用图标 (ItemLeft 12 + PaddingLeft 8 + IconMarginLeft 4 = 24 DIP)
            float iconLeft = 24.0f * s;
            float iconTop = itemTop + (64.0f - 36.0f) * 0.5f * s;
            D2D1_RECT_F iconRect = D2D1::RectF(iconLeft, iconTop, iconLeft + 36.0f * s, iconTop + 36.0f * s);

            if (ID2D1Bitmap *pIcon = IconManager::GetIcon(item, m_renderTarget.get(), m_hwnd))
            {
                m_renderTarget->DrawBitmap(pIcon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            else if (m_itemGlyphFormat)
            {
                wchar_t glyph = 0;
                if (item.Type == ItemType::Win32)
                {
                    glyph = 0xE756; // CommandPrompt 可执行程序兜底图标
                }
                else if (item.Type == ItemType::UWP)
                {
                    glyph = 0xE71D; // AllApps 现代商店应用兜底图标
                }
                else if (item.Type == ItemType::Settings)
                {
                    glyph = (!item.IconPath || item.IconPath[0] == L'\0') ? static_cast<wchar_t>(item.IconId)
                                                                          : static_cast<wchar_t>(0xE713);
                    if (glyph == 0)
                        glyph = 0xE713;
                }

                if (glyph != 0)
                {
                    m_renderTarget->DrawText(&glyph, 1, m_itemGlyphFormat.get(), iconRect, m_textPrimaryBrush.get());
                }
            }

            // 绘制右侧两行文本 (IconRight 60 + IconMarginRight 16 = 76 DIP)，直接复用预排版的 m_visibleLayouts 彻底消除每帧临时 Layout 分配
            float textLeft = 76.0f * s;
            if (!m_visibleLayouts[i].nameLayout && !item.Name.empty())
            {
                RebuildVisibleItemLayouts();
            }
            if (m_visibleLayouts[i].nameLayout)
            {
                m_renderTarget->DrawTextLayout(D2D1::Point2F(textLeft, itemTop + 10.0f * s),
                                               m_visibleLayouts[i].nameLayout.get(), m_textPrimaryBrush.get(),
                                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (m_visibleLayouts[i].pathLayout)
            {
                m_renderTarget->DrawTextLayout(D2D1::Point2F(textLeft, itemTop + 36.0f * s),
                                               m_visibleLayouts[i].pathLayout.get(), m_textSecondaryBrush.get(),
                                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        // 若总结果数超过 5 项，在右侧边缘绘制轻量级滚动指示器
        if (totalCount > kMaxVisibleItems)
        {
            float trackTop = listStartY + 4.0f * s;
            float trackHeight = (kMaxVisibleItems * kItemStepDip - 8.0f) * s;
            float thumbHeight = trackHeight * (static_cast<float>(kMaxVisibleItems) / totalCount);
            float maxScroll = static_cast<float>(totalCount - kMaxVisibleItems);
            float thumbTop = trackTop + (trackHeight - thumbHeight) * (m_scrollOffset / maxScroll);

            D2D1_ROUNDED_RECT scrollThumb = D2D1::RoundedRect(
                D2D1::RectF(winW - 6.0f * s, thumbTop, winW - 3.0f * s, thumbTop + thumbHeight), 1.5f * s, 1.5f * s);
            m_renderTarget->FillRoundedRectangle(scrollThumb, m_textPredictionBrush.get());
        }

        // 绘制顶层 Fluent 风格自绘右键菜单浮层
        if (m_isContextMenuVisible && !m_contextMenuItems.empty() && m_menuBgBrush && m_menuBorderBrush)
        {
            // 1. 绘制单层克制的 Fluent 底部环境微投影（避免多层半透明圆角矩形叠加产生同心圆 Banding 阶梯感）
            if (m_menuShadowBrush)
            {
                D2D1_RECT_F shadowRect = D2D1::RectF(
                    m_contextMenuRect.left - 1.0f * s, m_contextMenuRect.top + 3.0f * s,
                    m_contextMenuRect.right + 1.0f * s, m_contextMenuRect.bottom + 4.0f * s);
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(shadowRect, 9.0f * s, 9.0f * s),
                    m_menuShadowBrush.get());
            }

            // 2. 绘制菜单圆角背景板与 1px 高光描边
            D2D1_ROUNDED_RECT menuBox = D2D1::RoundedRect(m_contextMenuRect, 8.0f * s, 8.0f * s);
            m_renderTarget->FillRoundedRectangle(menuBox, m_menuBgBrush.get());
            m_renderTarget->DrawRoundedRectangle(menuBox, m_menuBorderBrush.get(), 1.0f * s);

            // 3. 绘制每个菜单项（悬停圆角高亮 + Fluent 图标 + 文本）
            float pad = kMenuPaddingDip * s;
            float itemH = kMenuItemHeightDip * s;

            for (size_t i = 0; i < m_contextMenuItems.size(); ++i)
            {
                const auto &menuItem = m_contextMenuItems[i];
                float mTop = m_contextMenuRect.top + pad + i * itemH;
                float mBottom = mTop + itemH;
                D2D1_RECT_F mItemRect =
                    D2D1::RectF(m_contextMenuRect.left + pad, mTop, m_contextMenuRect.right - pad, mBottom);

                if (static_cast<int>(i) == m_contextMenuHoveredIndex)
                {
                    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(mItemRect, 5.0f * s, 5.0f * s),
                                                         m_itemSelectedBrush.get());
                }

                // 左侧图标区域 (Width: 32 DIP)
                D2D1_RECT_F glyphRect = D2D1::RectF(mItemRect.left + 4.0f * s, mTop, mItemRect.left + 34.0f * s, mBottom);
                if (m_menuGlyphFormat)
                {
                    m_renderTarget->DrawText(&menuItem.glyph, 1, m_menuGlyphFormat.get(), glyphRect,
                                             m_textPrimaryBrush.get());
                }

                // 右侧文字区域
                D2D1_RECT_F labelRect = D2D1::RectF(mItemRect.left + 38.0f * s, mTop, mItemRect.right - 8.0f * s, mBottom);
                if (m_menuTextFormat)
                {
                    m_renderTarget->DrawText(menuItem.text.c_str(), static_cast<UINT32>(menuItem.text.length()),
                                             m_menuTextFormat.get(), labelRect, m_textPrimaryBrush.get(),
                                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    }

    // 弹出裁剪栈并提交渲染
    m_renderTarget->PopAxisAlignedClip();
    HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        DiscardDeviceResources();
    }
}

void LauncherWindow::ShowContextMenu(int index, POINT ptClient)
{
    if (index < 0 || index >= static_cast<int>(m_results.size()))
        return;

    const ItemType itemType = m_results[index].Type;
    m_contextMenuItems.clear();

    if (itemType == ItemType::Win32)
    {
        // 0xE7EF: Admin 盾牌图标, 0xE8DA: OpenFolder 打开文件夹图标
        m_contextMenuItems.push_back({IDM_RUN_AS_ADMIN, static_cast<wchar_t>(0xE7EF), I18n::MenuRunAsAdmin()});
        m_contextMenuItems.push_back({IDM_OPEN_LOCATION, static_cast<wchar_t>(0xE8DA), I18n::MenuOpenLocation()});
    }
    else if (itemType == ItemType::UWP)
    {
        // 0xE74D: Delete 卸载图标
        m_contextMenuItems.push_back({IDM_UNINSTALL_UWP, static_cast<wchar_t>(0xE74D), I18n::MenuUninstallUwp()});
    }
    else
    {
        CloseContextMenu();
        return;
    }

    float s = m_dpiScale;
    float menuW = kMenuWidthDip * s;
    float menuH = (kMenuPaddingDip * 2.0f + kMenuItemHeightDip * static_cast<float>(m_contextMenuItems.size())) * s;

    RECT rcClient = {};
    GetClientRect(m_hwnd, &rcClient);
    float clientW = static_cast<float>(rcClient.right - rcClient.left);
    float clientH = static_cast<float>(rcClient.bottom - rcClient.top);
    float minTop = (kSearchBarHeightDip + 4.0f) * s;

    float left = static_cast<float>(ptClient.x);
    float top = static_cast<float>(ptClient.y);

    // 自动边界防溢出修正，保证菜单及阴影完全处于主窗口客户区内
    if (left + menuW > clientW - 12.0f * s)
        left = clientW - menuW - 12.0f * s;
    if (left < 12.0f * s)
        left = 12.0f * s;
    if (top + menuH > clientH - 8.0f * s)
        top = clientH - menuH - 8.0f * s;
    if (top < minTop)
        top = minTop;

    m_contextMenuRect = D2D1::RectF(left, top, left + menuW, top + menuH);
    m_contextMenuTargetIndex = index;
    m_contextMenuHoveredIndex = 0; // 默认高亮第一项，方便直接按回车或方向键操作
    m_isContextMenuVisible = true;

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void LauncherWindow::CloseContextMenu()
{
    if (!m_isContextMenuVisible)
        return;
    m_isContextMenuVisible = false;
    m_contextMenuTargetIndex = -1;
    m_contextMenuHoveredIndex = -1;
    m_contextMenuItems.clear();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

int LauncherWindow::HitTestContextMenuItem(POINT ptClient) const
{
    if (!m_isContextMenuVisible || m_contextMenuItems.empty())
        return -1;

    float x = static_cast<float>(ptClient.x);
    float y = static_cast<float>(ptClient.y);

    if (x < m_contextMenuRect.left || x > m_contextMenuRect.right || y < m_contextMenuRect.top ||
        y > m_contextMenuRect.bottom)
    {
        return -1;
    }

    float s = m_dpiScale;
    float pad = kMenuPaddingDip * s;
    float itemH = kMenuItemHeightDip * s;

    if (y < m_contextMenuRect.top + pad || y >= m_contextMenuRect.bottom - pad)
        return -1;

    int idx = static_cast<int>((y - (m_contextMenuRect.top + pad)) / itemH);
    if (idx >= 0 && idx < static_cast<int>(m_contextMenuItems.size()))
    {
        return idx;
    }
    return -1;
}

void LauncherWindow::ExecuteContextMenuCommand(UINT cmdId)
{
    int index = m_contextMenuTargetIndex;
    CloseContextMenu();

    if (index < 0 || index >= static_cast<int>(m_results.size()))
        return;

    const std::wstring actionPath = m_results[index].ActionPath;

    if (cmdId == IDM_RUN_AS_ADMIN)
    {
        HideWindow();
        ProcessHelper::RunAsAdmin(actionPath);
    }
    else if (cmdId == IDM_OPEN_LOCATION)
    {
        HideWindow();
        ProcessHelper::OpenFileLocation(actionPath);
    }
    else if (cmdId == IDM_UNINSTALL_UWP)
    {
        HideWindow();
        HWND targetHwnd = m_hwnd;
        ProcessHelper::UninstallUwpByAumid(
            actionPath, [targetHwnd]() { SearchManager::RequestRescanAsync(targetHwnd, WM_SEARCH_REFRESHED); });
    }
}

void LauncherWindow::OnSearchTextChanged()
{
    CloseContextMenu();

    if (!m_hEdit)
        return;

    // 每次搜索词变动立即取消上一轮输入未完成的异步图标提取任务，防止打字过程中后台排队提取大量过期图标
    IconManager::CancelPendingTasks();

    int len = GetWindowTextLengthW(m_hEdit);
    if (len > 0)
    {
        // 直接复用 m_currentQuery 缓冲区，消除每次按键时的临时 wstring 堆分配
        m_currentQuery.resize(static_cast<size_t>(len));
        int actualLen = GetWindowTextW(m_hEdit, m_currentQuery.data(), len + 1);
        if (actualLen >= 0 && actualLen < len)
        {
            m_currentQuery.resize(static_cast<size_t>(actualLen));
        }
    }
    else
    {
        m_currentQuery.clear();
    }

    if (m_currentQuery.empty())
    {
        m_currentQuery.shrink_to_fit();
        m_results.clear();
        m_results.shrink_to_fit();
        m_predictionSuffix.clear();
        m_predictionSuffix.shrink_to_fit();
        m_contextMenuItems.clear();
        m_contextMenuItems.shrink_to_fit();
        ClearVisibleItemLayouts();
        m_searchTextLayout = nullptr;
        m_selectedIndex = -1;
        m_hoveredIndex = -1;
        m_scrollOffset = 0;

        // 窗口仍可见时仅清理动态图标缓存并收缩窗口高度，绝不在此处调用 DiscardDeviceResources()，
        // 避免数毫秒后 InvalidateRect 触发 WM_PAINT 立即全量重建 RenderTarget 造成 DXGI 表面反复分配抖动
        IconManager::ClearCache();
    }
    else
    {
        // 调用零堆分配复用接口，直接复用 m_results 及其内部字符串的容量
        SearchManager::Search(m_currentQuery, m_results);
        m_predictionSuffix.clear();

        if (!m_results.empty())
        {
            m_selectedIndex = 0;
            m_scrollOffset = 0;

            // 零拷贝、零堆分配：使用 CompareStringOrdinal 比较前缀
            const std::wstring &topMatch = m_results[0].Name;
            const int queryLen = static_cast<int>(m_currentQuery.length());

            if (topMatch.length() > m_currentQuery.length() &&
                CompareStringOrdinal(topMatch.data(), queryLen, m_currentQuery.data(), queryLen,
                                     TRUE /* 忽略大小写 */) == CSTR_EQUAL)
            {
                m_predictionSuffix.assign(topMatch.data() + queryLen, topMatch.length() - queryLen);
            }
        }
        else
        {
            m_selectedIndex = -1;
            m_scrollOffset = 0;
        }
        RebuildVisibleItemLayouts();
    }

    UpdateSearchBoxLayout();
    UpdateWindowSize();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void LauncherWindow::UpdateWindowSize()
{
    if (!m_hwnd)
        return;

    int width = static_cast<int>(kWindowWidthDip * m_dpiScale);
    int targetHeightDip = kSearchBarHeightDip;

    int count = static_cast<int>(m_results.size());
    if (count > 0 && !m_currentQuery.empty())
    {
        int visibleCount = (std::min)(count, kMaxVisibleItems);
        // 80(搜索栏) + 1(分割线) + 16(列表上下边距 4+12) + 72 * visibleCount
        targetHeightDip = 80 + 1 + 16 + (kItemStepDip * visibleCount);
    }

    int targetHeight = static_cast<int>(targetHeightDip * m_dpiScale);

    RECT rcWindow;
    GetWindowRect(m_hwnd, &rcWindow);
    if ((rcWindow.right - rcWindow.left) != width || (rcWindow.bottom - rcWindow.top) != targetHeight)
    {
        SetWindowPos(m_hwnd, nullptr, 0, 0, width, targetHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void LauncherWindow::EnsureSelectedVisible()
{
    if (m_selectedIndex < 0)
        return;
    int oldOffset = m_scrollOffset;
    if (m_selectedIndex < m_scrollOffset)
    {
        m_scrollOffset = m_selectedIndex;
    }
    else if (m_selectedIndex >= m_scrollOffset + kMaxVisibleItems)
    {
        m_scrollOffset = m_selectedIndex - kMaxVisibleItems + 1;
    }
    if (m_scrollOffset != oldOffset)
    {
        RebuildVisibleItemLayouts();
    }
}

int LauncherWindow::HitTestItem(POINT ptClient) const
{
    if (m_results.empty() || m_currentQuery.empty())
        return -1;

    float s = m_dpiScale;
    float listStartY = 85.0f * s;
    float itemLeft = 12.0f * s;
    float itemRight = (kWindowWidthDip - 12.0f) * s;

    if (ptClient.x < itemLeft || ptClient.x > itemRight || ptClient.y < listStartY)
    {
        return -1;
    }

    float relY = ptClient.y - listStartY;
    float stepPx = kItemStepDip * s;
    int visibleSlot = static_cast<int>(relY / stepPx);
    if (visibleSlot < 0 || visibleSlot >= kMaxVisibleItems)
        return -1;

    // 剔除每个槽位上下各 4 DIP 的空白外边距，确保只有点击在 64 DIP 圆角卡片内部才算命中
    float offsetInSlot = relY - visibleSlot * stepPx;
    if (offsetInSlot < 4.0f * s || offsetInSlot > (kItemStepDip - 4.0f) * s)
    {
        return -1;
    }

    int actualIndex = m_scrollOffset + visibleSlot;
    if (actualIndex >= 0 && actualIndex < static_cast<int>(m_results.size()))
    {
        return actualIndex;
    }
    return -1;
}

bool LauncherWindow::HitTestClearButton(POINT ptClient) const
{
    if (m_currentQuery.empty())
        return false;

    float s = m_dpiScale;
    float winW = kWindowWidthDip * s;
    float searchBarH = kSearchBarHeightDip * s;

    float btnRight = winW - 20.0f * s;
    float btnLeft = btnRight - 36.0f * s;
    float btnTop = (searchBarH - 36.0f * s) * 0.5f;
    float btnBottom = btnTop + 36.0f * s;

    return (ptClient.x >= btnLeft && ptClient.x <= btnRight && ptClient.y >= btnTop && ptClient.y <= btnBottom);
}

void LauncherWindow::LaunchSelectedItem()
{
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_results.size()))
    {
        SearchResultItem selected = m_results[m_selectedIndex];
        ExecuteItem(selected);
    }
}

void LauncherWindow::ExecuteItem(const SearchResultItem &item)
{
    if (item.Type == ItemType::Command)
    {
        if (item.ActionPath == L"internal://settings")
        {
            HideWindow();
            if (m_openSettingsCallback)
            {
                m_openSettingsCallback();
            }
            return;
        }
        else if (item.ActionPath == L"internal://refresh")
        {
            HideWindow();
            SearchManager::RequestRescanAsync();
            return;
        }
        else if (item.ActionPath == L"internal://exit")
        {
            DestroyWindow(m_hwnd);
            return;
        }
    }
    else
    {
        SearchManager::LaunchItem(item);
    }
    HideWindow();
}

void LauncherWindow::ShowAndFocus()
{
    if (!m_hwnd)
        return;

    // 清空上次状态与右键菜单
    if (m_hEdit)
    {
        SetWindowTextW(m_hEdit, L"");
        SendMessageW(m_hEdit, EM_EMPTYUNDOBUFFER, 0, 0);
    }
    m_currentQuery.clear();
    m_currentQuery.shrink_to_fit();
    m_imeCompString.clear();
    m_imeCompString.shrink_to_fit();
    m_imeCursorPos = 0;
    m_predictionSuffix.clear();
    m_predictionSuffix.shrink_to_fit();
    m_results.clear();
    m_results.shrink_to_fit();
    ClearVisibleItemLayouts();
    m_searchTextLayout = nullptr;
    m_selectedIndex = -1;
    m_hoveredIndex = -1;
    m_scrollOffset = 0;
    m_isContextMenuVisible = false;
    m_contextMenuTargetIndex = -1;
    m_contextMenuHoveredIndex = -1;
    m_contextMenuItems.clear();
    m_contextMenuItems.shrink_to_fit();

    // 获取当前鼠标所在的显示器工作区
    POINT ptCursor = {0, 0};
    GetCursorPos(&ptCursor);
    HMONITOR hMonitor = MonitorFromPoint(ptCursor, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO monitorInfo = {sizeof(MONITORINFO)};
    if (GetMonitorInfoW(hMonitor, &monitorInfo))
    {
        UINT dpiX = 96, dpiY = 96;
        if (FAILED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        {
            dpiX = GetDpiForWindow(m_hwnd);
        }

        // 当显示器 DPI 发生变化、隐藏期间释放了 DirectWrite 格式或语言发生变化时，按需重建
        if (dpiX != m_currentDpi || !m_searchFormat || m_fontLang != ConfigManager::GetLanguage())
        {
            m_currentDpi = dpiX;
            UpdateDpiResources();
        }
        UpdateSearchBoxLayout();

        int width = static_cast<int>(kWindowWidthDip * m_dpiScale);
        int initialHeight = static_cast<int>(kSearchBarHeightDip * m_dpiScale);
        int maxExpandedHeight = static_cast<int>(kMaxExpandedHeightDip * m_dpiScale);

        int screenWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        int screenHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;

        int posX = monitorInfo.rcWork.left + (screenWidth - width) / 2;
        int posY = monitorInfo.rcWork.top + (screenHeight - maxExpandedHeight) / 2;

        SetWindowPos(m_hwnd, nullptr, posX, posY, width, initialHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else
    {
        UpdateSearchBoxLayout();
        UpdateWindowSize();
    }

    // 显示并激活窗口
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    if (m_hEdit)
    {
        SetFocus(m_hEdit);
    }

    // 重置并启动光标闪烁计时器（Timer 2）
    m_caretVisible = true;
    SetTimer(m_hwnd, 2, GetCaretBlinkTime(), nullptr);

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void LauncherWindow::HideWindow()
{
    if (!m_hwnd || !IsWindowVisible(m_hwnd))
        return;

    // 停止光标闪烁计时器，杜绝后台无用 CPU 唤醒
    KillTimer(m_hwnd, 2);
    m_caretVisible = false;

    // 如果隐藏窗口时输入法还在打拼音，通知系统输入法立刻取消组字并关闭候选框
    if (m_hEdit)
    {
        HIMC hImc = ImmGetContext(m_hEdit);
        if (hImc)
        {
            ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(m_hEdit, hImc);
        }
    }

    // 先隐藏窗口（使 IsWindowVisible 立即变为 FALSE，从而静默拦截后续清空文本引发的 EN_CHANGE 和重绘）
    ShowWindow(m_hwnd, SW_HIDE);

    // 清空搜索框、Undo 缓冲、本次搜索结果及右键菜单状态
    if (m_hEdit)
    {
        SetWindowTextW(m_hEdit, L"");
        SendMessageW(m_hEdit, EM_EMPTYUNDOBUFFER, 0, 0);
    }
    m_currentQuery.clear();
    m_currentQuery.shrink_to_fit();
    m_imeCompString.clear();
    m_imeCompString.shrink_to_fit();
    m_imeCursorPos = 0;
    m_predictionSuffix.clear();
    m_predictionSuffix.shrink_to_fit();
    ClearVisibleItemLayouts();
    m_searchTextLayout = nullptr;
    m_results.clear();
    m_results.shrink_to_fit();
    m_selectedIndex = -1;
    m_hoveredIndex = -1;
    m_scrollOffset = 0;
    m_isTrackingMouse = false;
    m_isClearBtnHovered = false;
    m_isContextMenuVisible = false;
    m_contextMenuTargetIndex = -1;
    m_contextMenuHoveredIndex = -1;
    m_contextMenuItems.clear();
    m_contextMenuItems.shrink_to_fit();

    // 严格按序销毁：
    // 1. 先停止图标线程并清空图标缓存
    // 2. 先销毁 m_renderTarget 及隔离的 DWrite/D2D 工厂（彻底清空字形图集与字体流缓存）
    // 3. 最后再调用 UpdateWindowSize()，避免 SetWindowPos 同步发送 WM_SIZE 时对尚未销毁的 m_renderTarget 触发 Resize 反向分配显存
    IconManager::ClearCache();
    DiscardDeviceResources();
    DiscardDeviceIndependentResources();
    UpdateWindowSize();

    // 主动排空当前窗口残留的异步图标完成消息，防止隐藏后延迟到达
    MSG msg;
    while (PeekMessageW(&msg, m_hwnd, WM_ICON_LOADED, WM_ICON_LOADED, PM_REMOVE))
    {
    }

    // 清理 WinRT 激活工厂缓存并卸载无用的 COM/Shell 拓展模块
    winrt::clear_factory_cache();
    CoFreeUnusedLibrariesEx(0, 0);
}

LRESULT CALLBACK LauncherWindow::EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                  UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    auto *self = reinterpret_cast<LauncherWindow *>(dwRefData);
    if (!self)
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    if (uMsg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hWnd, EditSubclassProc, uIdSubclass);
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    // 辅助 Lambda：仅局部重绘顶部搜索栏，避免无谓重绘下方列表
    auto invalidateSearchBar = [self]() {
        if (!self->m_hwnd)
            return;
        RECT rcSearchBar = {0, 0, static_cast<LONG>(kWindowWidthDip * self->m_dpiScale),
                            static_cast<LONG>(kSearchBarHeightDip * self->m_dpiScale)};
        InvalidateRect(self->m_hwnd, &rcSearchBar, FALSE);
    };

    // 彻底拦截原生 EDIT 的 GDI 绘制与系统擦除，交给主窗口 Direct2D 透明渲染
    if (uMsg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        return 0;
    }
    if (uMsg == WM_ERASEBKGND || uMsg == WM_NCPAINT || uMsg == WM_PRINTCLIENT)
    {
        return 1;
    }

    // 拦截 WM_SETCURSOR，彻底消除鼠标在输入框内移动时光标在箭头与工字型之间闪烁的问题
    if (uMsg == WM_SETCURSOR)
    {
        SetCursor(LoadCursor(nullptr, self->m_isContextMenuVisible ? IDC_ARROW : IDC_IBEAM));
        return TRUE;
    }

    // 管理 D2D 自定义光标闪烁定时器 (Timer ID: 2)
    if (uMsg == WM_SETFOCUS)
    {
        LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        // 创建 0x0 空系统光标并隐藏，防止 EDIT 内部在鼠标/键盘处理时私自 ShowCaret 导致原生白块闪烁
        CreateCaret(hWnd, nullptr, 0, 0);
        HideCaret(hWnd);
        self->m_caretVisible = true;
        SetTimer(self->m_hwnd, 2, GetCaretBlinkTime(), nullptr);
        invalidateSearchBar();
        return res;
    }
    if (uMsg == WM_KILLFOCUS)
    {
        KillTimer(self->m_hwnd, 2);
        self->m_caretVisible = false;
        DestroyCaret();
        invalidateSearchBar();
        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    // 当 D2D 右键菜单处于打开状态时，点击顶部搜索框（EDIT 控件区域）立即收起右键菜单
    if (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN)
    {
        if (self->m_isContextMenuVisible)
        {
            self->CloseContextMenu();
        }
    }

    if (uMsg == WM_KEYDOWN)
    {
        // 若 D2D 右键菜单已打开，优先响应菜单的键盘导航
        if (self->m_isContextMenuVisible && !self->m_contextMenuItems.empty())
        {
            if (wParam == VK_ESCAPE)
            {
                self->CloseContextMenu();
                return 0;
            }
            else if (wParam == VK_UP)
            {
                int count = static_cast<int>(self->m_contextMenuItems.size());
                self->m_contextMenuHoveredIndex =
                    (self->m_contextMenuHoveredIndex <= 0) ? (count - 1) : (self->m_contextMenuHoveredIndex - 1);
                InvalidateRect(self->m_hwnd, nullptr, FALSE);
                return 0;
            }
            else if (wParam == VK_DOWN)
            {
                int count = static_cast<int>(self->m_contextMenuItems.size());
                self->m_contextMenuHoveredIndex = (self->m_contextMenuHoveredIndex + 1) % count;
                InvalidateRect(self->m_hwnd, nullptr, FALSE);
                return 0;
            }
            else if (wParam == VK_RETURN)
            {
                if (self->m_contextMenuHoveredIndex >= 0 &&
                    self->m_contextMenuHoveredIndex < static_cast<int>(self->m_contextMenuItems.size()))
                {
                    UINT cmd = self->m_contextMenuItems[self->m_contextMenuHoveredIndex].cmdId;
                    self->ExecuteContextMenuCommand(cmd);
                }
                return 0;
            }
        }

        if (wParam == VK_RETURN)
        {
            self->LaunchSelectedItem();
            return 0;
        }
        else if (wParam == VK_ESCAPE)
        {
            self->HideWindow();
            return 0;
        }
        else if (wParam == VK_UP)
        {
            if (!self->m_results.empty() && self->m_selectedIndex > 0)
            {
                self->m_selectedIndex--;
                self->EnsureSelectedVisible();
                InvalidateRect(self->m_hwnd, nullptr, FALSE);
            }
            return 0;
        }
        else if (wParam == VK_DOWN)
        {
            if (!self->m_results.empty() && self->m_selectedIndex < static_cast<int>(self->m_results.size()) - 1)
            {
                self->m_selectedIndex++;
                self->EnsureSelectedVisible();
                InvalidateRect(self->m_hwnd, nullptr, FALSE);
            }
            return 0;
        }
        else if (wParam == VK_RIGHT)
        {
            if (!self->m_predictionSuffix.empty())
            {
                DWORD start = 0, end = 0;
                SendMessageW(hWnd, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
                int textLen = GetWindowTextLengthW(hWnd);
                if (start == static_cast<DWORD>(textLen) && end == static_cast<DWORD>(textLen))
                {
                    std::wstring completed = self->m_currentQuery + self->m_predictionSuffix;
                    self->m_predictionSuffix.clear();
                    SetWindowTextW(hWnd, completed.c_str());
                    int newLen = static_cast<int>(completed.length());
                    SendMessageW(hWnd, EM_SETSEL, newLen, newLen);
                    self->m_caretVisible = true;
                    invalidateSearchBar();
                    return 0;
                }
            }
        }
        else if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            SendMessageW(hWnd, EM_SETSEL, 0, -1);
            HideCaret(hWnd);
            self->m_caretVisible = true;
            invalidateSearchBar();
            return 0;
        }
    }
    else if (uMsg == WM_CHAR)
    {
        if (wParam == VK_RETURN || wParam == VK_ESCAPE)
        {
            return 0;
        }
    }
    else if (uMsg == WM_MOUSEWHEEL)
    {
        return SendMessageW(self->m_hwnd, uMsg, wParam, lParam);
    }

    // 接管中文输入法 (IME) 组字，消灭系统白底黑字组字浮窗
    if (uMsg == WM_IME_STARTCOMPOSITION)
    {
        return 0;
    }
    if (uMsg == WM_IME_ENDCOMPOSITION)
    {
        self->m_imeCompString.clear();
        self->m_imeCursorPos = 0;
        self->RebuildSearchTextLayout();
        invalidateSearchBar();
        return 0;
    }
    if (uMsg == WM_IME_COMPOSITION)
    {
        HIMC hImc = ImmGetContext(hWnd);
        if (hImc)
        {
            // 如果用户按了空格或数字键完成选词（上屏）
            if (lParam & GCS_RESULTSTR)
            {
                LONG bytes = ImmGetCompositionStringW(hImc, GCS_RESULTSTR, nullptr, 0);
                if (bytes > 0)
                {
                    std::wstring resultStr(bytes / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(hImc, GCS_RESULTSTR, resultStr.data(), bytes);
                    // 将选好的中文词直接替换进 EDIT 控件（会自动触发 EN_CHANGE 搜索）
                    SendMessageW(hWnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(resultStr.c_str()));
                }
                self->m_imeCompString.clear();
                self->m_imeCursorPos = 0;
            }

            // 如果用户正在敲击拼音字母
            if (lParam & GCS_COMPSTR)
            {
                LONG bytes = ImmGetCompositionStringW(hImc, GCS_COMPSTR, nullptr, 0);
                if (bytes > 0)
                {
                    self->m_imeCompString.resize(bytes / sizeof(wchar_t));
                    ImmGetCompositionStringW(hImc, GCS_COMPSTR, self->m_imeCompString.data(), bytes);
                    self->m_imeCursorPos = ImmGetCompositionStringW(hImc, GCS_CURSORPOS, nullptr, 0);
                }
                else
                {
                    self->m_imeCompString.clear();
                    self->m_imeCursorPos = 0;
                }
            }
            else if (!(lParam & GCS_RESULTSTR))
            {
                self->m_imeCompString.clear();
                self->m_imeCursorPos = 0;
            }

            // 更新 D2D 文本布局并计算当前拼音光标的物理坐标，让输入法候选框完美跟随！
            self->RebuildSearchTextLayout();
            if (self->m_searchTextLayout)
            {
                DWORD selStart = 0, selEnd = 0;
                SendMessageW(hWnd, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));

                // 候选框水平位置对准拼音的起始位置 (selEnd)
                float compStartX = 0, compStartY = 0;
                DWRITE_HIT_TEST_METRICS htm{};
                self->m_searchTextLayout->HitTestTextPosition(selEnd, FALSE, &compStartX, &compStartY, &htm);

                LONG targetX = static_cast<LONG>(compStartX);
                LONG textBottomY = static_cast<LONG>(htm.height);

                COMPOSITIONFORM compForm = {};
                compForm.dwStyle = CFS_POINT;
                compForm.ptCurrentPos.x = targetX;
                compForm.ptCurrentPos.y = 0;
                ImmSetCompositionWindow(hImc, &compForm);

                // 使用 CFS_EXCLUDE 告诉输入法文字的实际上下边界，让候选框紧贴文字下沿弹出
                CANDIDATEFORM cf = {};
                cf.dwIndex = 0;
                cf.dwStyle = CFS_EXCLUDE;
                cf.ptCurrentPos.x = targetX;
                cf.ptCurrentPos.y = textBottomY;
                cf.rcArea.left = targetX;
                cf.rcArea.top = 0;
                cf.rcArea.right = targetX + 2;
                cf.rcArea.bottom = textBottomY;
                ImmSetCandidateWindow(hImc, &cf);
            }

            ImmReleaseContext(hWnd, hImc);
        }

        HideCaret(hWnd);
        self->m_caretVisible = true;
        SetTimer(self->m_hwnd, 2, GetCaretBlinkTime(), nullptr);
        invalidateSearchBar();
        return 0;
    }

    if (uMsg == WM_MOUSEMOVE)
    {
        // 当鼠标移入 EDIT 子控件时，主窗口的 TrackMouseEvent 会触发 WM_MOUSELEAVE，
        // 这里仅在按住鼠标左键拖拽选区时才调用默认过程并重绘搜索栏，平时移动绝不触发无效重绘
        if (wParam & MK_LBUTTON)
        {
            DWORD oldStart = 0, oldEnd = 0;
            SendMessageW(hWnd, EM_GETSEL, reinterpret_cast<WPARAM>(&oldStart), reinterpret_cast<LPARAM>(&oldEnd));
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            HideCaret(hWnd);
            DWORD newStart = 0, newEnd = 0;
            SendMessageW(hWnd, EM_GETSEL, reinterpret_cast<WPARAM>(&newStart), reinterpret_cast<LPARAM>(&newEnd));
            // 仅当选区或光标位置真正发生变化时才重绘
            if (oldStart != newStart || oldEnd != newEnd)
            {
                self->m_caretVisible = true;
                SetTimer(self->m_hwnd, 2, GetCaretBlinkTime(), nullptr);
                invalidateSearchBar();
            }
            return res;
        }
        return 0;
    }

    if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_CHAR || uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP ||
        uMsg == WM_LBUTTONDBLCLK)
    {
        LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
        HideCaret(hWnd);

        self->m_caretVisible = true;
        SetTimer(self->m_hwnd, 2, GetCaretBlinkTime(), nullptr);
        invalidateSearchBar();
        return res;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK LauncherWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    LauncherWindow *self = nullptr;
    if (message == WM_NCCREATE)
    {
        auto *createStruct = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = static_cast<LauncherWindow *>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hWnd;
    }
    else
    {
        self = reinterpret_cast<LauncherWindow *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (self)
    {
        return self->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

LRESULT LauncherWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCCALCSIZE: {
        if (wParam == TRUE)
        {
            return 0;
        }
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR: {
        // 如果鼠标在主窗口客户区内（且不在子控件上），由 WM_MOUSEMOVE 全权控制光标，阻止 DefWindowProc 重置
        if (reinterpret_cast<HWND>(wParam) == m_hwnd && LOWORD(lParam) == HTCLIENT)
        {
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(m_hwnd, &ps);
        OnPaint(ps.rcPaint);
        EndPaint(m_hwnd, &ps);
        return 0;
    }

    case WM_SIZE: {
        if (m_renderTarget)
        {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            m_renderTarget->Resize(D2D1::SizeU(width, height));
        }
        return 0;
    }

    case WM_COMMAND: {
        if (LOWORD(wParam) == 101 && HIWORD(wParam) == EN_CHANGE)
        {
            if (IsWindowVisible(m_hwnd))
            {
                OnSearchTextChanged();
            }
            return 0;
        }
        break;
    }

    case WM_ICON_LOADED: {
        if (IsWindowVisible(m_hwnd) && m_renderTarget && !m_currentQuery.empty())
        {
            IconManager::ProcessPendingIcons(m_renderTarget.get(), m_hwnd);
        }
        else
        {
            IconManager::CancelPendingTasks();
        }
        return 0;
    }

    case WM_SEARCH_REFRESHED: {
        if (IsWindowVisible(m_hwnd) && !m_currentQuery.empty())
        {
            OnSearchTextChanged();
        }
        return 0;
    }

    case WM_HOTKEY: {
        if (wParam == 1)
        {
            if (IsWindowVisible(m_hwnd) && GetForegroundWindow() == m_hwnd)
            {
                HideWindow();
            }
            else
            {
                ShowAndFocus();
            }
            return 0;
        }
        break;
    }

    case WM_SINGLE_INSTANCE_WAKEUP: {
        if (IsWindowVisible(m_hwnd) && GetForegroundWindow() == m_hwnd)
        {
            HideWindow();
        }
        else
        {
            ShowAndFocus();
        }
        return 0;
    }

    case WM_ACTIVATE: {
        if (LOWORD(wParam) == WA_INACTIVE)
        {
            HWND activeHwnd = GetForegroundWindow();
            if (activeHwnd)
            {
                DWORD activeThreadId = GetWindowThreadProcessId(activeHwnd, nullptr);
                if (activeThreadId != GetCurrentThreadId())
                {
                    HideWindow();
                }
            }
            else
            {
                HideWindow();
            }
        }
        return 0;
    }

    case WM_SYSCOMMAND: {
        if ((wParam & 0xFFF0) == SC_MINIMIZE)
        {
            HideWindow();
            return 0;
        }
        break;
    }

    case WM_CLOSE: {
        HideWindow();
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!m_isTrackingMouse)
        {
            TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hwnd, 0};
            TrackMouseEvent(&tme);
            m_isTrackingMouse = true;
        }
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        // 若右键菜单开启，优先进行右键菜单的悬停命中测试
        if (m_isContextMenuVisible)
        {
            int menuHover = HitTestContextMenuItem(pt);
            if (menuHover != m_contextMenuHoveredIndex)
            {
                m_contextMenuHoveredIndex = menuHover;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            SetCursor(LoadCursor(nullptr, menuHover >= 0 ? IDC_HAND : IDC_ARROW));
            return 0;
        }

        bool clearHover = HitTestClearButton(pt);
        int itemHover = HitTestItem(pt);

        bool clearChanged = (clearHover != m_isClearBtnHovered);
        bool itemChanged = (itemHover != m_hoveredIndex);

        if (clearChanged || itemChanged)
        {
            m_isClearBtnHovered = clearHover;
            m_hoveredIndex = itemHover;

            LONG searchBarBottom = static_cast<LONG>(kSearchBarHeightDip * m_dpiScale);
            LONG winWidth = static_cast<LONG>(kWindowWidthDip * m_dpiScale);

            if (clearChanged && itemChanged)
            {
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            else if (clearChanged)
            {
                // 仅重绘顶部搜索栏区域
                RECT rcTop = {0, 0, winWidth, searchBarBottom};
                InvalidateRect(m_hwnd, &rcTop, FALSE);
            }
            else
            {
                // 仅重绘搜索栏下方的列表区域
                RECT rcList = {0, searchBarBottom, winWidth, static_cast<LONG>(kMaxExpandedHeightDip * m_dpiScale)};
                InvalidateRect(m_hwnd, &rcList, FALSE);
            }
        }

        float textLeft = 72.0f * m_dpiScale;
        float textRight = (kWindowWidthDip - 64.0f) * m_dpiScale;
        if (clearHover || itemHover >= 0)
        {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
        }
        else if (pt.y < kSearchBarHeightDip * m_dpiScale && pt.x >= textLeft && pt.x <= textRight)
        {
            SetCursor(LoadCursor(nullptr, IDC_IBEAM));
        }
        else
        {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        m_isTrackingMouse = false;
        if (m_isClearBtnHovered || m_hoveredIndex != -1 || m_contextMenuHoveredIndex != -1)
        {
            m_isClearBtnHovered = false;
            m_hoveredIndex = -1;
            m_contextMenuHoveredIndex = -1;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

        // 若右键菜单开启：点击菜单项则执行对应命令，点击外部区域则收起菜单
        if (m_isContextMenuVisible)
        {
            int menuHit = HitTestContextMenuItem(pt);
            if (menuHit >= 0)
            {
                UINT cmd = m_contextMenuItems[menuHit].cmdId;
                ExecuteContextMenuCommand(cmd);
            }
            else
            {
                CloseContextMenu();
            }
            return 0;
        }

        // 点击顶部搜索栏左侧图标区域时，允许按住拖拽移动窗口
        if (pt.y < kSearchBarHeightDip * m_dpiScale && pt.x < 72.0f * m_dpiScale)
        {
            ReleaseCapture();
            SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        // 点击清空 'X' 按钮
        if (HitTestClearButton(pt))
        {
            if (m_hEdit)
            {
                HIMC hImc = ImmGetContext(m_hEdit);
                if (hImc)
                {
                    ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                    ImmReleaseContext(m_hEdit, hImc);
                }
            }
            m_imeCompString.clear();
            m_imeCursorPos = 0;
            SetWindowTextW(m_hEdit, L"");
            SetFocus(m_hEdit);
            return 0;
        }

        // 点击列表项执行启动
        int hitIdx = HitTestItem(pt);
        if (hitIdx >= 0)
        {
            m_selectedIndex = hitIdx;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            LaunchSelectedItem();
            return 0;
        }

        // 点击顶部搜索框右侧空白或预测词区域时，聚焦到输入框并将光标移至末尾
        if (pt.y < kSearchBarHeightDip * m_dpiScale && m_hEdit)
        {
            SetFocus(m_hEdit);
            int len = GetWindowTextLengthW(m_hEdit);
            SendMessageW(m_hEdit, EM_SETSEL, len, len);
            return 0;
        }
        break;
    }

    case WM_RBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int hitIdx = HitTestItem(pt);
        if (hitIdx >= 0)
        {
            m_selectedIndex = hitIdx;
            ShowContextMenu(hitIdx, pt);
            return 0;
        }
        else if (m_isContextMenuVisible)
        {
            CloseContextMenu();
            return 0;
        }
        break;
    }

    case WM_CONTEXTMENU: {
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (pt.x == -1 && pt.y == -1 && m_selectedIndex >= 0)
        {
            // 键盘菜单键触发（直接传递客户区坐标给 D2D 菜单）
            float s = m_dpiScale;
            int visIdx = m_selectedIndex - m_scrollOffset;
            POINT clientPt = {static_cast<LONG>(100 * s), static_cast<LONG>((85 + visIdx * kItemStepDip + 32) * s)};
            ShowContextMenu(m_selectedIndex, clientPt);
            return 0;
        }
        break;
    }

    case WM_MOUSEWHEEL: {
        if (m_isContextMenuVisible)
        {
            CloseContextMenu();
        }
        int totalCount = static_cast<int>(m_results.size());
        if (totalCount > kMaxVisibleItems)
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int maxOffset = totalCount - kMaxVisibleItems;
            int oldOffset = m_scrollOffset;

            if (delta < 0 && m_scrollOffset < maxOffset)
            {
                m_scrollOffset++;
            }
            else if (delta > 0 && m_scrollOffset > 0)
            {
                m_scrollOffset--;
            }

            if (m_scrollOffset != oldOffset)
            {
                RebuildVisibleItemLayouts();
                // 滚动后立即根据当前鼠标位置重新命中测试，保证悬停高亮框留在鼠标指针下方
                POINT ptClient = {0, 0};
                if (GetCursorPos(&ptClient) && ScreenToClient(m_hwnd, &ptClient))
                {
                    m_hoveredIndex = HitTestItem(ptClient);
                }
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_DPICHANGED: {
        m_currentDpi = HIWORD(wParam);
        UpdateDpiResources();
        const RECT *prcNewWindow = reinterpret_cast<const RECT *>(lParam);

        int width = static_cast<int>(kWindowWidthDip * m_dpiScale);
        int targetHeightDip = kSearchBarHeightDip;
        int count = static_cast<int>(m_results.size());
        if (count > 0 && !m_currentQuery.empty())
        {
            int visibleCount = (std::min)(count, kMaxVisibleItems);
            targetHeightDip = 80 + 1 + 16 + (kItemStepDip * visibleCount);
        }
        int targetHeight = static_cast<int>(targetHeightDip * m_dpiScale);

        SetWindowPos(m_hwnd, nullptr, prcNewWindow->left, prcNewWindow->top, width, targetHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateSearchBoxLayout();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGCHANGE: {
        // 防御第三方程序错误广播整数/ATOM 伪指针 (<= 0xFFFF)，并使用精确长度安全比对
        if (lParam && !IS_INTRESOURCE(lParam) &&
            wcsncmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet", _countof(L"ImmersiveColorSet")) == 0)
        {
            UpdateTheme();
            ApplyBackdrop(ConfigManager::GetBackdrop());
        }
        break;
    }

    case WM_DESTROY: {
        UnregisterHotKey(m_hwnd, 1);
        SearchManager::Shutdown();
        IconManager::Shutdown();
        DiscardDeviceResources();
        PostQuitMessage(0);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == 2)
        {
            m_caretVisible = !m_caretVisible;
            RECT rcSearchBar = {0, 0, static_cast<LONG>(kWindowWidthDip * m_dpiScale),
                                static_cast<LONG>(kSearchBarHeightDip * m_dpiScale)};
            InvalidateRect(m_hwnd, &rcSearchBar, FALSE);
            return 0;
        }
        break;
    }
    }

    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}
