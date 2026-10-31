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
#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "sql/resolver/ddl/ob_alter_table_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "query/vector/ob_vector_index_util.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;

int ObDDLResolver::resolve_ai_column_attribute(ObColumnSchemaV2 &column, const ParseNode &node)
{
  int ret = OB_SUCCESS;
  if (column.is_ai_column() || node.num_child_ != 2 || OB_ISNULL(node.children_[0])) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(column.set_ai_expression(ObString(node.children_[0]->str_len_,
                                                       node.children_[0]->str_value_)))) {
  } else {
    column.add_column_flag(AI_COLUMN_FLAG);
  }
  bool has_mode = false;
  bool has_retries = false;
  const ParseNode *options = node.children_[1];
  for (int64_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(options) && i < options->num_child_; ++i) {
    const ParseNode *option = options->children_[i];
    if (OB_ISNULL(option) || option->num_child_ != 2
        || OB_ISNULL(option->children_[0]) || OB_ISNULL(option->children_[1])) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const ParseNode &key = *option->children_[0];
      const ParseNode &value = *option->children_[1];
      const ObString name(key.str_len_, key.str_value_);
      if (name.case_compare("sync_mode") == 0) {
        if (has_mode || value.type_ != T_VARCHAR
            || ObString(value.str_len_, value.str_value_).case_compare("async") != 0) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN supports only sync_mode='async'");
        }
        has_mode = true;
      } else if (name.case_compare("max_retries") == 0) {
        if (has_retries || value.type_ != T_INT || value.value_ < 0 || value.value_ > 100) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN max_retries must be an integer from 0 to 100");
        } else {
          column.set_ai_max_retries(value.value_);
        }
        has_retries = true;
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unknown AI COLUMN option");
      }
    }
  }
  return ret;
}

namespace
{
int validate_ai_argument(const ObRawExpr *expr, int64_t depth = 0)
{
  int ret = OB_SUCCESS;
  bool non_pure = false;
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (depth >= 64) {
    ret = OB_SIZE_OVERFLOW;
  } else if (expr->get_expr_type() == T_FUN_SYS_AI_COMPLETE
          || expr->get_expr_type() == T_FUN_SYS_AI_EMBED
          || expr->get_expr_type() == T_FUN_SYS_AI_RERANK) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested model calls in AI COLUMN");
  } else if (expr->is_sys_func_expr() && expr->get_expr_type() != T_FUN_SYS_AI_PROMPT
          && OB_FAIL(expr->is_non_pure_sys_func_expr(non_pure))) {
  } else if (non_pure) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "nondeterministic AI COLUMN inputs");
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
      ret = validate_ai_argument(expr->get_param_expr(i), depth + 1);
    }
  }
  return ret;
}
} // namespace

int ObDDLResolver::resolve_ai_column(ObColumnSchemaV2 &column, const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  ObRawExpr *expression = nullptr;
  ObSEArray<ObQualifiedName, 4> names;
  ObSEArray<uint64_t, 4> dependencies;
  if (!column.is_ai_column()) {
  } else if (column.is_rowkey_column() || !column.is_nullable()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN must be nullable and cannot be a primary key");
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(column.get_ai_expression(),
      *params_.expr_factory_, *session_info_, expression, names, &table, nullptr,
      schema_checker_, ObResolverUtils::DISABLE_CHECK))) {
  } else if (OB_ISNULL(expression)
      || (expression->get_expr_type() != T_FUN_SYS_AI_COMPLETE
          && expression->get_expr_type() != T_FUN_SYS_AI_EMBED)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN requires AI_COMPLETE or AI_EMBED as its outermost function");
  } else if (expression->get_param_count() < 2 || expression->get_param_count() > 3) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!expression->get_param_expr(0)->is_const_raw_expr()
      || !static_cast<ObConstRawExpr *>(expression->get_param_expr(0))->get_value().is_varchar()
      || static_cast<ObConstRawExpr *>(expression->get_param_expr(0))->get_value().get_string().empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN requires a nonempty constant model name");
  }
  if (OB_SUCC(ret) && column.is_ai_column()) {
    const ObAiModelSchema *model = nullptr;
    const ObString model_name = static_cast<ObConstRawExpr *>(expression->get_param_expr(0))->get_value().get_string();
    if (OB_ISNULL(schema_checker_) || OB_ISNULL(schema_checker_->get_schema_guard())) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(schema_checker_->get_schema_guard()->get_ai_model_schema(model_name, model))) {
    } else if (OB_ISNULL(model)) {
      ret = OB_AI_FUNC_MODEL_NOT_FOUND;
      LOG_USER_ERROR(OB_AI_FUNC_MODEL_NOT_FOUND, model_name.length(), model_name.ptr());
    } else if ((expression->get_expr_type() == T_FUN_SYS_AI_EMBED
                   && model->get_type() != share::EndpointType::DENSE_EMBEDDING)
            || (expression->get_expr_type() == T_FUN_SYS_AI_COMPLETE
                   && model->get_type() != share::EndpointType::COMPLETION)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN model type does not match its generating function");
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && column.is_ai_column() && i < names.count(); ++i) {
    const ObQualifiedName &name = names.at(i);
    const ObColumnSchemaV2 *dependency = table.get_column_schema(name.col_name_);
    if (name.is_sys_func() || !name.database_name_.empty() || !name.tbl_name_.empty()) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_ISNULL(dependency) || dependency->is_hidden()) {
      ret = OB_ERR_BAD_FIELD_ERROR;
      LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, name.col_name_.length(), name.col_name_.ptr(),
                     9, "AI COLUMN");
    } else if (dependency->is_ai_column() || dependency->is_generated_column()) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(ObRawExprUtils::init_column_expr(*dependency, session_info_, *name.ref_expr_))) {
    } else if (OB_FAIL(add_var_to_array_no_dup(dependencies, dependency->get_column_id()))) {
    }
  }
  if (OB_SUCC(ret) && column.is_ai_column()) {
    for (int64_t i = 0; OB_SUCC(ret) && i < expression->get_param_count(); ++i) {
      ret = validate_ai_argument(expression->get_param_expr(i));
    }
    if (OB_SUCC(ret)) {
      ret = expression->formalize(session_info_);
    }
    const bool embedding = expression->get_expr_type() == T_FUN_SYS_AI_EMBED;
    int64_t dimension = 0;
    if (OB_FAIL(ret)) {
    } else if (embedding && !column.is_collection()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED result requires a VECTOR column");
    } else if (embedding && OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
          column.get_extended_type_info(), dimension))) {
    } else if (!embedding && column.is_collection()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "VECTOR AI COLUMN requires AI_EMBED");
    } else if (!embedding && !column.is_json() && !column.is_string_type()
            && !ob_is_numeric_type(column.get_data_type()) && !ob_is_datetime_tc(column.get_data_type())
            && !ob_is_date_tc(column.get_data_type())) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "unsupported AI COLUMN result type");
    }
    if (OB_SUCC(ret) && embedding && expression->get_param_count() == 3) {
      const ObRawExpr *argument = expression->get_param_expr(2);
      if (!argument->is_const_raw_expr()) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const ObObj &value = static_cast<const ObConstRawExpr *>(argument)->get_value();
        if (!value.is_integer_type() || value.get_int() <= 0) {
          ret = OB_INVALID_ARGUMENT;
        } else if (value.get_int() != dimension) {
          ret = OB_ERR_INVALID_VECTOR_DIM;
          LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<unsigned>(dimension),
                         static_cast<unsigned>(value.get_int()));
        }
      }
    }
    // Print resolved expressions so names and string escaping survive schema persistence.
    ObSqlString normalized;
    char *buffer = static_cast<char *>(allocator_->alloc(OB_MAX_DEFAULT_VALUE_LENGTH));
    auto append_expr = [&](ObRawExpr *expr) -> int {
      int64_t pos = 0;
      ObRawExprPrinter printer(buffer, OB_MAX_DEFAULT_VALUE_LENGTH, &pos, nullptr);
      int error = printer.do_print(expr, T_NONE_SCOPE, true);
      return error == OB_SUCCESS ? normalized.append(buffer, pos) : error;
    };
    if (OB_SUCC(ret) && OB_ISNULL(buffer)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    if (OB_SUCC(ret)) {
      const char *instruction = column.is_json() ? "Return exactly one valid JSON value"
          : ob_is_integer_type(column.get_data_type()) ? "Return exactly one base-10 integer without explanation"
          : ob_is_numeric_type(column.get_data_type()) ? "Return exactly one finite number without explanation"
          : (ob_is_datetime_tc(column.get_data_type()) || ob_is_date_tc(column.get_data_type()))
              ? "Return exactly one ISO 8601 date or datetime without explanation"
          : "Return only the requested text without explanation";
      if (OB_FAIL(normalized.append(embedding ? "AI_EMBED(" : "AI_COMPLETE("))) {
      } else if (OB_FAIL(append_expr(expression->get_param_expr(0)))) {
      } else if (OB_FAIL(normalized.append(", "))) {
      } else if (OB_FAIL(append_expr(expression->get_param_expr(1)))) {
      } else if (embedding && OB_FAIL(normalized.append_fmt(", %ld", dimension))) {
      } else if (!embedding) {
        if (OB_FAIL(normalized.append(", JSON_MERGE_PATCH(COALESCE("))) {
        } else if (expression->get_param_count() == 3) {
          ret = append_expr(expression->get_param_expr(2));
        } else {
          ret = normalized.append("NULL");
        }
        if (OB_SUCC(ret)) {
          ret = normalized.append_fmt(", JSON_OBJECT()), JSON_OBJECT('system_prompt', '%s'))", instruction);
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(normalized.append(")"))) {
      } else if (OB_SUCC(ret) && OB_FAIL(column.set_ai_normalized_expr(normalized.string()))) {
      } else if (OB_SUCC(ret)) {
        ret = column.set_ai_dependencies(dependencies);
      }
    }
  }
  return ret;
}

int ObDDLResolver::add_ai_hidden_columns(ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 4> ai_columns;
  uint64_t next_id = table.get_max_used_column_id();
  for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
    next_id = std::max(next_id, (*it)->get_column_id());
    if ((*it)->is_ai_column()) {
      if (OB_FAIL(resolve_ai_column(**it, table))) {
      } else if ((*it)->get_ai_state_column_id() == 0) {
        ret = ai_columns.push_back((*it)->get_column_id());
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    const uint64_t column_id = ai_columns.at(i);
    for (int64_t field = 0; OB_SUCC(ret) && field < 3; ++field) {
      ObColumnSchemaV2 hidden;
      ObSqlString name;
      ObObj initial;
      const char *kind = field == 0 ? "state" : field == 1 ? "request" : "result_spec";
      hidden.set_table_id(table.get_table_id());
      hidden.set_column_id(++next_id);
      hidden.set_data_type(field == 2 ? ObLongTextType : ObVarcharType);
      hidden.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      hidden.set_charset_type(CHARSET_UTF8MB4);
      hidden.set_data_length(field == 0 ? 7 : field == 1 ? 64 : OB_MAX_LONGTEXT_LENGTH);
      hidden.set_is_hidden(true);
      hidden.set_nullable(true);
      if (field == 0) {
        initial.set_varchar("PENDING");
        initial.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else if (field == 1) {
        initial.set_varchar("uuid()");
        initial.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {
        initial.set_null();
      }
      if (OB_FAIL(name.append_fmt("__ai_%s_%lu", kind, column_id))) {
      } else if (OB_FAIL(hidden.set_column_name(name.string()))) {
      } else if (OB_FAIL(hidden.set_cur_default_value(initial, field == 1))) {
      } else if (OB_FAIL(hidden.set_orig_default_value(initial))) {
      } else if (OB_FAIL(table.add_column(hidden))) {
      } else {
        ObColumnSchemaV2 *result = table.get_column_schema(column_id);
        if (field == 0) {
          result->set_ai_state_column_id(next_id);
        } else if (field == 1) {
          result->set_ai_request_column_id(next_id);
        } else {
          result->set_ai_result_spec_column_id(next_id);
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    table.set_max_used_column_id(next_id);
  }
  return ret;
}
int ObAlterTableResolver::resolve_ai_column_additions()
{
  int ret = OB_SUCCESS;
  ObAlterTableStmt *statement = get_alter_table_stmt();
  AlterTableSchema &changes = statement->get_alter_table_arg().alter_table_schema_;
  bool adds_ai = false;
  ObSEArray<uint64_t, 4> hidden_drops;
  for (auto it = changes.column_begin(); OB_SUCC(ret) && it != changes.column_end(); ++it) {
    adds_ai |= (*it)->is_ai_column();
    const auto &change = *static_cast<AlterColumnSchema *>(*it);
    if (change.alter_type_ == OB_DDL_ADD_COLUMN) { continue; }
    const ObColumnSchemaV2 *original = table_schema_->get_column_schema(change.get_origin_column_name());
    if (OB_ISNULL(original)) { continue; } // The regular DDL checks diagnose unknown columns.
    if (original->is_ai_column()) {
      if (change.alter_type_ != OB_DDL_DROP_COLUMN) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "modifying an existing AI COLUMN definition");
      } else if (OB_FAIL(hidden_drops.push_back(original->get_ai_state_column_id()))) {
      } else if (OB_FAIL(hidden_drops.push_back(original->get_ai_request_column_id()))) {
      } else {
        ret = hidden_drops.push_back(original->get_ai_result_spec_column_id());
      }
    } else if (change.alter_type_ == OB_DDL_DROP_COLUMN || change.alter_type_ == OB_DDL_CHANGE_COLUMN
            || change.alter_type_ == OB_DDL_MODIFY_COLUMN) {
      for (auto source = table_schema_->column_begin(); OB_SUCC(ret) && source != table_schema_->column_end(); ++source) {
        if (!(*source)->is_ai_column()) { continue; }
        ObSEArray<uint64_t, 4> dependencies;
        if (OB_FAIL((*source)->get_ai_dependencies(dependencies))) {
        } else if (has_exist_in_array(dependencies, original->get_column_id())) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "dropping, renaming or changing the type of an AI COLUMN dependency");
        }
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < hidden_drops.count(); ++i) {
    const ObColumnSchemaV2 *source = table_schema_->get_column_schema(hidden_drops.at(i));
    AlterColumnSchema hidden;
    if (OB_ISNULL(source)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(hidden.ObColumnSchemaV2::assign(*source))) {
    } else if (OB_FAIL(hidden.set_origin_column_name(source->get_column_name_str()))) {
    } else {
      hidden.alter_type_ = OB_DDL_DROP_COLUMN;
      ret = statement->add_column(hidden);
    }
  }
  if (OB_SUCC(ret) && adds_ai) {
    ObTableSchema merged;
    ObSEArray<AlterColumnSchema *, 4> additions;
    uint64_t next_id = table_schema_->get_max_used_column_id();
    if (OB_FAIL(merged.assign(*table_schema_))) {
    }
    for (auto it = changes.column_begin(); OB_SUCC(ret) && it != changes.column_end(); ++it) {
      auto *column = static_cast<AlterColumnSchema *>(*it);
      if (column->alter_type_ != OB_DDL_ADD_COLUMN) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "mixing AI COLUMN additions with other column changes");
      } else {
        column->set_column_id(++next_id);
        column->set_table_id(merged.get_table_id());
        if (OB_FAIL(merged.add_column(*column))) {
        } else {
          ret = additions.push_back(column);
        }
      }
    }
    merged.set_max_used_column_id(next_id);
    if (OB_SUCC(ret) && OB_FAIL(add_ai_hidden_columns(merged))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < additions.count(); ++i) {
      AlterColumnSchema &column = *additions.at(i);
      ret = column.ObColumnSchemaV2::assign(*merged.get_column_schema(column.get_column_id()));
    }
    // Append hidden additions in exactly the order used by the DDL ID allocator.
    for (uint64_t id = next_id + 1; OB_SUCC(ret) && id <= merged.get_max_used_column_id(); ++id) {
      AlterColumnSchema hidden;
      const ObColumnSchemaV2 *source = merged.get_column_schema(id);
      if (OB_ISNULL(source)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(hidden.ObColumnSchemaV2::assign(*source))) {
      } else {
        hidden.alter_type_ = OB_DDL_ADD_COLUMN;
        ret = statement->add_column(hidden);
      }
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
