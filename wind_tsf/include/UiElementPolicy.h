// TSF 候选 UI 元素的**纯判据**：什么时候答快照、什么时候补拉、谁算在画候选。
//
// 从 CTextService 里抽出来只为一件事：让它们可测。这个状态机有四个互相咬合的输入
// （宿主声明 / 读取闩 / 快照空否 / 脏位）× 两种调用来路（取内容 vs 取元信息），
// 而它跑在 TSF 宿主进程里、需要真机才能观察——真值表单测是唯一能把它钉住的手段。
//
// ⚠️ 覆盖边界：这里只有判据，没有取数。快照怎么拉、IPC 超时多久、UpdateUIElement
// 什么时候发，全在 TextService.cpp 里，本文件一行也覆盖不到。判据全绿 ≠ 候选喂对了。
//
// 背景与实测证据见 docs/design/game-compat-tsf-uielement.md §1.4。
#pragma once

namespace wind
{
namespace uielement
{

/// 「宿主在画候选」——声明接管**或**实际读走过候选串。
///
/// 两个入参语气不同，合并只发生在这一个函数里：`declared` 是宿主自己说的
/// （`BeginUIElement` 回 `pbShow=FALSE` / `Show(FALSE)` / UI-less 线程），是事实；
/// `readCandidates` 是「它把候选文本取走了」的推断。上报给服务端时**必须分两位**，
/// 因为推断那一半可被 compat 规则 `host_drawn_candidates` 关掉。
constexpr bool HostDraws(bool declared, bool readCandidates)
{
    return declared || readCandidates;
}

/// getter 答真快照（true）还是答占位（false）。
///
/// - 已认定宿主在画 ⇒ 恒答快照。与 `SetSelection` / `Finalize` / `Abort` 的放行判据
///   同源；两边不同源会出现「GetCount 说有 1 条、GetString 给占位『…』、宿主照着选
///   却被 SetSelection 回 E_INVALIDARG」这种自相矛盾。
/// - 尚未认定 ⇒ 要求快照**非空且不脏**。脏意味着候选已经变了而我们刻意没去拉
///   （见 [`ShouldRefreshOnGet`] 的成本论证），这时答旧快照就是在交付上一帧。
constexpr bool UseSnapshot(bool declared, bool readCandidates, bool snapshotEmpty, bool dirty)
{
    if (HostDraws(declared, readCandidates))
    {
        return true;
    }
    return !snapshotEmpty && !dirty;
}

/// getter 入口要不要现拉一次快照。`contentRead` = 本次调用是不是在取候选内容本身
/// （只有 `GetString` 为 true）。
///
/// ⚠️ `contentRead` 不是洁癖：`GetCount` 是 **msctf 自己**也会问的（它据此判断候选 UI
/// 「有没有意义」，Chromium 的 IME-first 调度就靠这个），在那里无条件补拉等于给每个
/// 宿主的每一次按键都加一次宿主 UI 线程上的同步 IPC。
constexpr bool ShouldRefreshOnGet(bool dirty, bool contentRead, bool readCandidates)
{
    if (!dirty)
    {
        return false;
    }
    return contentRead || readCandidates;
}

/// 候选变化时：立刻拉快照再通知（true），还是只记脏、等宿主来读再拉（false）。
constexpr bool ShouldRefreshEagerly(bool declared, bool readCandidates)
{
    return HostDraws(declared, readCandidates);
}

/// 脏位是否**可能**与「已认定在画」同时成立。
///
/// 恒 false，而且这是一条**由调用方维持的不变量**，不是这里推出来的结论：脏位只在
/// [`ShouldRefreshEagerly`] 回 false 那一支被置起（见 `NotifyCandidatesVisibilityChanged`
/// 第三分支），而读取闩只在 `GetString` 里合上、合闩之前必先经 [`ShouldRefreshOnGet`]
/// 补拉并清脏。
///
/// 写成一个函数是为了让真值表单测**机械地**把它钉住：一旦有人在别处置脏、或让闩在
/// 别的路径上合，`ShouldRefreshOnGet(dirty=true, contentRead=false, …)` 那几个
/// 当前不可达的格子就会变成可达，届时 5 个取元信息的 getter 里那句补拉会从
/// 「防御性死代码」变回活代码——那正是它留在那里的理由。
constexpr bool DirtyCanCoexistWithHostDraws()
{
    return false;
}

} // namespace uielement
} // namespace wind
