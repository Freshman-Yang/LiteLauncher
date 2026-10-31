#pragma once
#include "Core/SearchManager.h"
#include <atomic>
#include <memory>

#define WM_ICON_LOADED (WM_USER + 101)

struct PendingIconData
{
    uint64_t Key = 0;
    std::unique_ptr<uint32_t[]> Pixels; // 64x64 32bpp PBGRA 预乘像素（为空代表提取失败/负缓存）
    UINT Width = 0;
    UINT Height = 0;
    uint64_t Generation = 0;
};

struct IconLoadTask
{
    uint64_t Key = 0;
    std::wstring ActionPath;
    std::wstring TargetExePath;
    const wchar_t *IconPath = nullptr;
    int IconId = 0;
    ItemType Type = ItemType::Win32;
    HWND Hwnd = nullptr;
    uint64_t Generation = 0;
};

class IconManager
{
  public:
    static void Initialize();
    static ID2D1Bitmap *GetIcon(const SearchResultItem &item, ID2D1RenderTarget *pRT, HWND hwnd);
    static ID2D1Bitmap *GetCommandD2DBitmap(ID2D1RenderTarget *pRT);
    static void ProcessPendingIcons(ID2D1RenderTarget *pRT, HWND hwnd);
    static void CancelPendingTasks();
    static void ClearCache();
    static void OnRenderTargetDestroyed();
    static void Shutdown();

  private:
    static uint64_t GetItemKey(const SearchResultItem &item) noexcept;

    static void ExtractIconAsync(const IconLoadTask &task, IWICImagingFactory *pWicFactory);
    static std::unique_ptr<uint32_t[]> ExtractWin32Icon(const IconLoadTask &task,
                                                        IWICImagingFactory *pWicFactory,
                                                        UINT &outWidth,
                                                        UINT &outHeight);
    static std::unique_ptr<uint32_t[]> ExtractUwpIcon(const IconLoadTask &task,
                                                      IWICImagingFactory *pWicFactory,
                                                      UINT &outWidth,
                                                      UINT &outHeight);
    static std::unique_ptr<uint32_t[]> ExtractSettingsDllIcon(const IconLoadTask &task,
                                                              IWICImagingFactory *pWicFactory,
                                                              UINT &outWidth,
                                                              UINT &outHeight);

    static std::unique_ptr<uint32_t[]> ConvertHIconToPBGRA(HICON hIcon,
                                                           IWICImagingFactory *pWicFactory,
                                                           UINT &outWidth,
                                                           UINT &outHeight);
    static std::unique_ptr<uint32_t[]> ConvertHBitmapToPBGRA(HBITMAP hbmp,
                                                             IWICImagingFactory *pWicFactory,
                                                             UINT &outWidth,
                                                             UINT &outHeight);
    static std::unique_ptr<uint32_t[]> CopySourceToPBGRA(IWICBitmapSource *pSource,
                                                         IWICImagingFactory *pWicFactory,
                                                         UINT &outWidth,
                                                         UINT &outHeight);

  private:
    static constexpr size_t MAX_CACHE_SIZE = 32;

    struct CacheEntry
    {
        uint64_t Key = 0;
        winrt::com_ptr<ID2D1Bitmap> Bitmap; // 允许为 nullptr，代表负缓存（已尝试提取但失败/无图标）
    };

    // 仅限 UI 线程访问的资源（不持有任何常驻 WIC COM 对象，保证 windowscodecs.dll 可随时卸载）
    static inline std::unique_ptr<uint32_t[]> s_appIconPixels;
    static inline UINT s_appIconWidth = 0;
    static inline UINT s_appIconHeight = 0;
    static inline winrt::com_ptr<ID2D1Bitmap> s_commandD2DBitmap;
    static inline std::vector<CacheEntry> s_d2dCache;
    static inline ID2D1RenderTarget *s_currentRT = nullptr;
};

