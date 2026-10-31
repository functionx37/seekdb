/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <string>
#include "sql/parser/ob_parser.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

TEST(FileSQLParser, SourcesAliasesOptionsAndOrdinarySQL)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  for (const char *text : {
      "SELECT * FROM read_csv('/tmp/a.csv')",
      "SELECT id FROM read_csv('/tmp/a.csv', header=true, sample_size=-1) AS f",
      "SELECT * FROM read_json('/tmp/a.jsonl', format='newline_delimited') f",
      "SELECT * FROM read_ndjson('/tmp/a.jsonl')",
      "SELECT * FROM read_parquet('/tmp/a.parquet')",
      "SELECT * FROM glob('/tmp/*.csv') ORDER BY file",
      "SELECT a.id,b.name FROM read_csv('/tmp/a.csv') a LEFT JOIN read_ndjson('/tmp/b.jsonl') b ON a.id=b.id",
      "SELECT * FROM read_csv('/tmp/a.csv') UNION ALL SELECT * FROM read_csv('/tmp/b.csv')",
      "SELECT * FROM read_csv('/tmp/a.csv') FULL JOIN t ON t.id=1",
      "SELECT read_csv,read_json,glob FROM t",
      "CREATE TABLE read_csv(id INT)",
      "SELECT * FROM t WHERE id=1 ORDER BY id LIMIT 2"}) {
    ParseResult result;
    EXPECT_EQ(OB_SUCCESS, parser.parse(ObString(text), result)) << text;
  }
}

TEST(FileSQLParser, FileArgumentsAreFlatExpressions)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(
      "SELECT * FROM read_csv('/tmp/a.csv',header=true,sample_size=-1)"), result));
  auto find_reader = [](auto &&self, const ParseNode *node) -> const ParseNode * {
    if (node == nullptr || node->type_ == T_FILE_TABLE_EXPRESSION) { return node; }
    for (int64_t i = 0; i < node->num_child_; ++i) {
      const auto *reader = self(self, node->children_[i]);
      if (reader != nullptr) { return reader; }
    }
    return nullptr;
  };
  const auto *reader = find_reader(find_reader, result.result_tree_);
  ASSERT_NE(nullptr, reader);
  ASSERT_TRUE(reader->is_tree_not_param_);
  const auto *arguments = reader->children_[1];
  ASSERT_EQ(T_EXPR_LIST, arguments->type_);
  ASSERT_EQ(3, arguments->num_child_);
  EXPECT_EQ(T_VARCHAR, arguments->children_[0]->type_);
  EXPECT_EQ(T_OP_EQ, arguments->children_[1]->type_);
  EXPECT_EQ(T_OP_EQ, arguments->children_[2]->type_);
}

TEST(FileSQLParser, CopyAndDescribeMarkers)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  for (const char *text : {
      "COPY docs TO '/tmp/out.csv'",
      "COPY db.docs TO '/tmp/out.csv' (FORMAT csv, HEADER, NULLSTR 'NULL')",
      "COPY (SELECT id,name FROM read_csv('/tmp/a.csv') ORDER BY id) TO '/tmp/out.csv' (FORMAT csv, HEADER true, DELIMITER ';')",
      "COPY (SELECT 1 AS x) TO '/tmp/out.csv' (QUOTE='\"', ESCAPE='\"')",
      "COPY (SELECT 1 AS x UNION ALL SELECT 2 ORDER BY x) TO '/tmp/out.csv' (HEADER)"}) {
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(text), result)) << text;
    ASSERT_NE(nullptr, result.result_tree_);
    const ParseNode *query = result.result_tree_->children_[0];
    ASSERT_EQ(T_SELECT, query->type_);
    ASSERT_NE(nullptr, query->children_[PARSE_SELECT_INTO_EXTRA]);
    EXPECT_EQ(T_INTO_FILE_COPY, query->children_[PARSE_SELECT_INTO_EXTRA]->type_);
    EXPECT_TRUE(query->children_[PARSE_SELECT_INTO_EXTRA]->is_tree_not_param_);
  }
  for (const char *prefix : {"DESCRIBE", "DESC", "EXPLAIN"}) {
    std::string text = std::string(prefix) + " SELECT * FROM read_csv('/tmp/a.csv')";
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(text.c_str()), result)) << text;
    ASSERT_EQ(T_EXPLAIN, result.result_tree_->children_[0]->type_);
    const ParseNode *format = result.result_tree_->children_[0]->children_[0];
    if (std::string(prefix) == "EXPLAIN") {
      EXPECT_EQ(nullptr, format);
    } else {
      ASSERT_NE(nullptr, format);
      EXPECT_EQ(T_INT, format->type_);
      EXPECT_EQ(1, format->value_);
    }
  }
}

TEST(FileSQLParser, RejectMalformedSourceAndCopy)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  for (const char *text : {"SELECT * FROM read_csv()", "SELECT * FROM read_csv('/tmp/a.csv',)",
      "COPY docs '/tmp/out.csv'", "COPY (SELECT 1 INTO @x) TO '/tmp/out.csv'",
      "COPY docs TO '/tmp/out.csv' (HEADER,, FORMAT csv)"}) {
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString(text), result)) << text;
  }
}
} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  oceanbase::common::ObCharset::init_charset();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
