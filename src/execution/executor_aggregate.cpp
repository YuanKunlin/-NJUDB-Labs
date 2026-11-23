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
// Created by ziqi on 2024/8/5.
//

#include "executor_aggregate.h"

namespace njudb {

namespace {

auto GetAggregateSourceValue(const Record &record, const RTField &agg_field) -> ValueSptr
{
  auto index = record.GetSchema()->GetFieldIndex(agg_field.field_.table_id_, agg_field.field_.field_name_);
  NJUDB_ASSERT(index != record.GetSchema()->GetFieldCount(), "Invalid field for aggregate");
  return record.GetValueAt(index);
}

auto MakeGroupKey(const RecordSchema *group_schema, const Record &record) -> Record
{
  std::vector<ValueSptr> values;
  values.reserve(group_schema->GetFieldCount());
  for (size_t i = 0; i < group_schema->GetFieldCount(); ++i) {
    const auto &field = group_schema->GetFieldAt(i);
    auto        index = record.GetSchema()->GetFieldIndex(field.field_.table_id_, field.field_.field_name_);
    NJUDB_ASSERT(index != record.GetSchema()->GetFieldCount(), "Invalid group by field");
    values.push_back(record.GetValueAt(index));
  }
  return Record(group_schema, values, INVALID_RID);
}

}  // namespace

AggregateExecutor::AggregateValue::AggregateValue(RecordSchema *schema) : schema_(schema)
{
  values_.reserve(schema_->GetFieldCount());
  for (size_t i = 0; i < schema_->GetFieldCount(); ++i) {
    const auto &field = schema_->GetFieldAt(i);
    switch (field.agg_type_) {
      case AGG_COUNT_STAR:
      case AGG_COUNT: values_.push_back(ValueFactory::CreateIntValue(0)); break;
      case AGG_SUM:
      case AGG_MAX:
      case AGG_MIN: values_.push_back(ValueFactory::CreateNullValue(field.field_.field_type_)); break;
      case AGG_AVG:
        values_.push_back(ValueFactory::CreateNullValue(field.field_.field_type_));
        avg_count_map_[i] = 0;
        break;
      default: NJUDB_FATAL("Unsupported aggregate type");
    }
  }
}

AggregateExecutor::AggregateValue::AggregateValue(RecordSchema *schema, const Record &record) : schema_(schema)
{
  values_.reserve(schema_->GetFieldCount());
  for (size_t i = 0; i < schema_->GetFieldCount(); ++i) {
    const auto &field = schema_->GetFieldAt(i);
    switch (field.agg_type_) {
      case AGG_COUNT_STAR: values_.push_back(ValueFactory::CreateIntValue(1)); break;
      case AGG_COUNT: {
        auto value = GetAggregateSourceValue(record, field);
        values_.push_back(ValueFactory::CreateIntValue(value->IsNull() ? 0 : 1));
        break;
      }
      case AGG_AVG: {
        auto value = GetAggregateSourceValue(record, field);
        if (value->IsNull()) {
          values_.push_back(ValueFactory::CreateNullValue(field.field_.field_type_));
          avg_count_map_[i] = 0;
        } else {
          values_.push_back(value);
          avg_count_map_[i] = 1;
        }
        break;
      }
      case AGG_SUM:
      case AGG_MAX:
      case AGG_MIN: {
        auto value = GetAggregateSourceValue(record, field);
        values_.push_back(value->IsNull() ? ValueFactory::CreateNullValue(field.field_.field_type_) : value);
        break;
      }
      default: NJUDB_FATAL("Unsupported aggregate type");
    }
  }
}

void AggregateExecutor::AggregateValue::CombineWith(const AggregateExecutor::AggregateValue &other)
{
  if (schema_ == nullptr) {
    schema_        = other.schema_;
    values_        = other.values_;
    avg_count_map_ = other.avg_count_map_;
    summarized_    = other.summarized_;
    return;
  }
  NJUDB_ASSERT(schema_->GetFieldCount() == other.values_.size(), "Aggregate value schema mismatch");
  for (size_t i = 0; i < schema_->GetFieldCount(); ++i) {
    const auto &rhs = other.values_[i];
    switch (schema_->GetFieldAt(i).agg_type_) {
      case AGG_COUNT_STAR:
      case AGG_COUNT:
      case AGG_SUM: {
        if (rhs->IsNull()) {
          break;
        }
        if (values_[i]->IsNull()) {
          values_[i] = rhs;
        } else {
          *values_[i] += *rhs;
        }
        break;
      }
      case AGG_AVG: {
        if (!rhs->IsNull()) {
          if (values_[i]->IsNull()) {
            values_[i] = rhs;
          } else {
            *values_[i] += *rhs;
          }
        }
        auto it = other.avg_count_map_.find(i);
        if (it != other.avg_count_map_.end()) {
          avg_count_map_[i] += it->second;
        }
        break;
      }
      case AGG_MAX: {
        if (rhs->IsNull()) {
          break;
        }
        if (values_[i]->IsNull() || *rhs > *values_[i]) {
          values_[i] = rhs;
        }
        break;
      }
      case AGG_MIN: {
        if (rhs->IsNull()) {
          break;
        }
        if (values_[i]->IsNull() || *rhs < *values_[i]) {
          values_[i] = rhs;
        }
        break;
      }
      default: NJUDB_FATAL("Unsupported aggregate type");
    }
  }
}

auto AggregateExecutor::AggregateValue::Values() const -> const std::vector<ValueSptr> & { return values_; }

void AggregateExecutor::AggregateValue::Finalize()
{
  if (summarized_) {
    return;
  }
  for (size_t i = 0; i < schema_->GetFieldCount(); ++i) {
    const auto &field = schema_->GetFieldAt(i);
    if (field.agg_type_ != AGG_AVG) {
      continue;
    }
    auto it    = avg_count_map_.find(i);
    auto count = it == avg_count_map_.end() ? 0 : it->second;
    if (count == 0 || values_[i]->IsNull()) {
      values_[i] = ValueFactory::CreateNullValue(field.field_.field_type_);
      continue;
    }
    *values_[i] /= count;  // 框架为 avg 预留的除法：int 列整除，float 列浮点除
  }
  summarized_ = true;
}

AggregateExecutor::AggregateExecutor(
    AbstractExecutorUptr child, RecordSchemaUptr agg_schema, RecordSchemaUptr group_schema)
    : AbstractExecutor(Basic),
      child_(std::move(child)),
      agg_schema_(std::move(agg_schema)),
      group_schema_(std::move(group_schema))
{
  std::vector<RTField> fields;
  for (const auto &field : group_schema_->GetFields()) {
    fields.push_back(field);
  }
  for (const auto &field : agg_schema_->GetFields()) {
    fields.push_back(field);
  }
  out_schema_ = std::make_unique<RecordSchema>(fields);
}

void AggregateExecutor::Init()
{
  child_->Init();
  group_map_.clear();
  record_ = nullptr;
  while (!child_->IsEnd()) {
    auto record           = child_->GetRecord();
    auto [iter, inserted] =
        group_map_.try_emplace(MakeGroupKey(group_schema_.get(), *record), agg_schema_.get(), *record);
    if (!inserted) {
      iter->second.CombineWith(AggregateValue(agg_schema_.get(), *record));
    }
    child_->Next();
  }
  if (group_map_.empty() && group_schema_->GetFieldCount() == 0) {
    auto empty_value = AggregateValue(agg_schema_.get());
    empty_value.Finalize();
    bool has_valid_value = false;
    for (const auto &value : empty_value.Values()) {
      if (!value->IsNull()) {
        has_valid_value = true;
        break;
      }
    }
    if (has_valid_value) {
      group_map_.try_emplace(Record(group_schema_.get(), std::vector<ValueSptr>{}, INVALID_RID), std::move(empty_value));
    }
  }
  for (auto &entry : group_map_) {
    entry.second.Finalize();
  }
  group_iter_ = group_map_.begin();
  Next();  // 定位到第一个分组
}

void AggregateExecutor::Next()
{
  record_ = nullptr;
  if (group_iter_ == group_map_.end()) {
    return;
  }
  std::vector<ValueSptr> values = group_iter_->first.GetValues();
  for (const auto &value : group_iter_->second.Values()) {
    values.push_back(value);
  }
  record_ = std::make_unique<Record>(out_schema_.get(), values, INVALID_RID);
  ++group_iter_;
}

auto AggregateExecutor::IsEnd() const -> bool { return record_ == nullptr; }

}  // namespace njudb
