#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstddef>
#include <errno.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace interprocess
{

struct create_only_t
{
};
struct open_only_t
{
};
struct open_or_create_t
{
};

constexpr create_only_t create_only{};
constexpr open_only_t open_only{};
constexpr open_or_create_t open_or_create{};

enum class mode_t
{
    read_only = O_RDONLY,
    read_write = O_RDWR
};

class SharedMemoryObject
{
public:
    SharedMemoryObject(const SharedMemoryObject&) = delete;
    SharedMemoryObject& operator=(const SharedMemoryObject&) = delete;

    SharedMemoryObject() noexcept : fd(-1), mode(mode_t::read_only), created(false)
    {
    }

    SharedMemoryObject(SharedMemoryObject&& other) noexcept
        : fd(other.fd), name(std::move(other.name)), mode(other.mode), created(other.created)
    {
        other.fd = -1;
        other.mode = mode_t::read_only;
        other.created = false;
    }

    SharedMemoryObject& operator=(SharedMemoryObject&& other) noexcept
    {
        if (this != &other)
        {
            priv_close();
            fd = other.fd;
            name = std::move(other.name);
            mode = other.mode;
            created = other.created;
            other.fd = -1;
            other.mode = mode_t::read_only;
            other.created = false;
        }
        return *this;
    }

    SharedMemoryObject(create_only_t, const char* shared_memory_name, mode_t open_mode,
                       ::mode_t permissions)
        : fd(-1), mode(mode_t::read_only), created(false)
    {
        priv_open_or_create(O_CREAT | O_EXCL, shared_memory_name, open_mode, permissions, true);
        if (fd == -1)
        {
            throw std::system_error(errno, std::system_category(),
                                    "Failed to create shared memory object");
        }
    }

    SharedMemoryObject(open_or_create_t, const char* shared_memory_name, mode_t open_mode,
                       ::mode_t permissions)
        : fd(-1), mode(mode_t::read_only), created(false)
    {
        while (true)
        {
            priv_open_or_create(O_CREAT | O_EXCL, shared_memory_name, open_mode, permissions, true);
            if (fd != -1)
            {
                break;
            }
            int last_errno = errno;
            if (last_errno == EEXIST)
            {
                priv_open_or_create(0, shared_memory_name, open_mode, permissions, false);
                if (fd != -1)
                {
                    break;
                }
                last_errno = errno;
                if (errno == ENOENT)
                {
                    continue;
                }
            }
            throw std::system_error(last_errno, std::system_category(),
                                    "Failed to open or create shared memory object");
        }
    }

    SharedMemoryObject(open_only_t, const char* shared_memory_name, mode_t open_mode)
        : fd(-1), mode(mode_t::read_only), created(false)
    {
        priv_open_or_create(0, shared_memory_name, open_mode, 0, false);
        if (fd == -1)
        {
            throw std::system_error(errno, std::system_category(),
                                    "Failed to open shared memory object");
        }
    }

    ~SharedMemoryObject()
    {
        priv_close();
    }

    static bool remove(const char* shared_memory_name) noexcept
    {
        try
        {
            std::string shm_name = make_shared_memory_name(shared_memory_name);
            return shm_unlink_with_retry(shm_name.c_str()) == 0;
        }
        catch (...)
        {
            return false;
        }
    }

    void truncate(std::size_t length)
    {
        if (fd == -1)
        {
            throw std::runtime_error("SharedMemoryObject not open for truncate.");
        }
#if defined(__linux__)
        if (length != 0)
        {
            int fallocate_result = posix_fallocate(fd, 0, static_cast<off_t>(length));
            if (fallocate_result != 0 && fallocate_result != EINVAL && fallocate_result != ENOSYS &&
                fallocate_result != EOPNOTSUPP)
            {
                throw std::system_error(fallocate_result, std::system_category(),
                                        "Failed to allocate shared memory object storage");
            }
        }
#endif

        if (ftruncate_with_retry(fd, static_cast<off_t>(length)) == -1)
        {
            throw std::system_error(errno, std::system_category(),
                                    "Failed to truncate shared memory object");
        }
    }

    const char* get_name() const noexcept
    {
        return name.c_str();
    }

    int get_fd() const noexcept
    {
        return fd;
    }

    bool was_created() const noexcept
    {
        return created;
    }

    std::size_t get_size() const
    {
        if (fd == -1)
        {
            throw std::runtime_error("SharedMemoryObject not open for get_size.");
        }
        struct stat st;
        if (fstat(fd, &st) == -1)
        {
            throw std::system_error(errno, std::system_category(),
                                    "Failed to get shared memory size");
        }
        return static_cast<std::size_t>(st.st_size);
    }

    void swap(SharedMemoryObject& other) noexcept
    {
        std::swap(fd, other.fd);
        std::swap(name, other.name);
        std::swap(mode, other.mode);
        std::swap(created, other.created);
    }

private:
    static std::string make_shared_memory_name(const char* shared_memory_name)
    {
        if (shared_memory_name == nullptr || shared_memory_name[0] == '\0')
        {
            throw std::invalid_argument("Shared memory name must not be empty");
        }

        if (shared_memory_name[0] == '/')
        {
            return shared_memory_name;
        }

        std::string shm_name = "/";
        shm_name += shared_memory_name;
        return shm_name;
    }

    static int shm_open_with_retry(const char* name, int oflag, ::mode_t permissions) noexcept
    {
        int result = -1;
        do
        {
            result = shm_open(name, oflag, permissions);
        } while (result == -1 && errno == EINTR);
        return result;
    }

    static int ftruncate_with_retry(int file_descriptor, off_t length) noexcept
    {
        int result = -1;
        do
        {
            result = ftruncate(file_descriptor, length);
        } while (result == -1 && errno == EINTR);
        return result;
    }

    static int fchmod_with_retry(int file_descriptor, ::mode_t permissions) noexcept
    {
        int result = -1;
        do
        {
            result = fchmod(file_descriptor, permissions);
        } while (result == -1 && errno == EINTR);
        return result;
    }

    static int shm_unlink_with_retry(const char* name) noexcept
    {
        int result = -1;
        do
        {
            result = shm_unlink(name);
        } while (result == -1 && errno == EINTR);
        return result;
    }

    void priv_open_or_create(int flags, const char* shared_memory_name, mode_t open_mode,
                             ::mode_t permissions, bool created_on_success)
    {
        std::string shm_name = make_shared_memory_name(shared_memory_name);
        int oflag = static_cast<int>(open_mode) | flags;
        fd = shm_open_with_retry(shm_name.c_str(), oflag, permissions);
        if (fd != -1)
        {
            if (created_on_success && fchmod_with_retry(fd, permissions) == -1 && errno != EINVAL &&
                errno != ENOTSUP && errno != EOPNOTSUPP && errno != EPERM)
            {
                int last_errno = errno;
                priv_close();
                errno = last_errno;
                return;
            }

            name = shared_memory_name;
            mode = open_mode;
            created = created_on_success;
        }
    }

    void priv_close() noexcept
    {
        if (fd != -1)
        {
            close(fd);
            fd = -1;
        }
    }

private:
    int fd;
    std::string name;
    mode_t mode;
    bool created;
};

} // namespace interprocess
