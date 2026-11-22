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
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <limits>
#include <queue>
#include <vector>
#include "common/bitmap.h"
#include "common/config.h"
#include "executor_sort.h"

static long long sort_result_fresh_id_ = 0;
#define SORT_FILE_PATH(obj_name) FILE_NAME(TMP_DIR, obj_name, TMP_SUFFIX)

namespace njudb {

namespace {

constexpr size_t RID_BYTES = sizeof(page_id_t) + sizeof(slot_id_t);

// a run may already hold several chunks after the first merge pass, so the
// per-file bound must not be max_rec_num_; EOF decides where a run ends
constexpr size_t kUnboundedRecords = std::numeric_limits<size_t>::max();

/// run files store: [page_id][slot_id][nullmap][record data]
void WriteRecordTo(std::ostream &out, const Record &record)
{
  const auto page = record.GetRID().PageID();
  const auto slot = record.GetRID().SlotID();
  out.write(reinterpret_cast<const char *>(&page), sizeof(page));
  out.write(reinterpret_cast<const char *>(&slot), sizeof(slot));
  const auto nullmap_size = static_cast<std::streamsize>(BITMAP_SIZE(record.GetSchema()->GetFieldCount()));
  out.write(record.GetNullMap(), nullmap_size);
  out.write(record.GetData(), static_cast<std::streamsize>(record.GetSchema()->GetRecordLength()));
}

}  // namespace

SortExecutor::SortExecutor(AbstractExecutorUptr child, RecordSchemaUptr key_schema, bool is_desc)
    : AbstractExecutor(Basic),
      child_(std::move(child)),
      key_schema_(std::move(key_schema)),
      buf_idx_(0),
      is_desc_(is_desc),
      is_sorted_(false),
      is_merge_sort_(false),
      max_rec_num_(SORT_BUFFER_SIZE / child_->GetOutSchema()->GetRecordLength()),
      tmp_file_num_(0),
      merge_result_file_(fmt::format("sort_result_{}_{}", getpid(), sort_result_fresh_id_++))
{
  // For debugging, you can uncomment this line for more frequent sort and merge
  //  max_rec_num_ = 10;
  // test hook: force the external merge path with a small in-memory limit
  if (const char *limit = std::getenv("NJUDB_SORT_MAX_REC")) {
    max_rec_num_ = std::max<size_t>(2, std::strtoul(limit, nullptr, 10));
  }
}

SortExecutor::~SortExecutor()
{
  if (!is_merge_sort_) {
    return;
  }
  if (merge_result_file_handle_ != nullptr && merge_result_file_handle_->is_open()) {
    merge_result_file_handle_->close();
  }
  if (!merge_result_path_.empty()) {
    std::error_code ec;
    std::filesystem::remove(merge_result_path_, ec);
    merge_result_path_.clear();
  }
}

void SortExecutor::Init()
{
  is_merge_sort_ = false;
  is_sorted_     = false;
  buf_idx_       = 0;
  tmp_file_num_  = 0;
  child_->Init();

  auto fill_buffer = [this] {
    sort_buffer_.clear();
    while (!child_->IsEnd() && sort_buffer_.size() < max_rec_num_) {
      sort_buffer_.push_back(child_->GetRecord());  // a copy; the child reuses its record_
      child_->Next();
    }
  };

  fill_buffer();
  if (child_->IsEnd()) {
    // the whole table fits in memory: sort in place
    SortBuffer();
    is_sorted_ = true;
    record_    = sort_buffer_.empty() ? nullptr : std::make_unique<Record>(*sort_buffer_.front());
    return;
  }

  // more rows than the buffer can hold: sort run by run, then merge them
  is_merge_sort_ = true;
  do {
    SortBuffer();
    DumpBufferToFile(tmp_file_num_++);
    fill_buffer();
  } while (!sort_buffer_.empty());

  Merge();
  LoadMergeResult();
  buf_idx_   = 0;
  is_sorted_ = true;
  record_    = sort_buffer_.empty() ? nullptr : std::make_unique<Record>(*sort_buffer_.front());
}

void SortExecutor::Next()
{
  if (IsEnd()) {
    return;
  }
  ++buf_idx_;
  record_ = buf_idx_ < sort_buffer_.size() ? std::make_unique<Record>(*sort_buffer_[buf_idx_]) : nullptr;
}

auto SortExecutor::IsEnd() const -> bool { return buf_idx_ >= sort_buffer_.size(); }

auto SortExecutor::Compare(const Record &lhs, const Record &rhs) const -> bool
{
  auto lkey = std::make_unique<Record>(key_schema_.get(), lhs);
  auto rkey = std::make_unique<Record>(key_schema_.get(), rhs);
  return is_desc_ ? Record::Compare(*lkey, *rkey) > 0 : lkey->Compare(*lkey, *rkey) < 0;
}

auto SortExecutor::GetOutSchema() const -> const RecordSchema * { return child_->GetOutSchema(); }

/// methods below are only used for merge sort

auto SortExecutor::GetSortFileName(size_t file_group, size_t file_idx) const -> std::string
{
  return fmt::format("{}_{}_{}", merge_result_file_, file_group, file_idx);
}

// sort the records according to the key schema in the buffer
void SortExecutor::SortBuffer()
{
  std::sort(sort_buffer_.begin(), sort_buffer_.end(),
      [this](const RecordUptr &lhs, const RecordUptr &rhs) { return Compare(*lhs, *rhs); });
}

// dump the sorted buffer to the file denoted by file_idx
void SortExecutor::DumpBufferToFile(size_t file_idx)
{
  std::ofstream out(GetSortFileName(0, file_idx), std::ios::binary | std::ios::trunc);
  NJUDB_ASSERT(out.is_open(), fmt::format("cannot open sort run file {}", GetSortFileName(0, file_idx)));
  for (const auto &record : sort_buffer_) {
    WriteRecordTo(out, *record);
  }
  out.close();
}

// load sort result from merge_result_file_handle_ into buffer
void SortExecutor::LoadMergeResult()
{
  NJUDB_ASSERT(merge_result_file_handle_ != nullptr, "merge result file is nullptr");
  NJUDB_ASSERT(merge_result_file_handle_->is_open(), "merge result file is not open");

  sort_buffer_.clear();
  const auto *schema        = child_->GetOutSchema();
  const auto  nullmap_size  = BITMAP_SIZE(schema->GetFieldCount());
  auto       &in            = *merge_result_file_handle_;

  while (true) {
    page_id_t page{};
    slot_id_t slot{};
    in.read(reinterpret_cast<char *>(&page), sizeof(page));
    if (!in) {
      break;
    }
    in.read(reinterpret_cast<char *>(&slot), sizeof(slot));
    std::vector<char> nullmap(nullmap_size);
    std::vector<char> data(schema->GetRecordLength());
    in.read(nullmap.data(), static_cast<std::streamsize>(nullmap.size()));
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!in) {
      break;
    }
    sort_buffer_.push_back(std::make_unique<Record>(schema, nullmap.data(), data.data(), RID(page, slot)));
  }
  in.close();
}

void SortExecutor::Merge()
{
  const auto *schema   = child_->GetOutSchema();
  size_t      group    = 0;
  size_t      file_num = tmp_file_num_;

  // one pass merges up to SORT_WAY_NUM runs of the current group into one run of
  // the next group; repeat until a single run is left
  while (file_num > 1) {
    const size_t next_group = group + 1;
    size_t       next_num   = 0;

    for (size_t first = 0; first < file_num; first += SORT_WAY_NUM) {
      const size_t run_num = std::min(SORT_WAY_NUM, file_num - first);

      // min-heap on the sort key: Compare is "lhs before rhs", so invert it
      auto cmp = [this](const SortHeapNode &lhs, const SortHeapNode &rhs) {
        return Compare(*rhs.GetRecord(), *lhs.GetRecord());
      };
      std::priority_queue<SortHeapNode, std::vector<SortHeapNode>, decltype(cmp)> heap(cmp);

      for (size_t i = 0; i < run_num; ++i) {
        auto handle = std::make_shared<std::ifstream>(GetSortFileName(group, first + i), std::ios::binary);
        NJUDB_ASSERT(handle->is_open(), fmt::format("cannot open run {}", GetSortFileName(group, first + i)));
        SortHeapNode node(handle, schema, 0);
        if (node.LoadNextRecord(kUnboundedRecords)) {
          heap.push(std::move(node));
        }
      }

      std::ofstream out(GetSortFileName(next_group, next_num), std::ios::binary | std::ios::trunc);
      NJUDB_ASSERT(out.is_open(), fmt::format("cannot open run {}", GetSortFileName(next_group, next_num)));
      while (!heap.empty()) {
        auto node = heap.top();
        heap.pop();
        WriteRecordTo(out, *node.GetRecord());
        if (node.LoadNextRecord(kUnboundedRecords)) {
          heap.push(std::move(node));
        }
      }
      ++next_num;
    }

    // the runs of this group are fully merged into the next group
    std::error_code ec;
    for (size_t i = 0; i < file_num; ++i) {
      std::filesystem::remove(GetSortFileName(group, i), ec);
    }
    group    = next_group;
    file_num = next_num;
  }

  merge_result_path_        = GetSortFileName(group, 0);
  merge_result_file_handle_ = std::make_unique<std::ifstream>(merge_result_path_, std::ios::binary);
  NJUDB_ASSERT(merge_result_file_handle_->is_open(), fmt::format("cannot open {}", merge_result_path_));
}

}  // namespace njudb
