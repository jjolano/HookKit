#include "HKIDs.h"

#include <pthread.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t hk_compute_process_nonce(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t stack_entropy = (uint64_t)(uintptr_t)&tv;  // ASLR
    return ((uint64_t)tv.tv_sec << 32) ^ (uint64_t)tv.tv_usec
         ^ (uint64_t)getpid() ^ stack_entropy;
}

static uint64_t g_nonce;
static pthread_once_t g_nonce_once = PTHREAD_ONCE_INIT;

static void hk_init_nonce(void) {
    g_nonce = hk_compute_process_nonce();
}

static atomic_uint_least64_t g_counter = 0;

hk_id_t hk_id_generate(void) {
    pthread_once(&g_nonce_once, hk_init_nonce);
    // Starts at 1, not 0, so a zero-initialized hk_id_t ({0,0}) stays
    // distinguishable as "no ID" for callers that want that sentinel.
    uint64_t counter = atomic_fetch_add_explicit(&g_counter, 1, memory_order_relaxed) + 1;
    hk_id_t id;
    id.high = g_nonce;
    id.low = counter;
    return id;
}
