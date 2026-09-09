#pragma once

#include <mutex>
#include <new>

namespace netlib::ndisapi
{
    /// <summary>
    /// A singleton class that manages a pool of intermediate_buffer objects.
    /// </summary>
    class intermediate_buffer_pool {
    public:
        /// <summary>
        /// Deleted copy constructor to prevent copying.
        /// </summary>
        intermediate_buffer_pool(const intermediate_buffer_pool&) = delete;

        /// <summary>
        /// Deleted copy assignment operator to prevent copying.
        /// </summary>
        intermediate_buffer_pool& operator=(const intermediate_buffer_pool&) = delete;

        /// <summary>
        /// Default move constructor.
        /// </summary>
        intermediate_buffer_pool(intermediate_buffer_pool&&) noexcept = delete;

        /// <summary>
        /// Default move assignment operator.
        /// </summary>
        intermediate_buffer_pool& operator=(intermediate_buffer_pool&&) noexcept = delete;

        /// <summary>
        /// Default destructor.
        /// </summary>
        ~intermediate_buffer_pool() = default;

        /// <summary>
        /// Accessor for the singleton instance.
        /// </summary>
        /// <returns>A reference to the singleton instance of intermediate_buffer_pool.</returns>
        static intermediate_buffer_pool& instance() {
            static intermediate_buffer_pool instance;
            return instance;
        }

        /// <summary>
        /// Custom deleter for unique_ptr.
        /// </summary>
        struct deleter {
            /// <summary>
            /// Deletes the intermediate_buffer object.
            /// </summary>
            /// <param name="ptr">Pointer to the intermediate_buffer object to be deleted.</param>
            void operator()(intermediate_buffer* ptr) const {
                delete ptr;
            }
        };

        using intermediate_buffer_ptr = std::unique_ptr<intermediate_buffer, deleter>;

        /// <summary>
        /// Allocates a new intermediate_buffer object from the pool.
        /// </summary>
        /// <returns>A unique_ptr to the allocated intermediate_buffer object, or nullptr if allocation fails.</returns>
        intermediate_buffer_ptr allocate() {
            std::scoped_lock lock(mutex_);
            intermediate_buffer* raw_ptr = nullptr;

            try {
                raw_ptr = new (std::nothrow) intermediate_buffer();
                if (raw_ptr == nullptr)
                    return nullptr;
                std::fill_n(reinterpret_cast<char*>(raw_ptr), offsetof(_INTERMEDIATE_BUFFER, m_IBuffer), 0);
            }
            catch (...) {
                return nullptr;
            }

            return intermediate_buffer_ptr(raw_ptr, deleter{});
        }

        /// <summary>
        /// Allocates a new intermediate_buffer object from the pool and initializes it with the provided source.
        /// </summary>
        /// <param name="source">The source intermediate_buffer object to copy from.</param>
        /// <returns>A unique_ptr to the allocated and initialized intermediate_buffer object, or nullptr if allocation fails.</returns>
        intermediate_buffer_ptr allocate(const intermediate_buffer& source) {
            auto buffer = allocate(); // Allocate a new buffer

            if (buffer) { // Check if allocation was successful
                *buffer = source; // Use the copy assignment operator of intermediate_buffer
            }
            return buffer; // Return the allocated buffer or nullptr if allocation failed
        }

    private:
        /// <summary>
        /// Private constructor for singleton.
        /// </summary>
        /// <param name="initial_size">The initial size of the pool.</param>
        intermediate_buffer_pool() = default;

        std::mutex mutex_; ///< Mutex to protect the pool.
    };
}
