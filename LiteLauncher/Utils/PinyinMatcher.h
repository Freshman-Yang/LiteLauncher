#pragma once

class PinyinMatcher
{
public:
    // 将文本转换为驼峰全拼（如 "微信" -> "WeiXin", "百度cloud" -> "BaiDuCloud"）
    static std::wstring GetCamelPinyin(std::wstring_view text);
};
