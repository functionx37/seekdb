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

use super::*;
use std::io::Write;
use tempfile::TempDir;

fn plan(root: &TempDir, name: &str, format: &str, text: &str) -> String {
    let path = root.path().join(name);
    std::fs::write(&path, text).unwrap();
    prepare(&serde_json::json!({"path":path,"format":format}).to_string(), root.path().to_str().unwrap()).unwrap()
}

#[test]
fn csv_types_multiline_nulls_and_rewind() {
    let root = TempDir::new().unwrap();
    let plan = plan(&root, "source.csv", "csv",
        "\u{feff}id,name,score,active,day,created,note,id\r\n1,Alice,98.5,true,2024-02-29,2024-02-29T10:11:12,\"hello,\r\nworld\",101\r\n2,Bob,87,false,,,\"say \"\"hi\"\"\",102\r\n");
    let schema = columns(&plan).unwrap();
    assert_eq!(schema.iter().map(|c| c.kind).collect::<Vec<_>>(), [BIGINT,VARCHAR,DOUBLE,BOOLEAN,DATE,DATETIME,VARCHAR,BIGINT]);
    assert_eq!(schema[7].name, "id__2");
    assert!(schema[4].nullable);
    let mut reader = open(&plan, root.path().to_str().unwrap(), &[0,2,4,5,6]).unwrap();
    let batch = reader.next_batch(8).unwrap();
    assert_eq!(batch.rows, 2); assert_eq!(batch.cells.len(), 10);
    assert_eq!(batch.cells[0].integer, 1); assert_eq!(batch.cells[1].number, 98.5);
    assert_eq!(batch.cells[4].text, "hello,\r\nworld"); assert!(batch.cells[7].is_null);
    assert_eq!(batch.cells[9].text, "say \"hi\"");
    assert_eq!(reader.next_batch(8).unwrap().rows, 0);
    reader.rewind().unwrap(); assert_eq!(reader.next_batch(1).unwrap().cells[0].integer, 1);
}

#[test]
fn jsonl_union_missing_and_numeric_conflicts() {
    let root = TempDir::new().unwrap();
    let plan = plan(&root, "rows.jsonl", "json", "{\"id\":1,\"value\":1,\"day\":\"2026-01-01\"}\n{\"id\":2,\"extra\":true,\"value\":1.5}\n{\"id\":3,\"value\":\"text\"}\n");
    let schema = columns(&plan).unwrap();
    assert_eq!(schema.iter().map(|c| c.name.as_str()).collect::<Vec<_>>(), ["id","value","day","extra"]);
    assert_eq!(schema[1].kind, VARCHAR); assert!(schema[2].nullable && schema[3].nullable);
    let mut reader = open(&plan, root.path().to_str().unwrap(), &[1,3]).unwrap();
    let batch = reader.next_batch(4).unwrap();
    assert_eq!(batch.rows, 3); assert_eq!(batch.cells[0].text, "1"); assert!(batch.cells[1].is_null);
    assert_eq!(batch.cells[3].integer, 1); assert_eq!(batch.cells[4].text, "text");
}

#[test]
fn reject_bad_rows_nested_json_and_duplicates() {
    let root = TempDir::new().unwrap();
    for (format, contents) in [("csv", "a,b\n1\n"), ("csv", "a\n\"unterminated\n"),
        ("csv", "a\n\"x\"garbage\n"), ("json", "{\"x\":{}}\n"),
        ("json", "{\"x\":[]}\n"), ("json", "{\"x\":1,\"x\":2}\n"), ("json", "[]\n")] {
        let path = root.path().join("bad"); std::fs::write(&path, contents).unwrap();
        assert!(prepare(&serde_json::json!({"path":path,"format":format}).to_string(), root.path().to_str().unwrap()).is_err(), "{contents}");
    }
}

#[test]
fn deterministic_boundaries_and_all_null() {
    assert_eq!(infer_type("9223372036854775807"), BIGINT);
    assert_eq!(infer_type("-9223372036854775808"), BIGINT);
    assert_eq!(infer_type("9223372036854775808"), VARCHAR);
    assert_eq!(infer_type("1e400"), VARCHAR);
    assert_eq!(infer_type("NaN"), VARCHAR);
    assert_eq!(infer_type("2024-02-29"), DATE);
    assert_eq!(infer_type("2023-02-29"), VARCHAR);
    assert_eq!(infer_type("2024-01-01T24:00:00"), VARCHAR);
    assert_eq!(datetime("1970-01-01 00:00:00.123456"), Some(123456));
    assert_eq!(date("1970-01-01"), Some(0));
    let root = TempDir::new().unwrap();
    let plan = plan(&root, "null.csv", "csv", "a,b\n,\"\"\n");
    let schema = columns(&plan).unwrap(); assert_eq!(schema[0].kind, NULL); assert_eq!(schema[1].kind, VARCHAR);
}

#[test]
fn detect_changes_before_open_during_scan_and_rescan() {
    let root = TempDir::new().unwrap();
    let plan = plan(&root, "changed.csv", "csv", "a\n1\n2\n");
    let mut reader = open(&plan, root.path().to_str().unwrap(), &[0]).unwrap();
    assert_eq!(reader.next_batch(1).unwrap().rows, 1);
    std::fs::write(root.path().join("changed.csv"), "a\n3\n4\n").unwrap();
    assert!(reader.next_batch(1).is_err()); assert!(reader.rewind().is_err());
    assert!(open(&plan, root.path().to_str().unwrap(), &[0]).is_err());
}

#[cfg(unix)]
#[test]
fn secure_root_boundary_and_symlinks() {
    use std::os::unix::fs::symlink;
    let root = TempDir::new().unwrap(); let outside = TempDir::new().unwrap();
    let outside_file = outside.path().join("outside.csv"); std::fs::write(&outside_file, "a\n1\n").unwrap();
    symlink(&outside_file, root.path().join("link.csv")).unwrap();
    for path in [&outside_file, &root.path().join("link.csv")] {
        assert!(prepare(&serde_json::json!({"path":path,"format":"csv"}).to_string(), root.path().to_str().unwrap()).is_err());
    }
    let request = serde_json::json!({"path":outside_file,"format":"csv"}).to_string();
    assert!(prepare(&request, "").is_err()); assert!(prepare(&request, "NULL").is_err());
}

#[test]
fn glob_is_only_discovery_and_rewinds() {
    let root = TempDir::new().unwrap();
    std::fs::write(root.path().join("a.txt"), "not CSV").unwrap();
    std::fs::write(root.path().join("b.txt"), "not JSON").unwrap();
    let request = serde_json::json!({"path":root.path().join("*.txt"),"format":"glob"}).to_string();
    let plan = prepare(&request, root.path().to_str().unwrap()).unwrap();
    let mut reader = open(&plan, root.path().to_str().unwrap(), &[0]).unwrap();
    assert_eq!(reader.next_batch(8).unwrap().rows, 2);
    reader.rewind().unwrap(); assert_eq!(reader.next_batch(8).unwrap().rows, 2);
}

#[test]
fn parquet_multiple_groups_nulls_projection_and_snappy() {
    use parquet::basic::Compression;
    use parquet::data_type::Int64Type;
    use parquet::file::properties::WriterProperties;
    use parquet::file::writer::SerializedFileWriter;
    use std::sync::Arc;
    let root = TempDir::new().unwrap(); let path = root.path().join("data.parquet");
    let field = SchemaType::primitive_type_builder("id", PhysicalType::INT64)
        .with_repetition(Repetition::OPTIONAL).build().unwrap();
    let schema = SchemaType::group_type_builder("schema").with_fields(vec![Arc::new(field)]).build().unwrap();
    let properties = WriterProperties::builder().set_compression(Compression::SNAPPY).build();
    let mut writer = SerializedFileWriter::new(File::create(&path).unwrap(), Arc::new(schema), Arc::new(properties)).unwrap();
    for i in 0..3 {
        let mut group = writer.next_row_group().unwrap(); let mut column = group.next_column().unwrap().unwrap();
        column.typed::<Int64Type>().write_batch(&[i * 2, i * 2 + 1], Some(&[1,0,1]), None).unwrap();
        column.close().unwrap(); group.close().unwrap();
    }
    writer.close().unwrap();
    let plan = prepare(&serde_json::json!({"path":path,"format":"parquet"}).to_string(), root.path().to_str().unwrap()).unwrap();
    let mut scan = open(&plan, root.path().to_str().unwrap(), &[0]).unwrap();
    let batch = scan.next_batch(12).unwrap(); assert_eq!(batch.rows, 9); assert!(batch.cells[1].is_null);
    assert_eq!(batch.cells[8].integer, 5);
    scan.rewind().unwrap(); assert_eq!(scan.next_batch(1).unwrap().cells[0].integer, 0);
    let mut count_only = open(&plan, root.path().to_str().unwrap(), &[]).unwrap();
    assert_eq!(count_only.next_batch(12).unwrap().rows, 9);
}

#[test]
fn scan_batches_do_not_retain_previous_rows() {
    let root = TempDir::new().unwrap(); let path = root.path().join("large.csv");
    let mut file = File::create(&path).unwrap(); writeln!(file, "id,text").unwrap();
    for i in 0..10000 { writeln!(file, "{i},{}", "x".repeat(1000)).unwrap(); }
    drop(file);
    let plan = prepare(&serde_json::json!({"path":path,"format":"csv"}).to_string(), root.path().to_str().unwrap()).unwrap();
    let mut scan = open(&plan, root.path().to_str().unwrap(), &[0]).unwrap();
    let mut rows = 0;
    loop { let batch = scan.next_batch(64).unwrap(); if batch.rows == 0 { break; } assert!(batch.cells.len() <= 64); rows += batch.rows; }
    assert_eq!(rows, 10000);
}

#[test]
fn parquet_primitives_dates_timestamps_and_reordered_projection() {
    use parquet::data_type::{BoolType, ByteArray, ByteArrayType, DoubleType, Int32Type, Int64Type};
    use parquet::file::properties::WriterProperties;
    use parquet::file::writer::SerializedFileWriter;
    use parquet::schema::parser::parse_message_type;
    use std::sync::Arc;
    let root = TempDir::new().unwrap();
    let path = root.path().join("types.parquet");
    let schema = parse_message_type("message test {
        REQUIRED BOOLEAN active;
        REQUIRED DOUBLE score;
        OPTIONAL BINARY name (UTF8);
        REQUIRED INT32 day (DATE);
        REQUIRED INT64 created (TIMESTAMP_MICROS);
    }").unwrap();
    let mut writer = SerializedFileWriter::new(File::create(&path).unwrap(), Arc::new(schema),
        Arc::new(WriterProperties::builder().build())).unwrap();
    let mut group = writer.next_row_group().unwrap();
    let mut column = group.next_column().unwrap().unwrap();
    column.typed::<BoolType>().write_batch(&[true, false], None, None).unwrap();
    column.close().unwrap();
    let mut column = group.next_column().unwrap().unwrap();
    column.typed::<DoubleType>().write_batch(&[1.5, -2.0], None, None).unwrap();
    column.close().unwrap();
    let mut column = group.next_column().unwrap().unwrap();
    column.typed::<ByteArrayType>().write_batch(&[ByteArray::from("你好")], Some(&[1, 0]), None).unwrap();
    column.close().unwrap();
    let mut column = group.next_column().unwrap().unwrap();
    column.typed::<Int32Type>().write_batch(&[0, 1], None, None).unwrap();
    column.close().unwrap();
    let mut column = group.next_column().unwrap().unwrap();
    column.typed::<Int64Type>().write_batch(&[-1, 1_234_567], None, None).unwrap();
    column.close().unwrap();
    group.close().unwrap();
    writer.close().unwrap();
    let plan = prepare(&serde_json::json!({"path":path,"format":"parquet"}).to_string(),
        root.path().to_str().unwrap()).unwrap();
    assert_eq!(columns(&plan).unwrap().iter().map(|c| c.kind).collect::<Vec<_>>(),
        [BOOLEAN, DOUBLE, VARCHAR, DATE, DATETIME]);
    let mut scan = open(&plan, root.path().to_str().unwrap(), &[4, 2, 0, 3, 1]).unwrap();
    let batch = scan.next_batch(4).unwrap();
    assert_eq!(batch.rows, 2);
    assert_eq!(batch.cells[0].integer, -1);
    assert_eq!(batch.cells[1].text, "你好");
    assert_eq!(batch.cells[2].integer, 1);
    assert_eq!(batch.cells[4].number, 1.5);
    assert_eq!(batch.cells[5].integer, 1_234_567);
    assert!(batch.cells[6].is_null);
    assert_eq!(batch.cells[8].integer, 1);
}

#[test]
fn wide_text_batches_have_a_byte_budget() {
    let root = TempDir::new().unwrap();
    let path = root.path().join("wide.csv");
    let mut file = File::create(&path).unwrap();
    writeln!(file, "text").unwrap();
    let value = "x".repeat(1024 * 1024);
    for _ in 0..20 { writeln!(file, "{value}").unwrap(); }
    drop(file);
    let plan = prepare(&serde_json::json!({"path":path,"format":"csv"}).to_string(), root.path().to_str().unwrap()).unwrap();
    let mut scan = open(&plan, root.path().to_str().unwrap(), &[0]).unwrap();
    let first = scan.next_batch(1024).unwrap();
    assert!(first.rows <= 16);
    let consumed = first.rows;
    drop(first);
    assert_eq!(scan.next_batch(1024).unwrap().rows, 20 - consumed);
}
