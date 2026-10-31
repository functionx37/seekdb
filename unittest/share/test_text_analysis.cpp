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
#include "share/text_analysis/ob_text_analyzer.h"

namespace oceanbase {
namespace share {
using namespace common;
TEST(TextAnalysis, EnglishNormalizationMatchesCharsetConversion)
{
  for (ObCollationType collation : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
      CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_BINARY}) {
    const ObCharsetInfo *charset = ObCharset::get_charset(collation);
    ASSERT_NE(nullptr, charset);
    ObArenaAllocator allocator;
    share::ObTextAnalysisCtx ctx;
    ctx.cs_ = charset;
    ctx.filter_stopword_ = false;
    share::ObEnglishTextAnalyzer analyzer;
    ASSERT_EQ(OB_SUCCESS, analyzer.init(ctx, allocator));
    for (const char *text : {"database", "SQL", "MySQL", "AbC123", "I", "AéCOLEZ", "Abc中文Def"}) {
      ObDatum doc;
      doc.set_string(ObString(text));
      share::ObITokenStream *stream = nullptr;
      ASSERT_EQ(OB_SUCCESS, analyzer.analyze(doc, stream));
      ObDatum result;
      int64_t frequency = 0;
      ASSERT_EQ(OB_SUCCESS, stream->get_next(result, frequency));
      ObString expected;
      ASSERT_EQ(OB_SUCCESS, ObCharset::tolower(charset, ObString(text), expected, allocator));
      EXPECT_EQ(expected, result.get_string()) << text << " / " << collation;
      EXPECT_EQ(1, frequency);
      EXPECT_EQ(OB_ITER_END, stream->get_next(result, frequency));
    }
  }
}
} // namespace share
} // namespace oceanbase
int main(int argc, char **argv) { oceanbase::common::ObCharset::init_charset(); ::testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
