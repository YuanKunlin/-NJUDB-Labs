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
// Created by ziqi on 2024/7/17.
//
#include "buffer_pool_manager.h"
#include "page_guard.h"
#include "replacer/lru_replacer.h"
#include "replacer/lru_k_replacer.h"
#include "log/recovery.h"  // For FilePageHash

#include "../../../common/error.h"
#include "../../common/config.h"

namespace njudb {

BufferPoolManager::BufferPoolManager(DiskManager *disk_manager, njudb::LogManager *log_manager, size_t replacer_lru_k)
    : disk_manager_(disk_manager), log_manager_(log_manager)
{
  if (REPLACER == "LRUReplacer") {
    replacer_ = std::make_unique<LRUReplacer>();
  } else if (REPLACER == "LRUKReplacer") {
    replacer_ = std::make_unique<LRUKReplacer>(replacer_lru_k);
  } else {
    NJUDB_FATAL("Unknown replacer: " + REPLACER);
  }
  // init free_set_
  for (frame_id_t i = 0; i < static_cast<int>(BUFFER_POOL_SIZE); i++) {
    free_list_.push_back(i);
  }
}

auto BufferPoolManager::FetchPage(file_id_t fid, page_id_t pid) -> Page *
{
  std::lock_guard<std::mutex> latch_guard(latch_);
  const fid_pid_t             key{fid, pid};
  const auto                  lookup_it = page_frame_lookup_.find(key);
  if (lookup_it != page_frame_lookup_.end()) {
    auto &frame = frames_[lookup_it->second];
    frame.Pin();
    replacer_->Pin(lookup_it->second);
    return frame.GetPage();
  }
  const frame_id_t frame_id = GetAvailableFrame();
  UpdateFrame(frame_id, fid, pid);
  return frames_[frame_id].GetPage();
}

auto BufferPoolManager::UnpinPage(file_id_t fid, page_id_t pid, bool is_dirty) -> bool
{
  std::lock_guard<std::mutex> latch_guard(latch_);
  const auto                  lookup_it = page_frame_lookup_.find({fid, pid});
  if (lookup_it == page_frame_lookup_.end()) {
    return false;
  }
  auto &frame = frames_[lookup_it->second];
  if (!frame.InUse()) {
    return false;
  }
  if (is_dirty) {
    frame.SetDirty(true);
  }
  frame.Unpin();
  if (!frame.InUse()) {
    replacer_->Unpin(lookup_it->second);
  }
  return true;
}

auto BufferPoolManager::GetDirtyPages() -> std::unordered_map<std::pair<std::string, page_id_t>, lsn_t, FilePageHash>
{
  std::lock_guard<std::mutex> latch_guard(latch_);
  std::unordered_map<std::pair<std::string, page_id_t>, lsn_t, FilePageHash> dirty_pages;
  
  for (auto &[fid_pid, frame_id] : page_frame_lookup_) {
    auto &frame = frames_[frame_id];
    if (frame.IsDirty() && IsWALManagedPage(fid_pid.fid, fid_pid.pid) &&
        frame.GetRecLSN() != INVALID_LSN) {
      std::string filename = disk_manager_->GetFileName(fid_pid.fid);
      auto dpt_key = std::make_pair(filename, fid_pid.pid);
      dirty_pages[dpt_key] = frame.GetRecLSN();
    }
  }
  
  return dirty_pages;
}

auto BufferPoolManager::DeletePage(file_id_t fid, page_id_t pid) -> bool
{
  std::lock_guard<std::mutex> latch_guard(latch_);
  const auto                  lookup_it = page_frame_lookup_.find({fid, pid});
  if (lookup_it == page_frame_lookup_.end()) {
    return true;
  }
  const frame_id_t frame_id = lookup_it->second;
  auto            &frame    = frames_[frame_id];
  if (frame.InUse()) {
    return false;
  }
  if (frame.IsDirty()) {
    FlushFrameLocked(frame_id);
  }
  page_frame_lookup_.erase(lookup_it);
  // drop the frame from the replacer so the next page loaded here does not
  // inherit the access history of the deleted one
  replacer_->Remove(frame_id);
  frame.Reset();
  free_list_.push_back(frame_id);
  return true;
}

auto BufferPoolManager::DeleteAllPages(file_id_t fid) -> bool
{
  std::lock_guard<std::mutex> latch_guard(latch_);
  std::vector<frame_id_t>     frame_ids;
  for (const auto &[key, frame_id] : page_frame_lookup_) {
    if (key.fid == fid) {
      if (frames_[frame_id].InUse()) {
        return false;
      }
      frame_ids.push_back(frame_id);
    }
  }
  for (const frame_id_t frame_id : frame_ids) {
    auto &frame = frames_[frame_id];
    if (frame.IsDirty()) {
      FlushFrameLocked(frame_id);
    }
    page_frame_lookup_.erase({frame.GetPage()->GetFileId(), frame.GetPage()->GetPageId()});
    replacer_->Remove(frame_id);
    frame.Reset();
    free_list_.push_back(frame_id);
  }
  return true;
}

auto BufferPoolManager::FlushPage(file_id_t fid, page_id_t pid) -> bool
{
  std::lock_guard<std::mutex> latch_guard(latch_);
  const auto                  lookup_it = page_frame_lookup_.find({fid, pid});
  if (lookup_it == page_frame_lookup_.end()) {
    return false;
  }
  // keep the same rule as FlushAllPages: a page that is still pinned may be
  // under modification, so it must not be written out here
  return FlushFrameLocked(lookup_it->second);
}

auto BufferPoolManager::FlushAllPages(file_id_t fid) -> bool
{
  std::lock_guard<std::mutex> latch_guard(latch_);
  bool                        all_flushed = true;
  for (const auto &[key, frame_id] : page_frame_lookup_) {
    if (key.fid != fid) {
      continue;
    }
    if (!FlushFrameLocked(frame_id)) {
      all_flushed = false;
    }
  }
  return all_flushed;
}

auto BufferPoolManager::FlushAllPages() -> bool
{
  std::lock_guard<std::mutex> latch_guard(latch_);
  bool all_flushed = true;
  for (frame_id_t frame_id = 0; frame_id < static_cast<frame_id_t>(frames_.size()); ++frame_id) {
    if (!FlushFrameLocked(frame_id)) {
      all_flushed = false;
    }
  }
  return all_flushed;
}

auto BufferPoolManager::GetAvailableFrame() -> frame_id_t
{
  if (!free_list_.empty()) {
    const frame_id_t frame_id = free_list_.front();
    free_list_.pop_front();
    return frame_id;
  }
  frame_id_t frame_id = INVALID_FRAME_ID;
  if (!replacer_->Victim(&frame_id)) {
    NJUDB_THROW(NJUDB_NO_FREE_FRAME, "no frame can be evicted");
  }
  return frame_id;
}

void BufferPoolManager::UpdateFrame(frame_id_t frame_id, file_id_t fid, page_id_t pid)
{
  auto &frame = frames_[frame_id];
  auto *page  = frame.GetPage();
  // the frame is either free (Reset) or a victim, both have pin_count == 0;
  // a pinned frame must never be overwritten
  NJUDB_ASSERT(!frame.InUse(), "UpdateFrame on a pinned frame");
  const auto old_fid = page->GetFileId();
  const auto old_pid = page->GetPageId();
  if (frame.IsDirty()) {
    FlushFrameLocked(frame_id);
  }
  if (old_fid != INVALID_FILE_ID) {
    page_frame_lookup_.erase({old_fid, old_pid});
  }
  page->SetFilePageId(fid, pid);
  disk_manager_->ReadPage(fid, pid, page->GetData());
  frame.Pin();
  replacer_->Pin(frame_id);
  page_frame_lookup_[{fid, pid}] = frame_id;
}

auto BufferPoolManager::FlushFrameLocked(frame_id_t frame_id) -> bool
{
  auto &frame = frames_[frame_id];
  if (!frame.IsDirty()) {
    return true;
  }
  if (frame.InUse()) {
    return false;
  }

  auto *page = frame.GetPage();
  if (log_manager_ != nullptr && IsWALManagedPage(page->GetFileId(), page->GetPageId())) {
    const auto page_lsn = page->GetLsn();
    if (page_lsn != INVALID_LSN) {
      log_manager_->FlushLogUpTo(page_lsn);
    }
  }
  disk_manager_->WritePage(page->GetFileId(), page->GetPageId(), page->GetData());
  frame.SetDirty(false);
  return true;
}

auto BufferPoolManager::IsWALManagedPage(file_id_t fid, page_id_t pid) const -> bool
{
  if (pid == FILE_HEADER_PAGE_ID) {
    return false;
  }
  const auto filename = disk_manager_->GetFileName(fid);
  return filename.size() >= TAB_SUFFIX.size() &&
         filename.compare(filename.size() - TAB_SUFFIX.size(), TAB_SUFFIX.size(), TAB_SUFFIX) == 0;
}

auto BufferPoolManager::GetFrame(file_id_t fid, page_id_t pid) -> Frame *
{
  const auto it = page_frame_lookup_.find({fid, pid});
  return it == page_frame_lookup_.end() ? nullptr : &frames_[it->second];
}

auto BufferPoolManager::FetchPageRead(file_id_t fid, page_id_t pid) -> ReadPageGuard
{
  Page *page = FetchPage(fid, pid);
  return {this, page, fid, pid};
}

auto BufferPoolManager::FetchPageWrite(file_id_t fid, page_id_t pid) -> WritePageGuard
{
  Page *page = FetchPage(fid, pid);
  return {this, page, fid, pid};
}

}  // namespace njudb
