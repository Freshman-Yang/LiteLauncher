#include "pch.h"
#include "ConfigManager.h"
#include <KnownFolders.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>

using namespace winrt::Windows::Data::Json;

namespace
{
// 使用永不析构的堆分配结构体管理跨线程配置状态，彻底杜绝退出时后台线程读取引发 UAF
struct ConfigState
{
    std::mutex Mutex;
    BackdropType Backdrop = BackdropType::PureBlur;
    std::atomic<AppLanguage> Language{AppLanguage::ZhCN};
    std::vector<std::wstring> SearchPaths;
};

ConfigState &GetState()
{
    static ConfigState *s_state = new ConfigState();
    return *s_state;
}

std::wstring GetKnownFolder(REFKNOWNFOLDERID rfid)
{
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, nullptr, &path)))
    {
        if (path)
            result = path;
        CoTaskMemFree(path);
    }
    return result;
}

std::filesystem::path GetExeConfigPath()
{
    std::wstring selfExe = GetSelfExePath();
    if (selfExe.empty())
        return {};
    return std::filesystem::path(selfExe).parent_path() / L"config.json";
}

std::filesystem::path GetLocalAppDataConfigPath()
{
    std::wstring localAppData = GetKnownFolder(FOLDERID_LocalAppData);
    if (localAppData.empty())
        return {};
    return std::filesystem::path(localAppData) / L"LiteLauncher" / L"config.json";
}

// 遵循 EAFP 原则：直接尝试原子写入目标路径，若无权限则快速失败交由上层 Fallback，避免启动期探测产生额外 I/O
bool TryWriteConfigAtomically(const std::filesystem::path &cfgPath, const std::string &utf8Content) noexcept
{
    if (cfgPath.empty())
        return false;

    try
    {
        std::wstring tmpPath = cfgPath.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";

        HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD bytesToWrite = static_cast<DWORD>(utf8Content.size());
        DWORD bytesWritten = 0;
        BOOL writeOk = WriteFile(hFile, utf8Content.data(), bytesToWrite, &bytesWritten, nullptr);
        CloseHandle(hFile);

        if (!writeOk || bytesWritten != bytesToWrite)
        {
            DeleteFileW(tmpPath.c_str());
            return false;
        }

        // 原子替换配置文件，防止意外退出导致 config.json 损坏为空文件（移除 WRITE_THROUGH 避免阻塞 UI 线程）
        if (!MoveFileExW(tmpPath.c_str(), cfgPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileW(tmpPath.c_str());
            return false;
        }

        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::wstring Utf8ToWide(const std::string &str)
{
    if (str.empty())
        return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    if (size <= 0)
        return L"";
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size);
    return wstr;
}

std::string WideToUtf8(const std::wstring &wstr)
{
    if (wstr.empty())
        return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return "";
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size, nullptr, nullptr);
    return str;
}
} // namespace

std::wstring ConfigManager::NormalizePath(const std::wstring &path)
{
    std::wstring res = path;
    while (!res.empty() && (res.back() == L' ' || res.back() == L'\t' || res.back() == L'\r' || res.back() == L'\n' ||
                            res.back() == L'"'))
    {
        res.pop_back();
    }
    while (!res.empty() && (res.front() == L' ' || res.front() == L'\t' || res.front() == L'"'))
    {
        res.erase(res.begin());
    }

    res = ExpandEnvStrings(res);

    while (res.length() > 3 && (res.back() == L'\\' || res.back() == L'/'))
    {
        res.pop_back();
    }
    if (res.length() == 2 && res[1] == L':')
    {
        res += L'\\';
    }
    return ToLower(res);
}

std::vector<std::wstring> ConfigManager::GetDefaultPaths()
{
    std::vector<std::wstring> defaults;
    auto p1 = GetKnownFolder(FOLDERID_CommonPrograms);
    auto p2 = GetKnownFolder(FOLDERID_Programs);
    auto p3 = GetKnownFolder(FOLDERID_PublicDesktop);
    auto p4 = GetKnownFolder(FOLDERID_Desktop);
    if (!p1.empty())
        defaults.push_back(p1);
    if (!p2.empty())
        defaults.push_back(p2);
    if (!p3.empty())
        defaults.push_back(p3);
    if (!p4.empty())
        defaults.push_back(p4);
    return defaults;
}

AppLanguage ConfigManager::DetectSystemDefaultLanguage() noexcept
{
    LANGID langId = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(langId) == LANG_CHINESE)
    {
        return AppLanguage::ZhCN;
    }
    return AppLanguage::EnUS;
}

void ConfigManager::LoadConfig()
{
    auto &state = GetState();
    const AppLanguage systemDefaultLang = DetectSystemDefaultLanguage();
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        state.SearchPaths = GetDefaultPaths();
        state.Backdrop = BackdropType::PureBlur;
        state.Language.store(systemDefaultLang, std::memory_order_release);
    }

    try
    {
        std::error_code ec;
        std::filesystem::path exeCfg = GetExeConfigPath();
        std::filesystem::path appDataCfg = GetLocalAppDataConfigPath();
        std::filesystem::path targetFsPath;

        bool exeCfgExists = !exeCfg.empty() && std::filesystem::exists(exeCfg, ec) && !ec;
        bool appDataCfgExists = !appDataCfg.empty() && std::filesystem::exists(appDataCfg, ec) && !ec;

        if (exeCfgExists && appDataCfgExists)
        {
            // 若两处均存在（例如安装目录下带有只读预设 config.json，且此前已回退保存至 LocalAppData），优先读取修改时间更新的配置
            auto exeTime = std::filesystem::last_write_time(exeCfg, ec);
            auto appTime = std::filesystem::last_write_time(appDataCfg, ec);
            targetFsPath = (!ec && appTime >= exeTime) ? appDataCfg : exeCfg;
        }
        else if (exeCfgExists)
        {
            targetFsPath = exeCfg;
        }
        else if (appDataCfgExists)
        {
            targetFsPath = appDataCfg;
        }
        else
        {
            // 首次启动未找到任何配置文件，直接按默认配置触发落盘
            SaveConfig();
            return;
        }

        auto fileSize = std::filesystem::file_size(targetFsPath, ec);
        // 校验文件大小：若读取失败、为空或超过 1 MB 上限，直接放弃读取以防恶意大文件撑爆内存
        if (ec || fileSize == 0 || fileSize > 1024 * 1024)
            return;

        std::ifstream file(targetFsPath, std::ios::in | std::ios::binary);
        if (!file.is_open())
            return;

        std::string content(static_cast<size_t>(fileSize), '\0');
        if (!file.read(content.data(), static_cast<std::streamsize>(fileSize)))
            return;
        file.close();

        std::wstring jsonStr = Utf8ToWide(content);

        JsonObject jsonObj;
        if (JsonObject::TryParse(jsonStr, jsonObj))
        {
            BackdropType loadedBackdrop = BackdropType::PureBlur;
            AppLanguage loadedLanguage = systemDefaultLang;
            bool hasLanguageKey = false;
            std::vector<std::wstring> loadedPaths;

            if (jsonObj.HasKey(L"backdrop"))
            {
                int bd = static_cast<int>(jsonObj.GetNamedNumber(L"backdrop", 0));
                if (bd >= 0 && bd <= 3)
                    loadedBackdrop = static_cast<BackdropType>(bd);
            }
            if (jsonObj.HasKey(L"language"))
            {
                auto langVal = jsonObj.Lookup(L"language");
                if (langVal.ValueType() == JsonValueType::Number)
                {
                    int langNum = static_cast<int>(langVal.GetNumber());
                    if (langNum == 0 || langNum == 1)
                    {
                        loadedLanguage = (langNum == 1) ? AppLanguage::EnUS : AppLanguage::ZhCN;
                        hasLanguageKey = true;
                    }
                }
                else if (langVal.ValueType() == JsonValueType::String)
                {
                    std::wstring langStr = langVal.GetString().c_str();
                    if (!langStr.empty())
                    {
                        if (FindSubstringCaseInsensitive(langStr, L"en") == 0 || langStr == L"1")
                        {
                            loadedLanguage = AppLanguage::EnUS;
                            hasLanguageKey = true;
                        }
                        else if (FindSubstringCaseInsensitive(langStr, L"zh") == 0 || langStr == L"0")
                        {
                            loadedLanguage = AppLanguage::ZhCN;
                            hasLanguageKey = true;
                        }
                    }
                }
            }
            if (jsonObj.HasKey(L"searchPaths"))
            {
                auto arr = jsonObj.GetNamedArray(L"searchPaths");
                for (uint32_t i = 0; i < arr.Size(); ++i)
                {
                    std::wstring p = arr.GetStringAt(i).c_str();
                    if (!p.empty())
                        loadedPaths.push_back(p);
                }
            }

            {
                std::lock_guard<std::mutex> lock(state.Mutex);
                state.Backdrop = loadedBackdrop;
                state.Language.store(loadedLanguage, std::memory_order_release);
                if (!loadedPaths.empty())
                {
                    state.SearchPaths = std::move(loadedPaths);
                }
            }

            if (!hasLanguageKey)
            {
                SaveConfig();
            }
        }
    }
    catch (...)
    {
        OutputDebugStringW(L"[LiteLauncher] Load config.json failed, using defaults.\n");
    }
    winrt::clear_factory_cache();
}

void ConfigManager::SaveConfig()
{
    auto &state = GetState();
    BackdropType backdropCopy;
    AppLanguage languageCopy;
    std::vector<std::wstring> pathsCopy;

    // 仅在持锁阶段快速复制状态快照，随后立即释放锁，彻底避免在互斥锁保护区内进行耗时文件 I/O
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        backdropCopy = state.Backdrop;
        languageCopy = state.Language.load(std::memory_order_acquire);
        pathsCopy = state.SearchPaths;
    }

    try
    {
        JsonObject jsonObj;
        jsonObj.Insert(L"language",
                       JsonValue::CreateStringValue(languageCopy == AppLanguage::EnUS ? L"en-US" : L"zh-CN"));
        jsonObj.Insert(L"backdrop", JsonValue::CreateNumberValue(static_cast<double>(backdropCopy)));

        JsonArray arr;
        for (const auto &p : pathsCopy)
        {
            arr.Append(JsonValue::CreateStringValue(p));
        }
        jsonObj.Insert(L"searchPaths", arr);

        std::string utf8Str = WideToUtf8(jsonObj.Stringify().c_str());
        jsonObj = nullptr;
        arr = nullptr;
        winrt::clear_factory_cache();

        // 1. 优先尝试便携模式：直接写入 exe 同级目录
        std::filesystem::path exeCfg = GetExeConfigPath();
        if (TryWriteConfigAtomically(exeCfg, utf8Str))
        {
            return;
        }

        // 2. 若 exe 目录无写权限（如位于 Program Files 下），回退至 %LOCALAPPDATA%\LiteLauncher
        std::filesystem::path appDataCfg = GetLocalAppDataConfigPath();
        if (!appDataCfg.empty())
        {
            std::error_code ec;
            if (appDataCfg.has_parent_path())
            {
                std::filesystem::create_directories(appDataCfg.parent_path(), ec);
            }
            if (TryWriteConfigAtomically(appDataCfg, utf8Str))
            {
                return;
            }
        }

        OutputDebugStringW(L"[LiteLauncher] SaveConfig: Both ExeDir and LocalAppData write failed.\n");
    }
    catch (...)
    {
        OutputDebugStringW(L"[LiteLauncher] Save config.json failed.\n");
    }
}

BackdropType ConfigManager::GetBackdrop()
{
    auto &state = GetState();
    std::lock_guard<std::mutex> lock(state.Mutex);
    return state.Backdrop;
}

void ConfigManager::SetBackdrop(BackdropType type)
{
    auto &state = GetState();
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        state.Backdrop = type;
    }
    SaveConfig();
}

AppLanguage ConfigManager::GetLanguage() noexcept
{
    return GetState().Language.load(std::memory_order_acquire);
}

void ConfigManager::SetLanguage(AppLanguage lang)
{
    auto &state = GetState();
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        state.Language.store(lang, std::memory_order_release);
    }
    SaveConfig();
}

std::vector<std::wstring> ConfigManager::GetSearchPaths()
{
    auto &state = GetState();
    std::lock_guard<std::mutex> lock(state.Mutex);
    return state.SearchPaths;
}

void ConfigManager::SetSearchPaths(const std::vector<std::wstring> &paths)
{
    auto &state = GetState();
    {
        std::lock_guard<std::mutex> lock(state.Mutex);
        state.SearchPaths = paths;
    }
    SaveConfig();
}

std::vector<SearchPathConfig> ConfigManager::GetClassifiedSearchPaths()
{
    std::vector<std::wstring> currentPaths = GetSearchPaths();

    // 进程生命周期内恒定不变的系统固有目录，使用局部静态常量缓存以避免重复系统调用与规范化开销
    static const std::wstring commonStart = NormalizePath(GetKnownFolder(FOLDERID_CommonPrograms));
    static const std::wstring userStart = NormalizePath(GetKnownFolder(FOLDERID_Programs));
    static const std::wstring pubDesktop = NormalizePath(GetKnownFolder(FOLDERID_PublicDesktop));
    static const std::wstring userDesktop = NormalizePath(GetKnownFolder(FOLDERID_Desktop));

    std::vector<SearchPathConfig> result;
    result.reserve(currentPaths.size());

    for (const auto &rawPath : currentPaths)
    {
        std::wstring norm = NormalizePath(rawPath);
        if (norm.empty())
            continue;

        if (!commonStart.empty() && norm == commonStart)
        {
            result.push_back({std::move(norm), 4, true});
        }
        else if (!userStart.empty() && norm == userStart)
        {
            result.push_back({std::move(norm), 4, true});
        }
        else if (!pubDesktop.empty() && norm == pubDesktop)
        {
            result.push_back({std::move(norm), 1, true});
        }
        else if (!userDesktop.empty() && norm == userDesktop)
        {
            result.push_back({std::move(norm), 1, true});
        }
        else
        {
            result.push_back({std::move(norm), 2, false});
        }
    }
    return result;
}
