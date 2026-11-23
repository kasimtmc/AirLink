//! Filesystem helpers for transfers: collecting outgoing files and safely
//! mapping incoming relative paths into the download directory.

use std::path::{Component, Path, PathBuf};

use anyhow::{Context, Result, bail};

use crate::protocol::FileMeta;

/// Expands files and directories into a flat list of (local path, wire meta).
/// Directory entries keep their structure under the directory's own name.
pub fn collect_files(paths: &[PathBuf]) -> Result<Vec<(PathBuf, FileMeta)>> {
    let mut out = Vec::new();
    for p in paths {
        let md = std::fs::metadata(p).with_context(|| format!("cannot read {}", p.display()))?;
        let name = p
            .file_name()
            .and_then(|n| n.to_str())
            .with_context(|| format!("invalid file name: {}", p.display()))?
            .to_owned();
        if md.is_dir() {
            walk(p, &name, &mut out)?;
        } else {
            out.push((
                p.clone(),
                FileMeta {
                    path: name,
                    size: md.len(),
                },
            ));
        }
    }
    Ok(out)
}

fn walk(dir: &Path, rel: &str, out: &mut Vec<(PathBuf, FileMeta)>) -> Result<()> {
    let mut entries: Vec<_> = std::fs::read_dir(dir)?.collect::<Result<_, _>>()?;
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        let Some(name) = e.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        let child_rel = format!("{rel}/{name}");
        let ft = e.file_type()?;
        if ft.is_dir() {
            walk(&e.path(), &child_rel, out)?;
        } else if ft.is_file() {
            out.push((
                e.path(),
                FileMeta {
                    path: child_rel,
                    size: e.metadata()?.len(),
                },
            ));
        }
        // Symlinks and special files are skipped on purpose.
    }
    Ok(())
}

/// Turns an untrusted `/`-separated relative path into a safe relative
/// `PathBuf`, or `None` if nothing usable remains.
pub fn sanitize_rel_path(raw: &str) -> Option<PathBuf> {
    let mut out = PathBuf::new();
    for part in raw.split(['/', '\\']) {
        let part = part.trim();
        if part.is_empty() || part == "." || part == ".." {
            continue;
        }
        let mut clean: String = part
            .chars()
            .map(|c| {
                if c.is_control() || r#"<>:"|?*"#.contains(c) {
                    '_'
                } else {
                    c
                }
            })
            .collect();
        let clean_trimmed = clean.trim_end_matches(['.', ' ']).to_owned();
        clean = if clean_trimmed.is_empty() {
            "_".into()
        } else {
            clean_trimmed
        };
        if is_windows_reserved(&clean) {
            clean.insert(0, '_');
        }
        out.push(clean);
    }
    debug_assert!(out.components().all(|c| matches!(c, Component::Normal(_))));
    if out.as_os_str().is_empty() {
        None
    } else {
        Some(out)
    }
}

fn is_windows_reserved(name: &str) -> bool {
    let stem = name.split('.').next().unwrap_or("").to_ascii_uppercase();
    matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL")
        || (stem.len() == 4
            && (stem.starts_with("COM") || stem.starts_with("LPT"))
            && stem.as_bytes()[3].is_ascii_digit())
}

/// Returns `path` if free, otherwise `name (1).ext`, `name (2).ext`, ...
pub fn unique_path(path: PathBuf) -> PathBuf {
    if !path.exists() {
        return path;
    }
    let parent = path.parent().map(Path::to_path_buf).unwrap_or_default();
    let stem = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("file")
        .to_owned();
    let ext = path
        .extension()
        .and_then(|s| s.to_str())
        .map(|e| format!(".{e}"))
        .unwrap_or_default();
    for i in 1.. {
        let candidate = parent.join(format!("{stem} ({i}){ext}"));
        if !candidate.exists() {
            return candidate;
        }
    }
    unreachable!()
}

pub fn part_path(target: &Path) -> PathBuf {
    let mut s = target.as_os_str().to_owned();
    s.push(".airlink-part");
    PathBuf::from(s)
}

pub fn ensure_inside(base: &Path, p: &Path) -> Result<()> {
    if !p.starts_with(base) {
        bail!("path escapes download directory");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sanitizes_traversal_and_bad_chars() {
        assert_eq!(
            sanitize_rel_path("../../etc/passwd"),
            Some(PathBuf::from("etc/passwd"))
        );
        assert_eq!(
            sanitize_rel_path("/abs/a:b?.txt"),
            Some(PathBuf::from("abs/a_b_.txt"))
        );
        assert_eq!(
            sanitize_rel_path("dir\\..\\CON.txt"),
            Some(PathBuf::from("dir/_CON.txt"))
        );
        assert_eq!(sanitize_rel_path("../.."), None);
        assert_eq!(sanitize_rel_path("photo. "), Some(PathBuf::from("photo")));
    }
}
