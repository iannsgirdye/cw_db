#include "storage/pager.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace cw_db {

void Pager::set_cache_capacity(std::size_t capacity) {
    cache_capacity_ = capacity;
    while (cache_.size() > cache_capacity_) {
        evict_one();
    }
}

void Pager::clear_cache() const {
    lru_.clear();
    cache_.clear();
}

void Pager::evict_one() const {
    if (lru_.empty()) return;
    const std::uint32_t victim = lru_.back();
    auto it = cache_.find(victim);
    if (it == cache_.end()) {
        lru_.pop_back();
        return;
    }
    if (it->second.second.dirty) {
        const auto off = static_cast<std::streamoff>(victim) * kPageSize;
        stream_.seekp(off, std::ios::beg);
        stream_.write(reinterpret_cast<const char*>(it->second.second.data.data()), kPageSize);
        if (!stream_) {
            throw std::runtime_error("Write failed while evicting page " + std::to_string(victim));
        }
    }
    cache_.erase(it);
    lru_.pop_back();
}

void Pager::flush_dirty_pages() const {
    for (auto& [page_id, slot] : cache_) {
        if (!slot.second.dirty) continue;
        const auto off = static_cast<std::streamoff>(page_id) * kPageSize;
        stream_.seekp(off, std::ios::beg);
        stream_.write(reinterpret_cast<const char*>(slot.second.data.data()), kPageSize);
        if (!stream_) {
            throw std::runtime_error("Write failed for page " + std::to_string(page_id));
        }
        slot.second.dirty = false;
    }
}

void Pager::load_page_from_disk(std::uint32_t page_id) const {
    if (page_id >= page_count_) {
        throw std::runtime_error("Read past end of page file: page " + std::to_string(page_id));
    }
    std::vector<std::uint8_t> data(kPageSize, 0);
    const auto off = static_cast<std::streamoff>(page_id) * kPageSize;
    stream_.seekg(off, std::ios::beg);
    stream_.read(reinterpret_cast<char*>(data.data()), kPageSize);
    if (!stream_) throw std::runtime_error("Read failed for page " + std::to_string(page_id));
    put_cache(page_id, std::move(data), false);
}

void Pager::put_cache(std::uint32_t page_id, std::vector<std::uint8_t> data, bool dirty) const {
    check_page_size(data);
    if (cache_capacity_ == 0) {
        if (dirty) {
            const auto off = static_cast<std::streamoff>(page_id) * kPageSize;
            stream_.seekp(off, std::ios::beg);
            stream_.write(reinterpret_cast<const char*>(data.data()), kPageSize);
            if (!stream_) {
                throw std::runtime_error("Write failed for page " + std::to_string(page_id));
            }
        }
        scratch_ = std::move(data);
        return;
    }
    auto existing = cache_.find(page_id);
    if (existing != cache_.end()) {
        existing->second.second.data = std::move(data);
        existing->second.second.dirty = dirty;
        touch_cache(page_id);
        return;
    }
    while (cache_.size() >= cache_capacity_) {
        evict_one();
    }
    lru_.push_front(page_id);
    cache_.emplace(page_id, std::pair{lru_.begin(), CacheSlot{std::move(data), dirty}});
}

void Pager::touch_cache(std::uint32_t page_id) const {
    auto it = cache_.find(page_id);
    if (it == cache_.end()) return;
    lru_.splice(lru_.begin(), lru_, it->second.first);
    it->second.first = lru_.begin();
}

void Pager::open(const std::filesystem::path& path, bool create) {
    close();
    path_ = path;
    std::ios::openmode mode = std::ios::binary | std::ios::in | std::ios::out;
    if (create) {
        std::filesystem::create_directories(path.parent_path());
        mode |= std::ios::trunc;
    }
    stream_.open(path, mode);
    if (!stream_) {
        stream_.clear();
        stream_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    }
    if (!stream_) throw std::runtime_error("Cannot open page file: " + path.string());

    stream_.seekg(0, std::ios::end);
    auto bytes = stream_.tellg();
    if (bytes < 0) bytes = 0;
    page_count_ = static_cast<std::uint32_t>(bytes / kPageSize);
    stream_.seekg(0, std::ios::beg);
    clear_cache();
}

void Pager::close() {
    if (stream_.is_open()) {
        flush();
        stream_.close();
    }
    page_count_ = 0;
    clear_cache();
}

void Pager::ensure_page_count(std::uint32_t page_id) {
    if (page_id >= page_count_) page_count_ = page_id + 1;
}

void Pager::check_page_size(const std::vector<std::uint8_t>& data) {
    if (data.size() != kPageSize) {
        throw std::runtime_error("Page size must be exactly " + std::to_string(kPageSize));
    }
}

std::uint32_t Pager::allocate_page() {
    const std::uint32_t id = page_count_;
    std::vector<std::uint8_t> page(kPageSize, 0);
    write_page(id, page);
    return id;
}

const std::vector<std::uint8_t>& Pager::read_page(std::uint32_t page_id) const {
    if (cache_capacity_ == 0) {
        if (page_id >= page_count_) {
            throw std::runtime_error("Read past end of page file: page " + std::to_string(page_id));
        }
        scratch_.assign(kPageSize, 0);
        const auto off = static_cast<std::streamoff>(page_id) * kPageSize;
        stream_.seekg(off, std::ios::beg);
        stream_.read(reinterpret_cast<char*>(scratch_.data()), kPageSize);
        if (!stream_) throw std::runtime_error("Read failed for page " + std::to_string(page_id));
        return scratch_;
    }
    auto it = cache_.find(page_id);
    if (it != cache_.end()) {
        touch_cache(page_id);
        return it->second.second.data;
    }
    load_page_from_disk(page_id);
    it = cache_.find(page_id);
    if (it == cache_.end()) {
        throw std::runtime_error("Page cache miss after load: page " + std::to_string(page_id));
    }
    return it->second.second.data;
}

void Pager::write_page(std::uint32_t page_id, const std::vector<std::uint8_t>& data) {
    check_page_size(data);
    ensure_page_count(page_id);
    put_cache(page_id, data, true);
}

void Pager::flush() {
    if (!stream_.is_open()) return;
    flush_dirty_pages();
    stream_.flush();
}

}  // namespace cw_db
