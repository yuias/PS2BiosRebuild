// The RPC server and queue records the `sifcmd` library keeps (spec/03
// BOOT-12d, spec/06 IOP-5f).
//
// **A title's own module allocates these, so their size is not ours to
// choose.** `sceSifRegisterRpc` is handed a record the caller made, and a
// caller that lays its argument buffer out immediately after it -- `PADMAN`
// does -- has that buffer overwritten by any field past the SDK's own
// length. The server record is **17 words, 68 bytes**, measured: `PADMAN`'s
// buffer starts exactly 68 bytes after the record it registers.
//
// The field *order* below is the SDK's too, for the same reason: a client
// may read `buff` back, and `link`/`next` are how the reference's own
// `RemoveRpc` walks the list. Two slots carry something else: the SDK's
// `size` and `size2` are set by nobody -- neither `RegisterRpc` nor any
// caller writes them -- so the pending call's send size and function number
// live there rather than in an eighteenth and nineteenth word that would
// land in the caller's memory.

#pragma once

#include <stdint.h>

namespace ps2::sifrpc {

struct Queue;

// One registered server: what RegisterRpc was told, and the request its
// queue's thread has yet to answer. 17 words exactly -- see above.
struct Server {
    uint32_t sid;               // +0x00
    void *(*function)(uint32_t fno, void *buffer, uint32_t size);   // +0x04
    void *buffer;               // +0x08
    uint32_t send_size;         // +0x0C  the SDK's unset `size`
    void *(*cfunction)(uint32_t fno, void *buffer, uint32_t size);  // +0x10
    void *cbuffer;              // +0x14
    uint32_t fno;               // +0x18  the SDK's unset `size2`
    uint32_t client;            // +0x1C
    uint32_t pkt_addr;          // +0x20
    uint32_t rpc_id;            // +0x24
    uint32_t receive;           // +0x28
    uint32_t receive_size;      // +0x2C
    uint32_t mode;              // +0x30
    uint32_t rec_id;            // +0x34
    Server *next_pending;       // +0x38  the SDK's `link`
    Server *next;               // +0x3C  the next registered server
    Queue *queue;               // +0x40  the SDK's `base`
};

static_assert(sizeof(Server) == 68, "a title's module allocates this record");

// A queue: the thread RpcLoop runs on, and the servers with requests. Also
// caller-allocated, and the SDK's is three words.
struct Queue {
    uint32_t thread_id;
    Server *pending;
    Queue *next;
};

static_assert(sizeof(Queue) == 12, "a title's module allocates this record too");

}  // namespace ps2::sifrpc
