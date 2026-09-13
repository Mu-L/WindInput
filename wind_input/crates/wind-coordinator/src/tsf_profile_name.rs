//! TSF 语言配置文件显示名（HKLM）的读写——`system.dota2_compat` 的落地动作。
//!
//! **为什么要改这个名字**：Dota 2（起源2 引擎）的 `imemanager.dll` 内置一张硬编码的
//! 输入法白名单，按注册表里该输入法的 TSF Profile Description 做**全等**比对
//! （`V_stricmp_fast`/`V_wcsicmp`，非子串）。命中的走「取候选 → 游戏自己画」；
//! 未命中的在 `WM_IME_NOTIFY` 第一道闸门就被打发到 `DefWindowProc`，于是候选永远
//! 取不到，还会多出一个系统默认 IME 小窗。这是游戏那侧写死的，数据侧绕不过去——
//! 完整证据与九条已实测证伪的方向见 `docs/design/game-compat-tsf-uielement.md` §1.1。
//!
//! ⚠️ 写的是 **HKLM**，必须以管理员权限运行；非提权进程会得到 `ERROR_ACCESS_DENIED`（5）。
//! 调用方（设置程序）负责用 `runas` 提权拉起 `wind_input system dota2-compat on|off`。
//!
//! ⚠️ 名字本身由调用方**显式传入**，本模块不读配置——理由同 `system_cli` 文件头：
//! 提权到另一个管理员账户时读到的 `%APPDATA%` 未必是拧开关那个用户的。
//!
//! ⚠️ 生效时机：宿主是在**自己启动时**读这个名字的，改完必须重启游戏。

use std::io;

use winreg::RegKey;
use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_READ, KEY_SET_VALUE};

use crate::direct_switch::tip_guid_strings;

/// Dota 2 白名单里选定的默认别名。
///
/// 自 2026-09 起这只是**出厂值**：用户可在 `system.dota2_compat_name` 里改成表里的
/// 任何一条（理由见该字段的文档）。真值定义在 `wind-config`，与 serde 的 default 同源。
pub use wind_config::DEFAULT_DOTA2_ALIAS as DOTA2_ALIAS;

/// 别名清洗与校验。定义在 `wind-config`（纯逻辑、与注册表无关，且要跟出厂值同源），
/// 在此重导出，免得调用方为一个校验去依赖两个 crate。
pub use wind_config::sanitize_dota2_alias as sanitize_alias;

/// 「当前登记的这个名字是我们自己写上去的」的记录值名，落在本应用自己的 HKLM 键下。
///
/// **为什么需要它**：`RegisterProfile()` 无条件覆盖 `Description`，安装/升级重跑一次
/// 就会把用户的别名冲掉（设置页仍显示「已开启」，用户只会在某次更新后发现游戏里又
/// 打不出字）。C++ 侧原先靠「当前值是否等于那个硬编码别名」来决定要不要保留——名字
/// 可配置之后这条判据失效，故改为由本模块把选定的名字记下来，C++ 读它比对。
///
/// ⛔ 别把 C++ 那侧放宽成「保留任何非真名的值」：那会把误写与脏值也一并固化下来。
/// 读取方见 `wind_tsf/src/Register.cpp` 的 `_ProfileNameToRegister()`。
const ALIAS_VALUE: &str = "Dota2CompatAlias";

/// 本应用在 HKLM 下的自有键。与 `wind_tsf/include/Globals.h` 的 `WIND_APP_REGKEY`
/// 同一个键（`Software\<app_dir_name>`，dev 变体带 `Dev` 后缀），`InstallDir` 也在这儿。
fn app_key_path() -> String {
    format!(r"Software\{}", wind_config::variant::app_dir_name())
}

/// TSF 语言配置文件在注册表里的语言子键。与 `wind_tsf` 的 `TEXTSERVICE_LANGID`（0x0804）一致。
const LANGID_SUBKEY: &str = "0x00000804";

/// 本输入法的真实显示名。
///
/// ⚠️ **跨语言重复，无编译期约束**：必须与 `wind_tsf/include/Globals.h` 的
/// `TEXTSERVICE_NAME` 逐字一致。改那边要同步改这里，否则关闭开关后名字会被还原成
/// 一个错的字符串（且只在用户关开关时才暴露）。与本文件复用的 `tip_guid_strings()`
/// 同一性质的约定——那边也是照抄 `Globals.cpp` 的 GUID。
fn real_name() -> &'static str {
    if wind_config::variant::is_dev() {
        "清风输入法 (开发版)"
    } else {
        "清风输入法"
    }
}

/// 本输入法的 LanguageProfile 键路径（HKLM 下）。
fn profile_key_path() -> String {
    let (clsid, profile, _) = tip_guid_strings();
    format!(r"SOFTWARE\Microsoft\CTF\TIP\{clsid}\LanguageProfile\{LANGID_SUBKEY}\{profile}")
}

/// 读当前登记的显示名。键不存在（未注册 TSF 组件）时回 `Ok(None)`。
pub fn current_description() -> io::Result<Option<String>> {
    let hklm = RegKey::predef(HKEY_LOCAL_MACHINE);
    match hklm.open_subkey_with_flags(profile_key_path(), KEY_READ) {
        Ok(key) => match key.get_value::<String, _>("Description") {
            Ok(v) => Ok(Some(v)),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e),
        },
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e),
    }
}

/// 把显示名切到 `alias`（`enabled=true`）或还原为真实名（`enabled=false`）。
///
/// `alias` 先过 [`sanitize_alias`]；`enabled=false` 时它被忽略（连清洗都不做，
/// 免得一个坏名字挡住「关闭」这条退路——用户改坏了总得关得掉）。
///
/// 幂等：已经是目标值时直接回 `Ok(false)`（未改动），便于调用方少弹一次 UAC 之外的噪音。
/// 返回 `Ok(true)` 表示确实写了。
///
/// ⚠️ 记录值（[`ALIAS_VALUE`]）与 `Description` **不是原子写**：中间失败会留下
/// 「名字改了、记录没跟上」。取舍是让记录值滞后而非抢先——记录值多了会让
/// `RegisterProfile` 保留一个并不存在的别名（无害，下次落地即纠正），记录值少了
/// 只是退化回「重装冲掉别名」的老行为。故先写 `Description` 再写记录值。
pub fn set_dota2_compat(enabled: bool, alias: &str) -> io::Result<bool> {
    let target = if enabled {
        sanitize_alias(alias).map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e))?
    } else {
        real_name().to_string()
    };
    let path = profile_key_path();
    let hklm = RegKey::predef(HKEY_LOCAL_MACHINE);
    // 只开已存在的键，**不创建**：键不在说明 TSF 组件根本没注册，此时凭空造一个
    // 半截的 LanguageProfile 只会让系统多出一个点不开的输入法条目。
    let key = hklm.open_subkey_with_flags(&path, KEY_READ | KEY_SET_VALUE)?;
    let unchanged = key
        .get_value::<String, _>("Description")
        .is_ok_and(|cur| cur == target);
    if !unchanged {
        key.set_value("Description", &target)?;
    }
    // 即便 Description 没变也要把记录值补上：旧版本装机改过名却没有这个值，
    // 不补就永远补不上（幂等分支每次都提前 return），重装照样冲掉用户的别名。
    record_alias(enabled.then_some(target.as_str()))?;
    Ok(!unchanged)
}

/// 写下（或清掉）「我们登记的别名」这条记录。
///
/// 键不存在时**创建**——与上面 LanguageProfile 那条相反：那是系统的键，不在就意味着
/// 没注册；这是我们自己的键，三个部署方都会写 `InstallDir` 到这儿，但便携/手工部署
/// 下未必已经建好，缺了就自己建，不该因此让整个开关失败。
fn record_alias(alias: Option<&str>) -> io::Result<()> {
    let hklm = RegKey::predef(HKEY_LOCAL_MACHINE);
    let (key, _) = hklm.create_subkey(app_key_path())?;
    match alias {
        Some(a) => key.set_value(ALIAS_VALUE, &a),
        // 关闭时删掉而不是写空串：C++ 侧「值不存在」与「值为空」要分别处理就多一条
        // 分支，而两者语义完全相同。不存在时的 NotFound 不是错误。
        None => match key.delete_value(ALIAS_VALUE) {
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
            r => r,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn app_key_is_the_same_one_the_dll_reads() {
        // 与 wind_tsf/include/Globals.h 的 WIND_APP_REGKEY 同一个键。写错了不会报错，
        // 只是 C++ 那边读不到记录值 —— 表现为「重装之后别名被冲回真名」。
        let p = app_key_path();
        assert!(
            p == r"Software\WindInput" || p == r"Software\WindInputDev",
            "{p}"
        );
    }

    #[test]
    fn profile_path_targets_the_language_profile_key() {
        let p = profile_key_path();
        assert!(p.starts_with(r"SOFTWARE\Microsoft\CTF\TIP\{"), "{p}");
        assert!(
            p.ends_with(r"\LanguageProfile\0x00000804\{99C2DEB1-5C57-45A2-9C63-FB54B34FD90A}")
                || p.ends_with(
                    r"\LanguageProfile\0x00000804\{99C2EE31-5C57-45A2-9C63-FB54B34FD90A}"
                ),
            "profile 子键必须是本变体的 guidProfile，不是 CLSID: {p}"
        );
    }
}
