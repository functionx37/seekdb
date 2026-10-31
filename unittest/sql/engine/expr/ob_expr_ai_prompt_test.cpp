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
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace common
{
static int expand(ObIAllocator &allocator, const char *json, ObString &output)
{
  ObString text(json);
  ObJsonObject *object = nullptr;
  int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator, text, object);
  if (OB_SUCC(ret)) {
    ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, object, output);
  }
  return ret;
}

TEST(AIPrompt, RepeatedUnicodeArgumentsAndEmbeddedNul)
{
  ObArenaAllocator allocator;
  ObString output;
  ASSERT_EQ(OB_SUCCESS, expand(allocator,
      R"({"template":"{0}/{0}/{0}","args":["你好\u0000"]})", output));
  const std::string value("你好\0", 7);
  const std::string expected = value + "/" + value + "/" + value;
  EXPECT_EQ(expected, std::string(output.ptr(), output.length()));
}

TEST(AIPrompt, JSONAndNestedPrompt)
{
  ObArenaAllocator allocator;
  ObString output;
  ASSERT_EQ(OB_SUCCESS, expand(allocator,
      R"({"template":"{0}: {1}","args":[{"template":"Hi {0}","args":["世界"]},[1,true,null]]})", output));
  EXPECT_EQ("Hi 世界: [1, true, null]", std::string(output.ptr(), output.length()));
  ASSERT_EQ(OB_SUCCESS, expand(allocator,
      R"({"template":"{0}","args":[{"a":1}]})", output));
  EXPECT_EQ("{\"a\": 1}", std::string(output.ptr(), output.length()));
}

TEST(AIPrompt, InvalidIndexesAndLiteralBraces)
{
  ObArenaAllocator allocator;
  ObString output;
  for (const char *json : {
      R"({"template":"{1}","args":["x"]})",
      R"({"template":"{9223372036854775808}","args":["x"]})",
      R"({"template":"{999999999999999999999999999999999999999}","args":[]})",
      R"({"template":[],"args":[]})"}) {
    EXPECT_NE(OB_SUCCESS, expand(allocator, json, output));
  }
  ASSERT_EQ(OB_SUCCESS, expand(allocator,
      R"({"template":"{name} {} {0}","args":["{1}"]})", output));
  EXPECT_EQ("{name} {} {1}", std::string(output.ptr(), output.length()));
}

TEST(AIPrompt, RepeatedLongArguments)
{
  ObArenaAllocator allocator;
  std::string value(100000, 'x');
  std::string json = "{\"template\":\"{0}{0}{0}{0}\",\"args\":[\"" + value + "\"]}";
  ObString output;
  ASSERT_EQ(OB_SUCCESS, expand(allocator, json.c_str(), output));
  EXPECT_EQ(value + value + value + value, std::string(output.ptr(), output.length()));
}
} // namespace common
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
