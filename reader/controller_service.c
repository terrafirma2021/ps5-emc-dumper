/* Position-independent, resident, bounded taskqueue exercise. The service is
 * called only by the owner-gated getppid entry on a normal kernel stack. */
#include "task_probe.h"

#define TG_M_WAITOK 2
#define TG_PI_NET 8
#define TG_CLEANUP_BUSY 20
#ifdef TG_HOST_TEST
#define TG_CANONICAL UINT64_C(0)
#else
#define TG_CANONICAL UINT64_C(0xffff800000000000)
#endif

typedef void (*tg_enqueue_callback_fn)(void *);
typedef void *(*tg_create_fn)(const char *, int, tg_enqueue_callback_fn, void *);
typedef int (*tg_start_fn)(void **, int, int, const char *, ...);
typedef int (*tg_enqueue_fn)(void *, struct tg_task_source *);
typedef void (*tg_drain_fn)(void *, struct tg_task_source *);
typedef void (*tg_free_fn)(void *);

static const char queue_name[] = "ps5_emc_log";
static int valid_base(const struct tg_controller_request *r)
{
    return r->ktext >= TG_CANONICAL && !(r->ktext & 0xfff) &&
           r->task_entry == r->ktext + TG_TASK_ENTRY_OFFSET &&
           r->task_state == r->ktext + TG_TASK_OFFSET + TG_TASK_STATE_OFFSET &&
           r->source == r->ktext + TG_CONTROLLER_OFFSET + TG_CONTROLLER_TASK_OFFSET &&
           r->identity == r->ktext + TG_CONTROLLER_OFFSET + TG_CONTROLLER_REQUEST_OFFSET &&
           r->controller_fallback == r->ktext + TG_GETPPID_BODY_OFFSET;
}

static int create_queue(struct tg_controller_request *r)
{
    if (r->queue) return 1;
    tg_create_fn create = (void *)(uintptr_t)(r->ktext + 0x4ea760);
    tg_start_fn start = (void *)(uintptr_t)(r->ktext + 0x4eb820);
    tg_free_fn release = (void *)(uintptr_t)(r->ktext + 0x4ea8b0);
    tg_enqueue_callback_fn wake = (void *)(uintptr_t)(r->ktext + 0x261418);
    r->queue = (uintptr_t)create(queue_name, TG_M_WAITOK, wake, &r->queue);
    if (!r->queue || r->queue < TG_CANONICAL) {
        r->queue = 0;
        return 2;
    }
    int error = start((void **)&r->queue, 1, TG_PI_NET, queue_name);
    r->thread_count = *(volatile uint32_t *)(uintptr_t)(r->queue + 0x60);
    if (error || r->thread_count != 1) {
        release((void *)(uintptr_t)r->queue);
        r->queue = 0;
        return error ? 3 : 4;
    }
    return 0;
}

static int run_task(struct tg_controller_request *r)
{
    if (!r->queue || r->thread_count != 1) return 5;
    struct tg_task_state *state = (void *)(uintptr_t)r->task_state;
    struct tg_task_source *source = (void *)(uintptr_t)r->source;
    if (state->identity || state->source || state->admission) return 6;
    source->next = 0;
    source->pending = 0;
    source->priority = 0;
    source->pad = 0;
    source->function = (void *)(uintptr_t)r->task_entry;
    source->context = (void *)(uintptr_t)r->identity;
    state->source = r->source;
    state->target = r->ktext + TG_TASK_OFFSET + 0x100;
    state->fallback = r->ktext + TG_TASK_BODY_OFFSET;
    /* Exact identity/source registration is visible before the first enqueue.
     * Once published, the identity remains owned through the boot. */
    __atomic_store_n(&state->identity, r->identity, __ATOMIC_RELEASE);
    tg_enqueue_fn enqueue = (void *)(uintptr_t)(r->ktext + 0x4ea9b0);
    tg_drain_fn drain = (void *)(uintptr_t)(r->ktext + 0x4eb3c0);
    r->marker_before = state->marker;
    r->calls_before = state->calls;
    r->enqueue_result = (uint64_t)enqueue((void *)(uintptr_t)r->queue, source);
    if (r->enqueue_result) return 7;
    drain((void *)(uintptr_t)r->queue, source);
    r->marker_after = state->marker;
    r->calls_after = state->calls;
    return state->marker == 0 &&
           state->seen_identity == r->identity &&
           state->calls == r->calls_before + 1 && state->pending == 1 &&
           state->admission == 0 ? 0 : 8;
}

static int close_task(struct tg_controller_request *r, int exercise_closed)
{
    struct tg_task_state *state = (void *)(uintptr_t)r->task_state;
    struct tg_task_source *source = (void *)(uintptr_t)r->source;
    tg_drain_fn drain = (void *)(uintptr_t)(r->ktext + 0x4eb3c0);
    tg_free_fn release = (void *)(uintptr_t)(r->ktext + 0x4ea8b0);
    if (state->identity) {
        tg_task_close(state);
        if (r->queue) drain((void *)(uintptr_t)r->queue, source);
        if (state->admission != TG_TASK_CLOSED) return TG_CLEANUP_BUSY;
    }
    if (!r->queue) return 0;
    if (exercise_closed && state->identity) {
        tg_enqueue_fn enqueue = (void *)(uintptr_t)(r->ktext + 0x4ea9b0);
        r->calls_before = state->calls;
        r->enqueue_result = (uint64_t)enqueue((void *)(uintptr_t)r->queue, source);
        if (r->enqueue_result) return 9;
        drain((void *)(uintptr_t)r->queue, source);
        r->calls_after = state->calls;
        if (r->calls_after != r->calls_before) return 10;
    }
    release((void *)(uintptr_t)r->queue);
    r->queue = 0;
    r->thread_count = 0;
    ++r->cleanup_count;
    return 0;
}

__attribute__((section(".text.entry")))
uint64_t tg_controller_service(void *td, void *uap,
                               struct tg_controller_request *r)
{
    if (!valid_base(r)) {
        r->status = 11;
        return 0;
    }
    uint64_t flags, stack;
    unsigned short cs;
    __asm__ volatile("pushfq; pop %0; mov %%rsp,%1; mov %%cs,%2"
        : "=r"(flags), "=r"(stack), "=r"(cs));
#ifdef TG_HOST_TEST
    (void)flags;
    (void)stack;
    (void)cs;
    if (0) {
#else
    if (stack < TG_CANONICAL || (cs & 3) || !(flags & 0x200)) {
#endif
        r->status = 12;
    } else {
        switch (r->command) {
        case TG_CMD_CREATE: r->status = create_queue(r); break;
        case TG_CMD_RUN: r->status = run_task(r); break;
        case TG_CMD_CLOSE: r->status = close_task(r, 1); break;
        case TG_CMD_CLEANUP: r->status = close_task(r, 0); break;
        default: r->status = 13; break;
        }
    }
    int (*original)(void *, void *) =
        (void *)(uintptr_t)r->controller_fallback;
    /* The captured handler returns errno and writes td_retval at td+0x408.
     * Preserve both parts of that syscall ABI for the owner to verify. */
    r->original_status = (uint64_t)original(td, uap);
    r->original_ppid = *(volatile uint64_t *)((uintptr_t)td +
                                               TG_GETPPID_RETVAL_OFFSET);
    return r->original_status;
}
