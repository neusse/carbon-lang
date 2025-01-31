// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CARBON_TOOLCHAIN_SEM_IR_BLOCK_VALUE_STORE_H_
#define CARBON_TOOLCHAIN_SEM_IR_BLOCK_VALUE_STORE_H_

#include <type_traits>

#include "common/hashing.h"
#include "llvm/ADT/DenseMap.h"
#include "toolchain/base/value_store.h"
#include "toolchain/base/yaml.h"

namespace Carbon::SemIR {

// Provides a block-based ValueStore, which uses slab allocation of added
// blocks. This allows references to values to outlast vector resizes that might
// otherwise invalidate references.
//
// BlockValueStore is used as-is, but there are also children that expose the
// protected members for type-specific functionality.
//
// On IdT, this requires:
//   - IdT::ElementType to represent the underlying type in the block.
//   - IdT::ValueType to be llvm::MutableArrayRef<IdT::ElementType> for
//     compatibility with ValueStore.
template <typename IdT>
class BlockValueStore : public Yaml::Printable<BlockValueStore<IdT>> {
 public:
  using ElementType = IdT::ElementType;

  explicit BlockValueStore(llvm::BumpPtrAllocator& allocator)
      : allocator_(&allocator) {}

  // Adds a block with the given content, returning an ID to reference it.
  auto Add(llvm::ArrayRef<ElementType> content) -> IdT {
    return values_.Add(AllocateCopy(content));
  }

  // Returns the requested block.
  auto Get(IdT id) const -> llvm::ArrayRef<ElementType> {
    return values_.Get(id);
  }

  // Returns the requested block.
  auto Get(IdT id) -> llvm::MutableArrayRef<ElementType> {
    return values_.Get(id);
  }

  // Adds a block or finds an existing canonical block with the given content,
  // and returns an ID to reference it.
  auto AddCanonical(llvm::ArrayRef<ElementType> content) -> IdT {
    auto [it, added] = canonical_blocks_.insert({{content}, IdT::Invalid});
    if (added) {
      auto id = Add(content);
      it->first.data = Get(id);
      it->second = id;
    }
    return it->second;
  }

  // Promotes an existing block ID to a canonical block ID, or returns an
  // existing canonical block ID if the block was already added. The specified
  // block must not be modified after this point.
  auto MakeCanonical(IdT id) -> IdT {
    return canonical_blocks_.insert({{Get(id)}, id}).first->second;
  }

  auto OutputYaml() const -> Yaml::OutputMapping {
    return Yaml::OutputMapping([&](Yaml::OutputMapping::Map map) {
      for (auto block_index : llvm::seq(values_.size())) {
        auto block_id = IdT(block_index);
        map.Add(PrintToString(block_id),
                Yaml::OutputMapping([&](Yaml::OutputMapping::Map map) {
                  auto block = Get(block_id);
                  for (auto i : llvm::seq(block.size())) {
                    map.Add(llvm::itostr(i), Yaml::OutputScalar(block[i]));
                  }
                }));
      }
    });
  }

  auto size() const -> int { return values_.size(); }

 protected:
  // Reserves and returns a block ID. The contents of the block
  // should be specified by calling Set, or similar.
  auto AddDefaultValue() -> IdT { return values_.AddDefaultValue(); }

  // Adds an uninitialized block of the given size.
  auto AddUninitialized(size_t size) -> IdT {
    return values_.Add(AllocateUninitialized(size));
  }

  // Sets the contents of an empty block to the given content.
  auto Set(IdT block_id, llvm::ArrayRef<ElementType> content) -> void {
    CARBON_CHECK(Get(block_id).empty())
        << "inst block content set more than once";
    values_.Get(block_id) = AllocateCopy(content);
  }

 private:
  // A canonical block, for which we allocate a deduplicated ID.
  struct CanonicalBlock {
    // This is mutable so we can repoint it at the allocated data if insertion
    // succeeds.
    mutable llvm::ArrayRef<ElementType> data;

    // See common/hashing.h.
    friend auto CarbonHashValue(CanonicalBlock block, uint64_t seed)
        -> HashCode {
      Hasher hasher(seed);
      hasher.HashSizedBytes(block.data);
      return static_cast<HashCode>(hasher);
    }
  };

  struct CanonicalBlockDenseMapInfo {
    // Blocks whose data() points to the start of `SpecialData` are used to
    // represent the special "empty" and "tombstone" states.
    static constexpr ElementType SpecialData[1] = {ElementType::Invalid};
    static auto getEmptyKey() -> CanonicalBlock {
      return CanonicalBlock{
          llvm::ArrayRef(SpecialData, static_cast<size_t>(0))};
    }
    static auto getTombstoneKey() -> CanonicalBlock {
      return CanonicalBlock{llvm::ArrayRef(SpecialData, 1)};
    }
    static auto getHashValue(CanonicalBlock val) -> unsigned {
      return static_cast<uint64_t>(HashValue(val));
    }
    static auto isEqual(CanonicalBlock lhs, CanonicalBlock rhs) -> bool {
      return lhs.data == rhs.data && (lhs.data.data() == SpecialData) ==
                                         (rhs.data.data() == SpecialData);
    }
  };

  // Allocates an uninitialized array using our slab allocator.
  auto AllocateUninitialized(std::size_t size)
      -> llvm::MutableArrayRef<ElementType> {
    // We're not going to run a destructor, so ensure that's OK.
    static_assert(std::is_trivially_destructible_v<ElementType>);

    auto storage = static_cast<ElementType*>(
        allocator_->Allocate(size * sizeof(ElementType), alignof(ElementType)));
    return llvm::MutableArrayRef<ElementType>(storage, size);
  }

  // Allocates a copy of the given data using our slab allocator.
  auto AllocateCopy(llvm::ArrayRef<ElementType> data)
      -> llvm::MutableArrayRef<ElementType> {
    auto result = AllocateUninitialized(data.size());
    std::uninitialized_copy(data.begin(), data.end(), result.begin());
    return result;
  }

  llvm::BumpPtrAllocator* allocator_;
  ValueStore<IdT> values_;
  llvm::DenseMap<CanonicalBlock, IdT, CanonicalBlockDenseMapInfo>
      canonical_blocks_;
};

}  // namespace Carbon::SemIR

#endif  // CARBON_TOOLCHAIN_SEM_IR_BLOCK_VALUE_STORE_H_
