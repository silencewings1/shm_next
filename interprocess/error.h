#pragma once

#include <system_error>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace interprocess
{

enum class InterprocessErrc
{
    success = 0,
    segment_too_small,
    initialization_in_progress,
    initialization_corrupted,
    unknown_initialization_state,
    magic_mismatch,
    unsupported_layout_version,
    layout_header_mismatch,
    read_only_violation,
    invalid_name,
    allocation_size_overflow,
    bad_alloc,
    invalid_pointer,
    double_free,
    named_object_type_mismatch,
    metadata_changed_during_read,
    owner_dead
};

class InterprocessErrorCategory : public std::error_category
{
public:
    const char* name() const noexcept override
    {
        return "shm_next.interprocess";
    }

    std::string message(int condition) const override
    {
        switch (static_cast<InterprocessErrc>(condition))
        {
        case InterprocessErrc::success:
            return "success";
        case InterprocessErrc::segment_too_small:
            return "shared memory segment is too small";
        case InterprocessErrc::initialization_in_progress:
            return "shared memory manager initialization is in progress";
        case InterprocessErrc::initialization_corrupted:
            return "shared memory segment is corrupted";
        case InterprocessErrc::unknown_initialization_state:
            return "unknown shared memory initialization state";
        case InterprocessErrc::magic_mismatch:
            return "shared memory manager magic mismatch";
        case InterprocessErrc::unsupported_layout_version:
            return "unsupported shared memory layout version";
        case InterprocessErrc::layout_header_mismatch:
            return "shared memory layout header mismatch";
        case InterprocessErrc::read_only_violation:
            return "operation is not available on read-only shared memory mappings";
        case InterprocessErrc::invalid_name:
            return "invalid shared memory named object name";
        case InterprocessErrc::allocation_size_overflow:
            return "shared memory allocation size overflow";
        case InterprocessErrc::bad_alloc:
            return "shared memory allocation failed";
        case InterprocessErrc::invalid_pointer:
            return "invalid shared memory pointer";
        case InterprocessErrc::double_free:
            return "double free detected";
        case InterprocessErrc::named_object_type_mismatch:
            return "named object type mismatch";
        case InterprocessErrc::metadata_changed_during_read:
            return "shared memory metadata changed during read-only access";
        case InterprocessErrc::owner_dead:
            return "interprocess synchronization owner died";
        }
        return "unknown shared memory interprocess error";
    }
};

inline const std::error_category& interprocess_error_category() noexcept
{
    static const InterprocessErrorCategory category;
    return category;
}

inline std::error_code make_error_code(InterprocessErrc errc) noexcept
{
    return {static_cast<int>(errc), interprocess_error_category()};
}

class InterprocessError : public std::runtime_error
{
public:
    explicit InterprocessError(InterprocessErrc errc)
        : std::runtime_error(make_error_code(errc).message()), errc_(errc), code_(make_error_code(errc))
    {
    }

    InterprocessError(InterprocessErrc errc, const std::string& message)
        : std::runtime_error(message), errc_(errc), code_(make_error_code(errc))
    {
    }

    InterprocessErrc errc() const noexcept
    {
        return errc_;
    }

    std::error_code code() const noexcept
    {
        return code_;
    }

private:
    InterprocessErrc errc_;
    std::error_code code_;
};

[[noreturn]] inline void throw_interprocess_error(InterprocessErrc errc)
{
    throw InterprocessError(errc);
}

[[noreturn]] inline void throw_interprocess_error(InterprocessErrc errc,
                                                  const std::string& message)
{
    throw InterprocessError(errc, message);
}

} // namespace interprocess

namespace std
{

template <>
struct is_error_code_enum<interprocess::InterprocessErrc> : true_type
{
};

} // namespace std
