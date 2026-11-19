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

#include "lru_k_replacer.h"
#include "common/config.h"
#include "../common/error.h"

namespace njudb {

// k == 0 would leave the access history empty and make the backward distance
// undefined; treat it as LRU (k == 1), which is what an unset replacer_lru_k means
LRUKReplacer::LRUKReplacer(size_t k, size_t max_size) : max_size_(max_size), k_(k == 0 ? 1 : k) {}

auto LRUKReplacer::Victim(frame_id_t *frame_id) -> bool
{
  std::lock_guard<std::mutex> lock(latch_);
  if (cur_size_ == 0) {
    return false;
  }
  frame_id_t         victim       = INVALID_FRAME_ID;
  unsigned long long victim_dist  = 0;
  timestamp_t        victim_first = 0;
  for (const auto &[fid, node] : node_store_) {
    if (!node.IsEvictable()) {
      continue;
    }
    auto dist  = node.GetBackwardKDistance(cur_ts_);
    auto first = node.GetFirstAccessTimestamp();
    if (victim == INVALID_FRAME_ID || dist > victim_dist ||
        (dist == victim_dist && first < victim_first) ||
        (dist == victim_dist && first == victim_first && fid < victim)) {
      victim       = fid;
      victim_dist  = dist;
      victim_first = first;
    }
  }
  if (victim == INVALID_FRAME_ID) {
    return false;
  }
  *frame_id = victim;
  node_store_.erase(victim);
  --cur_size_;
  return true;
}

void LRUKReplacer::Pin(frame_id_t frame_id)
{
  std::lock_guard<std::mutex> lock(latch_);
  auto                        it = node_store_.find(frame_id);
  if (it == node_store_.end()) {
    it = node_store_.emplace(frame_id, LRUKNode(frame_id, k_)).first;
  }
  it->second.AddHistory(cur_ts_++);
  // the frame is in use again, so it must not be victimized
  if (it->second.IsEvictable()) {
    it->second.SetEvictable(false);
    --cur_size_;
  }
}

void LRUKReplacer::Unpin(frame_id_t frame_id)
{
  std::lock_guard<std::mutex> lock(latch_);
  auto                        it = node_store_.find(frame_id);
  if (it == node_store_.end() || it->second.IsEvictable()) {
    return;
  }
  it->second.SetEvictable(true);
  ++cur_size_;
}

void LRUKReplacer::Remove(frame_id_t frame_id)
{
  std::lock_guard<std::mutex> lock(latch_);
  auto it = node_store_.find(frame_id);
  if (it == node_store_.end()) {
    return;
  }
  if (it->second.IsEvictable()) {
    --cur_size_;
  }
  node_store_.erase(it);
}

auto LRUKReplacer::Size() -> size_t
{
  std::lock_guard<std::mutex> lock(latch_);
  return cur_size_;
}

}  // namespace njudb
