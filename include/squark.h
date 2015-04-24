/*
 * Helper library for running multiple quarks in subprocesses.
 */

#ifndef SQUARK_H
#define SQUARK_H

#include "quark.h"

typedef struct squark {
    bool is_dirty;
    lwt_heap_t* heap;
    rio_proc_t* proc;
    rio_t* out_h;
    rcd_sub_fiber_t* reader;
    rcd_sub_fiber_t* watcher;
} squark_t;

/// Squark main. Should be called from program main.
/// Will not return if main arguments indicate a squark spawn, i.e. first argument is "squark".
/// May thrown exceptions during normal squark lifecycle.
void squark_main(list(fstr_t)* main_args, list(fstr_t)* main_env);

/// Spawns a new squark. Invokes the own process with first argument "squark" plus additional arguments.
squark_t* squark_spawn(fstr_t db_dir, fstr_t index_id, uint16_t target_ipp, list(fstr_t)* unix_env);

/// Kills a running squark and frees associated resources. Does not wait for sync.
void squark_kill(squark_t* sq);

/// Used to throttle insert to disk speed and ensure persistence to external systems.
/// Returns a static fid that is exited with the guarantee that sync completed.
/// This call will uninterruptibly block if pipe is full.
rcd_fid_t squark_op_barrier(squark_t* sq);

/// Inserts an element. Buffers data in the squark pipe without waiting for reply.
/// This call will uninterruptibly block if pipe is full.
void squark_op_insert(squark_t* sq, fstr_t key, fstr_t value);

/// Upserts an element. Buffers data in the squark pipe without waiting for reply.
/// This call will uninterruptibly block if pipe is full.
void squark_op_upsert(squark_t* sq, fstr_t key, fstr_t value);

/// Starts an asynchronous scan operation. Call squark_get_scan_res() with returned
/// fiber id to block while waiting for the result.
/// This call will uninterruptibly block if pipe is full.
rcd_sub_fiber_t* squark_op_scan(squark_t* sq, qk_scan_op_t op);

/// Returns the scan result from a squark_op_scan() operation.
/// Killing the squark while calling this function is fine. In this situation the function
/// will stop blocking and return an empty string and out count 0 with out eof true.
fstr_mem_t* squark_get_scan_res(rcd_fid_t scan_fid, uint64_t* out_count, bool* out_eof);

/// Removes a squark index permanently.
void squark_rm_index(fstr_t db_dir, fstr_t index_id);

/// Lists all squark indexes in the database directory.
/// Returns a list of index_id's.
list(fstr_t)* squark_get_indexes(fstr_t db_dir);

#endif /* SQUARK_H */
