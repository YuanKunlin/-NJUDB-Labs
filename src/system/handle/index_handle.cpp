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
// Created by ziqi on 2024/7/28.
//

#include "index_handle.h"
#include "../../../common/error.h"
#include <unordered_set>

namespace njudb {

namespace {

/// 把表记录投影成索引键记录
auto ProjectKey(Index *index, const Record &record) -> Record
{
  return Record(index->GetKeySchema(), record);
}

/// 唯一索引不接受 NULL 键
void EnsureNoNullKey(const Record &key, const std::string &index_name)
{
  for (size_t i = 0; i < key.GetSchema()->GetFieldCount(); ++i) {
    if (key.GetValueAt(i)->IsNull()) {
      NJUDB_THROW(NJUDB_INDEX_FAIL, fmt::format("NULL key is not allowed in index \"{}\"", index_name));
    }
  }
}

}  // namespace
IndexHandle::IndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, table_id_t tid,
    idx_id_t iid, IndexType index_type, RecordSchemaUptr key_schema, std::string index_name)
    : table_id_(tid), index_(nullptr), index_name_(std::move(index_name)), key_schema_holder_(std::move(key_schema))
{
  switch (index_type) {
    case IndexType::BPTREE: {
      // bpt_param = ParseBPTreeIndex();
      index_ = std::make_unique<BPTreeIndex>(disk_manager, buffer_pool_manager, iid, key_schema_holder_.get());
      break;
    }
    case IndexType::HASH: {
      // hash_param = ParseHashIndex();
      index_ = std::make_unique<HashIndex>(disk_manager, buffer_pool_manager, iid, key_schema_holder_.get());
      break;
    }
    default: NJUDB_FATAL(fmt::format("{}", static_cast<int>(index_type)));
  }
}

void IndexHandle::InsertRecord(const Record &rec)
{
  std::lock_guard<std::mutex> lock(index_latch_);
  if (index_ == nullptr) {
    NJUDB_FATAL("Index not initialized");
  }
  auto key = ProjectKey(index_.get(), rec);
  EnsureNoNullKey(key, index_name_);
  // 唯一索引：已有相同键的记录就拒绝
  if (!index_->Search(key).empty()) {
    NJUDB_THROW(NJUDB_INDEX_FAIL, fmt::format("Insert would violate unique constraint on index {}", index_name_));
  }
  index_->Insert(key, rec.GetRID());
}

void IndexHandle::DeleteRecord(const Record &rec)
{
  std::lock_guard<std::mutex> lock(index_latch_);
  if (index_ == nullptr) {
    NJUDB_FATAL("Index not initialized");
  }
  auto key = ProjectKey(index_.get(), rec);
  index_->Delete(key);
}

void IndexHandle::UpdateRecord(const Record &old_rec, const Record &new_rec)
{
  std::lock_guard<std::mutex> lock(index_latch_);
  if (index_ == nullptr) {
    NJUDB_FATAL("Index not initialized");
  }
  auto old_key = ProjectKey(index_.get(), old_rec);
  auto new_key = ProjectKey(index_.get(), new_rec);
  if (Record::Compare(old_key, new_key) == 0) {
    return;  // 索引键没有变化，不需要维护
  }
  EnsureNoNullKey(new_key, index_name_);
  // 键发生变化时，新键不能与其他记录冲突（自身 RID 除外）
  for (const auto &rid : index_->Search(new_key)) {
    if (rid != new_rec.GetRID()) {
      NJUDB_THROW(NJUDB_INDEX_FAIL, fmt::format("Update would violate unique constraint on index {}", index_name_));
    }
  }
  index_->Delete(old_key);
  index_->Insert(new_key, new_rec.GetRID());
}

auto IndexHandle::SearchRange(const Record &low_key, const Record &high_key) -> std::vector<RID>
{
  std::lock_guard<std::mutex> lock(index_latch_);
  if (index_ == nullptr) {
    NJUDB_FATAL("Index not initialized");
  }
  return index_->SearchRange(low_key, high_key);
}

auto IndexHandle::Search(const Record &key) -> std::vector<RID>
{
  std::lock_guard<std::mutex> lock(index_latch_);
  if (index_ == nullptr) {
    NJUDB_FATAL("Index not initialized");
  }

  return index_->Search(key);
}

auto IndexHandle::CheckRecordExists(const Record &record) -> bool
{
  std::lock_guard<std::mutex> lock(index_latch_);
  if (index_ == nullptr) {
    NJUDB_FATAL("Index not initialized");
  }
  auto key = ProjectKey(index_.get(), record);
  return !index_->Search(key).empty();
}

auto IndexHandle::PrintIndexStats() -> std::string
{
  if (index_ == nullptr) {
    return "Index not initialized";
  }

  std::string stats;
  stats += fmt::format("Index Type: {}\n", static_cast<int>(index_->GetIndexType()));
  stats += fmt::format("Index ID: {}\n", index_->GetIndexId());
  stats += fmt::format("Size: {}\n", index_->Size());
  stats += fmt::format("Height: {}\n", index_->GetHeight());
  stats += fmt::format("Empty: {}\n", index_->IsEmpty() ? "Yes" : "No");

  return stats;
}

IndexHandle::~IndexHandle() {}
}  // namespace njudb
