// The RPC server records EESYNC's `sifcmd` library keeps for a server module
// (spec/03 BOOT-12d, spec/06 IOP-5f). Both sides are ours, so the records'
// shape is an interface between them rather than the SDK's.

#pragma once

#include <stdint.h>

namespace ps2::sifrpc {

struct Queue;

// One registered server: what RegisterRpc was told, and the request its
// queue's thread has yet to answer.
struct Server {
    uint32_t sid;
    void *(*function)(uint32_t fno, void *buffer, uint32_t size);
    void *buffer;
    uint32_t size;
    void *(*cfunction)(uint32_t fno, void *buffer, uint32_t size);
    void *cbuffer;
    uint32_t size2;
    Queue *queue;
    Server *next;               // the next registered server
    Server *next_pending;       // the next with a request, on the queue
    // The pending call (BOOT-12d's RPC_CALL fields).
    uint32_t client;
    uint32_t rec_id;
    uint32_t pkt_addr;
    uint32_t rpc_id;
    uint32_t fno;
    uint32_t send_size;
    uint32_t receive;
    uint32_t receive_size;
    uint32_t mode;
};

// A queue: the thread RpcLoop runs on, and the servers with requests.
struct Queue {
    uint32_t thread_id;
    Server *pending;
    Queue *next;
};

}  // namespace ps2::sifrpc
