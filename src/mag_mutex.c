#include "mag_mutex.h"
#include <stdlib.h>
#include <time.h>

static constexpr int MAX_SPIN_COUNT              = 40;
static constexpr long long NS_PER_SEC            = 1000000000LL;
static constexpr long long NS_PER_MS             = 1000000LL;
static constexpr long long FAIRNESS_THRESHOLD_NS = 1LL * NS_PER_MS;

static constexpr size_t BUCKET_COUNT     = 64;
static constexpr int POINTER_ALIGN_SHIFT = 3;

static inline void mag_yield() {
#if defined(_WIN32)
    SwitchToThread();
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    sched_yield();
#endif
}

static inline long long get_nanos(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (long long)ts.tv_sec * NS_PER_SEC + (long long)ts.tv_nsec;
}

#ifdef MAG_DEBUG
static constexpr size_t MAX_DEBUG_EDGES = 4096;
static constexpr size_t MAX_HELD_LOCKS = 64;

static plat_mtx_t debug_graph_mutex;
static struct {
    const MagMutex *from;
    const MagMutex *to;
} lock_edges[MAX_DEBUG_EDGES];
static int num_lock_edges = 0;

static thread_local const MagMutex *held_locks[MAX_HELD_LOCKS];
static thread_local int held_locks_count = 0;

static bool dfs_check_cycle(const MagMutex *current, const MagMutex *target) {
    if (current == target) return true;
    for (int i = 0; i < num_lock_edges; i++) {
        if (lock_edges[i].from == current) {
            if (dfs_check_cycle(lock_edges[i].to, target)) return true;
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
    _Atomic bool handed_off;
    long long time_to_be_fair;
    Waiter *next;
};

typedef struct {
    plat_mtx_t mutex;
    Waiter *head;
} Bucket;

static Bucket parking_lot[BUCKET_COUNT];

[[gnu::constructor]]
static void init_parking_lot() {
    for (size_t i = 0; i < BUCKET_COUNT; i++) {
        PLAT_MTX_INIT(&parking_lot[i].mutex);
        parking_lot[i].head = nullptr;
    }
#ifdef MAG_DEBUG
    PLAT_MTX_INIT(&debug_graph_mutex);
#endif
}

static inline size_t hash_address(const MagMutex *addr) {
    uintptr_t val = (uintptr_t)addr;
    return (val >> POINTER_ALIGN_SHIFT) % BUCKET_COUNT;
}

void mag_mutex_lock_slow(MagMutex *m) {
    size_t hash          = hash_address(m);
    Bucket *bucket       = &parking_lot[hash];
    long long start_time = 0;

    for (int spin = 0; spin < MAX_SPIN_COUNT; spin++) {
        uint8_t v = atomic_load_explicit(&m->bits, memory_order_relaxed);
        if (!(v & MAG_LOCKED) && !(v & MAG_POISONED)) {
            // Using try_lock here means spinners won't steal the lock 
            // if MAG_HAS_WAITERS is set. This helps prevent starvation.
            if (mag_mutex_try_lock(m)) {
#ifdef MAG_DEBUG
                atomic_fetch_add_explicit(&m->spin_success_count, 1, memory_order_relaxed);
#endif
                return;
            }
        }
        mag_yield();
    }

    for (;;) {
        uint8_t v = atomic_load_explicit(&m->bits, memory_order_relaxed);

#ifdef MAG_DEBUG
        if (v & MAG_POISONED) {
            assert(false && "FATAL: Mutex poisoned while waiting in slow path!");
            abort();
        }
#endif

        if (!(v & MAG_LOCKED)) {
            // Fix: Once in the slow path, we must be able to acquire the lock 
            // even if MAG_HAS_WAITERS is set. We CAS to v | MAG_LOCKED instead 
            // of calling try_lock (which requires v to be exactly MAG_UNLOCKED).
            uint8_t expected = v;
            if (atomic_compare_exchange_weak_explicit(&m->bits, &expected, v | MAG_LOCKED,
                                                      memory_order_acquire, memory_order_relaxed)) {
#ifdef MAG_DEBUG
                mag_debug_post_lock(m);
#endif
                return;
            }
            continue; 
        }

        if (!(v & MAG_HAS_WAITERS)) {
            uint8_t expected = v;
            if (!atomic_compare_exchange_weak_explicit(&m->bits, &expected, v | MAG_HAS_WAITERS,
                                                       memory_order_relaxed, memory_order_relaxed)) {
                continue;
            }
        }

        if (start_time == 0) start_time = get_nanos();

#ifdef MAG_DEBUG
        atomic_fetch_add_explicit(&m->park_count, 1, memory_order_relaxed);
#endif

        Waiter node = {
            .address         = m,
            .time_to_be_fair = start_time + FAIRNESS_THRESHOLD_NS,
            .next            = nullptr
        };
        atomic_init(&node.signaled, false);
        atomic_init(&node.handed_off, false);
        PLAT_CND_INIT(&node.cond);

        PLAT_MTX_LOCK(&bucket->mutex);

        uint8_t v_now = atomic_load_explicit(&m->bits, memory_order_relaxed);
        
        /* FIX: The "Lost Wakeup" deadlocks happen here. 
           If MAG_HAS_WAITERS was wiped by an unlocking thread while we were waiting 
           for the bucket lock, we CANNOT safely park. We must abort, loop, and re-set it. */
        if (!(v_now & MAG_LOCKED) || !(v_now & MAG_HAS_WAITERS) || (v_now & MAG_POISONED)) {
            PLAT_MTX_UNLOCK(&bucket->mutex);
            PLAT_CND_DESTROY(&node.cond);
            continue;
        }

        node.next    = bucket->head;
        bucket->head = &node;

        while (!atomic_load_explicit(&node.signaled, memory_order_acquire)) {
            PLAT_CND_WAIT(&node.cond, &bucket->mutex);
        }

        PLAT_MTX_UNLOCK(&bucket->mutex);
        PLAT_CND_DESTROY(&node.cond);

        if (atomic_load_explicit(&node.handed_off, memory_order_acquire)) {
#ifdef MAG_DEBUG
            mag_debug_post_lock(m);
#endif
            return;
        }
    }
}

void mag_mutex_unlock_slow(MagMutex *m) {
    size_t hash    = hash_address(m);
    Bucket *bucket = &parking_lot[hash];
    long long now  = get_nanos();

    PLAT_MTX_LOCK(&bucket->mutex);

    Waiter **curr     = &bucket->head;
    Waiter *to_wake   = nullptr;
    bool more_waiters = false;

    while (*curr != nullptr) {
        if ((*curr)->address == m && to_wake == nullptr) {
            to_wake = *curr;
            *curr   = to_wake->next;
            continue;
        }
        if ((*curr)->address == m) {
            more_waiters = true;
        }
        curr = &((*curr)->next);
    }

    uint8_t current_bits = atomic_load_explicit(&m->bits, memory_order_relaxed);
    uint8_t new_bits     = MAG_UNLOCKED | (current_bits & MAG_POISONED);

    if (to_wake != nullptr) {
        if (now > to_wake->time_to_be_fair) {
            atomic_store_explicit(&to_wake->handed_off, true, memory_order_relaxed);
            new_bits |= MAG_LOCKED;
        }
        if (more_waiters) {
            new_bits |= MAG_HAS_WAITERS;
        }
    }

    atomic_store_explicit(&m->bits, new_bits, memory_order_release);

    if (to_wake != nullptr) {
        // Signal while still holding bucket->mutex so the waiter
        // cannot destroy node.cond before we're done with it.
        atomic_store_explicit(&to_wake->signaled, true, memory_order_release);
        PLAT_CND_SIGNAL(&to_wake->cond);
    }

    PLAT_MTX_UNLOCK(&bucket->mutex);
}