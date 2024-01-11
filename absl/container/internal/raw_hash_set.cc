// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/container/internal/raw_hash_set.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/container/internal/container_memory.h"
#include "absl/hash/hash.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace container_internal {

// We have space for `growth_left` before a single block of control bytes. A
// single block of empty control bytes for tables without any slots allocated.
// This enables removing a branch in the hot path of find(). In order to ensure
// that the control bytes are aligned to 16, we have 16 bytes before the control
// bytes even though growth_left only needs 8.
constexpr ctrl_t ZeroCtrlT() { return static_cast<ctrl_t>(0); }
alignas(16) ABSL_CONST_INIT ABSL_DLL const ctrl_t kEmptyGroup[32] = {
    ZeroCtrlT(),       ZeroCtrlT(),    ZeroCtrlT(),    ZeroCtrlT(),
    ZeroCtrlT(),       ZeroCtrlT(),    ZeroCtrlT(),    ZeroCtrlT(),
    ZeroCtrlT(),       ZeroCtrlT(),    ZeroCtrlT(),    ZeroCtrlT(),
    ZeroCtrlT(),       ZeroCtrlT(),    ZeroCtrlT(),    ZeroCtrlT(),
    ctrl_t::kSentinel, ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty,
    ctrl_t::kEmpty,    ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty,
    ctrl_t::kEmpty,    ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty,
    ctrl_t::kEmpty,    ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty};

#ifdef ABSL_INTERNAL_NEED_REDUNDANT_CONSTEXPR_DECL
constexpr size_t Group::kWidth;
#endif

namespace {

// Returns "random" seed.
inline size_t RandomSeed() {
#ifdef ABSL_HAVE_THREAD_LOCAL
  static thread_local size_t counter = 0;
  // On Linux kernels >= 5.4 the MSAN runtime has a false-positive when
  // accessing thread local storage data from loaded libraries
  // (https://github.com/google/sanitizers/issues/1265), for this reason counter
  // needs to be annotated as initialized.
  ABSL_ANNOTATE_MEMORY_IS_INITIALIZED(&counter, sizeof(size_t));
  size_t value = ++counter;
#else   // ABSL_HAVE_THREAD_LOCAL
  static std::atomic<size_t> counter(0);
  size_t value = counter.fetch_add(1, std::memory_order_relaxed);
#endif  // ABSL_HAVE_THREAD_LOCAL
  return value ^ static_cast<size_t>(reinterpret_cast<uintptr_t>(&counter));
}

bool ShouldRehashForBugDetection(const ctrl_t* ctrl, size_t capacity) {
  // Note: we can't use the abseil-random library because abseil-random
  // depends on swisstable. We want to return true with probability
  // `min(1, RehashProbabilityConstant() / capacity())`. In order to do this,
  // we probe based on a random hash and see if the offset is less than
  // RehashProbabilityConstant().
  return probe(ctrl, capacity, absl::HashOf(RandomSeed())).offset() <
         RehashProbabilityConstant();
}

}  // namespace

GenerationType* EmptyGeneration() {
  if (SwisstableGenerationsEnabled()) {
    constexpr size_t kNumEmptyGenerations = 1024;
    static constexpr GenerationType kEmptyGenerations[kNumEmptyGenerations]{};
    return const_cast<GenerationType*>(
        &kEmptyGenerations[RandomSeed() % kNumEmptyGenerations]);
  }
  return nullptr;
}

bool CommonFieldsGenerationInfoEnabled::
    should_rehash_for_bug_detection_on_insert(const ctrl_t* ctrl,
                                              size_t capacity) const {
  if (reserved_growth_ == kReservedGrowthJustRanOut) return true;
  if (reserved_growth_ > 0) return false;
  return ShouldRehashForBugDetection(ctrl, capacity);
}

bool CommonFieldsGenerationInfoEnabled::should_rehash_for_bug_detection_on_move(
    const ctrl_t* ctrl, size_t capacity) const {
  return ShouldRehashForBugDetection(ctrl, capacity);
}

bool ShouldInsertBackwards(size_t hash, const ctrl_t* ctrl) {
  // To avoid problems with weak hashes and single bit tests, we use % 13.
  // TODO(kfm,sbenza): revisit after we do unconditional mixing
  return (H1(hash, ctrl) ^ RandomSeed()) % 13 > 6;
}

void ConvertDeletedToEmptyAndFullToDeleted(ctrl_t* ctrl, size_t capacity) {
  assert(ctrl[capacity] == ctrl_t::kSentinel);
  assert(IsValidCapacity(capacity));
  for (ctrl_t* pos = ctrl; pos < ctrl + capacity; pos += Group::kWidth) {
    Group{pos}.ConvertSpecialToEmptyAndFullToDeleted(pos);
  }
  // Copy the cloned ctrl bytes.
  std::memcpy(ctrl + capacity + 1, ctrl, NumClonedBytes());
  ctrl[capacity] = ctrl_t::kSentinel;
}
// Extern template instantiation for inline function.
template FindInfo find_first_non_full(const CommonFields&, size_t);

FindInfo find_first_non_full_outofline(const CommonFields& common,
                                       size_t hash) {
  return find_first_non_full(common, hash);
}

// Returns the address of the slot just after slot assuming each slot has the
// specified size.
static inline void* NextSlot(void* slot, size_t slot_size) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) + slot_size);
}

// Returns the address of the slot just before slot assuming each slot has the
// specified size.
static inline void* PrevSlot(void* slot, size_t slot_size) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) - slot_size);
}

bool find_key(
  CommonFields& common,
  const PolicyFunctions& policy, 
  const size_t key,
  const size_t hash) {
  void *ptr;
  auto seq = probe(common, hash);
  ctrl_t* ctrl = common.control();
  void* slot_array = common.slot_array();
  const size_t slot_size = policy.slot_size;

  while (true) {
    Group g{ctrl + seq.offset()};
    for (uint32_t i : g.Match(H2(hash))) {
      ptr = SlotAddress(slot_array, (seq.offset() + i) & common.capacity(), slot_size);
      if (*(size_t *)(ptr) == key) return true;
    }
    if (ABSL_PREDICT_TRUE(g.MaskEmpty())) break;
    seq.next();
  }
  return false;
}

bool checkAllReachable(
  CommonFields& common,
  const PolicyFunctions& policy) {
  void* set = &common;
  ctrl_t* ctrl = common.control();
  void* slot_array = common.slot_array();
  auto hasher = policy.hash_slot;
  const size_t slot_size = policy.slot_size;
  size_t item_count = 0;

  // printf(" Tombstone Count: %ld\n", common.TombstonesCount());
  for (size_t i=0; i<common.capacity(); i++) {
    if (ctrl[i]==ctrl_t::kDeleted || ctrl[i]==ctrl_t::kEmpty) {
      continue;
    }
    item_count++;
    void* target_ptr = SlotAddress(slot_array, i, slot_size);
    if(!(find_key(common, policy, *(size_t *)target_ptr, (*hasher)(set, target_ptr))==true)) {
      printf("Failed in checkAllReachable! %ld (homeslot: )%ld\n", i, probe(common, (*hasher)(set, target_ptr)).offset());
      assert(false);
    }
  }

  assert(item_count == common.size());
  // printf("AllReachable!\n");
  return true;
}

bool checkRangeReachable(
  CommonFields& common,
  const PolicyFunctions& policy,
  size_t start_offset,
  size_t end_offset) {
  void* set = &common;
  ctrl_t* ctrl = common.control();
  void* slot_array = common.slot_array();
  auto hasher = policy.hash_slot;
  const size_t slot_size = policy.slot_size;

  size_t tc_count = 0;
  size_t empty_count = 0;
  size_t count = 0;

  // printf(" Tombstone Count: %ld\n", common.TombstonesCount());
  for (size_t i=start_offset; i!=end_offset; i++) {
    if (i==common.capacity()) {
      i = -1;
      continue;
    }
    count++;
    if (ctrl[i]==ctrl_t::kDeleted) {
      tc_count++;
      continue;
    } else if (ctrl[i]==ctrl_t::kEmpty) {
      // printf("   %ld empty\n", i);
      empty_count++;
      continue;
    }
    void* target_ptr = SlotAddress(slot_array, i, slot_size);
    if(!(find_key(common, policy, *(size_t *)target_ptr, (*hasher)(set, target_ptr))==true)) {
      printf("Failed in checkAllReachable! %ld (homeslot: )%ld\n", i, probe(common, (*hasher)(set, target_ptr)).offset());
      assert(false);
    }
  }
  // printf("Range: %ld tc: %ld empty: %ld\n", count, tc_count, empty_count);
  return true;
}

void ConvertDeletedToEmptyAndFullToDeletedInRange(ctrl_t* ctrl, size_t start_offset, size_t end_offset, size_t capacity) {
  assert(ctrl[capacity] == ctrl_t::kSentinel);
  assert(IsValidCapacity(capacity));
  size_t end_pos = end_offset - Group::kWidth;
  if (end_offset < Group::kWidth) {
    end_pos = 0;
  }
  size_t pos = start_offset;
  while (pos < end_pos) {
    Group{ctrl + pos}.ConvertSpecialToEmptyAndFullToDeleted(ctrl + pos);
    pos = pos + Group::kWidth;
    pos = pos & capacity;
  }

  while (pos != end_offset) {
    if (ctrl[pos] == ctrl_t::kSentinel) {
      pos = 0;
      continue;
    }
    if (ctrl[pos] == ctrl_t::kDeleted) ctrl[pos] = ctrl_t::kEmpty;
    else if (ctrl[pos] != ctrl_t::kEmpty) ctrl[pos] = ctrl_t::kDeleted;
    pos = pos + 1;
  }
  // Copy the cloned ctrl bytes.
  std::memcpy(ctrl + capacity + 1, ctrl, NumClonedBytes());
  ctrl[capacity] = ctrl_t::kSentinel;
}

void ClearTombstonesInRangeByRehashing(
  CommonFields& common,
  const PolicyFunctions& policy, 
  size_t start_offset,
  size_t end_offset,
  void *tmp_space) {

  if (start_offset == end_offset) {
    abort();
  }
  void* set = &common;
  void* slot_array = common.slot_array();
  const size_t capacity = common.capacity();
  assert(IsValidCapacity(capacity));
  assert(!is_small(capacity));
  // assert start_offset is empty
  // assert end_offset is empty
  // Algorithm:
  // - mark all DELETED slots as EMPTY
  // - mark all FULL slots as DELETED (There are no full slots at this point, only empties and tombstones. tombstones represent items.)
  // - for each slot marked as DELETED  (basically for each item)
  //     hash = Hash(element)
  //     target = find_first_non_full(hash)
  //     if target is in the same group
  //       mark slot as FULL
  //     else if target is EMPTY (move item back)
  //       transfer element to target
  //       mark slot as EMPTY
  //       mark target as FULL
  //     else if target is DELETED
  //       swap current element with target element
  //       mark target as FULL
  //       repeat procedure for current slot with moved from element (target)
  // printf("Rebuilding [%ld %ld]\n", start_offset, end_offset);
  ctrl_t* ctrl = common.control();
  const size_t slot_size = policy.slot_size;
  auto hasher = policy.hash_slot;
  auto transfer = policy.transfer;
  bool range_broken = end_offset < start_offset;
  // printf("Before rebuild\n");
  // checkAllReachable(common, policy);
  // checkRangeReachable(common, policy, 0, capacity);
  // checkRangeReachable(common, policy, start_offset, end_offset);
  if (end_offset > start_offset) {
    ConvertDeletedToEmptyAndFullToDeletedInRange(ctrl, start_offset, end_offset, capacity);
  }
  else {
    ConvertDeletedToEmptyAndFullToDeletedInRange(ctrl, start_offset, capacity, capacity);
    ConvertDeletedToEmptyAndFullToDeletedInRange(ctrl, 0, end_offset, capacity);
  }
  // printf("After rebuild\n");

  // Check no full items between start_offset and end_offset.
  size_t tpos = start_offset;
  while (tpos != end_offset) {
    if (tpos != capacity) {
      assert(ctrl[tpos] == ctrl_t::kEmpty || ctrl[tpos] == ctrl_t::kDeleted);
    } else {
      assert(ctrl[tpos] == ctrl_t::kSentinel);
    }
    tpos++;
    tpos = tpos & capacity;
  }

  size_t pos = start_offset;
  void* slot_ptr = SlotAddress(slot_array, pos, slot_size);
  assert(ctrl[end_offset] == ctrl_t::kEmpty);
  while (true) {
    if (pos == end_offset) {
      assert(ctrl[end_offset] == ctrl_t::kEmpty);
      break;
    }
    if (ABSL_PREDICT_FALSE(ctrl[pos] == ctrl_t::kSentinel)) {
      pos = 0;
      slot_ptr = SlotAddress(slot_array, pos, slot_size);
      continue;
    }
    assert(slot_ptr == SlotAddress(slot_array, pos, slot_size));
    if (!IsDeleted(ctrl[pos])) {
      // printf("%ld empty\n", pos);
      pos = pos + 1;
      slot_ptr = NextSlot(slot_ptr, slot_size);
      continue;
    }
    const size_t hash = (*hasher)(set, slot_ptr);
    const FindInfo target = find_first_non_full(common, hash);
    const size_t new_i = target.offset;
    const size_t probe_offset = probe(common, hash).offset();
    const auto probe_index = [probe_offset, capacity](size_t pos) {
      return ((pos - probe_offset) & capacity) / Group::kWidth;
    };

    // Element doesn't move.
    if (ABSL_PREDICT_TRUE(probe_index(new_i) == probe_index(pos))) {
      SetCtrl(common, pos, H2(hash), slot_size);
      pos = pos + 1;
      slot_ptr = NextSlot(slot_ptr, slot_size);
      continue;
    }

    void* new_slot_ptr = SlotAddress(slot_array, new_i, slot_size);
    if (IsEmpty(ctrl[new_i])) {
      // Transfer element to the empty spot.
      // SetCtrl poisons/unpoisons the slots so we have to call it at the
      // right time.
      SetCtrl(common, new_i, H2(hash), slot_size);
      (*transfer)(set, new_slot_ptr, slot_ptr);
      SetCtrl(common, pos, ctrl_t::kEmpty, slot_size);
    } else {
      assert(IsDeleted(ctrl[new_i]));
      SetCtrl(common, new_i, H2(hash), slot_size);
      // Until we are done rehashing, DELETED marks previously FULL slots.
      // But if deleted is out
      if (!range_broken && (new_i > start_offset && new_i < end_offset)) {
        // Swap i and new_i elements.
        (*transfer)(set, tmp_space, new_slot_ptr);
        (*transfer)(set, new_slot_ptr, slot_ptr);
        (*transfer)(set, slot_ptr, tmp_space);
        // repeat the processing of the ith slot
        --pos;
        slot_ptr = PrevSlot(slot_ptr, slot_size);
      } else if (range_broken && (new_i > start_offset || new_i < end_offset)) {
        // Swap i and new_i elements.
        (*transfer)(set, tmp_space, new_slot_ptr);
        (*transfer)(set, new_slot_ptr, slot_ptr);
        (*transfer)(set, slot_ptr, tmp_space);
        // repeat the processing of the ith slot
        --pos;
        slot_ptr = PrevSlot(slot_ptr, slot_size);
      } else {
        (*transfer)(set, new_slot_ptr, slot_ptr);
        SetCtrl(common, pos, ctrl_t::kEmpty, slot_size);
      }
    }
    pos = pos + 1;
    slot_ptr = NextSlot(slot_ptr, slot_size);
  }
  for (size_t i=0; i < Group::kWidth; i++) {
    pos = pos + 1;
    // printf(" Checking extra %ld\n", pos);
    slot_ptr = NextSlot(slot_ptr, slot_size);
    if (pos == capacity) {
      pos = 0;
      slot_ptr = SlotAddress(slot_array, 0, slot_size);
    }
    if (!IsFull(ctrl[pos]))continue;

    const size_t hash = (*hasher)(set, slot_ptr);
    const size_t probe_offset = probe(common, hash).offset();
    if (!range_broken && probe_offset > end_offset) continue;
    if (range_broken && (probe_offset < start_offset && probe_offset > end_offset)) continue;

    // mark as tombstone and reinsert.
    SetCtrl(common, pos, ctrl_t::kDeleted, slot_size);
    const FindInfo target = find_first_non_full(common, hash);
    const size_t new_i = target.offset;
    const auto probe_index = [probe_offset, capacity](size_t pos) {
      return ((pos - probe_offset) & capacity) / Group::kWidth;
    };
    // If Same Group, don't transfer.
    if (ABSL_PREDICT_TRUE(probe_index(new_i) == probe_index(pos))) {
      SetCtrl(common, pos, H2(hash), slot_size);
      continue;
    }
    // Otherwise move to destination.
    void *new_slot_ptr = SlotAddress(slot_array, new_i, slot_size);
    (*transfer)(set, new_slot_ptr, slot_ptr);
    SetCtrl(common, new_i, H2(hash), slot_size);
  }
  // TODO: for Group::kWidth items after end, reinsert if homeslot is between
  // start and end offset.
  // checkRangeReachable(common, policy, start_offset, end_offset);
  // checkRangeReachable(common, policy, 0, capacity);
  // printf("After rebuilding\n");
  // checkAllReachable(common, policy);
}

void ClearTombstonesInRange(
  CommonFields& common,
  const PolicyFunctions& policy, 
  size_t start_offset,
  size_t end_offset) {
  // Clear all tombstones between start and offset.
  // Algorithm:
  // FOR each tombstone between [start_offset, end_offset] 
  // - scan ahead to find an item that fits here.
  // - if found, swap it in.
  // - else mark it as empty
  // TODO: For now we clear all tombstones, just to check for correctness.
  // Leaving in a lot of comments to help with debugging.
  // size_t outerProbeLength = 0;
  // size_t innerP obeLength = 0;
  void* set = &common;
  ctrl_t* ctrl = common.control();
  void* slot_array = common.slot_array();
  auto hasher = policy.hash_slot;
  auto transfer = policy.transfer;
  const size_t slot_size = policy.slot_size;
  const size_t capacity = common.capacity();

  void* candidate_ptr;
  void* tombstone_ptr;
  // void* target_ptr;
  size_t tombstone_slot;
  size_t candidate_slot;
  size_t candidate_hash;
  size_t candidate_home_slot;
  bool candidate_found;

  assert(ctrl[start_offset] == ctrl_t::kEmpty);
  assert(ctrl[end_offset] == ctrl_t::kEmpty);
  // printf("Clearing [%ld %ld] TC: %ld\n", start_offset, end_offset, common.TombstonesCount());
  // checkAllReachable(common, policy);

  // all items will have their homeslot between [candidate_hs_start, candidate_search_end].
  size_t candidate_hs_start = (start_offset - Group::kWidth) & capacity;
  size_t candidate_search_end = (end_offset + Group::kWidth) & capacity;

  // For every item we scan forward, it's home slot should lie between
  // [candidate_hs_start, tombstone] for it to be considered as a replacement
  // if tombstone < candidiate_hs_starrt, then that range is discontinous (wraps around).
  bool range_broken = start_offset < Group::kWidth;

  size_t slot = start_offset;
  while(slot != end_offset) {
    // checkAllReachable(common, policy);
    // outerProbeLength++;
    if (ctrl[slot] == ctrl_t::kSentinel) {
      slot++;
      slot = slot & capacity;
      range_broken = true;
      continue;
    }
    #if 0
    if (IsFull(ctrl[slot])) {
      target_ptr = SlotAddress(slot_array, (slot & capacity), slot_size);
      const size_t target_hash = (*hasher)(slot_array, target_ptr);
      const size_t target_home_slot = probe(common, target_hash).offset();
      if (candidate_search_end < candidate_search_end) {
        assert(target_home_slot >= candidate_search_end && target_home_slot <= candidate_search_end);
      } else {
        assert(target_home_slot >= candidate_search_end || target_home_slot <= candidate_search_end);
      }
      
      if(find_key(common, policy, *(size_t *)target_ptr, (*hasher)(set, target_ptr))==false) {
        printf("lost value somehow: %ld\n", *(size_t *)(target_ptr));
        assert(find_key(common, policy, *(size_t *)target_ptr, (*hasher)(set, target_ptr))==true);
      }
    }
    #endif
    if (IsDeleted(ctrl[slot])) {
      tombstone_slot = slot;
      tombstone_ptr = SlotAddress(slot_array, (tombstone_slot & capacity), slot_size);

      candidate_slot = slot;
      candidate_found = false;
      // size_t scan_length = 0;
      // printf("Trying to clear %ld [%ld %ld]\n", slot, candidate_hs_start, candidate_search_end);
      while (true) {
          // innerProbeLength++;
          candidate_slot++;
          candidate_slot = candidate_slot & capacity;
          if (candidate_slot == candidate_search_end) {
            // printf("Breaking because end found!\n");
            break;
          }
          if (ctrl[candidate_slot & capacity] == ctrl_t::kSentinel) {
            // printf("  Wrapped around! %ld\n", candidate_slot);
            // checkAllReachable(common, policy);
            // SetCtrl(common, (tombstone_slot & capacity), ctrl_t::kEmpty, slot_size);
            continue;
          }
          if(IsDeleted(ctrl[candidate_slot & capacity])) {
            continue;
          }
          if(IsEmpty(ctrl[candidate_slot & capacity])) {
            // printf("Skipping over an empty: %ld\n", candidate_slot);
            // candidate_search_end = candidate_slot + Group::kWidth;
            continue;
          }
          candidate_ptr = SlotAddress(slot_array, (candidate_slot & capacity), slot_size);
          candidate_hash = (*hasher)(set, candidate_ptr);
          candidate_home_slot = probe(common, candidate_hash).offset();
          bool viable_candidate;
          if (range_broken) {
            viable_candidate = (candidate_home_slot > candidate_hs_start || candidate_home_slot <= tombstone_slot);
          } else {
            viable_candidate = (candidate_home_slot > candidate_hs_start && candidate_home_slot <= tombstone_slot);
          }
          if (viable_candidate)  {
            // Transfer to new place
            // printf("  Before TC: %ld\n", common.TombstonesCount());
            SetCtrl(common, (tombstone_slot & capacity), H2(candidate_hash), slot_size);
            (*transfer)(set, tombstone_ptr, candidate_ptr);
            SetCtrl(common, (candidate_slot & capacity), ctrl_t::kDeleted, slot_size);
            candidate_found = true;
            // printf("  TC: %ld\n", common.TombstonesCount());
            // printf("  Using %ld to plug %ld value (homeslot: %ld): %ld\n", candidate_slot, tombstone_slot, candidate_home_slot, *(size_t *)candidate_ptr);
            #if 0
            if(find_key(common, policy, *(size_t *)tombstone_ptr, (*hasher)(set, tombstone_ptr))==false) {
               // printf("%ld lost!\n", *(size_t *)tombstone_ptr);
              assert(find_key(common, policy, *(size_t *)tombstone_ptr, (*hasher)(set, tombstone_ptr))==true);
            }
            #endif
            break;
          }
        }
        // Mark as empty.
        if (!candidate_found) {
          // printf("Finally cleared a tombstone!!\n");
          // checkAllReachable(common, policy);
          SetCtrl(common, (tombstone_slot & capacity), ctrl_t::kEmpty, slot_size);
          // checkAllReachable(common, policy);
        }       
      }
      slot++;
      slot = slot & capacity;
  }
  // printf("outerProbeLength: %ld innerProbeLength: %ld\n", outerProbeLength, innerProbeLength);
  // checkAllReachable(common, policy);
}

void DropDeletesWithoutResizeByPushingTombstones(
  CommonFields& common,
  const PolicyFunctions& policy, 
  size_t start_offset) {

  const size_t capacity = common.capacity();
  ctrl_t* ctrl = common.control();
  // Find the first slot after seq that has an empty.
  size_t range_start = start_offset;
  size_t range_end = start_offset;
  probe_seq<Group::kWidth> seq(start_offset, capacity);
  while (true) {
    GroupEmptyOrDeleted g{ctrl + seq.offset()};
    auto mask = g.MaskEmpty();
    if (mask) {
      range_start = seq.offset() + mask.LowestBitSet(); // First empty slot in group.
      break;
    }
    seq.next();
    assert(seq.index() != start_offset && "full table!");
  }
  start_offset = range_start;

  while (true) {
    // Find the next empty group.
    seq.next();
    while (true) {
      GroupEmptyOrDeleted g{ctrl + seq.offset()};
      auto mask = g.MaskEmpty();
      if (mask) {
        range_end = seq.offset() + mask.LowestBitSet(); // First empty slot in group.
        break;
      }
      seq.next();
      assert(seq.index() != start_offset && "full table!");
    }
    // printf("Clearing by pushing %ld %ld\n", range_start, range_end);
    ClearTombstonesInRange(common, policy, range_start, range_end);
    range_start = range_end;
    // We've completed the full scan.
    if (range_start == start_offset) break;
  }
  ResetGrowthLeft(common);
}

void DropDeletesWithoutResizeByRehashingRange(
  CommonFields& common,
  const PolicyFunctions& policy, 
  size_t start_offset,
  void *tmp_space) {

  size_t scan_length = 0;
  const size_t capacity = common.capacity();
  ctrl_t* ctrl = common.control();
  // Find the first slot after seq that has an empty.
  size_t range_start = start_offset;
  size_t range_end = start_offset;
  probe_seq<Group::kWidth> seq(start_offset, capacity);
  // Find an empty.
  while (true) {
    GroupEmptyOrDeleted g{ctrl + seq.offset()};
    auto mask = g.MaskEmpty();
    if (mask) {
      range_start = seq.offset() + mask.LowestBitSet(); // First empty slot in group.
      break;
    }
    seq.next();
    assert(seq.index() != start_offset && "full table!");
  }
  start_offset = range_start;

  // Keep finding the next empty.
  while (scan_length < capacity) {
    // Find the next empty group.
    seq.next();
    while (true) {
      GroupEmptyOrDeleted g{ctrl + seq.offset()};
      auto mask = g.MaskEmpty();
      if (mask) {
        range_end = seq.offset() + mask.LowestBitSet(); // First empty slot in group.
        break;
      }
      seq.next();
      scan_length += Group::kWidth;
      assert(seq.index() != start_offset && "full table!");
    }
    if (range_start == range_end) {
      abort();
    }
    // printf("Clearing: [%ld %ld] tc: %ld size: %ld\n", range_start, range_end, common.TombstonesCount(), common.capacity());
    ClearTombstonesInRangeByRehashing(common, policy, range_start, range_end, tmp_space);
    range_start = range_end;
    // We've completed the full scan.
    if (range_start == start_offset) break;
  }
  // printf("item at 589: %ld\n", *(size_t *)(SlotAddress(common.slot_array(), 589, policy.slot_size)));
  ResetGrowthLeft(common);
}


// Should be called right after DropDeletesWithoutResize.
// Scans the entire hash table and places a tombstone (deleted marker) every tombstone_distance spot.
void RedistributeTombstones(CommonFields& common,
                              const PolicyFunctions& policy, int tombstone_distance, void* tmp_space) {
  void* set = &common;
  void* slot_array = common.slot_array();
  const size_t capacity = common.capacity();
  assert(IsValidCapacity(capacity));
  assert(!is_small(capacity));

  ctrl_t* ctrl = common.control();
  auto transfer = policy.transfer;
  auto hasher = policy.hash_slot;
  const size_t slot_size = policy.slot_size;
  void* slot_ptr = SlotAddress(slot_array, 0, slot_size);

  size_t next_primitive_tombstone = tombstone_distance;
  void* primitive_tombstone_ptr = SlotAddress(slot_array, next_primitive_tombstone, slot_size);

  for (size_t i = 0; i != capacity;
       ++i, slot_ptr = NextSlot(slot_ptr, slot_size)) {
        // There should only be empty slots after a resize.
        assert(!IsDeleted(ctrl[i])); 
        if (IsFull(ctrl[i])) continue;
        // We do not swap for an item ahead of current as that would mess up home slots.
        if (i < next_primitive_tombstone) continue;

        assert(!IsDeleted(ctrl[next_primitive_tombstone])); 
        
        // Next Primitive Tombstone was already an empty space, so we can use this empty slot
        // for later.
        if (IsEmpty(ctrl[next_primitive_tombstone])) {
          next_primitive_tombstone += tombstone_distance;
          primitive_tombstone_ptr = SlotAddress(slot_array, next_primitive_tombstone, slot_size);
          --i;
          slot_ptr = PrevSlot(slot_ptr, slot_size);
          continue;
        }

        // Swap item in next_pts here, mark next_pts as tombstone 
        // the target as a tombstone.
        const size_t hash = (*hasher)(set, primitive_tombstone_ptr);
        SetCtrl(common, i, H2(hash), slot_size);
        // Swap i and new_i elements.
        (*transfer)(set, tmp_space, primitive_tombstone_ptr);
        (*transfer)(set, primitive_tombstone_ptr, slot_ptr);
        (*transfer)(set, slot_ptr, tmp_space);
        SetCtrl(common, next_primitive_tombstone, ctrl_t::kDeleted, slot_size);

        next_primitive_tombstone += tombstone_distance;
        primitive_tombstone_ptr = SlotAddress(slot_array, next_primitive_tombstone, slot_size);
  }
}

void DropDeletesWithoutResize(CommonFields& common,
                              const PolicyFunctions& policy, void* tmp_space) {
  void* set = &common;
  void* slot_array = common.slot_array();
  const size_t capacity = common.capacity();
  assert(IsValidCapacity(capacity));
  assert(!is_small(capacity));
  // Algorithm:
  // - mark all DELETED slots as EMPTY
  // - mark all FULL slots as DELETED
  // - for each slot marked as DELETED
  //     hash = Hash(element)
  //     target = find_first_non_full(hash)
  //     if target is in the same group
  //       mark slot as FULL
  //     else if target is EMPTY
  //       transfer element to target
  //       mark slot as EMPTY
  //       mark target as FULL
  //     else if target is DELETED
  //       swap current element with target element
  //       mark target as FULL
  //       repeat procedure for current slot with moved from element (target)
  ctrl_t* ctrl = common.control();
  ConvertDeletedToEmptyAndFullToDeleted(ctrl, capacity);
  auto hasher = policy.hash_slot;
  auto transfer = policy.transfer;
  const size_t slot_size = policy.slot_size;

  size_t total_probe_length = 0;
  void* slot_ptr = SlotAddress(slot_array, 0, slot_size);
  for (size_t i = 0; i != capacity;
       ++i, slot_ptr = NextSlot(slot_ptr, slot_size)) {
    assert(slot_ptr == SlotAddress(slot_array, i, slot_size));
    if (!IsDeleted(ctrl[i])) continue;
    const size_t hash = (*hasher)(set, slot_ptr);
    const FindInfo target = find_first_non_full(common, hash);
    const size_t new_i = target.offset;
    total_probe_length += target.probe_length;

    // Verify if the old and new i fall within the same group wrt the hash.
    // If they do, we don't need to move the object as it falls already in the
    // best probe we can.
    const size_t probe_offset = probe(common, hash).offset();
    const auto probe_index = [probe_offset, capacity](size_t pos) {
      return ((pos - probe_offset) & capacity) / Group::kWidth;
    };

    // Element doesn't move.
    if (ABSL_PREDICT_TRUE(probe_index(new_i) == probe_index(i))) {
      SetCtrl(common, i, H2(hash), slot_size);
      continue;
    }

    void* new_slot_ptr = SlotAddress(slot_array, new_i, slot_size);
    if (IsEmpty(ctrl[new_i])) {
      // Transfer element to the empty spot.
      // SetCtrl poisons/unpoisons the slots so we have to call it at the
      // right time.
      SetCtrl(common, new_i, H2(hash), slot_size);
      (*transfer)(set, new_slot_ptr, slot_ptr);
      SetCtrl(common, i, ctrl_t::kEmpty, slot_size);
    } else {
      assert(IsDeleted(ctrl[new_i]));
      SetCtrl(common, new_i, H2(hash), slot_size);
      // Until we are done rehashing, DELETED marks previously FULL slots.

      // Swap i and new_i elements.
      (*transfer)(set, tmp_space, new_slot_ptr);
      (*transfer)(set, new_slot_ptr, slot_ptr);
      (*transfer)(set, slot_ptr, tmp_space);

      // repeat the processing of the ith slot
      --i;
      slot_ptr = PrevSlot(slot_ptr, slot_size);
    }
  }
  ResetGrowthLeft(common);
  common.infoz().RecordRehash(total_probe_length);
}

static bool WasNeverFull(CommonFields& c, size_t index) {
  if (is_single_group(c.capacity())) {
    return true;
  }
  const size_t index_before = (index - Group::kWidth) & c.capacity();
  const auto empty_after = Group(c.control() + index).MaskEmpty();
  const auto empty_before = Group(c.control() + index_before).MaskEmpty();

  // We count how many consecutive non empties we have to the right and to the
  // left of `it`. If the sum is >= kWidth then there is at least one probe
  // window that might have seen a full group.
  return empty_before && empty_after &&
         static_cast<size_t>(empty_after.TrailingZeros()) +
                 empty_before.LeadingZeros() <
             Group::kWidth;
}

void EraseMetaOnly(CommonFields& c, size_t index, size_t slot_size) {
  assert(IsFull(c.control()[index]) && "erasing a dangling iterator");
  c.decrement_size();
  c.infoz().RecordErase();

  if (WasNeverFull(c, index)) {
    SetCtrl(c, index, ctrl_t::kEmpty, slot_size);
    c.set_growth_left(c.growth_left() + 1);
    return;
  }

  SetCtrl(c, index, ctrl_t::kDeleted, slot_size);
}

void ClearBackingArray(CommonFields& c, const PolicyFunctions& policy,
                       bool reuse) {
  c.set_size(0);
  if (reuse) {
    ResetCtrl(c, policy.slot_size);
    ResetGrowthLeft(c);
    c.infoz().RecordStorageChanged(0, c.capacity());
  } else {
    // We need to record infoz before calling dealloc, which will unregister
    // infoz.
    c.infoz().RecordClearedReservation();
    c.infoz().RecordStorageChanged(0, 0);
    (*policy.dealloc)(c, policy);
    c.set_control(EmptyGroup());
    c.set_generation_ptr(EmptyGeneration());
    c.set_slots(nullptr);
    c.set_capacity(0);
  }
}

void HashSetResizeHelper::GrowIntoSingleGroupShuffleControlBytes(
    ctrl_t* new_ctrl, size_t new_capacity) const {
  assert(is_single_group(new_capacity));
  constexpr size_t kHalfWidth = Group::kWidth / 2;
  assert(old_capacity_ < kHalfWidth);

  const size_t half_old_capacity = old_capacity_ / 2;

  // NOTE: operations are done with compile time known size = kHalfWidth.
  // Compiler optimizes that into single ASM operation.

  // Copy second half of bytes to the beginning.
  // We potentially copy more bytes in order to have compile time known size.
  // Mirrored bytes from the old_ctrl_ will also be copied.
  // In case of old_capacity_ == 3, we will copy 1st element twice.
  // Examples:
  // old_ctrl = 0S0EEEEEEE...
  // new_ctrl = S0EEEEEEEE...
  //
  // old_ctrl = 01S01EEEEE...
  // new_ctrl = 1S01EEEEEE...
  //
  // old_ctrl = 0123456S0123456EE...
  // new_ctrl = 456S0123?????????...
  std::memcpy(new_ctrl, old_ctrl_ + half_old_capacity + 1, kHalfWidth);
  // Clean up copied kSentinel from old_ctrl.
  new_ctrl[half_old_capacity] = ctrl_t::kEmpty;

  // Clean up damaged or uninitialized bytes.

  // Clean bytes after the intended size of the copy.
  // Example:
  // new_ctrl = 1E01EEEEEEE????
  // *new_ctrl= 1E0EEEEEEEE????
  // position      /
  std::memset(new_ctrl + old_capacity_ + 1, static_cast<int8_t>(ctrl_t::kEmpty),
              kHalfWidth);
  // Clean non-mirrored bytes that are not initialized.
  // For small old_capacity that may be inside of mirrored bytes zone.
  // Examples:
  // new_ctrl = 1E0EEEEEEEE??????????....
  // *new_ctrl= 1E0EEEEEEEEEEEEE?????....
  // position           /
  //
  // new_ctrl = 456E0123???????????...
  // *new_ctrl= 456E0123EEEEEEEE???...
  // position           /
  std::memset(new_ctrl + kHalfWidth, static_cast<int8_t>(ctrl_t::kEmpty),
              kHalfWidth);
  // Clean last mirrored bytes that are not initialized
  // and will not be overwritten by mirroring.
  // Examples:
  // new_ctrl = 1E0EEEEEEEEEEEEE????????
  // *new_ctrl= 1E0EEEEEEEEEEEEEEEEEEEEE
  // position           S       /
  //
  // new_ctrl = 456E0123EEEEEEEE???????????????
  // *new_ctrl= 456E0123EEEEEEEE???????EEEEEEEE
  // position                  S       /
  std::memset(new_ctrl + new_capacity + kHalfWidth,
              static_cast<int8_t>(ctrl_t::kEmpty), kHalfWidth);

  // Create mirrored bytes. old_capacity_ < kHalfWidth
  // Example:
  // new_ctrl = 456E0123EEEEEEEE???????EEEEEEEE
  // *new_ctrl= 456E0123EEEEEEEE456E0123EEEEEEE
  // position                  S/
  ctrl_t g[kHalfWidth];
  std::memcpy(g, new_ctrl, kHalfWidth);
  std::memcpy(new_ctrl + new_capacity + 1, g, kHalfWidth);

  // Finally set sentinel to its place.
  new_ctrl[new_capacity] = ctrl_t::kSentinel;
}

void HashSetResizeHelper::GrowIntoSingleGroupShuffleTransferableSlots(
    void* old_slots, void* new_slots, size_t slot_size) const {
  assert(old_capacity_ > 0);
  const size_t half_old_capacity = old_capacity_ / 2;

  SanitizerUnpoisonMemoryRegion(old_slots, slot_size * old_capacity_);
  std::memcpy(new_slots,
              SlotAddress(old_slots, half_old_capacity + 1, slot_size),
              slot_size * half_old_capacity);
  std::memcpy(SlotAddress(new_slots, half_old_capacity + 1, slot_size),
              old_slots, slot_size * (half_old_capacity + 1));
}

void HashSetResizeHelper::GrowSizeIntoSingleGroupTransferable(
    CommonFields& c, void* old_slots, size_t slot_size) {
  assert(old_capacity_ < Group::kWidth / 2);
  assert(is_single_group(c.capacity()));
  assert(IsGrowingIntoSingleGroupApplicable(old_capacity_, c.capacity()));

  GrowIntoSingleGroupShuffleControlBytes(c.control(), c.capacity());
  GrowIntoSingleGroupShuffleTransferableSlots(old_slots, c.slot_array(),
                                              slot_size);

  // We poison since GrowIntoSingleGroupShuffleTransferableSlots
  // may leave empty slots unpoisoned.
  PoisonSingleGroupEmptySlots(c, slot_size);
}

}  // namespace container_internal
ABSL_NAMESPACE_END
}  // namespace absl
