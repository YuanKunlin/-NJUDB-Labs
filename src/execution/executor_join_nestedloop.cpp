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

/// outer join
void NestedLoopJoinExecutor::InitOuterJoin() { NJUDB_STUDENT_TODO(l3, t2); }

void NestedLoopJoinExecutor::NextOuterJoin() { NJUDB_STUDENT_TODO(l3, t2); }

auto NestedLoopJoinExecutor::IsEndOuterJoin() const -> bool { return NJUDB_STUDENT_TODO(l3, t2); }

}  // namespace njudb
