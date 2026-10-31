#pragma once
#include "Core/I18n.h"
#include "Core/SearchManager.h"
#include "GeneratedSettings.inc"

class SettingsProvider
{
  public:
    static std::vector<SearchResultItem> GetSettingsItems()
    {
        std::vector<SearchResultItem> items;
        constexpr size_t mapSize = std::size(g_settings_maps);
        items.reserve(mapSize);

        const bool isEn = I18n::IsEnglish();

        for (size_t i = 0; i < mapSize; ++i)
        {
            const auto &m = g_settings_maps[i];

            SearchResultItem item;
            item.Name = isEn ? m.NameEn : m.NameZh;
            item.AltName = isEn ? m.NameZh : m.NameEn;
            item.Pinyin = m.Pinyin;
            item.Keywords = m.SearchTerms;
            item.Type = ItemType::Settings;

            std::wstring_view exeView(m.Executable);
            std::wstring_view argsView(m.Arguments);

            if (argsView.empty())
            {
                item.ActionPath = exeView;
            }
            else
            {
                item.ActionPath.reserve(exeView.size() + 1 + argsView.size());
                item.ActionPath.append(exeView).append(L" ").append(argsView);
            }

            item.TargetExePath = exeView;
            item.ActionArgs = m.Arguments;
            item.IconPath = m.IconPath;
            item.IconId = m.IconId;

            items.push_back(std::move(item));
        }
        return items;
    }
};
