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

#include "index_hash.h"
#include "../buffer/page_guard.h"
#include <algorithm>
#include <functional>
#include <cstring>
#include <stdexcept>

namespace njudb {

namespace {

/// 一条索引项 = 定长键 + RID（page_id + slot_id）
auto EntrySize(size_t key_size) -> size_t { return key_size + sizeof(page_id_t) + sizeof(slot_id_t); }

/// 桶页正文起点：跳过页头，后面紧跟 HashBucketPage 的固定头
auto BucketContent(char *page_data) -> HashBucketPage *
{
  return reinterpret_cast<HashBucketPage *>(PageContentPtr(page_data));
}

auto BucketContent(const char *page_data) -> const HashBucketPage *
{
  return reinterpret_cast<const HashBucketPage *>(PageContentPtr(page_data));
}

/// 从一条索引项里读出 RID
auto ReadRIDAt(const char *entry, size_t key_size) -> RID
{
  page_id_t page_id = INVALID_PAGE_ID;
  slot_id_t slot_id = INVALID_SLOT_ID;
  std::memcpy(&page_id, entry + key_size, sizeof(page_id_t));
  std::memcpy(&slot_id, entry + key_size + sizeof(page_id_t), sizeof(slot_id_t));
  return RID(page_id, slot_id);
}

/// 把一条索引项的键部分还原成 Record（只填数据区，NULL 位全 0）
auto MakeKeyRecord(const char *entry, const RecordSchema *key_schema) -> Record
{
  return Record(key_schema, nullptr, entry, INVALID_RID);
}

}  // namespace

// HashBucketPage implementation
auto HashBucketPage::GetMaxEntries(size_t key_size) -> size_t
{
  // 桶页可用空间 = 页大小 - 页头 - HashBucketPage 自己的固定头
  const size_t capacity = PAGE_SIZE - PAGE_HEADER_SIZE - sizeof(HashBucketPage);
  const size_t entry    = EntrySize(key_size);
  return entry == 0 ? 0 : capacity / entry;
}

void HashBucketPage::WriteEntry(size_t offset, const Record &key, const RID &rid)
{
  const size_t key_size = key.GetSchema()->GetRecordLength();
  const auto   page_id  = rid.PageID();
  const auto   slot_id  = rid.SlotID();
  std::memcpy(data_ + offset, key.GetData(), key_size);
  std::memcpy(data_ + offset + key_size, &page_id, sizeof(page_id_t));
  std::memcpy(data_ + offset + key_size + sizeof(page_id_t), &slot_id, sizeof(slot_id_t));
}

void HashBucketPage::ReadEntry(size_t offset, Record &key, RID &rid, size_t key_size) const
{
  rid = ReadRIDAt(data_ + offset, key_size);
  // Record 没有可写的裸缓冲区，只能整条记录替换
  key = Record(key.GetSchema(), nullptr, data_ + offset, INVALID_RID);
}

// HashIndex implementation
HashIndex::HashIndex(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, idx_id_t index_id,
    const RecordSchema *key_schema)
    : Index(disk_manager, buffer_pool_manager, IndexType::HASH, index_id, key_schema),
      bucket_count_(DEFAULT_BUCKET_COUNT),
      total_entries_(0),
      key_size_(key_schema->GetRecordLength())  // Only key data, no null map
{
  InitializeHashIndex();
}

void HashIndex::InitializeHashIndex()
{
  // Create header page for hash index
  auto header_guard = buffer_pool_manager_->FetchPageWrite(index_id_, FILE_HEADER_PAGE_ID);
  auto header_page  = reinterpret_cast<HashHeaderPage *>(header_guard.GetMutableData());

  if (header_page->bucket_count_ != 0) {
    // It is an index that has already been initialized
    bucket_count_  = header_page->bucket_count_;
    total_entries_ = header_page->total_entries_;
    return;
  }

  // Initialize header page
  header_page->bucket_count_  = bucket_count_;
  header_page->total_entries_ = 0;
  header_page->next_page_id_  = 2;  // Start allocating from page 2 (skip header and directory)

  // Initialize bucket directory page
  auto directory_guard = buffer_pool_manager_->FetchPageWrite(index_id_, HASH_KEY_PAGE);
  auto directory_page  = reinterpret_cast<HashBucketDirectory *>(directory_guard.GetMutableData());

  // Calculate how many bucket page IDs can fit in the directory page
  size_t available_space = PAGE_SIZE - sizeof(HashBucketDirectory) - PAGE_HEADER_SIZE;
  size_t max_bucket_refs = available_space / sizeof(page_id_t);

  if (bucket_count_ > max_bucket_refs) {
    // Too many buckets for single directory page - should handle this case
    // For now, limit to available space
    bucket_count_              = max_bucket_refs;
    header_page->bucket_count_ = bucket_count_;
  }

  // Initialize bucket page IDs - initially all invalid
  for (size_t i = 0; i < bucket_count_; ++i) {
    directory_page->bucket_page_ids_[i] = INVALID_PAGE_ID;
  }

  // header_guard and directory_guard will automatically unpin the pages as dirty when they go out of scope
}

auto HashIndex::Hash(const Record &key) -> size_t
{
  // Use the Record's built-in hash function
  return key.Hash() % bucket_count_;
}

auto HashIndex::GetHashHeaderPage() -> HashHeaderPage *
{
  auto page = buffer_pool_manager_->FetchPage(index_id_, FILE_HEADER_PAGE_ID);
  return reinterpret_cast<HashHeaderPage *>(page->GetData());
}

auto HashIndex::GetBucketDirectory() -> HashBucketDirectory *
{
  auto page = buffer_pool_manager_->FetchPage(index_id_, HASH_KEY_PAGE);
  return reinterpret_cast<HashBucketDirectory *>(page->GetData());
}

auto HashIndex::GetBucketPage(page_id_t page_id) -> HashBucketPage *
{
  auto page = buffer_pool_manager_->FetchPage(index_id_, page_id);
  return reinterpret_cast<HashBucketPage *>(PageContentPtr(page->GetData()));
}

auto HashIndex::AllocateBucketPage() -> page_id_t
{
  // 文件头里的 next_page_id_ 单调递增，分配一个全新的页号
  page_id_t new_page_id = INVALID_PAGE_ID;
  {
    auto header_guard          = buffer_pool_manager_->FetchPageWrite(index_id_, FILE_HEADER_PAGE_ID);
    auto header_page           = reinterpret_cast<HashHeaderPage *>(header_guard.GetMutableData());
    new_page_id                = header_page->next_page_id_;
    header_page->next_page_id_ = new_page_id + 1;
  }
  {
    // 初始化为空桶页：无溢出页、零条目
    auto bucket_guard = buffer_pool_manager_->FetchPageWrite(index_id_, new_page_id);
    auto bucket_page  = BucketContent(bucket_guard.GetMutableData());
    std::memset(bucket_page, 0, PAGE_SIZE - PAGE_HEADER_SIZE);
    bucket_page->next_page_id_ = INVALID_PAGE_ID;
    bucket_page->entry_count_  = 0;
  }
  return new_page_id;
}

void HashIndex::Insert(const Record &key, const RID &rid)
{
  InsertIntoBucket(Hash(key), key, rid);

  ++total_entries_;
  auto header_guard           = buffer_pool_manager_->FetchPageWrite(index_id_, FILE_HEADER_PAGE_ID);
  auto header_page            = reinterpret_cast<HashHeaderPage *>(header_guard.GetMutableData());
  header_page->total_entries_ = total_entries_;
}

void HashIndex::InsertIntoBucket(size_t bucket_index, const Record &key, const RID &rid)
{
  const size_t entry_size  = EntrySize(key_size_);
  const size_t max_entries = HashBucketPage::GetMaxEntries(key_size_);

  // 先定位桶链首页：没有就分配一个并挂到目录上
  page_id_t page_id = INVALID_PAGE_ID;
  {
    auto directory_guard = buffer_pool_manager_->FetchPageWrite(index_id_, HASH_KEY_PAGE);
    auto directory_page  = reinterpret_cast<HashBucketDirectory *>(directory_guard.GetMutableData());
    page_id              = directory_page->bucket_page_ids_[bucket_index];
    if (page_id == INVALID_PAGE_ID) {
      page_id                              = AllocateBucketPage();
      directory_page->bucket_page_ids_[bucket_index] = page_id;
    }
  }

  while (true) {
    auto bucket_guard = buffer_pool_manager_->FetchPageWrite(index_id_, page_id);
    auto bucket_page  = BucketContent(bucket_guard.GetMutableData());
    if (bucket_page->entry_count_ < max_entries) {
      // 追加到页尾
      bucket_page->WriteEntry(bucket_page->entry_count_ * entry_size, key, rid);
      ++bucket_page->entry_count_;
      return;
    }
    if (bucket_page->next_page_id_ == INVALID_PAGE_ID) {
      // 当前页满了：分配溢出页挂到链尾，bucket_guard 析构时把链指针写回
      page_id = AllocateBucketPage();
      bucket_page->next_page_id_ = page_id;
      continue;
    }
    page_id = bucket_page->next_page_id_;
  }
}

auto HashIndex::Delete(const Record &key) -> bool
{
  const size_t deleted = DeleteAllFromBucket(Hash(key), key);
  if (deleted == 0) {
    return false;
  }
  total_entries_ -= deleted;
  auto header_guard           = buffer_pool_manager_->FetchPageWrite(index_id_, FILE_HEADER_PAGE_ID);
  auto header_page            = reinterpret_cast<HashHeaderPage *>(header_guard.GetMutableData());
  header_page->total_entries_ = total_entries_;
  return true;
}

auto HashIndex::DeleteAllFromBucket(size_t bucket_index, const Record &key) -> size_t
{
  const size_t entry_size = EntrySize(key_size_);
  size_t       deleted    = 0;

  page_id_t page_id = INVALID_PAGE_ID;
  {
    auto directory_guard = buffer_pool_manager_->FetchPageRead(index_id_, HASH_KEY_PAGE);
    auto directory_page  = reinterpret_cast<const HashBucketDirectory *>(directory_guard.GetData());
    page_id              = directory_page->bucket_page_ids_[bucket_index];
  }

  while (page_id != INVALID_PAGE_ID) {
    auto bucket_guard = buffer_pool_manager_->FetchPageWrite(index_id_, page_id);
    auto bucket_page  = BucketContent(bucket_guard.GetMutableData());

    // 原地压缩：保留的条目依次前移，相等键全部删除（重复键一次性移除）
    size_t keep = 0;
    for (size_t i = 0; i < bucket_page->entry_count_; ++i) {
      char *entry = bucket_page->data_ + i * entry_size;
      if (Record::Compare(MakeKeyRecord(entry, key_schema_), key) == 0) {
        ++deleted;
        continue;
      }
      if (keep != i) {
        std::memmove(bucket_page->data_ + keep * entry_size, entry, entry_size);
      }
      ++keep;
    }
    bucket_page->entry_count_ = keep;
    page_id                   = bucket_page->next_page_id_;
  }
  return deleted;
}

auto HashIndex::Search(const Record &key) -> std::vector<RID> { return SearchInBucket(Hash(key), key); }

auto HashIndex::SearchInBucket(size_t bucket_index, const Record &key) -> std::vector<RID>
{
  const size_t     entry_size = EntrySize(key_size_);
  std::vector<RID> results;

  page_id_t page_id = INVALID_PAGE_ID;
  {
    auto directory_guard = buffer_pool_manager_->FetchPageRead(index_id_, HASH_KEY_PAGE);
    auto directory_page  = reinterpret_cast<const HashBucketDirectory *>(directory_guard.GetData());
    page_id              = directory_page->bucket_page_ids_[bucket_index];
  }

  while (page_id != INVALID_PAGE_ID) {
    auto bucket_guard = buffer_pool_manager_->FetchPageRead(index_id_, page_id);
    auto bucket_page  = BucketContent(bucket_guard.GetData());
    for (size_t i = 0; i < bucket_page->entry_count_; ++i) {
      const char *entry = bucket_page->data_ + i * entry_size;
      if (Record::Compare(MakeKeyRecord(entry, key_schema_), key) == 0) {
        results.push_back(ReadRIDAt(entry, key_size_));
      }
    }
    page_id = bucket_page->next_page_id_;
  }
  return results;
}

auto HashIndex::SearchRange(const Record &low_key, const Record &high_key) -> std::vector<RID>
{
  // 哈希索引不支持高效有序扫描：这里扫描所有桶链做过滤，保证功能正确
  std::vector<RID> results;
  if (Record::Compare(low_key, high_key) > 0) {
    return results;
  }
  const size_t entry_size = EntrySize(key_size_);

  auto directory_guard = buffer_pool_manager_->FetchPageRead(index_id_, HASH_KEY_PAGE);
  auto directory_page  = reinterpret_cast<const HashBucketDirectory *>(directory_guard.GetData());
  for (size_t bucket_index = 0; bucket_index < bucket_count_; ++bucket_index) {
    page_id_t page_id = directory_page->bucket_page_ids_[bucket_index];
    while (page_id != INVALID_PAGE_ID) {
      auto bucket_guard = buffer_pool_manager_->FetchPageRead(index_id_, page_id);
      auto bucket_page  = BucketContent(bucket_guard.GetData());
      for (size_t i = 0; i < bucket_page->entry_count_; ++i) {
        const char *entry  = bucket_page->data_ + i * entry_size;
        auto        record = MakeKeyRecord(entry, key_schema_);
        if (Record::Compare(record, low_key) >= 0 && Record::Compare(record, high_key) <= 0) {
          results.push_back(ReadRIDAt(entry, key_size_));
        }
      }
      page_id = bucket_page->next_page_id_;
    }
  }
  return results;
}

// HashIterator implementation
HashIndex::HashIterator::HashIterator(HashIndex *index, bool is_end)
    : index_(index), current_bucket_(0), current_entry_(0), current_page_id_(INVALID_PAGE_ID), is_end_(is_end)
{
  if (!is_end_) {
    FindNextValidEntry();
  }
}

auto HashIndex::HashIterator::IsValid() -> bool { return !is_end_ && current_bucket_ < index_->bucket_count_; }

void HashIndex::HashIterator::Next()
{
  if (is_end_) {
    return;
  }
  ++current_entry_;
  FindNextValidEntry();
}

void HashIndex::HashIterator::FindNextValidEntry()
{
  while (current_bucket_ < index_->bucket_count_) {
    if (current_page_id_ == INVALID_PAGE_ID) {
      // 当前桶还没开始：取桶链首页
      auto directory_guard = index_->buffer_pool_manager_->FetchPageRead(index_->index_id_, HASH_KEY_PAGE);
      auto directory_page  = reinterpret_cast<const HashBucketDirectory *>(directory_guard.GetData());
      current_page_id_     = directory_page->bucket_page_ids_[current_bucket_];
      current_entry_       = 0;
      if (current_page_id_ == INVALID_PAGE_ID) {
        ++current_bucket_;
        continue;
      }
    }
    {
      auto bucket_guard = index_->buffer_pool_manager_->FetchPageRead(index_->index_id_, current_page_id_);
      auto bucket_page  = BucketContent(bucket_guard.GetData());
      if (current_entry_ < bucket_page->entry_count_) {
        return;  // 找到可读条目
      }
      // 当前页读完：沿溢出链往后走
      current_page_id_ = bucket_page->next_page_id_;
      current_entry_   = 0;
      if (current_page_id_ != INVALID_PAGE_ID) {
        continue;
      }
    }
    ++current_bucket_;  // 当前桶遍历完
  }
  is_end_ = true;
}

auto HashIndex::HashIterator::GetKey() -> Record
{
  const size_t entry_size   = EntrySize(index_->key_size_);
  auto         bucket_guard = index_->buffer_pool_manager_->FetchPageRead(index_->index_id_, current_page_id_);
  auto         bucket_page  = BucketContent(bucket_guard.GetData());
  return MakeKeyRecord(bucket_page->data_ + current_entry_ * entry_size, index_->key_schema_);
}

auto HashIndex::HashIterator::GetRID() -> RID
{
  const size_t entry_size   = EntrySize(index_->key_size_);
  auto         bucket_guard = index_->buffer_pool_manager_->FetchPageRead(index_->index_id_, current_page_id_);
  auto         bucket_page  = BucketContent(bucket_guard.GetData());
  return ReadRIDAt(bucket_page->data_ + current_entry_ * entry_size, index_->key_size_);
}

auto HashIndex::Begin() -> std::unique_ptr<IIterator> { return std::make_unique<HashIterator>(this); }

auto HashIndex::Begin(const Record &key) -> std::unique_ptr<IIterator>
{
  // For hash index, Begin(key) is similar to Begin() since there's no ordering
  // We could optionally start from the bucket containing the key
  auto iter              = std::make_unique<HashIterator>(this, true);
  iter->is_end_          = false;
  iter->current_bucket_  = Hash(key);
  iter->current_page_id_ = INVALID_PAGE_ID;
  iter->current_entry_   = 0;
  iter->FindNextValidEntry();
  return iter;
}

auto HashIndex::End() -> std::unique_ptr<IIterator> { return std::make_unique<HashIterator>(this, true); }

void HashIndex::Clear()
{
  {
    auto directory_guard = buffer_pool_manager_->FetchPageWrite(index_id_, HASH_KEY_PAGE);
    auto directory_page  = reinterpret_cast<HashBucketDirectory *>(directory_guard.GetMutableData());
    for (size_t i = 0; i < bucket_count_; ++i) {
      directory_page->bucket_page_ids_[i] = INVALID_PAGE_ID;
    }
  }
  total_entries_ = 0;
  {
    auto header_guard           = buffer_pool_manager_->FetchPageWrite(index_id_, FILE_HEADER_PAGE_ID);
    auto header_page            = reinterpret_cast<HashHeaderPage *>(header_guard.GetMutableData());
    header_page->total_entries_ = 0;
  }
}

auto HashIndex::IsEmpty() -> bool { return total_entries_ == 0; }

auto HashIndex::Size() -> size_t { return total_entries_; }

auto HashIndex::GetHeight() -> int
{
  // Hash indexes have a constant height of 2 (header + bucket pages)
  return 2;
}

}  // namespace njudb
