#pragma once
#include "Core/SearchManager.h"

class Win32Provider
{
  public:
    static std::vector<SearchResultItem> ScanStartMenuApps();
    static std::wstring ResolveLnkTarget(const std::wstring &lnkPath);
};
