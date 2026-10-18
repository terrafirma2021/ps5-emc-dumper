#define _GNU_SOURCE
#include "../task_probe.h"
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define AREA 0x1000000
static unsigned char *base;
static int creates, starts, enqueues, drains, frees, fail_enqueue, fail_start;
static int fail_original;
static struct tg_controller_request *request;
extern uint64_t tg_controller_service(void *, void *, struct tg_controller_request *);

struct mock_queue { unsigned char bytes[0x60]; uint32_t thread_count; };
static void *mock_create(const char *name, int flags, void (*wake)(void *),
                         void *context)
{
    assert(!strcmp(name, "ps5_emc_log") && flags == 2 &&
           (uintptr_t)wake == (uintptr_t)(base + 0x261418) &&
           context == &request->queue);
    ++creates;
    return calloc(1, sizeof(struct mock_queue));
}
static int mock_start(void **queue, int count, int priority, const char *name, ...)
{
    assert((uintptr_t)*queue == request->queue && count == 1 && priority == 8 &&
            !strcmp(name, "ps5_emc_log"));
    ++starts;
    ((struct mock_queue *)*queue)->thread_count = fail_start ? 0 : 1;
    return 0;
}
__attribute__((no_sanitize("function")))
static int mock_enqueue(void *queue, struct tg_task_source *source)
{
    assert((uintptr_t)queue == request->queue &&
           (uintptr_t)source == request->source);
    struct tg_task_state *state = (void *)(uintptr_t)request->task_state;
    /* The exact identity/source pair and function pointer are published before
     * the source can reach this enqueue routine. */
    assert(state->identity == request->identity && state->source == request->source &&
           (uintptr_t)source->function == request->task_entry &&
           (uintptr_t)source->context == request->identity);
    ++enqueues;
    if (fail_enqueue) return 99;
    source->function(source->context, 1);
    return 0;
}
static void mock_drain(void *queue, struct tg_task_source *source)
{
    assert((uintptr_t)queue == request->queue &&
           (uintptr_t)source == request->source);
    ++drains;
}
static void mock_free(void *queue)
{
    assert((uintptr_t)queue == request->queue);
    ++frees;
    free(queue);
}
struct mock_thread {
    unsigned char before_retval[TG_GETPPID_RETVAL_OFFSET];
    uint64_t retval;
};
_Static_assert(offsetof(struct mock_thread, retval) == TG_GETPPID_RETVAL_OFFSET,
               "captured getppid retval offset");
static int mock_original(void *thread, void *args)
{
    assert(thread && args);
    ((struct mock_thread *)thread)->retval = 88;
    return fail_original ? 5 : 0;
}
static void jump_to(uintptr_t address, uintptr_t target)
{
    unsigned char code[12] = {0x48, 0xb8};
    memcpy(code + 2, &target, 8);
    code[10] = 0xff;
    code[11] = 0xe0;
    memcpy((void *)address, code, sizeof code);
}
static void task_branch(void)
{
    int64_t delta = (int64_t)(base + TG_TASK_OFFSET) -
                    (int64_t)(base + TG_TASK_ENTRY_OFFSET + 5);
    assert(delta >= INT32_MIN && delta <= INT32_MAX);
    int32_t rel = (int32_t)delta;
    unsigned char code[8] = {0xe9, 0, 0, 0, 0, 0xcc, 0xcc, 0xcc};
    memcpy(code + 1, &rel, 4);
    memcpy(base + TG_TASK_ENTRY_OFFSET, code, 8);
}
static int mock_icc_query(void *input, void *output)
{
    assert(input == output);
    unsigned char *message = input;
    assert(message[1] == 2 && message[2] == 7 && message[8] == 0x20);
    message[0xc] = 0xa5;
    return 0;
}
static void reset_fixture(void)
{
    memset(base + TG_TASK_OFFSET, 0x90, 4096);
    memset(base + TG_CONTROLLER_OFFSET, 0, 4096);
    struct tg_task_state *state = (void *)(base + TG_TASK_OFFSET + TG_TASK_STATE_OFFSET);
    memset(state, 0, sizeof *state);
    request = (void *)(base + TG_CONTROLLER_OFFSET + TG_CONTROLLER_REQUEST_OFFSET);
    request->ktext = (uintptr_t)base;
    request->task_entry = (uintptr_t)(base + TG_TASK_ENTRY_OFFSET);
    request->task_state = (uintptr_t)state;
    request->controller_fallback = (uintptr_t)(base + TG_GETPPID_BODY_OFFSET);
    request->source = (uintptr_t)(base + TG_CONTROLLER_OFFSET + TG_CONTROLLER_TASK_OFFSET);
    request->identity = (uintptr_t)request;
    state->icc_query = (uintptr_t)(base + 0xa3f4f0);
    unsigned char *message = base + TG_TASK_OFFSET + TG_TASK_MESSAGE_OFFSET;
    memset(message, 0, TG_TASK_MESSAGE_SIZE);
    message[1] = 2;
    message[2] = 7;
    message[8] = 0x20;
    creates = starts = enqueues = drains = frees = fail_enqueue = fail_start = 0;
    fail_original = 0;
}
static uint64_t command(uint64_t action)
{
    struct mock_thread thread = {0};
    int args;
    request->command = action;
    request->status = UINT64_MAX;
    request->original_status = UINT64_MAX;
    request->original_ppid = UINT64_MAX;
    uint64_t result = tg_controller_service(&thread, &args, request);
    assert(result == 0 && thread.retval == 88 &&
           tg_controller_original_ok(request, 88));
    return request->status;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    base = mmap(NULL, AREA, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(base != MAP_FAILED && !((uintptr_t)base & 0xfff));
    FILE *image = fopen(argv[1], "rb");
    assert(image);
    assert(fread(base + TG_TASK_OFFSET, 1, TG_TASK_IMAGE_SIZE, image) == TG_TASK_IMAGE_SIZE);
    assert(fgetc(image) == EOF && !fclose(image));
    task_branch();
    jump_to((uintptr_t)(base + 0x4ea760), (uintptr_t)mock_create);
    jump_to((uintptr_t)(base + 0x4eb820), (uintptr_t)mock_start);
    jump_to((uintptr_t)(base + 0x4ea9b0), (uintptr_t)mock_enqueue);
    jump_to((uintptr_t)(base + 0x4eb3c0), (uintptr_t)mock_drain);
    jump_to((uintptr_t)(base + 0x4ea8b0), (uintptr_t)mock_free);
    jump_to((uintptr_t)(base + TG_GETPPID_BODY_OFFSET), (uintptr_t)mock_original);
    jump_to((uintptr_t)(base + 0xa3f4f0), (uintptr_t)mock_icc_query);
    base[TG_TASK_BODY_OFFSET] = 0xc3; /* original task no-op */

    reset_fixture();
    image = fopen(argv[1], "rb");
    assert(image);
    assert(fread(base + TG_TASK_OFFSET, 1, TG_TASK_IMAGE_SIZE, image) == TG_TASK_IMAGE_SIZE);
    assert(!fclose(image));
    struct tg_task_state *state =
        (void *)(base + TG_TASK_OFFSET + TG_TASK_STATE_OFFSET);
    state->icc_query = (uintptr_t)(base + 0xa3f4f0);
    unsigned char *message = base + TG_TASK_OFFSET + TG_TASK_MESSAGE_OFFSET;
    message[1] = 2;
    message[2] = 7;
    message[8] = 0x20;
    assert(command(TG_CMD_CREATE) == 0);
    assert(request->queue && request->thread_count == 1);
    assert(command(TG_CMD_RUN) == 0);
    state = (void *)(uintptr_t)request->task_state;
    assert(state->calls == 1 && state->marker == 0 &&
           state->seen_identity == request->identity && enqueues == 1 && drains == 1);
    assert(command(TG_CMD_CLOSE) == 0);
    assert(state->admission == TG_TASK_CLOSED && state->calls == 1 &&
           !request->queue && enqueues == 2 && drains == 3 && frees == 1 &&
           request->cleanup_count == 1);
    puts("PASS: compiled controller source creates, registers, enqueues, drains and closes");

    reset_fixture();
    fail_start = 1;
    assert(command(TG_CMD_CREATE) == 4);
    assert(!request->queue && creates == 1 && starts == 1 && frees == 1);
    puts("PASS: zero-worker startup is rejected before source publication");

    reset_fixture();
    assert(command(TG_CMD_CREATE) == 0);
    fail_enqueue = 1;
    assert(command(TG_CMD_RUN) == 7);
    assert(request->queue && state->identity == request->identity);
    fail_enqueue = 0;
    assert(command(TG_CMD_CLEANUP) == 0);
    assert(!request->queue && state->admission == TG_TASK_CLOSED && frees == 1);
    puts("PASS: enqueue failure retains ownership until retryable cleanup");

    reset_fixture();
    fail_original = 1;
    struct mock_thread td = {0};
    int args = 0;
    request->command = TG_CMD_CLEANUP;
    assert(tg_controller_service(&td, &args, request) == 5);
    assert(request->status == 0 && td.retval == 88 &&
           request->original_status == 5 && request->original_ppid == 88 &&
           !tg_controller_original_ok(request, 88));
    puts("PASS: original syscall status and thread retval stay distinct");
    assert(munmap(base, AREA) == 0);
    puts("controller source host tests passed (no console access)");
    return 0;
}
