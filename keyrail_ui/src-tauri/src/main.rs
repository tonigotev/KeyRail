#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod setup;

use serde::Serialize;
use serde_json::{json, Value};
use std::{
    collections::HashSet,
    env,
    ffi::OsStr,
    fs,
    path::{Path, PathBuf},
    process::Command,
    thread,
    time::{Duration, Instant},
};

#[cfg(windows)]
use std::os::windows::ffi::OsStrExt;
#[cfg(windows)]
use std::os::windows::process::CommandExt;
#[cfg(windows)]
use windows_sys::Win32::Foundation::{
    CloseHandle, ERROR_FILE_NOT_FOUND, GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE,
};
#[cfg(windows)]
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, MoveFileExW, ReadFile, WriteFile, FILE_ATTRIBUTE_NORMAL,
    MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, OPEN_EXISTING,
};
#[cfg(windows)]
use windows_sys::Win32::System::Registry::{
    RegCloseKey, RegCreateKeyExW, RegDeleteValueW, RegOpenKeyExW, RegQueryValueExW, RegSetValueExW,
    HKEY_CURRENT_USER, KEY_READ, KEY_SET_VALUE, REG_OPTION_NON_VOLATILE, REG_SZ,
};

const PIPE_NAME: &str = r"\\.\pipe\keyrail-control";
const STARTUP_VALUE_NAME: &str = "KeyRailDaemon";
const ELEVATED_TASK_NAME: &str = "KeyRailDaemonElevated";
const RUN_KEY_PATH: &str = r"Software\Microsoft\Windows\CurrentVersion\Run";

#[derive(Serialize)]
struct ConfigEnvelope {
    path: String,
    config: Value,
    active_preset: Option<String>,
}

#[derive(Serialize)]
struct PresetInfo {
    name: String,
    path: String,
    active: bool,
}

#[derive(Serialize)]
struct BrowserBridgeStatus {
    connected: bool,
    clients: u32,
    targets: u32,
    detail: String,
}

#[derive(Serialize)]
struct StartupStatus {
    enabled: bool,
    elevated_enabled: bool,
    path: Option<String>,
    detail: String,
}

fn app_dir() -> Result<PathBuf, String> {
    let appdata = env::var_os("APPDATA").ok_or("APPDATA is not set")?;
    Ok(PathBuf::from(appdata).join("KeyRail"))
}

fn config_path() -> Result<PathBuf, String> {
    Ok(app_dir()?.join("config.json"))
}

fn presets_dir() -> Result<PathBuf, String> {
    Ok(app_dir()?.join("presets"))
}

fn active_preset_path() -> Result<PathBuf, String> {
    Ok(app_dir()?.join("active_preset.txt"))
}

fn default_config() -> Value {
    json!({
        "version": 1,
        "settings": {
            "hotkey_mode": "global",
            "command_hotkey": "ctrl+alt+space",
            "command_timeout_ms": 4000
        },
        "onboarding": {
            "completed": false,
            "version": 1,
            "seen_media_picker_extension": false,
            "seen_discord_bridge": false
        },
        "bindings": [
            {
                "id": "cycle-audio",
                "enabled": true,
                "hotkey": "ctrl+alt+o",
                "action": {
                    "type": "builtin",
                    "name": "cycle_audio_output"
                }
            },
            {
                "id": "open-notepad",
                "enabled": true,
                "hotkey": "ctrl+alt+t",
                "action": {
                    "type": "open_app",
                    "path": "C:\\Windows\\System32\\notepad.exe",
                    "args": [],
                    "show_window": true
                }
            }
        ]
    })
}

fn ensure_config_file() -> Result<PathBuf, String> {
    let path = config_path()?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }
    if !path.exists() {
        let text =
            serde_json::to_string_pretty(&default_config()).map_err(|error| error.to_string())?;
        fs::write(&path, text).map_err(|error| error.to_string())?;
    }
    Ok(path)
}

fn read_active_preset() -> Result<Option<String>, String> {
    let path = active_preset_path()?;
    if !path.exists() {
        return Ok(None);
    }
    let name = fs::read_to_string(path).map_err(|error| error.to_string())?;
    let trimmed = name.trim();
    if trimmed.is_empty() {
        Ok(None)
    } else {
        Ok(Some(trimmed.to_string()))
    }
}

fn write_active_preset(name: &str) -> Result<(), String> {
    let path = active_preset_path()?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }
    fs::write(path, name).map_err(|error| error.to_string())
}

fn clear_active_preset() -> Result<(), String> {
    let path = active_preset_path()?;
    if path.exists() {
        fs::remove_file(path).map_err(|error| error.to_string())?;
    }
    Ok(())
}

fn preset_json_path(name: &str) -> Result<PathBuf, String> {
    if !is_valid_preset_name(name) {
        return Err("invalid preset name".to_string());
    }
    Ok(presets_dir()?.join(name).join(format!("{name}.json")))
}

fn is_valid_preset_name(name: &str) -> bool {
    !name.is_empty()
        && name
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || ch == '-' || ch == '_')
}

fn next_preset_name() -> Result<String, String> {
    let dir = presets_dir()?;
    fs::create_dir_all(&dir).map_err(|error| error.to_string())?;
    for index in 1.. {
        let name = format!("preset-{index}");
        if !dir.join(&name).exists() {
            return Ok(name);
        }
    }
    Err("could not allocate preset name".to_string())
}

fn save_json_file(path: &PathBuf, config: &Value) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }
    let temp = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(config).map_err(|error| error.to_string())?;
    fs::write(&temp, text).map_err(|error| error.to_string())?;
    replace_file(&temp, path)
}

#[tauri::command]
fn load_config() -> Result<ConfigEnvelope, String> {
    let path = ensure_config_file()?;
    let text = fs::read_to_string(&path).map_err(|error| error.to_string())?;
    let config: Value = serde_json::from_str(&text).map_err(|error| error.to_string())?;
    Ok(ConfigEnvelope {
        path: path.to_string_lossy().to_string(),
        config,
        active_preset: read_active_preset()?,
    })
}

#[tauri::command]
fn save_config(config: Value) -> Result<(), String> {
    let path = ensure_config_file()?;
    save_json_file(&path, &config)?;
    if let Some(active) = read_active_preset()? {
        let preset_path = preset_json_path(&active)?;
        if preset_path.exists() {
            save_json_file(&preset_path, &config)?;
        }
    }
    Ok(())
}

#[tauri::command]
fn list_presets() -> Result<Vec<PresetInfo>, String> {
    let dir = presets_dir()?;
    fs::create_dir_all(&dir).map_err(|error| error.to_string())?;
    let active = read_active_preset()?.unwrap_or_default();
    let mut presets = Vec::new();

    for entry in fs::read_dir(&dir).map_err(|error| error.to_string())? {
        let entry = entry.map_err(|error| error.to_string())?;
        if !entry.file_type().map_err(|error| error.to_string())?.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        if !is_valid_preset_name(&name) {
            continue;
        }
        let path = entry.path().join(format!("{name}.json"));
        if !path.exists() {
            continue;
        }
        presets.push(PresetInfo {
            active: name == active,
            name,
            path: path.to_string_lossy().to_string(),
        });
    }

    presets.sort_by(|left, right| left.name.cmp(&right.name));
    Ok(presets)
}

#[tauri::command]
fn create_preset(config: Value) -> Result<PresetInfo, String> {
    let name = next_preset_name()?;
    let path = preset_json_path(&name)?;
    save_json_file(&path, &config)?;
    save_json_file(&ensure_config_file()?, &config)?;
    write_active_preset(&name)?;
    Ok(PresetInfo {
        name,
        path: path.to_string_lossy().to_string(),
        active: true,
    })
}

#[tauri::command]
fn switch_preset(name: String) -> Result<ConfigEnvelope, String> {
    let preset_path = preset_json_path(&name)?;
    if !preset_path.exists() {
        return Err(format!("preset not found: {name}"));
    }
    let text = fs::read_to_string(&preset_path).map_err(|error| error.to_string())?;
    let config: Value = serde_json::from_str(&text).map_err(|error| error.to_string())?;
    let path = ensure_config_file()?;
    save_json_file(&path, &config)?;
    write_active_preset(&name)?;
    Ok(ConfigEnvelope {
        path: path.to_string_lossy().to_string(),
        config,
        active_preset: Some(name),
    })
}

#[tauri::command]
fn delete_preset(name: String) -> Result<Option<ConfigEnvelope>, String> {
    if !is_valid_preset_name(&name) {
        return Err("invalid preset name".to_string());
    }

    let dir = presets_dir()?.join(&name);
    let active = read_active_preset()?.is_some_and(|active| active == name);
    if !dir.exists() {
        return Err(format!("preset not found: {name}"));
    }

    fs::remove_dir_all(&dir).map_err(|error| error.to_string())?;
    if !active {
        return Ok(None);
    }

    clear_active_preset()?;
    let path = ensure_config_file()?;
    let config = default_config();
    save_json_file(&path, &config)?;
    Ok(Some(ConfigEnvelope {
        path: path.to_string_lossy().to_string(),
        config,
        active_preset: None,
    }))
}

#[cfg(windows)]
fn replace_file(temp: &PathBuf, path: &PathBuf) -> Result<(), String> {
    let mut from: Vec<u16> = temp.as_os_str().encode_wide().collect();
    let mut to: Vec<u16> = path.as_os_str().encode_wide().collect();
    from.push(0);
    to.push(0);

    let ok = unsafe {
        MoveFileExW(
            from.as_ptr(),
            to.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if ok == 0 {
        Err(std::io::Error::last_os_error().to_string())
    } else {
        Ok(())
    }
}

#[cfg(not(windows))]
fn replace_file(temp: &PathBuf, path: &PathBuf) -> Result<(), String> {
    fs::rename(temp, path).map_err(|error| error.to_string())
}

fn daemon_candidates() -> Vec<PathBuf> {
    let mut paths = Vec::new();
    let mut seen = HashSet::new();

    fn push_candidate(paths: &mut Vec<PathBuf>, seen: &mut HashSet<PathBuf>, path: PathBuf) {
        if seen.insert(path.clone()) {
            paths.push(path);
        }
    }

    fn push_repo_candidates(paths: &mut Vec<PathBuf>, seen: &mut HashSet<PathBuf>, base: &Path) {
        for ancestor in base.ancestors() {
            push_candidate(
                paths,
                seen,
                ancestor
                    .join("keyrail_daemon")
                    .join("build")
                    .join("Release")
                    .join("keyraild.exe"),
            );
            push_candidate(
                paths,
                seen,
                ancestor
                    .join("keyrail_daemon")
                    .join("build")
                    .join("Debug")
                    .join("keyraild.exe"),
            );
        }
    }

    if let Some(path) = env::var_os("KEYRAIL_DAEMON_PATH") {
        push_candidate(&mut paths, &mut seen, PathBuf::from(path));
    }

    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            // An installed build ships the daemon as a bundled resource, which
            // lands beside the UI executable.
            push_candidate(&mut paths, &mut seen, dir.join("keyraild.exe"));
            push_candidate(&mut paths, &mut seen, dir.join("binaries").join("keyraild.exe"));
            push_repo_candidates(&mut paths, &mut seen, dir);
        }
    }

    if let Ok(current_dir) = env::current_dir() {
        push_candidate(&mut paths, &mut seen, current_dir.join("keyraild.exe"));
        push_repo_candidates(&mut paths, &mut seen, &current_dir);
    }

    paths
}

fn quote_windows_path(path: &Path) -> String {
    format!("\"{}\"", path.display())
}

fn startup_daemon_path() -> Result<PathBuf, String> {
    daemon_candidates()
        .into_iter()
        .find(|path| path.exists())
        .ok_or_else(|| "could not find keyraild.exe; build or install the daemon first".to_string())
}

#[cfg(windows)]
fn wide_null(value: &str) -> Vec<u16> {
    OsStr::new(value)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

#[cfg(windows)]
fn query_startup_value() -> Result<Option<String>, String> {
    let key_path = wide_null(RUN_KEY_PATH);
    let value_name = wide_null(STARTUP_VALUE_NAME);
    let mut key = std::ptr::null_mut();
    let open_result =
        unsafe { RegOpenKeyExW(HKEY_CURRENT_USER, key_path.as_ptr(), 0, KEY_READ, &mut key) };
    if open_result == ERROR_FILE_NOT_FOUND {
        return Ok(None);
    }
    if open_result != 0 {
        return Err(format!("could not open startup settings: {open_result}"));
    }

    let mut value_type = 0;
    let mut byte_len = 0;
    let query_result = unsafe {
        RegQueryValueExW(
            key,
            value_name.as_ptr(),
            std::ptr::null_mut(),
            &mut value_type,
            std::ptr::null_mut(),
            &mut byte_len,
        )
    };
    if query_result == ERROR_FILE_NOT_FOUND {
        unsafe {
            RegCloseKey(key);
        }
        return Ok(None);
    }
    if query_result != 0 {
        unsafe {
            RegCloseKey(key);
        }
        return Err(format!("could not read startup setting: {query_result}"));
    }
    if value_type != REG_SZ || byte_len < 2 {
        unsafe {
            RegCloseKey(key);
        }
        return Ok(None);
    }

    let mut buffer = vec![0u16; (byte_len as usize + 1) / 2];
    let read_result = unsafe {
        RegQueryValueExW(
            key,
            value_name.as_ptr(),
            std::ptr::null_mut(),
            &mut value_type,
            buffer.as_mut_ptr() as *mut u8,
            &mut byte_len,
        )
    };
    unsafe {
        RegCloseKey(key);
    }
    if read_result != 0 {
        return Err(format!("could not read startup setting: {read_result}"));
    }

    let end = buffer
        .iter()
        .position(|value| *value == 0)
        .unwrap_or(buffer.len());
    Ok(Some(String::from_utf16_lossy(&buffer[..end])))
}

#[cfg(windows)]
fn write_startup_value(command: &str) -> Result<(), String> {
    let key_path = wide_null(RUN_KEY_PATH);
    let value_name = wide_null(STARTUP_VALUE_NAME);
    let mut key = std::ptr::null_mut();
    let create_result = unsafe {
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            key_path.as_ptr(),
            0,
            std::ptr::null_mut(),
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            std::ptr::null(),
            &mut key,
            std::ptr::null_mut(),
        )
    };
    if create_result != 0 {
        return Err(format!("could not open startup settings: {create_result}"));
    }

    let value = wide_null(command);
    let byte_len = (value.len() * std::mem::size_of::<u16>()) as u32;
    let set_result = unsafe {
        RegSetValueExW(
            key,
            value_name.as_ptr(),
            0,
            REG_SZ,
            value.as_ptr() as *const u8,
            byte_len,
        )
    };
    unsafe {
        RegCloseKey(key);
    }
    if set_result != 0 {
        return Err(format!("could not save startup setting: {set_result}"));
    }
    Ok(())
}

#[cfg(windows)]
fn delete_startup_value() -> Result<(), String> {
    let key_path = wide_null(RUN_KEY_PATH);
    let value_name = wide_null(STARTUP_VALUE_NAME);
    let mut key = std::ptr::null_mut();
    let open_result = unsafe {
        RegOpenKeyExW(
            HKEY_CURRENT_USER,
            key_path.as_ptr(),
            0,
            KEY_SET_VALUE,
            &mut key,
        )
    };
    if open_result == ERROR_FILE_NOT_FOUND {
        return Ok(());
    }
    if open_result != 0 {
        return Err(format!("could not open startup settings: {open_result}"));
    }

    let delete_result = unsafe { RegDeleteValueW(key, value_name.as_ptr()) };
    unsafe {
        RegCloseKey(key);
    }
    if delete_result != 0 && delete_result != ERROR_FILE_NOT_FOUND {
        return Err(format!("could not remove startup setting: {delete_result}"));
    }
    Ok(())
}

#[cfg(windows)]
fn elevated_task_exists() -> bool {
    Command::new("schtasks.exe")
        .args(["/Query", "/TN", ELEVATED_TASK_NAME])
        .creation_flags(0x08000000)
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false)
}

#[cfg(windows)]
fn set_elevated_startup_task(enabled: bool) -> Result<(), String> {
    if enabled {
        let daemon = startup_daemon_path()?;
        let task_run = quote_windows_path(&daemon);
        let output = Command::new("schtasks.exe")
            .args([
                "/Create",
                "/TN",
                ELEVATED_TASK_NAME,
                "/SC",
                "ONLOGON",
                "/TR",
                &task_run,
                "/RL",
                "HIGHEST",
                "/F",
            ])
            .creation_flags(0x08000000)
            .output()
            .map_err(|error| error.to_string())?;
        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            let stdout = String::from_utf8_lossy(&output.stdout);
            return Err(format!("could not create elevated startup task: {stderr}{stdout}"));
        }
    } else {
        let output = Command::new("schtasks.exe")
            .args(["/Delete", "/TN", ELEVATED_TASK_NAME, "/F"])
            .creation_flags(0x08000000)
            .output()
            .map_err(|error| error.to_string())?;
        if !output.status.success() && elevated_task_exists() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            let stdout = String::from_utf8_lossy(&output.stdout);
            return Err(format!("could not remove elevated startup task: {stderr}{stdout}"));
        }
    }
    Ok(())
}

#[tauri::command]
fn daemon_startup_status() -> Result<StartupStatus, String> {
    #[cfg(windows)]
    {
        let value = query_startup_value()?;
        let elevated_enabled = elevated_task_exists();
        let Some(command) = value else {
            return Ok(StartupStatus {
                enabled: elevated_enabled,
                elevated_enabled,
                path: None,
                detail: if elevated_enabled {
                    "Daemon starts with Windows as administrator.".to_string()
                } else {
                    "Daemon will not start with Windows.".to_string()
                },
            });
        };

        Ok(StartupStatus {
            enabled: true,
            elevated_enabled,
            path: Some(command.clone()),
            detail: if elevated_enabled {
                format!("Windows starts the daemon normally and also has an administrator startup task: {command}")
            } else {
                format!("Windows starts the daemon with: {command}")
            },
        })
    }

    #[cfg(not(windows))]
    {
        Ok(StartupStatus {
            enabled: false,
            elevated_enabled: false,
            path: None,
            detail: "Startup registration is only available on Windows.".to_string(),
        })
    }
}

#[tauri::command]
fn set_daemon_startup(enabled: bool) -> Result<StartupStatus, String> {
    #[cfg(windows)]
    {
        if enabled {
            let daemon = startup_daemon_path()?;
            write_startup_value(&quote_windows_path(&daemon))?;
        } else {
            delete_startup_value()?;
        }
        daemon_startup_status()
    }

    #[cfg(not(windows))]
    {
        let _ = enabled;
        Err("startup registration is only available on Windows".to_string())
    }
}

#[tauri::command]
fn set_daemon_elevated_startup(enabled: bool) -> Result<StartupStatus, String> {
    #[cfg(windows)]
    {
        if enabled {
            delete_startup_value()?;
            set_elevated_startup_task(true)?;
        } else {
            set_elevated_startup_task(false)?;
        }
        daemon_startup_status()
    }

    #[cfg(not(windows))]
    {
        let _ = enabled;
        Err("elevated startup is only available on Windows".to_string())
    }
}

#[cfg(windows)]
fn send_pipe_raw(command: &str) -> Result<String, String> {
    let request = json!({ "command": command }).to_string();
    let mut pipe_name: Vec<u16> = PIPE_NAME.encode_utf16().collect();
    pipe_name.push(0);

    let mut last_error = String::new();
    for _ in 0..20 {
        let pipe = unsafe {
            CreateFileW(
                pipe_name.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                std::ptr::null_mut(),
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                std::ptr::null_mut(),
            )
        };

        if pipe != INVALID_HANDLE_VALUE {
            let mut written = 0;
            let write_ok = unsafe {
                WriteFile(
                    pipe,
                    request.as_ptr().cast(),
                    request.len() as u32,
                    &mut written,
                    std::ptr::null_mut(),
                )
            };
            if write_ok == 0 {
                let error = std::io::Error::last_os_error().to_string();
                unsafe { CloseHandle(pipe) };
                return Err(error);
            }

            let mut buffer = vec![0u8; 8192];
            let mut read = 0;
            let read_ok = unsafe {
                ReadFile(
                    pipe,
                    buffer.as_mut_ptr().cast(),
                    buffer.len() as u32,
                    &mut read,
                    std::ptr::null_mut(),
                )
            };
            unsafe { CloseHandle(pipe) };

            if read_ok == 0 {
                return Err(std::io::Error::last_os_error().to_string());
            }

            buffer.truncate(read as usize);
            return String::from_utf8(buffer).map_err(|error| error.to_string());
        }

        last_error = std::io::Error::last_os_error().to_string();
        thread::sleep(Duration::from_millis(50));
    }

    Err(last_error)
}

#[cfg(windows)]
fn wait_for_daemon_pipe_down(timeout: Duration) -> bool {
    let start = Instant::now();
    while start.elapsed() < timeout {
        if send_pipe_raw("status").is_err() {
            return true;
        }
        thread::sleep(Duration::from_millis(100));
    }
    send_pipe_raw("status").is_err()
}

#[cfg(windows)]
fn wait_for_daemon_pipe_status(timeout: Duration, needle: &str) -> bool {
    let start = Instant::now();
    while start.elapsed() < timeout {
        if let Ok(status) = send_pipe_raw("status") {
            if status.to_ascii_lowercase().contains(needle) {
                return true;
            }
        }
        thread::sleep(Duration::from_millis(150));
    }
    false
}

#[cfg(not(windows))]
fn send_pipe_raw(_command: &str) -> Result<String, String> {
    Err("daemon control pipe is Windows-only".to_string())
}

#[tauri::command]
fn send_daemon_command(command: String) -> Result<String, String> {
    let response = send_pipe_raw(&command)?;
    let value: Value = serde_json::from_str(&response).map_err(|error| error.to_string())?;
    if !value.get("ok").and_then(Value::as_bool).unwrap_or(false) {
        return Err(value
            .get("message")
            .and_then(Value::as_str)
            .unwrap_or("daemon command failed")
            .to_string());
    }
    Ok(value
        .get("status")
        .or_else(|| value.get("message"))
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string())
}

fn parse_count_line(status: &str, prefix: &str) -> u32 {
    status
        .lines()
        .find_map(|line| line.trim().strip_prefix(prefix))
        .and_then(|value| value.trim().parse::<u32>().ok())
        .unwrap_or(0)
}

#[tauri::command]
fn browser_bridge_status() -> Result<BrowserBridgeStatus, String> {
    let status = send_pipe_raw("browser_status")
        .and_then(|response| {
            let value: Value =
                serde_json::from_str(&response).map_err(|error| error.to_string())?;
            if !value.get("ok").and_then(Value::as_bool).unwrap_or(false) {
                return Err(value
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or("browser bridge status failed")
                    .to_string());
            }
            Ok(value
                .get("status")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string())
        })
        .unwrap_or_else(|error| format!("Daemon is not responding: {error}"));

    let clients = parse_count_line(&status, "browser bridge clients:");
    let targets = parse_count_line(&status, "browser targets:");

    Ok(BrowserBridgeStatus {
        connected: clients > 0,
        clients,
        targets,
        detail: status,
    })
}

/// Connected Vencord clients. Zero means any Discord device action will silently
/// do nothing, which the bindings list warns about.
#[tauri::command]
fn discord_bridge_clients() -> i64 {
    send_pipe_raw("discord_status")
        .ok()
        .and_then(|response| serde_json::from_str::<Value>(&response).ok())
        .and_then(|value| value.get("clients").and_then(Value::as_i64))
        .unwrap_or(0)
}

#[tauri::command]
fn ensure_daemon() -> Result<(), String> {
    if send_pipe_raw("status").is_ok() {
        return Ok(());
    }

    let daemon = daemon_candidates()
        .into_iter()
        .find(|path| path.exists())
        .ok_or_else(|| {
            let searched = daemon_candidates()
                .into_iter()
                .map(|path| format!("  {}", path.display()))
                .collect::<Vec<_>>()
                .join("\n");
            format!("could not find keyraild.exe; build the C++ daemon first\nsearched:\n{searched}")
        })?;

    let mut command = Command::new(daemon);
    #[cfg(windows)]
    command.creation_flags(0x08000000);
    command.spawn().map_err(|error| error.to_string())?;

    for _ in 0..20 {
        thread::sleep(Duration::from_millis(100));
        if send_pipe_raw("status").is_ok() {
            return Ok(());
        }
    }

    Err("daemon started but did not open the control pipe".to_string())
}

#[tauri::command]
fn restart_daemon_as_admin() -> Result<(), String> {
    #[cfg(windows)]
    {
        let daemon = startup_daemon_path()?;

        if send_pipe_raw("status").is_ok() {
            let _ = send_pipe_raw("quit");
            if !wait_for_daemon_pipe_down(Duration::from_secs(6)) {
                return Err(
                    "the existing daemon did not stop, so administrator restart was cancelled"
                        .to_string(),
                );
            }
        }

        let script = format!(
            "Start-Process -FilePath '{}' -Verb RunAs -WindowStyle Hidden",
            daemon.display().to_string().replace('\'', "''")
        );
        Command::new("powershell.exe")
            .args(["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", &script])
            .creation_flags(0x08000000)
            .spawn()
            .map_err(|error| error.to_string())?;

        let _ = wait_for_daemon_pipe_status(Duration::from_secs(8), "daemon elevated: yes");
        Ok(())
    }

    #[cfg(not(windows))]
    {
        Err("administrator restart is only available on Windows".to_string())
    }
}

#[tauri::command]
fn select_app_path() -> Option<String> {
    rfd::FileDialog::new()
        .add_filter("Applications", &["exe"])
        .pick_file()
        .map(|path| path.to_string_lossy().to_string())
}

#[tauri::command]
fn select_script_path() -> Option<String> {
    rfd::FileDialog::new()
        .add_filter(
            "Scripts and executables",
            &[
                "exe", "com", "py", "pyw", "ps1", "bat", "cmd", "js", "mjs", "cjs", "vbs", "wsf",
                "ahk",
            ],
        )
        .pick_file()
        .map(|path| path.to_string_lossy().to_string())
}

#[tauri::command]
fn open_browser_extensions_page(browser: String) -> Result<(), String> {
    // Replace these search URLs with the exact store listing URLs after publishing.
    let (exe, url) = match browser.as_str() {
        "brave" => ("brave.exe", "https://chromewebstore.google.com/search/Hotkey%20To%20Command%20Media%20Bridge"),
        "chrome" => ("chrome.exe", "https://chromewebstore.google.com/search/Hotkey%20To%20Command%20Media%20Bridge"),
        "edge" => ("msedge.exe", "https://microsoftedge.microsoft.com/addons/search/Hotkey%20To%20Command%20Media%20Bridge"),
        _ => return Err("unknown browser".to_string()),
    };

    let mut command = Command::new("cmd");
    command.args(["/C", "start", "", exe, url]);
    #[cfg(windows)]
    command.creation_flags(0x08000000);
    command.spawn().map_err(|error| error.to_string())?;
    Ok(())
}

/// `--run-setup <targets>` performs integration setup without opening a window,
/// so the installer can do it inline and show the output.
///
/// This is a windows subsystem binary, so it owns no console. Attaching to the
/// caller's console is what makes println! visible when a human runs it from a
/// terminal. Critically, that must NOT happen when stdout is already a pipe:
/// nsExec::ExecToLog reads through one, and attaching a console replaces the
/// inherited pipe handle, which silently throws the log away.
fn run_setup_cli() -> Option<i32> {
    let args: Vec<String> = env::args().collect();
    let index = args.iter().position(|arg| arg == "--run-setup")?;
    let targets = args.get(index + 1).cloned().unwrap_or_default();

    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::System::Console::{
            AttachConsole, GetStdHandle, ATTACH_PARENT_PROCESS, STD_OUTPUT_HANDLE,
        };

        let existing = GetStdHandle(STD_OUTPUT_HANDLE);
        if existing.is_null() || existing == INVALID_HANDLE_VALUE {
            AttachConsole(ATTACH_PARENT_PROCESS);
        }
    }

    Some(setup::run_headless_setup(&targets))
}

fn main() {
    if let Some(code) = run_setup_cli() {
        std::process::exit(code);
    }

    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            load_config,
            save_config,
            list_presets,
            create_preset,
            switch_preset,
            delete_preset,
            ensure_daemon,
            send_daemon_command,
            daemon_startup_status,
            set_daemon_startup,
            set_daemon_elevated_startup,
            restart_daemon_as_admin,
            browser_bridge_status,
            discord_bridge_clients,
            select_app_path,
            select_script_path,
            open_browser_extensions_page,
            setup::detect_setup,
            setup::take_pending_setup,
            setup::stage_browser_extension,
            setup::open_browser,
            setup::reveal_path,
            setup::open_url,
            setup::install_pnpm,
            setup::install_build_tools,
            setup::install_vencord_bridge
        ])
        .run(tauri::generate_context!())
        .expect("error while running KeyRail UI");
}
