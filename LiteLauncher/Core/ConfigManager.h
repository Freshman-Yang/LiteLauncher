#pragma once

enum class BackdropType
{
    PureBlur = 0,
    Acrylic = 1,
    Mica = 2,
    MicaAlt = 3
};

enum class AppLanguage
{
    ZhCN = 0,
    EnUS = 1
};

struct SearchPathConfig
{
    std::wstring Path;
    int MaxDepth;
    bool IsDefaultLnkOnly; // true: 仅扫描 .lnk (默认路径), false: 仅扫描 .exe (自定义路径)
};

class ConfigManager
{
  public:
    static void LoadConfig();
    static void SaveConfig();

    static BackdropType GetBackdrop();
    static void SetBackdrop(BackdropType type);

    static AppLanguage GetLanguage() noexcept;
    static void SetLanguage(AppLanguage lang);
    static AppLanguage DetectSystemDefaultLanguage() noexcept;

    static std::vector<std::wstring> GetSearchPaths();
    static void SetSearchPaths(const std::vector<std::wstring> &paths);

    static std::vector<std::wstring> GetDefaultPaths();
    static std::vector<SearchPathConfig> GetClassifiedSearchPaths();

  private:
    static std::wstring NormalizePath(const std::wstring &path);
};

