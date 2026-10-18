#define _GNU_SOURCE
#include "../task_probe.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

typedef void (*task_fn)(void *, int);
typedef uint64_t (*controller_fn)(void *, void *);
/* Flat copied text has no UBSan function-signature prefix. */
__attribute__((no_sanitize("function")))
static void invoke_task(task_fn entry, void *identity, int pending)
{ entry(identity, pending); }
__attribute__((no_sanitize("function")))
static uint64_t invoke_controller(controller_fn entry, void *owner, void *args)
{ return entry(owner, args); }
static atomic_int fallback_calls, marker_entered, marker_release, service_calls;
static atomic_int service_entered, service_release;
static uint64_t fallback_result = UINT64_C(0x13605eed1234);

static int icc_query(void *request, void *response)
{
    assert(request == response);
    unsigned char *message = request;
    assert(message[1] == 2 && message[2] == 7 && message[8] == 0x20);
    message[0xc] = 0x5a;
    return 0;
}

static void task_fallback(void *identity, int pending)
{
    assert(identity && pending > 0);
    atomic_fetch_add(&fallback_calls, 1);
}

static void blocked_marker(void *identity, int pending)
{
    assert(identity && pending == 7);
    atomic_store_explicit(&marker_entered, 1, memory_order_release);
    while (!atomic_load_explicit(&marker_release, memory_order_acquire))
        sched_yield();
}

static uint64_t controller_fallback(void *thread, void *args)
{
    assert(thread && args);
    return fallback_result;
}

static uint64_t controller_service(void *thread, void *args, void *request)
{
    assert(thread && args && request);
    atomic_fetch_add(&service_calls, 1);
    atomic_store_explicit(&service_entered, 1, memory_order_release);
    while (!atomic_load_explicit(&service_release, memory_order_acquire))
        sched_yield();
    return UINT64_C(0x875eed);
}

static void *read_image(const char *path, size_t image_size)
{
    unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED);
    memset(page, 0x90, 4096);
    FILE *input = fopen(path, "rb");
    assert(input);
    assert(fread(page, 1, image_size, input) == image_size);
    assert(fgetc(input) == EOF);
    assert(fclose(input) == 0);
    return page;
}

static void until(atomic_int *flag)
{
    struct timespec start, now;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        assert(now.tv_sec - start.tv_sec < 10);
        sched_yield();
    }
}

struct task_call { task_fn entry; void *identity; };
static void *run_task(void *opaque)
{
    struct task_call *call = opaque;
    invoke_task(call->entry, call->identity, 7);
    return NULL;
}

struct controller_call { controller_fn entry; void *owner, *args; uint64_t result; };
static void *run_controller(void *opaque)
{
    struct controller_call *call = opaque;
    call->result = invoke_controller(call->entry, call->owner, call->args);
    return NULL;
}

static void test_task(void *page)
{
    struct tg_task_state *state = (void *)((unsigned char *)page + TG_TASK_STATE_OFFSET);
    task_fn entry = (task_fn)page;
    int identity, other;
    state->target = (uintptr_t)((unsigned char *)page + 0x100);
    state->icc_query = (uintptr_t)icc_query;
    unsigned char *message = (unsigned char *)page + TG_TASK_MESSAGE_OFFSET;
    message[1] = 2;
    message[2] = 7;
    message[8] = 0x20;
    state->fallback = (uintptr_t)task_fallback;
    state->source = (uintptr_t)&identity;
    state->identity = (uintptr_t)&identity;
    invoke_task(entry, &other, 1);
    assert(atomic_load(&fallback_calls) == 1 && state->calls == 0);
    invoke_task(entry, &identity, 1);
    assert(state->calls == 1 && state->pending == 1 &&
            state->marker == 0 &&
           state->seen_identity == (uintptr_t)&identity &&
           state->admission == 0);
    assert((state->stack & 15) == 8);

    state->target = (uintptr_t)blocked_marker;
    struct task_call call = {entry, &identity};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_task, &call) == 0);
    until(&marker_entered);
    assert(state->admission == 1);
    assert(tg_task_close(state) == 1);
    assert(state->admission == TG_TASK_CLOSED + 1);
    invoke_task(entry, &identity, 7); /* Closed owned: resident no-op. */
    assert(atomic_load(&fallback_calls) == 1 && state->calls == 1);
    atomic_store_explicit(&marker_release, 1, memory_order_release);
    assert(pthread_join(thread, NULL) == 0);
    assert(state->admission == TG_TASK_CLOSED);
    invoke_task(entry, &other, 1); /* Unknown still uses native body. */
    assert(atomic_load(&fallback_calls) == 2);
    puts("PASS: task identity, native fallback, closed no-op and in-flight drain");
}

static void test_controller(void *page)
{
    struct tg_controller_state *state =
        (void *)((unsigned char *)page + TG_CONTROLLER_STATE_OFFSET);
    controller_fn entry = (controller_fn)page;
    int owner, foreign, args, request;
    state->owner = (uintptr_t)&owner;
    state->service = (uintptr_t)controller_service;
    state->request = (uintptr_t)&request;
    state->fallback = (uintptr_t)controller_fallback;
    assert(invoke_controller(entry, &owner, &args) == fallback_result);
    assert(tg_controller_arm(state));
    assert(invoke_controller(entry, &foreign, &args) == fallback_result);
    assert(state->phase == TG_CTRL_ARMED && !atomic_load(&service_calls));
    struct controller_call call = {entry, &owner, &args, 0};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_controller, &call) == 0);
    until(&service_entered);
    assert(state->phase == TG_CTRL_ACTIVE);
    assert(tg_controller_close(state) == TG_CLOSE_BUSY);
    assert(state->phase == TG_CTRL_DRAINING);
    atomic_store_explicit(&service_release, 1, memory_order_release);
    assert(pthread_join(thread, NULL) == 0);
    assert(call.result == UINT64_C(0x875eed));
    assert(state->phase == TG_CTRL_CLOSED);
    assert(!tg_controller_arm(state));
    assert(invoke_controller(entry, &owner, &args) == fallback_result);
    assert(atomic_load(&service_calls) == 1);
    puts("PASS: owner-gated controller, foreign fallback and terminal close");
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    void *task_page = read_image(argv[1], TG_TASK_IMAGE_SIZE);
    void *controller_page = read_image(argv[2], TG_CONTROLLER_IMAGE_SIZE);
    test_task(task_page);
    test_controller(controller_page);
    assert(munmap(controller_page, 4096) == 0);
    assert(munmap(task_page, 4096) == 0);
    puts("task gateway host tests passed (no console access)");
    return 0;
}
