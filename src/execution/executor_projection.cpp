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

#include "executor_projection.h"

namespace njudb {

ProjectionExecutor::ProjectionExecutor(AbstractExecutorUptr child, RecordSchemaUptr proj_schema)
    : AbstractExecutor(Basic), child_(std::move(child))
{
  out_schema_ = std::move(proj_schema);
}

void ProjectionExecutor::Init()
{
  child_->Init();
  record_ = nullptr;
  if (!child_->IsEnd()) {
    record_ = std::make_unique<Record>(out_schema_.get(), *child_->GetRecord());
  }
}

void ProjectionExecutor::Next()
{
  if (IsEnd()) {
    return;
  }
  child_->Next();
  if (child_->IsEnd()) {
    record_ = nullptr;
    return;
  }
  record_ = std::make_unique<Record>(out_schema_.get(), *child_->GetRecord());
}

auto ProjectionExecutor::IsEnd() const -> bool { return record_ == nullptr; }

}  // namespace njudb