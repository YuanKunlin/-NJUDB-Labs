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
// Created by ziqi on 2024/8/9.
//

#include "executor_join_hash.h"
#include "expr/condition_expr.h"
#include "common/bloom_filter.h"
#include <functional>

namespace njudb {

namespace {

auto EmptyBucket() -> std::vector<std::shared_ptr<Record>> &
{
  static std::vector<std::shared_ptr<Record>> empty;
  return empty;
}

auto HasNullJoinKey(const Record &record, const RecordSchema *key_schema) -> bool
{
  for (size_t i = 0; i < key_schema->GetFieldCount(); ++i) {
    auto index = record.GetSchema()->GetRTFieldIndex(key_schema->GetFieldAt(i));
    NJUDB_ASSERT(index != record.GetSchema()->GetFieldCount(), "Invalid join key field");
    if (record.GetValueAt(index)->IsNull()) {
      return true;
    }
  }
  return false;
}

auto HashKeyValue(const ValueSptr &value) -> size_t
{
  switch (value->GetType()) {
    case FieldType::TYPE_BOOL: return std::hash<bool>{}(std::dynamic_pointer_cast<BoolValue>(value)->Get());
    case FieldType::TYPE_INT: return std::hash<int32_t>{}(std::dynamic_pointer_cast<IntValue>(value)->Get());
    case FieldType::TYPE_FLOAT: return std::hash<float>{}(std::dynamic_pointer_cast<FloatValue>(value)->Get());
    case FieldType::TYPE_STRING: return std::hash<std::string>{}(std::dynamic_pointer_cast<StringValue>(value)->Get());
    default: NJUDB_FATAL("Unsupported join key type");
  }
}

auto CombineHash(size_t hash, size_t value_hash) -> size_t
{
  return hash ^ (value_hash + static_cast<size_t>(0x9e3779b97f4a7c15ULL) + (hash << 6) + (hash >> 2));
}

}  // namespace

// ===== HashJoinExecutor Implementation =====

HashJoinExecutor::HashJoinExecutor(JoinType join_type, AbstractExecutorUptr left, AbstractExecutorUptr right,
                                   RecordSchemaUptr left_key_schema, RecordSchemaUptr right_key_schema, ConditionVec conditions, bool use_bloom_filter)
    : JoinExecutor(join_type, std::move(left), std::move(right), std::move(conditions)),
      left_key_schema_(std::move(left_key_schema)),
      right_key_schema_(std::move(right_key_schema)),
      use_bloom_filter_(use_bloom_filter),
      is_probing_(false),
      current_left_has_match_(false),
      need_output_null_match_(false),
      build_records_(0),
      probe_records_(0)
{
  if (use_bloom_filter_) {
    bloom_filter_ = std::make_unique<BloomFilter>(8192, 3);
  }
}

void HashJoinExecutor::BuildHashTable()
{
  hash_table_.clear();
  build_records_ = 0;
  if (use_bloom_filter_) {
    bloom_filter_->Clear();
  }
  right_->Init();
  while (!right_->IsEnd()) {
    auto record = right_->GetRecord();
    if (!HasNullJoinKey(*record, right_key_schema_.get())) {
      auto hash = ComputeHash(*record, false);
      hash_table_[hash].push_back(std::make_shared<Record>(*record));
      if (use_bloom_filter_) {
        bloom_filter_->Insert(hash);
      }
      ++build_records_;
    }
    right_->Next();
  }
}

void HashJoinExecutor::StartProbing()
{
  left_->Init();
  is_probing_             = true;
  current_left_record_    = nullptr;
  current_left_has_match_ = false;
  need_output_null_match_ = false;
  probe_records_          = 0;
  if (left_->IsEnd()) {
    return;
  }
  current_left_record_ = left_->GetRecord();
}

void HashJoinExecutor::LocateMatches()
{
  current_match_iter_ = EmptyBucket().begin();
  current_match_end_  = EmptyBucket().end();
  if (current_left_record_ == nullptr) {
    return;
  }
  if (HasNullJoinKey(*current_left_record_, left_key_schema_.get())) {
    return;
  }
  auto hash = ComputeHash(*current_left_record_, true);
  if (use_bloom_filter_ && !bloom_filter_->MightContain(hash)) {
    return;
  }
  auto bucket = hash_table_.find(hash);
  if (bucket == hash_table_.end()) {
    return;
  }
  current_match_iter_ = bucket->second.begin();
  current_match_end_  = bucket->second.end();
  ++probe_records_;
}

auto HashJoinExecutor::ComputeHash(const Record &record, bool is_left_side) const -> size_t
{
  const auto *key_schema = is_left_side ? left_key_schema_.get() : right_key_schema_.get();
  size_t      hash       = 0;
  for (size_t i = 0; i < key_schema->GetFieldCount(); ++i) {
    auto index = record.GetSchema()->GetRTFieldIndex(key_schema->GetFieldAt(i));
    NJUDB_ASSERT(index != record.GetSchema()->GetFieldCount(), "Invalid join key field");
    hash = CombineHash(hash, HashKeyValue(record.GetValueAt(index)));
  }
  return hash;
}

auto HashJoinExecutor::CheckJoinCondition(const Record &left_record, const Record &right_record) const -> bool
{
  if (conditions_.empty()) {
    return true;
  }
  Record joined(out_schema_.get(), left_record, right_record);
  return ConditionExpr::Eval(conditions_, joined);
}

void HashJoinExecutor::InitInnerJoin()
{
  BuildHashTable();
  StartProbing();
  record_ = nullptr;
  if (current_left_record_ == nullptr) {
    return;  // 左表为空：没有连接结果
  }
  LocateMatches();
  NextInnerJoin();
}

void HashJoinExecutor::NextInnerJoin()
{
  while (current_left_record_ != nullptr) {
    while (current_match_iter_ != current_match_end_) {
      auto candidate = *current_match_iter_;
      ++current_match_iter_;
      if (CheckJoinCondition(*current_left_record_, *candidate)) {
        record_ = std::make_unique<Record>(out_schema_.get(), *current_left_record_, *candidate);
        return;
      }
    }
    left_->Next();
    if (left_->IsEnd()) {
      current_left_record_ = nullptr;
      break;
    }
    current_left_record_ = left_->GetRecord();
    LocateMatches();
  }
  record_ = nullptr;
}

auto HashJoinExecutor::IsEndInnerJoin() const -> bool
{
  return record_ == nullptr;
}

void HashJoinExecutor::InitOuterJoin()
{
  BuildHashTable();
  StartProbing();
  record_                  = nullptr;
  current_left_has_match_ = false;
  if (current_left_record_ == nullptr) {
    return;
  }
  LocateMatches();
  NextOuterJoin();
}

void HashJoinExecutor::NextOuterJoin()
{
  while (current_left_record_ != nullptr) {
    while (current_match_iter_ != current_match_end_) {
      auto candidate = *current_match_iter_;
      ++current_match_iter_;
      if (CheckJoinCondition(*current_left_record_, *candidate)) {
        current_left_has_match_ = true;
        record_ = std::make_unique<Record>(out_schema_.get(), *current_left_record_, *candidate);
        return;
      }
    }
    bool       gen_null_row = !current_left_has_match_;
    RecordUptr null_row;
    if (gen_null_row) {
      Record null_right(right_->GetOutSchema());
      null_row = std::make_unique<Record>(out_schema_.get(), *current_left_record_, null_right);
    }
    left_->Next();
    current_left_has_match_ = false;
    if (left_->IsEnd()) {
      current_left_record_ = nullptr;
    } else {
      current_left_record_ = left_->GetRecord();
      LocateMatches();
    }
    if (gen_null_row) {
      record_ = std::move(null_row);
      return;
    }
  }
  record_ = nullptr;
}

auto HashJoinExecutor::IsEndOuterJoin() const -> bool
{
  return record_ == nullptr;
}

}  // namespace njudb
