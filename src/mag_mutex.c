#include "mag_mutex.h"
#include <stdlib.h>
#include <time.h>

static constexpr int MAX_SPIN_COUNT = 100;

// Address sharding for the parking lot to eliminate false-sharing lock
// contention. 256 buckets padded to Apple Silicon / Modern x86 strict
// cache-line widths.
static constexpr size_t BUCKET_COUNT = 256;

#ifndef MAG_CACHELINE_ALIGNED
#    if defined(_MSC_VER)
#        define MAG_CACHELINE_ALIGNED __declspec(align(128))
#    elif defined(__GNUC__) || defined(__clang__)
#        define MAG_CACHELINE_ALIGNED __attribute__((aligned(128)))
#    else
#        define MAG_CACHELINE_ALIGNED
#    endif
#endif

// Pure CPU pause. Removed sched_yield which causes severe OS stalls under high
// contention.
static inline void mag_cpu_relax() {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(_WIN32)
    YieldProcessor();
#endif
}

static_assert(ATOMIC_INT_LOCK_FREE == 2, "MagMutex requires always-lock-free atomics!");
static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "MagMutex parking lot requires lock-free pointers!");

#ifdef MAG_DEBUG
static constexpr size_t MAX_DEBUG_EDGES = 4096;
static constexpr size_t MAX_HELD_LOCKS  = 64;

static plat_mtx_t debug_graph_mutex;
static struct {
    const MagMutex *from;
    const MagMutex *to;
} lock_edges[MAX_DEBUG_EDGES];
static int num_lock_edges = 0;

static thread_local const MagMutex *held_locks[MAX_HELD_LOCKS];
static thread_local int held_locks_count = 0;

static bool dfs_check_cycle(const MagMutex *current, const MagMutex *target) {
    if (current == target)
        return true;
    for (int i = 0; i < num_lock_edges; i++) {
        if (lock_edges[i].from == current) {
            if (dfs_check_cycle(lock_edges[i].to, target))
                return true;
        }
    }
    return false;
}

static void add_lock_edge(const MagMutex *from, const MagMutex *to) {
    PLAT_MTX_LOCK(&debug_graph_mutex);

    for (int i = 0; i < num_lock_edges; i++) {
        if (lock_edges[i].from == from && lock_edges[i].to == to) {
            PLAT_MTX_UNLOCK(&debug_graph_mutex);
            return;
        }
    }

    if (dfs_check_cycle(to, from)) {
        PLAT_MTX_UNLOCK(&debug_graph_mutex);
        assert(false && "DEADLOCK DETECTED: Lock order cycle/inversion!");
        abort();
    }

    if (num_lock_edges < MAX_DEBUG_EDGES) {
        lock_edges[num_lock_edges].from = from;
        lock_edges[num_lock_edges].to   = to;
        num_lock_edges++;
    }
    PLAT_MTX_UNLOCK(&debug_graph_mutex);
}

void mag_debug_clear_owner(MagMutex *m) {
    atomic_store_explicit(&m->has_owner, false, memory_order_release);
}

void mag_debug_check_pre_lock(MagMutex *m) {
    uint8_t bits = atomic_load_explicit(&m->bits, memory_order_relaxed);
    if (bits & MAG_POISONED) {
        assert(false && "FATAL: Attempting to lock a POISONED mutex!");
        abort();
    }

    if (bits & MAG_LOCKED) {
        if (atomic_load_explicit(&m->has_owner, memory_order_acquire)) {
            plat_thread_id_t current = PLAT_CURRENT_THREAD();
            plat_thread_id_t owner   = atomic_load_explicit(&m->owner, memory_order_relaxed);
            if (PLAT_THREADS_EQUAL(owner, current)) {
                assert(false && "DEADLOCK DETECTED: Recursive locking!");
                abort();
            }
        }
    }
}

void mag_debug_post_lock(MagMutex *m) {
    atomic_store_explicit(&m->owner, PLAT_CURRENT_THREAD(), memory_order_relaxed);
    atomic_store_explicit(&m->has_owner, true, memory_order_release);

    for (int i = 0; i < held_locks_count; i++) {
        add_lock_edge(held_locks[i], m);
    }
    if (held_locks_count < MAX_HELD_LOCKS) {
        held_locks[held_locks_count++] = m;
    }
}

void mag_debug_pre_unlock(MagMutex *m) {
    if (!atomic_load_explicit(&m->has_owner, memory_order_acquire)) {
        assert(false && "FATAL: Unlocking a mutex that has no owner!");
        abort();
    }

    plat_thread_id_t current = PLAT_CURRENT_THREAD();
    plat_thread_id_t owner   = atomic_load_explicit(&m->owner, memory_order_relaxed);

    if (!PLAT_THREADS_EQUAL(owner, current)) {
        assert(false && "FATAL: Unlocking a mutex owned by another thread!");
        abort();
    }

    for (int i = held_locks_count - 1; i >= 0; i--) {
        if (held_locks[i] == m) {
            for (int j = i; j < held_locks_count - 1; j++) {
                held_locks[j] = held_locks[j + 1];
            }
            held_locks_count--;
            break;
        }
    }
}
#endif

typedef struct Waiter Waiter;
struct Waiter {
    const MagMutex *address;
    plat_cnd_t cond;
    _Atomic bool signaled;
    Waiter *next;
};

// Padded 128-byte cacheline bucket to completely eliminate false sharing.
typedef struct {
    plat_mtx_t mutex;
    Waiter *head;
    uint8_t __pad[128 - (sizeof(plat_mtx_t) + sizeof(void *))];
} Bucket;

#if defined(_MSC_VER)
__declspec(align(128)) static Bucket parking_lot[BUCKET_COUNT];
#else
static Bucket parking_lot[BUCKET_COUNT] __attribute__((aligned(128)));
#endif

static_assert(sizeof(Bucket) == 128, "Bucket must be exactly 128 bytes!");

[[gnu::constructor]]
static void init_parking_lot() {
    for (size_t i = 0; i < BUCKET_COUNT; i++) {
        PLAT_MTX_INIT(&parking_lot[i].mutex);
        parking_lot[i].head = nullptr;
    }
}

static inline size_t hash_address(const MagMutex *addr) {
    uintptr_t x = (uintptr_t)addr;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x % BUCKET_COUNT;
}

MAG_COLD void MagMutex_LockSlow(MagMutex *m) {
    size_t hash    = hash_address(m);
    Bucket *bucket = &parking_lot[hash];

    for (int spin = 0; spin < MAX_SPIN_COUNT; spin++) {
        uint8_t v = atomic_load_explicit(&m->bits, memory_order_relaxed);
        if (!(v & MAG_LOCKED) && !(v & MAG_POISONED)) {
            if (atomic_compare_exchange_weak_explicit(&m->bits, &v, v | MAG_LOCKED,
                                                      memory_order_acquire, memory_order_relaxed)) {
                mag_debug_post_lock(m);
#ifdef MAG_DEBUG
                atomic_fetch_add_explicit(&m->spin_success_count, 1, memory_order_relaxed);
#endif
                return;
            }
        }
        mag_cpu_relax();
    }

    for (;;) {
        uint8_t v = atomic_load_explicit(&m->bits, memory_order_relaxed);
        if (!(v & MAG_LOCKED)) {
            if (atomic_compare_exchange_weak_explicit(&m->bits, &v, v | MAG_LOCKED,
                                                      memory_order_acquire, memory_order_relaxed)) {
                mag_debug_post_lock(m);
                return;
            }
            continue;
        }
        if (!(v & MAG_HAS_WAITERS)) {
            if (!atomic_compare_exchange_weak_explicit(&m->bits, &v, v | MAG_HAS_WAITERS,
                                                       memory_order_relaxed, memory_order_relaxed))
                continue;
        }

#ifdef MAG_DEBUG
        atomic_fetch_add_explicit(&m->park_count, 1, memory_order_relaxed);
#endif
        Waiter node = {.address = m, .next = nullptr};
        atomic_init(&node.signaled, false);
        PLAT_CND_INIT(&node.cond);
        PLAT_MTX_LOCK(&bucket->mutex);

        v = atomic_load_explicit(&m->bits, memory_order_relaxed);
        // Define the "Required State" for parking:
        // Must be LOCKED and have WAITERS bit set, and NOT poisoned.
        constexpr uint8_t PARK_MASK = MAG_LOCKED | MAG_HAS_WAITERS | MAG_POISONED;
        constexpr uint8_t PARK_EXPECTED = MAG_LOCKED | MAG_HAS_WAITERS;
        // Single branch: If the state is anything other than (LOCKED | WAITERS), bail.
        if (MAG_LIKELY((v & PARK_MASK) != PARK_EXPECTED)) {
            PLAT_MTX_UNLOCK(&bucket->mutex);
            PLAT_CND_DESTROY(&node.cond);
            continue;
        }

        node.next    = bucket->head;
        bucket->head = &node;
        while (!atomic_load_explicit(&node.signaled, memory_order_acquire))
            PLAT_CND_WAIT(&node.cond, &bucket->mutex);

        PLAT_MTX_UNLOCK(&bucket->mutex);
        PLAT_CND_DESTROY(&node.cond);
        // Note: Decoupled unlock means we wake up unlocked. We loop to try and re-acquire.
    }
}

MAG_COLD void MagMutex_UnlockSlow(MagMutex *m) {
    uint8_t v = atomic_load_explicit(&m->bits, memory_order_relaxed);
    for (;;) {
        uint8_t desired = v & ~MAG_LOCKED;
        if (atomic_compare_exchange_weak_explicit(&m->bits, &v, desired, memory_order_release,
                                                  memory_order_relaxed)) {
            if (!(v & MAG_HAS_WAITERS))
                return;
            break;
        }
    }

    size_t hash    = hash_address(m);
    Bucket *bucket = &parking_lot[hash];
    PLAT_MTX_LOCK(&bucket->mutex);

    Waiter **curr   = &bucket->head;
    Waiter *to_wake = nullptr;
    bool more       = false;

    while (*curr != nullptr) {
        if ((*curr)->address == m && to_wake == nullptr) {
            to_wake = *curr;
            *curr   = to_wake->next;
            continue;
        }
        if ((*curr)->address == m)
            more = true;
        curr = &((*curr)->next);
    }

    if (!more) {
        v = atomic_load_explicit(&m->bits, memory_order_relaxed);
        for (;;) {
            if (atomic_compare_exchange_weak_explicit(&m->bits, &v, v & ~MAG_HAS_WAITERS,
                                                      memory_order_relaxed, memory_order_relaxed))
                break;
        }
    }

    if (to_wake != nullptr) {
        atomic_store_explicit(&to_wake->signaled, true, memory_order_release);
        PLAT_CND_SIGNAL(&to_wake->cond);
    }
    PLAT_MTX_UNLOCK(&bucket->mutex);
}