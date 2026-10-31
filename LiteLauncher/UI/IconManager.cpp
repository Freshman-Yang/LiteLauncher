#include "pch.h"
#include <KnownFolders.h>
#include <shobjidl.h>
#include "IconManager.h"
#include "Assets/resource.h"
#include <chrono>
#include <condition_variable>
#include <shlobj.h>

namespace
{
// 跨线程同步状态（全部采用连续内存 std::vector，彻底消除 std::deque / std::unordered_set 节点驻留）
struct AsyncIconState
{
    std::mutex Mutex;
    std::condition_variable Cv;
    std::vector<IconLoadTask> TaskQueue;
    std::vector<uint64_t> LoadingKeys;
    std::vector<PendingIconData> PendingQueue;

    std::thread WorkerThread;
    bool WorkerRunning{false};
    bool StopWorkerRequested{false};

    std::atomic<bool> IsShuttingDown{false};
    std::atomic<uint64_t> Generation{1};
};

AsyncIconState &GetAsyncState()
{
    static AsyncIconState *s_state = new AsyncIconState();
    return *s_state;
}

// 排空当前 STA 线程隐藏套间窗口（OleMainThreadWndClass）的消息队列，确保 COM Stub/Proxy 延迟释放立即完成
void DrainCurrentThreadStaMessages() noexcept
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool ContainsKey(const std::vector<uint64_t> &vec, uint64_t key) noexcept
{
    for (uint64_t k : vec)
    {
        if (k == key)
            return true;
    }
    return false;
}

void EraseKey(std::vector<uint64_t> &vec, uint64_t key) noexcept
{
    for (size_t i = 0; i < vec.size(); ++i)
    {
        if (vec[i] == key)
        {
            vec[i] = vec.back();
            vec.pop_back();
            return;
        }
    }
}
} // namespace

void IconManager::Initialize()
{
    if (s_appIconPixels)
        return;

    HICON hAppIcon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
    if (!hAppIcon)
        return;

    {
        winrt::com_ptr<IWICImagingFactory> wicFactory;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(wicFactory.put()))))
        {
            s_appIconPixels = ConvertHIconToPBGRA(hAppIcon, wicFactory.get(), s_appIconWidth, s_appIconHeight);
        }
    }
    DestroyIcon(hAppIcon);

    // 立即卸载初始化阶段临时使用的 WIC 工厂，不在主线程保留任何 WIC COM 引用
    CoFreeUnusedLibrariesEx(0, 0);
}

void IconManager::Shutdown()
{
    auto &state = GetAsyncState();
    state.IsShuttingDown.store(true, std::memory_order_release);

    ClearCache();

    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        state.StopWorkerRequested = true;
        state.TaskQueue.clear();
        state.TaskQueue.shrink_to_fit();
        state.LoadingKeys.clear();
        state.LoadingKeys.shrink_to_fit();
        state.PendingQueue.clear();
        state.PendingQueue.shrink_to_fit();
        if (state.WorkerThread.joinable())
        {
            threadToJoin = std::move(state.WorkerThread);
        }
    }
    state.Cv.notify_all();

    if (threadToJoin.joinable())
    {
        threadToJoin.join();
    }

    s_d2dCache.clear();
    s_d2dCache.shrink_to_fit();
    s_commandD2DBitmap = nullptr;
    s_appIconPixels.reset();
    s_appIconWidth = 0;
    s_appIconHeight = 0;
    s_currentRT = nullptr;
}

uint64_t IconManager::GetItemKey(const SearchResultItem &item) noexcept
{
    if (item.Type == ItemType::Settings && item.IconPath && item.IconPath[0] != L'\0')
    {
        uint64_t h = HashStringFNV1a(item.IconPath, HashStringFNV1a(L"S_"));
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(item.IconId));
        return h * 1099511628211ULL;
    }
    uint64_t basis = static_cast<uint64_t>(item.Type) + 14695981039346656037ULL;
    return HashStringFNV1a(item.ActionPath, basis);
}

ID2D1Bitmap *IconManager::GetCommandD2DBitmap(ID2D1RenderTarget *pRT)
{
    if (!pRT)
        return nullptr;

    if (s_currentRT != pRT)
    {
        s_d2dCache.clear();
        s_d2dCache.shrink_to_fit();
        s_commandD2DBitmap = nullptr;
        s_currentRT = pRT;
    }

    if (s_commandD2DBitmap)
    {
        return s_commandD2DBitmap.get();
    }

    if (!s_appIconPixels)
    {
        Initialize();
    }

    if (s_appIconPixels && s_appIconWidth > 0 && s_appIconHeight > 0)
    {
        D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        winrt::com_ptr<ID2D1Bitmap> d2dBitmap;
        if (SUCCEEDED(pRT->CreateBitmap(D2D1::SizeU(s_appIconWidth, s_appIconHeight), s_appIconPixels.get(),
                                        s_appIconWidth * sizeof(uint32_t), &bmpProps, d2dBitmap.put())))
        {
            s_commandD2DBitmap = d2dBitmap;
            return s_commandD2DBitmap.get();
        }
    }
    return nullptr;
}

ID2D1Bitmap *IconManager::GetIcon(const SearchResultItem &item, ID2D1RenderTarget *pRT, HWND hwnd)
{
    auto &state = GetAsyncState();
    if (state.IsShuttingDown.load(std::memory_order_acquire))
        return nullptr;
    if (!pRT)
        return nullptr;

    if (s_currentRT != pRT)
    {
        s_d2dCache.clear();
        s_d2dCache.shrink_to_fit();
        s_commandD2DBitmap = nullptr;
        s_currentRT = pRT;
    }

    if (item.Type == ItemType::Command)
    {
        return GetCommandD2DBitmap(pRT);
    }

    if (item.Type == ItemType::Settings && (!item.IconPath || item.IconPath[0] == L'\0'))
    {
        return nullptr;
    }

    uint64_t key = GetItemKey(item);

    // 平坦连续数组查找（最多 32 项，完全位于 CPU L1 缓存行内，零节点分配开销）
    for (size_t i = 0; i < s_d2dCache.size(); ++i)
    {
        if (s_d2dCache[i].Key == key)
        {
            ID2D1Bitmap *bmp = s_d2dCache[i].Bitmap.get();
            if (i > 0)
            {
                CacheEntry hit = std::move(s_d2dCache[i]);
                for (size_t j = i; j > 0; --j)
                {
                    s_d2dCache[j] = std::move(s_d2dCache[j - 1]);
                }
                s_d2dCache[0] = std::move(hit);
            }
            return bmp;
        }
    }

    std::thread finishedWorkerToJoin;
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        if (!state.IsShuttingDown.load(std::memory_order_relaxed) && !ContainsKey(state.LoadingKeys, key))
        {
            state.LoadingKeys.push_back(key);
            state.StopWorkerRequested = false;
            uint64_t currentGen = state.Generation.load(std::memory_order_relaxed);

            IconLoadTask task;
            task.Key = key;
            task.ActionPath = item.ActionPath;
            if (item.Type == ItemType::Win32)
            {
                task.TargetExePath = item.TargetExePath;
            }
            task.IconPath = item.IconPath;
            task.IconId = item.IconId;
            task.Type = item.Type;
            task.Hwnd = hwnd;
            task.Generation = currentGen;

            state.TaskQueue.push_back(std::move(task));

            if (!state.WorkerRunning)
            {
                if (state.WorkerThread.joinable())
                {
                    finishedWorkerToJoin = std::move(state.WorkerThread);
                }

                state.WorkerRunning = true;
                state.WorkerThread = std::thread([]() {
                    // 专用按需 STA 图标线程：任务完成后自动排空 STA 消息并调用 CoUninitialize 彻底释放 Shell/MRM/WIC 套间缓存
                    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                    winrt::com_ptr<IWICImagingFactory> wicFactory;

                    auto &st = GetAsyncState();
                    for (;;)
                    {
                        IconLoadTask workerTask;
                        {
                            std::unique_lock<std::mutex> lk(st.Mutex);
                            if (st.TaskQueue.empty() && !st.IsShuttingDown.load(std::memory_order_acquire) &&
                                !st.StopWorkerRequested)
                            {
                                st.TaskQueue.shrink_to_fit();
                                st.LoadingKeys.shrink_to_fit();

                                // 保持短暂 120ms 热待命以吸收连续打字期间的图标请求；超时或显式清理时立即退出销毁 STA 套间
                                st.Cv.wait_for(lk, std::chrono::milliseconds(120), [&st]() {
                                    return st.IsShuttingDown.load(std::memory_order_acquire) ||
                                           st.StopWorkerRequested || !st.TaskQueue.empty();
                                });
                            }

                            if (st.IsShuttingDown.load(std::memory_order_acquire) || st.StopWorkerRequested ||
                                st.TaskQueue.empty())
                            {
                                st.TaskQueue.clear();
                                st.TaskQueue.shrink_to_fit();
                                st.LoadingKeys.clear();
                                st.LoadingKeys.shrink_to_fit();
                                st.WorkerRunning = false;
                                break;
                            }

                            workerTask = std::move(st.TaskQueue.front());
                            st.TaskQueue.erase(st.TaskQueue.begin());
                        }

                        if (!wicFactory && SUCCEEDED(hr))
                        {
                            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(wicFactory.put()));
                        }

                        IconManager::ExtractIconAsync(workerTask, wicFactory.get());
                        DrainCurrentThreadStaMessages();
                    }

                    // 严格按序释放 WIC 工厂 -> 排空 STA 隐藏窗口消息 -> 清理 WinRT/COM 缓存 -> 销毁 STA 套间
                    wicFactory = nullptr;
                    DrainCurrentThreadStaMessages();
                    winrt::clear_factory_cache();
                    CoFreeUnusedLibrariesEx(0, 0);
                    DrainCurrentThreadStaMessages();

                    if (SUCCEEDED(hr))
                    {
                        CoUninitialize();
                    }
                    // CoUninitialize 销毁 STA 套间后会释放套间级单例（MrmCoreR/thumbcache/windows.storage），再次触发卸载
                    CoFreeUnusedLibrariesEx(0, 0);
                });
            }
            state.Cv.notify_one();
        }
    }

    if (finishedWorkerToJoin.joinable())
    {
        finishedWorkerToJoin.join();
    }

    return nullptr;
}

void IconManager::ExtractIconAsync(const IconLoadTask &task, IWICImagingFactory *pWicFactory)
{
    auto &state = GetAsyncState();
    if (state.IsShuttingDown.load(std::memory_order_acquire))
        return;

    if (task.Generation != state.Generation.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        EraseKey(state.LoadingKeys, task.Key);
        return;
    }

    std::unique_ptr<uint32_t[]> pixels;
    UINT width = 0;
    UINT height = 0;

    try
    {
        if (pWicFactory)
        {
            if (task.Type == ItemType::Win32)
            {
                pixels = ExtractWin32Icon(task, pWicFactory, width, height);
            }
            else if (task.Type == ItemType::UWP)
            {
                pixels = ExtractUwpIcon(task, pWicFactory, width, height);
            }
            else if (task.Type == ItemType::Settings)
            {
                pixels = ExtractSettingsDllIcon(task, pWicFactory, width, height);
            }
        }
    }
    catch (...)
    {
    }

    if (state.IsShuttingDown.load(std::memory_order_acquire))
        return;

    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        EraseKey(state.LoadingKeys, task.Key);
        // 无论 pixels 是否为空（失败时为 nullptr 即记录负缓存），只要 Generation 匹配均入队通知主线程
        if (!state.IsShuttingDown.load(std::memory_order_acquire) && !state.StopWorkerRequested &&
            task.Generation == state.Generation.load(std::memory_order_acquire))
        {
            shouldNotify = state.PendingQueue.empty();
            PendingIconData pending;
            pending.Key = task.Key;
            pending.Pixels = std::move(pixels);
            pending.Width = width;
            pending.Height = height;
            pending.Generation = task.Generation;
            state.PendingQueue.push_back(std::move(pending));
        }
    }

    if (shouldNotify && task.Hwnd && IsWindow(task.Hwnd))
    {
        PostMessageW(task.Hwnd, WM_ICON_LOADED, 0, 0);
    }
}

std::unique_ptr<uint32_t[]> IconManager::ExtractWin32Icon(const IconLoadTask &task,
                                                          IWICImagingFactory *pWicFactory,
                                                          UINT &outWidth,
                                                          UINT &outHeight)
{
    const std::wstring &primaryPath = !task.TargetExePath.empty() ? task.TargetExePath : task.ActionPath;

    const std::wstring *pathsToTry[2] = {&primaryPath, &task.ActionPath};
    const size_t tryCount = (primaryPath == task.ActionPath) ? 1 : 2;

    for (size_t i = 0; i < tryCount; ++i)
    {
        const std::wstring &path = *pathsToTry[i];
        if (path.empty())
            continue;

        // 优化 1：若目标不是 .lnk 快捷方式（如直连 .exe），优先使用轻量级 SHDefExtractIconW 直接从 PE 资源提取，
        // 避免实例化 CShellItem / CFSFolder 及映射系统 thumbcache_*.db 缩略图数据库
        if (!EndsWithCaseInsensitive(path, L".lnk"))
        {
            HICON hIcon = nullptr;
            HRESULT hrIcon = SHDefExtractIconW(path.c_str(), 0, 0, &hIcon, nullptr, MAKELONG(64, 0));
            if (hrIcon == S_OK && hIcon)
            {
                auto result = ConvertHIconToPBGRA(hIcon, pWicFactory, outWidth, outHeight);
                DestroyIcon(hIcon);
                if (result)
                    return result;
            }
            else if (hIcon)
            {
                DestroyIcon(hIcon);
            }
        }

        // 回退路径：使用 IShellItemImageFactory 处理自定义图标快捷方式或无内嵌图标的特殊可执行程序
        winrt::com_ptr<IShellItemImageFactory> imageFactory;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(imageFactory.put()))))
        {
            HBITMAP hbmp = nullptr;
            if (SUCCEEDED(imageFactory->GetImage({64, 64}, SIIGBF_ICONONLY, &hbmp)) && hbmp)
            {
                auto result = ConvertHBitmapToPBGRA(hbmp, pWicFactory, outWidth, outHeight);
                DeleteObject(hbmp);
                if (result)
                    return result;
            }
        }
    }

    return nullptr;
}

std::unique_ptr<uint32_t[]> IconManager::ExtractUwpIcon(const IconLoadTask &task,
                                                        IWICImagingFactory *pWicFactory,
                                                        UINT &outWidth,
                                                        UINT &outHeight)
{
    winrt::com_ptr<IShellItem> shellItem;
    if (SUCCEEDED(
            SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, task.ActionPath.c_str(), IID_PPV_ARGS(shellItem.put()))))
    {
        if (auto imageFactory = shellItem.try_as<IShellItemImageFactory>())
        {
            HBITMAP hbmp = nullptr;
            if (SUCCEEDED(imageFactory->GetImage({64, 64}, SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY, &hbmp)) && hbmp)
            {
                auto result = ConvertHBitmapToPBGRA(hbmp, pWicFactory, outWidth, outHeight);
                DeleteObject(hbmp);
                if (result)
                    return result;
            }
        }
    }
    return nullptr;
}

std::unique_ptr<uint32_t[]> IconManager::ConvertHIconToPBGRA(HICON hIcon,
                                                             IWICImagingFactory *pWicFactory,
                                                             UINT &outWidth,
                                                             UINT &outHeight)
{
    if (!hIcon || !pWicFactory)
        return nullptr;

    winrt::com_ptr<IWICBitmap> wicBitmap;
    if (SUCCEEDED(pWicFactory->CreateBitmapFromHICON(hIcon, wicBitmap.put())) && wicBitmap)
    {
        return CopySourceToPBGRA(wicBitmap.get(), pWicFactory, outWidth, outHeight);
    }
    return nullptr;
}

std::unique_ptr<uint32_t[]> IconManager::ConvertHBitmapToPBGRA(HBITMAP hbmp,
                                                               IWICImagingFactory *pWicFactory,
                                                               UINT &outWidth,
                                                               UINT &outHeight)
{
    if (!hbmp || !pWicFactory)
        return nullptr;

    winrt::com_ptr<IWICBitmap> wicBitmap;
    if (SUCCEEDED(pWicFactory->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUseAlpha, wicBitmap.put())) && wicBitmap)
    {
        return CopySourceToPBGRA(wicBitmap.get(), pWicFactory, outWidth, outHeight);
    }
    return nullptr;
}

std::unique_ptr<uint32_t[]> IconManager::CopySourceToPBGRA(IWICBitmapSource *pSource,
                                                           IWICImagingFactory *pWicFactory,
                                                           UINT &outWidth,
                                                           UINT &outHeight)
{
    outWidth = 0;
    outHeight = 0;
    if (!pSource || !pWicFactory)
        return nullptr;

    UINT width = 0, height = 0;
    if (FAILED(pSource->GetSize(&width, &height)) || width == 0 || height == 0 || width > 256 || height > 256)
    {
        return nullptr;
    }

    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(pWicFactory->CreateFormatConverter(converter.put())))
    {
        return nullptr;
    }

    // 使用 WICBitmapPaletteTypeCustom 避免 32bppPBGRA 转换时无谓初始化 MedianCut 调色板量化器
    if (FAILED(converter->Initialize(pSource, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f,
                                     WICBitmapPaletteTypeCustom)))
    {
        return nullptr;
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    auto pixels = std::make_unique<uint32_t[]>(pixelCount);
    const UINT stride = width * sizeof(uint32_t);
    const UINT bufferSize = static_cast<UINT>(pixelCount * sizeof(uint32_t));

    if (SUCCEEDED(converter->CopyPixels(nullptr, stride, bufferSize, reinterpret_cast<BYTE *>(pixels.get()))))
    {
        outWidth = width;
        outHeight = height;
        return pixels;
    }

    return nullptr;
}

void IconManager::ProcessPendingIcons(ID2D1RenderTarget *pRT, HWND hwnd)
{
    if (!pRT)
    {
        CancelPendingTasks();
        return;
    }

    if (s_currentRT != pRT)
    {
        s_d2dCache.clear();
        s_d2dCache.shrink_to_fit();
        s_commandD2DBitmap = nullptr;
        s_currentRT = pRT;
    }

    auto &state = GetAsyncState();
    std::vector<PendingIconData> localQueue;
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        localQueue.swap(state.PendingQueue);
        state.PendingQueue.shrink_to_fit();
    }

    bool anyCreated = false;
    uint64_t currentGen = state.Generation.load(std::memory_order_acquire);
    D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);

    for (auto &data : localQueue)
    {
        if (data.Generation == currentGen)
        {
            winrt::com_ptr<ID2D1Bitmap> d2dBitmap;
            if (data.Pixels && data.Width > 0 && data.Height > 0)
            {
                pRT->CreateBitmap(D2D1::SizeU(data.Width, data.Height), data.Pixels.get(),
                                  data.Width * sizeof(uint32_t), &bmpProps, d2dBitmap.put());
                data.Pixels.reset(); // 上传至 D2D 纹理后立即释放临时像素缓冲
            }

            // 写入平坦 LRU 缓存：若提取失败则 d2dBitmap 为 nullptr，作为负缓存记录，防止后续重绘无限重复提取
            bool found = false;
            for (size_t i = 0; i < s_d2dCache.size(); ++i)
            {
                if (s_d2dCache[i].Key == data.Key)
                {
                    s_d2dCache[i].Bitmap = d2dBitmap;
                    if (i > 0)
                    {
                        CacheEntry updated = std::move(s_d2dCache[i]);
                        for (size_t j = i; j > 0; --j)
                        {
                            s_d2dCache[j] = std::move(s_d2dCache[j - 1]);
                        }
                        s_d2dCache[0] = std::move(updated);
                    }
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                if (s_d2dCache.size() >= MAX_CACHE_SIZE)
                {
                    s_d2dCache.pop_back();
                }
                else if (s_d2dCache.capacity() == 0)
                {
                    s_d2dCache.reserve(MAX_CACHE_SIZE);
                }
                s_d2dCache.insert(s_d2dCache.begin(), CacheEntry{data.Key, d2dBitmap});
            }

            if (d2dBitmap)
            {
                anyCreated = true;
            }
        }
    }

    if (anyCreated && hwnd && IsWindow(hwnd))
    {
        RECT rcClient{};
        if (GetClientRect(hwnd, &rcClient))
        {
            UINT dpi = GetDpiForWindow(hwnd);
            LONG searchBarBottom = static_cast<LONG>(80.0f * (dpi > 0 ? dpi : 96) / 96.0f);
            if (rcClient.bottom > searchBarBottom)
            {
                rcClient.top = searchBarBottom;
            }
            InvalidateRect(hwnd, &rcClient, FALSE);
        }
        else
        {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
}

void IconManager::CancelPendingTasks()
{
    auto &state = GetAsyncState();
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        state.Generation.fetch_add(1, std::memory_order_acq_rel);
        state.TaskQueue.clear();
        state.TaskQueue.shrink_to_fit();
        state.PendingQueue.clear();
        state.PendingQueue.shrink_to_fit();
        state.LoadingKeys.clear();
        state.LoadingKeys.shrink_to_fit();
    }
    state.Cv.notify_all();
}

void IconManager::ClearCache()
{
    auto &state = GetAsyncState();
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        state.Generation.fetch_add(1, std::memory_order_acq_rel);
        state.StopWorkerRequested = true;
        state.TaskQueue.clear();
        state.TaskQueue.shrink_to_fit();
        state.PendingQueue.clear();
        state.PendingQueue.shrink_to_fit();
        state.LoadingKeys.clear();
        state.LoadingKeys.shrink_to_fit();
    }
    state.Cv.notify_all();

    s_d2dCache.clear();
    s_d2dCache.shrink_to_fit();
    s_commandD2DBitmap = nullptr;
}

void IconManager::OnRenderTargetDestroyed()
{
    ClearCache();
    s_currentRT = nullptr;
}

std::unique_ptr<uint32_t[]> IconManager::ExtractSettingsDllIcon(const IconLoadTask &task,
                                                                IWICImagingFactory *pWicFactory,
                                                                UINT &outWidth,
                                                                UINT &outHeight)
{
    if (!task.IconPath || task.IconPath[0] == L'\0' || !pWicFactory)
        return nullptr;

    static const std::wstring s_sysDirPrefix = []() {
        WCHAR buf[MAX_PATH];
        UINT len = GetSystemDirectoryW(buf, MAX_PATH);
        return (len > 0 && len < MAX_PATH) ? (std::wstring(buf, len) + L'\\') : std::wstring();
    }();

    if (s_sysDirPrefix.empty())
        return nullptr;
    std::wstring fullDllPath = s_sysDirPrefix + task.IconPath;

    HICON hIcon = nullptr;
    HRESULT hr = SHDefExtractIconW(fullDllPath.c_str(), task.IconId, 0, &hIcon, nullptr, MAKELONG(64, 0));
    if (SUCCEEDED(hr) && hIcon)
    {
        auto result = ConvertHIconToPBGRA(hIcon, pWicFactory, outWidth, outHeight);
        DestroyIcon(hIcon);
        return result;
    }
    return nullptr;
}
