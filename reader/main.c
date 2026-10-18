/* Bounded task-only 13.60 installer. The calling owner thread remains alive
 * through task source drain, terminal controller close and both entry restores.
 * A failed restore retains the aliases and owner for inspection/retry. */
#include "task_probe.h"
#include <ps5/kernel.h>
#include <ps5/fw1360/kdlsym.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef TG_STANDALONE
#include <SDL.h>
#include <SDL2_gfxPrimitives.h>
#endif

#define MAGIC UINT32_C(0x454d4331)
#define WITNESS UINT64_C(0x1360a11a5eedc006)
#define RESERVATION UINT64_C(0x1360a11c5eed8001)
#define SPAN UINT64_C(0x200000)
#define PAGE UINT64_C(0x1000)
#define ALIAS_FLAGS UINT64_C(0x8000000000000007)
#define AD_BITS UINT64_C(0x60)
#define TASK_ORIGINAL UINT64_C(0xcccccc0028147be9)
#define CTRL_ORIGINAL UINT64_C(0xcccccc000437ebe9)
#define NOPS UINT64_C(0x9090909090909090)
#define HV_SUCCESS_MARKER UINT64_C(0xcafedead00001337)

typedef struct {
    char reserved[45];
    char message[3075];
} notify_request_t;
extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *message)
{
    notify_request_t request;
    memset(&request, 0, sizeof request);
    snprintf(request.message, sizeof request.message, "%s", message);
    (void)sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

static uint32_t load_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int slot_is_empty(const unsigned char *slot)
{
    for (unsigned i = 0; i < 32; ++i)
        if (slot[i] != 0 && slot[i] != 0xff) return 0;
    return 1;
}

static const char *code_label(uint32_t code)
{
    switch (code) {
    case 0x80000009: return "AC-in / unexpected shutdown";
    case 0x80800000: return "OS fatal shutdown / kernel panic family";
    case 0x808e0005: return "Host command wait-SIG1 error";
    case 0x808e0006: return "Host reset during command error";
    case 0x808e0109: return "TPM-related (detail unavailable)";
    case 0xb0082108: return "Wi-Fi or Sonics-bus fault";
    case 0xb0088108: return "Rebuild DBI / storage initialization";
    case 0xc0020303: return "Main SoC SB-TSI I2C access failure";
    default: return "UNKNOWN";
    }
}

static int write_decoded_report(const unsigned char *data, const char *path,
                                int summary_only)
{
    int output = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output < 0) return -1;
    uint32_t codes[63] = {0};
    unsigned counts[63] = {0}, unique = 0;
    for (unsigned slot = 0; slot < 63; ++slot) {
        uint32_t code = load_le32(data + 4 + slot * 32);
        unsigned found = unique;
        for (unsigned i = 0; i < unique; ++i)
            if (codes[i] == code) { found = i; break; }
        if (found == unique) { codes[unique] = code; ++unique; }
        ++counts[found];
    }
    char line[384];
    if (!summary_only) {
        int n = snprintf(line, sizeof line,
            "PS5 EMC Dumper decoded report\nfirmware: 13.60\n"
            "WARNING: labels are community-derived; raw latest.bin is authoritative.\n\n"
            "Code summary:\n");
        if (n <= 0 || write(output, line, (size_t)n) != n) { close(output); return -1; }
    }
    for (unsigned i = 0; i < unique; ++i) {
        int n = snprintf(line, sizeof line, "%2u x %08x  %s\n",
                         counts[i], codes[i], code_label(codes[i]));
        if (n <= 0 || write(output, line, (size_t)n) != n) { close(output); return -1; }
    }
    if (!summary_only) {
        static const char records[] = "\nRecords:\n";
        if (write(output, records, sizeof records - 1) != (ssize_t)(sizeof records - 1)) {
            close(output); return -1;
        }
        for (unsigned slot = 0; slot < 63; ++slot) {
            const unsigned char *record = data + 4 + slot * 32;
            uint32_t code = load_le32(record);
            uint32_t sequence_word = load_le32(record + 16);
            uint32_t telemetry = load_le32(record + 20);
            int n = snprintf(line, sizeof line,
                "%02u code=%08x label=%s rtc=%08x power=%08x upcause=%08x "
                "seq-prefix=%04x seq=%04x devpm=%04x soc-temp=%.2fC "
                "pad=%08x,%08x\n",
                slot, code, code_label(code), load_le32(record + 4),
                load_le32(record + 8), load_le32(record + 12),
                sequence_word >> 16, sequence_word & 0xffff, telemetry >> 16,
                (double)(telemetry & 0xffff) / 256.0,
                load_le32(record + 24), load_le32(record + 28));
            if (n <= 0 || (size_t)n >= sizeof line ||
                write(output, line, (size_t)n) != n) { close(output); return -1; }
        }
    }
    return close(output);
}

static int save_log(const unsigned char *data)
{
    (void)mkdir("/data/emc-error-log", 0777);
    int output = open("/data/emc-error-log/latest.bin",
                      O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output < 0) return -1;
    size_t done = 0;
    while (done < TG_EMC_LOG_SIZE) {
        ssize_t n = write(output, data + done, TG_EMC_LOG_SIZE - done);
        if (n <= 0) { close(output); return -1; }
        done += (size_t)n;
    }
    if (close(output)) return -1;

    unsigned nonzero = 0, populated = 0;
    for (unsigned i = 0; i < TG_EMC_LOG_SIZE; ++i) nonzero += data[i] != 0;
    for (unsigned i = 0; i < 63; ++i)
        populated += !slot_is_empty(data + 4 + i * 32);

    output = open("/data/emc-error-log/latest.json",
                  O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output < 0) return -1;
    char json[512];
    int length = snprintf(json, sizeof json,
        "{\n  \"schema\": 1,\n  \"firmware\": \"13.60\",\n"
        "  \"bytes\": %u,\n  \"nonzeroBytes\": %u,\n"
        "  \"headerLe\": \"0x%08x\",\n  \"slotSize\": 32,\n"
        "  \"slotCount\": 63,\n  \"populatedSlots\": %u,\n"
        "  \"rawPath\": \"/data/emc-error-log/latest.bin\",\n"
        "  \"textPath\": \"/data/emc-error-log/latest.txt\"\n}\n",
        TG_EMC_LOG_SIZE, nonzero, load_le32(data), populated);
    if (length <= 0 || (size_t)length >= sizeof json ||
        write(output, json, (size_t)length) != length || close(output)) return -1;

    output = open("/data/homebrew/PPSA99760/emc-result.json",
                  O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output < 0 || write(output, json, (size_t)length) != length ||
        close(output)) return -1;

    output = open("/data/emc-error-log/latest.txt",
                  O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output < 0) return -1;
    char line[256];
    length = snprintf(line, sizeof line,
        "PS5 EMC persistent error log\nfirmware: 13.60\n"
        "bytes: %u\nheader-le: 0x%08x\nslots: 63 x 32 bytes\n\n",
        TG_EMC_LOG_SIZE, load_le32(data));
    if (length <= 0 || write(output, line, (size_t)length) != length) {
        close(output); return -1;
    }
    for (unsigned slot = 0; slot < 63; ++slot) {
        const unsigned char *record = data + 4 + slot * 32;
        int at = snprintf(line, sizeof line, "%02u %s ", slot,
                          slot_is_empty(record) ? "empty    " : "populated");
        for (unsigned word = 0; word < 8 && at > 0 &&
             (size_t)at < sizeof line; ++word)
            at += snprintf(line + at, sizeof line - (size_t)at,
                           "%08x%s", load_le32(record + word * 4),
                           word == 7 ? "\n" : " ");
        if (at <= 0 || (size_t)at >= sizeof line ||
            write(output, line, (size_t)at) != at) { close(output); return -1; }
    }
    if (close(output)) return -1;
    if (write_decoded_report(data, "/data/emc-error-log/decoded.txt", 0)) return -1;
    if (write_decoded_report(data, "/data/homebrew/PPSA99760/emc-decoded.txt", 0)) return -1;
    return write_decoded_report(data, "/data/homebrew/PPSA99760/emc-display.txt", 1);
}

static void save_status(const char *state, unsigned code)
{
    (void)mkdir("/data/emc-error-log", 0777);
    int output = open("/data/emc-error-log/status.json",
                      O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output < 0) return;
    char json[192];
    int length = snprintf(json, sizeof json,
        "{\n  \"schema\": 1,\n  \"state\": \"%s\",\n"
        "  \"code\": %u,\n  \"firmware\": \"13.60\"\n}\n",
        state, code);
    if (length > 0 && (size_t)length < sizeof json)
        (void)write(output, json, (size_t)length);
    (void)close(output);

    /* The foreground title's /data is sandboxed. Mirror only the small status
     * document into its source folder so it can observe it through /app0. */
    output = open("/data/homebrew/PPSA99760/emc-status.json",
                  O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output >= 0) {
        if (length > 0 && (size_t)length < sizeof json)
            (void)write(output, json, (size_t)length);
        (void)close(output);
    }
}

#ifndef TG_STANDALONE
static void ui_text(SDL_Renderer *renderer, int x, int y, const char *text,
                    unsigned char r, unsigned char g, unsigned char b)
{
    (void)stringRGBA(renderer, (Sint16)x, (Sint16)y, text, r, g, b, 255);
}

static void show_ui_and_wait(int saved, const unsigned char *log)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        notify("PS5 EMC Log Reader\nDisplay initialization failed");
        sleep(5);
        return;
    }
    SDL_Window *window = SDL_CreateWindow("PS5 EMC Error Log Reader",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1920, 1080,
        SDL_WINDOW_FULLSCREEN);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1,
        SDL_RENDERER_SOFTWARE) : NULL;
    if (!renderer) {
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        notify("PS5 EMC Log Reader\nCould not open display");
        sleep(5);
        return;
    }
    (void)SDL_RenderSetLogicalSize(renderer, 960, 540);
    SDL_GameController *controller = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) { controller = SDL_GameControllerOpen(i); break; }
    unsigned nonzero = 0;
    for (unsigned i = 0; i < TG_EMC_LOG_SIZE; ++i) nonzero += log[i] != 0;
    char details[128];
    snprintf(details, sizeof details, "RAW LOG: %u BYTES   NONZERO: %u",
             TG_EMC_LOG_SIZE, nonzero);
    int done = 0;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_QUIT ||
                (event.type == SDL_CONTROLLERBUTTONDOWN &&
                 event.cbutton.button == SDL_CONTROLLER_BUTTON_B) ||
                (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
                done = 1;
        SDL_SetRenderDrawColor(renderer, 8, 14, 26, 255);
        SDL_RenderClear(renderer);
        (void)roundedBoxRGBA(renderer, 55, 45, 905, 495, 18, 19, 31, 50, 255);
        (void)boxRGBA(renderer, 55, 45, 65, 495, 46, 204, 113, 255);
        ui_text(renderer, 95, 85, "PS5 EMC ERROR LOG READER", 235, 242, 255);
        ui_text(renderer, 95, 125, "FIRMWARE 13.60", 133, 160, 200);
        ui_text(renderer, 95, 185, "EMC QUERY COMPLETE", 46, 204, 113);
        ui_text(renderer, 95, 225, details, 220, 230, 245);
        ui_text(renderer, 95, 275, saved ?
            "SAVED: /data/emc-error-log/latest.bin" :
            "WARNING: COULD NOT SAVE OUTPUT FILE", saved ? 120 : 255,
            saved ? 220 : 120, saved ? 170 : 120);
        ui_text(renderer, 95, 390,
            "KERNEL WORKER DRAINED - TEMPORARY HOOKS RESTORED", 150, 170, 200);
        ui_text(renderer, 95, 445, "PRESS CIRCLE TO EXIT", 255, 255, 255);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    if (controller) SDL_GameControllerClose(controller);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
#endif

extern const unsigned char tg_task_image[TG_TASK_IMAGE_SIZE];
extern const unsigned char tg_controller_image[TG_CONTROLLER_IMAGE_SIZE];
static volatile uint64_t witness = WITNESS;
struct report {
    uint32_t magic, stage, code, pid;
    uint64_t proc, tid, owner, witness_va, witness_value, alias_va, anchor_va;
    uint64_t pte[4], mapped[4];
    uint64_t task_before, task_after, ctrl_before, ctrl_after;
    uint64_t phase, task_admission, task_identity, task_source;
    uint64_t task_calls, task_marker, task_pending;
    uint64_t request_status, queue, thread_count, enqueue_result, cleanup_count;
    uint64_t ppid_before, ppid_after, other_tid, other_ppid;
    uint64_t task_reservation, controller_reservation;
};
_Static_assert(sizeof(struct report) == 312, "task probe wire report ABI");
struct order {
    uint64_t pte_base, task_frame, controller_frame;
    uint64_t task_stub_frame, controller_stub_frame, ktext;
};
struct worker_state { int command, done; uint64_t tid, ppid; };

static int send_all(int fd, const void *src, size_t size)
{
    const unsigned char *p = src;
    while (size) {
        ssize_t n = send(fd, p, size, 0);
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}
static int recv_all(int fd, void *dst, size_t size)
{
    unsigned char *p = dst;
    while (size) {
        ssize_t n = recv(fd, p, size, 0);
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}
static int read64(uint64_t va, uint64_t *out)
{ return kernel_copyout((intptr_t)va, out, 8); }
static int write64(uint64_t va, uint64_t value)
{ return kernel_copyin(&value, (intptr_t)va, 8); }
static int canonical(uint64_t va)
{ return va >= UINT64_C(0xffff800000000000); }
static int report_stage(int fd, struct report *r, uint32_t stage)
{ r->stage = stage; return send_all(fd, r, sizeof *r); }
static int approved(int fd)
{ unsigned char b = 0; return !recv_all(fd, &b, 1) && b == 1; }

#ifdef TG_STANDALONE
#define TG_PA_MASK UINT64_C(0x000ffffffffff000)
#define TG_PHYS_LIMIT UINT64_C(0x1000000000)

static int walk_user_pmap(uint64_t dmap, uint64_t cr3, uint64_t va,
                          uint64_t expected, uint64_t *leaf_address)
{
    static const unsigned shift[4] = {39, 30, 21, 12};
    uint64_t table = cr3 & TG_PA_MASK;
    for (unsigned level = 0; level < 4; ++level) {
        uint64_t address = dmap + table + (((va >> shift[level]) & 0x1ff) * 8);
        uint64_t value = 0;
        if (read64(address, &value) || !(value & 1)) return -1;
        if (level == 3) {
            uint64_t physical = (value & TG_PA_MASK) | (va & 0xfff);
            uint64_t observed = 0;
            if (physical >= TG_PHYS_LIMIT || read64(dmap + physical, &observed) ||
                observed != expected) return -1;
            if (leaf_address) *leaf_address = address;
            return 0;
        }
        if ((level == 1 || level == 2) && (value & 0x80)) return -1;
        table = value & TG_PA_MASK;
    }
    return -1;
}

static int discover_alias_pte(const struct report *r, uint64_t *pte_base)
{
    uint64_t vmspace = 0;
    if (read64(r->proc + 0x200, &vmspace) || !canonical(vmspace)) return -1;
    unsigned matches = 0;
    uint64_t selected = 0;
    for (uint64_t offset = 0x100; offset < 0x400; offset += 8) {
        uint64_t pml4 = 0, cr3 = 0;
        if (read64(vmspace + offset, &pml4) || read64(vmspace + offset + 8, &cr3) ||
            !canonical(pml4) || (pml4 & 0xfff) || !cr3 || (cr3 & 0xfff) ||
            cr3 >= TG_PHYS_LIMIT || pml4 < cr3) continue;
        uint64_t dmap = pml4 - cr3, anchor_leaf = 0;
        if (!canonical(dmap) || (dmap & UINT64_C(0xffffffff)) ||
            walk_user_pmap(dmap, cr3, r->witness_va, WITNESS, NULL) ||
            walk_user_pmap(dmap, cr3, r->anchor_va, 0x5a, &anchor_leaf)) continue;
        uint64_t base = anchor_leaf - 511 * 8;
        int empty = 1;
        for (unsigned i = 0; i < 384; ++i) {
            uint64_t value = 0;
            if (read64(base + i * 8, &value) || value) { empty = 0; break; }
        }
        if (!empty) continue;
        selected = base;
        ++matches;
    }
    if (matches != 1) return -1;
    *pte_base = selected;
    return 0;
}

static int kernel_mapping(uint64_t kbase, uint64_t va, uint64_t *frame)
{
    uint32_t rec = 0, pml4 = 0, pdp = 0;
    if (kernel_copyout((intptr_t)(kbase + 0x3b47e4c), &rec, 4) ||
        kernel_copyout((intptr_t)(kbase + 0x3b49294), &pml4, 4) ||
        kernel_copyout((intptr_t)(kbase + 0x3b49298), &pdp, 4) ||
        rec >= 512 || pml4 >= 512 || pdp >= 512) return -1;
    uint64_t dmap = UINT64_C(0xffff800000000000) |
                    (uint64_t)pml4 << 39 | (uint64_t)pdp << 30;
    uint64_t recursive = UINT64_C(0xffff800000000000) |
                         (uint64_t)rec << 39 | (uint64_t)rec << 30;
    uint64_t pde_address = recursive + 8 * ((va >> 21) & 0x7ffffff);
    uint64_t pde = 0, leaf = 0;
    if (read64(pde_address, &pde) || !(pde & 1)) return -1;
    if (pde & 0x80) {
        leaf = pde;
        *frame = ((leaf & TG_PA_MASK) & ~UINT64_C(0x1fffff)) |
                 (va & UINT64_C(0x1ff000));
    } else {
        uint64_t leaf_address = dmap + (pde & TG_PA_MASK) + ((va >> 9) & 0xff8);
        if (read64(leaf_address, &leaf) || !(leaf & 1)) return -1;
        *frame = leaf & TG_PA_MASK;
    }
    uint64_t cache_bits = (pde & 0x80) ? UINT64_C(0x1018) : UINT64_C(0x98);
    return (leaf & ((UINT64_C(1) << 63) | cache_bits)) ||
           !*frame || *frame >= TG_PHYS_LIMIT ? -1 : 0;
}

static int standalone_order(struct report *r, struct order *command)
{
    memset(command, 0, sizeof *command);
    command->ktext = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
    if (!canonical(command->ktext) || (command->ktext & 0xfff) ||
        discover_alias_pte(r, &command->pte_base) ||
        kernel_mapping(command->ktext, command->ktext + TG_TASK_OFFSET,
                       &command->task_frame) ||
        kernel_mapping(command->ktext, command->ktext + TG_CONTROLLER_OFFSET,
                       &command->controller_frame) ||
        kernel_mapping(command->ktext, command->ktext + TG_TASK_ENTRY_OFFSET,
                       &command->task_stub_frame) ||
        kernel_mapping(command->ktext, command->ktext + TG_GETPPID_ENTRY_OFFSET,
                       &command->controller_stub_frame)) return -1;
    return 0;
}
#endif
static int owner_matches(const struct report *r)
{
    long tid = 0;
    uint64_t proc = 0;
    uint32_t kernel_tid = 0;
    return !syscall(432, &tid) && (uint64_t)tid == r->tid &&
        !read64(r->owner + 8, &proc) && proc == r->proc &&
        !kernel_copyout((intptr_t)(r->owner + 0x9c), &kernel_tid, 4) &&
        kernel_tid == r->tid;
}
static void snapshot(struct report *r, const struct tg_controller_state *controller,
                     const struct tg_controller_request *request,
                     const struct tg_task_state *task)
{
    r->phase = __atomic_load_n(&controller->phase, __ATOMIC_ACQUIRE);
    r->task_admission = __atomic_load_n(&task->admission, __ATOMIC_ACQUIRE);
    r->task_identity = __atomic_load_n(&task->identity, __ATOMIC_ACQUIRE);
    r->task_source = task->source;
    r->task_calls = task->calls;
    r->task_marker = task->marker;
    r->task_pending = task->pending;
    r->request_status = request->status;
    r->queue = request->queue;
    r->thread_count = request->thread_count;
    r->enqueue_result = request->enqueue_result;
    r->cleanup_count = request->cleanup_count;
}
static void *other_thread(void *arg)
{
    struct worker_state *w = arg;
    long tid = 0;
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    if (!syscall(432, &tid)) w->tid = (uint64_t)tid;
    for (;;) {
        int command = __atomic_load_n(&w->command, __ATOMIC_ACQUIRE);
        if (command == 2) break;
        if (command == 1) {
            w->ppid = (uint64_t)getppid();
            __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
            break;
        }
        usleep(1000);
    }
    return NULL;
}
static int clean_pte(uint64_t address, uint64_t expected, uint64_t *readback)
{
    uint64_t current;
    if (read64(address, &current) ||
        (current && (current & ~AD_BITS) != expected)) return -1;
    if (current && write64(address, 0)) return -1;
    return read64(address, readback) || *readback ? -1 : 0;
}
static int candidate(uint64_t entry, uint64_t destination, uint64_t *out)
{
    int64_t delta = (int64_t)destination - (int64_t)(entry + 5);
    if (delta < INT32_MIN || delta > INT32_MAX || (entry & 7)) return -1;
    unsigned char slot[8] = {0xe9, 0, 0, 0, 0, 0xcc, 0xcc, 0xcc};
    int32_t relative = (int32_t)delta;
    memcpy(slot + 1, &relative, 4);
    memcpy(out, slot, 8);
    return 0;
}
static int page_is_nops(uint64_t address, const volatile unsigned char *alias)
{
    unsigned char page[PAGE];
    if (kernel_copyout((intptr_t)address, page, PAGE)) return 0;
    for (unsigned i = 0; i < PAGE; ++i)
        if (page[i] != 0x90 || alias[i] != 0x90) return 0;
    return 1;
}
static int page_matches(uint64_t address, const void *alias)
{
    unsigned char page[PAGE];
    return !kernel_copyout((intptr_t)address, page, PAGE) &&
           !memcmp(page, alias, PAGE);
}
static int invoke(struct report *r, struct tg_controller_state *state,
                  struct tg_controller_request *request, uint64_t command)
{
    if (!owner_matches(r)) return -1;
    request->command = command;
    request->status = UINT64_MAX;
    request->original_status = UINT64_MAX;
    request->original_ppid = UINT64_MAX;
    if (!tg_controller_arm(state)) return -1;
    r->ppid_after = (uint64_t)getppid();
    return r->ppid_after == r->ppid_before &&
           __atomic_load_n(&state->phase, __ATOMIC_ACQUIRE) == TG_CTRL_IDLE &&
           request->status == 0 &&
           tg_controller_original_ok(request, r->ppid_before) ? 0 : -1;
}

int main(void)
{
    if ((kernel_get_fw_version() & UINT32_C(0xffff0000)) != UINT32_C(0x13600000))
        return 1;
    uint64_t hv_marker = 0;
    if (read64(ktext(UINT64_C(0x2d39000) + 0x440), &hv_marker) ||
        hv_marker != HV_SUCCESS_MARKER) {
        notify("PS5 EMC Log Reader\nByepervisor Reborn is not active");
        return 2;
    }
    notify("PS5 EMC Log Reader\nReading persistent EMC faults...");
    save_status("reading", 0);
    int fd = -1, result = 1, task_published = 0, ctrl_published = 0;
    int state_ready = 0, worker_started = 0, source_drained = 0;
    void *arena = MAP_FAILED;
    uint64_t pte[4] = {0}, mapped[4] = {0};
    uint64_t task_va = 0, ctrl_va = 0, task_entry = 0, ctrl_entry = 0;
    uint64_t task_candidate = 0, ctrl_candidate = 0, expected = 0;
    struct tg_task_state *task = NULL;
    struct tg_controller_state *state = NULL;
    struct tg_controller_request *request = NULL;
    struct worker_state worker = {0};
    pthread_t thread;
    struct report r = {.magic = MAGIC};
    unsigned char emc_log[TG_EMC_LOG_SIZE] = {0};
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    arena = mmap(NULL, 2 * SPAN, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) goto out;
    uintptr_t span = ((uintptr_t)arena + SPAN - 1) & ~(SPAN - 1);
    volatile uint8_t *anchor = (volatile uint8_t *)(span + SPAN - PAGE);
    *anchor = 0x5a;
#ifndef TG_STANDALONE
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) goto out;
    struct timeval timeout = {.tv_sec = 30};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    struct sockaddr_in peer = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(7805);
    peer.sin_addr.s_addr = htonl(0xc0a83201);
    if (connect(fd, (struct sockaddr *)&peer, sizeof peer)) goto out;
#endif
    long tid = 0;
    r.pid = (uint32_t)getpid();
    r.proc = (uint64_t)kernel_get_proc((pid_t)r.pid);
    if (syscall(432, &tid) || tid <= 0) goto out;
    r.tid = (uint64_t)tid;
    r.owner = (uint64_t)kernel_get_proc_thread((pid_t)r.pid, (pid_t)tid);
    if (!canonical(r.owner) || !canonical(r.proc) || !owner_matches(&r)) goto out;
    r.witness_va = (uintptr_t)&witness;
    r.witness_value = witness;
    r.alias_va = span;
    r.anchor_va = (uintptr_t)anchor;
    r.ppid_before = (uint64_t)getppid();
    save_status("reading", 101);
    if (pthread_create(&thread, NULL, other_thread, &worker)) goto out;
    worker_started = 1;
    struct order command;
#ifdef TG_STANDALONE
    if (standalone_order(&r, &command)) { r.code = 24; goto cleanup; }
#else
    if (report_stage(fd, &r, 1)) goto cleanup;
    if (recv_all(fd, &command, sizeof command)) goto cleanup;
    if (!command.pte_base) { result = 0; goto cleanup; }
#endif
    if (!canonical(command.pte_base) || (command.pte_base & (PAGE - 1)) ||
        command.pte_base > UINT64_MAX - 24 ||
        !canonical(command.ktext) || (command.ktext & 0xfff) ||
        command.ktext > UINT64_MAX - TG_CONTROLLER_OFFSET - PAGE ||
        !command.task_frame || !command.controller_frame ||
        !command.task_stub_frame || !command.controller_stub_frame ||
        (command.task_frame & 0xfff) || (command.controller_frame & 0xfff) ||
        (command.task_stub_frame & 0xfff) ||
        (command.controller_stub_frame & 0xfff) ||
        command.task_frame >= UINT64_C(0x1000000000) ||
        command.controller_frame >= UINT64_C(0x1000000000) ||
        command.task_stub_frame >= UINT64_C(0x1000000000) ||
        command.controller_stub_frame >= UINT64_C(0x1000000000) ||
        command.task_frame == command.controller_frame ||
        command.task_frame == command.task_stub_frame ||
        command.task_frame == command.controller_stub_frame ||
        command.controller_frame == command.task_stub_frame ||
        command.controller_frame == command.controller_stub_frame) {
        r.code = 1; goto cleanup;
    }
    task_va = command.ktext + TG_TASK_OFFSET;
    ctrl_va = command.ktext + TG_CONTROLLER_OFFSET;
    task_entry = command.ktext + TG_TASK_ENTRY_OFFSET;
    ctrl_entry = command.ktext + TG_GETPPID_ENTRY_OFFSET;
    for (unsigned i = 0; i < 4; ++i) {
        pte[i] = command.pte_base + 8 * i;
        r.pte[i] = pte[i];
        if (read64(pte[i], &r.mapped[i]) || r.mapped[i]) { r.code = 2; goto cleanup; }
    }
    if (read64(task_entry, &r.task_before) || r.task_before != TASK_ORIGINAL ||
        read64(ctrl_entry, &r.ctrl_before) || r.ctrl_before != CTRL_ORIGINAL) {
        r.code = 2; goto cleanup;
    }
    mapped[0] = command.task_frame | ALIAS_FLAGS;
    mapped[1] = command.controller_frame | ALIAS_FLAGS;
    mapped[2] = command.task_stub_frame | ALIAS_FLAGS;
    mapped[3] = command.controller_stub_frame | ALIAS_FLAGS;
    for (unsigned i = 0; i < 4; ++i) {
        if (write64(pte[i], mapped[i]) || read64(pte[i], &r.mapped[i]) ||
            r.mapped[i] != mapped[i]) { r.code = 3; goto cleanup; }
    }
    save_status("reading", 102);
#ifndef TG_STANDALONE
    if (report_stage(fd, &r, 2) || !approved(fd)) goto cleanup;
#endif

    if (!page_is_nops(task_va, (const volatile unsigned char *)span) ||
        !page_is_nops(ctrl_va, (const volatile unsigned char *)(span + PAGE)) ||
        *(volatile uint64_t *)(span + 2 * PAGE + (TG_TASK_ENTRY_OFFSET & 0xfff)) !=
            TASK_ORIGINAL ||
        *(volatile uint64_t *)(span + 3 * PAGE + (TG_GETPPID_ENTRY_OFFSET & 0xfff)) !=
            CTRL_ORIGINAL) { r.code = 4; goto cleanup; }
    expected = NOPS;
    r.task_reservation = RESERVATION ^ r.owner;
    r.controller_reservation = (RESERVATION + 1) ^ r.owner;
    if (!r.task_reservation || r.task_reservation == NOPS ||
        !r.controller_reservation || r.controller_reservation == NOPS ||
        r.task_reservation == r.controller_reservation) {
        r.code = 5; goto cleanup;
    }
    if (!__atomic_compare_exchange_n((uint64_t *)(span + TG_TASK_CLAIM_OFFSET),
        &expected, r.task_reservation, 0, __ATOMIC_SEQ_CST,
        __ATOMIC_SEQ_CST)) { r.code = 5; goto cleanup; }
    expected = NOPS;
    if (!__atomic_compare_exchange_n(
            (uint64_t *)(span + PAGE + TG_CONTROLLER_CLAIM_OFFSET), &expected,
            r.controller_reservation, 0, __ATOMIC_SEQ_CST,
            __ATOMIC_SEQ_CST)) { r.code = 5; goto cleanup; }
    save_status("reading", 103);
    for (unsigned i = 0; i < TG_TASK_IMAGE_SIZE; ++i)
        ((volatile unsigned char *)span)[i] = tg_task_image[i];
    for (unsigned i = 0; i < TG_CONTROLLER_IMAGE_SIZE; ++i)
        ((volatile unsigned char *)(span + PAGE))[i] = tg_controller_image[i];
    task = (void *)(span + TG_TASK_STATE_OFFSET);
    state = (void *)(span + PAGE + TG_CONTROLLER_STATE_OFFSET);
    request = (void *)(span + PAGE + TG_CONTROLLER_REQUEST_OFFSET);
    task->fallback = command.ktext + TG_TASK_BODY_OFFSET;
    task->icc_query = command.ktext + UINT64_C(0xa3f4f0);
    volatile unsigned char *message = (volatile unsigned char *)span +
                                      TG_TASK_MESSAGE_OFFSET;
    memset((void *)message, 0, TG_TASK_MESSAGE_SIZE);
    message[1] = 2;
    message[2] = 7;
    message[8] = 0x20;
    state->owner = r.owner;
    state->service = ctrl_va + 0x100;
    state->request = ctrl_va + TG_CONTROLLER_REQUEST_OFFSET;
    state->fallback = command.ktext + TG_GETPPID_BODY_OFFSET;
    request->ktext = command.ktext;
    request->task_entry = task_entry;
    request->task_state = task_va + TG_TASK_STATE_OFFSET;
    request->controller_fallback = command.ktext + TG_GETPPID_BODY_OFFSET;
    request->source = ctrl_va + TG_CONTROLLER_TASK_OFFSET;
    request->identity = ctrl_va + TG_CONTROLLER_REQUEST_OFFSET;
    state_ready = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!page_matches(task_va, (const void *)span) ||
        !page_matches(ctrl_va, (const void *)(span + PAGE)) ||
        !owner_matches(&r)) { r.code = 6; goto cleanup; }
    save_status("reading", 104);
    snapshot(&r, state, request, task);
#ifndef TG_STANDALONE
    if (report_stage(fd, &r, 3) || !approved(fd)) goto cleanup;
#endif

    if (candidate(task_entry, task_va, &task_candidate) ||
        candidate(ctrl_entry, ctrl_va, &ctrl_candidate)) { r.code = 7; goto cleanup; }
    uint64_t *task_alias = (void *)(span + 2 * PAGE + (TG_TASK_ENTRY_OFFSET & 0xfff));
    uint64_t *ctrl_alias = (void *)(span + 3 * PAGE + (TG_GETPPID_ENTRY_OFFSET & 0xfff));
    expected = TASK_ORIGINAL;
    if (!owner_matches(&r) || !__atomic_compare_exchange_n(task_alias, &expected,
            task_candidate, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        r.code = 7; goto cleanup;
    }
    task_published = 1;
    if (read64(task_entry, &r.task_after) || r.task_after != task_candidate) {
        r.code = 8; goto cleanup;
    }
    save_status("reading", 105);
#ifndef TG_STANDALONE
    if (report_stage(fd, &r, 4) || !approved(fd)) goto cleanup;
#endif
    expected = CTRL_ORIGINAL;
    if (!owner_matches(&r) || !__atomic_compare_exchange_n(ctrl_alias, &expected,
            ctrl_candidate, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        r.code = 9; goto cleanup;
    }
    ctrl_published = 1;
    if (read64(ctrl_entry, &r.ctrl_after) || r.ctrl_after != ctrl_candidate) {
        r.code = 10; goto cleanup;
    }
    save_status("reading", 106);
#ifndef TG_STANDALONE
    if (report_stage(fd, &r, 5) || !approved(fd)) goto cleanup;
#endif

    /* A foreign thread must not consume the one owner request. */
    save_status("reading", 107);
    if ((uint64_t)getppid() != r.ppid_before || !owner_matches(&r) ||
        !tg_controller_arm(state)) { r.code = 11; goto cleanup; }
    __atomic_store_n(&worker.command, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 5000 &&
         !__atomic_load_n(&worker.done, __ATOMIC_ACQUIRE); ++i) usleep(1000);
    r.other_tid = worker.tid;
    r.other_ppid = worker.ppid;
    if (!__atomic_load_n(&worker.done, __ATOMIC_ACQUIRE) ||
        !r.other_tid || r.other_tid == r.tid ||
        r.other_ppid != r.ppid_before || state->phase != TG_CTRL_ARMED) {
        r.code = 12; goto cleanup;
    }
    request->command = TG_CMD_CREATE;
    request->status = UINT64_MAX;
    request->original_status = UINT64_MAX;
    request->original_ppid = UINT64_MAX;
    r.ppid_after = (uint64_t)getppid();
    if (r.ppid_after != r.ppid_before || request->status ||
        !tg_controller_original_ok(request, r.ppid_before) ||
        state->phase != TG_CTRL_IDLE || !canonical(request->queue) ||
        request->thread_count != 1) { r.code = 13; goto cleanup; }
    snapshot(&r, state, request, task);
#ifndef TG_STANDALONE
    if (report_stage(fd, &r, 6) || !approved(fd)) goto cleanup;
#endif
    save_status("reading", 108);
    if (invoke(&r, state, request, TG_CMD_RUN)) { r.code = 14; goto cleanup; }
    save_status("reading", 109);
    snapshot(&r, state, request, task);
    if (r.task_calls != 1 || r.task_marker != 0 ||
        r.task_pending != 1 || r.task_identity != request->identity ||
        r.task_source != request->source || r.task_admission != 0) {
        r.code = 15; goto cleanup;
    }
    memcpy(emc_log, (const void *)(span + TG_TASK_MESSAGE_OFFSET + 0xc),
           sizeof emc_log);
#ifndef TG_STANDALONE
    if (report_stage(fd, &r, 7) || send_all(fd,
            (const void *)(span + TG_TASK_MESSAGE_OFFSET + 0xc),
            TG_EMC_LOG_SIZE) || !approved(fd)) goto cleanup;
#endif
    save_status("reading", 110);
    if (invoke(&r, state, request, TG_CMD_CLOSE)) { r.code = 16; goto cleanup; }
    save_status("reading", 111);
    snapshot(&r, state, request, task);
    if (r.queue || r.thread_count || r.task_admission != TG_TASK_CLOSED ||
        r.task_calls != 1 || r.cleanup_count != 1) { r.code = 17; goto cleanup; }
    source_drained = 1;
#ifndef TG_STANDALONE
    if (report_stage(fd, &r, 8) || !approved(fd)) goto cleanup;
#endif
    if (!tg_controller_arm(state) ||
        tg_controller_close(state) != TG_CLOSE_DONE ||
        tg_controller_arm(state) || (uint64_t)getppid() != r.ppid_before) {
        r.code = 18; goto cleanup;
    }
    snapshot(&r, state, request, task);
#ifndef TG_STANDALONE
    if (report_stage(fd, &r, 9) || !approved(fd)) goto cleanup;
#endif
    result = 0;

cleanup:
    /* Neither reservation is reclaimed. Failed source cleanup or entry
     * restoration retains this process, its pmap and all referenced pages. */
    if (state_ready && request->queue && ctrl_published &&
        state->phase != TG_CTRL_CLOSED &&
        invoke(&r, state, request, TG_CMD_CLEANUP) && !r.code) r.code = 19;
    if (state_ready && !request->queue) {
        source_drained = 1;
        tg_task_close(task);
    }
    if (state_ready && !source_drained && !r.code) r.code = 19;
    if (state_ready && source_drained) {
        enum tg_close_result closed;
        do {
            closed = tg_controller_close(state);
            if (closed == TG_CLOSE_BUSY) usleep(1000);
        } while (closed == TG_CLOSE_BUSY);
        if (closed != TG_CLOSE_DONE) r.code = 20;
        snapshot(&r, state, request, task);
    }
    if (source_drained && task_published) {
        expected = task_candidate;
        if (!__atomic_compare_exchange_n(
            (uint64_t *)(span + 2 * PAGE + (TG_TASK_ENTRY_OFFSET & 0xfff)),
            &expected, TASK_ORIGINAL, 0, __ATOMIC_SEQ_CST,
            __ATOMIC_SEQ_CST)) r.code = 21;
    }
    if (task_entry && (read64(task_entry, &r.task_after) ||
                       r.task_after != TASK_ORIGINAL)) r.code = 21;
    if (source_drained && state_ready && state->phase == TG_CTRL_CLOSED &&
        r.task_after == TASK_ORIGINAL && ctrl_published) {
        expected = ctrl_candidate;
        if (!__atomic_compare_exchange_n(
            (uint64_t *)(span + 3 * PAGE + (TG_GETPPID_ENTRY_OFFSET & 0xfff)),
            &expected, CTRL_ORIGINAL, 0, __ATOMIC_SEQ_CST,
            __ATOMIC_SEQ_CST)) r.code = 22;
    }
    if (ctrl_entry && (read64(ctrl_entry, &r.ctrl_after) ||
                       r.ctrl_after != CTRL_ORIGINAL)) r.code = 22;
    if ((!state_ready || (source_drained && state->phase == TG_CTRL_CLOSED)) &&
        (!task_entry || r.task_after == TASK_ORIGINAL) &&
        (!ctrl_entry || r.ctrl_after == CTRL_ORIGINAL)) {
        for (int i = 3; i >= 0; --i)
            if (pte[i] && clean_pte(pte[i], mapped[i], &r.mapped[i])) r.code = 23;
    }
    if (fd >= 0) (void)report_stage(fd, &r, 10);
    if (fd >= 0) (void)approved(fd);
#ifdef TG_STANDALONE
    /* Cleanup deliberately retains the process while any temporary mapping or
     * entry hook is unsafe. Publish the cause before that potentially
     * unbounded safety wait so the foreground title never appears hung. */
    if (r.code) save_status("failed", r.code);
#endif
    for (;;) {
        uint64_t a[4] = {1, 1, 1, 1}, task_word = 0, ctrl_word = 0;
        int safe = 1;
        for (unsigned i = 0; i < 4; ++i)
            if (pte[i] && (read64(pte[i], &a[i]) || a[i])) safe = 0;
        if (task_entry && (read64(task_entry, &task_word) ||
                           task_word != TASK_ORIGINAL)) safe = 0;
        if (ctrl_entry && (read64(ctrl_entry, &ctrl_word) ||
                           ctrl_word != CTRL_ORIGINAL)) safe = 0;
        if (state_ready && (!source_drained || request->queue ||
                            state->phase != TG_CTRL_CLOSED)) safe = 0;
        if (safe) break;
        sleep(5);
    }
    if (r.code) result = 4;
out:
    if (worker_started) {
        __atomic_store_n(&worker.command, 2, __ATOMIC_RELEASE);
        (void)pthread_join(thread, NULL);
    }
    if (fd >= 0) close(fd);
    if (arena != MAP_FAILED) munmap(arena, 2 * SPAN);
    if (!result) {
        int saved = !save_log(emc_log);
        save_status(saved ? "complete" : "save-failed", saved ? 0 : 25);
#ifdef TG_STANDALONE
        notify(saved ? "PS5 EMC Log Reader\nPersistent fault log saved" :
                       "PS5 EMC Log Reader\nQuery succeeded but report save failed");
#else
        show_ui_and_wait(saved, emc_log);
        notify("PS5 EMC Log Reader\nClosed safely");
#endif
    }
    else save_status("failed", r.code ? r.code : (unsigned)result);
    return result;
}
