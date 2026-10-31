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

#define USING_LOG_PREFIX SERVER_OMT
#include "ob_ai_service.h"
#include "share/ai_service/ai_column_guard.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/time/ob_time_utility.h"
#include "common/json_type/ob_json_tree.h"
#include "share/object/ob_obj_cast.h"

using namespace oceanbase::share;
using namespace oceanbase::common;
namespace oceanbase
{
namespace omt
{
namespace
{
constexpr int64_t COLUMN_WORKER_COUNT = 4;
}

ObAiService::ObAiService()
  : is_inited_(false)
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    is_inited_ = true;
  }
  return ret;
}

void ObAiService::destroy()
{
  stop();
  wait();
  is_inited_ = false;
}

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(set_thread_count(COLUMN_WORKER_COUNT))) {
  } else {
    ret = share::ObThreadPool::start();
  }
  return ret;
}

void ObAiService::run1()
{
  lib::set_thread_name("AIColumn");
  while (!has_set_stop()) {
    const int ret = process_columns();
    if (ret != OB_SUCCESS && ret != OB_SCHEMA_EAGAIN) {
      LOG_WARN("AI COLUMN scheduler pass failed", K(ret));
    }
    ob_usleep(100000);
  }
}

namespace
{
using namespace share::schema;

int append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { ret = sql.append("`"); }
    if (OB_SUCC(ret)) { ret = sql.append(name.ptr() + i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int append_text(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("CONVERT(X'");
  const char hex[] = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char byte = value.ptr()[i];
    const char encoded[] = {hex[byte >> 4], hex[byte & 15]};
    ret = sql.append(encoded, 2);
  }
  if (OB_SUCC(ret)) { ret = sql.append("' USING utf8mb4)"); }
  return ret;
}

int quote_column_id(const ObTableSchema &table, uint64_t id,
    ObIAllocator &allocator, ObString &name)
{
  int ret = OB_SUCCESS;
  ObSqlString quoted;
  const auto *column = table.get_column_schema(id);
  if (OB_ISNULL(column)) { ret = OB_SCHEMA_EAGAIN; }
  else if (OB_FAIL(append_identifier(quoted, column->get_column_name_str()))) {}
  else { ret = ob_write_string(allocator, quoted.string(), name); }
  return ret;
}

struct ColumnFields
{
  ObString state_;
  ObString request_;
  ObString spec_;
};

// Hidden state is the durable queue: creation, invalidation and deletion use
// the business row's transaction. A rolled back row is never visible here.
int process_column_row(const ObTableSchema &table, const ObString &qualified_table,
    const ObIArray<const ObColumnSchemaV2 *> &columns, const ObAiService &service)
{
  int ret = OB_SUCCESS;
  ObSqlString select;
  ObSqlString key;
  ObSqlString predicate;
  ObArenaAllocator allocator("AIColumnRow");
  ObSEArray<ObString, 4> requests;
  ObSEArray<int64_t, 4> attempts;
  ObSEArray<bool, 4> pending;
  ObSEArray<ObJsonObject *, 4> saved;
  ObSEArray<ColumnFields, 4> fields;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const auto &column = *columns.at(i);
    ColumnFields names;
    if (OB_FAIL(quote_column_id(table, column.get_ai_state_column_id(), allocator, names.state_))) {}
    else if (OB_FAIL(quote_column_id(table, column.get_ai_request_column_id(), allocator, names.request_))) {}
    else if (OB_FAIL(quote_column_id(table, column.get_ai_result_spec_column_id(), allocator, names.spec_))) {}
    else { ret = fields.push_back(names); }
  }
  const ObRowkeyInfo &rowkey = table.get_rowkey_info();
  if (OB_SUCC(ret) && OB_FAIL(select.append("SELECT "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey.get_size(); ++i) {
    const ObColumnSchemaV2 *column = table.get_column_schema(rowkey.get_column(i)->column_id_);
    if (OB_ISNULL(column)) { ret = OB_ERR_UNEXPECTED; }
    else if (i > 0 && OB_FAIL(select.append(","))) {}
    else { ret = append_identifier(select, column->get_column_name_str()); }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const auto &names = fields.at(i);
    ret = select.append_fmt(",%.*s,%.*s,CAST(%.*s AS CHAR)",
        names.state_.length(), names.state_.ptr(), names.request_.length(), names.request_.ptr(),
        names.spec_.length(), names.spec_.ptr());
  }
  if (OB_SUCC(ret)) { ret = select.append_fmt(" FROM %.*s WHERE ", qualified_table.length(), qualified_table.ptr()); }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (i > 0 && OB_FAIL(select.append(" OR "))) {}
    else { ret = select.append_fmt("%.*s='PENDING'", fields.at(i).state_.length(), fields.at(i).state_.ptr()); }
  }
  if (OB_SUCC(ret)) { ret = select.append(" LIMIT 1"); }
  // Copy all row identities before closing the read cursor. Model requests
  // never hold a lock or an open transaction on a business row.
  if (OB_SUCC(ret)) {
    ObMySQLProxy::MySQLResult result;
    sqlclient::ObMySQLResult *rows = nullptr;
    if (OB_FAIL(GCTX.sql_proxy_->read(result, select.ptr()))) {
    } else if (OB_ISNULL(rows = result.get_result())) { ret = OB_ERR_UNEXPECTED; }
    else if (OB_FAIL(rows->next())) {
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; return ret; }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey.get_size(); ++i) {
      ObObj value;
      const auto *column = table.get_column_schema(rowkey.get_column(i)->column_id_);
      char literal[OB_MAX_VARCHAR_LENGTH];
      int64_t position = 0;
      if (OB_FAIL(rows->get_obj(i, value))) {}
      else if (i > 0 && OB_FAIL(key.append(" AND "))) {}
      else if (OB_FAIL(append_identifier(key, column->get_column_name_str()))) {}
      else if (OB_FAIL(key.append(" <=> "))) {}
      else if (OB_FAIL(value.print_sql_literal(literal, sizeof(literal), position))) {}
      else { ret = key.append(literal, position); }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      ObString state;
      ObString request;
      ObString owned;
      int64_t attempt = 0;
      ObObj spec;
      ObIJsonBase *json = nullptr;
      const int64_t offset = rowkey.get_size() + i * 3;
      if (OB_FAIL(rows->get_varchar(offset, state))) {}
      else if (OB_FAIL(rows->get_varchar(offset + 1, request))) {}
      else if (OB_FAIL(ob_write_string(allocator, request, owned))) {}
      else if (OB_FAIL(rows->get_obj(offset + 2, spec))) {}
      else if (!spec.is_null()) {
        ObString serialized;
        if (OB_FAIL(ob_write_string(allocator, spec.get_string(), serialized))) {}
        else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, serialized,
            ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json))) {}
        else if (json->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_INVALID_DATA; }
        else {
          auto *object = static_cast<ObJsonObject *>(json);
          auto *count = object->get_value("attempts");
          if (count == nullptr || count->json_type() != ObJsonNodeType::J_INT) { ret = OB_INVALID_DATA; }
          else { attempt = static_cast<ObJsonInt *>(count)->value(); }
        }
      }
      if (OB_FAIL(ret)) {}
      else if (OB_FAIL(pending.push_back(state == "PENDING"))) {}
      else if (OB_FAIL(requests.push_back(owned))) {}
      else if (OB_FAIL(saved.push_back(static_cast<ObJsonObject *>(json)))) {}
      else { ret = attempts.push_back(attempt); }
    }
  }
  if (OB_SUCC(ret)) { ret = predicate.assign(key.string()); }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (!pending.at(i)) { continue; }
    if (OB_FAIL(predicate.append_fmt(" AND %.*s=", fields.at(i).request_.length(), fields.at(i).request_.ptr()))) {}
    else if (OB_FAIL(append_text(predicate, requests.at(i)))) {}
    else { ret = predicate.append_fmt(" AND %.*s='PENDING'", fields.at(i).state_.length(), fields.at(i).state_.ptr()); }
  }
  ObSqlString assignments;
  for (int64_t i = 0; OB_SUCC(ret) && !service.has_set_stop() && i < columns.count(); ++i) {
    if (!pending.at(i)) { continue; }
    const auto &column = *columns.at(i);
    const auto &names = fields.at(i);
    ObString output;
    bool ready = false;
    bool is_null = true;
    bool complete = saved.at(i) != nullptr && saved.at(i)->get_value("ready") != nullptr;
    if (complete) {
      auto *status = saved.at(i)->get_value("ready");
      auto *value = saved.at(i)->get_value("result");
      if (value == nullptr) { return OB_INVALID_DATA; }
      if (status->json_type() == ObJsonNodeType::J_BOOLEAN) {
        ready = static_cast<ObJsonBoolean *>(status)->value();
      } else if (status->json_type() == ObJsonNodeType::J_INT) {
        ready = static_cast<ObJsonInt *>(status)->value() != 0;
      } else { return OB_INVALID_DATA; }
      is_null = value->json_type() == ObJsonNodeType::J_NULL;
      if (!is_null) {
        if (value->json_type() != ObJsonNodeType::J_STRING) { return OB_INVALID_DATA; }
        output = static_cast<ObJsonString *>(value)->get_str();
      }
    }
    ObSqlString own_predicate;
    if (OB_FAIL(own_predicate.append_fmt("%.*s AND %.*s='PENDING' AND %.*s=",
        key.length(), key.ptr(), names.state_.length(), names.state_.ptr(),
        names.request_.length(), names.request_.ptr()))) {}
    else { ret = append_text(own_predicate, requests.at(i)); }
    if (OB_FAIL(ret)) { return ret; }
    // Persist the attempt before sending. Recovery cannot reset the retry
    // budget, even if the process dies after the upstream accepted a call.
    for (int64_t attempt = attempts.at(i); !complete && !service.has_set_stop() && attempt <= column.get_ai_max_retries(); ++attempt) {
      ObSqlString claim;
      int64_t changed = 0;
      if (OB_FAIL(claim.append_fmt("UPDATE %.*s SET %.*s=JSON_OBJECT('attempts',%ld,'phase',1,'retry_count',%ld) WHERE %.*s",
          qualified_table.length(), qualified_table.ptr(), names.spec_.length(), names.spec_.ptr(), attempt + 1,
          attempt, own_predicate.length(), own_predicate.ptr()))) {}
      else if (OB_FAIL(GCTX.sql_proxy_->write(claim.ptr(), changed))) {}
      if (OB_FAIL(ret) || changed != 1) { return ret; }
      ObSqlString inference;
      ObMySQLProxy::MySQLResult result;
      sqlclient::ObMySQLResult *rows = nullptr;
      if (OB_FAIL(inference.append_fmt("SELECT CAST((%.*s) AS CHAR) FROM %.*s WHERE %.*s",
          column.get_ai_normalized_expr().length(), column.get_ai_normalized_expr().ptr(),
          qualified_table.length(), qualified_table.ptr(), own_predicate.length(), own_predicate.ptr()))) {}
      else if (OB_FAIL(GCTX.sql_proxy_->read(result, inference.ptr(), nullptr, 60000000))) {}
      else if (OB_ISNULL(rows = result.get_result())) { ret = OB_ERR_UNEXPECTED; }
      else if (OB_FAIL(rows->next())) {}
      else {
        ObString value;
        ObObj object;
        if (OB_FAIL(rows->get_obj(int64_t(0), object))) {}
        else if (object.is_null()) { ready = true; }
        else if (OB_FAIL(rows->get_varchar(int64_t(0), value))) {}
        else {
          ObIJsonBase *json = nullptr;
          // AI_COMPLETE returns the model's text verbatim. Validate that text
          // against the declared column type before allowing publication.
          if (OB_SUCC(ret) && column.is_json()) {
            ret = ObJsonBaseFactory::get_json_base(&allocator, value,
                ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json);
          } else if (OB_SUCC(ret) && !column.is_string_type() && !column.is_collection()) {
            ObObj source;
            ObObj converted;
            ObAccuracy accuracy = column.get_accuracy();
            source.set_varchar(value);
            source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
            ObCastCtx cast(&allocator, nullptr,
                CM_STRICT_MODE | CM_ERROR_ON_SCALE_OVER | CM_NO_ZERO_DATE,
                column.get_collation_type(), &accuracy);
            ret = ObObjCaster::to_type(column.get_data_type(), cast, source, converted);
            if (OB_SUCC(ret)) {
              ret = ob_obj_accuracy_check_only(accuracy, column.get_collation_type(), converted);
            }
          } else if (OB_SUCC(ret) && column.is_string_type()) {
            ObObj source;
            source.set_varchar(value);
            source.set_collation_type(column.get_collation_type());
            ret = ob_obj_accuracy_check_only(column.get_accuracy(), column.get_collation_type(), source);
          }
          if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, value, output))) {}
          else if (OB_SUCC(ret)) { ready = true; is_null = false; }
        }
      }
      if (ret == OB_ITER_END) { return OB_SUCCESS; } // superseded or deleted
      if (ready) { break; }
      ret = OB_SUCCESS;
      if (attempt < column.get_ai_max_retries()) {
        ObSqlString retry;
        if (OB_FAIL(retry.append_fmt("UPDATE %.*s SET %.*s=JSON_OBJECT('attempts',%ld,'phase',3,'retry_count',%ld) WHERE %.*s",
            qualified_table.length(), qualified_table.ptr(), names.spec_.length(), names.spec_.ptr(),
            attempt + 1, attempt + 1, own_predicate.length(), own_predicate.ptr()))) {}
        else { ret = GCTX.sql_proxy_->write(retry.ptr(), changed); }
        if (OB_FAIL(ret) || changed != 1) { return ret; }
        ob_usleep(200000);
      }
    }
    if (!complete && !service.has_set_stop()) {
      // Keep successful sibling results durable until the entire generation
      // is terminal. A crash or an unrelated dependency update cannot lose
      // their results or consume their retry budget a second time.
      ObSqlString save;
      int64_t changed = 0;
      if (OB_FAIL(save.append_fmt("UPDATE %.*s SET %.*s=JSON_OBJECT("
          "'attempts',%ld,'ready',%s,'result',", qualified_table.length(), qualified_table.ptr(),
          names.spec_.length(), names.spec_.ptr(), column.get_ai_max_retries() + 1, ready ? "TRUE" : "FALSE"))) {}
      else if (ready && !is_null) { ret = append_text(save, output); }
      else { ret = save.append("NULL"); }
      if (OB_SUCC(ret) && OB_FAIL(save.append_fmt(") WHERE %.*s", own_predicate.length(), own_predicate.ptr()))) {}
      else if (OB_SUCC(ret)) { ret = GCTX.sql_proxy_->write(save.ptr(), changed); }
      if (OB_FAIL(ret) || changed != 1) { return ret; }
    }
    if (assignments.length() > 0 && OB_FAIL(assignments.append(","))) {}
    else if (OB_FAIL(append_identifier(assignments, column.get_column_name_str()))) {}
    else if (OB_FAIL(assignments.append("="))) {}
    else if (ready && !is_null) { ret = append_text(assignments, output); }
    else { ret = assignments.append("NULL"); }
    if (OB_SUCC(ret)) {
      ret = assignments.append_fmt(",%.*s='%s',%.*s=NULL",
          names.state_.length(), names.state_.ptr(), ready ? "READY" : "FAILED",
          names.spec_.length(), names.spec_.ptr());
    }
  }
  if (OB_SUCC(ret) && !service.has_set_stop() && assignments.length() > 0) {
    // Revalidate definition identity as part of the publication statement.
    // The request predicates also reject delete/reinsert and primary-key ABA.
    ObSqlString publish;
    int64_t changed = 0;
    if (OB_FAIL(publish.append_fmt("UPDATE %.*s SET %.*s WHERE %.*s AND EXISTS "
        "(SELECT 1 FROM oceanbase.__all_table WHERE table_id=%lu AND schema_version=%ld)",
        qualified_table.length(), qualified_table.ptr(), assignments.length(), assignments.ptr(),
        predicate.length(), predicate.ptr(), table.get_table_id(), table.get_schema_version()))) {}
    else { ret = GCTX.sql_proxy_->write(publish.ptr(), changed); }
  }
  return ret;
}
} // namespace

int ObAiService::process_columns()
{
  int ret = OB_SUCCESS;
  schema::ObSchemaGetterGuard guard;
  ObSEArray<const schema::ObTableSchema *, 16> tables;
  if (OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(GCTX.sql_proxy_)
      || !GCTX.schema_service_->is_runtime_schema_refreshed()) {
    ret = OB_SCHEMA_EAGAIN;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schemas_in_runtime(tables))) {
  }
  ColumnExecutionScope worker_scope;
  for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < tables.count(); ++i) {
    const auto &table = *tables.at(i);
    // A table has exactly one owner in this server. Other tables can make
    // progress while its owner waits for an external model. Ownership is
    // derived from persistent table identity and is rebuilt after restart.
    if (!table.is_user_table() || table.is_in_recyclebin()
        || table.get_table_id() % COLUMN_WORKER_COUNT != get_thread_idx()) { continue; }
    ObSEArray<const schema::ObColumnSchemaV2 *, 4> columns;
    for (auto it = table.column_begin(); OB_SUCC(ret) && it != table.column_end(); ++it) {
      if ((*it)->is_ai_column()) { ret = columns.push_back(*it); }
    }
    if (OB_FAIL(ret) || columns.empty()) { continue; }
    const schema::ObDatabaseSchema *database = nullptr;
    ObSqlString qualified;
    if (OB_FAIL(guard.get_database_schema(table.get_database_id(), database))) {}
    else if (OB_ISNULL(database)) { ret = OB_SCHEMA_EAGAIN; }
    else if (OB_FAIL(append_identifier(qualified, database->get_database_name_str()))) {}
    else if (OB_FAIL(qualified.append("."))) {}
    else if (OB_FAIL(append_identifier(qualified, table.get_table_name_str()))) {}
    else {
      const int error = process_column_row(table, qualified.string(), columns, *this);
      if (error != OB_SUCCESS) { LOG_WARN("AI COLUMN row failed", K(error), "table_id", table.get_table_id()); }
    }
  }
  return ret;
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase
