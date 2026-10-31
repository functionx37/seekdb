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
class AIProtocolTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  ObJsonObject *object(const char *json)
  {
    ObJsonObject *result = nullptr;
    ObString text(json);
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, text, result));
    return result;
  }
};

TEST_F(AIProtocolTest, EmbeddingsRestoreInputOrder)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_, object(
      R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})"), result));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, result, 2, 2));
  ObJsonArray *vectors = static_cast<ObJsonArray *>(result);
  EXPECT_EQ(1, static_cast<ObJsonArray *>(vectors->get_value(0))->get_value(0)->get_int());
  EXPECT_EQ(3, static_cast<ObJsonArray *>(vectors->get_value(1))->get_value(0)->get_int());
}

TEST_F(AIProtocolTest, RejectMalformedEmbeddingResponses)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *json : {
      R"({"data":{}})", R"({"data":[null]})",
      R"({"data":[{"index":-1,"embedding":[1]}]})",
      R"({"data":[{"index":1,"embedding":[1]}]})",
      R"({"data":[{"index":0,"embedding":"bad"}]})",
      R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})"}) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, object(json), result)) << json;
  }
  for (const char *json : {R"({"vectors":[[]]})", R"({"vectors":[[1,"2"]]})",
                          R"({"vectors":[[true]]})", R"({"vectors":[[null]]})",
                          R"({"vectors":[[1],[2,3]]})"}) {
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(
        allocator_, object(json)->get_value("vectors"), 1, 0)) << json;
  }
  ObJsonNode *valid = object(R"({"vectors":[[1,2],[3,4]]})")->get_value("vectors");
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, valid, 2, 0));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, valid, 2, 3));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_,
      object(R"({"vectors":[[1],[2,3]]})")->get_value("vectors"), 2, 0));
}

TEST_F(AIProtocolTest, CompletionRejectsEmptyAndNonStringContent)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  for (const char *json : {R"({})", R"({"choices":[]})",
      R"({"choices":[{"message":{"content":null}}]})",
      R"({"choices":[{"message":{"content":""}}]})",
      R"({"choices":[{"message":{"content":{}}}]})"}) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, object(json), result));
  }
}

TEST_F(AIProtocolTest, OllamaProviderAndNativeProtocol)
{
  ObAIFuncIComplete *provider = nullptr;
  ObAIFuncIEmbed *embed = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, ObString("oLlAmA"), provider));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator_, ObString("ollama"), embed));
  ObArray<ObString> headers;
  ObString empty_key;
  ASSERT_EQ(OB_SUCCESS, provider->get_header(allocator_, empty_key, headers));
  ASSERT_EQ(1, headers.count());
  ObString model("local-model"), system("system instruction"), input("你好");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator_, model, system, input, nullptr, body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  EXPECT_EQ(input, static_cast<ObJsonString *>(body->get_value("prompt"))->get_str());
  EXPECT_EQ(system, static_cast<ObJsonString *>(body->get_value("system"))->get_str());
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_SUCCESS, provider->parse_output(allocator_, object(R"({"response":"answer"})"), result));
  EXPECT_NE(OB_SUCCESS, provider->parse_output(allocator_, object(R"({"response":null})"), result));
  EXPECT_EQ(OB_SUCCESS, embed->parse_output(allocator_, object(R"({"embeddings":[[1,2]]})"), result));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, result, 1, 2));
}

TEST_F(AIProtocolTest, RerankStableSortingAndResponseValidation)
{
  ObJsonArray *sorted = nullptr;
  ObIJsonBase *result = object(R"({"results":[
    {"index":2,"relevance_score":0.5},
    {"index":0,"relevance_score":0.5},
    {"index":1,"relevance_score":1}]})")->get_value("results");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::normalize_rerank(allocator_, result, 3, sorted));
  EXPECT_EQ(1, static_cast<ObJsonObject *>(sorted->get_value(0))->get_value("index")->get_int());
  EXPECT_EQ(0, static_cast<ObJsonObject *>(sorted->get_value(1))->get_value("index")->get_int());
  EXPECT_EQ(2, static_cast<ObJsonObject *>(sorted->get_value(2))->get_value("index")->get_int());
  for (const char *json : {R"({"results":[]})",
      R"({"results":[{"index":0,"relevance_score":"0.5"}]})",
      R"({"results":[{"index":2,"relevance_score":0.5}]})",
      R"({"results":[{"index":0,"relevance_score":0.5},{"index":0,"relevance_score":1}]})"}) {
    result = object(json)->get_value("results");
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::normalize_rerank(allocator_, result, 1, sorted));
  }
  result = object(R"({"results":[{"index":0,"relevance_score":0.5},
    {"index":0,"relevance_score":1}]})")->get_value("results");
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::normalize_rerank(allocator_, result, 2, sorted));
}
} // namespace common
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
