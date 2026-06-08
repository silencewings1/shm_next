#pragma once

#include "../allocator/offset_ptr.h"
#include "../allocator/shared_memory_allocator.h"
#include "detail/shared_memory_node_pool.h"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace interprocess
{

template <typename T, typename Allocator = SharedMemoryAllocator<T>>
class SharedMemoryList
{
private:
    struct BaseNode
    {
        OffsetPtr<BaseNode> prev;
        OffsetPtr<BaseNode> next;

        BaseNode() noexcept : prev(this), next(this)
        {
        }
    };

    struct Node : BaseNode
    {
        T value;

        template <typename... Args>
        explicit Node(Args&&... args) : BaseNode(), value(std::forward<Args>(args)...)
        {
        }
    };

    template <typename U>
    using rebind_alloc_t = typename Allocator::template rebind<U>::other;

    using node_allocator_type = rebind_alloc_t<Node>;
    using node_pool_type = detail::SharedMemoryNodePool<Node, node_allocator_type>;

    template <typename ValueType, typename PointerType, typename ReferenceType>
    class BasicIterator
    {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = ValueType;
        using difference_type = std::ptrdiff_t;
        using pointer = PointerType;
        using reference = ReferenceType;

        BasicIterator() noexcept : node(nullptr)
        {
        }

        explicit BasicIterator(BaseNode* input_node) noexcept : node(input_node)
        {
        }

        template <typename OtherValueType, typename OtherPointerType, typename OtherReferenceType>
        BasicIterator(
            const BasicIterator<OtherValueType, OtherPointerType, OtherReferenceType>& other,
            typename std::enable_if<std::is_convertible<OtherPointerType, PointerType>::value,
                                    int>::type = 0) noexcept
            : node(other.node)
        {
        }

        reference operator*() const
        {
            return static_cast<Node*>(node)->value;
        }

        pointer operator->() const
        {
            return &static_cast<Node*>(node)->value;
        }

        BasicIterator& operator++()
        {
            node = node->next.get();
            return *this;
        }

        BasicIterator operator++(int)
        {
            BasicIterator tmp(*this);
            ++(*this);
            return tmp;
        }

        BasicIterator& operator--()
        {
            node = node->prev.get();
            return *this;
        }

        BasicIterator operator--(int)
        {
            BasicIterator tmp(*this);
            --(*this);
            return tmp;
        }

        bool operator==(const BasicIterator& other) const noexcept
        {
            return node == other.node;
        }

        bool operator!=(const BasicIterator& other) const noexcept
        {
            return !(*this == other);
        }

    private:
        template <typename, typename>
        friend class SharedMemoryList;
        template <typename, typename, typename>
        friend class BasicIterator;

        BaseNode* node;
    };

public:
    using value_type = T;
    using allocator_type = Allocator;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = BasicIterator<value_type, value_type*, value_type&>;
    using const_iterator = BasicIterator<const value_type, const value_type*, const value_type&>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    explicit SharedMemoryList(const allocator_type& alloc) noexcept
        : allocator(alloc), node_pool(node_allocator_type(alloc.get_manager())), sentinel(), size_(0)
    {
    }

    explicit SharedMemoryList(size_type count, const allocator_type& alloc)
        : allocator(alloc), node_pool(node_allocator_type(alloc.get_manager())), sentinel(), size_(0)
    {
        try
        {
            while (size_ < count)
            {
                emplace_back();
            }
        }
        catch (...)
        {
            clear();
            shrink_to_fit();
            throw;
        }
    }

    SharedMemoryList(size_type count, const value_type& value, const allocator_type& alloc)
        : allocator(alloc), node_pool(node_allocator_type(alloc.get_manager())), sentinel(), size_(0)
    {
        try
        {
            insert(cbegin(), count, value);
        }
        catch (...)
        {
            clear();
            shrink_to_fit();
            throw;
        }
    }

    template <typename InputIt,
              typename = std::enable_if_t<!std::is_integral<InputIt>::value, int>>
    SharedMemoryList(InputIt first, InputIt last, const allocator_type& alloc)
        : allocator(alloc), node_pool(node_allocator_type(alloc.get_manager())), sentinel(), size_(0)
    {
        try
        {
            insert(cbegin(), first, last);
        }
        catch (...)
        {
            clear();
            shrink_to_fit();
            throw;
        }
    }

    SharedMemoryList(std::initializer_list<value_type> init, const allocator_type& alloc)
        : allocator(alloc), node_pool(node_allocator_type(alloc.get_manager())), sentinel(), size_(0)
    {
        try
        {
            insert(cbegin(), init.begin(), init.end());
        }
        catch (...)
        {
            clear();
            shrink_to_fit();
            throw;
        }
    }

    SharedMemoryList(const SharedMemoryList& other)
        : allocator(other.allocator), node_pool(node_allocator_type(other.allocator.get_manager())), sentinel(),
          size_(0)
    {
        try
        {
            insert(cbegin(), other.cbegin(), other.cend());
        }
        catch (...)
        {
            clear();
            shrink_to_fit();
            throw;
        }
    }

    SharedMemoryList(SharedMemoryList&& other) noexcept
        : allocator(other.allocator), node_pool(std::move(other.node_pool)), sentinel(),
          size_(0)
    {
        adopt_nodes_from(other);
    }

    ~SharedMemoryList()
    {
        clear();
        shrink_to_fit();
    }

    SharedMemoryList& operator=(const SharedMemoryList& other)
    {
        if (this == &other)
        {
            return *this;
        }

        SharedMemoryList tmp(other);
        swap(tmp);
        return *this;
    }

    SharedMemoryList& operator=(SharedMemoryList&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        clear();
        shrink_to_fit();
        allocator = other.allocator;
        node_pool = std::move(other.node_pool);
        adopt_nodes_from(other);
        return *this;
    }

    allocator_type get_allocator() const noexcept
    {
        return allocator;
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

    reference front()
    {
        return *begin();
    }

    const_reference front() const
    {
        return *begin();
    }

    reference back()
    {
        return *(--end());
    }

    const_reference back() const
    {
        return *(--end());
    }

    iterator begin() noexcept
    {
        return iterator(sentinel.next.get());
    }

    const_iterator begin() const noexcept
    {
        return const_iterator(sentinel.next.get());
    }

    const_iterator cbegin() const noexcept
    {
        return begin();
    }

    iterator end() noexcept
    {
        return iterator(const_cast<BaseNode*>(&sentinel));
    }

    const_iterator end() const noexcept
    {
        return const_iterator(const_cast<BaseNode*>(&sentinel));
    }

    const_iterator cend() const noexcept
    {
        return end();
    }

    reverse_iterator rbegin() noexcept
    {
        return reverse_iterator(end());
    }

    const_reverse_iterator rbegin() const noexcept
    {
        return const_reverse_iterator(end());
    }

    const_reverse_iterator crbegin() const noexcept
    {
        return const_reverse_iterator(cend());
    }

    reverse_iterator rend() noexcept
    {
        return reverse_iterator(begin());
    }

    const_reverse_iterator rend() const noexcept
    {
        return const_reverse_iterator(begin());
    }

    const_reverse_iterator crend() const noexcept
    {
        return const_reverse_iterator(cbegin());
    }

    void clear() noexcept
    {
        BaseNode* current = sentinel.next.get();
        while (current != &sentinel)
        {
            BaseNode* next = current->next.get();
            destroy_node(static_cast<Node*>(current));
            current = next;
        }
        reset_sentinel();
        size_ = 0;
    }

    size_type cached_node_count() const noexcept
    {
        return node_pool.cached_node_count();
    }

    size_type node_pool_hits() const noexcept
    {
        return node_pool.node_pool_hits();
    }

    size_type node_pool_allocations() const noexcept
    {
        return node_pool.node_pool_allocations();
    }

    void shrink_to_fit() noexcept
    {
        node_pool.shrink_to_fit();
    }

    void swap(SharedMemoryList& other) noexcept
    {
        if (this == &other)
        {
            return;
        }

        using std::swap;

        const bool this_empty = empty();
        const bool other_empty = other.empty();
        swap(allocator, other.allocator);
        swap(node_pool, other.node_pool);

        BaseNode* this_first = sentinel.next.get();
        BaseNode* this_last = sentinel.prev.get();
        BaseNode* other_first = other.sentinel.next.get();
        BaseNode* other_last = other.sentinel.prev.get();

        if (other_empty)
        {
            reset_sentinel();
        }
        else
        {
            sentinel.next = other_first;
            sentinel.prev = other_last;
            other_first->prev = &sentinel;
            other_last->next = &sentinel;
        }

        if (this_empty)
        {
            other.reset_sentinel();
        }
        else
        {
            other.sentinel.next = this_first;
            other.sentinel.prev = this_last;
            this_first->prev = &other.sentinel;
            this_last->next = &other.sentinel;
        }

        swap(size_, other.size_);
    }

    iterator insert(const_iterator pos, const T& value)
    {
        return emplace(pos, value);
    }

    iterator insert(const_iterator pos, T&& value)
    {
        return emplace(pos, std::move(value));
    }

    iterator insert(const_iterator pos, size_type count, const T& value)
    {
        if (count == 0)
        {
            return iterator(const_cast<BaseNode*>(pos.node));
        }

        iterator result = emplace(pos, value);
        iterator next = result;
        ++next;
        for (size_type i = 1; i < count; ++i)
        {
            emplace(next, value);
        }
        return result;
    }

    iterator insert(const_iterator pos, std::initializer_list<value_type> init)
    {
        return insert(pos, init.begin(), init.end());
    }

    template <typename InputIt,
              typename = std::enable_if_t<!std::is_integral<InputIt>::value, int>>
    iterator insert(const_iterator pos, InputIt first, InputIt last)
    {
        BaseNode* next_node = const_cast<BaseNode*>(pos.node);
        iterator result(next_node);
        bool has_result = false;
        for (; first != last; ++first)
        {
            iterator current = emplace(const_iterator(next_node), *first);
            if (!has_result)
            {
                result = current;
                has_result = true;
            }
        }
        return has_result ? result : iterator(next_node);
    }

    template <typename... Args>
    iterator emplace(const_iterator pos, Args&&... args)
    {
        Node* node = create_node(std::forward<Args>(args)...);
        BaseNode* next = const_cast<BaseNode*>(pos.node);
        BaseNode* prev = next->prev.get();
        link_between(prev, next, node);
        ++size_;
        return iterator(node);
    }

    iterator erase(const_iterator pos)
    {
        BaseNode* target = const_cast<BaseNode*>(pos.node);
        if (target == &sentinel)
        {
            return end();
        }

        BaseNode* next = target->next.get();
        unlink(target);
        destroy_node(static_cast<Node*>(target));
        --size_;
        return iterator(next);
    }

    iterator erase(const_iterator first, const_iterator last)
    {
        iterator current = iterator(const_cast<BaseNode*>(first.node));
        iterator finish = iterator(const_cast<BaseNode*>(last.node));
        while (current != finish)
        {
            current = erase(current);
        }
        return finish;
    }

    void push_front(const T& value)
    {
        emplace_front(value);
    }

    void push_front(T&& value)
    {
        emplace_front(std::move(value));
    }

    void push_back(const T& value)
    {
        emplace_back(value);
    }

    void push_back(T&& value)
    {
        emplace_back(std::move(value));
    }

    template <typename... Args>
    reference emplace_front(Args&&... args)
    {
        return *emplace(begin(), std::forward<Args>(args)...);
    }

    template <typename... Args>
    reference emplace_back(Args&&... args)
    {
        return *emplace(end(), std::forward<Args>(args)...);
    }

    void pop_front()
    {
        erase(begin());
    }

    void pop_back()
    {
        iterator last = end();
        --last;
        erase(last);
    }

    void resize(size_type count)
    {
        resize_default(count);
    }

    void resize(size_type count, const value_type& value)
    {
        resize_fill(count, value);
    }

    void assign(size_type count, const value_type& value)
    {
        clear();
        insert(cbegin(), count, value);
    }

    template <typename InputIt,
              typename = std::enable_if_t<!std::is_integral<InputIt>::value, int>>
    void assign(InputIt first, InputIt last)
    {
        clear();
        insert(cbegin(), first, last);
    }

    void assign(std::initializer_list<value_type> init)
    {
        clear();
        insert(cbegin(), init.begin(), init.end());
    }

    void remove(const value_type& value)
    {
        remove_if([&value](const value_type& current) { return current == value; });
    }

    template <typename UnaryPredicate>
    void remove_if(UnaryPredicate predicate)
    {
        iterator current = begin();
        while (current != end())
        {
            if (predicate(*current))
            {
                current = erase(current);
            }
            else
            {
                ++current;
            }
        }
    }

    void unique()
    {
        unique(std::equal_to<value_type>());
    }

    template <typename BinaryPredicate>
    void unique(BinaryPredicate predicate)
    {
        if (size_ < 2)
        {
            return;
        }

        iterator current = begin();
        iterator next = current;
        ++next;
        while (next != end())
        {
            if (predicate(*current, *next))
            {
                next = erase(next);
            }
            else
            {
                current = next;
                ++next;
            }
        }
    }

    void reverse() noexcept
    {
        if (size_ < 2)
        {
            return;
        }

        BaseNode* current = &sentinel;
        do
        {
            BaseNode* old_next = current->next.get();
            std::swap(current->next, current->prev);
            current = old_next;
        } while (current != &sentinel);
    }

    void splice(const_iterator pos, SharedMemoryList& other)
    {
        if (this == &other || other.empty())
        {
            return;
        }
        require_compatible_allocator(other, "splice");
        transfer_range_before(const_cast<BaseNode*>(pos.node), other, other.sentinel.next.get(),
                              const_cast<BaseNode*>(&other.sentinel), other.size_);
    }

    void splice(const_iterator pos, SharedMemoryList& other, const_iterator it)
    {
        BaseNode* node = const_cast<BaseNode*>(it.node);
        BaseNode* position = const_cast<BaseNode*>(pos.node);
        if (node == &other.sentinel)
        {
            return;
        }
        if (this == &other && (position == node || position == node->next.get()))
        {
            return;
        }
        if (this != &other)
        {
            require_compatible_allocator(other, "splice");
        }
        transfer_range_before(position, other, node, node->next.get(), 1);
    }

    void splice(const_iterator pos, SharedMemoryList& other, const_iterator first,
                const_iterator last)
    {
        BaseNode* first_node = const_cast<BaseNode*>(first.node);
        BaseNode* last_node = const_cast<BaseNode*>(last.node);
        BaseNode* position = const_cast<BaseNode*>(pos.node);
        if (first_node == last_node)
        {
            return;
        }
        if (this != &other)
        {
            require_compatible_allocator(other, "splice");
        }
        else if (position == first_node || position == last_node ||
                 is_node_in_range(position, first_node, last_node))
        {
            return;
        }

        transfer_range_before(position, other, first_node, last_node,
                              count_range(first_node, last_node));
    }

    void merge(SharedMemoryList& other)
    {
        merge(other, std::less<value_type>());
    }

    template <typename Compare>
    void merge(SharedMemoryList& other, Compare comp)
    {
        if (this == &other || other.empty())
        {
            return;
        }

        require_compatible_allocator(other, "merge");

        std::vector<BaseNode*> nodes;
        nodes.reserve(size_ + other.size_);

        for (BaseNode* current = sentinel.next.get(); current != &sentinel;
             current = current->next.get())
        {
            nodes.push_back(current);
        }
        for (BaseNode* current = other.sentinel.next.get(); current != &other.sentinel;
             current = current->next.get())
        {
            nodes.push_back(current);
        }

        std::stable_sort(nodes.begin(), nodes.end(),
                         [&comp](BaseNode* lhs, BaseNode* rhs) {
                             return comp(static_cast<Node*>(lhs)->value,
                                         static_cast<Node*>(rhs)->value);
                         });

        relink_nodes_in_order(nodes);
        size_ = nodes.size();
        other.reset_sentinel();
        other.size_ = 0;
    }

    void sort()
    {
        sort(std::less<value_type>());
    }

    template <typename Compare>
    void sort(Compare comp)
    {
        if (size_ < 2)
        {
            return;
        }

        std::vector<BaseNode*> nodes;
        nodes.reserve(size_);
        for (BaseNode* current = sentinel.next.get(); current != &sentinel;
             current = current->next.get())
        {
            nodes.push_back(current);
        }

        std::stable_sort(nodes.begin(), nodes.end(),
                         [&comp](BaseNode* lhs, BaseNode* rhs) {
                             return comp(static_cast<Node*>(lhs)->value,
                                         static_cast<Node*>(rhs)->value);
                         });

        relink_nodes_in_order(nodes);
    }

private:
    void reset_sentinel() noexcept
    {
        sentinel.prev = &sentinel;
        sentinel.next = &sentinel;
    }

    template <typename... Args>
    Node* create_node(Args&&... args)
    {
        return node_pool.create(std::forward<Args>(args)...);
    }

    void destroy_node(Node* node) noexcept
    {
        node_pool.destroy(node);
    }

    static void link_between(BaseNode* prev, BaseNode* next, BaseNode* node) noexcept
    {
        node->prev = prev;
        node->next = next;
        prev->next = node;
        next->prev = node;
    }

    static void unlink(BaseNode* node) noexcept
    {
        BaseNode* prev = node->prev.get();
        BaseNode* next = node->next.get();
        prev->next = next;
        next->prev = prev;
        node->prev = node;
        node->next = node;
    }

    void require_compatible_allocator(const SharedMemoryList& other, const char* operation) const
    {
        if (allocator != other.allocator)
        {
            throw std::runtime_error(std::string("SharedMemoryList::") + operation +
                                     " requires compatible allocators");
        }
    }

    static size_type count_range(BaseNode* first, BaseNode* last) noexcept
    {
        size_type count = 0;
        for (BaseNode* current = first; current != last; current = current->next.get())
        {
            ++count;
        }
        return count;
    }

    static bool is_node_in_range(BaseNode* target, BaseNode* first, BaseNode* last) noexcept
    {
        for (BaseNode* current = first; current != last; current = current->next.get())
        {
            if (current == target)
            {
                return true;
            }
        }
        return false;
    }

    void transfer_range_before(BaseNode* position, SharedMemoryList& other, BaseNode* first,
                               BaseNode* last_exclusive, size_type moved_count) noexcept
    {
        BaseNode* before_first = first->prev.get();
        BaseNode* last = last_exclusive->prev.get();

        before_first->next = last_exclusive;
        last_exclusive->prev = before_first;

        BaseNode* before_position = position->prev.get();
        before_position->next = first;
        first->prev = before_position;
        last->next = position;
        position->prev = last;

        if (this != &other)
        {
            size_ += moved_count;
            other.size_ -= moved_count;
        }
    }

    void relink_nodes_in_order(const std::vector<BaseNode*>& nodes) noexcept
    {
        if (nodes.empty())
        {
            reset_sentinel();
            return;
        }

        sentinel.next = nodes.front();
        sentinel.prev = nodes.back();
        nodes.front()->prev = &sentinel;
        nodes.back()->next = &sentinel;

        for (size_type i = 1; i < nodes.size(); ++i)
        {
            nodes[i - 1]->next = nodes[i];
            nodes[i]->prev = nodes[i - 1];
        }
    }

    void adopt_nodes_from(SharedMemoryList& other) noexcept
    {
        reset_sentinel();
        size_ = other.size_;
        if (other.empty())
        {
            return;
        }

        BaseNode* first = other.sentinel.next.get();
        BaseNode* last = other.sentinel.prev.get();
        sentinel.next = first;
        sentinel.prev = last;
        first->prev = &sentinel;
        last->next = &sentinel;
        other.reset_sentinel();
        other.size_ = 0;
    }

    void resize_default(size_type count)
    {
        while (size_ > count)
        {
            pop_back();
        }
        while (size_ < count)
        {
            emplace_back();
        }
    }

    void resize_fill(size_type count, const value_type& value)
    {
        while (size_ > count)
        {
            pop_back();
        }
        while (size_ < count)
        {
            emplace_back(value);
        }
    }

    allocator_type allocator;
    node_pool_type node_pool;
    BaseNode sentinel;
    size_type size_;
};

template <typename T, typename Allocator>
void swap(SharedMemoryList<T, Allocator>& lhs, SharedMemoryList<T, Allocator>& rhs) noexcept
{
    lhs.swap(rhs);
}

} // namespace interprocess
