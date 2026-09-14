// InstallerRunning 闸门的纯判定逻辑（不含 Win32 调用，便于单测）。
//
// ── 这个闸门是干什么的 ──────────────────────────────────────────────
// 安装/卸载期间安装器会杀掉主程序，而本 DLL 被各宿主进程加载着、随时可能把它重新
// 拉起来，于是安装器先在 HKLM\Software\<app>\InstallerRunning 立一个标记，本 DLL
// 见到它就不去启动服务。装完由安装计划的最后一步清掉。
//
// ── 为什么要有本文件 ────────────────────────────────────────────────
// 那个标记**没有兜底**：安装器进程若异常死亡（panic / 被杀 / 装到一半失败退出），
// 清除步骤永远跑不到，标记就永久留在注册表里。后果不是「装坏了」而是
// **输入法整个不工作且没有任何提示** —— 服务永远起不来，设置页读不到配置，
// 看起来就是「配置和自造词全没了」，连「重启服务」都没用（本 DLL 压根不去启动它），
// 而重装一次旧版本反倒会好（那次安装跑完整了，把标记清掉了）。
// 这三个现象的组合正是 issue #120 的原始描述，2026-09-14 在 vm-win11 上实测复现：
// 0.121.0 静默覆盖安装连续两次中途失败，每次都留下 InstallerRunning=1。
//
// ── 判据：标记的主人还活着吗 ────────────────────────────────────────
// 安装器额外写一个 `InstallerRunningOwner` = `"<pid>|<进程创建时间>"`，本 DLL 据此
// 校验「立这个标记的进程」是否仍在运行。进程没了 → 标记是遗物，忽略它。
//
// ⚠️ 为什么另起一个值名，而不是把身份并进 `InstallerRunning` 本身：
// 旧版 DLL 读该值用的是 `WCHAR value[8]` 的定长缓冲，值一旦超过 7 个字符，
// `RegQueryValueExW` 返回 ERROR_MORE_DATA 而非 ERROR_SUCCESS —— 旧 DLL 会把
// 「标记存在」读成「标记不存在」，于是安装期间照样去拉起服务，正是这个闸门要防的事。
// 故 `InstallerRunning` 的值**必须**原样保持 `"1"`，身份信息另放一个值名。
//
// 值名是跨语言、跨仓约定，见 Globals.h 中 WIND_APP_REGKEY 的注释。
#ifndef WIND_INSTALLER_GUARD_H
#define WIND_INSTALLER_GUARD_H

#include <cstdint>

namespace InstallerGuard
{

/// 立标记的进程身份。
///
/// 光有 PID 不够：Windows 会复用 PID，一个早已死去的安装器的 PID 可能正被别的进程
/// 占着，只比 PID 就会把遗物误判成「主人还活着」，闸门照样永久卡死。创建时间把
/// 这条堵上——PID 可以重复，`(PID, 创建时间)` 不会。
struct Owner
{
    uint32_t pid = 0;
    /// 进程创建时间，FILETIME 原值（100ns 自 1601-01-01 UTC）。两端只需口径一致。
    uint64_t createTime = 0;
};

/// 无 `InstallerRunningOwner` 可校验时，多久算陈旧。
///
/// 这条兜底只为「新 DLL + 旧安装器」这一种组合存在：旧安装器只写 `"1"`、不写 owner，
/// 若把「没有 owner」直接判成失效，旧安装器安装期间服务就会被拉起来——那正是闸门要防的。
/// 于是退而求其次看年龄：装完正常清除的话这个值根本不会留到超时，留下来即是遗物。
///
/// 取 10 分钟：实测一次完整安装（20MB 包、解压 + COM 注册 + 字体安装）在 1~2 分钟量级，
/// 十倍余量足够慢机器和慢盘，同时又远短于「用户觉得输入法坏了、去重装」的耐心。
///
/// ⚠️ 参照点是**注册表键**的最后写入时间，不是这个值自己的——Win32 只给得到键的时间。
/// 同键下 `InstallDir` 等别的值被写也会把它刷新，所以「10 分钟」严格讲是「该键十分钟
/// 没被动过」。方向仍是安全的（只会挡得更久），但它不是对标记本身的承诺。
constexpr uint64_t kStaleAfterMs = 10ull * 60ull * 1000ull;

/// 有 `InstallerRunningOwner` 时的硬兜底：主人进程启动至今超过这个时长，无论它是否
/// 「看起来还活着」，都按遗物处理。
///
/// 为什么身份校验之外还要有这一条：[`ShouldBlockStart`] 的调用方在**看不清**目标进程时
/// 一律答「活着」（跨用户、受保护进程、`ERROR_ACCESS_DENIED`……详见 IPCClient.cpp 的
/// `IsOwnerAlive`）。那个取向本身是对的，但若身份这条路上再没有年龄出口，
/// 「安装器死了 → PID 被一个 SYSTEM 服务复用 → 宿主永远打不开它 → 永远判定主人活着」
/// 就是个没有出口的状态机，现象与 #120 一模一样，而且重启也救不回来。
///
/// 参照点用 owner 串里的进程创建时间，而不是注册表键的写入时间：那是安装器自己写下的
/// 墙钟时刻，不会被同键下别的值刷新。
///
/// 取 24 小时：任何真实安装都远在其内，「慢安装仍受保护」的性质完整保留；它要防的不是
/// 慢安装，而是把「永久卡死」变成「有界卡死」。
constexpr uint64_t kOwnerHardStaleMs = 24ull * 60ull * 60ull * 1000ull;

/// 十进制解析一段 `wchar_t`，到 `stop` 或串尾为止。
///
/// 自己写而不用 `wcstoull`：那个函数会吞前导空白、接受 `+`/`-` 号、还看 locale，
/// 而这里的输入是本产品自己写下的固定格式，任何偏离都该判为「不认识」而非「尽量读」。
/// 溢出同样判失败——截断出来的 PID 会指向另一个进程。
inline bool ParseU64(const wchar_t*& p, wchar_t stop, uint64_t* out)
{
    if (p == nullptr || *p < L'0' || *p > L'9')
    {
        return false;
    }
    uint64_t v = 0;
    while (*p >= L'0' && *p <= L'9')
    {
        const uint64_t d = static_cast<uint64_t>(*p - L'0');
        if (v > (UINT64_MAX - d) / 10ull)
        {
            return false; // 溢出
        }
        v = v * 10ull + d;
        ++p;
    }
    if (*p != stop)
    {
        return false;
    }
    *out = v;
    return true;
}

/// 解析 `InstallerRunningOwner` 的值：`"<pid>|<createTime>"`，十进制、无空格。
///
/// 解析失败一律返回 false（调用方据此回落到年龄兜底），不做「尽力而为」的部分解析——
/// 一个残缺的身份比没有身份更糟：它会让校验以为自己拿到了确定答案。
///
/// ⚠️ 改这里的判据时，请人工同步 wind-installer 的
/// `src/installer/registry.rs::owner_format_tests::parse_owner_like_reader` ——
/// 那是本函数规则在写端的一份冻结快照，改这边不会让那边变红。
inline bool ParseOwner(const wchar_t* value, Owner* out)
{
    if (value == nullptr || out == nullptr)
    {
        return false;
    }
    const wchar_t* p = value;
    uint64_t pid = 0;
    if (!ParseU64(p, L'|', &pid) || pid == 0 || pid > 0xFFFFFFFFull)
    {
        return false;
    }
    ++p; // 跳过 '|'
    uint64_t create = 0;
    if (!ParseU64(p, L'\0', &create))
    {
        return false;
    }
    out->pid = static_cast<uint32_t>(pid);
    out->createTime = create;
    return true;
}

/// 距参照时刻 `refFt` 是否已超过 `limitMs`。两个时间都是 FILETIME 原值（100ns）。
///
/// `nowFt` 早于 `refFt`（时钟回拨、或参照点刚被写下）时返回 false —— 拿不准就当它
/// 有效，宁可多挡一会儿，也不要在安装真的正在进行时把服务拉起来。
inline bool IsOlderThan(uint64_t refFt, uint64_t nowFt, uint64_t limitMs)
{
    if (nowFt <= refFt)
    {
        return false;
    }
    const uint64_t elapsedMs = (nowFt - refFt) / 10000ull; // 100ns → ms
    return elapsedMs > limitMs;
}

/// 年龄兜底：注册表键的最后写入时间距今是否已超过 [`kStaleAfterMs`]。仅在没有 owner
/// 可校验（旧安装器写的标记）时使用。
inline bool IsStaleByAge(uint64_t keyWriteFt, uint64_t nowFt)
{
    return IsOlderThan(keyWriteFt, nowFt, kStaleAfterMs);
}

/// 硬兜底：owner 进程的创建时刻距今是否已超过 [`kOwnerHardStaleMs`]。
///
/// 创建时间为 0（写端从不这么写，见 registry.rs）会被判为「早已陈旧」，方向与
/// 「身份对不上 → 不是原来那个进程」一致。
inline bool IsOwnerHardStale(uint64_t ownerCreateFt, uint64_t nowFt)
{
    return IsOlderThan(ownerCreateFt, nowFt, kOwnerHardStaleMs);
}

/// 闸门的最终判定（把上面两条合成一句话，便于整体单测）。
///
/// - `hasFlag`         `InstallerRunning` 是否为 "1"
/// - `hasOwner`        `InstallerRunningOwner` 是否解析成功
/// - `ownerAlive`      该 owner 进程是否仍在运行且创建时间吻合（`hasOwner` 为假时忽略）
/// - `staleByAge`      [`IsStaleByAge`] 的结果（`hasOwner` 为真时忽略）
/// - `ownerHardStale`  [`IsOwnerHardStale`] 的结果（`hasOwner` 为假时忽略）
///
/// 返回 true 表示**应当阻止启动服务**。
inline bool ShouldBlockStart(bool hasFlag, bool hasOwner, bool ownerAlive, bool staleByAge,
                             bool ownerHardStale)
{
    if (!hasFlag)
    {
        return false;
    }
    if (hasOwner)
    {
        // 硬兜底先行：调用方看不清目标进程时一律答「活着」，没有这一条，
        // 「安装器死了 + PID 被一个看不见的进程复用」就再也走不出去了。
        if (ownerHardStale)
        {
            return false;
        }
        // 其余情况只认身份：主人还在就挡，主人没了就是遗物。十分钟的年龄兜底在这条路上
        // 不参与判断——一次超过十分钟的慢安装仍然应该被保护。
        return ownerAlive;
    }
    return !staleByAge;
}

} // namespace InstallerGuard

#endif // WIND_INSTALLER_GUARD_H
