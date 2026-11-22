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
// Created by ziqi on 2024/8/4.
//

#include "executor_join_nestedloop.h"
#include "expr/condition_expr.h"

namespace njudb {
NestedLoopJoinExecutor::NestedLoopJoinExecutor(
    JoinType join_type, AbstractExecutorUptr left, AbstractExecutorUptr right, ConditionVec conditions)
    : JoinExecutor(join_type, std::move(left), std::move(right), std::move(conditions))
{}

/// inner join
void NestedLoopJoinExecutor::InitInnerJoin()
{
  left_->Init();
  right_->Init();
  left_rec_ = nullptr;
  record_   = nullptr;
  if (left_->IsEnd()) {
    return;
  }
  left_rec_ = left_->GetRecord();
  NextInnerJoin();
}

void NestedLoopJoinExecutor::NextInnerJoin()
{
  while (!left_->IsEnd()) {
    while (!right_->IsEnd()) {
      auto joined = std::make_unique<Record>(out_schema_.get(), *left_rec_, *right_->GetRecord());
      right_->Next();
      if (ConditionExpr::Eval(conditions_, *joined)) {
        record_ = std::move(joined);
        return;
      }
    }
    left_->Next();
    if (left_->IsEnd()) {
      break;
    }
    left_rec_ = left_->GetRecord();
    right_->Init();
  }
  record_ = nullptr;
}

auto NestedLoopJoinExecutor::IsEndInnerJoin() const -> bool { return record_ == nullptr; }

/// 外连接（本题只要求左外连接）
void NestedLoopJoinExecutor::InitOuterJoin()
{
  left_->Init();
  right_->Init();
  left_rec_      = nullptr;
  record_        = nullptr;
  need_gen_null_ = true;
  if (left_->IsEnd()) {
    return;  // 左表为空，没有可输出的记录
  }
  // 缓存当前左记录，并把输出定位到第一条结果
  left_rec_ = left_->GetRecord();
  NextOuterJoin();
}

void NestedLoopJoinExecutor::NextOuterJoin()
{
  while (!left_->IsEnd()) {
    while (!right_->IsEnd()) {
      auto joined = std::make_unique<Record>(out_schema_.get(), *left_rec_, *right_->GetRecord());
      right_->Next();
      if (ConditionExpr::Eval(conditions_, *joined)) {
        need_gen_null_ = false;  // 当前左行已有匹配，不再需要补 NULL 行
        record_        = std::move(joined);
        return;
      }
    }
    // 右表扫完：当前左行从未匹配时，输出一条右表全 NULL 的扩展行
    bool       gen_null_row = need_gen_null_;
    RecordUptr null_row;
    if (gen_null_row) {
      Record null_right(right_->GetOutSchema());  // 所有字段为 NULL 的右表记录
      null_row = std::make_unique<Record>(out_schema_.get(), *left_rec_, null_right);
    }
    // 返回前先推进到下一行左记录，避免同一条左行被重复输出
    left_->Next();
    need_gen_null_ = true;
    if (!left_->IsEnd()) {
      left_rec_ = left_->GetRecord();
      right_->Init();
    }
    if (gen_null_row) {
      record_ = std::move(null_row);
      return;
    }
  }
  record_ = nullptr;
}

auto NestedLoopJoinExecutor::IsEndOuterJoin() const -> bool { return record_ == nullptr; }

}  // namespace njudb