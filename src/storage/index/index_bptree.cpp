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

#include "index_bptree.h"
#include "../../../common/error.h"
#include "../buffer/page_guard.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include <string>

#define TEST_BPTREE

namespace njudb {

namespace {

auto AsPage(char *data) -> BPTreePage * { return reinterpret_cast<BPTreePage *>(data); }
auto AsPage(const char *data) -> const BPTreePage * { return reinterpret_cast<const BPTreePage *>(data); }
auto AsLeaf(char *data) -> BPTreeLeafPage * { return reinterpret_cast<BPTreeLeafPage *>(data); }
auto AsLeaf(const char *data) -> const BPTreeLeafPage * { return reinterpret_cast<const BPTreeLeafPage *>(data); }
auto AsInternal(char *data) -> BPTreeInternalPage * { return reinterpret_cast<BPTreeInternalPage *>(data); }
auto AsInternal(const char *data) -> const BPTreeInternalPage * { return reinterpret_cast<const BPTreeInternalPage *>(data); }

/// 由页内键字节构造一个只读键记录
auto MakeKey(const char *key_data, const RecordSchema *schema) -> Record
{
  return Record(schema, nullptr, key_data, INVALID_RID);
}

/// 页内键与目标键比较
auto CompareWithKey(const char *key_data, const Record &key, const RecordSchema *schema) -> int
{
  return Record::Compare(MakeKey(key_data, schema), key);
}

/// 从内部页中删除下标 index 处的 (child, key) 条目：child 左移、key 相应左移，key[0] 保持无效
void EraseInternalEntry(BPTreeInternalPage *parent, int index)
{
  const int size = parent->GetSize();
  for (int j = index; j + 1 < size; ++j) {
    parent->SetValueAt(j, parent->ValueAt(j + 1));
  }
  for (int k = index + 1; k < size; ++k) {
    if (k - 1 >= 1) {
      parent->SetKeyAt(k - 1, parent->KeyAt(k));
    }
  }
  parent->SetSize(size - 1);
}

}  // namespace

static void DebugPrintLeaf(const BPTreeLeafPage *leaf_node, const RecordSchema *key_schema)
{
  return;
  // print all keys in the leaf node for debugging
  page_id_t leaf_page_id = leaf_node->GetPageId();
  printf("Leaf %d: ", leaf_page_id);
  for (int i = 0; i < leaf_node->GetSize(); i++) {
    Record current_key(key_schema, nullptr, leaf_node->KeyAt(i), INVALID_RID);
    printf("key[%d]: %s; ", i, current_key.GetValueAt(0)->ToString().c_str());
  }
  printf("\n");
}

static void DebugPrintInternal(const BPTreeInternalPage *internal_node, const RecordSchema *key_schema)
{
  return;
  // print all keys in the internal node for debugging
  page_id_t internal_page_id = internal_node->GetPageId();
  printf("Internal %d: ", internal_page_id);
  for (int i = 0; i < internal_node->GetSize(); i++) {
    Record current_key(key_schema, nullptr, internal_node->KeyAt(i), INVALID_RID);
    printf("key[%d]: %s, value: %d; ", i, current_key.GetValueAt(0)->ToString().c_str(), internal_node->ValueAt(i));
  }
  printf("\n");
}

// BPTreePage implementation
void BPTreePage::Init(idx_id_t index_id, page_id_t page_id, page_id_t parent_id, BPTreeNodeType node_type, int max_size)
{
  index_id_       = index_id;
  page_id_        = page_id;
  parent_page_id_ = parent_id;
  node_type_      = node_type;
  max_size_       = static_cast<size_t>(max_size);
  size_           = 0;
}

auto BPTreePage::IsLeaf() const -> bool { return node_type_ == BPTreeNodeType::LEAF; }

auto BPTreePage::IsRoot() const -> bool { return parent_page_id_ == INVALID_PAGE_ID; }

auto BPTreePage::GetSize() const -> int { return static_cast<int>(size_); }

auto BPTreePage::GetMaxSize() const -> int { return static_cast<int>(max_size_); }

void BPTreePage::SetSize(int size)
{
  NJUDB_ASSERT(size >= 0 && static_cast<size_t>(size) <= max_size_, "Invalid node size");
  size_ = static_cast<size_t>(size);
}

auto BPTreePage::GetPageId() const -> page_id_t { return page_id_; }

auto BPTreePage::GetParentPageId() const -> page_id_t { return parent_page_id_; }

void BPTreePage::SetParentPageId(page_id_t parent_page_id) { parent_page_id_ = parent_page_id; }

auto BPTreePage::IsSafe(bool is_insert) const -> bool
{
  if (is_insert) {
    // 插入：还没满就安全
    return size_ < max_size_;
  }
  // 删除：仍高于最小占用（ceil(max/2)）就安全；根节点的特殊规则由 AdjustRoot 处理
  return size_ >= (max_size_ + 1) / 2;
}

// BPTreeLeafPage implementation
void BPTreeLeafPage::Init(idx_id_t index_id, page_id_t page_id, page_id_t parent_id, int key_size, int max_size)
{
  BPTreePage::Init(index_id, page_id, parent_id, BPTreeNodeType::LEAF, max_size);
  next_page_id_ = INVALID_PAGE_ID;
  key_size_     = key_size;
}

auto BPTreeLeafPage::GetNextPageId() const -> page_id_t { return next_page_id_; }

void BPTreeLeafPage::SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

auto BPTreeLeafPage::KeyAt(int index) const -> const char *
{
  NJUDB_ASSERT(index >= 0 && index < GetMaxSize(), "Key index out of range");
  return GetKeysArray() + static_cast<size_t>(index) * static_cast<size_t>(key_size_);
}

auto BPTreeLeafPage::ValueAt(int index) const -> RID
{
  NJUDB_ASSERT(index >= 0 && index < GetMaxSize(), "Value index out of range");
  return GetValuesArray()[index];
}

void BPTreeLeafPage::SetKeyAt(int index, const char *key)
{
  NJUDB_ASSERT(index >= 0 && index < GetMaxSize(), "Key index out of range");
  std::memcpy(GetKeysArray() + static_cast<size_t>(index) * static_cast<size_t>(key_size_), key, key_size_);
}

void BPTreeLeafPage::SetValueAt(int index, const RID &value)
{
  NJUDB_ASSERT(index >= 0 && index < GetMaxSize(), "Value index out of range");
  GetValuesArray()[index] = value;
}

auto BPTreeLeafPage::KeyIndex(const Record &key, const RecordSchema *schema) const -> int
{
  // 返回第一个 >= key 的位置（相等则就是相等键的位置，否则是插入位置）
  return LowerBound(key, schema);
}

auto BPTreeLeafPage::LowerBound(const Record &key, const RecordSchema *schema) const -> int
{
  // Find the first position where key <= keys[pos]
  // This is useful for >= queries
  int low  = 0;
  int high = GetSize();
  while (low < high) {
    const int mid = (low + high) / 2;
    if (CompareWithKey(KeyAt(mid), key, schema) < 0) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low;
}

auto BPTreeLeafPage::UpperBound(const Record &key, const RecordSchema *schema) const -> int
{
  // Find the first position where key < keys[pos]
  // This is useful for < queries
  int low  = 0;
  int high = GetSize();
  while (low < high) {
    const int mid = (low + high) / 2;
    if (CompareWithKey(KeyAt(mid), key, schema) <= 0) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low;
}

auto BPTreeLeafPage::Lookup(const Record &key, const RecordSchema *schema) const -> std::vector<RID>
{
  std::vector<RID> results;
  for (int i = LowerBound(key, schema); i < GetSize(); ++i) {
    if (CompareWithKey(KeyAt(i), key, schema) != 0) {
      break;
    }
    results.push_back(ValueAt(i));
  }
  return results;
}

auto BPTreeLeafPage::Insert(const Record &key, const RID &value, const RecordSchema *schema) -> int
{
  const int pos = LowerBound(key, schema);
  // 先整体右移，再写入新键
  for (int i = GetSize(); i > pos; --i) {
    SetKeyAt(i, KeyAt(i - 1));
    SetValueAt(i, ValueAt(i - 1));
  }
  SetKeyAt(pos, key.GetData());
  SetValueAt(pos, value);
  size_ += 1;
  return GetSize();
}

void BPTreeLeafPage::MoveHalfTo(BPTreeLeafPage *recipient)
{
  const int total = GetSize();
  const int mid   = total / 2;
  const int move  = total - mid;
  for (int i = 0; i < move; ++i) {
    recipient->SetKeyAt(i, KeyAt(mid + i));
    recipient->SetValueAt(i, ValueAt(mid + i));
  }
  recipient->size_ = static_cast<size_t>(move);
  size_            = static_cast<size_t>(mid);
  // 修复叶链：原页 -> 新右页 -> 原后继
  recipient->SetNextPageId(GetNextPageId());
  SetNextPageId(recipient->GetPageId());
}

void BPTreeLeafPage::CopyNFrom(const char *keys, const RID *values, int size)
{
  const int base = GetSize();
  for (int i = 0; i < size; ++i) {
    SetKeyAt(base + i, keys + static_cast<size_t>(i) * static_cast<size_t>(key_size_));
    SetValueAt(base + i, values[i]);
  }
  size_ += static_cast<size_t>(size);
}

auto BPTreeLeafPage::RemoveAndDeleteRecord(const Record &key, const RecordSchema *schema) -> int
{
  const int pos = LowerBound(key, schema);
  if (pos >= GetSize() || CompareWithKey(KeyAt(pos), key, schema) != 0) {
    return -1;  // 没找到
  }
  for (int i = pos; i + 1 < GetSize(); ++i) {
    SetKeyAt(i, KeyAt(i + 1));
    SetValueAt(i, ValueAt(i + 1));
  }
  size_ -= 1;
  return GetSize();
}

void BPTreeLeafPage::MoveAllTo(BPTreeLeafPage *recipient)
{
  const int base = recipient->GetSize();
  for (int i = 0; i < GetSize(); ++i) {
    recipient->SetKeyAt(base + i, KeyAt(i));
    recipient->SetValueAt(base + i, ValueAt(i));
  }
  recipient->size_ += size_;
  // 被合并的右页从叶链上摘掉：左页直接指向右页的后继
  recipient->SetNextPageId(GetNextPageId());
  size_ = 0;
}

// BPTreeInternalPage implementation
void BPTreeInternalPage::Init(idx_id_t index_id, page_id_t page_id, page_id_t parent_id, int key_size, int max_size)
{
  BPTreePage::Init(index_id, page_id, parent_id, BPTreeNodeType::INTERNAL, max_size);
  key_size_ = key_size;
}

auto BPTreeInternalPage::KeyAt(int index) const -> const char *
{
  NJUDB_ASSERT(index >= 0 && index <= GetMaxSize(), "Key index out of range");
  return GetKeysArray() + static_cast<size_t>(index) * static_cast<size_t>(key_size_);
}

auto BPTreeInternalPage::GetKeySize() const -> int { return key_size_; }

auto BPTreeInternalPage::ValueAt(int index) const -> page_id_t
{
  NJUDB_ASSERT(index >= 0 && index <= GetMaxSize(), "Value index out of range");
  return GetChildrenArray()[index];
}

void BPTreeInternalPage::SetKeyAt(int index, const char *key)
{
  NJUDB_ASSERT(index >= 0 && index <= GetMaxSize(), "Key index out of range");
  std::memcpy(GetKeysArray() + static_cast<size_t>(index) * static_cast<size_t>(key_size_), key, key_size_);
}

void BPTreeInternalPage::SetValueAt(int index, page_id_t value)
{
  NJUDB_ASSERT(index >= 0 && index <= GetMaxSize(), "Value index out of range");
  GetChildrenArray()[index] = value;
}

auto BPTreeInternalPage::Lookup(const Record &key, const RecordSchema *schema) const -> page_id_t
{
  // 最后一个满足 KeyAt(i) <= key 的子页（KeyAt(0) 是占位，不能用）
  int index = 0;
  for (int i = 1; i < GetSize(); ++i) {
    if (CompareWithKey(KeyAt(i), key, schema) <= 0) {
      index = i;
    } else {
      break;
    }
  }
  return ValueAt(index);
}

auto BPTreeInternalPage::LookupForLowerBound(const Record &key, const RecordSchema *schema) const -> page_id_t
{
  // For lower bound, we want to find the leftmost position where key could be inserted
  // This means finding the leftmost child that could contain keys >= key
  for (int i = 1; i < GetSize(); ++i) {
    if (CompareWithKey(KeyAt(i), key, schema) >= 0) {
      return ValueAt(i - 1);
    }
  }
  return ValueAt(GetSize() - 1);
}

auto BPTreeInternalPage::LookupForUpperBound(const Record &key, const RecordSchema *schema) const -> page_id_t
{
  // For upper bound, we want to find the rightmost position where key could be inserted
  // This means finding the rightmost child that could contain keys <= key
  return Lookup(key, schema);
}

void BPTreeInternalPage::PopulateNewRoot(page_id_t old_root_id, const Record &new_key, page_id_t new_page_id)
{
  SetValueAt(0, old_root_id);
  SetValueAt(1, new_page_id);
  SetKeyAt(1, new_key.GetData());
  SetSize(2);
}

auto BPTreeInternalPage::InsertNodeAfter(page_id_t old_value, const Record &new_key, page_id_t new_value) -> int
{
  int index = -1;
  for (int i = 0; i < GetSize(); ++i) {
    if (ValueAt(i) == old_value) {
      index = i;
      break;
    }
  }
  NJUDB_ASSERT(index != -1, "InsertNodeAfter: old child not found");

  // 从后往前整体右移一格，把新子页插到 index+1
  for (int i = GetSize(); i > index + 1; --i) {
    SetValueAt(i, ValueAt(i - 1));
    SetKeyAt(i, KeyAt(i - 1));
  }
  SetValueAt(index + 1, new_value);
  SetKeyAt(index + 1, new_key.GetData());
  size_ += 1;
  return GetSize();
}

void BPTreeInternalPage::MoveHalfTo(BPTreeInternalPage *recipient, BufferPoolManager *buffer_pool_manager)
{
  const int total = GetSize();
  const int mid   = total / 2;
  const int move  = total - mid;
  for (int i = 0; i < move; ++i) {
    const page_id_t child = ValueAt(mid + i);
    recipient->SetValueAt(i, child);
    if (i > 0) {
      recipient->SetKeyAt(i, KeyAt(mid + i));
    }
    // 被移动子页的父指针必须指向接收页
    auto child_guard = buffer_pool_manager->FetchPageWrite(index_id_, child);
    AsPage(PageContentPtr(child_guard.GetMutableData()))->SetParentPageId(recipient->GetPageId());
  }
  recipient->size_ = static_cast<size_t>(move);
  size_            = static_cast<size_t>(mid);
}

void BPTreeInternalPage::MoveAllTo(
    BPTreeInternalPage *recipient, const Record &middle_key, BufferPoolManager *buffer_pool_manager)
{
  // For internal nodes, we need to merge:
  // 1. The middle key from the parent (this becomes a key in the recipient)
  // 2. All keys and children from the source node
  const int base = recipient->GetSize();
  recipient->SetKeyAt(base, middle_key.GetData());
  recipient->SetValueAt(base, ValueAt(0));
  for (int i = 1; i < GetSize(); ++i) {
    recipient->SetKeyAt(base + i, KeyAt(i));
    recipient->SetValueAt(base + i, ValueAt(i));
  }
  recipient->size_ += size_;
  for (int i = 0; i < static_cast<int>(size_); ++i) {
    const page_id_t child = recipient->ValueAt(base + i);
    auto            guard = buffer_pool_manager->FetchPageWrite(index_id_, child);
    AsPage(PageContentPtr(guard.GetMutableData()))->SetParentPageId(recipient->GetPageId());
  }
  size_ = 0;
}

void BPTreeInternalPage::CopyNFrom(
    const char *keys, const page_id_t *values, int size, BufferPoolManager *buffer_pool_manager)
{
  const int base = GetSize();
  for (int i = 0; i < size; ++i) {
    SetKeyAt(base + i, keys + static_cast<size_t>(i) * static_cast<size_t>(key_size_));
    SetValueAt(base + i, values[i]);
  }
  size_ += static_cast<size_t>(size);
  for (int i = 0; i < size; ++i) {
    auto guard = buffer_pool_manager->FetchPageWrite(index_id_, values[i]);
    AsPage(PageContentPtr(guard.GetMutableData()))->SetParentPageId(GetPageId());
  }
}

// BPTreeIndex implementation
BPTreeIndex::BPTreeIndex(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, idx_id_t index_id,
    const RecordSchema *key_schema)
    : Index(disk_manager, buffer_pool_manager, IndexType::BPTREE, index_id, key_schema)
{

  // Initialize index header
  InitializeIndex();
}

void BPTreeIndex::InitializeIndex()
{
  // Get or create header page
  auto header_guard = buffer_pool_manager_->FetchPageWrite(index_id_, FILE_HEADER_PAGE_ID);
  if (!header_guard.IsValid()) {
    NJUDB_THROW(NJUDB_EXCEPTION_EMPTY, "Cannot fetch header page");
  }

  auto header = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());

  // Check if already initialized
  if (header->page_num_ != 0) {
    key_size_          = header->key_size_;
    leaf_max_size_     = header->leaf_max_size_;
    internal_max_size_ = header->internal_max_size_;
    return;
  }

  // first check if the header and the schema raw data can accomodate int the header file
  if (key_schema_->SerializeSize() + sizeof(BPTreeIndexHeader) > PAGE_SIZE) {
    NJUDB_THROW(NJUDB_INDEX_FAIL, "Key schema too large to fit in B+ tree header");
  }

  // Initialize header
  header->root_page_id_       = INVALID_PAGE_ID;
  header->first_free_page_id_ = INVALID_PAGE_ID;
  header->tree_height_        = 0;
  header->page_num_           = 1;  // Header page counts
  header->key_size_           = key_schema_->GetRecordLength();
  header->value_size_         = sizeof(RID);
  header->num_entries_        = 0;

  // Note: TEST_BPTREE mode is for testing your B+tree implementation.
  // you can only undef it after you have passed gtests and is ready to use it in executors.
#ifdef TEST_BPTREE
  std::cout << "TEST_BPTREE mode: using fixed max sizes" << std::endl;
  header->leaf_max_size_     = 4;
  header->internal_max_size_ = 4;
#else
  // Calculate max sizes based on page size

  size_t leaf_header_size     = sizeof(BPTreeLeafPage);
  size_t available_leaf_space = PAGE_SIZE - PAGE_HEADER_SIZE - leaf_header_size;
  header->leaf_max_size_      = available_leaf_space / (header->key_size_ + sizeof(RID));

  size_t internal_header_size     = sizeof(BPTreeInternalPage);
  size_t available_internal_space = PAGE_SIZE - PAGE_HEADER_SIZE - internal_header_size;
  header->internal_max_size_      = available_internal_space / (header->key_size_ + sizeof(page_id_t));

  // check if the max size of leaf and internal are valid
  if (static_cast<int>(header->leaf_max_size_) <= 0 || static_cast<int>(header->internal_max_size_) <= 0) {
    NJUDB_THROW(NJUDB_INDEX_FAIL, "Key too large for a B+ tree node to fit into a single page");
  }
#endif

  key_size_          = header->key_size_;
  leaf_max_size_     = header->leaf_max_size_;
  internal_max_size_ = header->internal_max_size_;
}

auto BPTreeIndex::FetchHeaderRead() -> ReadPageGuard
{
  return buffer_pool_manager_->FetchPageRead(index_id_, FILE_HEADER_PAGE_ID);
}

auto BPTreeIndex::FetchHeaderWrite() -> WritePageGuard
{
  return buffer_pool_manager_->FetchPageWrite(index_id_, FILE_HEADER_PAGE_ID);
}

auto BPTreeIndex::NewPage() -> page_id_t
{
  page_id_t page_id = INVALID_PAGE_ID;
  {
    auto header_guard = FetchHeaderWrite();
    auto header       = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
    if (header->first_free_page_id_ != INVALID_PAGE_ID) {
      // 优先复用空闲链上的页
      page_id = header->first_free_page_id_;
      auto freed_guard = buffer_pool_manager_->FetchPageRead(index_id_, page_id);
      header->first_free_page_id_ = freed_guard.GetPage()->GetNextFreePageId();
    } else {
      page_id = static_cast<page_id_t>(header->page_num_);
      header->page_num_ += 1;
    }
  }
  // 新页内容清零，避免复用空闲页时残留旧数据
  auto guard = buffer_pool_manager_->FetchPageWrite(index_id_, page_id);
  std::memset(PageContentPtr(guard.GetMutableData()), 0, PAGE_SIZE - PAGE_HEADER_SIZE);
  return page_id;
}

void BPTreeIndex::DeletePage(page_id_t page_id)
{
  auto header_guard = FetchHeaderWrite();
  auto header       = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
  auto page_guard   = buffer_pool_manager_->FetchPageWrite(index_id_, page_id);
  page_guard.GetPage()->SetNextFreePageId(header->first_free_page_id_);
  header->first_free_page_id_ = page_id;
}

auto BPTreeIndex::FindLeafPage(const Record &key, bool leftMost) -> page_id_t
{
  page_id_t root_page_id = INVALID_PAGE_ID;
  {
    auto header_guard  = FetchHeaderRead();
    auto header        = reinterpret_cast<const BPTreeIndexHeader *>(header_guard.GetData());
    root_page_id       = header->root_page_id_;
  }
  if (root_page_id == INVALID_PAGE_ID) {
    return INVALID_PAGE_ID;
  }
  page_id_t page_id = root_page_id;
  while (true) {
    auto       guard = buffer_pool_manager_->FetchPageRead(index_id_, page_id);
    const auto *page = AsPage(PageContentPtr(guard.GetData()));
    if (page->IsLeaf()) {
      return page_id;
    }
    const auto *internal = reinterpret_cast<const BPTreeInternalPage *>(page);
    page_id              = leftMost ? internal->ValueAt(0) : internal->Lookup(key, key_schema_);
  }
}

auto BPTreeIndex::FindLeafPageForRange(const Record &key, bool isLowerBound) -> page_id_t
{
  page_id_t root_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = FetchHeaderRead();
    auto header       = reinterpret_cast<const BPTreeIndexHeader *>(header_guard.GetData());
    root_page_id      = header->root_page_id_;
  }
  if (root_page_id == INVALID_PAGE_ID) {
    return INVALID_PAGE_ID;
  }
  page_id_t page_id = root_page_id;
  while (true) {
    auto       guard = buffer_pool_manager_->FetchPageRead(index_id_, page_id);
    const auto *page = AsPage(PageContentPtr(guard.GetData()));
    if (page->IsLeaf()) {
      return page_id;
    }
    const auto *internal = reinterpret_cast<const BPTreeInternalPage *>(page);
    page_id = isLowerBound ? internal->LookupForLowerBound(key, key_schema_)
                           : internal->LookupForUpperBound(key, key_schema_);
  }
}

void BPTreeIndex::StartNewTree(const Record &key, const RID &value)
{
  const page_id_t root_page_id = NewPage();
  {
    auto page_guard = buffer_pool_manager_->FetchPageWrite(index_id_, root_page_id);
    auto leaf_page  = AsLeaf(PageContentPtr(page_guard.GetMutableData()));
    leaf_page->Init(index_id_, root_page_id, INVALID_PAGE_ID, static_cast<int>(key_size_), static_cast<int>(leaf_max_size_));
    leaf_page->Insert(key, value, key_schema_);
  }
  auto header_guard         = FetchHeaderWrite();
  auto header               = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
  header->root_page_id_     = root_page_id;
  header->tree_height_      = 1;
}

auto BPTreeIndex::InsertIntoLeaf(const Record &key, const RID &value) -> bool
{
  const page_id_t leaf_page_id = FindLeafPage(key);
  if (leaf_page_id == INVALID_PAGE_ID) {
    return false;
  }

  page_id_t   new_leaf_page_id = INVALID_PAGE_ID;
  RecordUptr  separator;
  {
    auto      leaf_guard = buffer_pool_manager_->FetchPageWrite(index_id_, leaf_page_id);
    auto      leaf_page  = AsLeaf(PageContentPtr(leaf_guard.GetMutableData()));
    if (leaf_page->IsSafe(true)) {
      leaf_page->Insert(key, value, key_schema_);
    } else {
      // 叶页已满：先分裂，再插入到合适的一侧
      new_leaf_page_id = NewPage();
      auto new_guard   = buffer_pool_manager_->FetchPageWrite(index_id_, new_leaf_page_id);
      auto new_page    = AsLeaf(PageContentPtr(new_guard.GetMutableData()));
      new_page->Init(index_id_, new_leaf_page_id, leaf_page->GetParentPageId(), static_cast<int>(key_size_),
          static_cast<int>(leaf_max_size_));
      leaf_page->MoveHalfTo(new_page);

      // 新页的最小键 <= key 时应该插到右页，否则插回左页
      if (CompareWithKey(new_page->KeyAt(0), key, key_schema_) <= 0) {
        new_page->Insert(key, value, key_schema_);
      } else {
        leaf_page->Insert(key, value, key_schema_);
      }
      separator = std::make_unique<Record>(MakeKey(new_page->KeyAt(0), key_schema_));
    }
  }
  if (new_leaf_page_id != INVALID_PAGE_ID) {
    InsertIntoParent(leaf_page_id, *separator, new_leaf_page_id);
  }
  return true;
}

void BPTreeIndex::InsertIntoParent(page_id_t old_node_id, const Record &key, page_id_t new_node_id)
{
  page_id_t parent_page_id = INVALID_PAGE_ID;
  {
    auto page_guard  = buffer_pool_manager_->FetchPageRead(index_id_, old_node_id);
    parent_page_id   = AsPage(PageContentPtr(page_guard.GetData()))->GetParentPageId();
  }
  if (parent_page_id == INVALID_PAGE_ID) {
    InsertIntoNewRoot(old_node_id, key, new_node_id);
    return;
  }

  page_id_t  new_internal_page_id = INVALID_PAGE_ID;
  RecordUptr promoted_key;
  {
    auto parent_guard = buffer_pool_manager_->FetchPageWrite(index_id_, parent_page_id);
    auto parent_page  = AsInternal(PageContentPtr(parent_guard.GetMutableData()));
    parent_page->InsertNodeAfter(old_node_id, key, new_node_id);
    if (parent_page->GetSize() > parent_page->GetMaxSize()) {
      // 内部页也满了：把后半部分切给新的内部页，中间键提升给父页
      const int mid = parent_page->GetSize() / 2;
      promoted_key  = std::make_unique<Record>(MakeKey(parent_page->KeyAt(mid), key_schema_));
      new_internal_page_id = NewPage();
      auto new_guard       = buffer_pool_manager_->FetchPageWrite(index_id_, new_internal_page_id);
      auto new_page        = AsInternal(PageContentPtr(new_guard.GetMutableData()));
      new_page->Init(index_id_, new_internal_page_id, parent_page->GetParentPageId(), static_cast<int>(key_size_),
          static_cast<int>(internal_max_size_));
      parent_page->MoveHalfTo(new_page, buffer_pool_manager_);
    }
  }
  if (new_internal_page_id != INVALID_PAGE_ID) {
    InsertIntoParent(parent_page_id, *promoted_key, new_internal_page_id);
  }
}

void BPTreeIndex::InsertIntoNewRoot(page_id_t old_root_id, const Record &key, page_id_t new_page_id)
{
  const page_id_t new_root_page_id = NewPage();
  {
    auto root_guard = buffer_pool_manager_->FetchPageWrite(index_id_, new_root_page_id);
    auto root_page  = AsInternal(PageContentPtr(root_guard.GetMutableData()));
    root_page->Init(index_id_, new_root_page_id, INVALID_PAGE_ID, static_cast<int>(key_size_),
        static_cast<int>(internal_max_size_));
    root_page->PopulateNewRoot(old_root_id, key, new_page_id);
  }
  {
    auto old_guard = buffer_pool_manager_->FetchPageWrite(index_id_, old_root_id);
    AsPage(PageContentPtr(old_guard.GetMutableData()))->SetParentPageId(new_root_page_id);
  }
  {
    auto new_guard = buffer_pool_manager_->FetchPageWrite(index_id_, new_page_id);
    AsPage(PageContentPtr(new_guard.GetMutableData()))->SetParentPageId(new_root_page_id);
  }
  auto header_guard     = FetchHeaderWrite();
  auto header           = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
  header->root_page_id_ = new_root_page_id;
  header->tree_height_ += 1;
}

void BPTreeIndex::Insert(const Record &key, const RID &rid)
{
  std::unique_lock lock(index_latch_);
  page_id_t        root_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = FetchHeaderRead();
    root_page_id      = reinterpret_cast<const BPTreeIndexHeader *>(header_guard.GetData())->root_page_id_;
  }
  if (root_page_id == INVALID_PAGE_ID) {
    StartNewTree(key, rid);
  } else {
    InsertIntoLeaf(key, rid);
  }
  auto header_guard = FetchHeaderWrite();
  auto header       = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
  header->num_entries_ += 1;
}

auto BPTreeIndex::Delete(const Record &key) -> bool
{
  std::unique_lock lock(index_latch_);
  {
    auto header_guard = FetchHeaderRead();
    if (reinterpret_cast<const BPTreeIndexHeader *>(header_guard.GetData())->root_page_id_ == INVALID_PAGE_ID) {
      return false;
    }
  }
  const page_id_t leaf_page_id = FindLeafPage(key);
  if (leaf_page_id == INVALID_PAGE_ID) {
    return false;
  }

  bool underflow = false;
  {
    auto leaf_guard = buffer_pool_manager_->FetchPageWrite(index_id_, leaf_page_id);
    auto leaf_page  = AsLeaf(PageContentPtr(leaf_guard.GetMutableData()));
    if (leaf_page->RemoveAndDeleteRecord(key, key_schema_) == -1) {
      return false;
    }
    underflow = !leaf_page->IsSafe(false);
    auto header_guard = FetchHeaderWrite();
    reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData())->num_entries_ -= 1;
  }
  if (underflow) {
    CoalesceOrRedistribute(leaf_page_id);
  }
  return true;
}

auto BPTreeIndex::CoalesceOrRedistribute(page_id_t node_id) -> bool
{
  page_id_t parent_page_id = INVALID_PAGE_ID;
  bool      is_leaf        = false;
  int       node_size      = 0;
  int       max_size       = 0;
  int       index          = -1;
  {
    auto node_guard = buffer_pool_manager_->FetchPageRead(index_id_, node_id);
    auto node_page  = AsPage(PageContentPtr(node_guard.GetData()));
    parent_page_id  = node_page->GetParentPageId();
    is_leaf         = node_page->IsLeaf();
    node_size       = node_page->GetSize();
    max_size        = node_page->GetMaxSize();
    if (parent_page_id != INVALID_PAGE_ID) {
      auto parent_guard = buffer_pool_manager_->FetchPageRead(index_id_, parent_page_id);
      auto parent_page  = AsInternal(PageContentPtr(parent_guard.GetData()));
      for (int i = 0; i < parent_page->GetSize(); ++i) {
        if (parent_page->ValueAt(i) == node_id) {
          index = i;
          break;
        }
      }
    }
  }
  if (parent_page_id == INVALID_PAGE_ID) {
    // 根节点：交给 AdjustRoot 单独处理，旧根页在让出守卫之后再回收
    bool reclaimed = false;
    {
      auto root_guard = buffer_pool_manager_->FetchPageWrite(index_id_, node_id);
      reclaimed       = AdjustRoot(AsPage(PageContentPtr(root_guard.GetMutableData())));
    }
    if (reclaimed) {
      DeletePage(node_id);
    }
    return reclaimed;
  }

  NJUDB_ASSERT(index != -1, "CoalesceOrRedistribute: node not found in parent");
  const int       neighbor_index = (index == 0) ? 1 : index - 1;
  page_id_t       neighbor_id    = INVALID_PAGE_ID;
  int             neighbor_size  = 0;
  {
    auto parent_guard = buffer_pool_manager_->FetchPageRead(index_id_, parent_page_id);
    auto parent_page  = AsInternal(PageContentPtr(parent_guard.GetData()));
    neighbor_id       = parent_page->ValueAt(neighbor_index);
  }
  {
    auto neighbor_guard = buffer_pool_manager_->FetchPageRead(index_id_, neighbor_id);
    neighbor_size       = AsPage(PageContentPtr(neighbor_guard.GetData()))->GetSize();
  }

  if (node_size + neighbor_size <= max_size) {
    return Coalesce(neighbor_id, node_id, parent_page_id, index);
  }
  // 两页放不下：在兄弟之间搬一个条目
  auto node_guard     = buffer_pool_manager_->FetchPageWrite(index_id_, node_id);
  auto neighbor_guard = buffer_pool_manager_->FetchPageWrite(index_id_, neighbor_id);
  Redistribute(AsPage(PageContentPtr(neighbor_guard.GetMutableData())),
      AsPage(PageContentPtr(node_guard.GetMutableData())), index);
  (void)is_leaf;
  return false;
}

auto BPTreeIndex::Coalesce(page_id_t neighbor_node_id, page_id_t node_id, page_id_t parent_id, int index) -> bool
{
  const bool      node_is_left = (index == 0);
  const page_id_t left_id      = node_is_left ? node_id : neighbor_node_id;
  const page_id_t right_id     = node_is_left ? neighbor_node_id : node_id;
  const int       right_index  = node_is_left ? 1 : index;

  // 内部页合并时要把父页的中间键下移
  RecordUptr middle_key;
  {
    auto left_guard  = buffer_pool_manager_->FetchPageWrite(index_id_, left_id);
    auto right_guard = buffer_pool_manager_->FetchPageWrite(index_id_, right_id);
    auto left_page   = AsPage(PageContentPtr(left_guard.GetMutableData()));
    auto right_page  = AsPage(PageContentPtr(right_guard.GetMutableData()));
    if (!left_page->IsLeaf()) {
      auto parent_guard = buffer_pool_manager_->FetchPageRead(index_id_, parent_id);
      auto parent_page  = AsInternal(PageContentPtr(parent_guard.GetData()));
      middle_key        = std::make_unique<Record>(MakeKey(parent_page->KeyAt(right_index), key_schema_));
      AsInternal(reinterpret_cast<char *>(right_page))
          ->MoveAllTo(AsInternal(reinterpret_cast<char *>(left_page)), *middle_key, buffer_pool_manager_);
    } else {
      AsLeaf(reinterpret_cast<char *>(right_page))->MoveAllTo(AsLeaf(reinterpret_cast<char *>(left_page)));
    }
  }
  DeletePage(right_id);

  bool      parent_is_root    = false;
  bool      parent_underflow  = false;
  bool      root_single_child = false;
  bool      root_became_empty = false;
  page_id_t only_child        = INVALID_PAGE_ID;
  {
    auto parent_guard = buffer_pool_manager_->FetchPageWrite(index_id_, parent_id);
    auto parent_page  = AsInternal(PageContentPtr(parent_guard.GetMutableData()));
    parent_is_root    = parent_page->GetParentPageId() == INVALID_PAGE_ID;
    EraseInternalEntry(parent_page, right_index);
    if (parent_is_root) {
      if (parent_page->GetSize() == 1) {
        root_single_child = true;
        only_child        = parent_page->ValueAt(0);
      } else if (parent_page->GetSize() == 0) {
        root_became_empty = true;
      }
    } else {
      parent_underflow = !parent_page->IsSafe(false);
    }
  }
  if (root_single_child) {
    {
      auto child_guard = buffer_pool_manager_->FetchPageWrite(index_id_, only_child);
      AsPage(PageContentPtr(child_guard.GetMutableData()))->SetParentPageId(INVALID_PAGE_ID);
    }
    auto header_guard     = FetchHeaderWrite();
    auto header           = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
    header->root_page_id_ = only_child;
    header->tree_height_ -= 1;
    DeletePage(parent_id);
  } else if (root_became_empty) {
    auto header_guard     = FetchHeaderWrite();
    auto header           = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
    header->root_page_id_ = INVALID_PAGE_ID;
    header->tree_height_  = 0;
    DeletePage(parent_id);
  } else if (parent_underflow) {
    CoalesceOrRedistribute(parent_id);
  }
  return true;
}

void BPTreeIndex::Redistribute(BPTreePage *neighbor_node, BPTreePage *node, int index)
{
  // index 是 node 在父页中的位置；兄弟取相邻的一侧
  const bool node_is_left = (index == 0);
  auto      *left_page    = node_is_left ? node : neighbor_node;
  auto      *right_page   = node_is_left ? neighbor_node : node;
  const int  left_index   = node_is_left ? index : index - 1;
  const int  right_index  = left_index + 1;

  auto parent_guard = buffer_pool_manager_->FetchPageWrite(index_id_, node->GetParentPageId());
  auto parent_page  = AsInternal(PageContentPtr(parent_guard.GetMutableData()));

  const int total = left_page->GetSize() + right_page->GetSize();
  int       mid   = total / 2;
  if (mid < 1) {
    mid = 1;
  }
  if (mid >= total) {
    mid = total - 1;
  }

  if (left_page->IsLeaf()) {
    auto *left_leaf  = AsLeaf(reinterpret_cast<char *>(left_page));
    auto *right_leaf = AsLeaf(reinterpret_cast<char *>(right_page));

    std::vector<std::vector<char>> keys;
    std::vector<RID>               rids;
    keys.reserve(total);
    rids.reserve(total);
    for (int i = 0; i < left_leaf->GetSize(); ++i) {
      keys.emplace_back(left_leaf->KeyAt(i), left_leaf->KeyAt(i) + key_size_);
      rids.push_back(left_leaf->ValueAt(i));
    }
    for (int i = 0; i < right_leaf->GetSize(); ++i) {
      keys.emplace_back(right_leaf->KeyAt(i), right_leaf->KeyAt(i) + key_size_);
      rids.push_back(right_leaf->ValueAt(i));
    }
    // 重新写成 左 mid 条 + 右 total-mid 条
    left_leaf->SetSize(0);
    for (int i = 0; i < mid; ++i) {
      left_leaf->SetKeyAt(i, keys[i].data());
      left_leaf->SetValueAt(i, rids[i]);
    }
    left_leaf->size_ = static_cast<size_t>(mid);
    right_leaf->SetSize(0);
    for (int i = 0; i < total - mid; ++i) {
      right_leaf->SetKeyAt(i, keys[mid + i].data());
      right_leaf->SetValueAt(i, rids[mid + i]);
    }
    right_leaf->size_ = static_cast<size_t>(total - mid);
    left_leaf->SetNextPageId(right_leaf->GetPageId());

    parent_page->SetKeyAt(right_index, keys[static_cast<size_t>(mid)].data());
    if (left_index >= 1) {
      parent_page->SetKeyAt(left_index, keys[0].data());
    }
  } else {
    auto *left_internal  = AsInternal(reinterpret_cast<char *>(left_page));
    auto *right_internal = AsInternal(reinterpret_cast<char *>(right_page));

    std::vector<page_id_t>         children;
    std::vector<std::vector<char>> min_keys;
    children.reserve(total);
    min_keys.reserve(total);
    for (int i = 0; i < left_internal->GetSize(); ++i) {
      children.push_back(left_internal->ValueAt(i));
      if (i == 0) {
        // 左节点的最小键存在父页里；左节点是父页第一个子页时没有对应键
        // 左节点是父页第一个子页时，它第一个子页的最小键在父页里没有独立存储；
        // 这个占位值不会被写回父页（left_index == 0 时不会用到）
        const char *left_min_key =
            (left_index >= 1) ? parent_page->KeyAt(left_index) : parent_page->KeyAt(right_index);
        min_keys.emplace_back(left_min_key, left_min_key + key_size_);
      } else {
        min_keys.emplace_back(left_internal->KeyAt(i), left_internal->KeyAt(i) + key_size_);
      }
    }
    for (int i = 0; i < right_internal->GetSize(); ++i) {
      children.push_back(right_internal->ValueAt(i));
      if (i == 0) {
        min_keys.emplace_back(parent_page->KeyAt(right_index), parent_page->KeyAt(right_index) + key_size_);
      } else {
        min_keys.emplace_back(right_internal->KeyAt(i), right_internal->KeyAt(i) + key_size_);
      }
    }
    left_internal->SetSize(0);
    for (int i = 0; i < mid; ++i) {
      left_internal->SetValueAt(i, children[static_cast<size_t>(i)]);
      if (i >= 1) {
        left_internal->SetKeyAt(i, min_keys[static_cast<size_t>(i)].data());
      }
    }
    left_internal->size_ = static_cast<size_t>(mid);
    right_internal->SetSize(0);
    for (int i = 0; i < total - mid; ++i) {
      right_internal->SetValueAt(i, children[static_cast<size_t>(mid + i)]);
      if (i >= 1) {
        right_internal->SetKeyAt(i, min_keys[static_cast<size_t>(mid + i)].data());
      }
    }
    right_internal->size_ = static_cast<size_t>(total - mid);
    parent_page->SetKeyAt(right_index, min_keys[static_cast<size_t>(mid)].data());

    // 子页父指针同步到新的两个父页
    for (int i = 0; i < total; ++i) {
      auto child_guard = buffer_pool_manager_->FetchPageWrite(index_id_, children[static_cast<size_t>(i)]);
      AsPage(PageContentPtr(child_guard.GetMutableData()))
          ->SetParentPageId(i < mid ? left_internal->GetPageId() : right_internal->GetPageId());
    }
  }
}

auto BPTreeIndex::AdjustRoot(BPTreePage *old_root_node) -> bool
{
  if (old_root_node->IsLeaf()) {
    if (old_root_node->GetSize() > 0) {
      return false;
    }
    // 根叶页被删空：整棵树变空
    auto header_guard     = FetchHeaderWrite();
    auto header           = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
    header->root_page_id_ = INVALID_PAGE_ID;
    header->tree_height_  = 0;
    header->num_entries_  = 0;
    return true;
  }
  auto      *root_page = AsInternal(reinterpret_cast<char *>(old_root_node));
  const int  size      = root_page->GetSize();
  if (size == 0) {
    auto header_guard     = FetchHeaderWrite();
    auto header           = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
    header->root_page_id_ = INVALID_PAGE_ID;
    header->tree_height_  = 0;
    return true;
  }
  if (size == 1) {
    // 内部根只剩一个子页：子页成为新根，树高减一
    const page_id_t child_page_id = root_page->ValueAt(0);
    {
      auto child_guard = buffer_pool_manager_->FetchPageWrite(index_id_, child_page_id);
      AsPage(PageContentPtr(child_guard.GetMutableData()))->SetParentPageId(INVALID_PAGE_ID);
    }
    auto header_guard     = FetchHeaderWrite();
    auto header           = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
    header->root_page_id_ = child_page_id;
    header->tree_height_ -= 1;
    return true;
  }
  return false;
}

auto BPTreeIndex::Search(const Record &key) -> std::vector<RID>
{
  std::shared_lock lock(index_latch_);
  const page_id_t  leaf_page_id = FindLeafPage(key);
  if (leaf_page_id == INVALID_PAGE_ID) {
    return {};
  }
  auto       leaf_guard = buffer_pool_manager_->FetchPageRead(index_id_, leaf_page_id);
  const auto *leaf_page = AsLeaf(PageContentPtr(leaf_guard.GetData()));
  return leaf_page->Lookup(key, key_schema_);
}

auto BPTreeIndex::SearchRange(const Record &low_key, const Record &high_key) -> std::vector<RID>
{
  std::shared_lock lock(index_latch_);
  std::vector<RID> results;
  if (Record::Compare(low_key, high_key) > 0) {
    return results;
  }
  page_id_t leaf_page_id = FindLeafPageForRange(low_key, true);
  while (leaf_page_id != INVALID_PAGE_ID) {
    page_id_t next_leaf_page_id = INVALID_PAGE_ID;
    {
      auto       leaf_guard = buffer_pool_manager_->FetchPageRead(index_id_, leaf_page_id);
      const auto *leaf_page = AsLeaf(PageContentPtr(leaf_guard.GetData()));
      for (int i = leaf_page->LowerBound(low_key, key_schema_); i < leaf_page->GetSize(); ++i) {
        if (CompareWithKey(leaf_page->KeyAt(i), high_key, key_schema_) > 0) {
          return results;
        }
        results.push_back(leaf_page->ValueAt(i));
      }
      next_leaf_page_id = leaf_page->GetNextPageId();
    }
    leaf_page_id = next_leaf_page_id;
  }
  return results;
}

// Iterator implementation
BPTreeIndex::BPTreeIterator::BPTreeIterator(BPTreeIndex *tree, page_id_t leaf_page_id, int index)
    : tree_(tree), leaf_page_id_(leaf_page_id), index_(index)
{}

auto BPTreeIndex::BPTreeIterator::IsValid() -> bool
{
  if (leaf_page_id_ == INVALID_PAGE_ID) {
    return false;
  }
  auto       leaf_guard = tree_->buffer_pool_manager_->FetchPageRead(tree_->index_id_, leaf_page_id_);
  const auto *leaf_page = AsLeaf(PageContentPtr(leaf_guard.GetData()));
  return index_ >= 0 && index_ < leaf_page->GetSize();
}

void BPTreeIndex::BPTreeIterator::Next()
{
  if (leaf_page_id_ == INVALID_PAGE_ID) {
    return;
  }
  auto       leaf_guard = tree_->buffer_pool_manager_->FetchPageRead(tree_->index_id_, leaf_page_id_);
  const auto *leaf_page = AsLeaf(PageContentPtr(leaf_guard.GetData()));
  if (index_ + 1 < leaf_page->GetSize()) {
    ++index_;
    return;
  }
  // 当前叶页读完：沿叶链走
  leaf_page_id_ = leaf_page->GetNextPageId();
  index_        = 0;
}

auto BPTreeIndex::BPTreeIterator::GetKey() -> Record
{
  auto       leaf_guard = tree_->buffer_pool_manager_->FetchPageRead(tree_->index_id_, leaf_page_id_);
  const auto *leaf_page = AsLeaf(PageContentPtr(leaf_guard.GetData()));
  return MakeKey(leaf_page->KeyAt(index_), tree_->key_schema_);
}

auto BPTreeIndex::BPTreeIterator::GetRID() -> RID
{
  auto       leaf_guard = tree_->buffer_pool_manager_->FetchPageRead(tree_->index_id_, leaf_page_id_);
  const auto *leaf_page = AsLeaf(PageContentPtr(leaf_guard.GetData()));
  return leaf_page->ValueAt(index_);
}

auto BPTreeIndex::Begin() -> std::unique_ptr<IIterator>
{
  std::shared_lock lock(index_latch_);
  page_id_t        page_id = INVALID_PAGE_ID;
  {
    auto header_guard = FetchHeaderRead();
    page_id           = reinterpret_cast<const BPTreeIndexHeader *>(header_guard.GetData())->root_page_id_;
  }
  if (page_id == INVALID_PAGE_ID) {
    return End();
  }
  // 沿最左子页一路下潜到叶页
  while (true) {
    auto        guard = buffer_pool_manager_->FetchPageRead(index_id_, page_id);
    const auto *page  = AsPage(PageContentPtr(guard.GetData()));
    if (page->IsLeaf()) {
      break;
    }
    page_id = reinterpret_cast<const BPTreeInternalPage *>(page)->ValueAt(0);
  }
  return std::make_unique<BPTreeIterator>(this, page_id, 0);
}

auto BPTreeIndex::Begin(const Record &key) -> std::unique_ptr<IIterator>
{
  std::shared_lock lock(index_latch_);
  page_id_t        leaf_page_id = FindLeafPageForRange(key, true);
  while (leaf_page_id != INVALID_PAGE_ID) {
    {
      auto       leaf_guard = buffer_pool_manager_->FetchPageRead(index_id_, leaf_page_id);
      const auto *leaf_page = AsLeaf(PageContentPtr(leaf_guard.GetData()));
      const int   pos       = leaf_page->LowerBound(key, key_schema_);
      if (pos < leaf_page->GetSize()) {
        return std::make_unique<BPTreeIterator>(this, leaf_page_id, pos);
      }
      leaf_page_id = leaf_page->GetNextPageId();
    }
  }
  return End();
}

auto BPTreeIndex::End() -> std::unique_ptr<IIterator>
{
  return std::make_unique<BPTreeIterator>(this, INVALID_PAGE_ID, 0);
}

void BPTreeIndex::Clear()
{
  std::unique_lock lock(index_latch_);
  page_id_t        root_page_id = INVALID_PAGE_ID;
  {
    auto header_guard = FetchHeaderRead();
    root_page_id      = reinterpret_cast<const BPTreeIndexHeader *>(header_guard.GetData())->root_page_id_;
  }
  if (root_page_id != INVALID_PAGE_ID) {
    ClearPage(root_page_id);
  }
  auto header_guard     = FetchHeaderWrite();
  auto header           = reinterpret_cast<BPTreeIndexHeader *>(header_guard.GetMutableData());
  header->root_page_id_ = INVALID_PAGE_ID;
  header->tree_height_  = 0;
  header->num_entries_  = 0;
}

void BPTreeIndex::ClearPage(page_id_t page_id)
{
  bool                  is_leaf = false;
  std::vector<page_id_t> children;
  {
    auto       page_guard = buffer_pool_manager_->FetchPageRead(index_id_, page_id);
    const auto *page      = AsPage(PageContentPtr(page_guard.GetData()));
    is_leaf               = page->IsLeaf();
    if (!is_leaf) {
      const auto *internal = reinterpret_cast<const BPTreeInternalPage *>(page);
      for (int i = 0; i < internal->GetSize(); ++i) {
        children.push_back(internal->ValueAt(i));
      }
    }
  }
  for (const auto child : children) {
    ClearPage(child);
  }
  DeletePage(page_id);
}

auto BPTreeIndex::IsEmpty() -> bool
{
  auto header_guard = FetchHeaderRead();
  return reinterpret_cast<const BPTreeIndexHeader *>(header_guard.GetData())->root_page_id_ == INVALID_PAGE_ID;
}

auto BPTreeIndex::Size() -> size_t
{
  auto header_guard = FetchHeaderRead();
  return reinterpret_cast<const BPTreeIndexHeader *>(header_guard.GetData())->num_entries_;
}

auto BPTreeIndex::GetHeight() -> int
{
  auto header_guard = FetchHeaderRead();
  return static_cast<int>(reinterpret_cast<const BPTreeIndexHeader *>(header_guard.GetData())->tree_height_);
}

auto BPTreeIndex::CompareKeys(const Record &key1, const Record &key2) const -> int
{
  return Record::Compare(key1, key2);
}

}  // namespace njudb
