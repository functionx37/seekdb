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

use crate::reader::{self, Scan};
use crate::export::{self, Export};

#[cxx::bridge(namespace = "oceanbase::sql::file")]
pub mod ffi {
    struct Column {
        name: String,
        kind: u8,
        nullable: bool,
    }
    struct Cell {
        text: String,
        integer: i64,
        number: f64,
        is_null: bool,
    }
    struct Batch {
        cells: Vec<Cell>,
        rows: usize,
    }
    extern "Rust" {
        type Scan;
        fn prepare_scan(options: &str, secure_root: &str) -> Result<String>;
        fn scan_columns(plan: &str) -> Result<Vec<Column>>;
        fn open_scan(plan: &str, secure_root: &str, projection: &[usize]) -> Result<Box<Scan>>;
        fn next_batch(self: &mut Scan, size: usize) -> Result<Batch>;
        fn rewind(self: &mut Scan) -> Result<()>;
        type Export;
        fn open_export(options: &str, secure_root: &str) -> Result<Box<Export>>;
        fn write_row(self: &mut Export, cells: &[Cell]) -> Result<()>;
        fn finish(self: &mut Export) -> Result<()>;
    }
}

fn prepare_scan(options: &str, secure_root: &str) -> Result<String, String> {
    reader::prepare(options, secure_root)
}
fn scan_columns(plan: &str) -> Result<Vec<ffi::Column>, String> {
    reader::columns(plan)
}
fn open_scan(plan: &str, secure_root: &str, projection: &[usize]) -> Result<Box<Scan>, String> {
    reader::open(plan, secure_root, projection).map(Box::new)
}

fn open_export(options: &str, secure_root: &str) -> Result<Box<Export>, String> {
    export::open(options, secure_root)
}
