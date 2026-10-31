#pragma once

enum class ItemType
{
    Win32,
    UWP,
    Settings,
    Command
};

struct SearchResultItem
{
    std::wstring Name;
    std::wstring Pinyin;        // 首字母大写的驼峰全拼与字母数字序列（如 "WeiXin"、"KongZhiMianBan"）
    std::wstring ActionPath;    // Win32为lnk/exe路径，UWP为AUMID，Settings作为唯一标识ID
    std::wstring TargetExePath; // 用于专门存储要执行的程序
    std::wstring ExtraKeywords; // 动态提取的英文/驼峰检索词（如 Win32 目标 exe 名 WeChat、UWP 包名等）

    // 静态常量表指针（零堆内存分配）：用于 Settings、内置 Command 以及 UWP 静态内置别名表
    const wchar_t *AltName = nullptr; // 双语条目的另一语言核心主名称（与 Name 同权匹配，零堆内存分配）
    const wchar_t *ActionArgs = nullptr;
    const wchar_t *Keywords = nullptr;
    const wchar_t *IconPath = nullptr;
    int IconId = 0;

    ItemType Type = ItemType::Win32;
};

class SearchManager
{
  public:
    // 异步请求扫描/刷新缓存（内部自动合并并发请求，按需拉起后台线程）
    static void RequestRescanAsync(HWND notifyHwnd = nullptr, UINT notifyMsg = 0);

    // 零堆内存分配的复用型搜索接口（供高频 UI 检索复用同一个 vector 与内部 string 容量）
    static void Search(const std::wstring &query, std::vector<SearchResultItem> &outResults);

    static void LaunchItem(const SearchResultItem &targetApp);

    // 进程退出前调用，通知后台扫描立即中止并进行百毫秒级安全同步等待
    static void Shutdown();
    static bool IsShuttingDown();

  private:
    // 同步执行扫描（供内部或特殊回调使用）
    static void InitializeCache();
};
