/**
 * @file    shm_ring.h
 * @brief   共享内存环形队列的布局与收发算法。
 *
 * 单向 SPSC（单生产者单消费者）字节环：head/tail 是自由增长的字节计数，
 * 下标为 pos % capacity，因此空/满没有二义性。变长记录在容量尾部放不下时，
 * 先写一条 8 字节的跳过标记，再从 0 继续。
 *
 * 该头文件只描述内存布局和算法，不负责创建共享内存或跨进程唤醒；
 * 这些由 shared_memory.cpp 负责。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ara {
namespace com {
namespace ipc {
namespace shm {

constexpr std::uint64_t RING_MAGIC = 0x41524153484D3031ULL;  // "ARASHM01"
constexpr std::uint32_t RING_VERSION = 1U;
constexpr std::uint32_t RING_STATE_OK = 0U;
constexpr std::uint32_t RING_STATE_BROKEN = 1U;
constexpr std::uint32_t RECORD_FLAG_SKIP = 1U;
constexpr std::size_t RECORD_HEADER_SIZE = 8U;
constexpr std::size_t RING_ALIGNMENT = 8U;
constexpr std::size_t MIN_CAPACITY = 4096U;
constexpr std::size_t MAX_CAPACITY = 64U * 1024U * 1024U;

/**
 * @struct RingHeader
 * @brief 每个方向一份，放在共享内存起始位置。
 *
 * head 与 tail 各占一条 cache line，避免生产者和消费者互相踩伪共享。
 * 其余不可变字段在 magic 发布前写完，客户端以 acquire 读取 magic 后再使用。
 */
struct alignas(64) RingHeader {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint64_t capacity;
    std::uint64_t max_message;
    std::int32_t  creator_pid;
    std::uint32_t state;

    alignas(64) std::uint64_t head;   // 消费者位置（自由增长字节计数）
    alignas(64) std::uint64_t tail;   // 生产者位置

    std::uint64_t published;
    std::uint64_t consumed;
    std::uint64_t dropped;
    std::uint64_t reserved;
};

static_assert(sizeof(RingHeader) % 64 == 0, "RingHeader 需要按 cache line 对齐");
static_assert(offsetof(RingHeader, head) % 64 == 0, "head 需要独占 cache line");
static_assert(offsetof(RingHeader, tail) % 64 == 0, "tail 需要独占 cache line");

struct RecordHeader {
    std::uint32_t length;
    std::uint32_t flags;
};

static_assert(sizeof(RecordHeader) == RECORD_HEADER_SIZE, "记录头固定 8 字节");

inline constexpr std::size_t region_size(std::size_t capacity) {
    return sizeof(RingHeader) + capacity;
}

inline std::uint64_t align_record(std::size_t payload_size) {
    const std::uint64_t raw = static_cast<std::uint64_t>(RECORD_HEADER_SIZE) +
                              static_cast<std::uint64_t>(payload_size);
    return (raw + RING_ALIGNMENT - 1U) & ~static_cast<std::uint64_t>(RING_ALIGNMENT - 1U);
}

// 共享内存里的原子操作不依赖 std::atomic 的跨进程 ABI 假设，直接使用 GCC/Clang 内建。
inline std::uint64_t load_acquire(const std::uint64_t* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

inline std::uint64_t load_relaxed(const std::uint64_t* value) {
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

inline void store_release(std::uint64_t* value, std::uint64_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

inline void store_relaxed(std::uint64_t* value, std::uint64_t next) {
    __atomic_store_n(value, next, __ATOMIC_RELAXED);
}

inline std::uint32_t load_state(const RingHeader* header) {
    return __atomic_load_n(&header->state, __ATOMIC_ACQUIRE);
}

inline void mark_broken(RingHeader* header) {
    __atomic_store_n(&header->state, RING_STATE_BROKEN, __ATOMIC_RELEASE);
}

inline void add_counter(std::uint64_t* counter) {
    __atomic_fetch_add(counter, 1U, __ATOMIC_RELAXED);
}

/**
 * @brief 初始化共享内存头部；magic 最后发布。
 */
inline void reset(RingHeader* header, std::size_t capacity, std::size_t max_message,
                  std::int32_t creator_pid) {
    std::memset(static_cast<void*>(header), 0, sizeof(RingHeader));
    header->version = RING_VERSION;
    header->header_size = static_cast<std::uint32_t>(sizeof(RingHeader));
    header->capacity = capacity;
    header->max_message = max_message;
    header->creator_pid = creator_pid;
    header->state = RING_STATE_OK;
    __atomic_store_n(&header->magic, RING_MAGIC, __ATOMIC_RELEASE);
}

/**
 * @brief 校验头部是否可用；客户端在映射后调用。
 */
inline bool valid(const RingHeader* header) {
    if (__atomic_load_n(&header->magic, __ATOMIC_ACQUIRE) != RING_MAGIC) return false;
    if (header->version != RING_VERSION) return false;
    if (header->header_size != sizeof(RingHeader)) return false;
    if (header->capacity < MIN_CAPACITY || header->capacity > MAX_CAPACITY) return false;
    if (header->capacity % RING_ALIGNMENT != 0) return false;
    if (header->max_message == 0 || header->max_message > header->capacity - RECORD_HEADER_SIZE) {
        return false;
    }
    const std::uint64_t head = load_acquire(&header->head);
    const std::uint64_t tail = load_acquire(&header->tail);
    if (tail < head || tail - head > header->capacity) return false;
    return load_state(header) == RING_STATE_OK;
}

/**
 * @brief 非阻塞写入一条记录。
 * @return 空间不足或参数非法返回 false；空间不足会计入 dropped。
 */
inline bool write(RingHeader* header, const std::uint8_t* data, std::size_t length) {
    if (length > header->max_message) return false;
    const std::uint64_t capacity = header->capacity;
    const std::uint64_t need = align_record(length);
    if (need > capacity) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(header) + header->header_size;
    std::uint64_t pos = load_relaxed(&header->tail);
    const std::uint64_t head = load_acquire(&header->head);
    if (pos < head || pos - head > capacity) {
        mark_broken(header);
        return false;
    }

    std::uint64_t offset = pos % capacity;
    std::uint64_t remain = capacity - offset;

    if (remain < need) {
        // 回绕本身也要占用空间；空间不够时不能写跳过标记，否则会盖住未读记录。
        if (pos - head + remain > capacity) {
            add_counter(&header->dropped);
            return false;
        }
        if (remain >= RECORD_HEADER_SIZE) {
            const RecordHeader skip{0, RECORD_FLAG_SKIP};
            std::memcpy(base + offset, &skip, sizeof(skip));
        }
        pos += remain;
        offset = 0;
    }

    if (pos - head + need > capacity) {
        add_counter(&header->dropped);
        return false;
    }

    const RecordHeader record{static_cast<std::uint32_t>(length), 0};
    std::memcpy(base + offset, &record, sizeof(record));
    if (length > 0) {
        std::memcpy(base + offset + RECORD_HEADER_SIZE, data, length);
    }

    store_release(&header->tail, pos + need);
    add_counter(&header->published);
    return true;
}

/**
 * @brief 非阻塞读取一条记录。
 * @return 没有数据返回 false；发现越界/不完整记录时把通道标成 BROKEN。
 */
inline bool read(RingHeader* header, std::vector<std::uint8_t>& out) {
    if (load_state(header) != RING_STATE_OK) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(header) + header->header_size;
    const std::uint64_t capacity = header->capacity;

    for (;;) {
        std::uint64_t head = load_relaxed(&header->head);
        const std::uint64_t tail = load_acquire(&header->tail);
        if (tail < head || tail - head > capacity) {
            mark_broken(header);
            return false;
        }
        if (head == tail) return false;

        const std::uint64_t offset = head % capacity;
        const std::uint64_t remain = capacity - offset;
        if (remain < RECORD_HEADER_SIZE) {
            head += remain;
            store_release(&header->head, head);
            continue;
        }

        RecordHeader record{};
        std::memcpy(&record, base + offset, sizeof(record));

        if (record.flags == RECORD_FLAG_SKIP) {
            head += remain;
            store_release(&header->head, head);
            continue;
        }
        if (record.flags != 0) {
            mark_broken(header);
            return false;
        }
        if (record.length > header->max_message ||
            record.length > capacity - RECORD_HEADER_SIZE) {
            mark_broken(header);
            return false;
        }

        const std::uint64_t need = align_record(record.length);
        if (need > tail - head) {
            // tail 已发布但记录不完整，说明共享内存被外部写坏。
            mark_broken(header);
            return false;
        }

        const auto* payload = base + offset + RECORD_HEADER_SIZE;
        out.assign(payload, payload + record.length);
        store_release(&header->head, head + need);
        add_counter(&header->consumed);
        return true;
    }
}

} // namespace shm
} // namespace ipc
} // namespace com
} // namespace ara
