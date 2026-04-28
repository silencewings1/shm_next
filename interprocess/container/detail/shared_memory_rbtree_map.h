#pragma once

#include "../../allocator/offset_ptr.h"
#include "../../allocator/shared_memory_allocator.h"
#include <cstddef>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace interprocess::detail
{

template <typename Key, typename T, typename Compare = std::less<Key>,
          typename Allocator = SharedMemoryAllocator<std::pair<const Key, T>>>
class SharedMemoryRbTreeMap
{
private:
    using self_type = SharedMemoryRbTreeMap<Key, T, Compare, Allocator>;

    struct Node
    {
        OffsetPtr<Node> parent;
        OffsetPtr<Node> left;
        OffsetPtr<Node> right;
        bool is_red;
        std::pair<const Key, T> value;

        template <typename... Args>
        explicit Node(Args&&... args)
            : parent(nullptr), left(nullptr), right(nullptr), is_red(true),
              value(std::forward<Args>(args)...)
        {
        }
    };

    template <typename U>
    using rebind_alloc_t = typename Allocator::template rebind<U>::other;

    using node_allocator_type = rebind_alloc_t<Node>;

    template <typename ValueType, typename PointerType, typename ReferenceType>
    class BasicIterator
    {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
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
            node = owner->successor(node);
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
            if (node == nullptr)
            {
                node = owner->maximum(owner->root.get());
            }
            else
            {
                node = owner->predecessor(node);
            }
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
            return owner == other.owner && node == other.node;
        }

        bool operator!=(const BasicIterator& other) const noexcept
        {
            return !(*this == other);
        }

    private:
        friend class SharedMemoryRbTreeMap<Key, T, Compare, Allocator>;
        template <typename, typename, typename>
        friend class BasicIterator;

        const self_type* owner;
        Node* node;
    };

public:
    using key_type = Key;
    using mapped_type = T;
    using value_type = std::pair<const Key, T>;
    using key_compare = Compare;
    using allocator_type = Allocator;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = value_type&;
    using const_reference = const value_type&;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using iterator = BasicIterator<value_type, value_type*, value_type&>;
    using const_iterator = BasicIterator<const value_type, const value_type*, const value_type&>;

    explicit SharedMemoryRbTreeMap(const allocator_type& alloc) noexcept
        : allocator(alloc), node_allocator(alloc.get_manager()), compare(), root(nullptr), size_(0)
    {
    }

    SharedMemoryRbTreeMap(const key_compare& comp, const allocator_type& alloc) noexcept
        : allocator(alloc), node_allocator(alloc.get_manager()), compare(comp), root(nullptr),
          size_(0)
    {
    }

    SharedMemoryRbTreeMap(const SharedMemoryRbTreeMap& other)
        : allocator(other.allocator), node_allocator(other.allocator.get_manager()),
          compare(other.compare), root(nullptr), size_(0)
    {
        copy_from(other);
    }

    SharedMemoryRbTreeMap(SharedMemoryRbTreeMap&& other) noexcept
        : allocator(other.allocator), node_allocator(other.allocator.get_manager()),
          compare(std::move(other.compare)), root(other.root), size_(other.size_)
    {
        other.root = nullptr;
        other.size_ = 0;
    }

    SharedMemoryRbTreeMap& operator=(const SharedMemoryRbTreeMap& other)
    {
        if (this == &other)
        {
            return *this;
        }

        clear();
        compare = other.compare;
        copy_from(other);
        return *this;
    }

    SharedMemoryRbTreeMap& operator=(SharedMemoryRbTreeMap&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        clear();
        compare = std::move(other.compare);

        if (allocator == other.allocator)
        {
            root = other.root;
            size_ = other.size_;
            other.root = nullptr;
            other.size_ = 0;
            return *this;
        }

        for (iterator it = other.begin(); it != other.end(); ++it)
        {
            emplace(it->first, std::move(it->second));
        }
        other.clear();
        return *this;
    }

    ~SharedMemoryRbTreeMap()
    {
        clear();
    }

    iterator begin() noexcept
    {
        return iterator(this, minimum(root.get()));
    }

    const_iterator begin() const noexcept
    {
        return const_iterator(this, minimum(root.get()));
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

    bool empty() const noexcept
    {
        return size_ == 0;
    }

    size_type size() const noexcept
    {
        return size_;
    }

    allocator_type get_allocator() const noexcept
    {
        return allocator;
    }

    key_compare key_comp() const
    {
        return compare;
    }

    mapped_type& at(const key_type& key)
    {
        iterator it = find(key);
        if (it == end())
        {
            throw std::out_of_range("SharedMemoryRbTreeMap::at key not found");
        }
        return it->second;
    }

    const mapped_type& at(const key_type& key) const
    {
        const_iterator it = find(key);
        if (it == end())
        {
            throw std::out_of_range("SharedMemoryRbTreeMap::at key not found");
        }
        return it->second;
    }

    mapped_type& operator[](const key_type& key)
    {
        return emplace(key, mapped_type()).first->second;
    }

    mapped_type& operator[](key_type&& key)
    {
        return emplace(std::move(key), mapped_type()).first->second;
    }

    std::pair<iterator, bool> insert(const value_type& value)
    {
        return insert_value(value);
    }

    std::pair<iterator, bool> insert(value_type&& value)
    {
        return insert_value(std::move(value));
    }

    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        value_type value(std::forward<Args>(args)...);
        return insert_value(std::move(value));
    }

    void clear()
    {
        while (root)
        {
            erase_node(root.get());
        }
    }

    iterator find(const key_type& key)
    {
        return iterator(this, find_node(key));
    }

    const_iterator find(const key_type& key) const
    {
        return const_iterator(this, find_node(key));
    }

    size_type count(const key_type& key) const
    {
        return find_node(key) ? 1 : 0;
    }

    bool contains(const key_type& key) const
    {
        return find_node(key) != nullptr;
    }

    iterator lower_bound(const key_type& key)
    {
        return iterator(this, lower_bound_node(key));
    }

    const_iterator lower_bound(const key_type& key) const
    {
        return const_iterator(this, lower_bound_node(key));
    }

    iterator upper_bound(const key_type& key)
    {
        return iterator(this, upper_bound_node(key));
    }

    const_iterator upper_bound(const key_type& key) const
    {
        return const_iterator(this, upper_bound_node(key));
    }

    std::pair<iterator, iterator> equal_range(const key_type& key)
    {
        return std::make_pair(lower_bound(key), upper_bound(key));
    }

    std::pair<const_iterator, const_iterator> equal_range(const key_type& key) const
    {
        return std::make_pair(lower_bound(key), upper_bound(key));
    }

    size_type erase(const key_type& key)
    {
        Node* target = find_node(key);
        if (!target)
        {
            return 0;
        }
        erase_node(target);
        return 1;
    }

    iterator erase(iterator pos)
    {
        if (pos == end())
        {
            return pos;
        }

        Node* next = successor(pos.node);
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

private:
    static bool is_red(Node* node)
    {
        return node != nullptr && node->is_red;
    }

    static bool is_black(Node* node)
    {
        return !is_red(node);
    }

    bool keys_equal(const key_type& lhs, const key_type& rhs) const
    {
        return !compare(lhs, rhs) && !compare(rhs, lhs);
    }

    Node* minimum(Node* node) const
    {
        if (!node)
        {
            return nullptr;
        }
        while (node->left)
        {
            node = node->left.get();
        }
        return node;
    }

    Node* maximum(Node* node) const
    {
        if (!node)
        {
            return nullptr;
        }
        while (node->right)
        {
            node = node->right.get();
        }
        return node;
    }

    Node* successor(Node* node) const
    {
        if (!node)
        {
            return nullptr;
        }
        if (node->right)
        {
            return minimum(node->right.get());
        }

        Node* parent = node->parent.get();
        while (parent && node == parent->right.get())
        {
            node = parent;
            parent = parent->parent.get();
        }
        return parent;
    }

    Node* predecessor(Node* node) const
    {
        if (!node)
        {
            return nullptr;
        }
        if (node->left)
        {
            return maximum(node->left.get());
        }

        Node* parent = node->parent.get();
        while (parent && node == parent->left.get())
        {
            node = parent;
            parent = parent->parent.get();
        }
        return parent;
    }

    Node* find_node(const key_type& key) const
    {
        Node* current = root.get();
        while (current)
        {
            if (compare(key, current->value.first))
            {
                current = current->left.get();
            }
            else if (compare(current->value.first, key))
            {
                current = current->right.get();
            }
            else
            {
                return current;
            }
        }
        return nullptr;
    }

    Node* lower_bound_node(const key_type& key) const
    {
        Node* result = nullptr;
        Node* current = root.get();
        while (current)
        {
            if (!compare(current->value.first, key))
            {
                result = current;
                current = current->left.get();
            }
            else
            {
                current = current->right.get();
            }
        }
        return result;
    }

    Node* upper_bound_node(const key_type& key) const
    {
        Node* result = nullptr;
        Node* current = root.get();
        while (current)
        {
            if (compare(key, current->value.first))
            {
                result = current;
                current = current->left.get();
            }
            else
            {
                current = current->right.get();
            }
        }
        return result;
    }

    template <typename ValueArg>
    std::pair<iterator, bool> insert_value(ValueArg&& value)
    {
        Node* parent = nullptr;
        Node* current = root.get();

        while (current)
        {
            parent = current;
            if (compare(value.first, current->value.first))
            {
                current = current->left.get();
            }
            else if (compare(current->value.first, value.first))
            {
                current = current->right.get();
            }
            else
            {
                return std::make_pair(iterator(this, current), false);
            }
        }

        Node* node = create_node(std::forward<ValueArg>(value));
        node->parent = parent;

        if (!parent)
        {
            root = node;
        }
        else if (compare(node->value.first, parent->value.first))
        {
            parent->left = node;
        }
        else
        {
            parent->right = node;
        }

        insert_fixup(node);
        ++size_;
        return std::make_pair(iterator(this, node), true);
    }

    template <typename ValueArg>
    Node* create_node(ValueArg&& value)
    {
        Node* storage = node_allocator.allocate(1);
        try
        {
            node_allocator.construct(storage, std::forward<ValueArg>(value));
        }
        catch (...)
        {
            node_allocator.deallocate(storage, 1);
            throw;
        }
        return storage;
    }

    void destroy_node(Node* node)
    {
        if (!node)
        {
            return;
        }
        node_allocator.destroy(node);
        node_allocator.deallocate(node, 1);
    }

    void rotate_left(Node* node)
    {
        Node* child = node->right.get();
        node->right = child->left;
        if (child->left)
        {
            child->left->parent = node;
        }

        child->parent = node->parent;
        if (!node->parent)
        {
            root = child;
        }
        else if (node == node->parent->left.get())
        {
            node->parent->left = child;
        }
        else
        {
            node->parent->right = child;
        }

        child->left = node;
        node->parent = child;
    }

    void rotate_right(Node* node)
    {
        Node* child = node->left.get();
        node->left = child->right;
        if (child->right)
        {
            child->right->parent = node;
        }

        child->parent = node->parent;
        if (!node->parent)
        {
            root = child;
        }
        else if (node == node->parent->right.get())
        {
            node->parent->right = child;
        }
        else
        {
            node->parent->left = child;
        }

        child->right = node;
        node->parent = child;
    }

    void insert_fixup(Node* node)
    {
        while (node != root.get() && is_red(node->parent.get()))
        {
            Node* parent = node->parent.get();
            Node* grandparent = parent->parent.get();

            if (parent == grandparent->left.get())
            {
                Node* uncle = grandparent->right.get();
                if (is_red(uncle))
                {
                    parent->is_red = false;
                    uncle->is_red = false;
                    grandparent->is_red = true;
                    node = grandparent;
                }
                else
                {
                    if (node == parent->right.get())
                    {
                        node = parent;
                        rotate_left(node);
                        parent = node->parent.get();
                        grandparent = parent->parent.get();
                    }

                    parent->is_red = false;
                    grandparent->is_red = true;
                    rotate_right(grandparent);
                }
            }
            else
            {
                Node* uncle = grandparent->left.get();
                if (is_red(uncle))
                {
                    parent->is_red = false;
                    uncle->is_red = false;
                    grandparent->is_red = true;
                    node = grandparent;
                }
                else
                {
                    if (node == parent->left.get())
                    {
                        node = parent;
                        rotate_right(node);
                        parent = node->parent.get();
                        grandparent = parent->parent.get();
                    }

                    parent->is_red = false;
                    grandparent->is_red = true;
                    rotate_left(grandparent);
                }
            }
        }

        if (root)
        {
            root->is_red = false;
        }
    }

    void transplant(Node* target, Node* replacement)
    {
        if (!target->parent)
        {
            root = replacement;
        }
        else if (target == target->parent->left.get())
        {
            target->parent->left = replacement;
        }
        else
        {
            target->parent->right = replacement;
        }

        if (replacement)
        {
            replacement->parent = target->parent;
        }
    }

    void erase_node(Node* target)
    {
        Node* replacement_parent = nullptr;
        Node* replacement = nullptr;
        Node* removed = target;
        bool removed_was_red = removed->is_red;

        if (!target->left)
        {
            replacement = target->right.get();
            replacement_parent = target->parent.get();
            transplant(target, replacement);
        }
        else if (!target->right)
        {
            replacement = target->left.get();
            replacement_parent = target->parent.get();
            transplant(target, replacement);
        }
        else
        {
            removed = minimum(target->right.get());
            removed_was_red = removed->is_red;
            replacement = removed->right.get();

            if (removed->parent.get() == target)
            {
                replacement_parent = removed;
                if (replacement)
                {
                    replacement->parent = removed;
                }
            }
            else
            {
                replacement_parent = removed->parent.get();
                transplant(removed, replacement);
                removed->right = target->right;
                removed->right->parent = removed;
            }

            transplant(target, removed);
            removed->left = target->left;
            removed->left->parent = removed;
            removed->is_red = target->is_red;
        }

        destroy_node(target);
        --size_;

        if (!removed_was_red)
        {
            erase_fixup(replacement, replacement_parent);
        }
    }

    void erase_fixup(Node* node, Node* parent)
    {
        while (node != root.get() && is_black(node))
        {
            if (node == (parent ? parent->left.get() : nullptr))
            {
                Node* sibling = parent ? parent->right.get() : nullptr;
                if (is_red(sibling))
                {
                    sibling->is_red = false;
                    parent->is_red = true;
                    rotate_left(parent);
                    sibling = parent->right.get();
                }

                if (is_black(sibling ? sibling->left.get() : nullptr) &&
                    is_black(sibling ? sibling->right.get() : nullptr))
                {
                    if (sibling)
                    {
                        sibling->is_red = true;
                    }
                    node = parent;
                    parent = node ? node->parent.get() : nullptr;
                }
                else
                {
                    if (is_black(sibling ? sibling->right.get() : nullptr))
                    {
                        if (sibling && sibling->left)
                        {
                            sibling->left->is_red = false;
                        }
                        if (sibling)
                        {
                            sibling->is_red = true;
                            rotate_right(sibling);
                        }
                        sibling = parent ? parent->right.get() : nullptr;
                    }

                    if (sibling)
                    {
                        sibling->is_red = parent ? parent->is_red : false;
                    }
                    if (parent)
                    {
                        parent->is_red = false;
                    }
                    if (sibling && sibling->right)
                    {
                        sibling->right->is_red = false;
                    }
                    if (parent)
                    {
                        rotate_left(parent);
                    }
                    node = root.get();
                    parent = nullptr;
                }
            }
            else
            {
                Node* sibling = parent ? parent->left.get() : nullptr;
                if (is_red(sibling))
                {
                    sibling->is_red = false;
                    parent->is_red = true;
                    rotate_right(parent);
                    sibling = parent->left.get();
                }

                if (is_black(sibling ? sibling->left.get() : nullptr) &&
                    is_black(sibling ? sibling->right.get() : nullptr))
                {
                    if (sibling)
                    {
                        sibling->is_red = true;
                    }
                    node = parent;
                    parent = node ? node->parent.get() : nullptr;
                }
                else
                {
                    if (is_black(sibling ? sibling->left.get() : nullptr))
                    {
                        if (sibling && sibling->right)
                        {
                            sibling->right->is_red = false;
                        }
                        if (sibling)
                        {
                            sibling->is_red = true;
                            rotate_left(sibling);
                        }
                        sibling = parent ? parent->left.get() : nullptr;
                    }

                    if (sibling)
                    {
                        sibling->is_red = parent ? parent->is_red : false;
                    }
                    if (parent)
                    {
                        parent->is_red = false;
                    }
                    if (sibling && sibling->left)
                    {
                        sibling->left->is_red = false;
                    }
                    if (parent)
                    {
                        rotate_right(parent);
                    }
                    node = root.get();
                    parent = nullptr;
                }
            }
        }

        if (node)
        {
            node->is_red = false;
        }
    }

    void copy_from(const SharedMemoryRbTreeMap& other)
    {
        for (const_iterator it = other.begin(); it != other.end(); ++it)
        {
            insert(*it);
        }
    }

    allocator_type allocator;
    node_allocator_type node_allocator;
    key_compare compare;
    OffsetPtr<Node> root;
    size_type size_;
};

} // namespace interprocess::detail
