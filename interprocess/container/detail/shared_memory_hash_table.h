#pragma once

#include "../../allocator/offset_ptr.h"
#include "../../allocator/shared_memory_allocator.h"
#include <cmath>
#include <cstddef>
#include <functional>
#include <iterator>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace interprocess::detail
{

template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Allocator = SharedMemoryAllocator<std::pair<const Key, T>>>
class SharedMemoryHashTable
{
private:
    using self_type = SharedMemoryHashTable<Key, T, Hash, KeyEqual, Allocator>;

    struct Node
    {
        OffsetPtr<Node> bucket_prev;
        OffsetPtr<Node> bucket_next;
        OffsetPtr<Node> iter_prev;
        OffsetPtr<Node> iter_next;
        std::pair<const Key, T> value;

        template <typename... Args>
        explicit Node(Args&&... args)
            : bucket_prev(nullptr), bucket_next(nullptr), iter_prev(nullptr), iter_next(nullptr),
              value(std::forward<Args>(args)...)
        {
        }
    };

    struct FreeNode
    {
        OffsetPtr<FreeNode> next;
    };

    template <typename U>
    using rebind_alloc_t = typename Allocator::template rebind<U>::other;

    using node_allocator_type = rebind_alloc_t<Node>;
    using bucket_allocator_type = rebind_alloc_t<OffsetPtr<Node>>;

    template <typename ValueType, typename PointerType, typename ReferenceType>
    class BasicIterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ValueType;
        using difference_type = std::ptrdiff_t;
        using pointer = PointerType;
        using reference = ReferenceType;

        BasicIterator() noexcept : owner(nullptr), node(nullptr)
        {
        }

        BasicIterator(const self_type* owner, Node* node) noexcept : owner(owner), node(node)
        {
        }

        template <typename OtherValueType, typename OtherPointerType, typename OtherReferenceType>
        BasicIterator(
            const BasicIterator<OtherValueType, OtherPointerType, OtherReferenceType>& other,
            typename std::enable_if<std::is_convertible<OtherPointerType, PointerType>::value,
                                    int>::type = 0) noexcept
            : owner(other.owner), node(other.node)
        {
        }

        reference operator*() const
        {
            return node->value;
        }

        pointer operator->() const
        {
            return &node->value;
        }

        BasicIterator& operator++()
        {
            node = node ? node->iter_next.get() : nullptr;
            return *this;
        }

        BasicIterator operator++(int)
        {
            BasicIterator tmp(*this);
            ++(*this);
            return tmp;
        }

        bool operator==(const BasicIterator& other) const noexcept
        {
            return owner == other.owner && node == other.node;
        }

        bool operator!=(const BasicIterator& other) const noexcept
        {
            return !(*this == other);
        }

    private:
        friend class SharedMemoryHashTable<Key, T, Hash, KeyEqual, Allocator>;
        template <typename, typename, typename>
        friend class BasicIterator;

        const self_type* owner;
        Node* node;
    };

    template <typename ValueType, typename PointerType, typename ReferenceType>
    class BasicLocalIterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ValueType;
        using difference_type = std::ptrdiff_t;
        using pointer = PointerType;
        using reference = ReferenceType;

        BasicLocalIterator() noexcept : owner(nullptr), node(nullptr)
        {
        }

        BasicLocalIterator(const self_type* owner, Node* node) noexcept : owner(owner), node(node)
        {
        }

        template <typename OtherValueType, typename OtherPointerType, typename OtherReferenceType>
        BasicLocalIterator(
            const BasicLocalIterator<OtherValueType, OtherPointerType, OtherReferenceType>& other,
            typename std::enable_if<std::is_convertible<OtherPointerType, PointerType>::value,
                                    int>::type = 0) noexcept
            : owner(other.owner), node(other.node)
        {
        }

        reference operator*() const
        {
            return node->value;
        }

        pointer operator->() const
        {
            return &node->value;
        }

        BasicLocalIterator& operator++()
        {
            node = node ? node->bucket_next.get() : nullptr;
            return *this;
        }

        BasicLocalIterator operator++(int)
        {
            BasicLocalIterator tmp(*this);
            ++(*this);
            return tmp;
        }

        bool operator==(const BasicLocalIterator& other) const noexcept
        {
            return owner == other.owner && node == other.node;
        }

        bool operator!=(const BasicLocalIterator& other) const noexcept
        {
            return !(*this == other);
        }

    private:
        friend class SharedMemoryHashTable<Key, T, Hash, KeyEqual, Allocator>;
        template <typename, typename, typename>
        friend class BasicLocalIterator;

        const self_type* owner;
        Node* node;
    };

public:
    using key_type = Key;
    using mapped_type = T;
    using value_type = std::pair<const Key, T>;
    using hasher = Hash;
    using key_equal = KeyEqual;
    using allocator_type = Allocator;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using iterator = BasicIterator<value_type, value_type*, value_type&>;
    using const_iterator = BasicIterator<const value_type, const value_type*, const value_type&>;
    using local_iterator = BasicLocalIterator<value_type, value_type*, value_type&>;
    using const_local_iterator =
        BasicLocalIterator<const value_type, const value_type*, const value_type&>;

    explicit SharedMemoryHashTable(const allocator_type& alloc) noexcept
        : allocator(alloc), node_allocator(alloc.get_manager()), bucket_allocator(alloc.get_manager()),
          hash_function_(), key_equal_(), buckets(nullptr), bucket_count_(0), size_(0),
          max_load_factor_(1.0f), head_(nullptr), tail_(nullptr), free_nodes_(nullptr),
          cached_node_count_(0), node_pool_hits_(0), node_pool_allocations_(0)
    {
        initialize_buckets(default_bucket_count);
    }

    explicit SharedMemoryHashTable(size_type bucket_count, const allocator_type& alloc)
        : allocator(alloc), node_allocator(alloc.get_manager()), bucket_allocator(alloc.get_manager()),
          hash_function_(), key_equal_(), buckets(nullptr), bucket_count_(0), size_(0),
          max_load_factor_(1.0f), head_(nullptr), tail_(nullptr), free_nodes_(nullptr),
          cached_node_count_(0), node_pool_hits_(0), node_pool_allocations_(0)
    {
        initialize_buckets(bucket_count);
    }

    template <typename InputIt,
              typename = std::enable_if_t<!std::is_integral<InputIt>::value, int>>
    SharedMemoryHashTable(InputIt first, InputIt last, const allocator_type& alloc)
        : allocator(alloc), node_allocator(alloc.get_manager()), bucket_allocator(alloc.get_manager()),
          hash_function_(), key_equal_(), buckets(nullptr), bucket_count_(0), size_(0),
          max_load_factor_(1.0f), head_(nullptr), tail_(nullptr), free_nodes_(nullptr),
          cached_node_count_(0), node_pool_hits_(0), node_pool_allocations_(0)
    {
        initialize_buckets(default_bucket_count);
        try
        {
            insert(first, last);
        }
        catch (...)
        {
            clear();
            release_cached_nodes();
            release_buckets();
            throw;
        }
    }

    SharedMemoryHashTable(size_type bucket_count, const hasher& hash,
                          const key_equal& equal, const allocator_type& alloc)
        : allocator(alloc), node_allocator(alloc.get_manager()), bucket_allocator(alloc.get_manager()),
          hash_function_(hash), key_equal_(equal), buckets(nullptr), bucket_count_(0), size_(0),
          max_load_factor_(1.0f), head_(nullptr), tail_(nullptr), free_nodes_(nullptr),
          cached_node_count_(0), node_pool_hits_(0), node_pool_allocations_(0)
    {
        initialize_buckets(bucket_count);
    }

    template <typename InputIt,
              typename = std::enable_if_t<!std::is_integral<InputIt>::value, int>>
    SharedMemoryHashTable(InputIt first, InputIt last, size_type bucket_count, const hasher& hash,
                          const key_equal& equal, const allocator_type& alloc)
        : allocator(alloc), node_allocator(alloc.get_manager()), bucket_allocator(alloc.get_manager()),
          hash_function_(hash), key_equal_(equal), buckets(nullptr), bucket_count_(0), size_(0),
          max_load_factor_(1.0f), head_(nullptr), tail_(nullptr), free_nodes_(nullptr),
          cached_node_count_(0), node_pool_hits_(0), node_pool_allocations_(0)
    {
        initialize_buckets(bucket_count);
        try
        {
            insert(first, last);
        }
        catch (...)
        {
            clear();
            release_cached_nodes();
            release_buckets();
            throw;
        }
    }

    SharedMemoryHashTable(std::initializer_list<value_type> init, const allocator_type& alloc)
        : SharedMemoryHashTable(init.begin(), init.end(), alloc)
    {
    }

    SharedMemoryHashTable(std::initializer_list<value_type> init, size_type bucket_count,
                          const hasher& hash, const key_equal& equal,
                          const allocator_type& alloc)
        : SharedMemoryHashTable(init.begin(), init.end(), bucket_count, hash, equal, alloc)
    {
    }

    SharedMemoryHashTable(const SharedMemoryHashTable& other)
        : allocator(other.allocator), node_allocator(other.allocator.get_manager()),
          bucket_allocator(other.allocator.get_manager()), hash_function_(other.hash_function_),
          key_equal_(other.key_equal_), buckets(nullptr), bucket_count_(0), size_(0),
          max_load_factor_(other.max_load_factor_), head_(nullptr), tail_(nullptr),
          free_nodes_(nullptr), cached_node_count_(0), node_pool_hits_(0),
          node_pool_allocations_(0)
    {
        initialize_buckets(other.bucket_count_);
        try
        {
            for (const_iterator it = other.cbegin(); it != other.cend(); ++it)
            {
                insert(*it);
            }
        }
        catch (...)
        {
            clear();
            release_buckets();
            throw;
        }
    }

    SharedMemoryHashTable(SharedMemoryHashTable&& other) noexcept
        : allocator(other.allocator), node_allocator(other.allocator.get_manager()),
          bucket_allocator(other.allocator.get_manager()), hash_function_(std::move(other.hash_function_)),
          key_equal_(std::move(other.key_equal_)), buckets(other.buckets),
          bucket_count_(other.bucket_count_), size_(other.size_),
          max_load_factor_(other.max_load_factor_), head_(other.head_), tail_(other.tail_),
          free_nodes_(other.free_nodes_), cached_node_count_(other.cached_node_count_),
          node_pool_hits_(other.node_pool_hits_),
          node_pool_allocations_(other.node_pool_allocations_)
    {
        other.buckets = nullptr;
        other.bucket_count_ = 0;
        other.size_ = 0;
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.free_nodes_ = nullptr;
        other.cached_node_count_ = 0;
        other.node_pool_hits_ = 0;
        other.node_pool_allocations_ = 0;
    }

    SharedMemoryHashTable& operator=(const SharedMemoryHashTable& other)
    {
        if (this == &other)
        {
            return *this;
        }

        SharedMemoryHashTable tmp(other);
        swap(tmp);
        return *this;
    }

    SharedMemoryHashTable& operator=(SharedMemoryHashTable&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        clear();
        release_cached_nodes();
        release_buckets();

        allocator = other.allocator;
        node_allocator = node_allocator_type(other.allocator.get_manager());
        bucket_allocator = bucket_allocator_type(other.allocator.get_manager());
        hash_function_ = std::move(other.hash_function_);
        key_equal_ = std::move(other.key_equal_);
        buckets = other.buckets;
        bucket_count_ = other.bucket_count_;
        size_ = other.size_;
        max_load_factor_ = other.max_load_factor_;
        head_ = other.head_;
        tail_ = other.tail_;
        free_nodes_ = other.free_nodes_;
        cached_node_count_ = other.cached_node_count_;
        node_pool_hits_ = other.node_pool_hits_;
        node_pool_allocations_ = other.node_pool_allocations_;

        other.buckets = nullptr;
        other.bucket_count_ = 0;
        other.size_ = 0;
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.free_nodes_ = nullptr;
        other.cached_node_count_ = 0;
        other.node_pool_hits_ = 0;
        other.node_pool_allocations_ = 0;
        return *this;
    }

    ~SharedMemoryHashTable()
    {
        clear();
        release_cached_nodes();
        release_buckets();
    }

    allocator_type get_allocator() const noexcept
    {
        return allocator;
    }

    hasher hash_function() const
    {
        return hash_function_;
    }

    key_equal key_eq() const
    {
        return key_equal_;
    }

    iterator begin() noexcept
    {
        return iterator(this, head_.get());
    }

    const_iterator begin() const noexcept
    {
        return const_iterator(this, head_.get());
    }

    const_iterator cbegin() const noexcept
    {
        return begin();
    }

    iterator end() noexcept
    {
        return iterator(this, nullptr);
    }

    const_iterator end() const noexcept
    {
        return const_iterator(this, nullptr);
    }

    const_iterator cend() const noexcept
    {
        return end();
    }

    local_iterator begin(size_type bucket_index) noexcept
    {
        return local_iterator(this, bucket_at(bucket_index));
    }

    const_local_iterator begin(size_type bucket_index) const noexcept
    {
        return const_local_iterator(this, bucket_at(bucket_index));
    }

    const_local_iterator cbegin(size_type bucket_index) const noexcept
    {
        return begin(bucket_index);
    }

    local_iterator end(size_type) noexcept
    {
        return local_iterator(this, nullptr);
    }

    const_local_iterator end(size_type) const noexcept
    {
        return const_local_iterator(this, nullptr);
    }

    const_local_iterator cend(size_type bucket_index) const noexcept
    {
        return end(bucket_index);
    }

    bool empty() const noexcept
    {
        return size_ == 0;
    }

    size_type size() const noexcept
    {
        return size_;
    }

    size_type max_size() const noexcept
    {
        return allocator.max_size();
    }

    size_type bucket_count() const noexcept
    {
        return bucket_count_;
    }

    size_type max_bucket_count() const noexcept
    {
        return std::numeric_limits<size_type>::max() / sizeof(OffsetPtr<Node>);
    }

    size_type bucket_size(size_type bucket_index) const
    {
        validate_bucket_index(bucket_index);
        size_type count = 0;
        for (Node* node = bucket_at(bucket_index); node != nullptr; node = node->bucket_next.get())
        {
            ++count;
        }
        return count;
    }

    size_type bucket(const key_type& key) const
    {
        return bucket_index_for(key, bucket_count_);
    }

    float load_factor() const noexcept
    {
        return bucket_count_ == 0 ? 0.0f : static_cast<float>(size_) /
                                                static_cast<float>(bucket_count_);
    }

    float max_load_factor() const noexcept
    {
        return max_load_factor_;
    }

    void max_load_factor(float value)
    {
        if (value <= 0.0f)
        {
            throw std::invalid_argument("SharedMemoryHashTable::max_load_factor must be positive");
        }
        max_load_factor_ = value;
        if (load_factor() > max_load_factor_)
        {
            rehash(minimum_bucket_count_for_size(size_));
        }
    }

    mapped_type& at(const key_type& key)
    {
        iterator it = find(key);
        if (it == end())
        {
            throw std::out_of_range("SharedMemoryHashTable::at key not found");
        }
        return it->second;
    }

    const mapped_type& at(const key_type& key) const
    {
        const_iterator it = find(key);
        if (it == end())
        {
            throw std::out_of_range("SharedMemoryHashTable::at key not found");
        }
        return it->second;
    }

    mapped_type& operator[](const key_type& key)
    {
        return try_emplace(key).first->second;
    }

    mapped_type& operator[](key_type&& key)
    {
        return try_emplace(std::move(key)).first->second;
    }

    std::pair<iterator, bool> insert(const value_type& value)
    {
        return emplace(value);
    }

    std::pair<iterator, bool> insert(value_type&& value)
    {
        return emplace(std::move(value));
    }

    template <typename InputIt,
              typename = std::enable_if_t<!std::is_integral<InputIt>::value, int>>
    void insert(InputIt first, InputIt last)
    {
        for (; first != last; ++first)
        {
            insert(*first);
        }
    }

    void insert(std::initializer_list<value_type> init)
    {
        insert(init.begin(), init.end());
    }

    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        value_type value(std::forward<Args>(args)...);
        return insert_value(std::move(value));
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(const key_type& key, Args&&... args)
    {
        return try_emplace_impl(key, key, std::forward<Args>(args)...);
    }

    template <typename... Args>
    std::pair<iterator, bool> try_emplace(key_type&& key, Args&&... args)
    {
        return try_emplace_impl(key, std::move(key), std::forward<Args>(args)...);
    }

    template <typename M>
    std::pair<iterator, bool> insert_or_assign(const key_type& key, M&& obj)
    {
        iterator it = find(key);
        if (it != end())
        {
            it->second = std::forward<M>(obj);
            return std::make_pair(it, false);
        }
        return try_emplace(key, std::forward<M>(obj));
    }

    template <typename M>
    std::pair<iterator, bool> insert_or_assign(key_type&& key, M&& obj)
    {
        iterator it = find(key);
        if (it != end())
        {
            it->second = std::forward<M>(obj);
            return std::make_pair(it, false);
        }
        return try_emplace(std::move(key), std::forward<M>(obj));
    }

    iterator find(const key_type& key)
    {
        Node* node = find_node(key);
        return iterator(this, node);
    }

    const_iterator find(const key_type& key) const
    {
        Node* node = find_node(key);
        return const_iterator(this, node);
    }

    bool contains(const key_type& key) const
    {
        return find_node(key) != nullptr;
    }

    size_type count(const key_type& key) const
    {
        return contains(key) ? 1 : 0;
    }

    std::pair<iterator, iterator> equal_range(const key_type& key)
    {
        iterator it = find(key);
        if (it == end())
        {
            return std::make_pair(it, it);
        }
        iterator next = it;
        ++next;
        return std::make_pair(it, next);
    }

    std::pair<const_iterator, const_iterator> equal_range(const key_type& key) const
    {
        const_iterator it = find(key);
        if (it == end())
        {
            return std::make_pair(it, it);
        }
        const_iterator next = it;
        ++next;
        return std::make_pair(it, next);
    }

    size_type erase(const key_type& key)
    {
        Node* node = find_node(key);
        if (!node)
        {
            return 0;
        }
        erase_node(node);
        return 1;
    }

    iterator erase(iterator pos)
    {
        if (pos == end())
        {
            return pos;
        }
        Node* next = pos.node->iter_next.get();
        erase_node(pos.node);
        return iterator(this, next);
    }

    iterator erase(iterator first, iterator last)
    {
        while (first != last)
        {
            first = erase(first);
        }
        return last;
    }

    void clear() noexcept
    {
        Node* current = head_.get();
        while (current != nullptr)
        {
            Node* next = current->iter_next.get();
            destroy_node(current);
            current = next;
        }
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
        clear_buckets();
    }

    void reserve(size_type count)
    {
        const size_type needed = minimum_bucket_count_for_size(count);
        if (needed > bucket_count_)
        {
            rehash(needed);
        }
    }

    void rehash(size_type new_bucket_count)
    {
        new_bucket_count = normalize_bucket_count(new_bucket_count);
        const size_type minimum_needed = minimum_bucket_count_for_size(size_);
        if (new_bucket_count < minimum_needed)
        {
            new_bucket_count = minimum_needed;
        }
        if (new_bucket_count == bucket_count_)
        {
            return;
        }

        OffsetPtr<Node>* new_buckets = bucket_allocator.allocate(new_bucket_count);
        initialize_bucket_array(new_buckets, new_bucket_count);

        try
        {
            relink_all_buckets(new_buckets, new_bucket_count);
        }
        catch (...)
        {
            bucket_allocator.deallocate(new_buckets, new_bucket_count);
            throw;
        }

        release_buckets();
        buckets = new_buckets;
        bucket_count_ = new_bucket_count;
    }

    void swap(SharedMemoryHashTable& other)
    {
        using std::swap;
        swap(allocator, other.allocator);
        swap(node_allocator, other.node_allocator);
        swap(bucket_allocator, other.bucket_allocator);
        swap(hash_function_, other.hash_function_);
        swap(key_equal_, other.key_equal_);
        swap(buckets, other.buckets);
        swap(bucket_count_, other.bucket_count_);
        swap(size_, other.size_);
        swap(max_load_factor_, other.max_load_factor_);
        swap(head_, other.head_);
        swap(tail_, other.tail_);
        swap(free_nodes_, other.free_nodes_);
        swap(cached_node_count_, other.cached_node_count_);
        swap(node_pool_hits_, other.node_pool_hits_);
        swap(node_pool_allocations_, other.node_pool_allocations_);
    }

    size_type cached_node_count() const noexcept
    {
        return cached_node_count_;
    }

    size_type node_pool_hits() const noexcept
    {
        return node_pool_hits_;
    }

    size_type node_pool_allocations() const noexcept
    {
        return node_pool_allocations_;
    }

private:
    static constexpr size_type default_bucket_count = 8;

    static size_type normalize_bucket_count(size_type requested) noexcept
    {
        return requested == 0 ? default_bucket_count : requested;
    }

    size_type minimum_bucket_count_for_size(size_type element_count) const
    {
        if (element_count == 0)
        {
            return default_bucket_count;
        }

        const double required =
            std::ceil(static_cast<double>(element_count) / static_cast<double>(max_load_factor_));
        return normalize_bucket_count(static_cast<size_type>(required));
    }

    void initialize_buckets(size_type requested_count)
    {
        bucket_count_ = normalize_bucket_count(requested_count);
        buckets = bucket_allocator.allocate(bucket_count_);
        initialize_bucket_array(buckets.get(), bucket_count_);
    }

    static void initialize_bucket_array(OffsetPtr<Node>* bucket_array, size_type count) noexcept
    {
        for (size_type i = 0; i < count; ++i)
        {
            bucket_array[i] = nullptr;
        }
    }

    void clear_buckets() noexcept
    {
        if (buckets)
        {
            initialize_bucket_array(buckets.get(), bucket_count_);
        }
    }

    void release_buckets() noexcept
    {
        if (buckets)
        {
            bucket_allocator.deallocate(buckets.get(), bucket_count_);
            buckets = nullptr;
            bucket_count_ = 0;
        }
    }

    void maybe_rehash_for_insert()
    {
        if (bucket_count_ == 0 ||
            static_cast<float>(size_ + 1) >
                static_cast<float>(bucket_count_) * max_load_factor_)
        {
            rehash(bucket_count_ == 0 ? default_bucket_count : bucket_count_ * 2);
        }
    }

    size_type bucket_index_for(const key_type& key, size_type mod) const
    {
        return mod == 0 ? 0 : static_cast<size_type>(hash_function_(key) % mod);
    }

    Node* bucket_at(size_type bucket_index) const noexcept
    {
        return buckets.get()[bucket_index].get();
    }

    void validate_bucket_index(size_type bucket_index) const
    {
        if (bucket_index >= bucket_count_)
        {
            throw std::out_of_range("SharedMemoryHashTable bucket index out of range");
        }
    }

    Node* find_node(const key_type& key) const
    {
        if (bucket_count_ == 0)
        {
            return nullptr;
        }
        const size_type index = bucket_index_for(key, bucket_count_);
        for (Node* node = bucket_at(index); node != nullptr; node = node->bucket_next.get())
        {
            if (key_equal_(node->value.first, key))
            {
                return node;
            }
        }
        return nullptr;
    }

    template <typename ValueArg>
    std::pair<iterator, bool> insert_value(ValueArg&& value)
    {
        Node* existing = find_node(value.first);
        if (existing)
        {
            return std::make_pair(iterator(this, existing), false);
        }

        maybe_rehash_for_insert();
        Node* node = create_node(std::forward<ValueArg>(value));
        link_new_node(node);
        ++size_;
        return std::make_pair(iterator(this, node), true);
    }

    template <typename KeyArg, typename... Args>
    std::pair<iterator, bool> try_emplace_impl(const key_type& lookup_key, KeyArg&& key,
                                               Args&&... args)
    {
        Node* existing = find_node(lookup_key);
        if (existing)
        {
            return std::make_pair(iterator(this, existing), false);
        }

        maybe_rehash_for_insert();
        Node* node =
            create_node(std::piecewise_construct, std::forward_as_tuple(std::forward<KeyArg>(key)),
                        std::forward_as_tuple(std::forward<Args>(args)...));
        link_new_node(node);
        ++size_;
        return std::make_pair(iterator(this, node), true);
    }

    template <typename... Args>
    Node* create_node(Args&&... args)
    {
        Node* storage = pop_cached_node();
        const bool from_cache = storage != nullptr;
        if (!storage)
        {
            storage = node_allocator.allocate(1);
            ++node_pool_allocations_;
        }

        try
        {
            node_allocator.construct(storage, std::forward<Args>(args)...);
        }
        catch (...)
        {
            if (from_cache)
            {
                push_cached_node(storage);
            }
            else
            {
                node_allocator.deallocate(storage, 1);
                --node_pool_allocations_;
            }
            throw;
        }
        return storage;
    }

    void destroy_node(Node* node) noexcept
    {
        node_allocator.destroy(node);
        push_cached_node(node);
    }

    Node* pop_cached_node() noexcept
    {
        Node* node = free_nodes_.get();
        if (node == nullptr)
        {
            return nullptr;
        }

        FreeNode* free_node = reinterpret_cast<FreeNode*>(node);
        free_nodes_ = reinterpret_cast<Node*>(free_node->next.get());
        --cached_node_count_;
        ++node_pool_hits_;
        return node;
    }

    void push_cached_node(Node* node) noexcept
    {
        FreeNode* free_node = reinterpret_cast<FreeNode*>(node);
        free_node->next = reinterpret_cast<FreeNode*>(free_nodes_.get());
        free_nodes_ = node;
        ++cached_node_count_;
    }

    void release_cached_nodes() noexcept
    {
        Node* node = free_nodes_.get();
        while (node != nullptr)
        {
            FreeNode* free_node = reinterpret_cast<FreeNode*>(node);
            Node* next = reinterpret_cast<Node*>(free_node->next.get());
            node_allocator.deallocate(node, 1);
            if (node_pool_allocations_ > 0)
            {
                --node_pool_allocations_;
            }
            node = next;
        }
        free_nodes_ = nullptr;
        cached_node_count_ = 0;
    }

    void link_new_node(Node* node)
    {
        const size_type index = bucket_index_for(node->value.first, bucket_count_);

        node->bucket_prev = nullptr;
        node->bucket_next = buckets.get()[index];
        if (node->bucket_next)
        {
            node->bucket_next->bucket_prev = node;
        }
        buckets.get()[index] = node;

        node->iter_prev = tail_;
        node->iter_next = nullptr;
        if (tail_)
        {
            tail_->iter_next = node;
        }
        else
        {
            head_ = node;
        }
        tail_ = node;
    }

    void unlink_bucket(Node* node) noexcept
    {
        const size_type index = bucket_index_for(node->value.first, bucket_count_);
        if (node->bucket_prev)
        {
            node->bucket_prev->bucket_next = node->bucket_next;
        }
        else
        {
            buckets.get()[index] = node->bucket_next;
        }
        if (node->bucket_next)
        {
            node->bucket_next->bucket_prev = node->bucket_prev;
        }
    }

    void unlink_iteration(Node* node) noexcept
    {
        if (node->iter_prev)
        {
            node->iter_prev->iter_next = node->iter_next;
        }
        else
        {
            head_ = node->iter_next;
        }
        if (node->iter_next)
        {
            node->iter_next->iter_prev = node->iter_prev;
        }
        else
        {
            tail_ = node->iter_prev;
        }
    }

    void erase_node(Node* node) noexcept
    {
        unlink_bucket(node);
        unlink_iteration(node);
        node->bucket_prev = nullptr;
        node->bucket_next = nullptr;
        node->iter_prev = nullptr;
        node->iter_next = nullptr;
        destroy_node(node);
        --size_;
    }

    void relink_all_buckets(OffsetPtr<Node>* new_buckets, size_type new_bucket_count)
    {
        for (Node* node = head_.get(); node != nullptr; node = node->iter_next.get())
        {
            node->bucket_prev = nullptr;
            node->bucket_next = nullptr;
        }

        for (Node* node = head_.get(); node != nullptr; node = node->iter_next.get())
        {
            const size_type index = bucket_index_for(node->value.first, new_bucket_count);
            node->bucket_prev = nullptr;
            node->bucket_next = new_buckets[index];
            if (node->bucket_next)
            {
                node->bucket_next->bucket_prev = node;
            }
            new_buckets[index] = node;
        }
    }

    allocator_type allocator;
    node_allocator_type node_allocator;
    bucket_allocator_type bucket_allocator;
    hasher hash_function_;
    key_equal key_equal_;
    OffsetPtr<OffsetPtr<Node>> buckets;
    size_type bucket_count_;
    size_type size_;
    float max_load_factor_;
    OffsetPtr<Node> head_;
    OffsetPtr<Node> tail_;
    OffsetPtr<Node> free_nodes_;
    size_type cached_node_count_;
    size_type node_pool_hits_;
    size_type node_pool_allocations_;
};

} // namespace interprocess::detail
