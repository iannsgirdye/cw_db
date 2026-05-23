#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "core/value.h"
#include "storage/pager.h"
#include "storage/value_less.h"

namespace cw_db {

class DiskBspIndex {
public:
    class const_iterator {
    public:
        const_iterator() = default;
        bool operator==(const const_iterator& o) const {
            return at_end_ == o.at_end_ && (at_end_ || (page_ == o.page_ && slot_ == o.slot_));
        }
        bool operator!=(const const_iterator& o) const { return !(*this == o); }

        const Value&   key()   const { return key_; }
        std::uint64_t  value() const { return rid_; }

        const_iterator& operator++();

    private:
        friend class DiskBspIndex;
        const DiskBspIndex* owner_ = nullptr;
        std::uint32_t       page_  = 0;
        std::uint16_t       slot_  = 0;
        Value               key_{};
        std::uint64_t       rid_ = 0;
        bool                at_end_ = true;

        const_iterator(const DiskBspIndex* owner, std::uint32_t page, std::uint16_t slot, bool end);
    };

    DiskBspIndex() = default;

    void create_empty(const std::filesystem::path& path, const ValueLess& less);
    void open(const std::filesystem::path& path, const ValueLess& less);
    void close();
    void flush();

    bool is_open() const noexcept { return pager_.is_open(); }
    std::size_t size() const noexcept { return size_; }

    void insert(const Value& key, std::uint64_t rid);
    bool erase(const Value& key);

    void bulk_build(const std::filesystem::path& path, const ValueLess& less,
                    std::vector<std::pair<Value, std::uint64_t>> entries);

    const_iterator find(const Value& key) const;
    const_iterator lower_bound(const Value& key) const;
    const_iterator upper_bound(const Value& key) const;
    const_iterator begin() const;
    const_iterator end() const { return const_iterator(this, 0, 0, true); }

private:
    static constexpr std::uint32_t kIdxMagic   = 0x49445842;  // "IDXB"
    static constexpr std::uint32_t kIdxVersion = 1;

    enum class NodeType : std::uint8_t { Leaf = 1, Inner = 2 };

    struct LeafEntry {
        std::vector<std::uint8_t> key;
        std::uint16_t               key_len = 0;
        std::uint64_t               rid = 0;
    };

    friend std::size_t leaf_payload_bytes(const std::vector<LeafEntry>& entries);
    friend std::size_t inner_payload_bytes(const std::vector<LeafEntry>& keys, std::size_t child_count);
    friend bool leaf_overflow(const std::vector<LeafEntry>& entries);
    friend bool leaf_underflow(const std::vector<LeafEntry>& entries, bool is_root);
    friend bool inner_overflow(const std::vector<LeafEntry>& keys, const std::vector<std::uint32_t>& children);
    friend bool inner_underflow(const std::vector<LeafEntry>& keys, const std::vector<std::uint32_t>& children,
                                bool is_root);

    struct ParsedPage {
        std::uint32_t               page_id = 0;
        NodeType                    type = NodeType::Leaf;
        std::uint8_t                key_count = 0;
        std::uint32_t               next_leaf = 0;
        std::vector<LeafEntry>      leaf_entries;
        std::vector<LeafEntry>      inner_keys;
        std::vector<std::uint32_t>  children;
    };

    struct LeafSearchResult {
        ParsedPage              leaf;
        std::vector<std::uint32_t> path;
    };

    Pager           pager_;
    ValueLess       less_{};
    std::uint32_t   root_page_ = 0;
    std::uint32_t   first_leaf_page_ = 0;
    std::size_t     size_ = 0;

    ParsedPage read_page(std::uint32_t page_id) const;
    void write_page(const ParsedPage& page);

    std::uint16_t lower_bound_slot(const ParsedPage& p, const Value& key) const;
    std::uint16_t upper_bound_slot(const ParsedPage& p, const Value& key) const;

    const_iterator leaf_find(std::uint32_t page, const Value& key, bool exact) const;
    const_iterator leaf_lower(std::uint32_t page, const Value& key) const;

    LeafSearchResult find_leaf(const Value& key);
    LeafEntry make_entry(const Value& key, std::uint64_t rid) const;

    std::uint32_t allocate_node_page();
    void write_superblock();
    void bulk_build_from_flat(std::vector<LeafEntry> flat);

    static LeafEntry subtree_min_entry(const DiskBspIndex& idx, std::uint32_t page_id);
    static LeafEntry separator_key_for_page(const DiskBspIndex& idx, std::uint32_t page_id);
    std::uint32_t leftmost_leaf(std::uint32_t page_id) const;

    void rebuild_internal_keys(ParsedPage& node);
    void propagate_subtree_min_change(std::uint32_t page_id, const std::vector<std::uint32_t>& path);
    void promote_single_child_root(ParsedPage& root);

    void insert_into_parent(ParsedPage& left, const LeafEntry& separator, ParsedPage& right,
                            const std::vector<std::uint32_t>& left_path);
    void split_leaf(ParsedPage& leaf, const std::vector<std::uint32_t>& path);
    bool try_give_to_right_leaf(ParsedPage& leaf, ParsedPage& parent, std::size_t child_index);
    bool try_give_to_left_leaf(ParsedPage& leaf, ParsedPage& parent, std::size_t child_index);
    void split_leaf_pair(ParsedPage& left, ParsedPage& right, ParsedPage& parent,
                         std::size_t parent_index, const std::vector<std::uint32_t>& parent_path);
    void redistribute_leaf_pair(ParsedPage& left, ParsedPage& right, ParsedPage& parent,
                                const std::vector<std::uint32_t>& parent_path);

    void split_internal(ParsedPage& node, const std::vector<std::uint32_t>& path);
    void split_internal_pair(ParsedPage& left, ParsedPage& right, ParsedPage& parent,
                             std::size_t parent_index, const std::vector<std::uint32_t>& parent_path);
    bool try_redistribute_internal_with_right(ParsedPage& node, ParsedPage& parent,
                                              std::size_t child_index,
                                              const std::vector<std::uint32_t>& parent_path);
    bool try_redistribute_internal_with_left(ParsedPage& node, ParsedPage& parent,
                                             std::size_t child_index,
                                             const std::vector<std::uint32_t>& parent_path);
    void redistribute_internal_pair(ParsedPage& left, ParsedPage& right, ParsedPage& parent,
                                    const std::vector<std::uint32_t>& parent_path);

    void fix_leaf_underflow(ParsedPage& leaf, const std::vector<std::uint32_t>& path);
    void fix_internal_underflow(ParsedPage& node, const std::vector<std::uint32_t>& path);
    void handle_internal_after_child_removed(ParsedPage& node, const std::vector<std::uint32_t>& path);
};

}  // namespace cw_db
