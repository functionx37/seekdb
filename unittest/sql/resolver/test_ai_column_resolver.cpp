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
#include "sql/resolver/ddl/ob_ddl_resolver.h"

namespace oceanbase::sql
{
using namespace common;
using share::schema::ObColumnSchemaV2;

class ColumnResolver final : public ObDDLResolver
{
public:
  explicit ColumnResolver(ObResolverParams &params) : ObDDLResolver(params) {}
  int resolve(const ParseNode &) override { return OB_NOT_SUPPORTED; }
  using ObDDLResolver::resolve_ai_column_attribute;
};

static const ParseNode *find_definition(const ParseNode *node)
{
  if (node == nullptr || node->type_ == T_AI_COLUMN) { return node; }
  for (int64_t i = 0; i < node->num_child_; ++i) {
    if (const ParseNode *found = find_definition(node->children_[i])) { return found; }
  }
  return nullptr;
}

static int resolve_options(const char *text, ObColumnSchemaV2 &column)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result;
  ObResolverParams params;
  params.allocator_ = &allocator;
  ColumnResolver resolver(params);
  int ret = parser.parse(ObString(text), result);
  if (OB_SUCC(ret)) {
    const ParseNode *definition = find_definition(result.result_tree_);
    ret = definition == nullptr ? OB_ERR_UNEXPECTED : resolver.resolve_ai_column_attribute(column, *definition);
  }
  return ret;
}

TEST(AIColumnResolver, DefaultAsyncAndExplicitRetryOptions)
{
  ObColumnSchemaV2 column;
  ASSERT_EQ(OB_SUCCESS, resolve_options(
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m', content)))", column));
  EXPECT_TRUE(column.is_ai_column());
  EXPECT_EQ(2, column.get_ai_max_retries());
  // The parser arena has already been destroyed: schema strings must own their data.
  EXPECT_EQ(ObString("AI_COMPLETE('m', content)"), column.get_ai_expression());
  EXPECT_FALSE(column.is_generated_column());
  column.reset();
  ASSERT_EQ(OB_SUCCESS, resolve_options(
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m', content)) WITH(SYNC_MODE='ASYNC',MAX_RETRIES=0))", column));
  EXPECT_EQ(0, column.get_ai_max_retries());
}

TEST(AIColumnResolver, RejectInvalidOrRepeatedOptions)
{
  for (const char *text : {
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m', content)) WITH(max_retries=-1))",
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m', content)) WITH(max_retries=1.5))",
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m', content)) WITH(max_retries=true))",
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m', content)) WITH(max_retries=1,max_retries=2))",
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m', content)) WITH(sync_mode='async',sync_mode='async'))",
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m', content)) WITH(unknown_option=1))"}) {
    ObColumnSchemaV2 column;
    EXPECT_NE(OB_SUCCESS, resolve_options(text, column)) << text;
  }
  ObColumnSchemaV2 column;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve_options(
      "CREATE TABLE t(c TEXT AI COLUMN(AI_COMPLETE('m', content)) WITH(sync_mode='sync'))", column));
}
} // namespace oceanbase::sql

int main(int argc, char **argv)
{
  oceanbase::common::ObCharset::init_charset();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
