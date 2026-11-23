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

#include "executor_insert.h"
#include "concurrency/lock_manager.h"
#include "concurrency/version_manager.h"
#include "concurrency/concurrency_protocol.h"
#include "mvcc_write_coordinator.h"
#include <fmt/format.h>
#include <unordered_set>

namespace njudb {

InsertExecutor::InsertExecutor(TableHandle *tbl, std::list<IndexHandle *> indexes, std::vector<RecordUptr> inserts, Context *context)
    : AbstractExecutor(DML), tbl_(tbl), indexes_(std::move(indexes)), inserts_(std::move(inserts)), context_(context), is_end_(false)
{
  std::vector<RTField> fields(1);
  fields[0]   = RTField{.field_ = {.field_name_ = "inserted", .field_size_ = sizeof(int), .field_type_ = TYPE_INT}};
  out_schema_ = std::make_unique<RecordSchema>(fields);
}

void InsertExecutor::Init()
{
  // Lab04：先完成唯一性校验，再执行任何物理写入，保证一批 INSERT 要么全成要么全失败
  for (auto *index : indexes_) {
    std::unordered_set<Record> batch_keys;
    for (const auto &record : inserts_) {
      Record key(&index->GetKeySchema(), *record);
      for (size_t i = 0; i < key.GetSchema()->GetFieldCount(); ++i) {
        if (key.GetValueAt(i)->IsNull()) {
          NJUDB_THROW(NJUDB_INDEX_FAIL, fmt::format("NULL key is not allowed in index \"{}\"", index->GetIndexName()));
        }
      }
      // 1. 与已有索引项冲突
      if (index->CheckRecordExists(*record)) {
        NJUDB_THROW(NJUDB_INDEX_FAIL, fmt::format("Insert would violate unique constraint on index {}", index->GetIndexName()));
      }
      // 2. 本批次内部重复
      if (!batch_keys.insert(std::move(key)).second) {
        NJUDB_THROW(NJUDB_INDEX_FAIL,
            fmt::format("Insert would violate unique constraint on index {}", index->GetIndexName()));
      }
    }
  }

  int  count = 0;
  auto *txn   = context_ != nullptr ? context_->txn : nullptr;
  is_end_     = false;
  for (auto &record : inserts_) {
    if (txn != nullptr && txn->GetProtocol() == ConcurrencyProtocol::MVCC) {
      MVCCWriteCoordinator::Insert(tbl_, indexes_, *record, txn, context_->version_mgr_);
    } else {
      const RID rid = tbl_->InsertRecord(*record, txn);
      record->SetRID(rid);
      for (auto *index : indexes_) {
        index->InsertRecord(*record);
      }
    }
    ++count;
  }
  std::vector<ValueSptr> values{ValueFactory::CreateIntValue(count)};
  record_ = std::make_unique<Record>(out_schema_.get(), values, INVALID_RID);
}

void InsertExecutor::Next() { is_end_ = true; }

auto InsertExecutor::IsEnd() const -> bool { return is_end_; }

}  // namespace njudb
