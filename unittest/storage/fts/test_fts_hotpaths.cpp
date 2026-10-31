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
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/dict/ob_ft_range_dict.h"

namespace oceanbase
{
namespace storage
{
using namespace common;
using CharType = ObFTCharUtil::CharType;

class RangeDictTestAccess
{
public:
  static int add(ObFTRangeDict &dictionary, const char *first, const char *last, ObIFTDict &target)
  {
    ObFTRangeDict::ObFTRange range;
    int ret = range.start_.set_word(first, STRLEN(first));
    if (OB_SUCC(ret)) { ret = range.end_.set_word(last, STRLEN(last)); }
    range.dict_ = &target;
    if (OB_SUCC(ret)) { ret = dictionary.range_dicts_.push_back(range); }
    return ret;
  }
  static int find(const ObFTRangeDict &dictionary, const char *word, ObIFTDict *&target)
  {
    return dictionary.find_first_char_range(ObString(word), target);
  }
};

TEST(FTSHotpaths, DictionaryRangeBoundariesAndGaps)
{
  ObArenaAllocator allocator;
  ObFTDictDesc desc("test", ObFTDictType::DICT_IK_MAIN, CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
  ObFTRangeDict dictionary(allocator, nullptr, desc);
  ObFTRangeDict first(allocator, nullptr, desc);
  ObFTRangeDict second(allocator, nullptr, desc);
  ObFTRangeDict third(allocator, nullptr, desc);
  ObIFTDict *target = nullptr;
  EXPECT_EQ(OB_ENTRY_NOT_EXIST, RangeDictTestAccess::find(dictionary, "a", target));
  ASSERT_EQ(OB_SUCCESS, RangeDictTestAccess::add(dictionary, "a", "d", first));
  ASSERT_EQ(OB_SUCCESS, RangeDictTestAccess::add(dictionary, "f", "h", second));
  ASSERT_EQ(OB_SUCCESS, RangeDictTestAccess::add(dictionary, "中", "文", third));
  for (const char *word : {"a", "b", "d"}) {
    ASSERT_EQ(OB_SUCCESS, RangeDictTestAccess::find(dictionary, word, target));
    EXPECT_EQ(&first, target);
  }
  for (const char *word : {"f", "g", "h"}) {
    ASSERT_EQ(OB_SUCCESS, RangeDictTestAccess::find(dictionary, word, target));
    EXPECT_EQ(&second, target);
  }
  for (const char *word : {"中", "我", "文"}) {
    ASSERT_EQ(OB_SUCCESS, RangeDictTestAccess::find(dictionary, word, target));
    EXPECT_EQ(&third, target);
  }
  for (const char *word : {"", "A", "e", "i", "汉"}) {
    EXPECT_EQ(OB_ENTRY_NOT_EXIST, RangeDictTestAccess::find(dictionary, word, target));
  }
}

TEST(FTSHotpaths, UnicodeClassificationBoundaries)
{
  struct Case { const char *text_; uint8_t length_; CharType expected_; };
  const Case cases[] = {
    {"a", 1, CharType::ENGLISH_LETTER}, {"Z", 1, CharType::ENGLISH_LETTER},
    {"0", 1, CharType::ARABIC_LETTER}, {"９", 3, CharType::ARABIC_LETTER},
    {"Ａ", 3, CharType::ENGLISH_LETTER}, {"ｚ", 3, CharType::ENGLISH_LETTER},
    {"中", 3, CharType::CHINESE}, {"\xE3\x90\x80", 3, CharType::CHINESE},
    {"あ", 3, CharType::OTHER_CJK}, {"한", 3, CharType::OTHER_CJK},
    {"，", 3, CharType::OTHER_CJK}, {"😀", 4, CharType::USELESS},
    {"é", 2, CharType::USELESS}, {"_", 1, CharType::USELESS},
  };
  for (const auto &test : cases) {
    CharType actual;
    ASSERT_EQ(OB_SUCCESS, ObFTCharUtil::classify_first_char(
        CS_TYPE_UTF8MB4_BIN, test.text_, test.length_, actual));
    EXPECT_EQ(test.expected_, actual) << test.text_;
  }
  CharType actual;
  EXPECT_NE(OB_SUCCESS, ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, nullptr, 1, actual));
  EXPECT_NE(OB_SUCCESS, ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, "\xE4\xB8", 2, actual));
}

TEST(FTSHotpaths, UTF16EncodingsKeepClassification)
{
  CharType actual;
  for (ObCollationType collation : {CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN}) {
    if (ObCharset::charset_type_by_coll(collation) == CHARSET_INVALID) {
      // Lite builds do not register UTF-16 collations. Keep their existing
      // rejection semantics; exercise all BMP characters when registered.
      EXPECT_EQ(OB_NOT_SUPPORTED, ObFTCharUtil::classify_first_char(collation, "ab", 2, actual));
      continue;
    }
    const bool little = collation == CS_TYPE_UTF16LE_BIN;
    for (uint32_t code = 0; code <= 0xffff; ++code) {
      if (code >= 0xd800 && code <= 0xdfff) {
        continue;
      }
      char bytes[2] = {static_cast<char>(little ? code : code >> 8),
                       static_cast<char>(little ? code >> 8 : code)};
      CharType expected = CharType::USELESS;
      if (ObUnicodeBlockUtils::is_alpha(code)) {
        expected = CharType::ENGLISH_LETTER;
      } else if (ObUnicodeBlockUtils::is_arabic(code)) {
        expected = CharType::ARABIC_LETTER;
      } else if (ObUnicodeBlockUtils::is_chinese(code)) {
        expected = CharType::CHINESE;
      } else if (ObUnicodeBlockUtils::is_other_cjk(code)) {
        expected = CharType::OTHER_CJK;
      }
      ASSERT_EQ(OB_SUCCESS, ObFTCharUtil::classify_first_char(collation, bytes, 2, actual));
      ASSERT_EQ(expected, actual) << code << " / " << collation;
    }
  }
}

} // namespace storage
} // namespace oceanbase

int main(int argc, char **argv)
{
  oceanbase::common::ObCharset::init_charset();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
