#ifndef WG_TASK_PROBE_H
#define WG_TASK_PROBE_H

/* Final same-boot UI migration slot. CBA/CBF remain clean and need not be
 * adjacent; CBB-CBE hold earlier, closed development images. */
#define TG_TASK_OFFSET 0xcba000
#define TG_CONTROLLER_OFFSET 0xcbf000
#define TG_TASK_ENTRY_OFFSET 0x26a880
#define TG_TASK_BODY_OFFSET 0x4ebd00
#define TG_GETPPID_ENTRY_OFFSET 0x265310
#define TG_GETPPID_BODY_OFFSET 0x2a8b00
#define TG_GETPPID_RETVAL_OFFSET 0x408
#define TG_TASK_STATE_OFFSET 0xf00
#define TG_TASK_CLAIM_OFFSET 0xff0
#define TG_TASK_IMAGE_SIZE 0xf60
#define TG_CONTROLLER_REQUEST_OFFSET 0xd00
#define TG_CONTROLLER_TASK_OFFSET 0xe00
#define TG_CONTROLLER_STATE_OFFSET 0xf00
#define TG_CONTROLLER_CLAIM_OFFSET 0xff0
#define TG_CONTROLLER_IMAGE_SIZE 0xf40

#define TG_TASK_IDENTITY 0x00
#define TG_TASK_SOURCE 0x08
#define TG_TASK_ADMISSION 0x10
#define TG_TASK_TARGET 0x18
#define TG_TASK_FALLBACK 0x20
#define TG_TASK_CALLS 0x28
#define TG_TASK_PENDING 0x30
#define TG_TASK_MARKER 0x38
#define TG_TASK_STACK 0x40
#define TG_TASK_CS 0x48
#define TG_TASK_SEEN_IDENTITY 0x50
#define TG_TASK_ICC_QUERY 0x58
#define TG_TASK_CLOSED 0x8000000000000000
#define TG_TASK_MESSAGE_OFFSET 0x300
#define TG_TASK_MESSAGE_SIZE 0x7f0
#define TG_EMC_LOG_SIZE 0x7e4

#define TG_CTRL_OWNER 0x00
#define TG_CTRL_SERVICE 0x08
#define TG_CTRL_REQUEST 0x10
#define TG_CTRL_PHASE 0x18
#define TG_CTRL_LAST_TD 0x20
#define TG_CTRL_RESERVED 0x28
#define TG_CTRL_FALLBACK 0x30
#define TG_CTRL_SEEN 0x38

#define TG_CTRL_IDLE 0
#define TG_CTRL_ARMED 1
#define TG_CTRL_ACTIVE 2
#define TG_CTRL_DRAINING 3
#define TG_CTRL_CLOSED 4

#define TG_CMD_CREATE 1
#define TG_CMD_RUN 2
#define TG_CMD_CLOSE 3
#define TG_CMD_CLEANUP 4

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

struct tg_task_state {
    uint64_t identity, source, admission, target, fallback;
    uint64_t calls, pending, marker, stack, cs, seen_identity, icc_query;
};
_Static_assert(offsetof(struct tg_task_state, admission) == TG_TASK_ADMISSION,
               "task admission offset");
_Static_assert(offsetof(struct tg_task_state, seen_identity) == TG_TASK_SEEN_IDENTITY,
               "task identity marker offset");
_Static_assert(offsetof(struct tg_task_state, icc_query) == TG_TASK_ICC_QUERY,
               "ICC query offset");

struct tg_controller_state {
    uint64_t owner, service, request, phase;
    uint64_t last_thread, reserved, fallback, seen;
};
_Static_assert(sizeof(struct tg_controller_state) == 0x40, "controller state");

struct tg_task_source {
    uint64_t next;
    uint16_t pending, priority;
    uint32_t pad;
    void (*function)(void *, int);
    void *context;
};
_Static_assert(sizeof(struct tg_task_source) == 32 &&
               offsetof(struct tg_task_source, function) == 16 &&
               offsetof(struct tg_task_source, context) == 24, "Sony task ABI");

struct tg_controller_request {
    uint64_t command, status, ktext, task_entry, task_state;
    uint64_t controller_fallback, queue, source, identity;
    uint64_t thread_count, enqueue_result, marker_before, marker_after;
    uint64_t calls_before, calls_after, original_ppid, cleanup_count;
    uint64_t original_status;
};
_Static_assert(offsetof(struct tg_controller_request, original_ppid) == 0x78 &&
               offsetof(struct tg_controller_request, original_status) == 0x88,
               "controller result wire offsets");
_Static_assert(sizeof(struct tg_controller_request) <=
               TG_CONTROLLER_TASK_OFFSET - TG_CONTROLLER_REQUEST_OFFSET,
               "controller request fits before task source");

static inline int tg_controller_original_ok(const struct tg_controller_request *r,
                                             uint64_t expected_ppid)
{
    return r->original_status == 0 && r->original_ppid == expected_ppid;
}

static inline int tg_controller_arm(struct tg_controller_state *state)
{
    uint64_t expected = TG_CTRL_IDLE;
    return __atomic_compare_exchange_n(&state->phase, &expected,
        TG_CTRL_ARMED, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

enum tg_close_result { TG_CLOSE_INVALID = -1, TG_CLOSE_BUSY = 0, TG_CLOSE_DONE = 1 };
static inline enum tg_close_result
tg_controller_close(struct tg_controller_state *state)
{
    uint64_t phase = __atomic_load_n(&state->phase, __ATOMIC_ACQUIRE);
    for (;;) {
        uint64_t next;
        switch (phase) {
        case TG_CTRL_IDLE:
        case TG_CTRL_ARMED: next = TG_CTRL_CLOSED; break;
        case TG_CTRL_ACTIVE: next = TG_CTRL_DRAINING; break;
        case TG_CTRL_DRAINING: return TG_CLOSE_BUSY;
        case TG_CTRL_CLOSED: return TG_CLOSE_DONE;
        default: return TG_CLOSE_INVALID;
        }
        if (__atomic_compare_exchange_n(&state->phase, &phase, next, 0,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return next == TG_CTRL_CLOSED ? TG_CLOSE_DONE : TG_CLOSE_BUSY;
    }
}

static inline uint64_t tg_task_close(struct tg_task_state *state)
{
    return __atomic_fetch_or(&state->admission, TG_TASK_CLOSED,
                             __ATOMIC_ACQ_REL);
}
#endif
#endif
