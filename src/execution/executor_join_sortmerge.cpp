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
// Created by ziqi on 2024/8/7.
//

#include "executor_join_sortmerge.h"

namespace njudb {
SortMergeJoinExecutor::SortMergeJoinExecutor(JoinType join_type, AbstractExecutorUptr left, AbstractExecutorUptr right,
    RecordSchemaUptr left_key_schema, RecordSchemaUptr right_key_schema, CompOp join_op)
    // condition vec is not used in sort merge join, it has been converted to key schemas
    : JoinExecutor(join_type, std::move(left), std::move(right), {}),
      left_key_schema_(std::move(left_key_schema)),
      right_key_schema_(std::move(right_key_schema)),
      join_op_(join_op)
{}

auto SortMergeJoinExecutor::Compare(const Record &left, const Record &right) const -> int
{
  for (size_t i = 0; i < left_key_schema_->GetFieldCount(); ++i) {
    auto left_index  = left.GetSchema()->GetRTFieldIndex(left_key_schema_->GetFieldAt(i));
    auto right_index = right.GetSchema()->GetRTFieldIndex(right_key_schema_->GetFieldAt(i));
    NJUDB_ASSERT(left_index != left.GetSchema()->GetFieldCount(), "Invalid left join key field");
    NJUDB_ASSERT(right_index != right.GetSchema()->GetFieldCount(), "Invalid right join key field");
    auto left_value  = left.GetValueAt(left_index);
    auto right_value = right.GetValueAt(right_index);
    if (left_value->IsNull() || right_value->IsNull()) {
      if (left_value->IsNull() && right_value->IsNull()) {
        continue;
      }
      return left_value->IsNull() ? -1 : 1;
    }
    ValueFactory::AlignTypes(left_value, right_value);
    if (*left_value < *right_value) {
      return -1;
    }
    if (*left_value > *right_value) {
      return 1;
    }
  }
  return 0;
}

auto SortMergeJoinExecutor::EvaluateJoinCondition(const Record &left, const Record &right) const -> bool
{
  auto cmp = Compare(left, right);
  switch (join_op_) {
    case OP_EQ: return cmp == 0;
    case OP_LT: return cmp < 0;
    case OP_LE: return cmp <= 0;
    case OP_GT: return cmp > 0;
    case OP_GE: return cmp >= 0;
    default: NJUDB_FATAL("Unsupported join operator for sort merge join");
  }
}

void SortMergeJoinExecutor::FillEqualityBuffer()
{
  right_buffer_.clear();
  right_idx_ = 0;
  while (!right_->IsEnd()) {
    auto right_rec = right_->GetRecord();
    auto cmp       = Compare(*left_rec_, *right_rec);
    if (cmp > 0) {
      right_->Next();
      continue;
    }
    if (cmp < 0) {
      break;
    }
    right_buffer_.push_back(std::make_shared<Record>(*right_rec));
    right_->Next();
  }
}

void SortMergeJoinExecutor::InitJoin()
{
  switch (join_op_) {
    case OP_EQ: InitEqualityJoin(); return;
    case OP_LT:
    case OP_LE: InitInequalityJoinLessFamily(); return;
    case OP_GT:
    case OP_GE: InitInequalityJoinGreaterFamily(); return;
    default: NJUDB_FATAL("Unsupported join operator for sort merge join");
  }
}

void SortMergeJoinExecutor::NextJoin()
{
  switch (join_op_) {
    case OP_EQ: NextEqualityJoin(); return;
    case OP_LT:
    case OP_LE: NextInequalityJoinLessFamily(); return;
    case OP_GT:
    case OP_GE: NextInequalityJoinGreaterFamily(); return;
    default: NJUDB_FATAL("Unsupported join operator for sort merge join");
  }
}

void SortMergeJoinExecutor::InitEqualityJoin()
{
  left_->Init();
  right_->Init();
  left_rec_      = nullptr;
  right_idx_     = 0;
  need_gen_null_ = true;
  record_        = nullptr;
  right_buffer_.clear();
  if (left_->IsEnd()) {
    return;
  }
  left_rec_ = left_->GetRecord();
  FillEqualityBuffer();
  NextEqualityJoin(); 
}

void SortMergeJoinExecutor::NextEqualityJoin()
{
  while (left_rec_ != nullptr) {
    if (right_idx_ < right_buffer_.size()) {
      record_ = std::make_unique<Record>(out_schema_.get(), *left_rec_, *right_buffer_[right_idx_]);
      ++right_idx_;
      need_gen_null_ = false;
      return;
    }
    bool       gen_null_row = join_type_ == OUTER_JOIN && right_buffer_.empty() && need_gen_null_;
    RecordUptr null_row;
    if (gen_null_row) {
      Record null_right(right_->GetOutSchema());  // 所有字段为 NULL 的右表记录
      null_row = std::make_unique<Record>(out_schema_.get(), *left_rec_, null_right);
    }
    left_->Next();
    need_gen_null_ = true;
    right_idx_     = 0;
    if (left_->IsEnd()) {
      left_rec_ = nullptr;
    } else {
      left_rec_ = left_->GetRecord();
      if (right_buffer_.empty() || Compare(*left_rec_, *right_buffer_.front()) != 0) {
        FillEqualityBuffer();
      }
    }
    if (gen_null_row) {
      record_ = std::move(null_row);
      return;
    }
  }
  record_ = nullptr;
}

void SortMergeJoinExecutor::InitInequalityJoinLessFamily()
{
  left_->Init();
  right_->Init();
  left_rec_      = nullptr;
  right_idx_     = 0;
  need_gen_null_ = true;
  record_        = nullptr;
  right_buffer_.clear();
  if (left_->IsEnd()) {
    return;
  }
  left_rec_ = left_->GetRecord();
  while (!right_->IsEnd()) {
    auto right_rec = right_->GetRecord();
    if (!EvaluateJoinCondition(*left_rec_, *right_rec)) {
      break;
    }
    right_buffer_.push_back(std::make_shared<Record>(*right_rec));
    right_->Next();
  }
  NextInequalityJoinLessFamily();
}

void SortMergeJoinExecutor::NextInequalityJoinLessFamily()
{
  while (left_rec_ != nullptr) {
    if (right_idx_ < right_buffer_.size()) {
      record_ = std::make_unique<Record>(out_schema_.get(), *left_rec_, *right_buffer_[right_idx_]);
      ++right_idx_;
      need_gen_null_ = false;
      return;
    }
    bool       gen_null_row = join_type_ == OUTER_JOIN && right_buffer_.empty() && need_gen_null_;
    RecordUptr null_row;
    if (gen_null_row) {
      Record null_right(right_->GetOutSchema());
      null_row = std::make_unique<Record>(out_schema_.get(), *left_rec_, null_right);
    }
    left_->Next();
    need_gen_null_ = true;
    right_idx_     = 0;
    if (left_->IsEnd()) {
      left_rec_ = nullptr;
    } else {
      left_rec_ = left_->GetRecord();
      while (!right_buffer_.empty() && !EvaluateJoinCondition(*left_rec_, *right_buffer_.back())) {
        right_buffer_.pop_back();
      }
    }
    if (gen_null_row) {
      record_ = std::move(null_row);
      return;
    }
  }
  record_ = nullptr;
}

void SortMergeJoinExecutor::InitInequalityJoinGreaterFamily() { InitInequalityJoinLessFamily(); }

void SortMergeJoinExecutor::NextInequalityJoinGreaterFamily() { NextInequalityJoinLessFamily(); }

void SortMergeJoinExecutor::InitInnerJoin() { InitJoin(); }

void SortMergeJoinExecutor::NextInnerJoin() { NextJoin(); }

auto SortMergeJoinExecutor::IsEndInnerJoin() const -> bool { return record_ == nullptr; }

void SortMergeJoinExecutor::InitOuterJoin() { InitJoin(); }

void SortMergeJoinExecutor::NextOuterJoin() { NextJoin(); }

auto SortMergeJoinExecutor::IsEndOuterJoin() const -> bool { return record_ == nullptr; }

}  // namespace njudb
