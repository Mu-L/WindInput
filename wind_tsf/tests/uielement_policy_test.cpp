// UiElementPolicy 真值表测试
//
// 跑法（纯 C++17、不含 Win32 头，本机不需要 MSVC）：
//   g++ -std=c++17 -I../include -o uielement_policy_test uielement_policy_test.cpp && ./uielement_policy_test
// 或经 CMake：
//   cmake -B build -DWIND_TSF_TESTS=ON && cmake --build build --target uielement_policy_test
//
// 变异检验已做（2026-09-14），逐条命中：
//   - UseSnapshot 去掉 `!dirty` 那一半（= 本次修复前的行为）      → TestUseSnapshot 红
//   - UseSnapshot 的 HostDraws 换回只看 declared（= 修复前）      → TestUseSnapshot 红
//   - ShouldRefreshOnGet 去掉 contentRead 闸（= 第一版无条件补拉）→ TestRefreshCost 红
//   - HostDraws 改成 declared && readCandidates                   → TestHostDraws 红
// 改测试时请保住这个性质——一条永远绿的测试不会告诉你任何事。
//
// ⚠️ 覆盖边界：这里测的只是**判据**，不是取数。快照怎么拉、IPC 超时多久、
// UpdateUIElement 什么时候发，全在 TextService.cpp 里，这里一行也覆盖不到。
// 判据全绿 ≠ 候选喂对了——那半边只能上真机验。

#include "UiElementPolicy.h"

#include <cstdio>

namespace
{

using namespace wind::uielement;

int g_failures = 0;
const char* g_case = "";

#define CHECK(expr)                                                                      \
    do                                                                                   \
    {                                                                                    \
        if (!(expr))                                                                     \
        {                                                                                \
            std::printf("  FAIL  %s:%d  [%s]  %s\n", __FILE__, __LINE__, g_case, #expr); \
            g_failures++;                                                                \
        }                                                                                \
    } while (0)

#define CASE(name)                 \
    do                             \
    {                              \
        g_case = name;             \
        std::printf("  %s\n", name); \
    } while (0)

// 四个布尔输入的全枚举，省得手抄 16 行还抄漏。
template <typename F>
void ForEachDeclaredAndRead(F f)
{
    for (int d = 0; d < 2; ++d)
    {
        for (int r = 0; r < 2; ++r)
        {
            f(d != 0, r != 0);
        }
    }
}

void TestHostDraws()
{
    CASE("HostDraws：声明或读过，任一即算在画");
    CHECK(!HostDraws(false, false));
    CHECK(HostDraws(true, false));  // ← 宿主自己声明（真 UI-less）
    CHECK(HostDraws(false, true));  // ← 只是把候选串读走了（IMM32 桥）
    CHECK(HostDraws(true, true));
}

void TestUseSnapshot()
{
    CASE("UseSnapshot：认定在画就恒答快照，与 SetSelection 的放行判据同源");
    // 已认定在画 ⇒ 四种快照状态下都答快照。空快照时答的是 count=0，
    // 这比答占位「…」诚实：IPC 断了我们本来就无内容可画，而此时服务端已收窗，
    // 屏幕上只剩宿主画的那个，给它「…」等于显示一个错的。
    ForEachDeclaredAndRead([](bool declared, bool readCands) {
        if (!HostDraws(declared, readCands))
        {
            return;
        }
        for (int empty = 0; empty < 2; ++empty)
        {
            for (int dirty = 0; dirty < 2; ++dirty)
            {
                CHECK(UseSnapshot(declared, readCands, empty != 0, dirty != 0));
            }
        }
    });

    CASE("UseSnapshot：尚未认定时，快照空或脏一律答占位");
    CHECK(UseSnapshot(false, false, /*empty=*/false, /*dirty=*/false)); // 预取过、还没变 ⇒ 真数据
    CHECK(!UseSnapshot(false, false, /*empty=*/true, /*dirty=*/false)); // 没快照 ⇒ 占位（Chromium 要的「至少 1 条」）
    // ★ 脏这一格是本次修复的核心：候选已经变了而我们刻意没去拉，答旧快照就是在交付
    // 上一帧。旧实现缺这一半，于是合闩那次读取序列里 GetCount 答**上一键**的条数、
    // GetString 答**本键**的内容（它自己会先补拉），宿主拿到「旧条数 + 新内容」。
    CHECK(!UseSnapshot(false, false, /*empty=*/false, /*dirty=*/true)); // ← 旧实现在此变红
    CHECK(!UseSnapshot(false, false, /*empty=*/true, /*dirty=*/true));
}

void TestRefreshCost()
{
    CASE("ShouldRefreshOnGet：不脏就一次 IPC 都不发");
    for (int c = 0; c < 2; ++c)
    {
        for (int r = 0; r < 2; ++r)
        {
            CHECK(!ShouldRefreshOnGet(/*dirty=*/false, c != 0, r != 0));
        }
    }

    CASE("ShouldRefreshOnGet：闩未合时只有取内容那一次值得一趟同步 IPC");
    // ⚠ 这一格决定了「不读候选串的普通宿主键路径零额外 IPC」这条承诺成不成立：
    // GetCount 是 msctf 自己也会问的，它传 contentRead=false。
    CHECK(!ShouldRefreshOnGet(/*dirty=*/true, /*contentRead=*/false, /*readCands=*/false));
    CHECK(ShouldRefreshOnGet(/*dirty=*/true, /*contentRead=*/true, /*readCands=*/false));

    CASE("ShouldRefreshOnGet：闩合上之后取元信息也补拉（防御性，见下）");
    CHECK(ShouldRefreshOnGet(/*dirty=*/true, /*contentRead=*/false, /*readCands=*/true));
    CHECK(ShouldRefreshOnGet(/*dirty=*/true, /*contentRead=*/true, /*readCands=*/true));
}

void TestEagerRefresh()
{
    CASE("ShouldRefreshEagerly：认定在画就急刷，否则只记脏");
    ForEachDeclaredAndRead([](bool declared, bool readCands) {
        CHECK(ShouldRefreshEagerly(declared, readCands) == HostDraws(declared, readCands));
    });
}

void TestInvariantPredicate()
{
    CASE("InvariantBroken：只在「脏 ∧ 已认定在画」时为真");
    // ⚠️ 这条测的是**谓词本身**，不是「不变量成立」——那是 TextService.cpp 的性质，
    // 这里够不着。真正的看守在 _EnsureUiSnapshotFresh 里：它每次都调这个谓词，破了记
    // WARN 进环形缓冲。本用例保证那个看守的判据不会被改歪（比如写反、或漏掉读取闩）。
    CHECK(!InvariantBroken(/*dirty=*/false, /*declared=*/false, /*readCands=*/false));
    CHECK(!InvariantBroken(/*dirty=*/false, /*declared=*/true, /*readCands=*/true));
    CHECK(!InvariantBroken(/*dirty=*/true, /*declared=*/false, /*readCands=*/false)); // 正常态
    CHECK(InvariantBroken(/*dirty=*/true, /*declared=*/true, /*readCands=*/false));
    CHECK(InvariantBroken(/*dirty=*/true, /*declared=*/false, /*readCands=*/true));
    CHECK(InvariantBroken(/*dirty=*/true, /*declared=*/true, /*readCands=*/true));
}

} // namespace

int main()
{
    std::printf("UiElementPolicy tests\n");
    TestHostDraws();
    TestUseSnapshot();
    TestRefreshCost();
    TestEagerRefresh();
    TestInvariantPredicate();

    if (g_failures == 0)
    {
        std::printf("OK\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
