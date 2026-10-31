#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef GetCurrentTime
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>
#include <string_view>

// 原生 Win32 / Direct2D / WIC 头文件
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>

// 标准库
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <thread>
#include <mutex>

// 系统内置 WinRT 基础支持 (供 UWP 扫描与 JSON 配置解析使用)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Core.h> // 提供 Package 与 AppListEntry (GetAppListEntriesAsync/DisplayInfo/AppUserModelId) 的完整内联定义
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Data.Json.h>

// 全局字符串小写转换工具函数
inline std::wstring ToLower(std::wstring str) {
    if (!str.empty()) {
        CharLowerBuffW(str.data(), static_cast<DWORD>(str.size()));
    }
    return str;
}

// 动态获取当前 EXE 完整路径（突破 MAX_PATH 260 字符限制）
inline std::wstring GetSelfExePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) return L"";
        if (size < buffer.size() - 1) {
            buffer.resize(size);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

// 展开路径中的环境变量（如 %APPDATA%、%ProgramData%）
inline std::wstring ExpandEnvStrings(const std::wstring& path) {
    if (path.find(L'%') == std::wstring::npos) return path;
    DWORD needed = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (needed > 0) {
        std::wstring expanded(needed, L'\0');
        DWORD written = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), needed);
        if (written > 0 && written <= needed) {
            expanded.resize(written - 1);
            return expanded;
        }
    }
    return path;
}

// 全局零堆分配的 64 位 FNV-1a 哈希函数（供路径去重与 D2D 图标缓存 Key 使用）
constexpr uint64_t HashStringFNV1a(std::wstring_view sv, uint64_t basis = 14695981039346656037ULL) noexcept {
    uint64_t hash = basis;
    for (wchar_t c : sv) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline int FindSubstringCaseInsensitive(std::wstring_view text, std::wstring_view pattern) noexcept {
    if (pattern.empty()) return 0;
    if (text.size() < pattern.size()) return -1;
    return FindStringOrdinal(FIND_FROMSTART, text.data(), static_cast<int>(text.size()),
                             pattern.data(), static_cast<int>(pattern.size()), TRUE);
}

// 零分配极速字符转小写（ASCII 与 CJK 无大小写区间零系统调用）
inline wchar_t ToLowerCharFast(wchar_t c) noexcept {
    if (c < 0x80) {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    }
    // 常用 CJK 统一表意文字 (0x4E00-0x9FFF)、扩展 A 区与 CJK 符号标点 (0x3000-0x4DBF) 均无大小写，直接原样返回
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3000 && c <= 0x4DBF)) {
        return c;
    }
    return static_cast<wchar_t>(reinterpret_cast<uintptr_t>(
        CharLowerW(reinterpret_cast<LPWSTR>(static_cast<uintptr_t>(c)))));
}

// 零内存分配：大小写不敏感的 FNV-1a 哈希（用于去重）
inline uint64_t HashStringCaseInsensitiveFNV1a(std::wstring_view sv, uint64_t basis = 14695981039346656037ULL) noexcept {
    uint64_t hash = basis;
    for (wchar_t c : sv) {
        c = ToLowerCharFast(c);
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// 大小写不敏感的后缀匹配工具
inline bool EndsWithCaseInsensitive(std::wstring_view sv, std::wstring_view suffix) noexcept {
    if (sv.size() < suffix.size()) return false;
    return FindStringOrdinal(
        FIND_FROMSTART,
        sv.data() + (sv.size() - suffix.size()),
        static_cast<int>(suffix.size()),
        suffix.data(),
        static_cast<int>(suffix.size()),
        TRUE) == 0;
}

// 大小写不敏感的字符串完全相等比较（零堆内存分配）
inline bool EqualsCaseInsensitive(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return FindStringOrdinal(FIND_FROMSTART, a.data(), static_cast<int>(a.size()),
                             b.data(), static_cast<int>(b.size()), TRUE) == 0;
}

// 全局检测系统是否处于深色模式（默认回退为深色）
inline bool IsSystemAppsDarkMode() noexcept {
    DWORD value = 0; // 默认深色 (0)
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return (value == 0);
    }
    return true;
}
