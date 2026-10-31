#include "pch.h"
#include "PinyinMatcher.h"
#include "GeneratedPinyinMap.inc"

std::wstring PinyinMatcher::GetCamelPinyin(std::wstring_view text)
{
    // 1. 极速预检：若文本中不包含任何汉字（纯英文/数字软件名），直接返回空串！
    //    既节省近一半应用的堆内存，又避免 Search 时对纯英文名在 Name 和 Pinyin 通道重复跑两次 DFS。
    bool hasHanzi = false;
    for (wchar_t ch : text)
    {
        if (ch >= 0x4E00 && ch <= 0x9FA5)
        {
            hasHanzi = true;
            break;
        }
    }
    if (!hasHanzi)
    {
        return {};
    }

    std::wstring result;
    result.reserve(text.size() * 4);

    // 标记下一个遇到的英文字母是否处于新词起点（词首、汉字后、数字后或分隔符后）
    bool is_word_start = true;
    wchar_t prev_raw_char = 0;

    for (wchar_t ch : text)
    {
        // 1. 处理标准汉字范围 (CJK Unified Ideographs 0x4E00 - 0x9FA5)
        if (ch >= 0x4E00 && ch <= 0x9FA5)
        {
            const char* syllable = GetHanziCamelPinyin(ch);
            if (syllable != nullptr && syllable[0] != '\0')
            {
                for (const char* p = syllable; *p != '\0'; ++p)
                {
                    result.push_back(static_cast<wchar_t>(*p));
                }
            }
            is_word_start = true;
        }
        // 2. 处理 ASCII 英文字母（规范化为标准驼峰，防止 "WPS文字" -> "WPSWenZi" 或 "DirectX诊断" -> "DirectXZhen" 发生大写粘连）
        else if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'))
        {
            bool is_upper = (ch >= L'A' && ch <= L'Z');
            bool prev_was_lower = (prev_raw_char >= L'a' && prev_raw_char <= L'z');

            // 仅当处于新词起点，或者原本就是“小写转大写”的标准驼峰边界（如 WeChat 的 C）时，才输出大写；
            // 连续大写字母（如 WPS、QQ、DirectX 的 X）后续字母转为小写（Wps、Qq、Directx），确保每个词只有一个大写锚点！
            if (is_word_start || (is_upper && prev_was_lower))
            {
                result.push_back(is_upper ? ch : static_cast<wchar_t>(ch - 32));
            }
            else
            {
                result.push_back(is_upper ? static_cast<wchar_t>(ch + 32) : ch);
            }
            is_word_start = false;
        }
        // 3. 处理 ASCII 数字
        else if (ch >= L'0' && ch <= L'9')
        {
            result.push_back(ch);
            is_word_start = true;
        }
        // 4. 空白符、标点符号及其他非索引字符
        else
        {
            is_word_start = true;
        }

        prev_raw_char = ch;
    }

    return result;
}
