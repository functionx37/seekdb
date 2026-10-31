// Copyright (c) 2025 OceanBase.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use crate::bridge::ffi::Cell;
use serde::Deserialize;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
#[cfg(unix)]
use std::{ffi::CString, os::fd::AsRawFd};

type Result<T> = std::result::Result<T, String>;
const MAX_RECORD: usize = 16 * 1024 * 1024;
fn comma() -> String { ",".into() }
fn quote() -> String { "\"".into() }

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Options {
    path: String,
    #[serde(default)] header: bool,
    #[serde(default = "comma")] delimiter: String,
    #[serde(default = "quote")] quote: String,
    #[serde(default = "quote")] escape: String,
    #[serde(default)] nullstr: String,
    columns: Vec<String>,
}

pub struct Export {
    options: Options,
    writer: BufWriter<File>,
    #[cfg(unix)] directory: File,
    #[cfg(unix)] temporary: CString,
    #[cfg(unix)] destination: CString,
    parent_path: PathBuf,
    policy_root: PathBuf,
    canonical_root: PathBuf,
    finished: bool,
}

#[cfg(unix)]
fn create(path: &Path, root: &str) -> Result<(File, File, CString, CString)> {
    use std::os::fd::{FromRawFd, OwnedFd};
    use std::os::unix::ffi::OsStrExt;
    use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQUENCE: AtomicU64 = AtomicU64::new(0);
    if root.is_empty() || root.eq_ignore_ascii_case("NULL") {
        return Err("secure_file_priv does not allow COPY".into());
    }
    let source = std::env::current_dir().map_err(|e| e.to_string())?.join(path);
    let name = source.file_name().ok_or("COPY requires a file name")?;
    let parent = source.parent().ok_or("COPY requires a parent directory")?
        .canonicalize().map_err(|e| e.to_string())?;
    let root = Path::new(root).canonicalize().map_err(|e| e.to_string())?;
    let relative = parent.strip_prefix(&root).map_err(|_| "COPY path is outside secure_file_priv")?;
    let before = std::fs::metadata(&root).map_err(|e| e.to_string())?;
    let directory = std::fs::OpenOptions::new().read(true)
        .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(&root).map_err(|e| e.to_string())?;
    let after = directory.metadata().map_err(|e| e.to_string())?;
    if before.dev() != after.dev() || before.ino() != after.ino() {
        return Err("secure_file_priv changed during COPY open".into());
    }
    let mut directory: OwnedFd = directory.into();
    for component in relative.components() {
        if !matches!(component, std::path::Component::Normal(_)) { return Err("invalid COPY path".into()); }
        let part = CString::new(component.as_os_str().as_bytes()).map_err(|e| e.to_string())?;
        // SAFETY: directory owns the parent fd and part is NUL-terminated.
        let fd = unsafe { libc::openat(directory.as_raw_fd(), part.as_ptr(),
            libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC) };
        if fd < 0 { return Err(std::io::Error::last_os_error().to_string()); }
        // SAFETY: openat returned a fresh descriptor.
        directory = unsafe { OwnedFd::from_raw_fd(fd) };
    }
    let destination = CString::new(name.as_bytes()).map_err(|e| e.to_string())?;
    let mut metadata = std::mem::MaybeUninit::<libc::stat>::uninit();
    // SAFETY: metadata provides writable stat storage; strings and fd are valid.
    let exists = unsafe { libc::fstatat(directory.as_raw_fd(), destination.as_ptr(),
        metadata.as_mut_ptr(), libc::AT_SYMLINK_NOFOLLOW) };
    if exists == 0 { return Err("COPY destination already exists".into()); }
    let error = std::io::Error::last_os_error();
    if error.raw_os_error() != Some(libc::ENOENT) { return Err(error.to_string()); }
    for _ in 0..100 {
        let nonce = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)
            .map_err(|e| e.to_string())?.as_nanos();
        let temporary = CString::new(format!(".copy-{}-{nonce}-{}", std::process::id(),
            SEQUENCE.fetch_add(1, Ordering::Relaxed))).map_err(|e| e.to_string())?;
        // SAFETY: create a new regular file relative to the pinned directory.
        let fd = unsafe { libc::openat(directory.as_raw_fd(), temporary.as_ptr(),
            libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL | libc::O_NOFOLLOW | libc::O_CLOEXEC, 0o600) };
        if fd >= 0 {
            // SAFETY: this function transfers unique ownership of both fds.
            return Ok((unsafe { File::from_raw_fd(fd) }, File::from(directory), temporary, destination));
        }
        let error = std::io::Error::last_os_error();
        if error.raw_os_error() != Some(libc::EEXIST) { return Err(error.to_string()); }
    }
    Err("could not create COPY temporary file".into())
}

pub fn open(options: &str, root: &str) -> Result<Box<Export>> {
    if options.len() > 2 * 1024 * 1024 { return Err("COPY options exceed resource limit".into()); }
    let options: Options = serde_json::from_str(options).map_err(|e| e.to_string())?;
    if options.columns.is_empty() || options.columns.len() > 1024 { return Err("invalid COPY column count".into()); }
    for value in [&options.delimiter, &options.quote, &options.escape] {
        if value.len() != 1 || !value.is_ascii() || matches!(value.as_bytes()[0], 0 | b'\r' | b'\n') {
            return Err("COPY delimiter, quote and escape must be single ASCII characters".into());
        }
    }
    if options.delimiter == options.quote || options.delimiter == options.escape
        || options.nullstr.bytes().any(|b| matches!(b, b'\r' | b'\n')
            || b == options.delimiter.as_bytes()[0] || b == options.quote.as_bytes()[0]) {
        return Err("COPY options cannot be read back as CSV".into());
    }
    #[cfg(unix)]
    {
        let source = std::env::current_dir().map_err(|e| e.to_string())?.join(&options.path);
        let parent_path = source.parent().ok_or("COPY requires a directory")?
            .canonicalize().map_err(|e| e.to_string())?;
        let policy_root = PathBuf::from(root);
        let canonical_root = policy_root.canonicalize().map_err(|e| e.to_string())?;
        let (file, directory, temporary, destination) = create(Path::new(&options.path), root)?;
        let mut export = Box::new(Export { options, writer: BufWriter::with_capacity(65536, file),
            directory, temporary, destination, parent_path, policy_root, canonical_root, finished: false });
        if export.options.header {
            let columns: Vec<_> = export.options.columns.iter().map(|text| Cell {
                text: text.clone(), integer: 0, number: 0.0, is_null: false }).collect();
            export.write_row(&columns)?;
        }
        Ok(export)
    }
    #[cfg(not(unix))]
    { let _ = (options, root); Err("COPY secure writes are not supported on this platform".into()) }
}

impl Export {
    fn check_directory(&self) -> Result<()> {
        if self.policy_root.canonicalize().map_err(|e| e.to_string())? != self.canonical_root
            || self.parent_path.canonicalize().map_err(|e| e.to_string())? != self.parent_path
            || !self.parent_path.starts_with(&self.canonical_root) {
            return Err("COPY destination directory changed".into());
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::MetadataExt;
            let expected = self.directory.metadata().map_err(|e| e.to_string())?;
            let current = self.parent_path.metadata().map_err(|e| e.to_string())?;
            if expected.dev() != current.dev() || expected.ino() != current.ino() {
                return Err("COPY destination directory changed".into());
            }
        }
        Ok(())
    }

    pub fn write_row(&mut self, cells: &[Cell]) -> Result<()> {
        if self.finished || cells.len() != self.options.columns.len() { return Err("invalid COPY row".into()); }
        self.check_directory()?;
        let quote = self.options.quote.as_bytes()[0];
        let escape = self.options.escape.as_bytes()[0];
        let mut record = Vec::new();
        for (i, cell) in cells.iter().enumerate() {
            if i > 0 { record.extend_from_slice(self.options.delimiter.as_bytes()); }
            if cell.is_null {
                record.extend_from_slice(self.options.nullstr.as_bytes());
            } else {
                record.push(quote);
                for &byte in cell.text.as_bytes() {
                    if byte == quote || byte == escape { record.push(escape); }
                    record.push(byte);
                    if record.len() >= MAX_RECORD { return Err("COPY record exceeds resource limit".into()); }
                }
                record.push(quote);
            }
            if record.len() >= MAX_RECORD { return Err("COPY record exceeds resource limit".into()); }
        }
        record.push(b'\n');
        self.writer.write_all(&record).map_err(|e| e.to_string())
    }

    pub fn finish(&mut self) -> Result<()> {
        if self.finished { return Err("COPY was already published".into()); }
        self.check_directory()?;
        self.writer.flush().map_err(|e| e.to_string())?;
        self.writer.get_ref().sync_data().map_err(|e| e.to_string())?;
        #[cfg(unix)]
        {
            // linkat publishes atomically and fails if any destination exists.
            // SAFETY: both names are relative to the retained directory fd.
            if unsafe { libc::linkat(self.directory.as_raw_fd(), self.temporary.as_ptr(),
                self.directory.as_raw_fd(), self.destination.as_ptr(), 0) } != 0 {
                return Err(std::io::Error::last_os_error().to_string());
            }
            self.finished = true;
            // SAFETY: remove only the temporary name created by this object.
            unsafe { libc::unlinkat(self.directory.as_raw_fd(), self.temporary.as_ptr(), 0); }
            Ok(())
        }
        #[cfg(not(unix))]
        { Err("COPY is not supported on this platform".into()) }
    }
}

impl Drop for Export {
    fn drop(&mut self) {
        #[cfg(unix)]
        // SAFETY: the retained directory and name belong to this exporter.
        if !self.finished {
            unsafe { libc::unlinkat(self.directory.as_raw_fd(), self.temporary.as_ptr(), 0); }
        }
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use tempfile::TempDir;

    fn options(root: &TempDir, filename: &str) -> String {
        serde_json::json!({"path":root.path().join(filename), "columns":["a","b"],
            "header":true, "nullstr":"NULL"}).to_string()
    }
    fn cell(text: &str, is_null: bool) -> Cell {
        Cell { text: text.into(), integer: 0, number: 0.0, is_null }
    }

    #[test]
    fn csv_roundtrip_and_publish_only_on_success() {
        let root = TempDir::new().unwrap();
        let mut writer = open(&options(&root, "out.csv"), root.path().to_str().unwrap()).unwrap();
        writer.write_row(&[cell("你好,\"world\"\r\nnext", false), cell("", true)]).unwrap();
        writer.write_row(&[cell("", false), cell("NULL", false)]).unwrap();
        assert!(!root.path().join("out.csv").exists());
        writer.finish().unwrap();
        drop(writer);
        let plan = crate::reader::prepare(&serde_json::json!({"path":root.path().join("out.csv"),
            "format":"csv", "nullstr":"NULL"}).to_string(), root.path().to_str().unwrap()).unwrap();
        let mut scan = crate::reader::open(&plan, root.path().to_str().unwrap(), &[0,1]).unwrap();
        let data = scan.next_batch(10).unwrap();
        assert_eq!(data.rows, 2);
        assert_eq!(data.cells[0].text, "你好,\"world\"\r\nnext");
        assert!(data.cells[1].is_null);
        assert_eq!(data.cells[2].text, "");
        assert!(!data.cells[2].is_null);
        assert_eq!(data.cells[3].text, "NULL");
        assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 1);
    }

    #[test]
    fn failure_cleans_temporary_and_never_overwrites() {
        let root = TempDir::new().unwrap();
        let request = options(&root, "out.csv");
        let writer = open(&request, root.path().to_str().unwrap()).unwrap();
        drop(writer); // A failed SQL query abandons its exporter.
        assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 0);
        let mut writer = open(&request, root.path().to_str().unwrap()).unwrap();
        std::fs::write(root.path().join("out.csv"), "existing").unwrap();
        assert!(writer.finish().is_err());
        drop(writer);
        assert_eq!(std::fs::read_to_string(root.path().join("out.csv")).unwrap(), "existing");
        assert!(open(&request, root.path().to_str().unwrap()).is_err());
        assert_eq!(std::fs::read_dir(root.path()).unwrap().count(), 1);
    }

    #[test]
    fn reject_escape_from_secure_root_and_missing_policy() {
        let root = TempDir::new().unwrap();
        let outside = TempDir::new().unwrap();
        let request = options(&outside, "out.csv");
        assert!(open(&request, root.path().to_str().unwrap()).is_err());
        assert!(open(&request, "").is_err());
        assert!(open(&request, "NULL").is_err());
        std::os::unix::fs::symlink(outside.path(), root.path().join("link")).unwrap();
        assert!(open(&options(&root, "link/out.csv"), root.path().to_str().unwrap()).is_err());
        assert_eq!(std::fs::read_dir(outside.path()).unwrap().count(), 0);
    }

    #[test]
    fn moving_output_directory_cannot_publish_outside_policy() {
        let root = TempDir::new().unwrap();
        let outside = TempDir::new().unwrap();
        std::fs::create_dir(root.path().join("nested")).unwrap();
        let mut writer = open(&options(&root, "nested/out.csv"), root.path().to_str().unwrap()).unwrap();
        std::fs::rename(root.path().join("nested"), outside.path().join("moved")).unwrap();
        assert!(writer.finish().is_err());
        drop(writer);
        assert_eq!(std::fs::read_dir(outside.path().join("moved")).unwrap().count(), 0);
    }
}
