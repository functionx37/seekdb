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
#include "lib/string/ob_sql_string.h"

namespace oceanbase::sql
{
using namespace common;
using namespace share::schema;
namespace
{
int append_task_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { ret = sql.append("`"); }
    if (OB_SUCC(ret)) { ret = sql.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int append_task_label(ObSqlString &sql, const ObString &text)
{
  const char digits[] = "0123456789abcdef";
  int ret = sql.append("CONVERT(X'");
  for (int64_t i = 0; OB_SUCC(ret) && i < text.length(); ++i) {
    const unsigned char byte = text.ptr()[i];
    const char pair[] = {digits[byte >> 4], digits[byte & 15]};
    ret = sql.append(pair, 2);
  }
  if (OB_SUCC(ret)) { ret = sql.append("' USING utf8mb4)"); }
  return ret;
}
}

int ObDMLResolver::build_ai_column_tasks_view(ObString &definition)
{
  int ret = OB_SUCCESS;
  ObSqlString query;
  ObSEArray<const ObTableSchema *, 16> tables;
  if (session_info_->get_priv_user_id() != OB_SYS_USER_ID
      && !session_info_->has_user_super_privilege()) {
    ret = OB_ERR_NO_PRIVILEGE;
    LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "inspect AI COLUMN tasks");
  } else if (OB_FAIL(schema_checker_->get_schema_guard()->get_table_schemas_in_runtime(tables))) {}
  // Schema discovery must run again after CREATE/DROP/ALTER TABLE.
  get_stmt()->get_query_ctx()->get_query_hint_for_update().get_global_hint().plan_cache_policy_
      = OB_USE_PLAN_CACHE_NONE;
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
    const ObTableSchema &table = *tables.at(i);
    if (!table.is_user_table() || table.is_in_recyclebin()) { continue; }
    for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
      const auto &column = **it;
      if (!column.is_ai_column()) { continue; }
      const auto *state = table.get_column_schema(column.get_ai_state_column_id());
      const auto *request = table.get_column_schema(column.get_ai_request_column_id());
      const auto *spec = table.get_column_schema(column.get_ai_result_spec_column_id());
      const ObDatabaseSchema *database = nullptr;
      if (OB_ISNULL(state) || OB_ISNULL(request) || OB_ISNULL(spec)) { ret = OB_SCHEMA_EAGAIN; }
      else if (OB_FAIL(schema_checker_->get_schema_guard()->get_database_schema(table.get_database_id(), database))) {}
      else if (OB_ISNULL(database)) { ret = OB_SCHEMA_EAGAIN; }
      else if (query.length() > 0 && OB_FAIL(query.append(" UNION ALL "))) {}
      else if (OB_FAIL(query.append_fmt("SELECT %lu AS table_id,%lu AS column_id,",
          table.get_table_id(), column.get_column_id()))) {}
      else if (OB_FAIL(append_task_identifier(query, request->get_column_name_str()))) {}
      else if (OB_FAIL(query.append(" AS request_id,CONCAT("))) {}
      const auto &keys = table.get_rowkey_info();
      for (int64_t k = 0; OB_SUCC(ret) && k < keys.get_size(); ++k) {
        const auto *key = table.get_column_schema(keys.get_column(k)->column_id_);
        ObSqlString label;
        if (OB_ISNULL(key)) { ret = OB_SCHEMA_EAGAIN; }
        else if (k > 0 && OB_FAIL(query.append(",' AND ',"))) {}
        else if (OB_FAIL(append_task_identifier(label, key->get_column_name_str()))) {}
        else if (OB_FAIL(label.append(" = "))) {}
        else if (OB_FAIL(append_task_label(query, label.string()))) {}
        else if (OB_FAIL(query.append(ob_is_numeric_type(key->get_data_type()) ? ",CAST(" : ",QUOTE(CAST("))) {}
        else if (OB_FAIL(append_task_identifier(query, key->get_column_name_str()))) {}
        else { ret = query.append(ob_is_numeric_type(key->get_data_type()) ? " AS CHAR)" : " AS CHAR))"); }
      }
      if (OB_SUCC(ret)) { ret = query.append(") AS rowkey_predicate,NOW(6) AS eligible_ts,"); }
      for (const char *field : {"phase", "retry_count"}) {
        if (OB_FAIL(ret)) { break; }
        if (OB_FAIL(query.append("COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT("))) {}
        else if (OB_FAIL(append_task_identifier(query, spec->get_column_name_str()))) {}
        else { ret = query.append_fmt(",'$.%s')) AS SIGNED),0) AS %s", field, field[0] == 'p' ? "state" : "retry_count"); }
        if (OB_SUCC(ret) && field[0] == 'p') { ret = query.append(","); }
      }
      if (OB_FAIL(ret)) {}
      else if (OB_FAIL(query.append(" FROM "))) {}
      else if (OB_FAIL(append_task_identifier(query, database->get_database_name_str()))) {}
      else if (OB_FAIL(query.append("."))) {}
      else if (OB_FAIL(append_task_identifier(query, table.get_table_name_str()))) {}
      else if (OB_FAIL(query.append(" WHERE "))) {}
      else if (OB_FAIL(append_task_identifier(query, state->get_column_name_str()))) {}
      else { ret = query.append("='PENDING'"); }
      if (OB_SUCC(ret) && query.length() > 16 * 1024 * 1024) { ret = OB_SIZE_OVERFLOW; }
    }
  }
  if (OB_SUCC(ret) && query.empty()) {
    ret = query.append("SELECT CAST(NULL AS UNSIGNED) AS table_id,CAST(NULL AS UNSIGNED) AS column_id,"
        "CAST(NULL AS CHAR(64)) AS request_id,CAST(NULL AS CHAR(4096)) AS rowkey_predicate,"
        "NOW(6) AS eligible_ts,0 AS state,0 AS retry_count WHERE FALSE");
  }
  if (OB_SUCC(ret)) { ret = ob_write_string(*allocator_, query.string(), definition); }
  return ret;
}
}
