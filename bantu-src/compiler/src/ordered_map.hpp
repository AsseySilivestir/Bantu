#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  ordered_map.hpp — a string-keyed map that iterates in INSERTION order.
//
//  Bantu objects were `std::unordered_map<std::string, Value>`, so iterating a
//  dict yielded hash order -- which is not insertion order, and worse, is not
//  the SAME order on every platform. The same program printed:
//
//      {"k":1,"v":2,"w":3,"alpha":4,"beta":5}
//    macOS   (libc++)     -> [k, v, alpha, w, beta]
//    Linux   (libstdc++)  -> [beta, w, v, alpha, k]
//
//  That silently made platform-dependent: json.stringify() key order, every
//  JSON API response body, CSV column order, and arctic DataFrame columns --
//  a DataFrame built as {"k":…, "v":…} reported its columns as [v, k] on Linux
//  and [k, v] on macOS. It is what made tests/arctic_api_test.b fail on Linux
//  and pass on macOS, which is how it was finally noticed.
//
//  Every mainstream language settled this the same way: Python dicts have been
//  insertion-ordered since 3.7, JavaScript objects since ES2015, Ruby hashes
//  since 1.9. Bantu now matches, and matches itself across platforms.
//
//  Implementation: a vector of entries in insertion order, plus a hash index
//  from key to position. Lookup stays O(1); iteration is a contiguous walk,
//  which is friendlier to the cache than chasing an unordered_map's buckets.
//  Erase is O(n) because it has to close the gap and reindex -- deliberate,
//  since dict deletion is rare and order must survive it.
// ════════════════════════════════════════════════════════════════════════════

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// `Value` is incomplete where ObjectMap is declared (see the note in
// types.hpp), so this template must not require T to be complete at
// declaration time. std::vector permits that; the members are only
// instantiated once T is complete.
template <typename T>
class BantuOrderedMap {
public:
    using value_type     = std::pair<std::string, T>;
    using storage        = std::vector<value_type>;
    using iterator       = typename storage::iterator;
    using const_iterator = typename storage::const_iterator;
    using size_type      = std::size_t;
    using mapped_type    = T;
    using key_type       = std::string;

    iterator       begin()        { return items_.begin(); }
    iterator       end()          { return items_.end(); }
    const_iterator begin()  const { return items_.begin(); }
    const_iterator end()    const { return items_.end(); }
    const_iterator cbegin() const { return items_.begin(); }
    const_iterator cend()   const { return items_.end(); }

    bool      empty() const { return items_.empty(); }
    size_type size()  const { return items_.size(); }

    void clear() { items_.clear(); index_.clear(); }

    // Inserts at the END when the key is new, which is what makes iteration
    // insertion-ordered. An existing key keeps its original position.
    T& operator[](const std::string& key) {
        auto it = index_.find(key);
        if (it != index_.end()) return items_[it->second].second;
        index_.emplace(key, items_.size());
        items_.emplace_back(key, T());
        return items_.back().second;
    }

    iterator find(const std::string& key) {
        auto it = index_.find(key);
        return (it == index_.end()) ? items_.end() : items_.begin() + (std::ptrdiff_t)it->second;
    }
    const_iterator find(const std::string& key) const {
        auto it = index_.find(key);
        return (it == index_.end()) ? items_.end() : items_.begin() + (std::ptrdiff_t)it->second;
    }

    size_type count(const std::string& key) const { return index_.count(key); }

    T& at(const std::string& key) {
        auto it = index_.find(key);
        if (it == index_.end()) throw std::out_of_range("BantuOrderedMap::at: " + key);
        return items_[it->second].second;
    }
    const T& at(const std::string& key) const {
        auto it = index_.find(key);
        if (it == index_.end()) throw std::out_of_range("BantuOrderedMap::at: " + key);
        return items_[it->second].second;
    }

    // Returns {iterator, inserted}; an existing key is left untouched, matching
    // std::unordered_map::insert / emplace.
    std::pair<iterator, bool> insert(const value_type& kv) {
        auto it = index_.find(kv.first);
        if (it != index_.end())
            return { items_.begin() + (std::ptrdiff_t)it->second, false };
        index_.emplace(kv.first, items_.size());
        items_.push_back(kv);
        return { items_.end() - 1, true };
    }
    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args) {
        return insert(value_type(std::forward<Args>(args)...));
    }

    size_type erase(const std::string& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return 0;
        items_.erase(items_.begin() + (std::ptrdiff_t)it->second);
        reindex();
        return 1;
    }
    iterator erase(const_iterator pos) {
        std::ptrdiff_t at = pos - items_.cbegin();
        auto next = items_.erase(items_.begin() + at);
        reindex();
        return next;
    }

    bool operator==(const BantuOrderedMap& o) const { return items_ == o.items_; }
    bool operator!=(const BantuOrderedMap& o) const { return !(*this == o); }

private:
    // Positions shift when an entry is removed, so the index is rebuilt whole.
    // O(n), and only on erase.
    void reindex() {
        index_.clear();
        index_.reserve(items_.size());
        for (std::size_t i = 0; i < items_.size(); i++) index_.emplace(items_[i].first, i);
    }

    storage items_;
    std::unordered_map<std::string, std::size_t> index_;
};
