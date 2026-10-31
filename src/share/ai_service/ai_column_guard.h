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
#pragma once

namespace oceanbase::share
{
// A process-internal capability. SQL and stored routines cannot acquire it.
class ColumnExecutionScope final
{
public:
  ColumnExecutionScope() : previous_(active_) { active_ = true; }
  ~ColumnExecutionScope() { active_ = previous_; }
  static bool active() { return active_; }
  ColumnExecutionScope(const ColumnExecutionScope &) = delete;
  ColumnExecutionScope &operator=(const ColumnExecutionScope &) = delete;
private:
  inline static thread_local bool active_ = false;
  bool previous_;
};

// Allows only the generated task view to resolve its hidden storage columns.
// It grants no permission to publish AI results.
class ColumnTaskViewScope final
{
public:
  ColumnTaskViewScope() : previous_(active_) { active_ = true; }
  ~ColumnTaskViewScope() { active_ = previous_; }
  static bool active() { return active_; }
  ColumnTaskViewScope(const ColumnTaskViewScope &) = delete;
  ColumnTaskViewScope &operator=(const ColumnTaskViewScope &) = delete;
private:
  inline static thread_local bool active_ = false;
  bool previous_;
};
}
