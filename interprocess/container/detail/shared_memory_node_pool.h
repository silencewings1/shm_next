#pragma once

#include "../../allocator/offset_ptr.h"
#include <cstddef>
#include <type_traits>
#include <utility>

namespace interprocess::detail
{

template <typename Node, typename NodeAllocator>
class SharedMemoryNodePool
{
public:
    using node_type = Node;
    using allocator_type = NodeAllocator;
    using size_type = std::size_t;

    explicit SharedMemoryNodePool(const allocator_type& allocator) noexcept
        : allocator_(allocator), free_nodes_(nullptr), cached_node_count_(0), node_pool_hits_(0),
          node_pool_allocations_(0)
    {
    }

    SharedMemoryNodePool(const SharedMemoryNodePool& other) noexcept
        : allocator_(other.allocator_), free_nodes_(nullptr), cached_node_count_(0),
          node_pool_hits_(0), node_pool_allocations_(0)
    {
    }

    SharedMemoryNodePool(SharedMemoryNodePool&& other) noexcept
        : allocator_(std::move(other.allocator_)), free_nodes_(other.free_nodes_),
          cached_node_count_(other.cached_node_count_), node_pool_hits_(other.node_pool_hits_),
          node_pool_allocations_(other.node_pool_allocations_)
    {
        other.free_nodes_ = nullptr;
        other.cached_node_count_ = 0;
        other.node_pool_hits_ = 0;
        other.node_pool_allocations_ = 0;
    }

    SharedMemoryNodePool& operator=(const SharedMemoryNodePool& other) noexcept
    {
        if (this != &other)
        {
            shrink_to_fit();
            allocator_ = other.allocator_;
            free_nodes_ = nullptr;
            cached_node_count_ = 0;
            node_pool_hits_ = 0;
            node_pool_allocations_ = 0;
        }
        return *this;
    }

    SharedMemoryNodePool& operator=(SharedMemoryNodePool&& other) noexcept
    {
        if (this != &other)
        {
            shrink_to_fit();
            allocator_ = std::move(other.allocator_);
            free_nodes_ = other.free_nodes_;
            cached_node_count_ = other.cached_node_count_;
            node_pool_hits_ = other.node_pool_hits_;
            node_pool_allocations_ = other.node_pool_allocations_;
            other.free_nodes_ = nullptr;
            other.cached_node_count_ = 0;
            other.node_pool_hits_ = 0;
            other.node_pool_allocations_ = 0;
        }
        return *this;
    }

    template <typename... Args>
    Node* create(Args&&... args)
    {
        Node* storage = pop_cached_node();
        const bool from_cache = storage != nullptr;
        if (storage == nullptr)
        {
            storage = allocator_.allocate(1);
            ++node_pool_allocations_;
        }

        try
        {
            allocator_.construct(storage, std::forward<Args>(args)...);
        }
        catch (...)
        {
            if (from_cache)
            {
                push_cached_node(storage);
            }
            else
            {
                allocator_.deallocate(storage, 1);
                if (node_pool_allocations_ > 0)
                {
                    --node_pool_allocations_;
                }
            }
            throw;
        }
        return storage;
    }

    void destroy(Node* node) noexcept
    {
        if (node == nullptr)
        {
            return;
        }
        allocator_.destroy(node);
        push_cached_node(node);
    }

    void shrink_to_fit() noexcept
    {
        Node* node = reverse_free_list();
        free_nodes_ = nullptr;
        while (node != nullptr)
        {
            FreeNode* free_node = reinterpret_cast<FreeNode*>(node);
            Node* next = reinterpret_cast<Node*>(free_node->next.get());
            allocator_.deallocate(node, 1);
            if (node_pool_allocations_ > 0)
            {
                --node_pool_allocations_;
            }
            node = next;
        }
        cached_node_count_ = 0;
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

    allocator_type get_allocator() const noexcept
    {
        return allocator_;
    }

    void swap(SharedMemoryNodePool& other) noexcept
    {
        using std::swap;
        swap(allocator_, other.allocator_);
        swap(free_nodes_, other.free_nodes_);
        swap(cached_node_count_, other.cached_node_count_);
        swap(node_pool_hits_, other.node_pool_hits_);
        swap(node_pool_allocations_, other.node_pool_allocations_);
    }

private:
    struct FreeNode
    {
        OffsetPtr<FreeNode> next;
    };

    static_assert(sizeof(Node) >= sizeof(FreeNode),
                  "SharedMemoryNodePool node storage must fit free-list link");
    static_assert(alignof(Node) >= alignof(FreeNode),
                  "SharedMemoryNodePool node alignment must fit free-list link");

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

    Node* reverse_free_list() noexcept
    {
        Node* current = free_nodes_.get();
        Node* previous = nullptr;
        while (current != nullptr)
        {
            FreeNode* current_free = reinterpret_cast<FreeNode*>(current);
            Node* next = reinterpret_cast<Node*>(current_free->next.get());
            current_free->next = reinterpret_cast<FreeNode*>(previous);
            previous = current;
            current = next;
        }
        return previous;
    }

    allocator_type allocator_;
    OffsetPtr<Node> free_nodes_;
    size_type cached_node_count_;
    size_type node_pool_hits_;
    size_type node_pool_allocations_;
};

template <typename Node, typename NodeAllocator>
void swap(SharedMemoryNodePool<Node, NodeAllocator>& lhs,
          SharedMemoryNodePool<Node, NodeAllocator>& rhs) noexcept
{
    lhs.swap(rhs);
}

} // namespace interprocess::detail
