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
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/parser/ob_parser.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
namespace
{
bool contains_file_query(const ObSelectStmt &query, int64_t depth = 0)
{
  if (depth > 64) { return false; }
  for (int64_t i = 0; i < query.get_table_size(); ++i) {
    const TableItem *table = query.get_table_item(i);
    if (OB_ISNULL(table)) { continue; }
    if (table->is_json_table() && OB_NOT_NULL(table->json_table_def_)
        && table->json_table_def_->table_type_ == OB_FILE_TABLE_TYPE) {
      return true;
    } else if (table->is_generated_table() && OB_NOT_NULL(table->ref_query_)
        && contains_file_query(*table->ref_query_, depth + 1)) {
      return true;
    }
  }
  for (int64_t i = 0; i < query.get_set_query().count(); ++i) {
    if (OB_NOT_NULL(query.get_set_query().at(i))
        && contains_file_query(*query.get_set_query().at(i), depth + 1)) { return true; }
  }
  return false;
}
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_INT:
            if (format->value_ != 1) { ret = OB_ERR_UNEXPECTED; }
            break;
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  if (OB_SUCC(ret) && OB_NOT_NULL(parse_tree.children_[EXPLAIN_FORMAT])
      && parse_tree.children_[EXPLAIN_FORMAT]->type_ == T_INT
      && parse_tree.children_[EXPLAIN_FORMAT]->value_ == 1
      && OB_NOT_NULL(explain_stmt->get_explain_query_stmt())
      && explain_stmt->get_explain_query_stmt()->is_select_stmt()) {
    const ObSelectStmt &query = *static_cast<ObSelectStmt *>(explain_stmt->get_explain_query_stmt());
    if (contains_file_query(query)) {
      ret = describe_file_query(query);
    }
  }
  return ret;
}

int ObExplainResolver::describe_file_query(const ObSelectStmt &query)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ret = sql.append("SELECT column_name,column_type,`null`,`key`,`default`,extra FROM (");
  // Hex literals keep metadata names independent of SQL mode and escaping.
  auto string_literal = [&](const ObString &value) -> int {
    int ret = sql.append("CONVERT(X'");
    for (int64_t j = 0; OB_SUCC(ret) && j < value.length(); ++j) {
      ret = sql.append_fmt("%02X", static_cast<unsigned char>(value.ptr()[j]));
    }
    if (OB_SUCC(ret)) {
      ret = sql.append("' USING utf8mb4)");
    }
    return ret;
  };
  for (int64_t i = 0; OB_SUCC(ret) && i < query.get_select_item_size(); ++i) {
    const SelectItem &item = query.get_select_item(i);
    if (OB_ISNULL(item.expr_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const ObExprResType &type = item.expr_->get_result_type();
      const char *name = type.is_integer_type() ? (type.get_type() == ObTinyIntType ? "BOOLEAN" : "BIGINT")
          : type.get_type() == ObDoubleType ? "DOUBLE"
          : type.get_type() == ObDateType ? "DATE"
          : type.get_type() == ObDateTimeType ? "DATETIME"
          : type.is_null() ? "NULL" : "VARCHAR";
      if (i > 0 && OB_FAIL(sql.append(" UNION ALL "))) {
      } else if (OB_FAIL(sql.append_fmt("SELECT %ld AS ordinal, ", i))) {
      } else if (OB_FAIL(string_literal(item.alias_name_.empty() ? item.expr_name_ : item.alias_name_))) {
      } else if (OB_FAIL(sql.append(" AS column_name, "))) {
      } else if (OB_FAIL(string_literal(ObString(name)))) {
      } else if (OB_FAIL(sql.append(" AS column_type, "))) {
      } else if (OB_FAIL(string_literal(ObString(type.has_result_flag(NOT_NULL_FLAG) ? "NO" : "YES")))) {
      } else if (OB_FAIL(sql.append(" AS `null`, NULL AS `key`, NULL AS `default`, NULL AS extra"))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append(") AS file_schema ORDER BY ordinal");
  }
  if (OB_SUCC(ret)) {
    ParseResult parsed;
    ObParser parser(*allocator_, session_info_->get_sql_mode());
    ObSelectResolver resolver(params_);
    if (OB_FAIL(parser.parse(sql.string(), parsed))) {
    } else if (OB_ISNULL(parsed.result_tree_) || parsed.result_tree_->num_child_ != 1
        || OB_ISNULL(parsed.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(resolver.resolve(*parsed.result_tree_->children_[0]))) {
    } else {
      set_basic_stmt(resolver.get_basic_stmt());
    }
  }
  return ret;
}
