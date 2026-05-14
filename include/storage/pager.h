#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <list>
#include <unordered_map>
#include <vector>

namespace cw_db {

inline constexpr std::uint32_t kPageSize   = 4096;
inline constexpr std::uint32_t kPageMagic  = 0x50414745;  // "PAGE"

inline constexpr std::size_t kDefaultPageCacheCapacity = 64;

class Pager {
public:
    Pager() = default;

    void open(const std::filesystem::path& path, bool create);
    void close();

    bool is_open() const noexcept { return stream_.is_open(); }

    std::uint32_t page_count() const noexcept { return page_count_; }

    void set_cache_capacity(std::size_t capacity);
    std::size_t cache_capacity() const noexcept { return cache_capacity_; }

    std::uint32_t allocate_page();

    const std::vector<std::uint8_t>& read_page(std::uint32_t page_id) const;
    void write_page(std::uint32_t page_id, const std::vector<std::uint8_t>& data);

    void flush();

private:
    struct CacheSlot {
        std::vector<std::uint8_t> data;
        bool                      dirty = false;
    };

    using LruList = std::list<std::uint32_t>;
    using CacheMap = std::unordered_map<std::uint32_t, std::pair<LruList::iterator, CacheSlot>>;

    mutable std::fstream      stream_;
    std::filesystem::path     path_;
    std::uint32_t             page_count_ = 0;

    std::size_t cache_capacity_ = kDefaultPageCacheCapacity;
    mutable LruList           lru_;
    mutable CacheMap          cache_;
    mutable std::vector<std::uint8_t> scratch_;

    void ensure_page_count(std::uint32_t page_id);
    static void check_page_size(const std::vector<std::uint8_t>& data);

    void clear_cache() const;
    void evict_one() const;
    void flush_dirty_pages() const;
    void load_page_from_disk(std::uint32_t page_id) const;
    void put_cache(std::uint32_t page_id, std::vector<std::uint8_t> data, bool dirty) const;
    void touch_cache(std::uint32_t page_id) const;
};

}  // namespace cw_db
