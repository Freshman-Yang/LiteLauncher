#include "pch.h"
#include "SearchManager.h"
#include "I18n.h"
#include "Providers/SettingsProvider.h"
#include "Providers/UwpProvider.h"
#include "Providers/Win32Provider.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <shared_mutex>
#include <shobjidl.h>
#include <unordered_set>

namespace
{
// 热数据条目：紧凑排列打分所需的视图与 64 位位图掩码，最大化 CPU L1/L2 缓存行命中率
struct HotSearchEntry
{
    uint64_t AsciiMask = 0;
    std::wstring_view Name;
    std::wstring_view AltName;
    std::wstring_view Pinyin;
    std::wstring_view Keywords;
    std::wstring_view ExtraKeywords;
    ItemType Type = ItemType::Win32;
};

// 永不析构的堆驻留状态（热冷数据平行分离，由同一把读写锁保护生命周期）
struct SearchCacheState
{
    std::shared_mutex Mutex;
    std::vector<SearchResultItem> ColdCache; // 持有完整字符串与冷数据（图标、路径等）
    std::vector<HotSearchEntry> HotCache;    // 持有指向 ColdCache 的紧凑视图与预计算位图

    // 线程生命周期管理与同步原语
    std::mutex LifecycleMutex;
    std::condition_variable ScanExitCv;
    std::thread ScanThread;
    bool IsThreadActive{false};

    std::atomic<bool> IsScanning{false};
    std::atomic<bool> RescanRequested{false};
    std::atomic<bool> IsShuttingDown{false};
    std::atomic<HWND> NotifyHwnd{nullptr};
    std::atomic<UINT> NotifyMsg{0};
};

SearchCacheState &GetCacheState()
{
    static SearchCacheState *s_state = new SearchCacheState();
    return *s_state;
}

// 计算字符串的 64 位 ASCII 字母数字位图掩码 (a-z -> bit 0..25, 0-9 -> bit 26..35)
constexpr uint64_t ComputeAsciiMask(std::wstring_view sv, uint64_t seed = 0) noexcept
{
    uint64_t mask = seed;
    for (wchar_t c : sv)
    {
        if (c >= L'a' && c <= L'z')
        {
            mask |= (1ULL << (c - L'a'));
        }
        else if (c >= L'A' && c <= L'Z')
        {
            mask |= (1ULL << (c - L'A'));
        }
        else if (c >= L'0' && c <= L'9')
        {
            mask |= (1ULL << (26 + (c - L'0')));
        }
    }
    return mask;
}

// 根据固定就位的 SearchResultItem 构建热数据视图（调用后严禁发生会导致该 Item 内部缓冲区重分配的操作）
inline HotSearchEntry BuildHotEntry(const SearchResultItem &item) noexcept
{
    HotSearchEntry hot;
    hot.Name = item.Name;
    hot.AltName = item.AltName ? std::wstring_view(item.AltName) : std::wstring_view{};
    hot.Pinyin = item.Pinyin;
    hot.Keywords = item.Keywords ? std::wstring_view(item.Keywords) : std::wstring_view{};
    hot.ExtraKeywords = item.ExtraKeywords;
    hot.Type = item.Type;

    uint64_t mask = ComputeAsciiMask(hot.Name);
    mask = ComputeAsciiMask(hot.AltName, mask);
    mask = ComputeAsciiMask(hot.Pinyin, mask);
    mask = ComputeAsciiMask(hot.Keywords, mask);
    mask = ComputeAsciiMask(hot.ExtraKeywords, mask);
    hot.AsciiMask = mask;
    return hot;
}

// 将源 Item 内容原地赋值给目标 Item，复用目标原有的 wstring 堆缓冲区，且不拷贝 UI 渲染无关的 Pinyin 与 ExtraKeywords
inline void CopyItemReuseCapacity(SearchResultItem &dst, const SearchResultItem &src)
{
    dst.Name.assign(src.Name);
    dst.Pinyin.clear();
    dst.ActionPath.assign(src.ActionPath);
    dst.TargetExePath.assign(src.TargetExePath);
    dst.ExtraKeywords.clear();
    dst.AltName = src.AltName;
    dst.ActionArgs = src.ActionArgs;
    dst.Keywords = src.Keywords;
    dst.IconPath = src.IconPath;
    dst.IconId = src.IconId;
    dst.Type = src.Type;
}

// 判断 text[index] 是否为“单词/音节首字母或驼峰锚点”
// 区分是“全新单词/音节的起点”（用于计算跳词惩罚），还是“全大写缩写词内部的字母”（如 OBS 中的 B）
inline bool IsSubsequenceAnchor(std::wstring_view text, size_t index, bool &outIsNewWord) noexcept
{
    outIsNewWord = false;
    if (index >= text.size())
        return false;

    wchar_t curr = text[index];

    // 分隔符本身绝不作为匹配锚点
    if (curr == L' ' || curr == L'-' || curr == L'_' || curr == L'.' ||
        curr == L'/' || curr == L'\\' || curr == L'(' || curr == L')' ||
        curr == L'[' || curr == L']' || curr == L':')
    {
        return false;
    }

    if (index == 0)
    {
        outIsNewWord = true;
        return true;
    }

    wchar_t prev = text[index - 1];

    // 1. 紧跟在分隔符之后的字符，必然是新单词首字母
    if (prev == L' ' || prev == L'-' || prev == L'_' || prev == L'.' ||
        prev == L'/' || prev == L'\\' || prev == L'(' || prev == L'[')
    {
        outIsNewWord = true;
        return true;
    }

    // 2. 驼峰边界：小写字母后紧跟大写字母（如 PowerShell 的 S，WeiXin 的 X）
    if (curr >= L'A' && curr <= L'Z' && prev >= L'a' && prev <= L'z')
    {
        outIsNewWord = true;
        return true;
    }

    // 3. 连续大写字母缩写（如 OBS Studio 或 LLVMCompiler）：
    if (curr >= L'A' && curr <= L'Z' && prev >= L'A' && prev <= L'Z')
    {
        if (index + 1 < text.size() && text[index + 1] >= L'a' && text[index + 1] <= L'z')
        {
            outIsNewWord = true;
        }
        return true;
    }

    // 4. 字母与数字交界，或 ASCII 与中文字符交界
    auto charClass = [](wchar_t ch) noexcept -> int {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'))
            return 1; // 英文
        if (ch >= L'0' && ch <= L'9')
            return 2; // 数字
        if (ch >= 0x80)
            return 3; // 中文/Unicode
        return 0;     // 符号
    };

    int prevClass = charClass(prev);
    int currClass = charClass(curr);
    if (currClass != 0 && prevClass != currClass)
    {
        outIsNewWord = true;
        return true;
    }

    return false;
}

// 统计文本中锚点（新单词/驼峰音节）的总数，用于判断缩写是否完整覆盖了所有音节
inline int CountTotalNewWords(std::wstring_view text) noexcept
{
    int count = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        bool isNewWord = false;
        if (IsSubsequenceAnchor(text, i, isNewWord) && isNewWord)
        {
            ++count;
        }
    }
    return count;
}

// 混合子序列状态机打分算法（零内存分配）
// 参数说明：
//   - allowInternalJumps: 是否允许同词内部辅音骨架跳字（英文主名称为 true，拼音与关键词为 false）
//   - allowSingleAnchorPrefix: 是否允许单锚点连续命中（拼音通道为 true，支持单声母 "w" 或单字全拼 "xun" -> "XunLei"）
int EvaluateWordSubsequence(std::wstring_view text,
                            std::wstring_view lowerQuery,
                            bool allowInternalJumps,
                            bool allowSingleAnchorPrefix) noexcept
{
    const size_t qLen = lowerQuery.size();
    const size_t tLen = text.size();
    if (qLen == 0 || qLen > tLen)
        return 0;
    if (qLen < 2 && !allowSingleAnchorPrefix)
        return 0;

    const int maxAllowedJumps = allowInternalJumps
                                    ? static_cast<int>((std::min)(static_cast<size_t>(3), qLen > 1 ? qLen - 1 : 0))
                                    : 0;
    const int totalNewWords = CountTotalNewWords(text);
    int remainingBudget = 160;

    auto dfs = [&](auto &self, size_t qIdx, size_t tIdx, int lastMatchedIdx,
                   int anchorHits, int consecutiveHits, int internalJumps, int internalGapChars,
                   int skippedNewWords, int firstHitIdx) noexcept -> int {
        if (--remainingBudget < 0)
            return 0;

        if (qIdx == qLen)
        {
            // 检查最后一个匹配字符是否恰好完整闭合了一个单词/音节
            bool dummyNewWord = false;
            size_t nextPos = static_cast<size_t>(lastMatchedIdx + 1);
            bool isWordCompleted = (nextPos == tLen) ||
                                   (text[nextPos] == L' ' || text[nextPos] == L'-' || text[nextPos] == L'_' ||
                                    text[nextPos] == L'.' || text[nextPos] == L')' || text[nextPos] == L']') ||
                                   IsSubsequenceAnchor(text, nextPos, dummyNewWord);

            // 【梯队 A】：零内部跳字（跨词/跨音节首字母缩写、驼峰缩写、音节前缀组合，如 vsc / vscode / wx / weixin / xun）
            if (internalJumps == 0)
            {
                if (anchorHits < 2)
                {
                    // 单锚点连续命中（仅在拼音通道开启 allowSingleAnchorPrefix 时放行，解决单声母 'w' 或单字全拼 'xun'）
                    if (!allowSingleAnchorPrefix || anchorHits < 1)
                        return 0;

                    int score = 365;
                    score += consecutiveHits * 20;
                    if (isWordCompleted)
                        score += 35; // 完整拼完整个单字音节（如 "xun" 命中 "XunLei" 的 "Xun"）

                    if (firstHitIdx == 0)
                        score += 45;
                    else
                        score -= firstHitIdx * 5 + skippedNewWords * 22;

                    score -= static_cast<int>(tLen - qLen);
                    return (std::max)(score, 150);
                }

                int score = 340;
                score += anchorHits * 45;      // 单词/音节首字母强奖励
                score += consecutiveHits * 18; // 单词/音节前缀连续命中奖励

                // 纯首字母缩写大奖（如 "vsc" 或 "wx" 每个字母都是首字母锚点！）
                if (anchorHits == static_cast<int>(qLen))
                {
                    score += 55;
                    if (firstHitIdx == 0 && skippedNewWords == 0)
                    {
                        score += 35; // 从第 1 个词/音节起连续命中首字母

                        // 若恰好一字不落地命中了所有单词/音节的首字母（如 "wx" -> "WeiXin" 2/2, "kzmb" -> "KongZhiMianBan" 4/4）
                        if (static_cast<int>(qLen) == totalNewWords)
                        {
                            score = (std::max)(score, 860 - static_cast<int>(tLen - qLen));
                        }
                    }
                }
                // 前缀 + 尾词/尾音节完整闭合奖励（如 "vscode" 拼完 "Code"，或 "weixin" / "wxin" 拼完最后一个音节）
                else if (isWordCompleted && consecutiveHits >= 1)
                {
                    score += 30;
                    // 若从头开始零跳词，且最后一个音节也完整闭合（如 "wxin" 命中 "WeiXin"），给予准全拼高分激励
                    if (firstHitIdx == 0 && skippedNewWords == 0 && anchorHits == totalNewWords)
                    {
                        score += 60;
                    }
                }

                if (firstHitIdx == 0)
                    score += 35;
                else
                    score -= firstHitIdx * 4;

                score -= skippedNewWords * 18;
                score -= static_cast<int>(tLen - qLen);
                return (std::max)(score, 160);
            }

            // 【梯队 B】：包含同词内部跳字（辅音骨架/遗漏元音，如 stll -> Stellarium, tlg / tlgm -> Telegram）
            if (!allowInternalJumps || anchorHits < 1)
                return 0;

            if (consecutiveHits == 0 && anchorHits == 1 && internalGapChars > static_cast<int>(qLen * 2))
                return 0;

            int score = 200;
            score += anchorHits * 30;
            score += consecutiveHits * 24;
            if (isWordCompleted)
                score += 15;

            score -= internalJumps * 28;
            score -= internalGapChars * 7;
            score -= skippedNewWords * 20;

            if (firstHitIdx == 0)
                score += 25;
            else
                score -= firstHitIdx * 5;

            score -= static_cast<int>(tLen - qLen);

            return (score >= 100) ? score : 0;
        }

        wchar_t target = lowerQuery[qIdx];
        int best = 0;
        int localSkippedWords = 0;
        bool crossedWordBoundary = false;
        bool foundValidInternalJumpAtThisLevel = false;

        for (size_t i = tIdx; i < tLen; ++i)
        {
            if (tLen - i < qLen - qIdx)
                break;

            bool isNewWord = false;
            bool isAnchor = IsSubsequenceAnchor(text, i, isNewWord);
            bool isConsecutive = (lastMatchedIdx >= 0 && static_cast<int>(i) == lastMatchedIdx + 1);

            if (isNewWord && firstHitIdx != -1)
            {
                crossedWordBoundary = true;
            }

            if (ToLowerCharFast(text[i]) == target)
            {
                // 路径 1：命中单词/音节首字母锚点，或紧邻上一个字符连续延伸（正规路径）
                if (isAnchor || isConsecutive)
                {
                    int nextFirstHit = (firstHitIdx == -1) ? static_cast<int>(i) : firstHitIdx;
                    int subScore = self(self,
                                        qIdx + 1,
                                        i + 1,
                                        static_cast<int>(i),
                                        anchorHits + (isAnchor ? 1 : 0),
                                        consecutiveHits + (!isAnchor && isConsecutive ? 1 : 0),
                                        internalJumps,
                                        internalGapChars,
                                        skippedNewWords + localSkippedWords,
                                        nextFirstHit);
                    if (subScore > best)
                        best = subScore;
                }
                // 路径 2：同词内部辅音骨架跳字（仅限英文主名称开启 allowInternalJumps 时生效）
                else if (allowInternalJumps && !foundValidInternalJumpAtThisLevel &&
                         firstHitIdx != -1 && qLen >= 3 && !crossedWordBoundary && internalJumps < maxAllowedJumps)
                {
                    int gap = static_cast<int>(i) - lastMatchedIdx - 1;
                    if (gap >= 1 && gap <= 4)
                    {
                        int subScore = self(self,
                                            qIdx + 1,
                                            i + 1,
                                            static_cast<int>(i),
                                            anchorHits,
                                            consecutiveHits,
                                            internalJumps + 1,
                                            internalGapChars + gap,
                                            skippedNewWords + localSkippedWords,
                                            firstHitIdx);
                        if (subScore > 0)
                        {
                            foundValidInternalJumpAtThisLevel = true;
                            if (subScore > best)
                                best = subScore;
                        }
                    }
                }
            }

            if (isNewWord && firstHitIdx != -1)
            {
                ++localSkippedWords;
            }
        }

        return best;
    };

    return dfs(dfs, 0, 0, -1, 0, 0, 0, 0, 0, -1);
}

// 评估以空格分隔的关键词列表（支持保留驼峰大小写的 Keywords 与 ExtraKeywords）
int EvaluateKeywordList(std::wstring_view kwList,
                        std::wstring_view rawQuery,
                        std::wstring_view lowerQuery,
                        int nameLen) noexcept
{
    if (kwList.empty())
        return 0;

    int bestKwScore = 0;
    const int qLen = static_cast<int>(lowerQuery.size());

    size_t pos = 0;
    while (pos < kwList.size())
    {
        while (pos < kwList.size() && kwList[pos] == L' ')
            ++pos;
        if (pos >= kwList.size())
            break;

        size_t end = kwList.find(L' ', pos);
        if (end == std::wstring_view::npos)
            end = kwList.size();

        std::wstring_view token = kwList.substr(pos, end - pos);
        const int tokenLen = static_cast<int>(token.size());

        // 1. 连续子串/前缀匹配（大小写不敏感）
        int tokenMatch = FindSubstringCaseInsensitive(token, rawQuery);
        if (tokenMatch == 0)
        {
            if (tokenLen == qLen)
            {
                int kwExact = (std::max)(500, 850 - nameLen);
                bestKwScore = (std::max)(bestKwScore, kwExact);
            }
            else
            {
                int kwPrefix = (std::max)(250, 480 - (tokenLen - qLen) * 3 - nameLen);
                bestKwScore = (std::max)(bestKwScore, kwPrefix);
            }
        }
        else if (tokenMatch > 0)
        {
            bool isNewWord = false;
            bool isBoundary = IsSubsequenceAnchor(token, static_cast<size_t>(tokenMatch), isNewWord);
            if (isBoundary || qLen >= 3)
            {
                int bonus = isBoundary ? 80 : 0;
                int kwSub = (std::max)(15, 200 + bonus - tokenMatch * 4 - (tokenLen - qLen) - nameLen);
                bestKwScore = (std::max)(bestKwScore, kwSub);
            }
        }

        // 2. 驼峰首字母与音节前缀子序列匹配（如 WeChat -> wc, WindowsTerminal -> wt, XiTongBanBen -> xtbb）
        //    关闭内部跳字 (allowInternalJumps = false)，防止拼音型 Keyword 产生音节内跳字误报
        if (bestKwScore < 750 && qLen >= 2 && tokenLen > qLen)
        {
            int subseq = EvaluateWordSubsequence(token, lowerQuery, false, false);
            if (subseq > 0)
            {
                int adjusted = (std::max)(80, subseq - 35 - nameLen);
                bestKwScore = (std::max)(bestKwScore, adjusted);
            }
        }

        pos = end + 1;
    }

    return bestKwScore;
}

// 核心主名称匹配（连续子串/前缀匹配 + 跨词首字母与同词辅音骨架子序列匹配）
int EvaluatePrimaryNameMatch(std::wstring_view primaryName,
                             std::wstring_view rawQuery,
                             std::wstring_view lowerQuery,
                             ItemType type) noexcept
{
    if (primaryName.empty())
        return 0;

    int bestScore = 0;
    const int qLen = static_cast<int>(lowerQuery.size());
    const int nameLen = static_cast<int>(primaryName.size());

    // 1. 匹配核心主名称连续子串 (大小写不敏感)
    int namePos = FindSubstringCaseInsensitive(primaryName, rawQuery);
    if (namePos == 0)
    {
        if (nameLen == qLen)
        {
            return (type == ItemType::Command) ? 1030 : 1000; // 名称完全相等，直接短路返回最高分
        }
        else
        {
            int prefixScore = (std::max)(400, 650 - (nameLen - qLen) * 2);
            bestScore = (std::max)(bestScore, prefixScore); // 名称连续前缀匹配
        }
    }
    else if (namePos > 0)
    {
        bool isNewWord = false;
        int boundaryBonus = IsSubsequenceAnchor(primaryName, static_cast<size_t>(namePos), isNewWord) ? 90 : 0;
        int subStrScore = (std::max)(20, 300 + boundaryBonus - namePos * 3 - (nameLen - qLen));
        bestScore = (std::max)(bestScore, subStrScore);
    }

    // 2. 匹配核心主名称的子序列（高分跨词首字母 vsc/vscode + 低分同词辅音跳字 stll/tlgm）
    if (bestScore < 600 && qLen >= 2)
    {
        int subseqScore = EvaluateWordSubsequence(primaryName, lowerQuery, true, false);
        bestScore = (std::max)(bestScore, subseqScore);
    }

    return bestScore;
}

// 主打分函数：直接基于紧凑的 HotSearchEntry 进行零内存分配评估
int EvaluateItemMatch(const HotSearchEntry &item,
                      std::wstring_view rawQuery,
                      std::wstring_view lowerQuery) noexcept
{
    const int qLen = static_cast<int>(lowerQuery.size());
    const int exactFullScore = (item.Type == ItemType::Command) ? 1030 : 1000;

    // 1. 匹配当前展示主名称 Name（连续子串/前缀 + 跨词首字母/同词辅音跳字子序列）
    int bestScore = EvaluatePrimaryNameMatch(item.Name, rawQuery, lowerQuery, item.Type);
    if (bestScore >= exactFullScore)
    {
        return bestScore;
    }

    // 2. 双语核心名称同权匹配：若条目拥有另一语言核心主名称 AltName，以完全相同的 Primary Name 级别参与匹配
    if (!item.AltName.empty())
    {
        int altScore = EvaluatePrimaryNameMatch(item.AltName, rawQuery, lowerQuery, item.Type);
        if (altScore >= exactFullScore)
        {
            return altScore;
        }
        bestScore = (std::max)(bestScore, altScore);
    }

    // 3. 大一统匹配驼峰全拼 Pinyin（仅含汉字的条目 Pinyin 非空，如 "WeiXin"、"KongZhiMianBan"）
    //    - 严禁音节内部跳字 (allowInternalJumps = false)
    //    - 允许单音节前缀/单声母匹配 (allowSingleAnchorPrefix = true)
    if (!item.Pinyin.empty())
    {
        const int pyLen = static_cast<int>(item.Pinyin.size());

        // 3.1 快速检测全拼完全相等（如输入 "weixin" 匹配 "WeiXin"）
        if (pyLen == qLen && FindSubstringCaseInsensitive(item.Pinyin, rawQuery) == 0)
        {
            bestScore = (std::max)(bestScore, 920);
        }
        else
        {
            int pyScore = EvaluateWordSubsequence(item.Pinyin, lowerQuery, false, true);
            bestScore = (std::max)(bestScore, pyScore);
        }
    }

    // 4. 匹配静态关键词 Keywords 与动态提取关键词 ExtraKeywords
    const int nameLen = item.AltName.empty()
                            ? static_cast<int>(item.Name.size())
                            : static_cast<int>((std::min)(item.Name.size(), item.AltName.size()));
    if (!item.Keywords.empty())
    {
        int kwScore = EvaluateKeywordList(item.Keywords, rawQuery, lowerQuery, nameLen);
        bestScore = (std::max)(bestScore, kwScore);
    }
    if (!item.ExtraKeywords.empty())
    {
        int extraKwScore = EvaluateKeywordList(item.ExtraKeywords, rawQuery, lowerQuery, nameLen);
        bestScore = (std::max)(bestScore, extraKwScore);
    }

    // 内置指令微调加权
    if (bestScore > 0 && item.Type == ItemType::Command)
    {
        bestScore += 30;
    }

    return bestScore;
}
} // namespace

void SearchManager::Shutdown()
{
    auto &state = GetCacheState();
    state.IsShuttingDown.store(true, std::memory_order_release);

    std::unique_lock<std::mutex> lock(state.LifecycleMutex);
    if (state.IsThreadActive)
    {
        // 最多等待 150ms 让后台线程退出
        bool finished = state.ScanExitCv.wait_for(lock, std::chrono::milliseconds(150), [&state]() {
            return !state.IsThreadActive;
        });

        if (state.ScanThread.joinable())
        {
            if (finished)
            {
                state.ScanThread.join();
            }
            else
            {
                // 极端情况下（如后台线程阻塞在休眠磁盘 IO），超时后降级为 detach 避免卡死主界面退出
                state.ScanThread.detach();
                state.IsThreadActive = false;
            }
        }
    }
    else if (state.ScanThread.joinable())
    {
        state.ScanThread.join();
    }
}

bool SearchManager::IsShuttingDown()
{
    return GetCacheState().IsShuttingDown.load(std::memory_order_acquire);
}

void SearchManager::RequestRescanAsync(HWND notifyHwnd, UINT notifyMsg)
{
    auto &state = GetCacheState();
    if (state.IsShuttingDown.load(std::memory_order_acquire))
        return;

    if (notifyHwnd && notifyMsg != 0)
    {
        state.NotifyHwnd.store(notifyHwnd, std::memory_order_release);
        state.NotifyMsg.store(notifyMsg, std::memory_order_release);
    }

    state.RescanRequested.store(true, std::memory_order_release);

    bool expected = false;
    if (!state.IsScanning.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(state.LifecycleMutex);
    if (state.IsShuttingDown.load(std::memory_order_acquire))
    {
        state.IsScanning.store(false, std::memory_order_release);
        return;
    }

    // 回收上一次已经跑完的线程句柄
    if (state.ScanThread.joinable())
    {
        state.ScanThread.join();
    }

    state.IsThreadActive = true;
    state.ScanThread = std::thread([]() {
        SearchManager::InitializeCache();

        auto &st = GetCacheState();
        HWND targetHwnd = st.NotifyHwnd.exchange(nullptr, std::memory_order_acq_rel);
        UINT targetMsg = st.NotifyMsg.exchange(0, std::memory_order_acq_rel);
        if (!SearchManager::IsShuttingDown() && targetHwnd && targetMsg != 0 && IsWindow(targetHwnd))
        {
            PostMessageW(targetHwnd, targetMsg, 0, 0);
        }

        {
            std::lock_guard<std::mutex> lk(st.LifecycleMutex);
            st.IsThreadActive = false;
        }
        st.ScanExitCv.notify_all();
    });
}

void SearchManager::InitializeCache()
{
    auto &state = GetCacheState();

    // 在循环外统一初始化一次纯净 STA 套间：
    // 1. 彻底杜绝同一线程在 MTA 与 STA 之间反复切换引发的 COM 内部状态残留
    // 2. 确保 CLSID_ShellLink (注册表 ThreadingModel=Apartment) 100% 进程内直连实例化，无跨套间 Proxy 封送开销
    HRESULT hrSta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    for (;;)
    {
        while (state.RescanRequested.exchange(false, std::memory_order_acq_rel))
        {
            if (state.IsShuttingDown.load(std::memory_order_acquire))
                break;

            std::vector<SearchResultItem> tempCache;
            tempCache.reserve(512);

            const bool isEn = I18n::IsEnglish();

            // 1. 注入内置指令：设置 LiteLauncher（支持中英双语核心主名称同权匹配与驼峰全拼）
            SearchResultItem settingsCmd;
            settingsCmd.Name = isEn ? I18n::kCmdSettingsEn : I18n::kCmdSettingsZh;
            settingsCmd.AltName = isEn ? I18n::kCmdSettingsZh : I18n::kCmdSettingsEn;
            settingsCmd.Pinyin = L"SheZhiLiteLauncher";
            settingsCmd.Keywords = L"settings config";
            settingsCmd.ActionPath = L"internal://settings";
            settingsCmd.Type = ItemType::Command;
            tempCache.push_back(std::move(settingsCmd));

            // 2. 注入内置指令：刷新缓存
            SearchResultItem refreshCmd;
            refreshCmd.Name = isEn ? I18n::kCmdRefreshEn : I18n::kCmdRefreshZh;
            refreshCmd.AltName = isEn ? I18n::kCmdRefreshZh : I18n::kCmdRefreshEn;
            refreshCmd.Pinyin = L"ShuaXinLiteLauncher";
            refreshCmd.Keywords = L"refresh reload";
            refreshCmd.ActionPath = L"internal://refresh";
            refreshCmd.Type = ItemType::Command;
            tempCache.push_back(std::move(refreshCmd));

            // 3. 注入内置指令：退出应用
            SearchResultItem exitCmd;
            exitCmd.Name = isEn ? I18n::kCmdExitEn : I18n::kCmdExitZh;
            exitCmd.AltName = isEn ? I18n::kCmdExitZh : I18n::kCmdExitEn;
            exitCmd.Pinyin = L"TuiChuLiteLauncher";
            exitCmd.Keywords = L"exit quit";
            exitCmd.ActionPath = L"internal://exit";
            exitCmd.Type = ItemType::Command;
            tempCache.push_back(std::move(exitCmd));

            // 加载系统设置搜索项（纯静态内存操作，不依赖 COM）
            auto settings = SettingsProvider::GetSettingsItems();
            tempCache.insert(tempCache.end(), std::make_move_iterator(settings.begin()),
                             std::make_move_iterator(settings.end()));
            if (state.IsShuttingDown.load(std::memory_order_acquire))
                break;

            // 阶段 A：在独立纯净的 MTA 子线程中加载 UWP 应用，物理隔离 MTA 与当前 STA 套间，避免套间切换与 STA 阻塞死锁
            {
                std::vector<SearchResultItem> uwps;
                std::thread uwpWorker([&uwps]() {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    uwps = UwpProvider::ScanUwpApps();
                    // 必须先清除 C++/WinRT 全局静态激活工厂缓存，否则 Windows.Management.Deployment.dll 引用计数永不为 0
                    winrt::clear_factory_cache();
                    CoFreeUnusedLibrariesEx(0, 0);
                    winrt::uninit_apartment();
                });
                uwpWorker.join();

                tempCache.insert(tempCache.end(), std::make_move_iterator(uwps.begin()),
                                 std::make_move_iterator(uwps.end()));
            }
            if (state.IsShuttingDown.load(std::memory_order_acquire))
                break;

            // 阶段 B：直接在当前 STA 线程加载 Win32 应用（CLSID_ShellLink 原生运行于 STA，零封送开销）
            {
                auto win32Apps = Win32Provider::ScanStartMenuApps();
                tempCache.insert(tempCache.end(), std::make_move_iterator(win32Apps.begin()),
                                 std::make_move_iterator(win32Apps.end()));
            }
            if (state.IsShuttingDown.load(std::memory_order_acquire))
                break;

            // 全局去重与自身过滤（使用 64 位 FNV-1a 联合哈希：结合 Name 与实际执行目标去重，保留同名但不同版本的合法程序）
            constexpr uint64_t kSelfNameHash = HashStringFNV1a(L"LiteLauncher");
            std::vector<SearchResultItem> finalColdCache;
            finalColdCache.reserve(tempCache.size());

            {
                std::unordered_set<uint64_t> seenItemHashes;
                seenItemHashes.reserve(tempCache.size());

                for (auto &item : tempCache)
                {
                    uint64_t nameHash = HashStringCaseInsensitiveFNV1a(item.Name);
                    if (item.Type != ItemType::Command && nameHash == kSelfNameHash)
                    {
                        continue;
                    }

                    // 区分不同目标的同名应用（如不同版本的 Python IDLE 或不同目录下的同名工具）
                    std::wstring_view identityTarget = !item.TargetExePath.empty()
                                                           ? std::wstring_view(item.TargetExePath)
                                                           : std::wstring_view(item.ActionPath);
                    uint64_t itemHash = HashStringCaseInsensitiveFNV1a(identityTarget, HashStringFNV1a(L"|", nameHash));

                    if (seenItemHashes.insert(itemHash).second)
                    {
                        finalColdCache.push_back(std::move(item));
                    }
                }
            }

            // 立即销毁峰值期的临时容器 tempCache，防止后续长生命周期缓存在堆高水位末端分配而钉死堆段
            {
                std::vector<SearchResultItem> emptyTemp;
                tempCache.swap(emptyTemp);
            }

            if (state.IsShuttingDown.load(std::memory_order_acquire))
                break;

            // 收缩每个常驻条目内部字符串的冗余容量（如 GetCamelPinyin 预留的 4 倍容量），再固定 finalColdCache 容量
            for (auto &coldItem : finalColdCache)
            {
                coldItem.Name.shrink_to_fit();
                coldItem.Pinyin.shrink_to_fit();
                coldItem.ActionPath.shrink_to_fit();
                coldItem.TargetExePath.shrink_to_fit();
                coldItem.ExtraKeywords.shrink_to_fit();
            }
            finalColdCache.shrink_to_fit();

            std::vector<HotSearchEntry> finalHotCache;
            finalHotCache.reserve(finalColdCache.size());
            for (const auto &coldItem : finalColdCache)
            {
                finalHotCache.push_back(BuildHotEntry(coldItem));
            }

            {
                // 写锁下同步原子交换冷热双缓存，保证读线程看到的视图与底层字符串生命周期绝对一致
                std::unique_lock<std::shared_mutex> lock(state.Mutex);
                state.ColdCache.swap(finalColdCache);
                state.HotCache.swap(finalHotCache);
            }
        }

        state.IsScanning.store(false, std::memory_order_release);
        if (state.IsShuttingDown.load(std::memory_order_acquire) ||
            !state.RescanRequested.load(std::memory_order_acquire))
        {
            break;
        }
        bool expected = false;
        if (!state.IsScanning.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            break;
        }
    }

    if (SUCCEEDED(hrSta))
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        winrt::clear_factory_cache();
        CoFreeUnusedLibrariesEx(0, 0);
        CoUninitialize();
        CoFreeUnusedLibrariesEx(0, 0);
    }
}

void SearchManager::Search(const std::wstring &query, std::vector<SearchResultItem> &outResults)
{
    if (query.empty())
    {
        outResults.clear();
        return;
    }

    auto &state = GetCacheState();
    if (state.IsShuttingDown.load(std::memory_order_acquire))
    {
        outResults.clear();
        return;
    }

    // 栈上固定缓冲区转换小写（长度 < 128 时 100% 零堆分配）
    wchar_t stackLowerBuf[128];
    std::wstring heapLowerBuf;
    std::wstring_view lowerQuery;
    if (query.size() < _countof(stackLowerBuf))
    {
        for (size_t i = 0; i < query.size(); ++i)
        {
            stackLowerBuf[i] = ToLowerCharFast(query[i]);
        }
        lowerQuery = std::wstring_view(stackLowerBuf, query.size());
    }
    else
    {
        heapLowerBuf = ToLower(query);
        lowerQuery = heapLowerBuf;
    }

    const uint64_t queryAsciiMask = ComputeAsciiMask(lowerQuery);

    struct MatchCandidate
    {
        size_t index;
        int score;
    };

    // 直接在栈上固定大小数组中维护 Top 7 结果，彻底消灭每次击键时 std::vector<MatchCandidate> 的堆分配与扩容
    constexpr size_t MAX_RESULTS = 7;
    MatchCandidate topCandidates[MAX_RESULTS]{};
    size_t topCount = 0;

    std::shared_lock<std::shared_mutex> lock(state.Mutex);
    const auto &hotCache = state.HotCache;
    const auto &coldCache = state.ColdCache;
    const size_t cacheSize = hotCache.size();

    auto isBetterCandidate = [&hotCache, &coldCache](const MatchCandidate &a, const MatchCandidate &b) noexcept {
        if (a.score != b.score)
            return a.score > b.score;
        const auto &hotA = hotCache[a.index];
        const auto &hotB = hotCache[b.index];
        if (hotA.Type == ItemType::Command && hotB.Type != ItemType::Command)
            return true;
        if (hotB.Type == ItemType::Command && hotA.Type != ItemType::Command)
            return false;
        if (hotA.Name != hotB.Name)
            return hotA.Name < hotB.Name;
        return coldCache[a.index].ActionPath < coldCache[b.index].ActionPath;
    };

    for (size_t i = 0; i < cacheSize; ++i)
    {
        const auto &hotItem = hotCache[i];

        // O(1) 64-bit ASCII 位图极速预过滤：若查询中包含任何该条目未出现的 ASCII 字母/数字，直接剪枝跳过
        if ((hotItem.AsciiMask & queryAsciiMask) != queryAsciiMask)
        {
            continue;
        }

        int score = EvaluateItemMatch(hotItem, query, lowerQuery);
        if (score > 0)
        {
            MatchCandidate cand{i, score};
            if (topCount < MAX_RESULTS)
            {
                size_t insertPos = topCount;
                while (insertPos > 0 && isBetterCandidate(cand, topCandidates[insertPos - 1]))
                {
                    topCandidates[insertPos] = topCandidates[insertPos - 1];
                    --insertPos;
                }
                topCandidates[insertPos] = cand;
                ++topCount;
            }
            else if (isBetterCandidate(cand, topCandidates[MAX_RESULTS - 1]))
            {
                size_t insertPos = MAX_RESULTS - 1;
                while (insertPos > 0 && isBetterCandidate(cand, topCandidates[insertPos - 1]))
                {
                    topCandidates[insertPos] = topCandidates[insertPos - 1];
                    --insertPos;
                }
                topCandidates[insertPos] = cand;
            }
        }
    }

    // 仅对最终胜出的 Top N 结果访问冷数据，复用已有元素的内部 std::wstring 堆容量
    outResults.resize(topCount);
    for (size_t i = 0; i < topCount; ++i)
    {
        CopyItemReuseCapacity(outResults[i], coldCache[topCandidates[i].index]);
    }
}

void SearchManager::LaunchItem(const SearchResultItem &targetApp)
{
    if (targetApp.ActionPath.empty() && targetApp.TargetExePath.empty())
        return;

    // 1. 100ms 原子 CAS 时间戳防抖（消除 Check-Then-Act 数据竞争，防止键盘回车物理抖动瞬发多次，兼顾极客连按响应）
    // 2. 在途并发配额原子计数（最多允许 4 个慢速 IO/UAC 请求并发，避免慢盘拖垮后续进程唤起）
    static std::atomic<ULONGLONG> s_lastLaunchTick{0};
    static std::atomic<int> s_activeLaunchCount{0};

    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = s_lastLaunchTick.load(std::memory_order_relaxed);
    do
    {
        if (now - last < 100)
            return;
    } while (!s_lastLaunchTick.compare_exchange_weak(
        last, now, std::memory_order_acq_rel, std::memory_order_relaxed));

    // 原子递增占位，若超过并发上限（4）则立刻原子回退并拦截
    if (s_activeLaunchCount.fetch_add(1, std::memory_order_acq_rel) >= 4)
    {
        s_activeLaunchCount.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    struct LaunchTask
    {
        ItemType type;
        std::wstring actionPath;
        std::wstring targetExePath;
        const wchar_t *actionArgs;
    };

    LaunchTask task{targetApp.Type, targetApp.ActionPath, targetApp.TargetExePath, targetApp.ActionArgs};

    try
    {
        std::thread([t = std::move(task)]() {
            struct LaunchScopeGuard
            {
                ~LaunchScopeGuard() noexcept
                {
                    s_activeLaunchCount.fetch_sub(1, std::memory_order_acq_rel);
                }
            } guard;

            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

            auto shellExecSync = [](const wchar_t *file, const wchar_t *params, const wchar_t *dir) {
                SHELLEXECUTEINFOW sei = {sizeof(sei)};
                sei.fMask = SEE_MASK_NOASYNC;
                sei.lpVerb = L"open";
                sei.lpFile = file;
                sei.lpParameters = params;
                sei.lpDirectory = dir;
                sei.nShow = SW_SHOWNORMAL;
                ShellExecuteExW(&sei);
            };

            if (t.type == ItemType::UWP)
            {
                winrt::com_ptr<IApplicationActivationManager> activator;
                if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                                               IID_PPV_ARGS(activator.put()))))
                {
                    DWORD pid = 0;
                    activator->ActivateApplication(t.actionPath.c_str(), nullptr, AO_NONE, &pid);
                }
                else
                {
                    std::wstring shellUri = L"shell:AppsFolder\\" + t.actionPath;
                    shellExecSync(shellUri.c_str(), nullptr, nullptr);
                }
            }
            else if (t.type == ItemType::Win32)
            {
                std::wstring workDir;
                // 若 ActionPath 本身是 .lnk 快捷方式，必须传 nullptr 让系统 Shell 原生使用 .lnk 内置的“起始位置”和“参数”；
                // 仅当直接启动非 .lnk 文件（如直连 .exe）时，才提取其父目录作为默认工作目录。
                if (!EndsWithCaseInsensitive(t.actionPath, L".lnk"))
                {
                    const std::wstring &realExe = !t.targetExePath.empty() ? t.targetExePath : t.actionPath;
                    if (!realExe.empty())
                    {
                        std::error_code ec;
                        std::filesystem::path p(realExe);
                        if (p.has_parent_path() && std::filesystem::exists(p.parent_path(), ec))
                        {
                            workDir = p.parent_path().wstring();
                        }
                    }
                }
                shellExecSync(t.actionPath.c_str(), nullptr, workDir.empty() ? nullptr : workDir.c_str());
            }
            else if (t.type == ItemType::Settings)
            {
                const std::wstring &exeToRun = !t.targetExePath.empty() ? t.targetExePath : t.actionPath;
                const wchar_t *args = (t.actionArgs && t.actionArgs[0] != L'\0') ? t.actionArgs : nullptr;
                if (!exeToRun.empty())
                {
                    // 静态缓存 System32 与 Windows 目录，避免每次启动重复发起系统调用
                    static const std::wstring s_sysDir = []() {
                        WCHAR buf[MAX_PATH];
                        UINT len = GetSystemDirectoryW(buf, MAX_PATH);
                        return (len > 0 && len < MAX_PATH) ? std::wstring(buf, len) : std::wstring();
                    }();

                    static const std::wstring s_winDir = []() {
                        WCHAR buf[MAX_PATH];
                        UINT len = GetWindowsDirectoryW(buf, MAX_PATH);
                        return (len > 0 && len < MAX_PATH) ? std::wstring(buf, len) : std::wstring();
                    }();

                    const wchar_t *safeWorkDir = s_sysDir.empty() ? nullptr : s_sysDir.c_str();

                    // 若是通过 explorer.exe 打开 "shell:..." 特殊文件夹，直接交由 ShellExecuteExW 打开该协议。
                    if (args && FindSubstringCaseInsensitive(args, L"shell:") == 0 &&
                        (EndsWithCaseInsensitive(exeToRun, L"explorer.exe") ||
                         EndsWithCaseInsensitive(exeToRun, L"explorer")))
                    {
                        shellExecSync(args, nullptr, nullptr);
                    }
                    // 若不包含 ':'，严格在 System32 或 Windows 目录下定位绝对路径。
                    else if (exeToRun.find(L':') == std::wstring::npos && !s_sysDir.empty())
                    {
                        auto buildFullPath = [&](const std::wstring &baseDir) {
                            std::wstring fullPath = baseDir;
                            if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/')
                            {
                                fullPath.push_back(L'\\');
                            }
                            fullPath.append(exeToRun);

                            // 检查文件名部分是否缺少扩展名（如 L"control"），若无扩展名则补齐 L".exe"
                            size_t lastSlash = exeToRun.find_last_of(L"\\/");
                            std::wstring_view fileName = (lastSlash == std::wstring::npos)
                                                             ? std::wstring_view(exeToRun)
                                                             : std::wstring_view(exeToRun).substr(lastSlash + 1);
                            if (fileName.find(L'.') == std::wstring_view::npos)
                            {
                                fullPath.append(L".exe");
                            }
                            return fullPath;
                        };

                        std::wstring targetPath = buildFullPath(s_sysDir);
                        DWORD attr = GetFileAttributesW(targetPath.c_str());

                        // 若 System32 下不存在该程序（例如 explorer.exe 位于 C:\Windows 下），则安全回退至 Windows 目录
                        if ((attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) && !s_winDir.empty())
                        {
                            std::wstring winTargetPath = buildFullPath(s_winDir);
                            DWORD winAttr = GetFileAttributesW(winTargetPath.c_str());
                            if (winAttr != INVALID_FILE_ATTRIBUTES && !(winAttr & FILE_ATTRIBUTE_DIRECTORY))
                            {
                                targetPath = std::move(winTargetPath);
                            }
                        }

                        shellExecSync(targetPath.c_str(), args, safeWorkDir);
                    }
                    else
                    {
                        const bool isDrivePath =
                            (exeToRun.size() >= 3 &&
                             ((exeToRun[0] >= L'A' && exeToRun[0] <= L'Z') ||
                              (exeToRun[0] >= L'a' && exeToRun[0] <= L'z')) &&
                             exeToRun[1] == L':' && (exeToRun[2] == L'\\' || exeToRun[2] == L'/'));
                        const bool isProtocolUri =
                            (FindSubstringCaseInsensitive(exeToRun, L"ms-") == 0) ||
                            (exeToRun.find(L':') != std::wstring::npos && !isDrivePath);

                        shellExecSync(exeToRun.c_str(), args, isProtocolUri ? nullptr : safeWorkDir);
                    }
                }
            }

            if (SUCCEEDED(hr))
                CoUninitialize();
        }).detach();
    }
    catch (...)
    {
        s_activeLaunchCount.fetch_sub(1, std::memory_order_acq_rel);
    }
}
