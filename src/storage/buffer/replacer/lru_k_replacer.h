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

#ifndef NJUDB_LRU_K_REPLACER_H
#define NJUDB_LRU_K_REPLACER_H
#include <limits>
#include <list>
#include <mutex>
#include <unordered_map>
#include "replacer.h"
#include "common/config.h"
#include "../common/error.h"

namespace njudb {

class LRUKReplacer : public Replacer
{
public:
  explicit LRUKReplacer(size_t k, size_t max_size = BUFFER_POOL_SIZE);

  ~LRUKReplacer() override = default;

  auto Victim(frame_id_t *frame_id) -> bool override;

  void Pin(frame_id_t frame_id) override;

  void Unpin(frame_id_t frame_id) override;

  void Remove(frame_id_t frame_id) override;

  auto Size() -> size_t override;

private:
  class LRUKNode
  {
  public:
    LRUKNode() = default;

    explicit LRUKNode(frame_id_t fid, size_t k) : fid_(fid), k(k), is_evictable_(false) {}

    void AddHistory(timestamp_t ts)
    {
      history_.push_back(ts);
      // only the last k accesses matter for the backward k distance
      if (history_.size() > k) {
        history_.pop_front();
      }
    }

    /**
     * Get the distance between the current timestamp and the k-th timestamp in the history,
     * think: why return type is unsigned long long?
     * @param cur_ts
     * @return
     */
    auto GetBackwardKDistance(timestamp_t cur_ts) const -> unsigned long long
    {
      // fewer than k accesses: the frame has no k-th backward reference, so it is
      // treated as infinitely far away. unsigned long long is used so that this
      // "infinity" is representable and always larger than any real distance.
      if (history_.size() < k) {
        return std::numeric_limits<unsigned long long>::max();
      }
      return cur_ts - history_.front();
    }

    /**
     * Timestamp of the first access. Only meaningful while the history is not full,
     * where it is used to break ties between frames whose distance is infinite.
     */
    [[nodiscard]] auto GetFirstAccessTimestamp() const -> timestamp_t
    {
      return history_.empty() ? 0 : history_.front();
    }

    [[nodiscard]] auto IsEvictable() const -> bool { return is_evictable_; }

    auto SetEvictable(bool set_evictable) -> void { is_evictable_ = set_evictable; }

  private:
    std::list<timestamp_t> history_;
    frame_id_t             fid_{INVALID_FRAME_ID};
    size_t                 k{};
    bool                   is_evictable_{};
  };

private:
  std::unordered_map<frame_id_t, LRUKNode> node_store_;  // frame_id -> LRUKNode
  size_t                                   cur_ts_{0};
  size_t                                   cur_size_{0};  // number of evictable frames
  size_t                                   max_size_;     // maximum number of frames that can be stored
  size_t                                   k_;            // k for LRU-k
  std::mutex                               latch_;        // mutex for curr_size_, node_store_, and curr_timestamp_
};
}  // namespace njudb

#endif  // NJUDB_LRU_K_REPLACER_H
