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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/string/ob_string_buffer.h"
#include "rust/file_scan.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

int ObDMLResolver::resolve_file_table_item(const ParseNode &node, TableItem *&item)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  ObString secure_root;
  ObString function;
  ObString path;
  ObJsonObject request(allocator_);
  const ParseNode *args = nullptr;
  const ParseNode *alias = nullptr;
  if (node.num_child_ != 3 || OB_ISNULL(node.children_[0])
      || OB_ISNULL(args = node.children_[1]) || args->num_child_ < 1) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else if (OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled; set enable_file_sql = ON");
  } else if (OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_SECURE_FILE_PRIV, secure_root))) {
  } else {
    function.assign_ptr(node.children_[0]->str_value_, node.children_[0]->str_len_);
    alias = node.children_[2];
  }
  auto add_string = [&](const ObString &key, const ObString &value) -> int {
    int ret = OB_SUCCESS;
    ObJsonString *text = OB_NEWx(ObJsonString, allocator_, value);
    if (OB_ISNULL(text)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ret = request.add(key, text);
    }
    return ret;
  };
  if (OB_SUCC(ret)) {
    const char *format = nullptr;
    if (function.case_compare("read_csv") == 0) {
      format = "csv";
    } else if (function.case_compare("read_json") == 0 || function.case_compare("read_ndjson") == 0) {
      format = "json";
    } else if (function.case_compare("read_parquet") == 0) {
      format = "parquet";
    } else if (function.case_compare("glob") == 0) {
      format = "glob";
    } else {
      ret = OB_ERR_PARSER_SYNTAX;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(args->children_[0])) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else if (OB_FAIL(resolve_str_const(*args->children_[0], path))) {
    } else if (OB_FAIL(add_string("path", path))) {
    } else if (OB_FAIL(add_string("format", ObString(format)))) {
    }
  }
  ObSEArray<ObString, 8> seen;
  for (int64_t i = 1; OB_SUCC(ret) && i < args->num_child_; ++i) {
    const ParseNode *option = args->children_[i];
    const ParseNode *name = nullptr;
    const ParseNode *value = nullptr;
    if (OB_ISNULL(option) || option->type_ != T_OP_EQ || option->num_child_ != 2
        || OB_ISNULL(name = option->children_[0]) || OB_ISNULL(value = option->children_[1])) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else if (name->type_ == T_COLUMN_REF && name->num_child_ == 3
        && OB_ISNULL(name->children_[0]) && OB_ISNULL(name->children_[1])) {
      name = name->children_[2];
    }
    ObString key;
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(name) || OB_ISNULL(name->str_value_)) {
        ret = OB_ERR_PARSER_SYNTAX;
      } else {
        key.assign_ptr(name->str_value_, name->str_len_);
        if (key.case_compare("delim") == 0) {
          key = ObString::make_string("delimiter");
        }
        for (int64_t j = 0; OB_SUCC(ret) && j < seen.count(); ++j) {
          if (key.case_compare(seen.at(j)) == 0) {
            ret = OB_INVALID_ARGUMENT;
          }
        }
        if (OB_SUCC(ret)) {
          ret = seen.push_back(key);
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (key.case_compare("sample_size") == 0) {
      const bool all = (value->type_ == T_INT && value->value_ == -1)
          || (value->type_ == T_OP_NEG && value->num_child_ == 1
              && OB_NOT_NULL(value->children_[0]) && value->children_[0]->type_ == T_INT
              && value->children_[0]->value_ == 1);
      if (!all) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL requires sample_size = -1");
      }
    } else if (key.case_compare("header") == 0) {
      if ((value->type_ != T_BOOL && value->type_ != T_INT) || value->value_ < 0 || value->value_ > 1) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        ObJsonBoolean *boolean = OB_NEWx(ObJsonBoolean, allocator_, value->value_ != 0);
        if (OB_ISNULL(boolean)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          ret = request.add("header", boolean);
        }
      }
    } else {
      ObString text;
      if (OB_FAIL(resolve_str_const(*value, text))) {
      } else if (key.case_compare("format") == 0) {
        if ((function.case_compare("read_json") != 0 && function.case_compare("read_ndjson") != 0)
            || text.case_compare("newline_delimited") != 0) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "only newline_delimited JSON is supported");
        }
      } else {
        const char *normalized = key.case_compare("delimiter") == 0 || key.case_compare("delim") == 0 ? "delimiter"
            : key.case_compare("quote") == 0 ? "quote"
            : key.case_compare("escape") == 0 ? "escape"
            : key.case_compare("nullstr") == 0 ? "nullstr" : nullptr;
        if (OB_ISNULL(normalized)) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ret = add_string(ObString(normalized), text);
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObStringBuffer request_text(allocator_);
    if (OB_FAIL(request.print(request_text, 0))) {
    } else {
      try {
        const auto plan = file::prepare_scan(rust::Str(request_text.ptr(), request_text.length()),
            rust::Str(secure_root.empty() ? "" : secure_root.ptr(), secure_root.length()));
        const auto columns = file::scan_columns(rust::Str(plan.data(), plan.size()));
        ObString alias_name = function;
        if (OB_NOT_NULL(alias)) {
          alias_name.assign_ptr(alias->str_value_, alias->str_len_);
        }
        ObString owned_alias;
        ObString owned_plan;
        ObConstRawExpr *source = nullptr;
        ObDmlJtColDef *root = nullptr;
        if (OB_FAIL(ob_write_string(*allocator_, alias_name, owned_alias))) {
        } else if (OB_FAIL(create_unnest_table_item(item, owned_alias))) {
        } else if (OB_FALSE_IT(item->json_table_def_->table_type_ = OB_FILE_TABLE_TYPE)) {
        } else if (OB_FAIL(ob_write_string(*allocator_, ObString(plan.size(), plan.data()), owned_plan))) {
        } else if (OB_FAIL(ObRawExprUtils::build_const_string_expr(*params_.expr_factory_,
            ObVarcharType, owned_plan, CS_TYPE_UTF8MB4_BIN, source))) {
        } else if (OB_FAIL(item->json_table_def_->doc_exprs_.push_back(source))) {
        } else if (OB_FAIL(get_json_table_column_by_id(item->table_id_, root))) {
        } else {
          root->col_base_info_.col_type_ = COL_TYPE_FILE_ROOT;
        }
        for (size_t i = 0; OB_SUCC(ret) && i < columns.size(); ++i) {
          const auto &column = columns[i];
          ObDmlJtColDef *definition = OB_NEWx(ObDmlJtColDef, allocator_);
          ObString name;
          if (OB_ISNULL(definition)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(ob_write_string(*allocator_, ObString(column.name.size(), column.name.data()), name))) {
          } else {
            ObJtColBaseInfo &info = definition->col_base_info_;
            info.col_name_ = name;
            info.col_type_ = COL_TYPE_VALUE;
            info.parent_id_ = 0;
            info.id_ = i + 1;
            // Before code generation this field identifies the resolved SQL
            // column; the physical plan later replaces it with a datum slot.
            info.output_column_idx_ = OB_APP_MIN_COLUMN_ID + i;
            ObObjType type = column.kind == 0 ? ObNullType : column.kind == 1 ? ObIntType : column.kind == 2 ? ObDoubleType
                : column.kind == 3 ? ObTinyIntType : column.kind == 5 ? ObDateType
                : column.kind == 6 ? ObDateTimeType : ObVarcharType;
            info.data_type_.set_obj_type(type);
            info.data_type_.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[type]);
            if (type == ObVarcharType) {
              info.data_type_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
              info.data_type_.set_collation_level(CS_LEVEL_IMPLICIT);
              info.data_type_.set_length(16 * 1024 * 1024);
            }
            ColumnItem *output = nullptr;
            if (OB_FAIL(generate_json_table_output_column_item(item, info.data_type_, name,
                OB_APP_MIN_COLUMN_ID + i, output))) {
            } else if (OB_ISNULL(output)) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              output->col_idx_ = item->json_table_def_->all_cols_.count();
              if (!column.nullable) {
                output->expr_->set_result_flag(NOT_NULL_FLAG);
              }
              if (OB_FAIL(item->json_table_def_->all_cols_.push_back(&info))) {
              } else if (OB_FAIL(root->regular_cols_.push_back(definition))) {
              }
            }
          }
        }
      } catch (const rust::Error &error) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("file table preparation failed", K(ret), "reason", error.what());
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, error.what());
      }
    }
  }
  if (OB_FAIL(ret)) { LOG_WARN("file table resolution failed", K(ret)); }
  return ret;
}
int ObSelectResolver::resolve_copy_options(const ParseNode &node, ObSelectIntoItem &item)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  ObJsonObject options(allocator_);
  auto add_text = [&](const char *key, const ObString &value) -> int {
    ObJsonString *text = OB_NEWx(ObJsonString, allocator_, value);
    return OB_ISNULL(text) ? OB_ALLOCATE_MEMORY_FAILED : options.add(key, text);
  };
  if (node.num_child_ != 2 || OB_ISNULL(node.children_[0])) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else if (OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY requires enable_file_sql = ON");
  } else if (OB_FAIL(resolve_into_const_node(node.children_[0], item.outfile_name_))) {
  } else if (!item.outfile_name_.is_string_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ret = add_text("path", item.outfile_name_.get_string());
  }
  ObSEArray<ObString, 8> seen;
  const ParseNode *list = node.children_[1];
  for (int64_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(list) && i < list->num_child_; ++i) {
    const ParseNode *option = list->children_[i];
    if (OB_ISNULL(option) || option->num_child_ != 2 || OB_ISNULL(option->children_[0])) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else {
      const ParseNode &name = *option->children_[0];
      const ParseNode *value = option->children_[1];
      ObString key(name.str_len_, name.str_value_);
      if (key.case_compare("delim") == 0) {
        key = ObString::make_string("delimiter");
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < seen.count(); ++j) {
        if (key.case_compare(seen.at(j)) == 0) {
          ret = OB_INVALID_ARGUMENT;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(seen.push_back(key))) {
      } else if (key.case_compare("header") == 0) {
        if (OB_NOT_NULL(value) && (value->type_ != T_BOOL || (value->value_ != 0 && value->value_ != 1))) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ObJsonBoolean *header = OB_NEWx(ObJsonBoolean, allocator_, OB_ISNULL(value) || value->value_ != 0);
          ret = OB_ISNULL(header) ? OB_ALLOCATE_MEMORY_FAILED : options.add("header", header);
        }
      } else if (OB_ISNULL(value) || OB_ISNULL(value->str_value_)) {
        ret = OB_ERR_PARSER_SYNTAX;
      } else {
        ObString text(value->str_len_, value->str_value_);
        if (key.case_compare("format") == 0) {
          if (text.case_compare("csv") != 0) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY supports only CSV");
          }
        } else {
          const char *normalized = key.case_compare("delimiter") == 0 || key.case_compare("delim") == 0 ? "delimiter"
              : key.case_compare("quote") == 0 ? "quote"
              : key.case_compare("escape") == 0 ? "escape"
              : key.case_compare("nullstr") == 0 ? "nullstr" : nullptr;
          if (OB_ISNULL(normalized) || (value->type_ != T_VARCHAR && value->type_ != T_CHAR)) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            ret = add_text(normalized, text);
          }
        }
      }
    }
  }
  ObJsonArray names(allocator_);
  const ObSelectStmt *query = get_select_stmt();
  LOG_DEBUG("COPY resolved projection", "count", query->get_select_item_size(),
      "set_query", query->is_set_stmt());
  for (int64_t i = 0; OB_SUCC(ret) && i < query->get_select_item_size(); ++i) {
    const SelectItem &column = query->get_select_item(i);
    const ObString name = column.alias_name_.empty() ? column.expr_name_ : column.alias_name_;
    ObJsonString *text = OB_NEWx(ObJsonString, allocator_, name);
    if (OB_ISNULL(text)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ret = names.append(text);
    }
  }
  if (OB_SUCC(ret)) {
    ObStringBuffer serialized(allocator_);
    if (OB_FAIL(options.add("columns", &names))) {
    } else if (OB_FAIL(options.print(serialized, 0))) {
    } else {
      ret = serialized.get_result_string(item.external_properties_);
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
