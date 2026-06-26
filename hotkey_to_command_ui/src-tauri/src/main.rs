#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::Serialize;
use serde_json::{json, Value};
use std::{
    collections::HashSet,
    env, fs,
    path::{Path, PathBuf},
    process::Command,
    thread,
    time::Duration,
};

#[cfg(windows)]
use std::os::windows::ffi::OsStrExt;
#[cfg(windows)]
use std::os::windows::process::CommandExt;
#[cfg(windows)]
use windows_sys::Win32::Foundation::{
    CloseHandle, GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE,
};
#[cfg(windows)]
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, MoveFileExW, ReadFile, WriteFile, FILE_ATTRIBUTE_NORMAL,
    MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, OPEN_EXISTING,
};

const PIPE_NAME: &str = r"\\.\pipe\hotkeyd-control";

#[derive(Serialize)]
struct ConfigEnvelope {
    path: String,
    config: Value,
}

fn app_dir() -> Result<PathBuf, String> {
    let appdata = env::var_os("APPDATA").ok_or("APPDATA is not set")?;
    Ok(PathBuf::from(appdata).join("HotkeyToCommand"))
}

fn config_path() -> Result<PathBuf, String> {
    Ok(app_dir()?.join("config.json"))
}

fn default_config() -> Value {
    json!({
        "version": 1,
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

#[tauri::command]
fn load_config() -> Result<ConfigEnvelope, String> {
    let path = ensure_config_file()?;
    let text = fs::read_to_string(&path).map_err(|error| error.to_string())?;
    let config: Value = serde_json::from_str(&text).map_err(|error| error.to_string())?;
    Ok(ConfigEnvelope {
        path: path.to_string_lossy().to_string(),
        config,
    })
}

#[tauri::command]
fn save_config(config: Value) -> Result<(), String> {
    let path = ensure_config_file()?;
    let temp = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(&config).map_err(|error| error.to_string())?;
    fs::write(&temp, text).map_err(|error| error.to_string())?;
    replace_file(&temp, &path)
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
                    .join("hotkey_to_command_cpp")
                    .join("build")
                    .join("Release")
                    .join("hotkeyd.exe"),
            );
            push_candidate(
                paths,
                seen,
                ancestor
                    .join("hotkey_to_command_cpp")
                    .join("build")
                    .join("Debug")
                    .join("hotkeyd.exe"),
            );
        }
    }

    if let Some(path) = env::var_os("HOTKEYD_PATH") {
        push_candidate(&mut paths, &mut seen, PathBuf::from(path));
    }

    if let Ok(exe) = env::current_exe() {
        if let Some(dir) = exe.parent() {
            push_candidate(&mut paths, &mut seen, dir.join("hotkeyd.exe"));
            push_repo_candidates(&mut paths, &mut seen, dir);
        }
    }

    if let Ok(current_dir) = env::current_dir() {
        push_candidate(&mut paths, &mut seen, current_dir.join("hotkeyd.exe"));
        push_repo_candidates(&mut paths, &mut seen, &current_dir);
    }

    paths
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
            format!("could not find hotkeyd.exe; build the C++ daemon first\nsearched:\n{searched}")
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
fn select_app_path() -> Option<String> {
    rfd::FileDialog::new()
        .add_filter("Applications", &["exe"])
        .pick_file()
        .map(|path| path.to_string_lossy().to_string())
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            load_config,
            save_config,
            ensure_daemon,
            send_daemon_command,
            select_app_path
        ])
        .run(tauri::generate_context!())
        .expect("error while running Hotkey To Command UI");
}
