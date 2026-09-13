//! `wind_input system` 命令行：与操作系统集成方式相关的动作（`[system]` 配置域的落地）。
//!
//! 这些动作写 **HKLM**，必须以管理员权限运行。设计上不由服务自己在配置变更时执行——
//! 服务跑在普通用户上下文，写不动 HKLM；由设置程序以 `runas` 拉起本子命令，用户当场
//! 看到一次 UAC，知道自己在改系统层面的登记。
//!
//! ⚠️ 本子命令**不读配置**，目标状态由参数显式给出。若改成「读 config 再应用」，
//! 提权进程在「以另一个管理员账户提权」时会读到那个账户的 `%APPDATA%`，
//! 从而把别人的偏好写进系统——显式传参没有这个歧义。

use wind_coordinator::tsf_profile_name;

const USAGE: &str = "\
用法: wind_input system <动作>

动作:
  dota2-compat on [--name <名称>]   开启兼容（改本输入法在系统里登记的显示名）
  dota2-compat off                 关闭，还原为真实名称
  dota2-compat status              显示当前登记的名称
  help                             显示本帮助

说明: Dota 2 按输入法名称查一张内置白名单决定要不要由游戏绘制候选，
      不在表内就取不到候选、还会多出一个左上角的系统 IME 小窗。
      开启后语言栏与 Windows 设置里显示的名称会随之改变。
      写 HKLM，需管理员权限；改完需重启游戏才生效。

      --name 省略时用出厂别名。名单里多数条目带空格，那时 shell 里要整体加引号。
      可选名称的完整清单见 docs/design/game-compat-tsf-uielement.md §1.3。";

/// 子命令入口。`args` 为 `system` 之后的参数。返回进程退出码。
pub fn run(args: &[String]) -> i32 {
    match args.first().map(String::as_str) {
        None | Some("help" | "--help" | "-h") => {
            println!("{USAGE}");
            0
        }
        Some("dota2-compat") => dota2_compat(&args[1..]),
        Some(other) => {
            eprintln!("未知的 system 动作: {other}\n");
            eprintln!("{USAGE}");
            2
        }
    }
}

fn dota2_compat(args: &[String]) -> i32 {
    let want = match args.first().map(String::as_str) {
        Some("on") => true,
        Some("off") => false,
        Some("status") => return dota2_status(),
        _ => {
            eprintln!("用法: wind_input system dota2-compat on|off|status");
            return 2;
        }
    };
    // 名称由调用方显式传入，理由同本文件头部那条「不读配置」：提权进程读到的
    // %APPDATA% 未必是拧开关那个用户的。省略即出厂别名。
    let alias = match parse_name(&args[1..]) {
        Ok(v) => v,
        Err(msg) => {
            eprintln!("{msg}");
            return 2;
        }
    };
    match tsf_profile_name::set_dota2_compat(want, &alias) {
        Ok(true) => {
            println!(
                "✓ 已{}Dota 2 兼容；当前登记名称: {}",
                if want { "开启" } else { "关闭" },
                describe_current()
            );
            println!("  需重启游戏后生效。");
            0
        }
        Ok(false) => {
            println!("已经是目标状态，未改动（当前名称: {}）", describe_current());
            0
        }
        // 5 = ERROR_ACCESS_DENIED：几乎总是「没提权」，直说而不是抛原始错误。
        Err(e) if e.raw_os_error() == Some(5) => {
            eprintln!("权限不足：本命令要写 HKLM，请以管理员身份运行。");
            1
        }
        // 键不存在 = TSF 组件没注册，改名无从谈起；提示往安装那边找，别让用户
        // 对着一个「找不到路径」的报错猜。
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            eprintln!("找不到本输入法的 TSF 注册项，可能尚未安装或注册失败。");
            1
        }
        // 名称过不了清洗（含控制字符/超长）。这是用户输入错误，不是系统故障，
        // 报 2（用法错）而不是 1。
        Err(e) if e.kind() == std::io::ErrorKind::InvalidInput => {
            eprintln!("名称不可用: {e}");
            2
        }
        Err(e) => {
            eprintln!("写注册表失败: {e}");
            1
        }
    }
}

/// 解析 `--name <名称>`。省略时回出厂别名。
///
/// 手写而不引入参数解析库：整个 `system` 子命令就这一个可选参数，且它是**最后**一个
/// 位置——把剩余参数整体当名字会吞掉将来新加的参数，故要求显式 `--name`。
fn parse_name(rest: &[String]) -> Result<String, String> {
    match rest {
        [] => Ok(tsf_profile_name::DOTA2_ALIAS.to_string()),
        [flag, value] if flag == "--name" => Ok(value.clone()),
        [flag] if flag == "--name" => Err("--name 后面缺少名称".to_string()),
        _ => Err(format!(
            "无法识别的参数: {}\n用法: wind_input system dota2-compat on [--name <名称>]",
            rest.join(" ")
        )),
    }
}

fn dota2_status() -> i32 {
    println!("当前登记名称: {}", describe_current());
    // ★ 记录值是「重装/升级后还认不认得出用户的选择」的唯一依据（见
    // `tsf_profile_name::ALIAS_VALUE`）。排查「升级后别名没了」这类故障时第一个要看它，
    // 不列出来就只能让人手动 reg query。
    match tsf_profile_name::recorded_alias() {
        Ok(Some(v)) => println!("已登记的兼容别名（重装后据此恢复）: {v}"),
        Ok(None) => println!("已登记的兼容别名: （无——当前为关闭状态）"),
        Err(e) => println!("已登记的兼容别名: （读取失败: {e}）"),
    }
    println!(
        "出厂别名（--name 省略时用它）: {}",
        tsf_profile_name::DOTA2_ALIAS
    );
    0
}

fn describe_current() -> String {
    match tsf_profile_name::current_description() {
        Ok(Some(v)) => v,
        Ok(None) => "（未注册）".to_string(),
        Err(e) => format!("（读取失败: {e}）"),
    }
}
