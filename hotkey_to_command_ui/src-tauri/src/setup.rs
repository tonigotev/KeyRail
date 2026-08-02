// Integration setup: detect what is already installed, then do as much of the
// install as Windows actually allows.
//
// Two honest limits shape this module:
//
// 1. Chrome and Brave cannot be made to load an unpacked extension by another
//    program. The supported routes are the Web Store or an enterprise policy in
//    HKLM, and the policy route needs admin and labels the extension as
//    "installed by your administrator". So the extension is staged to a stable
//    folder, the browser is opened on its extensions page, and the user does the
//    final Load unpacked click. Everything up to that click is automated.
//
// 2. Vencord userplugins only work from a source checkout, not from the normal
//    Vencord installer build. That means git + node + pnpm, a clone, a build and
//    an inject. Those are automated, but the toolchain itself is only detected
//    and linked, never silently downloaded.

use std::collections::HashSet;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

#[cfg(windows)]
use std::os::windows::process::CommandExt;

use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager};

#[cfg(windows)]
const NO_WINDOW: u32 = 0x08000000;

const VENCORD_REPO: &str = "https://github.com/Vendicated/Vencord.git";
const PLUGIN_FOLDER: &str = "outputDeviceBridge";
const EXTENSION_FOLDER: &str = "mediaTargetBridge";

// ---- shared helpers ---------------------------------------------------------

fn no_window(command: &mut Command) -> &mut Command {
    #[cfg(windows)]
    command.creation_flags(NO_WINDOW);
    command
}

fn app_dir() -> Result<PathBuf, String> {
    let appdata = env::var_os("APPDATA").ok_or("APPDATA is not set")?;
    Ok(PathBuf::from(appdata).join("HotkeyToCommand"))
}

fn work_dir() -> Result<PathBuf, String> {
    let local = env::var_os("LOCALAPPDATA").ok_or("LOCALAPPDATA is not set")?;
    Ok(PathBuf::from(local).join("HotkeyToCommand"))
}

/// Runs a tool with `--version` to find out whether it is usable and which
/// build is installed. Returns None when the tool is missing from PATH.
fn tool_version(program: &str, args: &[&str]) -> Option<String> {
    let mut command = Command::new(program);
    command.args(args);
    no_window(&mut command);
    let output = command.output().ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&output.stdout);
    Some(text.lines().next().unwrap_or("").trim().to_string())
}

/// npm and pnpm are batch shims on Windows, so they only resolve through a shell.
fn shell_tool_version(program: &str) -> Option<String> {
    let mut command = Command::new("cmd");
    command.args(["/C", program, "--version"]);
    no_window(&mut command);
    let output = command.output().ok()?;
    if !output.status.success() {
        return None;
    }
    let text = String::from_utf8_lossy(&output.stdout);
    Some(text.lines().next().unwrap_or("").trim().to_string())
}

/// Rebuilds this process's PATH from the registry.
///
/// A freshly installed tool appends itself to the machine or user PATH, but a
/// process that was already running keeps the copy it inherited at launch. Every
/// `git`/`node`/`pnpm` call after an install would still say "not found" without
/// this, which looks exactly like the install having failed.
#[cfg(windows)]
fn refresh_path_from_registry() {
    let machine = read_registry_string(
        windows_sys::Win32::System::Registry::HKEY_LOCAL_MACHINE,
        r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment",
        "Path",
    )
    .unwrap_or_default();
    let user = read_registry_string(
        windows_sys::Win32::System::Registry::HKEY_CURRENT_USER,
        r"Environment",
        "Path",
    )
    .unwrap_or_default();

    let mut combined = String::new();
    for part in [machine.as_str(), user.as_str()] {
        if part.is_empty() {
            continue;
        }
        if !combined.is_empty() {
            combined.push(';');
        }
        combined.push_str(part);
    }

    if !combined.is_empty() {
        env::set_var("PATH", combined);
    }
}

#[cfg(not(windows))]
fn refresh_path_from_registry() {}

fn winget_available() -> bool {
    tool_version("winget", &["--version"]).is_some()
}

fn tools_dir() -> Result<PathBuf, String> {
    Ok(work_dir()?.join("tools"))
}

/// Runs a command and returns stdout only, for the cases where the output is
/// data to parse rather than a log line to show.
fn capture(program: &str, args: &[&str]) -> Result<String, String> {
    let mut command = Command::new(program);
    command.args(args);
    no_window(&mut command);
    let output = command
        .output()
        .map_err(|error| format!("{program} could not start: {error}"))?;
    if !output.status.success() {
        return Err(format!(
            "{program} failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(String::from_utf8_lossy(&output.stdout).to_string())
}

/// Prepends a directory to this process's PATH so tools we just unpacked are
/// found by everything we spawn afterwards.
fn prepend_to_path(dir: &Path) {
    let current = env::var("PATH").unwrap_or_default();
    let dir = dir.display().to_string();
    if current.split(';').any(|part| part.eq_ignore_ascii_case(&dir)) {
        return;
    }
    env::set_var("PATH", format!("{dir};{current}"));
}

/// Installs Node.js and git without winget and without administrator rights, by
/// unpacking the official portable builds into our own folder.
///
/// This exists because winget is absent from Windows Sandbox, some LTSC builds
/// and stripped enterprise images, and a setup that only works where winget
/// happens to be present is not really automatic. Nothing here touches the
/// system: the tools live under %LOCALAPPDATA%, and only this process's PATH is
/// adjusted, so uninstalling the app takes them with it.
fn install_tools_portable(reporter: &Reporter) -> Result<(), String> {
    let root = tools_dir()?;
    fs::create_dir_all(&root).map_err(|error| error.to_string())?;

    if tool_version("node", &["--version"]).is_none() {
        progress(reporter, "install Node.js", "asking nodejs.org for the current LTS");
        let index = capture("curl.exe", &["-sL", "--fail", "https://nodejs.org/dist/index.json"])?;
        let releases: serde_json::Value =
            serde_json::from_str(&index).map_err(|error| format!("node index parse failed: {error}"))?;

        // The index is newest-first and marks LTS releases with a codename
        // rather than `false`.
        let version = releases
            .as_array()
            .and_then(|list| {
                list.iter().find(|entry| {
                    entry.get("lts").map(|lts| !lts.is_boolean()).unwrap_or(false)
                })
            })
            .and_then(|entry| entry.get("version"))
            .and_then(|value| value.as_str())
            .ok_or("could not find a Node LTS release in the index")?
            .to_string();

        let folder = format!("node-{version}-win-x64");
        let url = format!("https://nodejs.org/dist/{version}/{folder}.zip");
        let archive = root.join("node.zip");

        progress(reporter, "install Node.js", &format!("downloading {version}"));
        run_step(
            reporter,
            "download Node.js",
            "curl.exe",
            &["-L", "--fail", "-s", "-S", "-o", &archive.display().to_string(), &url],
            None,
            false,
        )?;

        progress(reporter, "install Node.js", "unpacking");
        run_step(
            reporter,
            "unpack Node.js",
            "tar.exe",
            &["-xf", &archive.display().to_string(), "-C", &root.display().to_string()],
            None,
            false,
        )?;
        let _ = fs::remove_file(&archive);

        prepend_to_path(&root.join(&folder));
        finished(reporter, "install Node.js", &format!("{version} ready"));
    }

    if tool_version("git", &["--version"]).is_none() {
        progress(reporter, "install git", "finding the current PortableGit release");
        let release = capture(
            "curl.exe",
            &[
                "-sL",
                "--fail",
                "-H",
                "User-Agent: HotkeyToCommand",
                "https://api.github.com/repos/git-for-windows/git/releases/latest",
            ],
        )?;
        let parsed: serde_json::Value =
            serde_json::from_str(&release).map_err(|error| format!("git release parse failed: {error}"))?;

        let url = parsed
            .get("assets")
            .and_then(|assets| assets.as_array())
            .and_then(|assets| {
                assets.iter().find(|asset| {
                    asset
                        .get("name")
                        .and_then(|name| name.as_str())
                        .map(|name| name.starts_with("PortableGit-") && name.ends_with("-64-bit.7z.exe"))
                        .unwrap_or(false)
                })
            })
            .and_then(|asset| asset.get("browser_download_url"))
            .and_then(|value| value.as_str())
            .ok_or("could not find a PortableGit download in the latest release")?
            .to_string();

        let installer = root.join("PortableGit.7z.exe");
        let target = root.join("git");

        progress(reporter, "install git", "downloading, this one is around 56 MB");
        run_step(
            reporter,
            "download git",
            "curl.exe",
            &["-L", "--fail", "-s", "-S", "-o", &installer.display().to_string(), &url],
            None,
            false,
        )?;

        progress(reporter, "install git", "unpacking");
        run_step(
            reporter,
            "unpack git",
            &installer.display().to_string(),
            &[&format!("-o{}", target.display()), "-y"],
            None,
            false,
        )?;
        let _ = fs::remove_file(&installer);

        prepend_to_path(&target.join("cmd"));
        finished(reporter, "install git", "ready");
    }

    Ok(())
}

/// Installs git and Node.js through winget, then pnpm through npm.
///
/// winget is used rather than downloading installers directly because it is the
/// mechanism Windows ships for exactly this: it resolves the current version,
/// verifies the package, and installs silently. Hand-rolled downloads would mean
/// pinning URLs that rot and skipping signature checks.
fn ensure_build_tools(reporter: &Reporter) -> Result<(), String> {
    // Reuse anything a previous run already unpacked before deciding to install.
    if let Ok(root) = tools_dir() {
        if let Ok(entries) = fs::read_dir(&root) {
            for entry in entries.flatten() {
                let name = entry.file_name();
                let name = name.to_string_lossy();
                if name.starts_with("node-") {
                    prepend_to_path(&entry.path());
                } else if name == "git" {
                    prepend_to_path(&entry.path().join("cmd"));
                }
            }
        }
    }

    let needs_git = tool_version("git", &["--version"]).is_none();
    let needs_node = tool_version("node", &["--version"]).is_none();

    if needs_git || needs_node {
        // winget first when present: it installs system-wide, so the tools stay
        // useful outside this app. The portable route is the fallback for the
        // many images that have no winget, and needs no administrator rights.
        let mut installed = false;

        if winget_available() {
            let common = [
                "--exact",
                "--silent",
                "--accept-package-agreements",
                "--accept-source-agreements",
                "--disable-interactivity",
            ];

            let mut winget_ok = true;
            if needs_git {
                let mut args = vec!["install", "--id", "Git.Git"];
                args.extend_from_slice(&common);
                winget_ok &= run_step(reporter, "install git", "winget", &args, None, true).is_ok();
            }
            if winget_ok && needs_node {
                let mut args = vec!["install", "--id", "OpenJS.NodeJS.LTS"];
                args.extend_from_slice(&common);
                winget_ok &= run_step(reporter, "install Node.js", "winget", &args, None, true).is_ok();
            }

            if winget_ok {
                progress(reporter, "refresh PATH", "picking up the newly installed tools");
                refresh_path_from_registry();
                installed = tool_version("git", &["--version"]).is_some()
                    && tool_version("node", &["--version"]).is_some();
            }

            if !installed {
                progress(reporter, "install tools", "winget did not provide them, unpacking portable builds instead");
            }
        } else {
            progress(reporter, "install tools", "winget is unavailable, unpacking portable builds");
        }

        if !installed {
            install_tools_portable(reporter)?;
        }
    }

    if shell_tool_version("pnpm").is_none() {
        install_pnpm_inner(reporter)?;
        refresh_path_from_registry();
    }

    // Re-check rather than trust the installers' exit codes.
    let mut still_missing = Vec::new();
    if tool_version("git", &["--version"]).is_none() {
        still_missing.push("git");
    }
    if tool_version("node", &["--version"]).is_none() {
        still_missing.push("Node.js");
    }
    if shell_tool_version("pnpm").is_none() {
        still_missing.push("pnpm");
    }

    if still_missing.is_empty() {
        Ok(())
    } else {
        Err(format!(
            "still missing after install: {}. A sign-out and back in usually makes a new PATH visible.",
            still_missing.join(", ")
        ))
    }
}

#[tauri::command]
pub fn install_build_tools(app: AppHandle) -> Result<String, String> {
    let reporter = Reporter::Gui(app);
    ensure_build_tools(&reporter)?;
    Ok("git, Node.js and pnpm are ready".to_string())
}

fn copy_dir_all(from: &Path, to: &Path) -> Result<(), String> {
    fs::create_dir_all(to).map_err(|error| error.to_string())?;
    for entry in fs::read_dir(from).map_err(|error| error.to_string())? {
        let entry = entry.map_err(|error| error.to_string())?;
        let kind = entry.file_type().map_err(|error| error.to_string())?;
        let target = to.join(entry.file_name());
        if kind.is_dir() {
            copy_dir_all(&entry.path(), &target)?;
        } else if kind.is_file() {
            fs::copy(entry.path(), &target).map_err(|error| error.to_string())?;
        }
    }
    Ok(())
}

/// Finds a folder that ships with the app. Installed builds get it from the
/// bundled resources; a dev run walks up to the repo checkout.
/// `app` is optional because this also runs from the installer, where no Tauri
/// app exists. An installed build puts resources beside the executable, so the
/// exe-relative walk below finds them either way.
fn integration_source(app: Option<&AppHandle>, relative: &[&str]) -> Result<PathBuf, String> {
    let mut candidates: Vec<PathBuf> = Vec::new();
    let mut seen: HashSet<PathBuf> = HashSet::new();

    let push = |path: PathBuf, seen: &mut HashSet<PathBuf>, out: &mut Vec<PathBuf>| {
        if seen.insert(path.clone()) {
            out.push(path);
        }
    };

    if let Some(resources) = app.and_then(|app| app.path().resource_dir().ok()) {
        let mut path = resources.join("integrations");
        for part in relative {
            path = path.join(part);
        }
        push(path, &mut seen, &mut candidates);
    }

    let mut roots: Vec<PathBuf> = Vec::new();
    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            roots.push(dir.to_path_buf());
        }
    }
    if let Ok(cwd) = env::current_dir() {
        roots.push(cwd);
    }

    for root in roots {
        for ancestor in root.ancestors() {
            let mut path = ancestor.join("integrations");
            for part in relative {
                path = path.join(part);
            }
            push(path, &mut seen, &mut candidates);
        }
    }

    candidates
        .iter()
        .find(|path| path.exists())
        .cloned()
        .ok_or_else(|| {
            format!(
                "could not find integrations/{} next to the app or in the repo",
                relative.join("/")
            )
        })
}

// ---- headless setup, driven by the installer --------------------------------

/// Runs the same setup the wizard runs, with no GUI, printing progress to stdout
/// so the installer can stream it into its details pane.
///
/// `targets` is a comma-separated list: "browser", "discord".
///
/// This never returns an error to the caller. A failed integration must not fail
/// the installation, because the app itself is perfectly usable without either
/// bridge, and the first-run wizard can pick up whatever did not finish here.
pub fn run_headless_setup(targets: &str) -> i32 {
    let reporter = Reporter::Cli;
    let wanted: Vec<&str> = targets
        .split(',')
        .map(|part| part.trim())
        .filter(|part| !part.is_empty())
        .collect();

    let mut failures = 0;

    // "browser" stages the files only. "browser-open" additionally puts the path
    // on the clipboard and opens the extensions page, so the installer can hand
    // the user straight to the one click no installer is allowed to make.
    let stage_browser = wanted.contains(&"browser") || wanted.contains(&"browser-open");
    if stage_browser {
        progress(&reporter, "browser bridge", "staging extension files");
        match stage_browser_extension_inner(None) {
            Ok(path) => {
                finished(&reporter, "browser bridge", &format!("staged to {path}"));

                if wanted.contains(&"browser-open") {
                    if copy_to_clipboard(&path) {
                        println!("    Folder path copied to the clipboard.");
                    }

                    let browser = default_browser_id();
                    let url = extensions_url(&browser);
                    if !browser.is_empty() {
                        progress(&reporter, "browser bridge", &format!("opening {browser}"));
                        if let Err(error) = open_browser(browser.clone()) {
                            println!("    Could not open {browser}: {error}");
                        }
                    }
                    println!("    Finish in your browser: go to {url}, turn on Developer mode,");
                    println!("    choose Load unpacked, and paste the copied folder path.");
                } else {
                    println!(
                        "    Load it with Load unpacked in your browser. The app can open that page for you."
                    );
                }
            }
            Err(error) => {
                failed(&reporter, "browser bridge", &error);
                failures += 1;
            }
        }
    }

    if wanted.contains(&"discord") {
        // A clean machine has none of these, so fetch them rather than bailing
        // out with a list of things for the user to go install by hand.
        if let Err(error) = ensure_build_tools(&reporter) {
            failed(&reporter, "build tools", &error);
            println!("    Setup will offer the Discord bridge again when you open the app.");
            return 0;
        }

        match install_vencord_bridge_inner(&reporter, true) {
            Ok(message) => finished(&reporter, "discord bridge", &message),
            Err(error) => {
                failed(&reporter, "discord bridge", &error);
                println!("    Setup will offer this again when you open the app.");
                failures += 1;
            }
        }
    }

    if failures > 0 {
        println!("{failures} step(s) did not finish; the app will offer them again on first run.");
    }
    0
}

// ---- first run --------------------------------------------------------------

/// The installer writes HKCU\Software\HotkeyToCommand\PendingSetup after a fresh
/// install. Reading it here consumes it, so the wizard opens exactly once rather
/// than every launch. The installer cannot run the integration setup itself: it
/// may be elevated, while Vencord and Discord are per-user.
#[tauri::command]
pub fn take_pending_setup() -> bool {
    #[cfg(windows)]
    {
        use windows_sys::Win32::System::Registry::{
            RegCloseKey, RegDeleteValueW, RegOpenKeyExW, RegQueryValueExW, HKEY_CURRENT_USER,
            KEY_READ, KEY_SET_VALUE,
        };

        fn wide(value: &str) -> Vec<u16> {
            value.encode_utf16().chain(std::iter::once(0)).collect()
        }

        let key_path = wide(r"Software\HotkeyToCommand");
        let value_name = wide("PendingSetup");

        unsafe {
            let mut key = std::ptr::null_mut();
            if RegOpenKeyExW(
                HKEY_CURRENT_USER,
                key_path.as_ptr(),
                0,
                KEY_READ | KEY_SET_VALUE,
                &mut key,
            ) != 0
            {
                return false;
            }

            let mut data = 0u32;
            let mut size = std::mem::size_of::<u32>() as u32;
            let mut kind = 0u32;
            let result = RegQueryValueExW(
                key,
                value_name.as_ptr(),
                std::ptr::null_mut(),
                &mut kind,
                &mut data as *mut u32 as *mut u8,
                &mut size,
            );

            let pending = result == 0 && data != 0;
            if pending {
                RegDeleteValueW(key, value_name.as_ptr());
            }
            RegCloseKey(key);
            pending
        }
    }

    #[cfg(not(windows))]
    false
}

// ---- detection --------------------------------------------------------------

#[derive(Serialize, Clone)]
pub struct ToolInfo {
    pub name: String,
    pub found: bool,
    pub version: String,
    pub install_url: String,
}

#[derive(Serialize, Clone)]
pub struct BrowserInfo {
    pub id: String,
    pub name: String,
    pub path: String,
    pub installed: bool,
    pub is_default: bool,
}

#[derive(Serialize, Clone)]
pub struct DiscordInfo {
    pub variant: String,
    pub path: String,
    pub running: bool,
}

#[derive(Serialize, Clone)]
pub struct VencordInfo {
    pub checkout_path: String,
    pub checkout_ready: bool,
    pub dependencies_installed: bool,
    pub plugin_installed: bool,
    pub built: bool,
    /// Read from Discord's own files, not from whether the installer exited 0.
    pub injected: bool,
}

#[derive(Serialize, Clone)]
pub struct SetupStatus {
    pub tools: Vec<ToolInfo>,
    pub browsers: Vec<BrowserInfo>,
    pub default_browser: String,
    /// What the detection actually saw, so a wrong answer can be diagnosed
    /// instead of guessed at. Windows only honours a UserChoice whose Hash
    /// validates, and silently falls back to Edge when it does not, so "the app
    /// is wrong" and "Windows really does think Edge is default" look identical
    /// from the outside without this.
    pub default_browser_detail: String,
    pub extension_staged_path: String,
    pub extension_staged: bool,
    pub discord: Vec<DiscordInfo>,
    pub vencord: VencordInfo,
}

#[cfg(windows)]
fn read_hkcu_string(subkey: &str, value: &str) -> Option<String> {
    read_registry_string(
        windows_sys::Win32::System::Registry::HKEY_CURRENT_USER,
        subkey,
        value,
    )
}

#[cfg(windows)]
fn read_registry_string(
    root: windows_sys::Win32::System::Registry::HKEY,
    subkey: &str,
    value: &str,
) -> Option<String> {
    use windows_sys::Win32::System::Registry::{
        RegCloseKey, RegOpenKeyExW, RegQueryValueExW, KEY_READ,
    };

    fn wide(value: &str) -> Vec<u16> {
        value.encode_utf16().chain(std::iter::once(0)).collect()
    }

    let key_path = wide(subkey);
    let value_name = wide(value);

    unsafe {
        let mut key = std::ptr::null_mut();
        if RegOpenKeyExW(root, key_path.as_ptr(), 0, KEY_READ, &mut key) != 0 {
            return None;
        }

        // Sized from the first call rather than a fixed buffer: PATH routinely
        // runs past a kilobyte and would otherwise come back truncated.
        let mut size = 0u32;
        let mut kind = 0u32;
        if RegQueryValueExW(
            key,
            value_name.as_ptr(),
            std::ptr::null_mut(),
            &mut kind,
            std::ptr::null_mut(),
            &mut size,
        ) != 0
        {
            RegCloseKey(key);
            return None;
        }

        let mut buffer = vec![0u16; (size as usize / 2) + 1];
        let mut actual = size;
        let result = RegQueryValueExW(
            key,
            value_name.as_ptr(),
            std::ptr::null_mut(),
            &mut kind,
            buffer.as_mut_ptr() as *mut u8,
            &mut actual,
        );
        RegCloseKey(key);

        if result != 0 {
            return None;
        }

        let len = buffer.iter().position(|c| *c == 0).unwrap_or(buffer.len());
        let raw = String::from_utf16_lossy(&buffer[..len]);

        // REG_EXPAND_SZ values such as the machine PATH contain %SystemRoot%
        // and friends, which have to be expanded before they are usable.
        const REG_EXPAND_SZ: u32 = 2;
        if kind == REG_EXPAND_SZ {
            Some(expand_environment_string(&raw))
        } else {
            Some(raw)
        }
    }
}

#[cfg(windows)]
fn expand_environment_string(value: &str) -> String {
    use windows_sys::Win32::System::Environment::ExpandEnvironmentStringsW;

    let input: Vec<u16> = value.encode_utf16().chain(std::iter::once(0)).collect();
    unsafe {
        let needed = ExpandEnvironmentStringsW(input.as_ptr(), std::ptr::null_mut(), 0);
        if needed == 0 {
            return value.to_string();
        }
        let mut out = vec![0u16; needed as usize];
        let written = ExpandEnvironmentStringsW(input.as_ptr(), out.as_mut_ptr(), needed);
        if written == 0 {
            return value.to_string();
        }
        let len = out.iter().position(|c| *c == 0).unwrap_or(out.len());
        String::from_utf16_lossy(&out[..len])
    }
}

/// Resolves the default browser to a real executable path.
///
/// Matching on the ProgId string alone is unreliable: it assumes every browser
/// puts its name in there. Following the ProgId to its registered open command
/// gives the actual exe, which is unambiguous.
///
/// AssocQueryStringW would be the tidy way to do this, but it returns
/// ERROR_NO_APPLICATION_ASSOCIATED for http on real machines, so the registry
/// chain below is what actually works.
#[cfg(windows)]
fn default_browser_executable() -> Option<String> {
    let mut prog_id = String::new();
    for association in [
        r"Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https\UserChoice",
        r"Software\Microsoft\Windows\Shell\Associations\UrlAssociations\http\UserChoice",
        r"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.html\UserChoice",
    ] {
        if let Some(value) = read_hkcu_string(association, "ProgId") {
            if !value.is_empty() {
                prog_id = value;
                break;
            }
        }
    }

    if prog_id.is_empty() {
        return None;
    }

    let command = read_registry_string(
        windows_sys::Win32::System::Registry::HKEY_CLASSES_ROOT,
        &format!(r"{prog_id}\shell\open\command"),
        "",
    )?;

    Some(command.to_lowercase())
}

/// Human-readable account of how the default browser was determined.
#[cfg(windows)]
fn default_browser_detail() -> String {
    let https = read_hkcu_string(
        r"Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https\UserChoice",
        "ProgId",
    );
    let hashed = read_hkcu_string(
        r"Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https\UserChoice",
        "Hash",
    )
    .is_some();

    match https {
        None => "no https UserChoice is set, so Windows uses its built-in default".to_string(),
        Some(prog_id) => {
            let command = read_registry_string(
                windows_sys::Win32::System::Registry::HKEY_CLASSES_ROOT,
                &format!(r"{prog_id}\shell\open\command"),
                "",
            )
            .unwrap_or_else(|| "(no open command registered)".to_string());

            format!(
                "https ProgId={prog_id}, hash present={hashed}, opens with {command}"
            )
        }
    }
}

#[cfg(not(windows))]
fn default_browser_detail() -> String {
    String::new()
}

#[cfg(windows)]
fn default_browser_id() -> String {
    // Preferred: resolve the real handler executable.
    if let Some(exe) = default_browser_executable() {
        if exe.contains("brave") {
            return "brave".to_string();
        }
        if exe.contains("chrome") {
            return "chrome".to_string();
        }
        if exe.contains("msedge") {
            return "edge".to_string();
        }
    }

    // Fallback: the UserChoice ProgId. Checked for several associations because
    // a browser's "make me default" button does not always set all of them, and
    // a freshly set default can leave some entries untouched.
    let mut prog_id = String::new();
    for association in [
        r"Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https\UserChoice",
        r"Software\Microsoft\Windows\Shell\Associations\UrlAssociations\http\UserChoice",
        r"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.html\UserChoice",
    ] {
        if let Some(value) = read_hkcu_string(association, "ProgId") {
            if !value.is_empty() {
                prog_id = value;
                break;
            }
        }
    }

    if prog_id.is_empty() {
        return String::new();
    }

    {
        let prog_id = prog_id.to_lowercase();

        if prog_id.contains("brave") {
            "brave".to_string()
        } else if prog_id.contains("chrome") {
            "chrome".to_string()
        } else if prog_id.contains("msedge") || prog_id.contains("edge") {
            "edge".to_string()
        } else if prog_id.contains("firefox") {
            "firefox".to_string()
        } else {
            String::new()
        }
    }
}

#[cfg(not(windows))]
fn default_browser_id() -> String {
    String::new()
}

fn browser_candidates() -> Vec<(&'static str, &'static str, Vec<PathBuf>)> {
    let program_files = env::var_os("ProgramFiles").map(PathBuf::from);
    let program_files_x86 = env::var_os("ProgramFiles(x86)").map(PathBuf::from);
    let local = env::var_os("LOCALAPPDATA").map(PathBuf::from);

    let join_all = |parts: &[&str]| -> Vec<PathBuf> {
        let mut out = Vec::new();
        for base in [&program_files, &program_files_x86, &local] {
            if let Some(base) = base {
                let mut path = base.clone();
                for part in parts {
                    path = path.join(part);
                }
                out.push(path);
            }
        }
        out
    };

    vec![
        (
            "brave",
            "Brave",
            join_all(&["BraveSoftware", "Brave-Browser", "Application", "brave.exe"]),
        ),
        (
            "chrome",
            "Google Chrome",
            join_all(&["Google", "Chrome", "Application", "chrome.exe"]),
        ),
        (
            "edge",
            "Microsoft Edge",
            join_all(&["Microsoft", "Edge", "Application", "msedge.exe"]),
        ),
    ]
}

/// Every plausible Local AppData root, because %LOCALAPPDATA% cannot be trusted
/// on its own. A redirected or stale profile can leave the environment variable
/// pointing somewhere Discord was never installed, while the resolved Shell
/// Folders value still holds the real path. Probing all of them costs nothing
/// and is the difference between finding Discord and reporting it missing.
fn local_app_data_roots() -> Vec<PathBuf> {
    let mut roots: Vec<PathBuf> = Vec::new();
    let push = |path: PathBuf, roots: &mut Vec<PathBuf>| {
        if path.is_dir() && !roots.contains(&path) {
            roots.push(path);
        }
    };

    if let Some(value) = env::var_os("LOCALAPPDATA") {
        push(PathBuf::from(value), &mut roots);
    }
    #[cfg(windows)]
    if let Some(value) = read_hkcu_string(
        r"Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders",
        "Local AppData",
    ) {
        push(PathBuf::from(value), &mut roots);
    }
    if let Some(value) = env::var_os("USERPROFILE") {
        push(PathBuf::from(value).join("AppData").join("Local"), &mut roots);
    }

    roots
}

fn discord_candidates() -> Vec<(&'static str, PathBuf)> {
    let mut found: Vec<(&'static str, PathBuf)> = Vec::new();
    for root in local_app_data_roots() {
        for (variant, folder) in [
            ("Discord", "Discord"),
            ("Discord PTB", "DiscordPTB"),
            ("Discord Canary", "DiscordCanary"),
        ] {
            let path = root.join(folder);
            if path.is_dir() && !found.iter().any(|(_, existing)| *existing == path) {
                found.push((variant, path));
            }
        }
    }
    found
}

/// Vencord renames Discord's original bundle to _app.asar and drops a small
/// loader in its place, so this is the on-disk proof that a patch really landed.
/// The installer exits 0 in situations where it patched nothing, which is how a
/// failed inject previously got reported as success.
fn discord_is_patched(install: &Path) -> bool {
    let Ok(entries) = fs::read_dir(install) else {
        return false;
    };
    for entry in entries.flatten() {
        if !entry.file_name().to_string_lossy().starts_with("app-") {
            continue;
        }
        if entry.path().join("resources").join("_app.asar").exists() {
            return true;
        }
    }
    false
}

fn process_running(image: &str) -> bool {
    let mut command = Command::new("tasklist");
    command.args(["/FI", &format!("IMAGENAME eq {image}"), "/NH"]);
    no_window(&mut command);
    let Ok(output) = command.output() else {
        return false;
    };
    String::from_utf8_lossy(&output.stdout).to_lowercase().contains(&image.to_lowercase())
}

pub fn vencord_dir() -> Result<PathBuf, String> {
    Ok(work_dir()?.join("Vencord"))
}

pub fn extension_staged_dir() -> Result<PathBuf, String> {
    Ok(app_dir()?.join("extensions").join(EXTENSION_FOLDER))
}

/// Puts the staged folder on the clipboard so the Load unpacked dialog can be
/// filled with a paste. Without this the installer would hand the user a long
/// path to retype. Done with the raw API because the headless installer path has
/// no Tauri app and therefore no clipboard plugin.
#[cfg(windows)]
fn copy_to_clipboard(text: &str) -> bool {
    use windows_sys::Win32::Foundation::HANDLE;
    use windows_sys::Win32::System::DataExchange::{
        CloseClipboard, EmptyClipboard, OpenClipboard, SetClipboardData,
    };
    use windows_sys::Win32::System::Memory::{GlobalAlloc, GlobalLock, GlobalUnlock, GMEM_MOVEABLE};

    const CF_UNICODETEXT: u32 = 13;

    let wide: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
    let bytes = wide.len() * std::mem::size_of::<u16>();

    unsafe {
        if OpenClipboard(std::ptr::null_mut()) == 0 {
            return false;
        }
        EmptyClipboard();

        let handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if handle.is_null() {
            CloseClipboard();
            return false;
        }

        let target = GlobalLock(handle) as *mut u16;
        if target.is_null() {
            CloseClipboard();
            return false;
        }
        std::ptr::copy_nonoverlapping(wide.as_ptr(), target, wide.len());
        GlobalUnlock(handle);

        // Ownership passes to the clipboard on success, so the handle is not
        // freed here.
        let ok = !SetClipboardData(CF_UNICODETEXT, handle as HANDLE).is_null();
        CloseClipboard();
        ok
    }
}

#[cfg(not(windows))]
fn copy_to_clipboard(_text: &str) -> bool {
    false
}

#[tauri::command]
pub fn detect_setup() -> Result<SetupStatus, String> {
    let tools = vec![
        ToolInfo {
            name: "git".into(),
            found: tool_version("git", &["--version"]).is_some(),
            version: tool_version("git", &["--version"]).unwrap_or_default(),
            install_url: "https://git-scm.com/download/win".into(),
        },
        ToolInfo {
            name: "node".into(),
            found: tool_version("node", &["--version"]).is_some(),
            version: tool_version("node", &["--version"]).unwrap_or_default(),
            install_url: "https://nodejs.org/en/download".into(),
        },
        ToolInfo {
            name: "pnpm".into(),
            found: shell_tool_version("pnpm").is_some(),
            version: shell_tool_version("pnpm").unwrap_or_default(),
            install_url: "https://pnpm.io/installation".into(),
        },
    ];

    let default_browser = default_browser_id();
    let browsers = browser_candidates()
        .into_iter()
        .map(|(id, name, paths)| {
            let found = paths.into_iter().find(|path| path.exists());
            BrowserInfo {
                is_default: default_browser == id,
                id: id.to_string(),
                name: name.to_string(),
                path: found
                    .as_ref()
                    .map(|path| path.display().to_string())
                    .unwrap_or_default(),
                installed: found.is_some(),
            }
        })
        .collect();

    let installs = discord_candidates();
    let injected = !installs.is_empty()
        && installs.iter().any(|(_, path)| discord_is_patched(path));
    let discord = installs
        .iter()
        .map(|(variant, path)| DiscordInfo {
            running: process_running(&format!("{}.exe", variant.replace(' ', ""))),
            variant: (*variant).to_string(),
            path: path.display().to_string(),
        })
        .collect();

    let checkout = vencord_dir()?;
    let vencord = VencordInfo {
        checkout_ready: checkout.join("package.json").exists(),
        dependencies_installed: checkout.join("node_modules").exists(),
        plugin_installed: checkout
            .join("src")
            .join("userplugins")
            .join(PLUGIN_FOLDER)
            .exists(),
        built: checkout.join("dist").exists(),
        injected,
        checkout_path: checkout.display().to_string(),
    };

    let staged = extension_staged_dir()?;
    Ok(SetupStatus {
        tools,
        browsers,
        default_browser_detail: default_browser_detail(),
        default_browser,
        extension_staged: staged.join("manifest.json").exists(),
        extension_staged_path: staged.display().to_string(),
        discord,
        vencord,
    })
}

// ---- progress ---------------------------------------------------------------

#[derive(Serialize, Clone)]
struct SetupProgress {
    step: String,
    detail: String,
    done: bool,
    failed: bool,
}

/// Where setup progress goes. The same routines run from the wizard and from the
/// installer, so they report through this rather than assuming a GUI exists.
/// The installer captures stdout with nsExec::ExecToLog, which is why the CLI
/// arm prints instead of emitting.
pub enum Reporter {
    Gui(AppHandle),
    Cli,
}

impl Reporter {
    fn send(&self, step: &str, detail: &str, done: bool, failed: bool) {
        match self {
            Reporter::Gui(app) => {
                let _ = app.emit(
                    "setup-progress",
                    SetupProgress {
                        step: step.to_string(),
                        detail: detail.to_string(),
                        done,
                        failed,
                    },
                );
            }
            Reporter::Cli => {
                let mark = if failed { "!!" } else if done { "ok" } else { "..." };
                println!("[{mark}] {step}: {detail}");
                use std::io::Write;
                let _ = std::io::stdout().flush();
            }
        }
    }

    fn app(&self) -> Option<&AppHandle> {
        match self {
            Reporter::Gui(app) => Some(app),
            Reporter::Cli => None,
        }
    }
}

fn progress(app: &Reporter, step: &str, detail: &str) {
    app.send(step, detail, false, false);
}

fn finished(app: &Reporter, step: &str, detail: &str) {
    app.send(step, detail, true, false);
}

fn failed(app: &Reporter, step: &str, detail: &str) {
    app.send(step, detail, true, true);
}

/// Runs a build step and streams a one-line summary. Returns the combined output
/// so failures can be reported with the real error instead of just a code.
fn run_step(
    app: &Reporter,
    step: &str,
    program: &str,
    args: &[&str],
    cwd: Option<&Path>,
    shell: bool,
) -> Result<String, String> {
    progress(app, step, &format!("{program} {}", args.join(" ")));

    let mut command = if shell {
        let mut c = Command::new("cmd");
        c.arg("/C").arg(program).args(args);
        c
    } else {
        let mut c = Command::new(program);
        c.args(args);
        c
    };
    if let Some(dir) = cwd {
        command.current_dir(dir);
    }
    no_window(&mut command);

    let output = command
        .output()
        .map_err(|error| format!("{program} could not start: {error}"))?;

    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
    let stderr = String::from_utf8_lossy(&output.stderr).to_string();
    let combined = format!("{stdout}\n{stderr}").trim().to_string();

    if !output.status.success() {
        return Err(format!("{step} failed:\n{combined}"));
    }

    // Show what the tool actually said. Reporting only the command hid a fatal
    // installer error behind a green tick.
    let tail: Vec<&str> = combined
        .lines()
        .filter(|line| !line.trim().is_empty())
        .rev()
        .take(4)
        .collect();
    let summary = tail.into_iter().rev().collect::<Vec<_>>().join("\n");
    if !summary.is_empty() {
        progress(app, step, &summary);
    }

    Ok(combined)
}

// ---- browser extension ------------------------------------------------------

/// Copies the extension somewhere permanent. Chrome remembers the folder an
/// unpacked extension was loaded from and disables it if that folder moves, so
/// this must not live in the repo or a temp directory.
#[tauri::command]
pub fn stage_browser_extension(app: AppHandle) -> Result<String, String> {
    stage_browser_extension_inner(Some(&app))
}

fn stage_browser_extension_inner(app: Option<&AppHandle>) -> Result<String, String> {
    let source = integration_source(app, &["browser", EXTENSION_FOLDER])?;
    let target = extension_staged_dir()?;

    if target.exists() {
        fs::remove_dir_all(&target).map_err(|error| error.to_string())?;
    }
    copy_dir_all(&source, &target)?;

    Ok(target.display().to_string())
}

/// Brings the browser to the front. Deliberately launches it with no URL.
///
/// Chromium refuses to navigate to privileged schemes given on the command line,
/// so passing brave://extensions silently lands on a New Tab instead. Verified
/// against plain, --new-window and --app= forms; all three open New Tab. The
/// address has to be typed or pasted by hand, so the UI shows it rather than
/// pretending the button can get there.
#[tauri::command]
pub fn open_browser(browser: String) -> Result<(), String> {
    let path = browser_candidates()
        .into_iter()
        .find(|(id, _, _)| *id == browser)
        .and_then(|(_, _, paths)| paths.into_iter().find(|path| path.exists()))
        .ok_or_else(|| format!("{browser} is not installed"))?;

    // Launch the resolved executable rather than going through `start`, which
    // depends on an App Paths entry that a portable install may not register.
    let mut command = Command::new(path);
    no_window(&mut command);
    command.spawn().map_err(|error| error.to_string())?;
    Ok(())
}

/// The address the user has to type to reach the extensions page.
pub fn extensions_url(browser: &str) -> &'static str {
    match browser {
        "brave" => "brave://extensions",
        "chrome" => "chrome://extensions",
        "edge" => "edge://extensions",
        _ => "chrome://extensions",
    }
}

#[tauri::command]
pub fn reveal_path(path: String) -> Result<(), String> {
    let mut command = Command::new("explorer");
    command.arg(&path);
    // explorer returns a non-zero exit code even on success, so the result is
    // deliberately not checked here.
    let _ = command.spawn().map_err(|error| error.to_string())?;
    Ok(())
}

#[tauri::command]
pub fn open_url(url: String) -> Result<(), String> {
    if !url.starts_with("https://") && !url.starts_with("http://") {
        return Err("only http and https links can be opened".to_string());
    }
    let mut command = Command::new("cmd");
    command.args(["/C", "start", "", &url]);
    no_window(&mut command);
    command.spawn().map_err(|error| error.to_string())?;
    Ok(())
}

// ---- Vencord ----------------------------------------------------------------

#[tauri::command]
pub fn install_pnpm(app: AppHandle) -> Result<String, String> {
    install_pnpm_inner(&Reporter::Gui(app))
}

fn install_pnpm_inner(reporter: &Reporter) -> Result<String, String> {
    if shell_tool_version("pnpm").is_some() {
        return Ok("pnpm is already installed".to_string());
    }
    if tool_version("node", &["--version"]).is_none() {
        return Err("Node.js is required before pnpm can be installed".to_string());
    }

    match run_step(reporter, "install pnpm", "npm", &["install", "-g", "pnpm"], None, true) {
        Ok(_) => {
            finished(reporter, "install pnpm", "pnpm installed");
            Ok("pnpm installed".to_string())
        }
        Err(error) => {
            failed(reporter, "install pnpm", &error);
            Err(error)
        }
    }
}

/// Clone or update Vencord, drop our plugin into src/userplugins, build, and
/// inject into Discord. Progress is streamed on the "setup-progress" event.
#[tauri::command]
pub fn install_vencord_bridge(app: AppHandle, close_discord: bool) -> Result<String, String> {
    let reporter = Reporter::Gui(app);
    let result = install_vencord_bridge_inner(&reporter, close_discord);
    match &result {
        Ok(message) => finished(&reporter, "done", message),
        Err(error) => failed(&reporter, "failed", error),
    }
    result
}

fn install_vencord_bridge_inner(app: &Reporter, close_discord: bool) -> Result<String, String> {
    if tool_version("git", &["--version"]).is_none() {
        return Err("git is not installed. Install it, then run setup again.".to_string());
    }
    if tool_version("node", &["--version"]).is_none() {
        return Err("Node.js is not installed. Install it, then run setup again.".to_string());
    }
    if shell_tool_version("pnpm").is_none() {
        return Err("pnpm is not installed. Use the Install pnpm button first.".to_string());
    }

    // Checked before any work, not after. This used to sit below the build, so a
    // machine without Discord spent several minutes cloning and compiling
    // Vencord only to be told there was nothing to patch.
    let installs = discord_candidates();
    if installs.is_empty() {
        return Err(
            "No Discord install was found, so there is nothing to patch. Install Discord and open \
             it once, then run this step again."
                .to_string(),
        );
    }

    let checkout = vencord_dir()?;
    if let Some(parent) = checkout.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }

    if checkout.join(".git").exists() {
        run_step(app, "update Vencord", "git", &["pull", "--ff-only"], Some(&checkout), false)?;
    } else {
        if checkout.exists() {
            fs::remove_dir_all(&checkout).map_err(|error| error.to_string())?;
        }
        let target = checkout.display().to_string();
        run_step(app, "clone Vencord", "git", &["clone", VENCORD_REPO, &target], None, false)?;
    }

    run_step(app, "install dependencies", "pnpm", &["install", "--frozen-lockfile"], Some(&checkout), true)?;

    progress(app, "copy plugin", "copying outputDeviceBridge into src/userplugins");
    let plugin_source = integration_source(app.app(), &["vencord", PLUGIN_FOLDER])?;
    let plugin_target = checkout.join("src").join("userplugins").join(PLUGIN_FOLDER);
    if plugin_target.exists() {
        fs::remove_dir_all(&plugin_target).map_err(|error| error.to_string())?;
    }
    copy_dir_all(&plugin_source, &plugin_target)?;

    run_step(app, "build Vencord", "pnpm", &["build"], Some(&checkout), true)?;

    if close_discord {
        progress(app, "close Discord", "stopping Discord so the patch can be written");
        for image in ["Discord.exe", "DiscordPTB.exe", "DiscordCanary.exe"] {
            let mut command = Command::new("taskkill");
            command.args(["/F", "/IM", image]);
            no_window(&mut command);
            let _ = command.output();
        }
        std::thread::sleep(std::time::Duration::from_millis(1500));
    }

    // `pnpm inject` is interactive: it asks which Discord install to patch with
    // an arrow-key menu. Run with no console it reads EOF and dies with
    // "FATAL ^D" while still exiting 0, which is how a failed patch used to be
    // reported as a success. Driving the installer CLI directly with an explicit
    // -location skips the prompt entirely. runInstaller.mjs forwards everything
    // after `--` and downloads the CLI if it is missing.
    let mut patched: Vec<String> = Vec::new();
    for (variant, install) in &installs {
        let location = install.display().to_string();
        let step = format!("patch {variant}");
        run_step(
            app,
            &step,
            "node",
            &[
                "scripts/runInstaller.mjs",
                "--",
                "-install",
                "-location",
                &location,
            ],
            Some(&checkout),
            false,
        )?;

        if discord_is_patched(install) {
            patched.push((*variant).to_string());
            finished(app, &step, &format!("{variant} patched"));
        } else {
            return Err(format!(
                "the installer reported success but {variant} is not patched. Close {variant} \
                 completely, including the tray icon, and try again."
            ));
        }
    }

    Ok(format!(
        "Vencord built and injected into {}. Start Discord, then enable OutputDeviceBridge in \
         Settings > Vencord > Plugins.",
        patched.join(", ")
    ))
}
