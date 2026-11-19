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

#include <iterator>

#include "lru_replacer.h"
#include "common/config.h"
#include "../common/error.h"
namespace njudb {

LRUReplacer::LRUReplacer() : cur_size_(0), max_size_(BUFFER_POOL_SIZE) {}

auto LRUReplacer::Victim(frame_id_t *frame_id) -> bool
{
  std::lock_guard<std::mutex> lock(latch_);
  for (auto it = lru_list_.begin(); it != lru_list_.end(); ++it) {
    if (!it->second) {
      continue;
    }
    *frame_id = it->first;
    lru_hash_.erase(it->first);
    lru_list_.erase(it);
    --cur_size_;
    return true;
  }
  return false;
}

void LRUReplacer::Pin(frame_id_t frame_id)
{
  std::lock_guard<std::mutex> lock(latch_);
  auto it = lru_hash_.find(frame_id);
  if (it == lru_hash_.end()) {
    lru_list_.emplace_back(frame_id, false);
    lru_hash_.emplace(frame_id, std::prev(lru_list_.end()));
    return;
  }
  if (it->second->second) {
    it->second->second = false;
    --cur_size_;
  }
  lru_list_.splice(lru_list_.end(), lru_list_, it->second);
}

void LRUReplacer::Unpin(frame_id_t frame_id)
{
  std::lock_guard<std::mutex> lock(latch_);
  auto it = lru_hash_.find(frame_id);
  if (it == lru_hash_.end() || it->second->second) {
    return;
  }
  it->second->second = true;
  ++cur_size_;
}

void LRUReplacer::Remove(frame_id_t frame_id)
{
  std::lock_guard<std::mutex> lock(latch_);
  auto it = lru_hash_.find(frame_id);
  if (it == lru_hash_.end()) {
    return;
  }
  if (it->second->second) {
    --cur_size_;
  }
  lru_list_.erase(it->second);
  lru_hash_.erase(it);
}

auto LRUReplacer::Size() -> size_t
{
  std::lock_guard<std::mutex> lock(latch_);
  return cur_size_;
}

}  // namespace njudb
