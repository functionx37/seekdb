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
#include <vector>
#include "share/schema/ob_column_schema.h"

namespace oceanbase::share::schema
{
using namespace common;

static void initialize(ObColumnSchemaV2 &column)
{
  column.set_table_id(50001);
  column.set_column_id(20);
  column.set_data_type(ObJsonType);
  column.add_column_flag(AI_COLUMN_FLAG);
  ASSERT_EQ(OB_SUCCESS, column.set_column_name("tags"));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_expression("AI_COMPLETE('model', content)"));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_normalized_expr(
      "AI_COMPLETE('model', CONCAT(content, 'Return exactly one valid JSON value'))"));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_dependency_ids("16,17"));
  column.set_ai_max_retries(3);
  column.set_ai_definition_version(8);
  column.set_ai_state_column_id(21);
  column.set_ai_request_column_id(22);
  column.set_ai_result_spec_column_id(23);
}

TEST(AIColumnSchema, DeepCopyAndSerialization)
{
  ObColumnSchemaV2 original;
  initialize(original);
  ObColumnSchemaV2 copy;
  ASSERT_EQ(OB_SUCCESS, copy.assign(original));
  ASSERT_NE(copy.get_ai_expression().ptr(), original.get_ai_expression().ptr());
  ASSERT_EQ(OB_SUCCESS, original.set_ai_expression("changed"));
  EXPECT_EQ(ObString("AI_COMPLETE('model', content)"), copy.get_ai_expression());
  std::vector<char> bytes(copy.get_serialize_size());
  int64_t position = 0;
  ASSERT_EQ(OB_SUCCESS, copy.serialize(bytes.data(), bytes.size(), position));
  EXPECT_EQ(bytes.size(), position);
  ObColumnSchemaV2 restored;
  position = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize(bytes.data(), bytes.size(), position));
  ASSERT_TRUE(restored.is_ai_column());
  EXPECT_EQ(copy.get_ai_expression(), restored.get_ai_expression());
  EXPECT_EQ(copy.get_ai_normalized_expr(), restored.get_ai_normalized_expr());
  EXPECT_EQ(copy.get_ai_dependency_ids(), restored.get_ai_dependency_ids());
  EXPECT_EQ(3, restored.get_ai_max_retries());
  EXPECT_EQ(8, restored.get_ai_definition_version());
  EXPECT_EQ(21, restored.get_ai_state_column_id());
  EXPECT_EQ(22, restored.get_ai_request_column_id());
  EXPECT_EQ(23, restored.get_ai_result_spec_column_id());
  std::fill(bytes.begin(), bytes.end(), '\0');
  EXPECT_EQ(copy.get_ai_normalized_expr(), restored.get_ai_normalized_expr());
  restored.reset();
  EXPECT_FALSE(restored.is_ai_column());
  EXPECT_TRUE(restored.get_ai_expression().empty());
}

TEST(AIColumnSchema, RemapsDependenciesAndHiddenColumnIDs)
{
  ObColumnSchemaV2 column;
  initialize(column);
  hash::ObHashMap<uint64_t, uint64_t> mapping;
  ASSERT_EQ(OB_SUCCESS, mapping.create(16, "AIColumnTest"));
  for (uint64_t id : {16, 17, 20, 21, 22, 23}) {
    ASSERT_EQ(OB_SUCCESS, mapping.set_refactored(id, id + 100));
  }
  ASSERT_EQ(OB_SUCCESS, column.convert_column_id(mapping));
  EXPECT_EQ(120, column.get_column_id());
  EXPECT_EQ(121, column.get_ai_state_column_id());
  EXPECT_EQ(122, column.get_ai_request_column_id());
  EXPECT_EQ(123, column.get_ai_result_spec_column_id());
  EXPECT_EQ(ObString("116,117"), column.get_ai_dependency_ids());
}

TEST(AIColumnSchema, DependencyIDsRejectTruncationAndOverflow)
{
  ObColumnSchemaV2 column;
  ObSEArray<uint64_t, 4> dependencies;
  for (const char *invalid : {"16,", ",16", "16,,17", "-1", "1x", "18446744073709551616"}) {
    ASSERT_EQ(OB_SUCCESS, column.set_ai_dependency_ids(ObString(invalid)));
    EXPECT_EQ(OB_INVALID_ARGUMENT, column.get_ai_dependencies(dependencies)) << invalid;
  }
  ASSERT_EQ(OB_SUCCESS, column.set_ai_dependency_ids(""));
  ASSERT_EQ(OB_SUCCESS, column.get_ai_dependencies(dependencies));
  EXPECT_EQ(0, dependencies.count());
}
} // namespace oceanbase::share::schema

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
