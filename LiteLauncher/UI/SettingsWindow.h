#pragma once

class SettingsWindow
{
  public:
    // 显示设置窗口（若已打开则激活至前台，未打开则按需创建）
    // onBackdropChanged: 当用户在下拉框切换材质时的实时回调，参数为材质索引 (0~3)
    // onLanguageChanged: 当用户在下拉框切换语言时的实时回调
    static void Show(std::function<void(int)> onBackdropChanged = nullptr,
                     std::function<void()> onLanguageChanged = nullptr);

    // 关闭并销毁设置窗口（供外部退出程序时主动清理）
    static void Close();

    // 获取当前设置窗口句柄（可用于主消息循环中的 IsDialogMessageW 处理）
    static HWND GetHwnd()
    {
        return s_hwnd;
    }

  private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static void RegisterWindowClass(HINSTANCE hInstance);
    static void CreateControls(HWND hwnd);
    static void RefreshLocalizedTexts(HWND hwnd);
    static void UpdateFontsAndLayout(HWND hwnd, UINT dpi);
    static void LoadCurrentSettings();
    static void ApplyTheme(HWND hwnd);

    // 控件事件处理
    static void OnBackdropSelectionChanged();
    static void OnLanguageSelectionChanged();
    static void OnAutorunToggle();
    static void OnRestoreDefaultPathsClick();
    static void OnSaveClick();
    static void OnCancelClick();

  private:
    static inline HWND s_hwnd = nullptr;
    static inline bool s_classRegistered = false;
    static inline bool s_isInitializing = false;
    static inline std::function<void(int)> s_onBackdropChanged = nullptr;
    static inline std::function<void()> s_onLanguageChanged = nullptr;

    // 子控件句柄
    static inline HWND s_lblBackdrop = nullptr;
    static inline HWND s_cmbBackdrop = nullptr;
    static inline HWND s_lblLanguage = nullptr;
    static inline HWND s_cmbLanguage = nullptr;
    static inline HWND s_chkAutorun = nullptr;
    static inline HWND s_lblPaths = nullptr;
    static inline HWND s_btnRestore = nullptr;
    static inline HWND s_lblDesc = nullptr;
    static inline HWND s_edtPaths = nullptr;
    static inline HWND s_btnSave = nullptr;
    static inline HWND s_btnCancel = nullptr;

    // GDI 资源（随窗口关闭或 DPI 改变自动释放）
    static inline HFONT s_fontTitle = nullptr;
    static inline HFONT s_fontNormal = nullptr;
    static inline HFONT s_fontDesc = nullptr;
    static inline HFONT s_fontCode = nullptr;
    static inline HBRUSH s_bgBrush = nullptr;
    static inline HBRUSH s_editBgBrush = nullptr;
    static inline bool s_isDarkMode = false;

    // 控件 ID 定义
    static constexpr int IDC_CMB_BACKDROP = 1001;
    static constexpr int IDC_BTN_RESTORE = 1002;
    static constexpr int IDC_EDT_PATHS = 1003;
    static constexpr int IDC_BTN_SAVE = 1004;
    static constexpr int IDC_BTN_CANCEL = 1005;
    static constexpr int IDC_CHK_AUTORUN = 1006;
    static constexpr int IDC_CMB_LANGUAGE = 1007;
};
