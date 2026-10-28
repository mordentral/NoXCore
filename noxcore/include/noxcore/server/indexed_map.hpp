// A std::map that keeps an index of its own in step: told of each entry as it's put in or taken
// out, however that's done (operator[], emplace, insert, erase, extract, clear), so the index may
// hold pointers to the entries (a map's nodes stay where they are). A copy indexes its own
// entries afresh; a move takes the index with the nodes.
#pragma once

#include <map>
#include <utility>

namespace nox::server {

/// `Index` has add(const K&, V&), remove(const K&, V&) and clear(). (Seen as its std::map only
/// to read it, or to copy it: every change goes through the IndexedMap's own.)
template <class K, class V, class Index>
class IndexedMap : public std::map<K, V> {
    using Base = std::map<K, V>;

public:
    using typename Base::const_iterator;
    using typename Base::const_reverse_iterator;
    using typename Base::insert_return_type;
    using typename Base::iterator;
    using typename Base::key_type;
    using typename Base::mapped_type;
    using typename Base::node_type;
    using typename Base::reverse_iterator;
    using typename Base::size_type;
    using typename Base::value_type;

    using Base::at;
    using Base::begin;
    using Base::cbegin;
    using Base::cend;
    using Base::contains;
    using Base::count;
    using Base::crbegin;
    using Base::crend;
    using Base::empty;
    using Base::end;
    using Base::equal_range;
    using Base::find;
    using Base::lower_bound;
    using Base::rbegin;
    using Base::rend;
    using Base::size;
    using Base::upper_bound;

    IndexedMap() = default;
    IndexedMap(const IndexedMap& o) : Base(o) { reindex(); }
    IndexedMap(IndexedMap&& o) noexcept : Base(std::move(o)), index_(std::move(o.index_))
    {
        o.Base::clear();
        o.index_.clear();
    }
    IndexedMap& operator=(const IndexedMap& o)
    {
        if (this != &o) {
            Base::operator=(o);
            reindex();
        }
        return *this;
    }
    IndexedMap& operator=(IndexedMap&& o) noexcept
    {
        if (this != &o) {
            Base::operator=(std::move(o));
            index_ = std::move(o.index_);
            o.Base::clear();
            o.index_.clear();
        }
        return *this;
    }

    V& operator[](const K& k) { return try_emplace(k).first->second; }
    template <class... Args>
    std::pair<iterator, bool> try_emplace(const K& k, Args&&... args)
    {
        auto r = Base::try_emplace(k, std::forward<Args>(args)...);
        if (r.second) {
            index_.add(r.first->first, r.first->second);
        }
        return r;
    }
    template <class... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        auto r = Base::emplace(std::forward<Args>(args)...);
        if (r.second) {
            index_.add(r.first->first, r.first->second);
        }
        return r;
    }
    std::pair<iterator, bool> insert(value_type&& v) { return emplace(std::move(v)); }
    std::pair<iterator, bool> insert(const value_type& v) { return emplace(v); }
    insert_return_type insert(node_type&& n)
    {
        auto r = Base::insert(std::move(n));
        if (r.inserted) {
            index_.add(r.position->first, r.position->second);
        }
        return r;
    }
    iterator erase(const_iterator it)
    {
        index_.remove(it->first, const_cast<V&>(it->second));
        return Base::erase(it);
    }
    iterator erase(iterator it) { return erase(const_iterator(it)); }
    size_type erase(const K& k)
    {
        const auto it = Base::find(k);
        if (it == Base::end()) {
            return 0;
        }
        erase(const_iterator(it));
        return 1;
    }
    node_type extract(const_iterator it)
    {
        index_.remove(it->first, const_cast<V&>(it->second));
        return Base::extract(it);
    }
    node_type extract(iterator it) { return extract(const_iterator(it)); }
    node_type extract(const K& k)
    {
        const auto it = Base::find(k);
        return it == Base::end() ? node_type{} : extract(const_iterator(it));
    }
    void clear() noexcept
    {
        Base::clear();
        index_.clear();
    }

    // (The map's other changes, hidden: none of them keeps the index.)
    template <class... Args>
    void emplace_hint(Args&&...) = delete;
    template <class... Args>
    void insert_or_assign(Args&&...) = delete;
    template <class... Args>
    void merge(Args&&...) = delete;
    template <class... Args>
    void swap(Args&&...) = delete;

    const Index& index() const noexcept { return index_; }

private:
    void reindex()
    {
        index_.clear();
        for (auto& [k, v] : static_cast<Base&>(*this)) {
            index_.add(k, v);
        }
    }

    Index index_;
};

} // namespace nox::server
