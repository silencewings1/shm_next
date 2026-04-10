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

enum mode_t
{
    read_only = O_RDONLY,
    read_write = O_RDWR
};

class SharedMemoryObject
{
public:
    SharedMemoryObject(const SharedMemoryObject&) = delete;
    SharedMemoryObject& operator=(const SharedMemoryObject&) = delete;

    SharedMemoryObject() noexcept : fd(-1), mode(read_only)
    {
    }

    SharedMemoryObject(SharedMemoryObject&& other) noexcept
        : fd(other.fd), name(std::move(other.name)), mode(other.mode)
    {
        other.fd = -1;
        other.mode = read_only;
    }

    SharedMemoryObject& operator=(SharedMemoryObject&& other) noexcept
    {
        if (this != &other)
        {
            priv_close();
            fd = other.fd;
            name = std::move(other.name);
            mode = other.mode;
            other.fd = -1;
            other.mode = read_only;
        }
        return *this;
    }

    SharedMemoryObject(create_only_t, const char* shared_memory_name, mode_t open_mode,
                       ::mode_t permissions)
    {
        priv_open_or_create(O_CREAT | O_EXCL, shared_memory_name, open_mode, permissions);
        if (fd == -1)
        {
            throw std::system_error(errno, std::system_category(),
                                    "Failed to create shared memory object");
        }
    }

    SharedMemoryObject(open_or_create_t, const char* shared_memory_name, mode_t open_mode,
                       ::mode_t permissions)
    {
        while (true)
        {
            priv_open_or_create(O_CREAT | O_EXCL, shared_memory_name, open_mode, permissions);
            if (fd != -1)
            {
                break;
            }
            int current_errno = errno;
            if (current_errno == EEXIST)
            {
                priv_open_or_create(0, shared_memory_name, open_mode, permissions);
                if (fd != -1)
                {
                    break;
                }
                if (errno == ENOENT)
                {
                    continue;
                }
            }
            throw std::system_error(current_errno, std::system_category(),
                                    "Failed to open or create shared memory object");
        }
    }

    SharedMemoryObject(open_only_t, const char* shared_memory_name, mode_t open_mode)
    {
        priv_open_or_create(0, shared_memory_name, open_mode, 0);
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
        std::string shm_name = "/";
        shm_name += shared_memory_name;
        return shm_unlink(shm_name.c_str()) == 0;
    }

    void truncate(std::size_t length)
    {
        if (fd == -1)
        {
            throw std::runtime_error("SharedMemoryObject not open for truncate.");
        }
        if (ftruncate(fd, length) == -1)
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
    }

private:
    void priv_open_or_create(int flags, const char* shared_memory_name, mode_t open_mode,
                             ::mode_t permissions)
    {
        std::string shm_name = "/";
        shm_name += shared_memory_name;
        int oflag = open_mode | flags;
        fd = shm_open(shm_name.c_str(), oflag, permissions);
        if (fd != -1)
        {
            name = shared_memory_name;
            mode = open_mode;
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

    int fd;
    std::string name;
    mode_t mode;
};

} // namespace interprocess
