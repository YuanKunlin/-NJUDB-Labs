/*------------------------------------------------------------------------------
 - Copyright (c) 2024. Websoft research group, Nanjing University.
 -
 - This program is free software: you can redistribute it and/or modify
 - it under the terms of the GNU General Public License as published by
 - the Free Software Foundation, either version 3 of the License, or
 - (at your option) any later version.
 -
 - This program is distributed in the hope that it will be useful,
 - but WITHOUT ANY WARRANTY; without even the implied warranty of
 - MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 - GNU General Public License for more details.
 -
 - You should have received a copy of the GNU General Public License
 - along with this program.  If not, see <https://www.gnu.org/licenses/>.
 -----------------------------------------------------------------------------*/

//
// Created by ziqi on 2024/7/18.
//

#include "executor_update.h"
#include "concurrency/lock_manager.h"
#include "concurrency/version_manager.h"
#include "concurrency/concurrency_protocol.h"
#include "mvcc_write_coordinator.h"
#include <fmt/format.h>

namespace njudb {

UpdateExecutor::UpdateExecutor(AbstractExecutorUptr child, TableHandle *tbl, std::list<IndexHandle *> indexes,
    std::vector<std::pair<RTField, ValueSptr>> updates, Context *context)
    : AbstractExecutor(DML),
      child_(std::move(child)),
      tbl_(tbl),
      indexes_(std::move(indexes)),
      updates_(std::move(updates)),
      context_(context),
      is_end_(false)
{
  std::vector<RTField> fields(1);
  fields[0]   = RTField{.field_ = {.field_name_ = "updated", .field_size_ = sizeof(int), .field_type_ = TYPE_INT}};
  out_schema_ = std::make_unique<RecordSchema>(fields);
}

void UpdateExecutor::Init()
{
  // Lab 02 may skip Lab 04's all-or-nothing unique-index validation.
  // 1. Build old/new record pairs.
  // 2. Validate the complete batch. NJUDB_STUDENT_TODO(l4, t2)
  // 3. Apply table and index updates atomically.
  is_end_ = false;
  updates_to_perform_.clear();
  auto *txn = context_ != nullptr ? context_->txn : nullptr;

  child_->Init();
  while (!child_->IsEnd()) {
    auto old_record = child_->GetRecord();
    auto values     = old_record->GetValues();
    for (auto &[field, value] : updates_) {
      const auto index = old_record->GetSchema()->GetRTFieldIndex(field);
      NJUDB_ASSERT(index < old_record->GetSchema()->GetFieldCount(),
          fmt::format("Unknown field in update: {}", field.field_.field_name_));
      values[index] = value;
    }
    auto new_record = std::make_unique<Record>(old_record->GetSchema(), values, old_record->GetRID());
    updates_to_perform_.emplace_back(std::move(old_record), std::move(new_record));
    child_->Next();
  }

  int count = 0;
  for (auto &[old_record, new_record] : updates_to_perform_) {
    if (txn != nullptr && txn->GetProtocol() == ConcurrencyProtocol::MVCC) {
      MVCCWriteCoordinator::Update(tbl_, indexes_, *old_record, *new_record, txn, context_->version_mgr_);
    } else {
      for (auto *index : indexes_) {
        index->UpdateRecord(*old_record, *new_record);
      }
      tbl_->UpdateRecord(new_record->GetRID(), *new_record, txn);
    }
    ++count;
  }

  std::vector<ValueSptr> values{ValueFactory::CreateIntValue(count)};
  record_ = std::make_unique<Record>(out_schema_.get(), values, INVALID_RID);
}

void UpdateExecutor::Next() { is_end_ = true; }

auto UpdateExecutor::IsEnd() const -> bool { return is_end_; }

}  // namespace njudb
