#pragma once
#include "Core/SearchManager.h"

class UwpProvider
{
  public:
    static std::vector<SearchResultItem> ScanUwpApps();
};
