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

use crate::bridge::ffi;
use parquet::basic::{ConvertedType, Repetition, Type as PhysicalType};
use parquet::file::reader::FileReader;
use parquet::file::serialized_reader::SerializedFileReader;
use parquet::record::{reader::RowIter, Field};
use parquet::schema::types::Type as SchemaType;
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fs::{File, Metadata};
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

const MAX_RECORD: usize = 16 * 1024 * 1024;
const MAX_COLUMNS: usize = 1024;
const MAX_SCHEMA: usize = 1024 * 1024;
const MAX_GROUP: i64 = 64 * 1024 * 1024;
const MAX_FILES: usize = 10000;
const NULL: u8 = 0;
const BIGINT: u8 = 1;
const DOUBLE: u8 = 2;
const BOOLEAN: u8 = 3;
const VARCHAR: u8 = 4;
const DATE: u8 = 5;
const DATETIME: u8 = 6;
type Result<T> = std::result::Result<T, String>;

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Options {
    path: String,
    format: String,
    #[serde(default = "yes")]
    header: bool,
    #[serde(default = "comma")]
    delimiter: String,
    #[serde(default = "quote")]
    quote: String,
    #[serde(default = "quote")]
    escape: String,
    #[serde(default)]
    nullstr: String,
}
fn yes() -> bool { true }
fn comma() -> String { ",".into() }
fn quote() -> String { "\"".into() }

#[derive(Clone, Deserialize, Serialize, Debug, PartialEq)]
struct Column { name: String, kind: u8, nullable: bool }

#[derive(Clone, Deserialize, Serialize, Debug, PartialEq)]
struct Snapshot {
    size: u64,
    modified: u128,
    #[cfg(unix)]
    device: u64,
    #[cfg(unix)]
    inode: u64,
    #[cfg(unix)]
    changed: (i64, i64),
}
fn snapshot(metadata: Metadata) -> Result<Snapshot> {
    if !metadata.is_file() { return Err("file SQL requires a regular file".into()); }
    #[cfg(unix)]
    use std::os::unix::fs::MetadataExt;
    Ok(Snapshot {
        size: metadata.len(),
        modified: metadata.modified().map_err(error)?.duration_since(std::time::UNIX_EPOCH)
            .map_err(error)?.as_nanos(),
        #[cfg(unix)] device: metadata.dev(),
        #[cfg(unix)] inode: metadata.ino(),
        #[cfg(unix)] changed: (metadata.ctime(), metadata.ctime_nsec()),
    })
}

#[derive(Clone, Deserialize, Serialize)]
struct Plan { options: Options, source: PathBuf, path: PathBuf, snapshot: Option<Snapshot>, columns: Vec<Column> }
fn error(e: impl std::fmt::Display) -> String { e.to_string() }
fn decode_plan(text: &str) -> Result<Plan> {
    if text.len() > MAX_SCHEMA * 2 { return Err("file SQL plan exceeds resource limit".into()); }
    serde_json::from_str(text).map_err(error)
}

/// Canonicalize for containment, then walk from a pinned root directory using
/// openat/O_NOFOLLOW. A symlink replacement cannot redirect the actual open.
fn secure_file(path: &Path, root: &str) -> Result<(File, PathBuf)> {
    if root.is_empty() || root.eq_ignore_ascii_case("NULL") {
        return Err("secure_file_priv does not allow file SQL".into());
    }
    let root = Path::new(root).canonicalize().map_err(error)?;
    let canonical = path.canonicalize().map_err(error)?;
    let relative = canonical.strip_prefix(&root).map_err(|_| "path is outside secure_file_priv")?;
    #[cfg(unix)]
    {
        use std::ffi::CString;
        use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
        use std::os::unix::ffi::OsStrExt;
        use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
        let before = std::fs::metadata(&root).map_err(error)?;
        let directory = std::fs::OpenOptions::new().read(true)
            .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC).open(&root).map_err(error)?;
        let after = directory.metadata().map_err(error)?;
        if before.dev() != after.dev() || before.ino() != after.ino() {
            return Err("secure_file_priv directory changed during open".into());
        }
        let parts: Vec<_> = relative.components().collect();
        if parts.is_empty() { return Err("file SQL requires a file path".into()); }
        let mut handle: OwnedFd = directory.into();
        for (i, part) in parts.iter().enumerate() {
            if !matches!(part, std::path::Component::Normal(_)) { return Err("invalid path component".into()); }
            let name = CString::new(part.as_os_str().as_bytes()).map_err(error)?;
            let flags = libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK
                | if i + 1 == parts.len() { 0 } else { libc::O_DIRECTORY };
            // SAFETY: name is NUL-terminated; handle owns an open directory fd.
            let fd = unsafe { libc::openat(handle.as_raw_fd(), name.as_ptr(), flags) };
            if fd < 0 { return Err(error(std::io::Error::last_os_error())); }
            // SAFETY: openat returned a new fd; it has exactly this owner.
            handle = unsafe { OwnedFd::from_raw_fd(fd) };
        }
        let file = File::from(handle);
        snapshot(file.metadata().map_err(error)?)?;
        Ok((file, canonical))
    }
    #[cfg(not(unix))]
    { let _ = relative; Err("file SQL secure opens are not supported on this platform".into()) }
}

#[derive(Clone, Debug)]
struct Raw { text: String, kind: u8, null: bool }
impl Raw {
    fn null() -> Self { Self { text: String::new(), kind: NULL, null: true } }
    fn text(text: String, infer: bool) -> Self {
        let kind = if infer { infer_type(&text) } else {
            match infer_type(&text) { DATE => DATE, DATETIME => DATETIME, _ => VARCHAR }
        };
        Self { text, kind, null: false }
    }
}

fn merge_type(a: u8, b: u8) -> u8 {
    match (a, b) {
        (NULL, _) => b, (_, NULL) => a,
        _ if a == b => a,
        (BIGINT, DOUBLE) | (DOUBLE, BIGINT) => DOUBLE,
        (DATE, DATETIME) | (DATETIME, DATE) => DATETIME,
        _ => VARCHAR,
    }
}

fn digits(text: &str) -> Option<i64> {
    if text.is_empty() || !text.bytes().all(|b| b.is_ascii_digit()) { return None; }
    text.parse().ok()
}
fn date(text: &str) -> Option<i64> {
    if text.len() != 10 || !text.is_ascii() || &text[4..5] != "-" || &text[7..8] != "-" { return None; }
    let y = digits(&text[..4])?; let m = digits(&text[5..7])?; let d = digits(&text[8..])?;
    let leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    let days = [31, if leap { 29 } else { 28 }, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if !(1..=9999).contains(&y) || !(1..=12).contains(&m) || d < 1 || d > days[(m - 1) as usize] { return None; }
    let y = y - i64::from(m <= 2); let era = y / 400; let year = y - era * 400;
    let month = m + if m > 2 { -3 } else { 9 };
    Some(era * 146097 + year * 365 + year / 4 - year / 100 + (153 * month + 2) / 5 + d - 1 - 719468)
}
fn datetime(text: &str) -> Option<i64> {
    if text.len() < 19 || text.len() > 26 || !text.is_ascii()
        || !matches!(text.as_bytes()[10], b' ' | b'T') || &text[13..14] != ":" || &text[16..17] != ":" { return None; }
    let day = date(&text[..10])?; let h = digits(&text[11..13])?;
    let m = digits(&text[14..16])?; let s = digits(&text[17..19])?;
    if h > 23 || m > 59 || s > 59 { return None; }
    let fraction = if text.len() > 19 {
        if &text[19..20] != "." { return None; }
        digits(&text[20..])? * 10_i64.pow((26 - text.len()) as u32)
    } else { 0 };
    Some((day * 86400 + h * 3600 + m * 60 + s) * 1_000_000 + fraction)
}
fn infer_type(text: &str) -> u8 {
    if text.eq_ignore_ascii_case("true") || text.eq_ignore_ascii_case("false") { return BOOLEAN; }
    if text.parse::<i64>().is_ok() { return BIGINT; }
    // Integer overflow stays textual; do not silently round it through f64.
    let unsigned = text.strip_prefix(['+', '-']).unwrap_or(text);
    if unsigned.bytes().all(|b| b.is_ascii_digit()) { return VARCHAR; }
    if (text.contains('.') || text.contains('e') || text.contains('E'))
        && text.parse::<f64>().is_ok_and(f64::is_finite) { return DOUBLE; }
    if date(text).is_some() { DATE } else if datetime(text).is_some() { DATETIME } else { VARCHAR }
}

struct Csv { input: BufReader<File>, options: Options, row: usize }
impl Csv {
    fn byte(&mut self) -> Result<Option<u8>> {
        let buffer = self.input.fill_buf().map_err(error)?;
        let byte = buffer.first().copied();
        if byte.is_some() { self.input.consume(1); }
        Ok(byte)
    }
    fn peek(&mut self) -> Result<Option<u8>> { Ok(self.input.fill_buf().map_err(error)?.first().copied()) }
    fn record(&mut self) -> Result<Option<Vec<Raw>>> {
        let delimiter = self.options.delimiter.as_bytes()[0];
        let quote = self.options.quote.as_bytes()[0];
        let escape = self.options.escape.as_bytes()[0];
        let mut fields = Vec::new(); let mut field = Vec::new();
        let mut quoted = false; let mut in_quote = false; let mut closed = false;
        let mut seen = false; let mut size = 0;
        loop {
            let next = self.byte()?;
            if next.is_some() { seen = true; size += 1; }
            if size > MAX_RECORD { return Err(format!("CSV record {} exceeds resource limit", self.row + 1)); }
            if in_quote {
                match next {
                    None => return Err(format!("CSV record {}: unterminated quoted field", self.row + 1)),
                    Some(ch) if ch == escape && escape != quote => {
                        field.push(self.byte()?.ok_or("CSV: unterminated escape")?); size += 1;
                    }
                    Some(ch) if ch == quote => {
                        if escape == quote && self.peek()? == Some(quote) { self.byte()?; field.push(quote); size += 1; }
                        else { in_quote = false; closed = true; }
                    }
                    Some(ch) => field.push(ch),
                }
                continue;
            }
            if next == Some(quote) && field.is_empty() && !closed {
                quoted = true; in_quote = true; continue;
            }
            let end = next.is_none() || matches!(next, Some(b'\r' | b'\n'));
            if end || next == Some(delimiter) {
                if !seen && fields.is_empty() && field.is_empty() { return Ok(None); }
                let text = String::from_utf8(std::mem::take(&mut field)).map_err(|e| format!("CSV record {}: {e}", self.row + 1))?;
                fields.push(if !quoted && text == self.options.nullstr { Raw::null() } else { Raw::text(text, true) });
                if fields.len() > MAX_COLUMNS { return Err("CSV column count exceeds resource limit".into()); }
                quoted = false; closed = false;
                if end {
                    if next == Some(b'\r') && self.peek()? == Some(b'\n') { self.byte()?; }
                    self.row += 1;
                    return Ok(Some(fields));
                }
            } else if closed || next == Some(quote) {
                return Err(format!("CSV record {}: unexpected character in quoted field", self.row + 1));
            } else if let Some(ch) = next { field.push(ch); }
        }
    }
}

#[derive(Default)]
struct FlatObject(Vec<(String, Raw)>);
impl<'de> Deserialize<'de> for FlatObject {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> std::result::Result<Self, D::Error> {
        struct Visitor;
        impl<'de> serde::de::Visitor<'de> for Visitor {
            type Value = FlatObject;
            fn expecting(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result { f.write_str("a flat JSON object") }
            fn visit_map<A: serde::de::MapAccess<'de>>(self, mut map: A) -> std::result::Result<FlatObject, A::Error> {
                let mut values = Vec::new(); let mut names = HashSet::new();
                while let Some((name, value)) = map.next_entry::<String, serde_json::Value>()? {
                    if values.len() >= MAX_COLUMNS || !names.insert(name.clone()) {
                        return Err(serde::de::Error::custom("duplicate JSON field or column limit exceeded"));
                    }
                    let raw = match value {
                        serde_json::Value::Null => Raw::null(),
                        serde_json::Value::String(text) => Raw::text(text, false),
                        serde_json::Value::Bool(v) => Raw { text: v.to_string(), kind: BOOLEAN, null: false },
                        serde_json::Value::Number(v) => Raw::text(v.to_string(), true),
                        _ => return Err(serde::de::Error::custom("nested JSON objects and arrays are not supported")),
                    };
                    values.push((name, raw));
                }
                Ok(FlatObject(values))
            }
        }
        deserializer.deserialize_map(Visitor)
    }
}
struct JsonLines { input: BufReader<File>, line: usize }
impl JsonLines {
    fn record(&mut self) -> Result<Option<FlatObject>> {
        loop {
            let mut bytes = Vec::new();
            loop {
                let buffer = self.input.fill_buf().map_err(error)?;
                if buffer.is_empty() { break; }
                let length = buffer.iter().position(|b| *b == b'\n').map_or(buffer.len(), |n| n + 1);
                if bytes.len() + length > MAX_RECORD { return Err(format!("JSONL line {} exceeds resource limit", self.line + 1)); }
                let end = buffer[length - 1] == b'\n'; bytes.extend_from_slice(&buffer[..length]);
                self.input.consume(length); if end { break; }
            }
            if bytes.is_empty() { return Ok(None); }
            self.line += 1;
            if bytes.iter().all(u8::is_ascii_whitespace) { continue; }
            return serde_json::from_slice(&bytes).map(Some).map_err(|e| format!("JSONL line {}: {e}", self.line));
        }
    }
}

enum Backend { Csv(Csv), Json(JsonLines), Parquet(RowIter<'static>), Glob(Vec<String>, usize) }
pub struct Scan { plan: Plan, file: Option<File>, backend: Backend, projection: Vec<usize> }

fn unique_names(names: impl Iterator<Item = String>) -> Result<Vec<Column>> {
    let mut used = HashSet::new(); let mut columns = Vec::new(); let mut bytes = 0;
    for (index, name) in names.enumerate() {
        let base = if name.is_empty() { format!("column{}", index + 1) } else { name };
        let mut name = base.clone(); let mut suffix = 2;
        while !used.insert(name.to_lowercase()) { name = format!("{base}__{suffix}"); suffix += 1; }
        bytes += name.len();
        if columns.len() >= MAX_COLUMNS || bytes > MAX_SCHEMA || name.contains('\0') { return Err("file schema exceeds resource limit".into()); }
        columns.push(Column { name, kind: NULL, nullable: false });
    }
    Ok(columns)
}

fn validate_options(options: &Options) -> Result<()> {
    if !["csv", "json", "parquet", "glob"].contains(&options.format.as_str()) { return Err("unsupported file format".into()); }
    for value in [&options.delimiter, &options.quote, &options.escape] {
        if value.len() != 1 || !value.is_ascii() || matches!(value.as_bytes()[0], 0 | b'\r' | b'\n') {
            return Err("CSV delimiter, quote and escape must be single ASCII characters".into());
        }
    }
    if options.delimiter == options.quote || options.delimiter == options.escape { return Err("conflicting CSV delimiter and quoting options".into()); }
    Ok(())
}

pub fn prepare(request: &str, root: &str) -> Result<String> {
    if request.len() > MAX_SCHEMA { return Err("file options exceed resource limit".into()); }
    let options: Options = serde_json::from_str(request).map_err(error)?; validate_options(&options)?;
    let source = std::env::current_dir().map_err(error)?.join(&options.path);
    if options.format == "glob" {
        discover(&source, root)?;
        return serde_json::to_string(&Plan { options, source, path: PathBuf::new(), snapshot: None,
            columns: vec![Column { name: "file".into(), kind: VARCHAR, nullable: false }] }).map_err(error);
    }
    let (file, path) = secure_file(&source, root)?;
    let signature = snapshot(file.metadata().map_err(error)?)?;
    let columns = match options.format.as_str() {
        "csv" => {
            let mut reader = csv(file.try_clone().map_err(error)?, &options)?;
            let first = reader.record()?.ok_or("CSV file has no schema")?;
            let mut columns = if options.header { unique_names(first.into_iter().map(|v| v.text))? }
                else { let mut c = unique_names((1..=first.len()).map(|i| format!("column{i}")))?; infer_record(&mut c, &first)?; c };
            while let Some(record) = reader.record()? { infer_record(&mut columns, &record)?; }
            columns
        }
        "json" => {
            let mut reader = JsonLines { input: BufReader::new(file.try_clone().map_err(error)?), line: 0 };
            let mut columns: Vec<Column> = Vec::new(); let mut lookup = HashMap::new(); let mut rows = 0;
            while let Some(record) = reader.record()? {
                let mut present = HashSet::new();
                for (name, raw) in record.0 {
                    let id = if let Some(id) = lookup.get(&name) { *id } else {
                        if columns.len() >= MAX_COLUMNS || columns.iter().map(|c| c.name.len()).sum::<usize>() + name.len() > MAX_SCHEMA {
                            return Err("JSONL schema exceeds resource limit".into());
                        }
                        let id = columns.len(); lookup.insert(name.clone(), id);
                        columns.push(Column { name, kind: NULL, nullable: rows > 0 }); id
                    };
                    present.insert(id); columns[id].kind = merge_type(columns[id].kind, raw.kind);
                    columns[id].nullable |= raw.null;
                }
                for (i, column) in columns.iter_mut().enumerate() { column.nullable |= !present.contains(&i); }
                rows += 1;
            }
            if columns.is_empty() { return Err("JSONL file has no columns".into()); }
            columns
        }
        "parquet" => parquet_schema(&file)?,
        _ => return Err("unsupported file format".into()),
    };
    let plan = Plan { options, source, path, snapshot: Some(signature), columns };
    check_file(&plan, &file)?;
    serde_json::to_string(&plan).map_err(error)
}
fn infer_record(columns: &mut [Column], record: &[Raw]) -> Result<()> {
    if columns.len() != record.len() { return Err(format!("CSV field count mismatch: expected {}, got {}", columns.len(), record.len())); }
    for (column, value) in columns.iter_mut().zip(record) {
        column.kind = merge_type(column.kind, value.kind); column.nullable |= value.null;
    }
    Ok(())
}
fn csv(file: File, options: &Options) -> Result<Csv> {
    let mut input = BufReader::with_capacity(65536, file);
    if input.fill_buf().map_err(error)?.starts_with(&[0xef, 0xbb, 0xbf]) { input.consume(3); }
    Ok(Csv { input, options: options.clone(), row: 0 })
}
fn check_file(plan: &Plan, file: &File) -> Result<()> {
    if plan.source.canonicalize().map_err(error)? != plan.path
        || Some(snapshot(file.metadata().map_err(error)?)?) != plan.snapshot
        || Some(snapshot(std::fs::metadata(&plan.source).map_err(error)?)?) != plan.snapshot {
        return Err("file changed after schema inference".into());
    }
    Ok(())
}
pub fn columns(text: &str) -> Result<Vec<ffi::Column>> {
    Ok(decode_plan(text)?.columns.into_iter().map(|c| ffi::Column { name: c.name, kind: c.kind, nullable: c.nullable }).collect())
}
pub fn open(text: &str, root: &str, projection: &[usize]) -> Result<Scan> {
    let plan = decode_plan(text)?;
    if projection.iter().any(|i| *i >= plan.columns.len()) { return Err("invalid file projection".into()); }
    if plan.options.format == "glob" {
        let paths = discover(&plan.source, root)?;
        return Ok(Scan { plan, file: None, backend: Backend::Glob(paths, 0), projection: projection.into() });
    }
    let (file, _) = secure_file(&plan.source, root)?; check_file(&plan, &file)?;
    let backend = make_backend(&plan, &file, projection)?;
    Ok(Scan { plan, file: Some(file), backend, projection: projection.into() })
}
fn make_backend(plan: &Plan, file: &File, projection: &[usize]) -> Result<Backend> {
    let mut cloned = file.try_clone().map_err(error)?; cloned.seek(SeekFrom::Start(0)).map_err(error)?;
    Ok(match plan.options.format.as_str() {
        "csv" => { let mut reader = csv(cloned, &plan.options)?; if plan.options.header { reader.record()?; } Backend::Csv(reader) }
        "json" => Backend::Json(JsonLines { input: BufReader::new(cloned), line: 0 }),
        "parquet" => {
            validate_footer(&mut cloned)?;
            let reader = SerializedFileReader::new(cloned).map_err(error)?;
            let root = reader.metadata().file_metadata().schema();
            let fields = projection.iter().map(|i| root.get_fields()[*i].clone()).collect::<Vec<_>>();
            // A zero-column projection still needs row counts; read one physical
            // column, then discard it. It never materializes a row group.
            let fields = if fields.is_empty() { vec![root.get_fields()[0].clone()] } else { fields };
            let projected = SchemaType::group_type_builder(root.name()).with_fields(fields).build().map_err(error)?;
            Backend::Parquet(RowIter::from_file_into(Box::new(reader)).project(Some(projected)).map_err(error)?)
        }
        _ => return Err("unsupported file format".into()),
    })
}
impl Scan {
    pub fn rewind(&mut self) -> Result<()> {
        if let Some(file) = &self.file {
            check_file(&self.plan, file)?;
            self.backend = make_backend(&self.plan, file, &self.projection)?;
        } else if let Backend::Glob(_, position) = &mut self.backend { *position = 0; }
        Ok(())
    }
    pub fn next_batch(&mut self, size: usize) -> Result<ffi::Batch> {
        if size == 0 || size > 1024 { return Err("invalid file scan batch size".into()); }
        if let Some(file) = &self.file { check_file(&self.plan, file)?; }
        let mut batch = ffi::Batch { cells: Vec::new(), rows: 0 };
        let mut batch_bytes = 0usize;
        for _ in 0..size {
            let values = match &mut self.backend {
                Backend::Csv(csv) => match csv.record()? {
                    None => break,
                    Some(row) => {
                        if row.len() != self.plan.columns.len() { return Err(format!("CSV record {}: field count mismatch", csv.row)); }
                        self.projection.iter().map(|i| row[*i].clone()).collect::<Vec<_>>()
                    }
                },
                Backend::Json(json) => match json.record()? {
                    None => break,
                    Some(row) => {
                        let mut row = row.0.into_iter().collect::<HashMap<_, _>>();
                        self.projection.iter().map(|i| row.remove(&self.plan.columns[*i].name).unwrap_or_else(Raw::null)).collect()
                    }
                },
                Backend::Parquet(rows) => match rows.next() {
                    None => break,
                    Some(row) => row.map_err(error)?.get_column_iter().take(self.projection.len())
                        .map(|(_, value)| parquet_value(value)).collect::<Result<Vec<_>>>()?,
                },
                Backend::Glob(paths, position) => {
                    let Some(path) = paths.get(*position) else { break; };
                    *position += 1;
                    self.projection.iter().map(|_| Raw::text(path.clone(), false)).collect()
                },
            };
            for (value, index) in values.into_iter().zip(&self.projection) {
                batch_bytes = batch_bytes.checked_add(value.text.len() + std::mem::size_of::<ffi::Cell>())
                    .ok_or("file scan batch exceeds resource limit")?;
                batch.cells.push(typed(value, self.plan.columns[*index].kind)
                    .map_err(|error| format!("column '{}': {error}", self.plan.columns[*index].name))?);
            }
            batch.rows += 1;
            // A record is already bounded independently. Stop between records
            // so wide text rows cannot multiply the batch's memory footprint.
            if batch_bytes >= MAX_RECORD { break; }
        }
        if let Some(file) = &self.file { check_file(&self.plan, file)?; }
        Ok(batch)
    }
}
fn typed(raw: Raw, kind: u8) -> Result<ffi::Cell> {
    let mut cell = ffi::Cell { text: raw.text, integer: 0, number: 0.0, is_null: raw.null };
    if cell.is_null { return Ok(cell); }
    let invalid = || "file value does not match inferred schema".to_string();
    match kind {
        BIGINT => cell.integer = cell.text.parse().map_err(error)?,
        DOUBLE => { cell.number = cell.text.parse().map_err(error)?; if !cell.number.is_finite() { return Err(invalid()); } }
        BOOLEAN => cell.integer = if cell.text.eq_ignore_ascii_case("true") { 1 } else if cell.text.eq_ignore_ascii_case("false") { 0 } else { return Err(invalid()); },
        DATE => cell.integer = date(&cell.text).ok_or_else(invalid)?,
        DATETIME => cell.integer = datetime(&cell.text).or_else(|| date(&cell.text).map(|d| d * 86400_000_000)).ok_or_else(invalid)?,
        VARCHAR => {},
        _ => return Err(invalid()),
    }
    Ok(cell)
}

fn discover(pattern: &Path, root: &str) -> Result<Vec<String>> {
    // Validate the non-wildcard prefix even when there are no matches.
    let mut prefix = PathBuf::new();
    for component in pattern.components() {
        if component.as_os_str().to_string_lossy().contains(['*', '?', '[']) { break; }
        prefix.push(component);
    }
    while !prefix.exists() && prefix.pop() {}
    let root_path = Path::new(root).canonicalize().map_err(error)?;
    if root.is_empty() || !prefix.canonicalize().map_err(error)?.starts_with(&root_path) {
        return Err("glob path is outside secure_file_priv".into());
    }
    let mut paths = Vec::new();
    for path in glob::glob(pattern.to_str().ok_or("non-UTF8 glob path")?).map_err(error)? {
        let path = path.map_err(error)?;
        if path.is_file() {
            secure_file(&path, root)?;
            if paths.len() == MAX_FILES { return Err("glob file count exceeds resource limit".into()); }
            paths.push(path.to_str().ok_or("non-UTF8 file path")?.into());
        }
    }
    paths.sort(); Ok(paths)
}

fn validate_footer(file: &mut File) -> Result<()> {
    let size = file.metadata().map_err(error)?.len();
    if size < 12 { return Err("invalid Parquet footer".into()); }
    file.seek(SeekFrom::End(-8)).map_err(error)?;
    let mut footer = [0; 8]; file.read_exact(&mut footer).map_err(error)?;
    let length = u32::from_le_bytes(footer[..4].try_into().map_err(error)?) as u64;
    if &footer[4..] != b"PAR1" || length > MAX_RECORD as u64 || length + 12 > size {
        return Err("invalid or oversized Parquet footer".into());
    }
    file.seek(SeekFrom::Start(0)).map_err(error)?; Ok(())
}
fn parquet_schema(file: &File) -> Result<Vec<Column>> {
    let mut cloned = file.try_clone().map_err(error)?; validate_footer(&mut cloned)?;
    let reader = SerializedFileReader::new(cloned).map_err(error)?;
    let schema = reader.metadata().file_metadata().schema();
    if schema.get_fields().is_empty() || schema.get_fields().len() > MAX_COLUMNS { return Err("invalid Parquet column count".into()); }
    for group in reader.metadata().row_groups() {
        if group.total_byte_size() < 0 || group.total_byte_size() > MAX_GROUP
            || group.columns().iter().any(|c| c.uncompressed_size() < 0 || c.uncompressed_size() > MAX_GROUP) {
            return Err("Parquet row group exceeds resource limit".into());
        }
    }
    let mut columns = Vec::new(); let mut names = HashSet::new();
    for field in schema.get_fields() {
        if !field.is_primitive() || field.get_basic_info().repetition() == Repetition::REPEATED
            || !names.insert(field.name().to_lowercase()) { return Err("nested, repeated or duplicate Parquet columns are not supported".into()); }
        let kind = match field.get_basic_info().converted_type() {
            ConvertedType::DATE => DATE,
            ConvertedType::TIMESTAMP_MILLIS | ConvertedType::TIMESTAMP_MICROS => DATETIME,
            ConvertedType::UTF8 => VARCHAR,
            ConvertedType::NONE => match field.get_physical_type() {
                PhysicalType::BOOLEAN => BOOLEAN,
                PhysicalType::INT32 | PhysicalType::INT64 => BIGINT,
                PhysicalType::FLOAT | PhysicalType::DOUBLE => DOUBLE,
                PhysicalType::BYTE_ARRAY => VARCHAR,
                _ => return Err("unsupported Parquet physical type".into()),
            },
            _ => return Err("unsupported Parquet logical type".into()),
        };
        columns.push(Column { name: field.name().into(), kind,
            nullable: field.get_basic_info().repetition() != Repetition::REQUIRED });
    }
    Ok(columns)
}
fn parquet_value(value: &Field) -> Result<Raw> {
    let (text, kind) = match value {
        Field::Null => return Ok(Raw::null()),
        Field::Bool(v) => (v.to_string(), BOOLEAN),
        Field::Byte(v) => (v.to_string(), BIGINT), Field::Short(v) => (v.to_string(), BIGINT),
        Field::Int(v) => (v.to_string(), BIGINT), Field::Long(v) => (v.to_string(), BIGINT),
        Field::Float(v) => (v.to_string(), DOUBLE), Field::Double(v) => (v.to_string(), DOUBLE),
        Field::Str(v) => (v.clone(), VARCHAR),
        Field::Bytes(v) => (String::from_utf8(v.data().to_vec()).map_err(error)?, VARCHAR),
        Field::Date(_) => (value.to_string(), DATE),
        Field::TimestampMillis(_) | Field::TimestampMicros(_) => {
            // The record API renders UTC as "... +00:00". SQL DATETIME stores
            // the UTC calendar value without the zone suffix.
            let rendered = value.to_string();
            let calendar = rendered.strip_suffix(" +00:00")
                .or_else(|| rendered.strip_suffix('Z')).unwrap_or(&rendered);
            (calendar.replace('T', " "), DATETIME)
        },
        _ => return Err("unsupported Parquet field value".into()),
    };
    if text.len() > MAX_RECORD { return Err("Parquet value exceeds resource limit".into()); }
    Ok(Raw { text, kind, null: false })
}

#[cfg(test)]
mod tests;
