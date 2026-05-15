#include "storage/disk_bsp_index.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#include "storage/bulk_build_guard.h"
#include "storage/value_io.h"

namespace cw_db {

namespace {

constexpr std::size_t kLeafHeader  = 12;
constexpr std::size_t kInnerHeader = 12;

std::size_t leaf_entry_bytes(std::uint16_t key_len) {
    return 2 + key_len + sizeof(std::uint64_t);
}

int cmp_key_bytes(const ValueLess& less, const std::uint8_t* a, std::size_t alen, const Value& b) {
    std::size_t consumed = 0;
    Value va = decode_value(a, alen, consumed);
    bool valid = true;
    return va.compare(b, valid);
}

bool keys_equal_bytes(const std::uint8_t* a, std::size_t alen, const std::uint8_t* b, std::size_t blen) {
    return compare_encoded_keys(a, alen, b, blen) == 0;
}

std::array<std::size_t, 3> three_way_counts(std::size_t total) {
    const auto first  = (total + 2) / 3;
    const auto second = (total - first + 1) / 2;
    const auto third  = total - first - second;
    return {first, second, third};
}

}  // namespace

constexpr std::size_t kMinLeafFillBytes = 12 + (kPageSize - 12) * 2 / 3;
constexpr std::size_t kMinInnerFillBytes = 12 + (kPageSize - 12) * 2 / 3;

std::size_t leaf_payload_bytes(const std::vector<DiskBspIndex::LeafEntry>& entries) {
    std::size_t off = 12;
    for (const auto& e : entries) {
        off += 2 + e.key_len + sizeof(std::uint64_t);
    }
    return off;
}

std::size_t inner_payload_bytes(const std::vector<DiskBspIndex::LeafEntry>& keys,
                                std::size_t child_count) {
    std::size_t off = 12 + child_count * sizeof(std::uint32_t);
    for (const auto& k : keys) {
        off += 2 + k.key_len;
    }
    return off;
}

bool leaf_overflow(const std::vector<DiskBspIndex::LeafEntry>& entries) {
    return leaf_payload_bytes(entries) > kPageSize;
}

bool leaf_underflow(const std::vector<DiskBspIndex::LeafEntry>& entries, bool is_root) {
    if (is_root) return false;
    if (entries.empty()) return true;
    return leaf_payload_bytes(entries) < kMinLeafFillBytes;
}

bool inner_overflow(const std::vector<DiskBspIndex::LeafEntry>& keys,
                    const std::vector<std::uint32_t>& children) {
    return inner_payload_bytes(keys, children.size()) > kPageSize;
}

bool inner_underflow(const std::vector<DiskBspIndex::LeafEntry>& keys,
                     const std::vector<std::uint32_t>& children, bool is_root) {
    if (is_root) return false;
    if (children.size() <= 1) return true;
    return inner_payload_bytes(keys, children.size()) < kMinInnerFillBytes;
}

DiskBspIndex::const_iterator::const_iterator(const DiskBspIndex* owner, std::uint32_t page,
                                             std::uint16_t slot, bool end)
    : owner_(owner), page_(page), slot_(slot), at_end_(end) {
    if (!at_end_ && owner_) {
        auto p = owner_->read_page(page_);
        if (slot_ < p.leaf_entries.size()) {
            std::size_t c = 0;
            key_ = decode_value(p.leaf_entries[slot_].key.data(), p.leaf_entries[slot_].key_len, c);
            rid_ = p.leaf_entries[slot_].rid;
        } else {
            at_end_ = true;
        }
    }
}

DiskBspIndex::const_iterator& DiskBspIndex::const_iterator::operator++() {
    if (at_end_ || !owner_) return *this;
    auto p = owner_->read_page(page_);
    ++slot_;
    if (slot_ < p.leaf_entries.size()) {
        std::size_t c = 0;
        key_ = decode_value(p.leaf_entries[slot_].key.data(), p.leaf_entries[slot_].key_len, c);
        rid_ = p.leaf_entries[slot_].rid;
        return *this;
    }
    if (p.next_leaf == 0) {
        at_end_ = true;
        return *this;
    }
    page_ = p.next_leaf;
    slot_ = 0;
    auto np = owner_->read_page(page_);
    if (slot_ < np.leaf_entries.size()) {
        std::size_t c = 0;
        key_ = decode_value(np.leaf_entries[slot_].key.data(), np.leaf_entries[slot_].key_len, c);
        rid_ = np.leaf_entries[slot_].rid;
    } else {
        at_end_ = true;
    }
    return *this;
}

void DiskBspIndex::create_empty(const std::filesystem::path& path, const ValueLess& less) {
    less_ = less;
    pager_.open(path, true);
    {
        std::vector<std::uint8_t> reserved(kPageSize, 0);
        pager_.write_page(0, reserved);
    }
    root_page_ = allocate_node_page();
    first_leaf_page_ = root_page_;
    size_ = 0;
    ParsedPage leaf;
    leaf.page_id = root_page_;
    leaf.type = NodeType::Leaf;
    leaf.next_leaf = 0;
    write_page(leaf);
    write_superblock();
}

void DiskBspIndex::open(const std::filesystem::path& path, const ValueLess& less) {
    less_ = less;
    pager_.open(path, false);
    const auto& page = pager_.read_page(0);
    std::uint32_t magic = 0, ver = 0;
    std::memcpy(&magic, page.data(), 4);
    std::memcpy(&ver, page.data() + 4, 4);
    if (magic != kIdxMagic || ver != kIdxVersion) {
        throw std::runtime_error("Bad index file: " + path.string());
    }
    std::memcpy(&root_page_, page.data() + 8, 4);
    std::memcpy(&first_leaf_page_, page.data() + 12, 4);
    std::uint64_t sz = 0;
    std::memcpy(&sz, page.data() + 16, 8);
    size_ = static_cast<std::size_t>(sz);
}

void DiskBspIndex::close() {
    if (pager_.is_open()) {
        write_superblock();
        pager_.close();
    }
    size_ = 0;
}

void DiskBspIndex::flush() {
    write_superblock();
    pager_.flush();
}

void DiskBspIndex::write_superblock() {
    if (!pager_.is_open()) return;
    std::vector<std::uint8_t> page(kPageSize, 0);
    std::memcpy(page.data(), &kIdxMagic, 4);
    std::uint32_t ver = kIdxVersion;
    std::memcpy(page.data() + 4, &ver, 4);
    std::memcpy(page.data() + 8, &root_page_, 4);
    std::memcpy(page.data() + 12, &first_leaf_page_, 4);
    std::uint64_t sz = static_cast<std::uint64_t>(size_);
    std::memcpy(page.data() + 16, &sz, 8);
    pager_.write_page(0, page);
}

std::uint32_t DiskBspIndex::allocate_node_page() {
    if (pager_.page_count() == 0) {
        std::vector<std::uint8_t> reserved(kPageSize, 0);
        pager_.write_page(0, reserved);
    }
    return pager_.allocate_page();
}

DiskBspIndex::ParsedPage DiskBspIndex::read_page(std::uint32_t page_id) const {
    const auto& raw = pager_.read_page(page_id);
    ParsedPage p;
    p.page_id = page_id;
    std::uint32_t magic = 0;
    std::memcpy(&magic, raw.data(), 4);
    if (magic != kPageMagic) throw std::runtime_error("Corrupt index page");
    p.type = static_cast<NodeType>(raw[4]);
    p.key_count = raw[5];
    std::memcpy(&p.next_leaf, raw.data() + 8, 4);
    std::size_t off = kLeafHeader;
    if (p.type == NodeType::Leaf) {
        for (std::uint8_t i = 0; i < p.key_count; ++i) {
            if (off + 2 + sizeof(std::uint64_t) > kPageSize) break;
            std::uint16_t klen = 0;
            std::memcpy(&klen, raw.data() + off, 2);
            off += 2;
            LeafEntry e;
            e.key_len = klen;
            e.key.assign(raw.data() + off, raw.data() + off + klen);
            off += klen;
            std::memcpy(&e.rid, raw.data() + off, sizeof(std::uint64_t));
            off += sizeof(std::uint64_t);
            p.leaf_entries.push_back(std::move(e));
        }
    } else {
        const std::uint8_t n = p.key_count;
        const std::uint32_t child_count = static_cast<std::uint32_t>(n) + 1;
        if (off + child_count * 4 > kPageSize) throw std::runtime_error("Corrupt inner index page");
        p.children.resize(child_count);
        for (std::uint32_t i = 0; i < child_count; ++i) {
            std::memcpy(&p.children[i], raw.data() + off, 4);
            off += 4;
        }
        for (std::uint8_t i = 0; i < n; ++i) {
            std::uint16_t klen = 0;
            std::memcpy(&klen, raw.data() + off, 2);
            off += 2;
            LeafEntry e;
            e.key_len = klen;
            e.key.assign(raw.data() + off, raw.data() + off + klen);
            off += klen;
            p.inner_keys.push_back(std::move(e));
        }
    }
    return p;
}

void DiskBspIndex::write_page(const ParsedPage& page) {
    if (page.type == NodeType::Leaf) {
        if (leaf_overflow(page.leaf_entries)) {
            throw std::runtime_error("Leaf index page overflow");
        }
        std::vector<std::uint8_t> raw(kPageSize, 0);
        std::memcpy(raw.data(), &kPageMagic, 4);
        raw[4] = static_cast<std::uint8_t>(NodeType::Leaf);
        raw[5] = static_cast<std::uint8_t>(page.leaf_entries.size());
        std::uint32_t next = page.next_leaf;
        std::memcpy(raw.data() + 8, &next, 4);
        std::size_t off = kLeafHeader;
        for (const auto& e : page.leaf_entries) {
            std::uint16_t klen = e.key_len;
            std::memcpy(raw.data() + off, &klen, 2);
            off += 2;
            std::memcpy(raw.data() + off, e.key.data(), klen);
            off += klen;
            std::memcpy(raw.data() + off, &e.rid, sizeof(std::uint64_t));
            off += sizeof(std::uint64_t);
        }
        pager_.write_page(page.page_id, raw);
        return;
    }

    if (inner_overflow(page.inner_keys, page.children)) {
        throw std::runtime_error("Inner index page overflow");
    }
    std::vector<std::uint8_t> raw(kPageSize, 0);
    std::memcpy(raw.data(), &kPageMagic, 4);
    raw[4] = static_cast<std::uint8_t>(NodeType::Inner);
    raw[5] = static_cast<std::uint8_t>(page.inner_keys.size());
    std::size_t off = kInnerHeader;
    for (std::uint32_t c : page.children) {
        std::memcpy(raw.data() + off, &c, 4);
        off += 4;
    }
    for (const auto& k : page.inner_keys) {
        std::uint16_t klen = k.key_len;
        std::memcpy(raw.data() + off, &klen, 2);
        off += 2;
        std::memcpy(raw.data() + off, k.key.data(), klen);
        off += klen;
    }
    pager_.write_page(page.page_id, raw);
}

DiskBspIndex::LeafEntry DiskBspIndex::make_entry(const Value& key, std::uint64_t rid) const {
    LeafEntry e;
    encode_value(e.key, key);
    e.key_len = static_cast<std::uint16_t>(e.key.size());
    e.rid = rid;
    return e;
}

DiskBspIndex::LeafEntry DiskBspIndex::separator_key_for_page(const DiskBspIndex& idx,
                                                              std::uint32_t page_id) {
    auto p = idx.read_page(page_id);
    if (p.type == NodeType::Leaf) {
        if (p.leaf_entries.empty()) {
            throw std::runtime_error("Cannot take separator from empty leaf");
        }
        return p.leaf_entries.front();
    }
    return subtree_min_entry(idx, page_id);
}

void DiskBspIndex::bulk_build(const std::filesystem::path& path, const ValueLess& less,
                              std::vector<std::pair<Value, std::uint64_t>> entries) {
    validate_bulk_build_input(entries);
    less_ = less;
    std::vector<LeafEntry> flat;
    flat.reserve(entries.size());
    for (auto& [key, rid] : entries) {
        flat.push_back(make_entry(key, rid));
    }
    for (std::size_t i = 1; i < flat.size(); ++i) {
        if (compare_encoded_keys(flat[i - 1].key.data(), flat[i - 1].key_len,
                                 flat[i].key.data(), flat[i].key_len) >= 0) {
            throw std::runtime_error(
                "bulk_build requires strictly increasing unique keys (sort input first)");
        }
    }
    pager_.open(path, true);
    bulk_build_from_flat(std::move(flat));
}

void DiskBspIndex::bulk_build_from_flat(std::vector<LeafEntry> flat) {
    {
        std::vector<std::uint8_t> reserved(kPageSize, 0);
        pager_.write_page(0, reserved);
    }

    size_ = flat.size();

    if (flat.empty()) {
        root_page_ = allocate_node_page();
        first_leaf_page_ = root_page_;
        ParsedPage leaf;
        leaf.page_id = root_page_;
        leaf.type = NodeType::Leaf;
        leaf.next_leaf = 0;
        write_page(leaf);
        write_superblock();
        pager_.flush();
        return;
    }

    std::vector<std::uint32_t> leaf_pages;
    std::vector<LeafEntry> chunk;
    std::size_t chunk_bytes = kLeafHeader;
    for (auto& entry : flat) {
        const std::size_t need = leaf_entry_bytes(entry.key_len);
        if (!chunk.empty() && chunk_bytes + need > kPageSize) {
            const std::uint32_t pid = allocate_node_page();
            ParsedPage leaf;
            leaf.page_id = pid;
            leaf.type = NodeType::Leaf;
            leaf.next_leaf = 0;
            leaf.leaf_entries = std::move(chunk);
            write_page(leaf);
            leaf_pages.push_back(pid);
            chunk.clear();
            chunk_bytes = kLeafHeader;
        }
        chunk_bytes += need;
        chunk.push_back(std::move(entry));
    }
    if (!chunk.empty()) {
        const std::uint32_t pid = allocate_node_page();
        ParsedPage leaf;
        leaf.page_id = pid;
        leaf.type = NodeType::Leaf;
        leaf.next_leaf = 0;
        leaf.leaf_entries = std::move(chunk);
        write_page(leaf);
        leaf_pages.push_back(pid);
    }

    for (std::size_t i = 0; i + 1 < leaf_pages.size(); ++i) {
        auto p = read_page(leaf_pages[i]);
        p.next_leaf = leaf_pages[i + 1];
        write_page(p);
    }
    first_leaf_page_ = leaf_pages.front();

    std::vector<std::uint32_t> level = leaf_pages;
    while (level.size() > 1) {
        std::vector<std::uint32_t> next_level;
        for (std::size_t i = 0; i < level.size(); ) {
            std::vector<std::uint32_t> children{level[i]};
            std::vector<LeafEntry> keys;
            std::size_t page_bytes = kInnerHeader + children.size() * sizeof(std::uint32_t);
            std::size_t j = i + 1;
            for (; j < level.size(); ++j) {
                const LeafEntry sep = separator_key_for_page(*this, level[j]);
                const std::size_t need = leaf_entry_bytes(sep.key_len) + sizeof(std::uint32_t);
                if (!keys.empty() && page_bytes + need > kPageSize) {
                    break;
                }
                keys.push_back(sep);
                children.push_back(level[j]);
                page_bytes = kInnerHeader + children.size() * sizeof(std::uint32_t);
                for (const auto& k : keys) {
                    page_bytes += 2 + k.key_len;
                }
            }
            if (j == i + 1 && i + 1 < level.size()) {
                throw std::runtime_error("Inner index page cannot fit a separator key during bulk build");
            }
            const std::uint32_t inner = allocate_node_page();
            ParsedPage inner_page;
            inner_page.page_id = inner;
            inner_page.type = NodeType::Inner;
            inner_page.children = std::move(children);
            inner_page.inner_keys = std::move(keys);
            inner_page.key_count = static_cast<std::uint8_t>(inner_page.inner_keys.size());
            write_page(inner_page);
            next_level.push_back(inner);
            i = j;
        }
        level = std::move(next_level);
    }
    root_page_ = level.front();
    write_superblock();
    pager_.flush();
}

std::uint16_t DiskBspIndex::lower_bound_slot(const ParsedPage& p, const Value& key) const {
    std::uint16_t lo = 0, hi = static_cast<std::uint16_t>(p.leaf_entries.size());
    while (lo < hi) {
        std::uint16_t mid = static_cast<std::uint16_t>((lo + hi) / 2);
        if (cmp_key_bytes(less_, p.leaf_entries[mid].key.data(), p.leaf_entries[mid].key_len, key) < 0) {
            lo = static_cast<std::uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return lo;
}

std::uint16_t DiskBspIndex::upper_bound_slot(const ParsedPage& p, const Value& key) const {
    std::uint16_t lo = 0, hi = static_cast<std::uint16_t>(p.leaf_entries.size());
    while (lo < hi) {
        std::uint16_t mid = static_cast<std::uint16_t>((lo + hi) / 2);
        int c = cmp_key_bytes(less_, p.leaf_entries[mid].key.data(), p.leaf_entries[mid].key_len, key);
        if (c <= 0) {
            lo = static_cast<std::uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return lo;
}

DiskBspIndex::const_iterator DiskBspIndex::leaf_find(std::uint32_t page, const Value& key,
                                                     bool exact) const {
    auto p = read_page(page);
    std::uint16_t slot = lower_bound_slot(p, key);
    if (slot < p.leaf_entries.size()) {
        int c = cmp_key_bytes(less_, p.leaf_entries[slot].key.data(), p.leaf_entries[slot].key_len, key);
        if (c == 0) return const_iterator(this, page, slot, false);
        if (!exact && c > 0) return const_iterator(this, page, slot, false);
    }
    if (exact) return end();
    if (slot < p.leaf_entries.size()) return const_iterator(this, page, slot, false);
    if (p.next_leaf) return const_iterator(this, p.next_leaf, 0, false);
    return end();
}

DiskBspIndex::const_iterator DiskBspIndex::leaf_lower(std::uint32_t page, const Value& key) const {
    auto p = read_page(page);
    std::uint16_t slot = lower_bound_slot(p, key);
    if (slot < p.leaf_entries.size()) return const_iterator(this, page, slot, false);
    if (p.next_leaf) return const_iterator(this, p.next_leaf, 0, false);
    return end();
}

DiskBspIndex::const_iterator DiskBspIndex::find(const Value& key) const {
    std::uint32_t page = root_page_;
    while (true) {
        auto p = read_page(page);
        if (p.type == NodeType::Leaf) return leaf_find(page, key, true);
        std::uint16_t child = 0;
        for (std::uint8_t i = 0; i < p.key_count; ++i) {
            if (cmp_key_bytes(less_, p.inner_keys[i].key.data(), p.inner_keys[i].key_len, key) <= 0) {
                child = static_cast<std::uint16_t>(i + 1);
            }
        }
        if (child >= p.children.size()) throw std::runtime_error("Corrupt inner index page");
        page = p.children[child];
    }
}

DiskBspIndex::const_iterator DiskBspIndex::lower_bound(const Value& key) const {
    std::uint32_t page = root_page_;
    while (true) {
        auto p = read_page(page);
        if (p.type == NodeType::Leaf) return leaf_lower(page, key);
        std::uint16_t child = 0;
        for (std::uint8_t i = 0; i < p.key_count; ++i) {
            if (cmp_key_bytes(less_, p.inner_keys[i].key.data(), p.inner_keys[i].key_len, key) <= 0) {
                child = static_cast<std::uint16_t>(i + 1);
            }
        }
        if (child >= p.children.size()) throw std::runtime_error("Corrupt inner index page");
        page = p.children[child];
    }
}

DiskBspIndex::const_iterator DiskBspIndex::upper_bound(const Value& key) const {
    std::uint32_t page = root_page_;
    while (true) {
        auto p = read_page(page);
        if (p.type == NodeType::Leaf) {
            std::uint16_t slot = upper_bound_slot(p, key);
            if (slot < p.leaf_entries.size()) return const_iterator(this, page, slot, false);
            if (p.next_leaf) return const_iterator(this, p.next_leaf, 0, false);
            return end();
        }
        std::uint16_t child = 0;
        for (std::uint8_t i = 0; i < p.key_count; ++i) {
            if (cmp_key_bytes(less_, p.inner_keys[i].key.data(), p.inner_keys[i].key_len, key) <= 0) {
                child = static_cast<std::uint16_t>(i + 1);
            }
        }
        if (child >= p.children.size()) throw std::runtime_error("Corrupt inner index page");
        page = p.children[child];
    }
}

DiskBspIndex::const_iterator DiskBspIndex::begin() const {
    if (size_ == 0) return end();
    return const_iterator(this, first_leaf_page_, 0, false);
}

DiskBspIndex::LeafSearchResult DiskBspIndex::find_leaf(const Value& key) {
    std::uint32_t page = root_page_;
    std::vector<std::uint32_t> path;
    while (true) {
        path.push_back(page);
        auto p = read_page(page);
        if (p.type == NodeType::Leaf) {
            return LeafSearchResult{std::move(p), std::move(path)};
        }
        std::uint16_t child = 0;
        for (std::uint8_t i = 0; i < p.key_count; ++i) {
            if (cmp_key_bytes(less_, p.inner_keys[i].key.data(), p.inner_keys[i].key_len, key) <= 0) {
                child = static_cast<std::uint16_t>(i + 1);
            }
        }
        if (child >= p.children.size()) throw std::runtime_error("Corrupt inner index page");
        page = p.children[child];
    }
}

DiskBspIndex::LeafEntry DiskBspIndex::subtree_min_entry(const DiskBspIndex& idx,
                                                        std::uint32_t page_id) {
    auto p = idx.read_page(page_id);
    while (p.type != NodeType::Leaf) {
        if (p.children.empty()) throw std::runtime_error("Empty internal index node");
        p = idx.read_page(p.children.front());
    }
    if (p.leaf_entries.empty()) throw std::runtime_error("Empty leaf in index subtree");
    return p.leaf_entries.front();
}

std::uint32_t DiskBspIndex::leftmost_leaf(std::uint32_t page_id) const {
    auto p = read_page(page_id);
    while (p.type != NodeType::Leaf) {
        if (p.children.empty()) throw std::runtime_error("Empty internal index node");
        page_id = p.children.front();
        p = read_page(page_id);
    }
    return p.page_id;
}

void DiskBspIndex::rebuild_internal_keys(ParsedPage& node) {
    if (node.type == NodeType::Leaf) {
        throw std::logic_error("Leaf node has no internal separators");
    }
    if (node.children.empty()) {
        node.inner_keys.clear();
        node.key_count = 0;
        return;
    }
    node.inner_keys.resize(node.children.size() - 1);
    for (std::size_t i = 1; i < node.children.size(); ++i) {
        node.inner_keys[i - 1] = subtree_min_entry(*this, node.children[i]);
    }
    node.key_count = static_cast<std::uint8_t>(node.inner_keys.size());
}

void DiskBspIndex::propagate_subtree_min_change(std::uint32_t page_id,
                                                const std::vector<std::uint32_t>& path) {
    if (path.size() < 2 || path.back() != page_id) return;

    auto current_page_id = page_id;
    auto current_min = subtree_min_entry(*this, current_page_id);

    for (std::size_t i = path.size() - 1; i > 0; --i) {
        auto parent = read_page(path[i - 1]);
        if (parent.type == NodeType::Leaf) {
            throw std::logic_error("Leaf cannot be parent");
        }
        auto child_it = std::find(parent.children.begin(), parent.children.end(), current_page_id);
        if (child_it == parent.children.end()) return;
        const auto child_index = static_cast<std::size_t>(child_it - parent.children.begin());
        if (child_index > 0) {
            parent.inner_keys[child_index - 1] = current_min;
            parent.key_count = static_cast<std::uint8_t>(parent.inner_keys.size());
            write_page(parent);
            return;
        }
        current_page_id = parent.page_id;
        current_min = subtree_min_entry(*this, current_page_id);
    }
}

void DiskBspIndex::promote_single_child_root(ParsedPage& root) {
    if (root.type == NodeType::Leaf || root.children.size() != 1) {
        throw std::logic_error("Root must have exactly one child to promote");
    }
    auto child = read_page(root.children.front());
    root_page_ = child.page_id;
    first_leaf_page_ = child.type == NodeType::Leaf ? child.page_id : leftmost_leaf(child.page_id);
}

void DiskBspIndex::insert_into_parent(ParsedPage& left, const LeafEntry& separator,
                                      ParsedPage& right, const std::vector<std::uint32_t>& left_path) {
    if (left_path.empty() || left_path.back() != left.page_id) {
        throw std::logic_error("Invalid path to left node");
    }

    write_page(left);
    write_page(right);

    if (left_path.size() == 1) {
        const auto new_root_id = allocate_node_page();
        ParsedPage new_root;
        new_root.page_id = new_root_id;
        new_root.type = NodeType::Inner;
        new_root.children = {left.page_id, right.page_id};
        new_root.inner_keys = {separator};
        new_root.key_count = 1;
        write_page(new_root);
        root_page_ = new_root_id;
        return;
    }

    const auto parent_id = left_path[left_path.size() - 2];
    auto parent = read_page(parent_id);
    if (parent.type == NodeType::Leaf) throw std::logic_error("Leaf cannot be parent");

    auto child_it = std::find(parent.children.begin(), parent.children.end(), left.page_id);
    if (child_it == parent.children.end()) throw std::logic_error("Parent missing child");
    const auto child_index = static_cast<std::size_t>(child_it - parent.children.begin());

    parent.inner_keys.insert(parent.inner_keys.begin() + static_cast<std::ptrdiff_t>(child_index),
                             separator);
    parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(child_index + 1),
                           right.page_id);
    parent.key_count = static_cast<std::uint8_t>(parent.inner_keys.size());

    rebuild_internal_keys(parent);

    if (!inner_overflow(parent.inner_keys, parent.children)) {
        write_page(parent);
        return;
    }

    std::vector<std::uint32_t> parent_path(left_path.begin(), left_path.end() - 1);
    split_internal(parent, parent_path);
}

bool DiskBspIndex::try_give_to_right_leaf(ParsedPage& leaf, ParsedPage& parent,
                                          std::size_t child_index) {
    if (child_index + 1 >= parent.children.size()) return false;
    auto right = read_page(parent.children[child_index + 1]);
    if (right.type != NodeType::Leaf || leaf.leaf_entries.empty()) return false;

    auto cand_leaf = leaf;
    auto cand_right = right;
    auto moved = cand_leaf.leaf_entries.back();
    cand_leaf.leaf_entries.pop_back();
    cand_right.leaf_entries.insert(cand_right.leaf_entries.begin(), std::move(moved));

    if (cand_leaf.leaf_entries.empty()) return false;
    if (leaf_overflow(cand_right.leaf_entries) || leaf_overflow(cand_leaf.leaf_entries)) {
        return false;
    }

    leaf = std::move(cand_leaf);
    right = std::move(cand_right);
    write_page(leaf);
    write_page(right);
    rebuild_internal_keys(parent);
    write_page(parent);
    return true;
}

bool DiskBspIndex::try_give_to_left_leaf(ParsedPage& leaf, ParsedPage& parent,
                                         std::size_t child_index) {
    if (child_index == 0 || leaf.leaf_entries.empty()) return false;
    auto left = read_page(parent.children[child_index - 1]);
    if (left.type != NodeType::Leaf) return false;

    auto cand_leaf = leaf;
    auto cand_left = left;
    auto moved = cand_left.leaf_entries.back();
    cand_left.leaf_entries.pop_back();
    cand_leaf.leaf_entries.insert(cand_leaf.leaf_entries.begin(), std::move(moved));

    if (cand_left.leaf_entries.empty()) return false;
    if (leaf_overflow(cand_leaf.leaf_entries) || leaf_overflow(cand_left.leaf_entries)) {
        return false;
    }

    leaf = std::move(cand_leaf);
    left = std::move(cand_left);
    write_page(leaf);
    write_page(left);
    rebuild_internal_keys(parent);
    write_page(parent);
    return true;
}

void DiskBspIndex::split_leaf_pair(ParsedPage& left, ParsedPage& right, ParsedPage& parent,
                                   std::size_t parent_index,
                                   const std::vector<std::uint32_t>& parent_path) {
    struct Item {
        LeafEntry entry;
    };
    std::vector<Item> items;
    items.reserve(left.leaf_entries.size() + right.leaf_entries.size());
    for (const auto& e : left.leaf_entries) items.push_back({e});
    for (const auto& e : right.leaf_entries) items.push_back({e});
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return compare_encoded_keys(a.entry.key.data(), a.entry.key_len,
                                    b.entry.key.data(), b.entry.key_len) < 0;
    });

    const auto counts = three_way_counts(items.size());
    const auto first_count  = counts[0];
    const auto second_count = counts[1];

    const auto middle_id = allocate_node_page();
    ParsedPage middle;
    middle.page_id = middle_id;
    middle.type = NodeType::Leaf;
    middle.next_leaf = right.page_id;

    left.leaf_entries.clear();
    middle.leaf_entries.clear();
    right.leaf_entries.clear();

    for (std::size_t i = 0; i < items.size(); ++i) {
        ParsedPage* target = &right;
        if (i < first_count) {
            target = &left;
        } else if (i < first_count + second_count) {
            target = &middle;
        }
        target->leaf_entries.push_back(items[i].entry);
    }

    if (left.leaf_entries.empty() || middle.leaf_entries.empty() || right.leaf_entries.empty()) {
        throw std::logic_error("Leaf pair split produced empty node");
    }

    left.next_leaf = middle.page_id;
    write_page(left);
    write_page(middle);
    write_page(right);

    parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(parent_index + 1),
                           middle.page_id);
    rebuild_internal_keys(parent);

    if (!inner_overflow(parent.inner_keys, parent.children)) {
        write_page(parent);
        return;
    }
    split_internal(parent, parent_path);
}

void DiskBspIndex::redistribute_leaf_pair(ParsedPage& left, ParsedPage& right, ParsedPage& parent,
                                          const std::vector<std::uint32_t>& parent_path) {
    std::vector<LeafEntry> merged;
    merged.reserve(left.leaf_entries.size() + right.leaf_entries.size());
    merged.insert(merged.end(), left.leaf_entries.begin(), left.leaf_entries.end());
    merged.insert(merged.end(), right.leaf_entries.begin(), right.leaf_entries.end());

    const auto counts = three_way_counts(merged.size());
    left.leaf_entries.assign(merged.begin(),
                             merged.begin() + static_cast<std::ptrdiff_t>(counts[0]));
    std::vector<LeafEntry> mid(merged.begin() + static_cast<std::ptrdiff_t>(counts[0]),
                               merged.begin() + static_cast<std::ptrdiff_t>(counts[0] + counts[1]));
    right.leaf_entries.assign(merged.begin() + static_cast<std::ptrdiff_t>(counts[0] + counts[1]),
                              merged.end());

    if (leaf_overflow(left.leaf_entries) || leaf_overflow(right.leaf_entries)) {
        throw std::length_error("Redistributed leaf siblings do not fit");
    }

    const auto middle_id = allocate_node_page();
    ParsedPage middle;
    middle.page_id = middle_id;
    middle.type = NodeType::Leaf;
    middle.leaf_entries = std::move(mid);
    middle.next_leaf = right.page_id;
    left.next_leaf = middle.page_id;

    write_page(left);
    write_page(middle);
    write_page(right);

    auto child_it = std::find(parent.children.begin(), parent.children.end(), left.page_id);
    if (child_it == parent.children.end()) throw std::logic_error("Parent missing left child");
    const auto idx = static_cast<std::size_t>(child_it - parent.children.begin());
    parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(idx + 1),
                           middle.page_id);
    rebuild_internal_keys(parent);
    write_page(parent);
    propagate_subtree_min_change(parent.page_id, parent_path);
}

void DiskBspIndex::split_leaf(ParsedPage& leaf, const std::vector<std::uint32_t>& path) {
    if (path.size() > 1) {
        const auto parent_id = path[path.size() - 2];
        auto parent = read_page(parent_id);
        if (parent.type == NodeType::Leaf) throw std::logic_error("Leaf cannot be parent");

        auto child_it = std::find(parent.children.begin(), parent.children.end(), leaf.page_id);
        if (child_it == parent.children.end()) throw std::logic_error("Parent missing child");
        const auto child_index = static_cast<std::size_t>(child_it - parent.children.begin());

        if (try_give_to_right_leaf(leaf, parent, child_index)) return;
        if (try_give_to_left_leaf(leaf, parent, child_index)) return;

        std::vector<std::uint32_t> parent_path(path.begin(), path.end() - 1);

        if (child_index + 1 < parent.children.size()) {
            auto right = read_page(parent.children[child_index + 1]);
            if (right.type != NodeType::Leaf) throw std::logic_error("Expected leaf sibling");
            split_leaf_pair(leaf, right, parent, child_index, parent_path);
            return;
        }
        if (child_index > 0) {
            auto left = read_page(parent.children[child_index - 1]);
            if (left.type != NodeType::Leaf) throw std::logic_error("Expected leaf sibling");
            split_leaf_pair(left, leaf, parent, child_index - 1, parent_path);
            return;
        }
    }

    const auto right_id = allocate_node_page();
    ParsedPage right;
    right.page_id = right_id;
    right.type = NodeType::Leaf;
    right.next_leaf = leaf.next_leaf;

    const auto split_at = leaf.leaf_entries.size() / 2;
    right.leaf_entries.assign(leaf.leaf_entries.begin() + static_cast<std::ptrdiff_t>(split_at),
                              leaf.leaf_entries.end());
    leaf.leaf_entries.erase(leaf.leaf_entries.begin() + static_cast<std::ptrdiff_t>(split_at),
                            leaf.leaf_entries.end());
    leaf.next_leaf = right.page_id;

    if (right.leaf_entries.empty()) throw std::logic_error("Leaf split produced empty right node");

    const auto separator = right.leaf_entries.front();
    insert_into_parent(leaf, separator, right, path);
}

void DiskBspIndex::split_internal_pair(ParsedPage& left, ParsedPage& right, ParsedPage& parent,
                                       std::size_t parent_index,
                                       const std::vector<std::uint32_t>& parent_path) {
    std::vector<std::uint32_t> children;
    children.reserve(left.children.size() + right.children.size());
    children.insert(children.end(), left.children.begin(), left.children.end());
    children.insert(children.end(), right.children.begin(), right.children.end());

    const auto counts = three_way_counts(children.size());
    left.children.assign(children.begin(),
                         children.begin() + static_cast<std::ptrdiff_t>(counts[0]));
    std::vector<std::uint32_t> mid(children.begin() + static_cast<std::ptrdiff_t>(counts[0]),
                                   children.begin() + static_cast<std::ptrdiff_t>(counts[0] + counts[1]));
    right.children.assign(children.begin() + static_cast<std::ptrdiff_t>(counts[0] + counts[1]),
                          children.end());

    rebuild_internal_keys(left);
    rebuild_internal_keys(right);

    if (inner_overflow(left.inner_keys, left.children) ||
        inner_overflow(right.inner_keys, right.children)) {
        throw std::length_error("Redistributed internal siblings do not fit");
    }

    const auto middle_id = allocate_node_page();
    ParsedPage middle;
    middle.page_id = middle_id;
    middle.type = NodeType::Inner;
    middle.children = std::move(mid);
    rebuild_internal_keys(middle);

    write_page(left);
    write_page(middle);
    write_page(right);

    parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(parent_index + 1),
                           middle.page_id);
    rebuild_internal_keys(parent);

    if (!inner_overflow(parent.inner_keys, parent.children)) {
        write_page(parent);
        return;
    }
    split_internal(parent, parent_path);
}

bool DiskBspIndex::try_redistribute_internal_with_right(
    ParsedPage& node, ParsedPage& parent, std::size_t child_index,
    const std::vector<std::uint32_t>& parent_path) {
    if (child_index + 1 >= parent.children.size()) return false;
    auto right = read_page(parent.children[child_index + 1]);
    if (right.type == NodeType::Leaf) return false;

    auto cand = node;
    auto cand_right = right;
    if (cand_right.children.size() <= 1) return false;

    const auto moved = cand_right.children.front();
    cand_right.children.erase(cand_right.children.begin());
    cand.children.push_back(moved);

    rebuild_internal_keys(cand);
    rebuild_internal_keys(cand_right);

    if (inner_overflow(cand.inner_keys, cand.children) ||
        inner_overflow(cand_right.inner_keys, cand_right.children)) {
        return false;
    }

    node = std::move(cand);
    right = std::move(cand_right);
    write_page(node);
    write_page(right);
    rebuild_internal_keys(parent);
    write_page(parent);
    propagate_subtree_min_change(parent.page_id, parent_path);
    return true;
}

bool DiskBspIndex::try_redistribute_internal_with_left(
    ParsedPage& node, ParsedPage& parent, std::size_t child_index,
    const std::vector<std::uint32_t>& parent_path) {
    if (child_index == 0) return false;
    auto left = read_page(parent.children[child_index - 1]);
    if (left.type == NodeType::Leaf) return false;

    auto cand = node;
    auto cand_left = left;
    if (cand_left.children.size() <= 1) return false;

    const auto moved = cand_left.children.back();
    cand_left.children.pop_back();
    cand.children.insert(cand.children.begin(), moved);

    rebuild_internal_keys(cand);
    rebuild_internal_keys(cand_left);

    if (inner_overflow(cand.inner_keys, cand.children) ||
        inner_overflow(cand_left.inner_keys, cand_left.children)) {
        return false;
    }

    node = std::move(cand);
    left = std::move(cand_left);
    write_page(node);
    write_page(left);
    rebuild_internal_keys(parent);
    write_page(parent);
    propagate_subtree_min_change(parent.page_id, parent_path);
    return true;
}

void DiskBspIndex::redistribute_internal_pair(ParsedPage& left, ParsedPage& right,
                                            ParsedPage& parent,
                                            const std::vector<std::uint32_t>& parent_path) {
    std::vector<std::uint32_t> children;
    children.reserve(left.children.size() + right.children.size());
    children.insert(children.end(), left.children.begin(), left.children.end());
    children.insert(children.end(), right.children.begin(), right.children.end());

    const auto left_count = (children.size() + 1) / 2;
    left.children.assign(children.begin(),
                         children.begin() + static_cast<std::ptrdiff_t>(left_count));
    right.children.assign(children.begin() + static_cast<std::ptrdiff_t>(left_count),
                          children.end());

    rebuild_internal_keys(left);
    rebuild_internal_keys(right);

    if (inner_overflow(left.inner_keys, left.children) ||
        inner_overflow(right.inner_keys, right.children)) {
        throw std::length_error("Redistributed internal siblings do not fit");
    }

    write_page(left);
    write_page(right);
    rebuild_internal_keys(parent);
    write_page(parent);
    propagate_subtree_min_change(parent.page_id, parent_path);
}

void DiskBspIndex::split_internal(ParsedPage& node, const std::vector<std::uint32_t>& path) {
    if (path.size() > 1) {
        const auto parent_id = path[path.size() - 2];
        auto parent = read_page(parent_id);
        if (parent.type == NodeType::Leaf) throw std::logic_error("Leaf cannot be parent");

        auto child_it = std::find(parent.children.begin(), parent.children.end(), node.page_id);
        if (child_it == parent.children.end()) throw std::logic_error("Parent missing child");
        const auto child_index = static_cast<std::size_t>(child_it - parent.children.begin());

        std::vector<std::uint32_t> parent_path(path.begin(), path.end() - 1);

        if (try_redistribute_internal_with_right(node, parent, child_index, parent_path)) return;
        if (try_redistribute_internal_with_left(node, parent, child_index, parent_path)) return;

        if (child_index + 1 < parent.children.size()) {
            auto right = read_page(parent.children[child_index + 1]);
            if (right.type == NodeType::Leaf) throw std::logic_error("Expected internal sibling");
            split_internal_pair(node, right, parent, child_index, parent_path);
            return;
        }
        if (child_index > 0) {
            auto left = read_page(parent.children[child_index - 1]);
            if (left.type == NodeType::Leaf) throw std::logic_error("Expected internal sibling");
            split_internal_pair(left, node, parent, child_index - 1, parent_path);
            return;
        }
    }

    const auto right_id = allocate_node_page();
    ParsedPage right;
    right.page_id = right_id;
    right.type = NodeType::Inner;

    const auto split_at = node.children.size() / 2;
    right.children.assign(node.children.begin() + static_cast<std::ptrdiff_t>(split_at),
                          node.children.end());
    node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(split_at),
                        node.children.end());

    rebuild_internal_keys(node);
    rebuild_internal_keys(right);

    if (right.children.empty()) throw std::logic_error("Internal split produced empty right child");

    const auto separator = subtree_min_entry(*this, right.page_id);
    insert_into_parent(node, separator, right, path);
}

void DiskBspIndex::handle_internal_after_child_removed(ParsedPage& node,
                                                       const std::vector<std::uint32_t>& path) {
    if (node.type == NodeType::Leaf) throw std::logic_error("Expected internal node");
    if (path.empty() || path.back() != node.page_id) throw std::logic_error("Invalid path");

    rebuild_internal_keys(node);

    if (node.page_id == root_page_) {
        if (node.children.size() == 1) {
            promote_single_child_root(node);
            return;
        }
        write_page(node);
        return;
    }

    if (inner_underflow(node.inner_keys, node.children, false)) {
        fix_internal_underflow(node, path);
        return;
    }

    write_page(node);
    propagate_subtree_min_change(node.page_id, path);
}

void DiskBspIndex::fix_leaf_underflow(ParsedPage& leaf, const std::vector<std::uint32_t>& path) {
    if (path.empty() || path.back() != leaf.page_id) throw std::logic_error("Invalid path");
    if (leaf.page_id == root_page_) {
        write_page(leaf);
        return;
    }

    const auto parent_id = path[path.size() - 2];
    auto parent = read_page(parent_id);
    if (parent.type == NodeType::Leaf) throw std::logic_error("Leaf cannot be parent");

    auto child_it = std::find(parent.children.begin(), parent.children.end(), leaf.page_id);
    if (child_it == parent.children.end()) throw std::logic_error("Parent missing child");
    const auto child_index = static_cast<std::size_t>(child_it - parent.children.begin());
    std::vector<std::uint32_t> parent_path(path.begin(), path.end() - 1);

    if (child_index > 0) {
        auto left = read_page(parent.children[child_index - 1]);
        if (left.type != NodeType::Leaf) throw std::logic_error("Expected leaf sibling");
        if (!leaf_underflow(left.leaf_entries, false)) {
            leaf.leaf_entries.insert(leaf.leaf_entries.begin(), left.leaf_entries.back());
            left.leaf_entries.pop_back();
            write_page(left);
            write_page(leaf);
            rebuild_internal_keys(parent);
            write_page(parent);
            propagate_subtree_min_change(parent.page_id, parent_path);
            return;
        }
    }

    if (child_index + 1 < parent.children.size()) {
        auto right = read_page(parent.children[child_index + 1]);
        if (right.type != NodeType::Leaf) throw std::logic_error("Expected leaf sibling");
        if (!leaf_underflow(right.leaf_entries, false)) {
            leaf.leaf_entries.push_back(right.leaf_entries.front());
            right.leaf_entries.erase(right.leaf_entries.begin());
            write_page(right);
            write_page(leaf);
            rebuild_internal_keys(parent);
            write_page(parent);
            propagate_subtree_min_change(parent.page_id, parent_path);
            return;
        }
    }

    if (child_index > 0) {
        auto left = read_page(parent.children[child_index - 1]);
        ParsedPage merged = left;
        merged.leaf_entries.insert(merged.leaf_entries.end(), leaf.leaf_entries.begin(),
                                   leaf.leaf_entries.end());
        merged.next_leaf = leaf.next_leaf;
        if (leaf_overflow(merged.leaf_entries)) {
            redistribute_leaf_pair(left, leaf, parent, parent_path);
            return;
        }
        if (first_leaf_page_ == leaf.page_id) first_leaf_page_ = merged.page_id;
        parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(child_index));
        if (child_index > 0) {
            parent.inner_keys.erase(parent.inner_keys.begin() + static_cast<std::ptrdiff_t>(child_index - 1));
        }
        parent.key_count = static_cast<std::uint8_t>(parent.inner_keys.size());
        write_page(merged);
        handle_internal_after_child_removed(parent, parent_path);
        return;
    }

    if (child_index + 1 >= parent.children.size()) {
        throw std::logic_error("Leaf has no sibling for merge");
    }
    auto right = read_page(parent.children[child_index + 1]);
    ParsedPage merged = leaf;
    merged.leaf_entries.insert(merged.leaf_entries.end(), right.leaf_entries.begin(),
                               right.leaf_entries.end());
    merged.next_leaf = right.next_leaf;
    if (leaf_overflow(merged.leaf_entries)) {
        redistribute_leaf_pair(leaf, right, parent, parent_path);
        return;
    }
    if (first_leaf_page_ == right.page_id) first_leaf_page_ = merged.page_id;
    parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(child_index + 1));
    parent.inner_keys.erase(parent.inner_keys.begin() + static_cast<std::ptrdiff_t>(child_index));
    parent.key_count = static_cast<std::uint8_t>(parent.inner_keys.size());
    write_page(merged);
    handle_internal_after_child_removed(parent, parent_path);
}

void DiskBspIndex::fix_internal_underflow(ParsedPage& node, const std::vector<std::uint32_t>& path) {
    if (path.empty() || path.back() != node.page_id) throw std::logic_error("Invalid path");

    if (node.page_id == root_page_) {
        if (node.children.size() == 1) {
            promote_single_child_root(node);
        } else {
            rebuild_internal_keys(node);
            write_page(node);
        }
        return;
    }

    const auto parent_id = path[path.size() - 2];
    auto parent = read_page(parent_id);
    if (parent.type == NodeType::Leaf) throw std::logic_error("Leaf cannot be parent");

    auto child_it = std::find(parent.children.begin(), parent.children.end(), node.page_id);
    if (child_it == parent.children.end()) throw std::logic_error("Parent missing child");
    const auto child_index = static_cast<std::size_t>(child_it - parent.children.begin());
    std::vector<std::uint32_t> parent_path(path.begin(), path.end() - 1);

    if (child_index > 0) {
        auto left = read_page(parent.children[child_index - 1]);
        if (left.type == NodeType::Leaf) throw std::logic_error("Expected internal sibling");
        if (!inner_underflow(left.inner_keys, left.children, false) && left.children.size() > 1) {
            node.children.insert(node.children.begin(), left.children.back());
            left.children.pop_back();
            rebuild_internal_keys(left);
            rebuild_internal_keys(node);
            write_page(left);
            write_page(node);
            rebuild_internal_keys(parent);
            write_page(parent);
            propagate_subtree_min_change(parent.page_id, parent_path);
            return;
        }
    }

    if (child_index + 1 < parent.children.size()) {
        auto right = read_page(parent.children[child_index + 1]);
        if (right.type == NodeType::Leaf) throw std::logic_error("Expected internal sibling");
        if (!inner_underflow(right.inner_keys, right.children, false) && right.children.size() > 1) {
            node.children.push_back(right.children.front());
            right.children.erase(right.children.begin());
            rebuild_internal_keys(right);
            rebuild_internal_keys(node);
            write_page(right);
            write_page(node);
            rebuild_internal_keys(parent);
            write_page(parent);
            propagate_subtree_min_change(parent.page_id, parent_path);
            return;
        }
    }

    if (child_index > 0) {
        auto left = read_page(parent.children[child_index - 1]);
        ParsedPage merged = left;
        merged.children.insert(merged.children.end(), node.children.begin(), node.children.end());
        rebuild_internal_keys(merged);
        if (inner_overflow(merged.inner_keys, merged.children)) {
            redistribute_internal_pair(left, node, parent, parent_path);
            return;
        }
        parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(child_index));
        if (child_index > 0) {
            parent.inner_keys.erase(parent.inner_keys.begin() + static_cast<std::ptrdiff_t>(child_index - 1));
        }
        parent.key_count = static_cast<std::uint8_t>(parent.inner_keys.size());
        write_page(merged);
        handle_internal_after_child_removed(parent, parent_path);
        return;
    }

    if (child_index + 1 >= parent.children.size()) {
        throw std::logic_error("Internal node has no sibling for merge");
    }
    auto right = read_page(parent.children[child_index + 1]);
    ParsedPage merged = node;
    merged.children.insert(merged.children.end(), right.children.begin(), right.children.end());
    rebuild_internal_keys(merged);
    if (inner_overflow(merged.inner_keys, merged.children)) {
        redistribute_internal_pair(node, right, parent, parent_path);
        return;
    }
    parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(child_index + 1));
    parent.inner_keys.erase(parent.inner_keys.begin() + static_cast<std::ptrdiff_t>(child_index));
    parent.key_count = static_cast<std::uint8_t>(parent.inner_keys.size());
    write_page(merged);
    handle_internal_after_child_removed(parent, parent_path);
}

void DiskBspIndex::insert(const Value& key, std::uint64_t rid) {
    if (key.is_null()) throw std::invalid_argument("Cannot index NULL key");
    auto ent = make_entry(key, rid);

    if (size_ == 0) {
        ParsedPage root;
        root.page_id = root_page_;
        root.type = NodeType::Leaf;
        root.leaf_entries.push_back(std::move(ent));
        root.next_leaf = 0;
        write_page(root);
        size_ = 1;
        write_superblock();
        return;
    }

    auto result = find_leaf(key);
    auto& leaf = result.leaf;
    const auto slot = lower_bound_slot(leaf, key);
    if (slot < leaf.leaf_entries.size()) {
        const auto& existing = leaf.leaf_entries[slot];
        if (keys_equal_bytes(existing.key.data(), existing.key_len, ent.key.data(), ent.key_len)) {
            throw std::runtime_error("Duplicate index key");
        }
    }

    leaf.leaf_entries.insert(leaf.leaf_entries.begin() + static_cast<std::ptrdiff_t>(slot),
                             std::move(ent));

    if (!leaf_overflow(leaf.leaf_entries)) {
        write_page(leaf);
    } else {
        split_leaf(leaf, result.path);
    }

    ++size_;
    write_superblock();
}

bool DiskBspIndex::erase(const Value& key) {
    if (size_ == 0) return false;

    auto result = find_leaf(key);
    auto& leaf = result.leaf;
    const auto slot = lower_bound_slot(leaf, key);
    if (slot >= leaf.leaf_entries.size()) return false;

    const auto probe = make_entry(key, 0);
    const auto& existing = leaf.leaf_entries[slot];
    if (!keys_equal_bytes(existing.key.data(), existing.key_len,
                          probe.key.data(), probe.key_len)) {
        return false;
    }

    leaf.leaf_entries.erase(leaf.leaf_entries.begin() + static_cast<std::ptrdiff_t>(slot));
    --size_;

    if (size_ == 0) {
        leaf.leaf_entries.clear();
        write_page(leaf);
        root_page_ = leaf.page_id;
        first_leaf_page_ = leaf.page_id;
        write_superblock();
        return true;
    }

    if (leaf.page_id == root_page_ || !leaf_underflow(leaf.leaf_entries, false)) {
        write_page(leaf);
        if (slot == 0 && !leaf.leaf_entries.empty()) {
            propagate_subtree_min_change(leaf.page_id, result.path);
        }
        write_superblock();
        return true;
    }

    fix_leaf_underflow(leaf, result.path);
    write_superblock();
    return true;
}

}  // namespace cw_db
