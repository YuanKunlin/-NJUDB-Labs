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

#include "executor_idxscan.h"
#include "concurrency/lock_manager.h"
#include "concurrency/version_manager.h"
#include "concurrency/concurrency_protocol.h"
#include "common/value.h"
#include "expr/condition_expr.h"
#include <algorithm>

namespace njudb {

IdxScanExecutor::IdxScanExecutor(TableHandle *tbl, IndexHandle *idx, ConditionVec conds, bool is_ascending, Context *context)
    : AbstractExecutor(Basic), tbl_(tbl), idx_(idx), conds_(std::move(conds)), context_(context), is_ascending_(is_ascending)
{
  // Generate low and high keys using conditions
  GenerateRangeKeys();
}

void IdxScanExecutor::GenerateRangeKeys()
{
  needs_first_record_check_ = false;
  needs_last_record_check_  = false;

  const RecordSchema &key_schema  = idx_->GetKeySchema();
  const size_t        field_count = key_schema.GetFieldCount();

  // 每个键字段上收集：等值、下界（> / >=）、上界（< / <=）
  struct Bound
  {
    bool      has_{false};
    ValueSptr value_;
    CompOp    op_{OP_EQ};
  };
  std::vector<Bound> equality(field_count), lower(field_count), upper(field_count);

  for (const auto &cond : conds_) {
    // 索引扫描只处理「列 op 常量」形式的谓词
    if (cond.GetRhsType() != kValue) {
      continue;
    }
    const auto &lcol = cond.GetLCol();
    size_t      pos  = field_count;
    for (size_t i = 0; i < field_count; ++i) {
      const auto &field = key_schema.GetFieldAt(i).field_;
      if (field.table_id_ == lcol.field_.table_id_ && field.field_name_ == lcol.field_.field_name_) {
        pos = i;
        break;
      }
    }
    if (pos == field_count) {
      continue;
    }
    auto value = cond.GetRVal();
    switch (cond.GetOp()) {
      case OP_EQ: equality[pos] = Bound{true, value, OP_EQ}; break;
      case OP_GT:
      case OP_GE: {
        auto &bound = lower[pos];
        if (!bound.has_) {
          bound = Bound{true, value, cond.GetOp()};
        } else {
          // 取更紧的下界：值更大者优先；同值时 > 比 >= 更紧
          ValueSptr cur = bound.value_, cand = value;
          ValueFactory::AlignTypes(cur, cand);
          if (*cand > *cur || (!(*cand < *cur) && cond.GetOp() == OP_GT && bound.op_ == OP_GE)) {
            bound = Bound{true, value, cond.GetOp()};
          }
        }
        break;
      }
      case OP_LT:
      case OP_LE: {
        auto &bound = upper[pos];
        if (!bound.has_) {
          bound = Bound{true, value, cond.GetOp()};
        } else {
          // 取更紧的上界：值更小者优先；同值时 < 比 <= 更紧
          ValueSptr cur = bound.value_, cand = value;
          ValueFactory::AlignTypes(cur, cand);
          if (*cand < *cur || (!(*cand < *cur) && !(*cur < *cand) && cond.GetOp() == OP_LT && bound.op_ == OP_LE)) {
            bound = Bound{true, value, cond.GetOp()};
          }
        }
        break;
      }
      default: break;
    }
  }

  // 按最左前缀构造 [low, high]：等值字段两侧相同；第一个非等值字段放范围，其余字段用最小/最大填充
  std::vector<ValueSptr> low_values, high_values;
  low_values.reserve(field_count);
  high_values.reserve(field_count);
  bool range_started = false;
  for (size_t i = 0; i < field_count; ++i) {
    const auto type = key_schema.GetFieldAt(i).field_.field_type_;
    if (!range_started && equality[i].has_) {
      low_values.push_back(equality[i].value_);
      high_values.push_back(equality[i].value_);
      continue;
    }
    if (!range_started) {
      range_started = true;
      if (lower[i].has_) {
        low_values.push_back(lower[i].value_);
        needs_first_record_check_ = (lower[i].op_ == OP_GT);  // 开区间下界要二次过滤
      } else {
        low_values.push_back(ValueFactory::CreateMinValueForType(type));
      }
      if (upper[i].has_) {
        high_values.push_back(upper[i].value_);
        needs_last_record_check_ = (upper[i].op_ == OP_LT);  // 开区间上界要二次过滤
      } else {
        high_values.push_back(ValueFactory::CreateMaxValueForType(type));
      }
      continue;
    }
    low_values.push_back(ValueFactory::CreateMinValueForType(type));
    high_values.push_back(ValueFactory::CreateMaxValueForType(type));
  }

  low_  = std::make_unique<Record>(&key_schema, low_values, INVALID_RID);
  high_ = std::make_unique<Record>(&key_schema, high_values, INVALID_RID);
}

void IdxScanExecutor::Init()
{
  // 2PL：先申请表级意向共享锁，与 TableHandle::GetRecord 的元组 S 锁配合
  if (context_ != nullptr && context_->txn != nullptr && context_->lock_mgr_ != nullptr &&
      context_->txn->GetProtocol() == ConcurrencyProtocol::TWO_PHASE_LOCKING) {
    (void)context_->lock_mgr_->LockTable(context_->txn, LockMode::INTENTION_SHARED, tbl_->GetTableId());
  }

  rids_ = idx_->SearchRange(*low_, *high_);
  if (!is_ascending_) {
    // SearchRange 按索引键升序返回；降序扫描要按键降序重排 RID
    std::vector<std::pair<Record, RID>> entries;
    entries.reserve(rids_.size());
    for (const auto &rid : rids_) {
      auto record = GetRecordWithProtocol(rid);
      if (record == nullptr) {
        continue;
      }
      entries.emplace_back(Record(&idx_->GetKeySchema(), *record), rid);
    }
    std::sort(entries.begin(), entries.end(),
        [](const auto &lhs, const auto &rhs) { return Record::Compare(lhs.first, rhs.first) > 0; });
    rids_.clear();
    rids_.reserve(entries.size());
    for (auto &entry : entries) {
      rids_.push_back(entry.second);
    }
  }

  start_idx_   = 0;
  end_idx_     = rids_.size();
  current_idx_ = 0;
  record_      = nullptr;
  LoadNextVisibleRecord();
}

void IdxScanExecutor::LoadNextVisibleRecord()
{
  while (current_idx_ < end_idx_) {
    auto record = GetRecordWithProtocol(rids_[current_idx_]);
    if (record != nullptr) {
      // 索引范围是闭区间；开区间边界以及复合索引的残差条件在这里精确过滤
      if (conds_.empty() || ConditionExpr::Eval(conds_, *record)) {
        record_ = std::move(record);
        return;
      }
    }
    ++current_idx_;
  }
  record_ = nullptr;
}

void IdxScanExecutor::Next()
{
  if (IsEnd()) {
    return;
  }
  ++current_idx_;
  LoadNextVisibleRecord();
}

auto IdxScanExecutor::IsEnd() const -> bool { return current_idx_ >= end_idx_; }

auto IdxScanExecutor::GetOutSchema() const -> const RecordSchema * { return &tbl_->GetSchema(); }

auto IdxScanExecutor::GetRecordWithProtocol(const RID &rid) -> RecordUptr
{
  try {
    return tbl_->GetRecord(rid, context_ != nullptr ? context_->txn : nullptr);
  } catch (const NJUDBException_ &e) {
    if (e.type_ == NJUDB_RECORD_MISS) {
      return nullptr;
    }
    throw;
  }
}

}  // namespace njudb
