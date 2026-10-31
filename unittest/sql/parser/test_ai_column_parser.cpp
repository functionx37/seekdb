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
#include "sql/parser/ob_parser.h"

namespace oceanbase::sql
{
using namespace common;

static const ParseNode *find_ai_column(const ParseNode *node)
{
  if (node != nullptr && node->type_ != T_AI_COLUMN) {
    for (int64_t i = 0; i < node->num_child_; ++i) {
      const ParseNode *found = find_ai_column(node->children_[i]);
      if (found != nullptr) { return found; }
    }
    return nullptr;
  }
  return node;
}

TEST(AIColumnParser, CreateAddModifyAndShow)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  for (const char *sql : {
      "CREATE TABLE docs(id BIGINT PRIMARY KEY, content TEXT, tags JSON AI COLUMN(AI_COMPLETE('model', content)) WITH(sync_mode='async',max_retries=2))",
      "CREATE TABLE docs(content TEXT, emb VECTOR(4) AI COLUMN(AI_EMBED('model', content, 4)))",
      "ALTER TABLE docs ADD COLUMN tags JSON AI COLUMN(AI_COMPLETE('model', CONCAT('Classify: ', content))) WITH(max_retries=0)",
      "ALTER TABLE docs MODIFY tags JSON AI COLUMN(AI_COMPLETE('model',content)) WITH(sync_mode='async')"}) {
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(sql), result)) << sql;
    const ParseNode *definition = find_ai_column(result.result_tree_);
    ASSERT_NE(nullptr, definition) << sql;
    ASSERT_EQ(2, definition->num_child_);
    ASSERT_NE(nullptr, definition->children_[0]);
    EXPECT_GT(definition->children_[0]->str_len_, 0);
  }
  for (const char *sql : {"SHOW AI COLUMNS FROM docs", "SHOW AI COLUMNS IN db.docs",
                         "SHOW AI COLUMNS FROM docs FROM db LIKE 'tag%'"}) {
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(sql), result)) << sql;
    const ParseNode *show = result.result_tree_->children_[0];
    ASSERT_EQ(T_SHOW_COLUMNS, show->type_);
    EXPECT_EQ(4, show->children_[0]->value_);
  }
}

TEST(AIColumnParser, RejectMalformedDefinitionAndPreserveOrdinarySQL)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  for (const char *sql : {"CREATE TABLE t(c TEXT AI COLUMN())",
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m','x')) WITH())",
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m','x')) WITH(max_retries=))"}) {
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString(sql), result)) << sql;
  }
  for (const char *sql : {"CREATE TABLE t(ai INT, content TEXT)", "SELECT ai FROM t",
                         "SHOW COLUMNS FROM docs", "CREATE TABLE t(c INT AS (1+1) STORED)"}) {
    ParseResult result;
    EXPECT_EQ(OB_SUCCESS, parser.parse(ObString(sql), result)) << sql;
    EXPECT_EQ(nullptr, find_ai_column(result.result_tree_));
  }
}

TEST(AIColumnParser, RetryIsTransactionalDMLAndKeepsQuotedStatus)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  for (const char *sql : {"ALTER TABLE docs RETRY AI COLUMN tags WHERE status='FAILED'",
      "ALTER TABLE db.docs RETRY AI COLUMN tags WHERE id=1",
      "ALTER TABLE docs RETRY AI COLUMN tags WHERE `status`='business'",
      "ALTER TABLE docs RETRY AI COLUMN tags"}) {
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(sql), result)) << sql;
    const auto *update = result.result_tree_->children_[0];
    ASSERT_EQ(T_UPDATE, update->type_);
    ASSERT_EQ(T_AI_COLUMN, update->children_[2]->type_);
    const auto *column = update->children_[2]->children_[0];
    ASSERT_EQ(ObString("tags"), ObString(column->str_len_, column->str_value_));
  }
  for (const char *sql : {"ALTER TABLE docs RETRY AI COLUMN",
      "ALTER TABLE docs RETRY AI COLUMN tags WHERE", "ALTER TABLE docs RETRY AI COLUMN tags,emb"}) {
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString(sql), result)) << sql;
  }
  ParseResult result;
  EXPECT_EQ(OB_SUCCESS, parser.parse(ObString("SELECT retry FROM docs"), result));
}
} // namespace oceanbase::sql

int main(int argc, char **argv)
{
  oceanbase::common::ObCharset::init_charset();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
