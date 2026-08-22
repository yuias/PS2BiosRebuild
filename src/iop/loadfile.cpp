// LOADFILE: the RPC server the EE's `SifLoadModule` talks to.
//
// docs/spec/06-iop-kernel.md IOP-5f and IOP-5g, over spec/03 BOOT-12e's
// request and answer. The module exports nothing: its entry starts a thread
// of priority 88 on a 4 KiB stack, and that thread registers one server,
// `sid 0x80000006`, and loops on its queue for ever. Function 0 reads the
// path out of the request, asks MODLOAD to load and start it, and answers
// the id or error and the module's own return.

#include "module.hpp"
#include "sifrpc.hpp"

#include <stdint.h>

namespace {

using ps2::sifrpc::Queue;
using ps2::sifrpc::Server;

constexpr uint32_t kServerId = 0x80000006;
constexpr uint32_t kRequestBytes = 0x200;
constexpr uint32_t kThreadPriority = 0x58;
constexpr uint32_t kThreadStack = 0x1000;
constexpr uint32_t kThreadAttr = 0x02000000;    // TH_C [header]
constexpr int kIllegalObject = -201;
constexpr uint32_t kFunctionLoad = 0;

struct ThreadParameters {
    uint32_t attr;
    uint32_t option;
    void (*entry)(void *);
    uint32_t stack_size;
    uint32_t priority;
};

extern "C" {
int _import_thbase_create(ThreadParameters *parameters);
int _import_thbase_start(uint32_t id, uint32_t arg);
int _import_thbase_get_id();
int _import_sifcmd_init_rpc(uint32_t mode);
int _import_sifcmd_register_rpc(Server *server, uint32_t sid, void *function, void *buffer,
                                void *cfunction, void *cbuffer, Queue *queue);
int _import_sifcmd_set_rpc_queue(Queue *queue, uint32_t thread_id);
int _import_sifcmd_rpc_loop(Queue *queue);
int _import_modload_load_start(const char *path, uint32_t arglen, const char *args, int *result);
int _import_modload_is_illegal(const char *path);
}

alignas(16) uint32_t request[kRequestBytes / 4];
alignas(16) int32_t answer[4];
Server server;
Queue queue;

// BOOT-12e: the request is `{ arg_len, result, path[252], args[252] }` and
// the answer `{ id | error, modres }`.
void *serve(uint32_t fno, void *buffer, uint32_t) {
    auto *words = static_cast<uint32_t *>(buffer);
    if (fno != kFunctionLoad) {
        return nullptr;                         // IOP-5f: no answer at all
    }
    const auto *path = reinterpret_cast<const char *>(words + 2);
    const auto *args = reinterpret_cast<const char *>(words + 0x41);
    answer[1] = 0;
    if (_import_modload_is_illegal(path) != 0) {
        answer[0] = kIllegalObject;
        return answer;
    }
    int modres = 0;
    answer[0] = _import_modload_load_start(path, words[0], args, &modres);
    answer[1] = modres;
    return answer;
}

void serverThread(void *) {
    _import_sifcmd_init_rpc(0);
    _import_sifcmd_set_rpc_queue(&queue, static_cast<uint32_t>(_import_thbase_get_id()));
    _import_sifcmd_register_rpc(&server, kServerId, reinterpret_cast<void *>(serve),
                                request, nullptr, nullptr, &queue);
    _import_sifcmd_rpc_loop(&queue);
}

}  // namespace

PS2_IMPORTS_BEGIN("thbase\0\0", 0x0101)
PS2_IMPORT(_import_thbase_create, 4)
PS2_IMPORT(_import_thbase_start, 6)
PS2_IMPORT(_import_thbase_get_id, 20)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("sifcmd\0\0", 0x0101)
PS2_IMPORT(_import_sifcmd_init_rpc, 14)
PS2_IMPORT(_import_sifcmd_register_rpc, 17)
PS2_IMPORT(_import_sifcmd_set_rpc_queue, 19)
PS2_IMPORT(_import_sifcmd_rpc_loop, 22)
PS2_IMPORTS_END()

PS2_IMPORTS_BEGIN("modload\0", 0x0101)
PS2_IMPORT(_import_modload_load_start, 7)
PS2_IMPORT(_import_modload_is_illegal, 15)
PS2_IMPORTS_END()

extern "C" {

int _module_start(int, char **) {
    ThreadParameters parameters;
    parameters.attr = kThreadAttr;
    parameters.option = 0;
    parameters.entry = serverThread;
    parameters.stack_size = kThreadStack;
    parameters.priority = kThreadPriority;
    const int id = _import_thbase_create(&parameters);
    if (id < 0) {
        return 1;
    }
    _import_thbase_start(static_cast<uint32_t>(id), 0);
    return 0;                                   // resident
}

}  // extern "C"
