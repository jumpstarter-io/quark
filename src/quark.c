/* Copyright © 2014, Jumpstarter AB.
 * Quark b-skip list implementation. */

#include "quark-internal.h"
#include "hmap.h"

#pragma librcd

noret void qk_throw_sanity_error(fstr_t file, int64_t line) {
    throw(concs("quark detected fatal memory corruption or algorithm error at ", file, ":", line), exception_fatal);
}

/// Allocates a new empty partition.
static inline qk_part_t* qk_part_alloc_new(qk_map_ctx_t* mctx, uint8_t level, uint64_t req_space) {
    // Allocate and initialize partition.
    size_t part_size;
    uint8_t size_class;
    uint64_t min_size = sizeof(qk_part_t) + req_space;
    qk_part_t* part = qk_vm_alloc(mctx, min_size, &part_size, &size_class);
    *part = (qk_part_t) {
        .total_size = part_size
    };
    // Update statistics.
    mctx->map->stats.part_class_count[size_class]++;
    mctx->map->stats.lvl[level].total_alloc_b += part_size;
    mctx->map->stats.lvl[level].part_count++;
    // Return partition.
    return part;
}

/// Frees a no longer used and no longer referenced partition.
static inline void qk_part_alloc_free(qk_map_ctx_t* mctx, uint8_t level, qk_part_t* part) {
    // Free partition.
    uint8_t size_class;
    uint64_t total_size = part->total_size;
    qk_vm_free(mctx, part, total_size, &size_class);
    // Update statistics.
    mctx->map->stats.part_class_count[size_class]--;
    mctx->map->stats.lvl[level].total_alloc_b -= total_size;
    mctx->map->stats.lvl[level].part_count--;
}

/// Returns the number of bytes required to store a certain key/value pair at a certain level.
static inline uint64_t qk_space_kv_level(uint8_t level, fstr_t key, fstr_t value) {
    /// level0: [qk_idx_t] <--- free space ---> ["key"][qk_part_t*]
    /// level1+: [qk_idx_t] <--- free space ---> ["...key..."][uint64_t: valuelen]["...value..."]
    uint64_t size = sizeof(qk_idx_t) + key.len;
    if (level > 0) {
        size += sizeof(qk_part_t*);
    } else {
        size += sizeof(uint64_t) + value.len;
    }
    return size;
}

static inline uint64_t qk_space_idx_data_level(uint8_t level, qk_idx_t* idx) {
    uint64_t space = idx->keylen;
    if (level > 0) {
        space += sizeof(qk_part_t*);
    } else {
        space += sizeof(uint64_t);
        space += qk_idx0_get_value(idx).len;
    }
    return space;
}

static inline uint64_t qk_space_range_level(uint8_t level, qk_idx_t* idxS, qk_idx_t* idxE) {
    uint64_t space = 0;
    for (qk_idx_t* idxC = idxS; idxC < idxE; idxC++) {
        space += sizeof(qk_idx_t);
        space += qk_space_idx_data_level(level, idxC);
    }
    return space;
}

/// Cross partition copy of entries in a source range from a
/// source partition to destination partition on the same level.
/// The partition meta data is not updated to reflect the change.
static void qk_part_insert_entry_range(uint8_t level, qk_part_t* dst_part, qk_idx_t* idxS, qk_idx_t* idxSE) {
    qk_idx_t* idx0 = qk_part_get_idx0(dst_part);
    void* write0 = qk_part_get_write0(dst_part);
    void* writeD = write0;
    qk_idx_t* idxD = idx0 + dst_part->n_keys;
    for (; idxS < idxSE; idxS++, idxD++) {
        // Copy entry data.
        size_t dsize = qk_space_idx_data_level(level, idxS);
        writeD -= dsize;
        memcpy(writeD, idxS->keyptr, dsize);
        // Write index.
        idxD->keylen = idxS->keylen;
        idxD->keyptr = writeD;
        // Assert that we had space left, the caller is responsible for this.
        assert((void*) (idxD + 1) <= writeD);
    }
    dst_part->n_keys = (idxD - idx0);
    dst_part->data_size += (write0 - writeD);
    // No statistics is required to be updated as we assume the
    // entries copied over was already allocated and accounted for.
}

/// Raw write of entry data.
static inline void* qk_write_entry_data(uint8_t level, void* write0, fstr_t key, fstr_t value, qk_part_t*** out_downR) {
    void* writeD = write0;
    if (level > 0) {
        // Allocate down pointer in data and return pointer to it so caller
        // can write it when the down partition has been resolved.
        writeD -= sizeof(qk_part_t*);
        // Writing right down pointer is required by the caller.
        *out_downR = (qk_part_t**) writeD;
    } else {
        // Allocate value and write it.
        writeD -= value.len;
        memcpy(writeD, value.str, value.len);
        // Allocate valuelen and write it.
        writeD -= sizeof(uint64_t);
        *((uint64_t*) writeD) = value.len;
    }
    // Allocate key memory and write it.
    writeD -= key.len;
    memcpy(writeD, key.str, key.len);
    return writeD;
}

/// Inserts an entry into a partition at a target index.
/// When idxT is null the function will add the entity to the end of the index.
/// References are returned to the "left" and "right" down pointers. The right
/// down pointer corresponds to the the inserted entry. The left down pointer
/// corresponds to the entry immediately lower than the inserted entry and can
/// be returned as null when targeting the leftmost index.
/// The caller is responsible for:
///     - Looking up the correct target index to insert into and passing it
///       into idxT. Inserting here should preserve the sorted property of
///       the index.
///     - Ensuring that the partition has room for the insert.
///     - Updating the returned right and left down pointer references
///       as required/applicable.
static void qk_part_insert_entry(
    qk_map_t* map, uint8_t level,
    qk_part_t* dst_part, qk_idx_t* idxT,
    fstr_t key, fstr_t value,
    qk_part_t*** out_downL, qk_part_t*** out_downR
) {
    // Write entry data.
    void* write0 = qk_part_get_write0(dst_part);
    void* writeD = qk_write_entry_data(level, write0, key, value, out_downR);
    // Resolve destination index.
    qk_idx_t* idx0 = qk_part_get_idx0(dst_part);
    qk_idx_t* idxE = idx0 + dst_part->n_keys;
    if (idxT == 0) {
        // Append to index.
        idxT = idxE;
    } else if (idxT < idxE) {
        // Insertion sort insert:
        // Move all indexes forward to make room for insert into target index.
        memmove(idxT + 1, idxT, (idxE - idxT) * sizeof(*idxT));
    }
    assert(idxT >= idx0 && idxT <= idxE);
    assert((void*) (idxE + 1) <= writeD);
    // Write index.
    idxT->keylen = key.len;
    idxT->keyptr = writeD;
    // Update partition meta data.
    uint64_t data_alloc = (write0 - writeD);
    dst_part->n_keys++;
    dst_part->data_size += data_alloc;
    // Update statistics.
    map->stats.lvl[level].ent_count++;
    map->stats.lvl[level].data_alloc_b += data_alloc;
    // Resolve left down pointer if requested.
    if (level > 0 && out_downL != 0) {
        *out_downL = (idxT > idx0)? qk_idx1_get_down_ptr(idxT - 1): 0;
    }
}

static void qk_part_delete_entry(
    qk_map_t* map, uint8_t level,
    qk_part_t* part, qk_idx_t* idxT, bool rm_key
) {
    assert(part->n_keys > 0);
    size_t ent_dsize = qk_space_idx_data_level(level, idxT);
    void* d_beg = qk_part_get_write0(part);
    void* d_end = idxT->keyptr;
    assert(d_beg <= d_end);
    qk_idx_t* idx0 = qk_part_get_idx0(part);
    qk_idx_t* idxE = idx0 + part->n_keys;
    // Erase data by moving all left data to the right.
    //x-dbg/ DPRINT("moving ", d_beg, " to ", d_end, " [", ent_dsize, "] b forward");
    if (d_beg < d_end) {
        memmove(d_beg + ent_dsize, d_beg, d_end - d_beg);
        // Move all lower pointers forward.
        for (qk_idx_t* idxC = idx0; idxC < idxE; idxC++) {
            if ((void*) idxC->keyptr < d_end) {
                idxC->keyptr += ent_dsize;
            }
        }
    }
    part->data_size -= ent_dsize;
    map->stats.lvl[level].data_alloc_b -= ent_dsize;
    if (rm_key) {
        // Erase key by moving all right keys to the left.
        if (idxT < idxE - 1) {
            memmove(idxT, idxT + 1, sizeof(*idxT) * (idxE - idxT - 1));
        }
        part->n_keys--;
        map->stats.lvl[level].ent_count--;
    }
}

/// Returns the number of bytes of free space in a partition.
static inline uint64_t qk_part_free_space(qk_part_t* part) {
    return part->total_size
        - sizeof(qk_part_t)
        - part->n_keys * sizeof(qk_idx_t)
        - part->data_size;
}

/// Reallocates a partition so it can support the requested amount of free space.
/// This deallocates the old partition and allocates a new partition that
/// replaces it with the necessary amount of free space.
/// The new partition has all internal pointers updated as required.
/// The function will however not update any external references, it leaves
/// that responsibility to the caller.
static qk_part_t* qk_part_realloc(qk_map_ctx_t* mctx, uint8_t level, qk_part_t* part, uint64_t req_space) {
    // Allocate replacement partition.
    qk_part_t* new_part = qk_part_alloc_new(mctx, level, part->total_size + req_space);
    // Copy data of entities. No internal translation is required.
    {
        void* data_dst = ((void*) new_part) + new_part->total_size - part->data_size;
        void* data_src = ((void*) part) + part->total_size - part->data_size;
        memcpy(data_dst, data_src, part->data_size);
    }
    // Copy keys of entities with translated key pointers.
    {
        int64_t part_offs =  (void*) new_part - (void*) part;
        int64_t part_data_offs = (int64_t) new_part->total_size - (int64_t) part->total_size + part_offs;
        qk_idx_t* idxC_old = qk_part_get_idx0(part);
        qk_idx_t* idxE_old = idxC_old + part->n_keys;
        qk_idx_t* idxC_new = qk_part_get_idx0(new_part);
        for (; idxC_old < idxE_old; idxC_old++, idxC_new++) {
            idxC_new->keylen = idxC_old->keylen;
            idxC_new->keyptr = idxC_old->keyptr + part_data_offs;
        }
    }
    // Initialize header.
    new_part->n_keys = part->n_keys;
    new_part->data_size = part->data_size;
    // Free old partition.
    qk_part_alloc_free(mctx, level, part);
    // Using new expanded partition now.
    assert(qk_part_free_space(new_part) >= req_space);
    return new_part;
}

/// Expands partition so one more entity can be inserted.
/// Translate target index after reallocation is complete and returns the new partition.
static qk_part_t* qk_part_insert_expand(qk_map_ctx_t* mctx, uint8_t level, qk_part_t* part, uint64_t req_space, qk_idx_t** io_idxT) {
    //x-dbg/ DBGFN("reallocating partition ", part);
    // We must reallocate the partition so we can have room for insert.
    void* old_part_ptr = part;
    uint64_t old_size = part->total_size;
    qk_part_t* new_part = qk_part_realloc(mctx, level, part, req_space);
    // Translate reference to target index. This faster than doing a new lookup.
    int64_t part_offs = (void*) new_part - (void*) part;
    *io_idxT = ((void*) *io_idxT) + part_offs;
    // We are now "on" the new partition.
    return new_part;
}

/// Binary search in a partition index for specified key and returns the index for it.
/// When the key is not found false is returned and a pointer to the index where the
/// key should be inserted.
static bool qk_idx_lookup(qk_idx_t* idx0, qk_idx_t* idxE, fstr_t keyT, qk_idx_t** out_idxT) {
    /// The range we are searching are from start (inclusive) to end (exclusive).
    qk_idx_t* idxS = idx0;
    qk_idx_t* idxC;
    int64_t cmp = 0;
    while (idxE > idxS) {
        idxC = idxS + ((idxE - idxS) / 2);
        fstr_t keyC = {.str = idxC->keyptr, .len = idxC->keylen};
        cmp = fstr_cmp_lexical(keyC, keyT);
        if (cmp < 0) {
            // keyC is too low. We look to the right after a higher keyC.
            idxS = (idxC + 1);
        } else if (cmp > 0) {
            // keyC is too high. We look to the left after a lower keyC.
            idxE = idxC;
        } else if (cmp == 0) {
            // Key found.
            *out_idxT = idxC;
            return true;
        }
    }
    // Based on last compare we can find the node than is less than the node we searched for.
    if (cmp < 0) {
        // Too low and last value. We must have looked at the largest value lower than keyT last.
        // This means that the target index is to the right.
        *out_idxT = idxC + 1;
    } else if (cmp > 0) {
        // Too high and last value. We must have looked at the smallest value higher than keyT last.
        // This means that this is our target index.
        *out_idxT = idxC;
    } else if (cmp == 0) {
        // No key looked at. Index is empty. The target index is the first.
        assert(idx0 == idxE);
        *out_idxT = idx0;
    }
    return false;
}

static void qk_check_keylen(fstr_t key) {
    if (key.len > UINT16_MAX) sub_heap {
        throw(concs("key is too large, [", key.len, "] > [", UINT16_MAX, "]"), exception_io);
    }
}

typedef struct lookup_res {
    /// Reference to:
    /// When found: 0 level partition.
    /// When not found: undefined.
    qk_part_t** ref0;
    /// Reference to insert level partition.
    qk_part_t** refI;
    /// When found: the level the element is currently inserted at.
    uint8_t insert_lvl;
    struct {
        /// Target partition to insert or split.
        qk_part_t* part;
        /// Target index slot to insert key.
        qk_idx_t* idxT;
    } target[8];
} lookup_res_t;

typedef enum lookup_mode {
    lookup_mode_key,
    lookup_mode_first,
    lookup_mode_last,
} lookup_mode_t;

typedef struct lookup_op {
    /// Controls if lookup up key or first/last.
    lookup_mode_t mode;
    /// Key to lookup if mode == lookup_mode_key.
    fstr_t key;
    /// Normally the index returned for every target level is the down index followed during the lookup.
    /// When this is set to true AND the key is not found, the down index will instead be the index after
    /// the down index followed where an insert would be made.
    /// The down index will always be (idx0 - 1) when following the root.
    bool insert_idx;
    /// Insert level, controls lookup ref.
    uint8_t insert_lvl;
    /// When true the lookup aborts and returns undefined lookup_res_t if the key is found.
    bool found_abort;
} lookup_op_t;

/// Quark lookup with specified operation.
static inline bool qk_lookup(qk_map_ctx_t* mctx, lookup_op_t op, lookup_res_t* out_r) {
    if (op.mode == lookup_mode_key)
        qk_check_keylen(op.key);
    qk_map_t* map = mctx->map;
    CASSERT(LENGTHOF(out_r->target) == LENGTHOF(map->root));
    bool following_root = true;
    qk_part_t** ref;
    qk_part_t* part;
    for (size_t i_lvl = LENGTHOF(map->root) - 1;; i_lvl--) {
        // Resolve partition and register it.
        if (following_root) {
            ref = &map->root[i_lvl];
            part = *ref;
        }
        assert(part != 0);
        out_r->target[i_lvl].part = part;
        // Search partition index.
        qk_idx_t* idx0 = qk_part_get_idx0(part);
        qk_idx_t* idxE = idx0 + part->n_keys;
        qk_idx_t* idxT;
        bool found;
        switch (op.mode) {{
        } case lookup_mode_key: {
            // Normal key compare lookup with binary search.
            found = qk_idx_lookup(idx0, idxE, op.key, &idxT);
            break;
        } case lookup_mode_first: {
            // Simulate lookup with infinitely small key.
            idxT = idx0;
            found = false;
            break;
        } case lookup_mode_last: {
            // Simulate lookup with infinitely big key.
            idxT = idxE;
            found = false;
            break;
        }}
        assert(idxT >= idx0 && idxT <= idxE);
        // Fast travel to value when key is found.
        if (found) {
            if (op.found_abort)
                return true;
            out_r->refI = ref;
            out_r->insert_lvl = i_lvl;
            out_r->target[i_lvl].idxT = idxT;
            while (i_lvl > 0) {
                ref = qk_idx1_get_down_ptr(idxT);
                part = *ref;
                idxT = qk_part_get_idx0(part);
                i_lvl--;
                out_r->target[i_lvl].part = part;
                out_r->target[i_lvl].idxT = idxT;
            }
            out_r->ref0 = ref;
            return true;
        }
        // Register target/down index.
        qk_idx_t* idxD = (idxT - 1);
        out_r->target[i_lvl].idxT = (op.insert_idx? idxT: idxD);
        if (i_lvl == op.insert_lvl) {
            out_r->refI = ref;
        }
        // Travel further.
        if (i_lvl == 0) {
            // Key was not found.
            out_r->target[i_lvl].idxT = idxT;
            out_r->ref0 = ref;
            return false;
        }
        // Determine how to reference the next level.
        if (idxT == idx0) {
            // This partition is too high, keep following root.
            QK_SANTIY_CHECK(following_root);
        } else {
            // We follow the key that is immediately lower than the key
            // we are looking up to get to the next partition.
            ref = qk_idx1_get_down_ptr(idxD);
            part = *ref;
            following_root = false;
        }
    }
}

bool qk_get(qk_map_ctx_t* mctx, fstr_t key, fstr_t* out_value) {
    lookup_op_t op = {
        .mode = lookup_mode_key,
        .key = key,
    };
    lookup_res_t r;
    if (qk_lookup(mctx, op, &r)) {
        *out_value = qk_idx0_get_value(r.target[0].idxT);
        return true;
    } else {
        return false;
    }
}

bool qk_band_read(fstr_t* io_mem, fstr_t* out_key, fstr_t* out_value) {
    if (io_mem->len == 0)
        return false;
    fstr_t band = *io_mem;
    // Determine key/value location from offsets on band.
    uint16_t keylen = *((uint16_t*) band.str);
    fstr_t key = {
        .str = band.str + sizeof(uint16_t),
        .len = keylen
    };
    uint64_t* valuelen_ptr = (void*) (key.str + key.len);
    uint8_t* valuestr = (void*) (valuelen_ptr + 1);
    fstr_t value = {
        .str = valuestr,
        .len = *valuelen_ptr
    };
    // Update band.
    void* next_ptr = value.str + value.len;
    uint64_t raw_band_len = next_ptr - (void*) band.str;
    *io_mem = fstr_slice(band, raw_band_len, -1);
    // Return key/value.
    if (out_key != 0)
        *out_key = key;
    if (out_value != 0)
        *out_value = value;
    return true;
}

/// Seek forward to next level 0 partition with appropriate side
/// effects on lookup result.
bool qk_seek_lvl0_part_fwd(lookup_res_t* r, uint8_t level) {
    while (level < LENGTHOF(r->target)) {
        // Get level position and target index.
        qk_part_t* part = r->target[level].part;
        if (part->n_keys == 0)
            goto next_lvl;
        qk_idx_t* idxT = r->target[level].idxT;
        qk_idx_t* idx0 = qk_part_get_idx0(part);
        qk_idx_t* idxE = idx0 + part->n_keys;
        // Seek forward in index to next key.
        idxT++;
        if (idxT < idxE) {
            r->target[level].idxT = idxT;
            // Seek down to level 0 and set offset to first index.
            while (level > 0) {
                level--;
                part = *qk_idx1_get_down_ptr(idxT);
                assert(part->n_keys > 0);
                idxT = qk_part_get_idx0(part);
                r->target[level].part = part;
                r->target[level].idxT = idxT;
            }
            return true;
        }
        next_lvl:
        level++;
    }
    return false;
}

/// Seek backward to previous level 0 partition with appropriate side
/// effects on lookup result.
bool qk_seek_lvl0_part_rev(qk_map_ctx_t* mctx, lookup_res_t* r, uint8_t level) {
    qk_map_t* map = mctx->map;
    for (;;) {
        assert(level < LENGTHOF(r->target));
        // Get level position and target index.
        qk_part_t* part = r->target[level].part;
        new_root_lvl_inject:;
        if (part->n_keys == 0)
            goto next_lvl;
        qk_idx_t* idxT = r->target[level].idxT;
        qk_idx_t* idx0 = qk_part_get_idx0(part);
        qk_idx_t* idxE = idx0 + part->n_keys;
        // Seek backward in index to previous key.
        idxT--;
        if (idxT >= idx0) {
            r->target[level].idxT = idxT;
            // Seek down to level 0 and set offset to last index.
            while (level > 0) {
                level--;
                part = *qk_idx1_get_down_ptr(idxT);
                assert(part->n_keys > 0);
                idxT = qk_part_get_idx0(part) + part->n_keys - 1;
                r->target[level].part = part;
                r->target[level].idxT = idxT;
            }
            return true;
        }
        next_lvl:
        if (part == map->root[level]) {
            // Reached smallest key/value pair supported by this root level.
            // Need to travel to lower root.
            do {
                if (level == 0) {
                    // We are done, nothing is lower.
                    return false;
                }
                level--;
                part = map->root[level];
            } while (part == r->target[level].part);
            r->target[level].part = part;
            r->target[level].idxT = qk_part_get_idx0(part) + part->n_keys;
            goto new_root_lvl_inject;
        } else {
            // Not reached root entry partition yet, travel up.
            level++;
        }
    }
}

static inline bool qk_band_write(qk_idx_t* idxT, fstr_t* band_tail, uint64_t* ent_count, uint64_t limit, bool ignore_data, bool* out_eof) {
    // Check if we have reached the limit for the number of items we may scan.
    if (limit > 0 && *ent_count >= limit)
        return false;
    if (ignore_data) {
        // Only copy over key, ignore value.
        // Check if we have space on remaining band to do the copy.
        size_t req_space = sizeof(uint16_t) + idxT->keylen + sizeof(uint64_t);
        if (band_tail->len < req_space)
            goto no_more_space;
        // Write key only to band. Emulate zero length value.
        void* band_ptr = band_tail->str;
        *((uint16_t*) band_ptr) = idxT->keylen;
        band_ptr += sizeof(uint16_t);
        memcpy(band_ptr, idxT->keyptr, idxT->keylen);
        band_ptr += idxT->keylen;
        *((uint64_t*) band_ptr) = 0;
        band_ptr += sizeof(uint64_t);
        // Update band tail.
        assert((void*) (band_tail->str) == (band_ptr - req_space));
        band_tail->str = band_ptr;
        band_tail->len -= req_space;
    } else {
        // Check if we have space on remaining band to do the copy.
        size_t dsize = qk_space_idx_data_level(0, idxT);
        size_t req_space = sizeof(uint16_t) + dsize;
        if (band_tail->len < req_space)
            goto no_more_space;
        // Quickly copy over u16 keylen and value blob.
        *((uint16_t*) band_tail->str) = idxT->keylen;
        band_tail->str += sizeof(uint16_t);
        memcpy(band_tail->str, idxT->keyptr, dsize);
        // Update band tail.
        band_tail->str += dsize;
        band_tail->len -= req_space;
    }
    // Update band entry count.
    *ent_count = *ent_count + 1;
    // Need only continue scan if we are allowed to write more items to band.
    return (limit == 0 || *ent_count < limit);
    // Buffer has run out. This is the only situation where we use false eof.
    no_more_space: {
        *out_eof = false;
        return false;
    }
}

uint64_t qk_scan(qk_map_ctx_t* mctx, qk_scan_op_t op, fstr_t* io_mem, bool* out_eof) {
    // Initialize default return values.
    // End of "file" is only set to false if band runs out.
    bool end_of_file = true;
    uint64_t ent_count = 0;
    fstr_t band = *io_mem;
    fstr_t band_tail = band;
    lookup_res_t r;
    bool start_equal;
    if (op.with_start) {
        // Initial lookup/seek to start key.
        lookup_op_t l_op = {
            .mode = lookup_mode_key,
            .key = op.key_start,
        };
        start_equal = qk_lookup(mctx, l_op, &r);
    } else {
        // Initial lookup/seek to index start/end.
        lookup_op_t l_op = {
            .mode = (op.descending? lookup_mode_last: lookup_mode_first),
            .key = op.key_start,
        };
        qk_lookup(mctx, l_op, &r);
        // Made an infinite positive/negative lookup that never matches exactly.
        start_equal = false;
    }
    // The lookup results looks up the insert location of the start key.
    // However we are interested in the element before or after it.
    // The relationship between these locations can be slightly complicated
    // since the insert target could even be an index that doesn't exist.
    if (!start_equal || !op.inc_start) {
        // We did not match start key and could have an invalid insert position
        // or we did. In both these cases we fix the problem by stepping in
        // whatever direction we are configured to.
        if (op.descending) {
            // Descending seek, i.e. reverse.
            // Target index is too high or at invalid, seek back.
            if (!qk_seek_lvl0_part_rev(mctx, &r, 0)) {
                goto scan_done;
            }
        } else {
            // Ascending seek, i.e. forward.
            if (start_equal) {
                // Step forward on lowest level to not include start.
                if (!qk_seek_lvl0_part_fwd(&r, 0)) {
                    goto scan_done;
                }
            } else {
                // Need only step if at invalid offset.
                qk_part_t* part = r.target[0].part;
                qk_idx_t* idxT = r.target[0].idxT;
                qk_idx_t* idx0 = qk_part_get_idx0(part);
                qk_idx_t* idxE = idx0 + part->n_keys;
                if (idxT == (idx0 - 1) || idxT == idxE) {
                    // We can start step on level 1 since we already know level 0 is invalid.
                    if (!qk_seek_lvl0_part_fwd(&r, 1)) {
                        goto scan_done;
                    }
                } else {
                    // Target index should already be valid.
                    assert(idx0 <= idxT && idxT < idxE);
                }
            }
        }
    } else {
        // We matched the start key so idxT is valid and it's also to be included in the scan.
        assert(start_equal && op.inc_start);
    }

    // The idxT is valid now and positioned on the first element to scan.
    for (;;) {
        qk_part_t* part = r.target[0].part;
        assert(part->n_keys > 0);
        qk_idx_t* idxT = r.target[0].idxT;
        qk_idx_t* idx0 = qk_part_get_idx0(part);
        qk_idx_t* idxE = idx0 + part->n_keys;
        assert(idx0 <= idxT && idxT < idxE);
        // Iterate quickly through partition and scan to band.
        for (;;) {
            // Get key.
            fstr_t key = qk_idx_get_key(idxT);
            // Compare with end key.
            if (op.with_end) {
                int64_t cmp = fstr_cmp_lexical(key, op.key_end);
                if (cmp == 0) {
                    if (op.inc_end) {
                        // Write end k/v pair to band.
                        qk_band_write(idxT, &band_tail, &ent_count, op.limit, op.ignore_data, &end_of_file);
                    }
                    goto scan_done;
                }
                // Enforce end at end key.
                if ((!op.descending && cmp > 0) || (op.descending && cmp < 0)) {
                    goto scan_done;
                }
            }
            // Write k/v pair to band.
            if (!qk_band_write(idxT, &band_tail, &ent_count, op.limit, op.ignore_data, &end_of_file)) {
                goto scan_done;
            }
            // Go to next k/v pair.
            if (op.descending) {
                // Descending seek, i.e. reverse.
                idxT--;
                if (idxT < idx0) {
                    if (!qk_seek_lvl0_part_rev(mctx, &r, 1)) {
                        goto scan_done;
                    }
                    break;
                }
            } else {
                // Ascending seek, i.e. forward.
                idxT++;
                if (idxT >= idxE) {
                    if (!qk_seek_lvl0_part_fwd(&r, 1)) {
                        goto scan_done;
                    }
                    break;
                }
            }
        }
    }
    scan_done:
    *io_mem = fstr_detail(band, band_tail);
    *out_eof = end_of_file;
    return ent_count;
}

static void qk_update_ent(qk_map_ctx_t* mctx, fstr_t key, fstr_t new_value, lookup_res_t* r) {
    qk_part_t* part = r->target[0].part;
    qk_idx_t* idxT = r->target[0].idxT;
    fstr_t cur_value = qk_idx0_get_value(idxT);
    if (new_value.len == cur_value.len) {
        // Replace in-place.
        if (cur_value.len > 0) {
            memcpy(cur_value.str, new_value.str, cur_value.len);
        }
    } else { // (new_value.len != cur_value.len)
        // Delete the entry data by moving everything on the left into it.
        qk_map_t* map = mctx->map;
        qk_part_delete_entry(map, 0, part, idxT, false);
        // Insert the new value now.
        if (new_value.len > cur_value.len) {
            // Expand may be required.
            //x-dbg/ DPRINT("may require expand: ", new_value.len, " > ", cur_value.len);
            uint64_t free_space = qk_part_free_space(part);
            uint64_t req_space = qk_space_kv_level(0, key, new_value) - sizeof(qk_idx_t);
            if (free_space < req_space) {
                //x-dbg/ DPRINT("expand required: ", req_space, " > ", free_space);
                // Reallocate the partition to expand it and translate the index target.
                qk_part_t* new_part = qk_part_insert_expand(mctx, 0, part, req_space, &idxT);
                // Update old partition reference (root pointer or a down pointer) to point to new partition.
                assert(*(r->ref0) == part);
                *(r->ref0) = new_part;
                part = new_part;
            }
        }
        // Write the new data.
        //x-dbg/ DPRINT("writing new data");
        assert(qk_space_kv_level(0, key, new_value) - sizeof(qk_idx_t) <= qk_part_free_space(part));
        void* write0 = qk_part_get_write0(part);
        void* writeD = qk_write_entry_data(0, write0, key, new_value, 0);
        // Write the new pointer.
        idxT->keyptr = writeD;
        // Adjust data size.
        size_t ent_dsize = (write0 - writeD);
        part->data_size += ent_dsize;
        map->stats.lvl[0].data_alloc_b += ent_dsize;
    }
}

bool qk_update(qk_map_ctx_t* mctx, fstr_t key, fstr_t new_value) {
    lookup_op_t op = {
        .mode = lookup_mode_key,
        .key = key,
    };
    lookup_res_t r;
    if (!qk_lookup(mctx, op, &r)) {
        // Update require key to exist.
        return false;
    }
    // Perform update of looked up entity now.
    qk_update_ent(mctx, key, new_value, &r);
    return true;
}

static bool qk_xsert(qk_map_ctx_t* mctx, fstr_t key, fstr_t value, bool upsert) {
    qk_check_keylen(key);
    qk_map_t* map = mctx->map;
    // Calculate the level to insert node at through a series of presumably heavily biased coin tosses.
    // Due to the heavy bias of the coin toss it should be faster to do this numerically than using software math.
    uint64_t dspace = MAX(map->target_ipp, 1) + 1ULL;
    uint8_t insert_lvl = 0;
    do {
        uint64_t rnd64, dice;
        // Get 64 bit of random entropy.
        if (map->dtrm_seed == 0) {
            rnd64 = lwt_rdrand64();
        } else {
            rnd64 = hmap_murmurhash_64a(key.str, key.len, map->dtrm_seed + insert_lvl);
        }
        // Translate to dice space.
        dice = rnd64 % dspace;
        // The dice space is at max 16 bit. The worst case probability error we can get from
        // non-aligned dspace (2^64 % dspace != 0) is negligible (2^16 / 2^48 = 2e-10) and
        // can safely be ignored.
        if (dice != 0)
            break;
        insert_lvl++;
    } while (insert_lvl < LENGTHOF(map->root) - 1);
    //x-dbg/ DBGFN("inserting [", key, "] => [", value, "] on level #", insert_lvl);
    // We have generated a fair insert level and is ready to begin insert.
    // Read phase: Search from top level to:
    //  1) Resolve target partitions to insert on.
    //  2) Resolve insert level target partition reference.
    //  3) Resolve all target lte indexes of all target partitions.
    //  4) See if key is already inserted.
    lookup_op_t op = {
        .mode = lookup_mode_key,
        .key = key,
        .insert_idx = true,
        .insert_lvl = insert_lvl,
        .found_abort = !upsert,
    };
    lookup_res_t r;
    if (qk_lookup(mctx, op, &r)) {
        // Key already inserted!
        if (upsert) {
            // Perform update of looked up value now.
            qk_update_ent(mctx, key, value, &r);
        } else {
            // Insert require key to not exist.
        }
        return false;
    }
    // Write phase.
    // Calculate required insert space at entry level.
    uint64_t req_space = qk_space_kv_level(insert_lvl, key, value);
    // Start mutation.
    qk_part_t **downL, **downR;
    qk_part_t** prev_down_ptr = 0;
    for (size_t i_lvl = insert_lvl;;) {
        // Go to next resolved target partition
        qk_part_t* part = r.target[i_lvl].part;
        qk_idx_t* idxT = r.target[i_lvl].idxT;
        if (i_lvl == insert_lvl) {
            // At insert level we do a normal insert without any split.
            // Make sure the partition has enough space.
            uint64_t free_space = qk_part_free_space(part);
            if (free_space < req_space) {
                // Reallocate the partition to expand it and translate the index target.
                part = qk_part_insert_expand(mctx, i_lvl, part, req_space, &idxT);
                // Update old partition reference (root pointer or a down pointer) to point to new partition.
                *r.refI = part;
            }
            // Insert the entity now at the resolved target index.
            // Also resolve initial left and right down reference for split phase.
            qk_part_insert_entry(map, i_lvl, part, idxT, key, value, &downL, &downR);
        } else {
            assert(i_lvl < insert_lvl);
            // Partition split mode.
            // We can only have come here from normal insert at level above
            // with a down pointer that is pending to be written.
            assert(downR != 0);
            // There are three main split modes:
            // * Right is empty (or both): no move is required. We can simply allocate new right "side"
            //   partition and insert on. The existing partition is treated as the left side.
            // * Left side is empty: we allocate a new empty left side partition and attach it to the root.
            //   After that we insert into the front of the existing partition, "adopting" all entities in it
            //   and treating it as our right side partition instead.
            // * Right and left both has entities: standard "hard" split. We allocate two new partitions
            //   left and right and write all entities to the respective side.
            // Each new mode resolves a left and a right partition after completion. The down pointer in the
            // partition above is initialized to point to the resolved right partition while the left
            // partition immediate left key down pointer is passed to the next iteration so it can be updated
            // to the resolved next left partition.
            qk_idx_t* idx0 = qk_part_get_idx0(part);
            qk_idx_t* idxE = idx0 + part->n_keys;
            assert(idxT >= idx0 && idxT <= idxE);
            bool left_empty = (idxT == idx0);
            bool right_empty = (idxT == idxE);
            qk_part_t *partL, *partR;
            qk_part_t **next_downR;
            if (right_empty) {
                // The current partition is the left partition which is kept intact.
                // No left down pointer update required.
                partL = part;
                // We allocate the right partition and insert on instead so no data move is required.
                //x-dbg/ DBGFN("new splitting partition ", part, " on level #", i_lvl);
                partR = qk_part_alloc_new(mctx, i_lvl, req_space);
                qk_part_insert_entry(map, i_lvl, partR, 0, key, value, 0, &next_downR);
            } else {
                // Standard "hard" split.
                // Calculate required space for new left and right partition.
                //x-dbg/ DBGFN("hard splitting partition ", part, " on level #", i_lvl);
                assert(idxT != 0);
                uint64_t spaceL = qk_space_range_level(i_lvl, idx0, idxT);
                // Allocate new left partition.
                partL = qk_part_alloc_new(mctx, i_lvl, spaceL);
                // Update left down pointer to point to the new left partition.
                if (downL != 0) {
                    assert(map->root[i_lvl] != part);
                    assert(*downL == part);
                    *downL = partL;
                } else {
                    // Root entry partitions are not referenced by any down pointer
                    // so in that and only that case we get a null downL.
                    assert(map->root[i_lvl] == part);
                    map->root[i_lvl] = partL;
                }
                if (left_empty) {
                    assert(downL == 0);
                    // Adopt all elements on the current partition and cut them of from the
                    // root by inserting to front.
                    partR = part;
                    uint64_t free_space = qk_part_free_space(partR);
                    if (free_space < req_space) {
                        // Reallocate the partition to expand it and translate the index target.
                        partR = qk_part_insert_expand(mctx, i_lvl, partR, req_space, &idxT);
                    }
                    // Insert to front of partition.
                    qk_part_insert_entry(map, i_lvl, partR, idxT, key, value, 0, &next_downR);
                } else {
                    // Allocate new right partition.
                    uint64_t spaceR = req_space + qk_space_range_level(i_lvl, idxT, idxE);
                    partR = qk_part_alloc_new(mctx, i_lvl, spaceR);
                    // Copy all entries to the left over to the left partition.
                    qk_part_insert_entry_range(i_lvl, partL, idx0, idxT);
                    // First element we insert in right partition is the new entity.
                    qk_part_insert_entry(map, i_lvl, partR, 0, key, value, 0, &next_downR);
                    // Copy all entries to the right over to the right partition.
                    qk_part_insert_entry_range(i_lvl, partR, idxT, idxE);
                    // Deallocate the old partition.
                    qk_part_alloc_free(mctx, i_lvl, part);
                }
            }
            // Split complete.
            // Write previous right down pointer now to initialize it.
            *downR = partR;
            // We are now "on" the right partition.
            part = partR;
            // Pass left/right down pointers to lower level insert.
            if (i_lvl > 0) {
                // The right down pointer is not yet written and the reference we received from insert.
                downR = next_downR;
                // The left down pointer reference is the last entity in the left partition we split.
                // When the left partition is an empty partition it's a root entry partition without
                // any left down pointer that needs to be updated. We use a null downL to signal this.
                downL = (partL->n_keys > 0? qk_idx1_get_down_ptr(qk_part_get_idx0(partL) + partL->n_keys - 1): 0);
            }
        }
        // Go down one level.
        if (i_lvl == 0) {
            break;
        }
        i_lvl--;
        // Level zero has new space requirements.
        if (i_lvl == 0) {
            req_space = qk_space_kv_level(i_lvl, key, value);
        }
    }
    // Insert complete.
    return true;
}

bool qk_insert(qk_map_ctx_t* mctx, fstr_t key, fstr_t value) {
    return qk_xsert(mctx, key, value, false);
}

bool qk_upsert(qk_map_ctx_t* mctx, fstr_t key, fstr_t value) {
    return qk_xsert(mctx, key, value, true);
}

bool qk_delete(qk_map_ctx_t* mctx, fstr_t key) {
    qk_check_keylen(key);
    qk_map_t* map = mctx->map;
    // Lookup key.
    lookup_op_t op = {
        .mode = lookup_mode_key,
        .key = key,
    };
    lookup_res_t r;
    if (!qk_lookup(mctx, op, &r)) {
        // Key does not exist.
        return false;
    }
    // Start mutation.
    qk_part_t** downL;
    for (size_t i_lvl = r.insert_lvl;; i_lvl--) {
        // Go to next resolved target partition
        qk_part_t* part = r.target[i_lvl].part;
        qk_idx_t* idxT = r.target[i_lvl].idxT;
        if (i_lvl == r.insert_lvl) {
            // Remove the key/value from the partition.
            qk_part_delete_entry(map, i_lvl, part, idxT, true);
            // Go down to next level.
            if (i_lvl == 0)
                break;
            // Resolve the next left partition.
            qk_idx_t* idx0 = qk_part_get_idx0(part);
            if (idxT == idx0) {
                // Root to the left, follow root down. (We would otherwise have seen this key on a higher insert level.)
                assert(part == map->root[i_lvl]);
                downL = &map->root[i_lvl - 1];
            } else {
                // Follow left down pointer down. This reference could have changed after the delete
                // entry operation (by data move) which is why we take the reference here.
                downL = qk_idx1_get_down_ptr(idxT - 1);
            }
        } else {
            // Resolve left + right partition.
            qk_part_t* partL = *downL;
            qk_part_t* partR = part;
            // We're about to free the first entry in the right partition.
            // Update the statistics first.
            assert(partR->n_keys > 0);
            qk_idx_t* idxR0 = qk_part_get_idx0(partR);
            size_t ent_dsize = qk_space_idx_data_level(i_lvl, idxR0);
            map->stats.lvl[i_lvl].data_alloc_b -= ent_dsize;
            map->stats.lvl[i_lvl].ent_count--;
            // Track left partition number of keys before it's filled on the right.
            size_t n_pre_keysL = partL->n_keys;
            // Copy everything in the dangling right partition into left partition.
            if (partR->n_keys > 1) {
                qk_idx_t* idxRE = idxR0 + partR->n_keys;
                // Calculate if expand is required or if we can just copy over everything immediately.
                uint64_t free_space = qk_part_free_space(partL);
                uint64_t req_space = 0;
                for (qk_idx_t* idxRT = idxR0 + 1; idxRT < idxRE; idxRT++) {
                    req_space += sizeof(*idxRT) + qk_space_idx_data_level(i_lvl, idxRT);
                }
                if (free_space < req_space) {
                    // Reallocate left partition with required space.
                    partL = qk_part_realloc(mctx, i_lvl, partL, req_space);
                    // Update old partition reference (root pointer or a down pointer) to point to new partition.
                    *downL = partL;
                }
                // Copy over data immediately from right to left partition.
                qk_part_insert_entry_range(i_lvl, partL, idxR0 + 1, idxRE);
            }
            // Deallocate the dangling right partition.
            qk_part_alloc_free(mctx, i_lvl, partR);
            // Go down to next level.
            if (i_lvl == 0)
                break;
            // Resolve the next left partition.
            // Must do it here after the potential reallocation to avoid relocating the down reference.
            // We're not interested in following the keys we right filled though so we use the saved key count.
            if (n_pre_keysL == 0) {
                // Root to the left, follow root down.
                assert(partL == map->root[i_lvl]);
                downL = &map->root[i_lvl - 1];
            } else {
                // Follow left partition right most down pointer down to find next left most partition to merge with.
                qk_idx_t* idxL0 = qk_part_get_idx0(partL);
                downL = qk_idx1_get_down_ptr(idxL0 + n_pre_keysL - 1);
            }
        }
    }
    // Delete complete.
    return true;
}

json_value_t qk_get_stats(qk_map_ctx_t* mctx) {
    qk_map_t* map = mctx->map;
    json_value_t levels = jarr_new();
    for (uint8_t i_lvl = 0; i_lvl < LENGTHOF(map->stats.lvl); i_lvl++) {
        json_append(levels, jobj_new(
            {"level", jnum(i_lvl)},
            {"ent_count", jnum(map->stats.lvl[i_lvl].ent_count)},
            {"part_count", jnum(map->stats.lvl[i_lvl].part_count)},
            {"total_alloc_b", jnum(map->stats.lvl[i_lvl].total_alloc_b)},
            {"data_alloc_b", jnum(map->stats.lvl[i_lvl].data_alloc_b)},
        ));
    }
    json_value_t part_class_count = jobj_new();
    for (uint8_t class = 0; class < LENGTHOF(map->stats.part_class_count); class++) {
        uint64_t count = map->stats.part_class_count[class];
        if (count == 0)
            continue;
        JSON_SET(part_class_count, concs(qk_atoms_2e_to_bytes(class), "b"), jnum(count));
    }
    return jobj_new(
        {"entry_cap", jnum(mctx->entry_cap)},
        {"levels", levels},
        {"part_class_count", part_class_count},
    );
}

static int cmp_qk_map(const struct avltree_node* a, const struct avltree_node* b) {
    qk_map_t* a_map = AVLTREE_NODE2ELEM(qk_map_t, node, a);
    qk_map_t* b_map = AVLTREE_NODE2ELEM(qk_map_t, node, b);
    int64_t cmp = fstr_cmp(a_map->name, b_map->name);
    return (cmp < 0? -1: (cmp > 0? 1: 0));
}

qk_ctx_t* qk_open(acid_h* ah) {
    // Create context.
    fstr_t am = acid_memory(ah);
    qk_ctx_t new_ctx = {
        .hdr = (void*) am.str,
        .ah = ah,
    };
    qk_ctx_t* ctx = cln(&new_ctx);
    // Read quark header.
    qk_hdr_t* hdr = ctx->hdr;
    if (hdr->magic == 0) {
        // This is a new database, initialize it.
        memset(hdr, 0, sizeof(*hdr));
        hdr->magic = QK_HEADER_MAGIC;
        hdr->version = QK_VERSION;
        avltree_init(&hdr->maps, cmp_qk_map, true);
    } else if (hdr->magic == QK_HEADER_MAGIC) {
        // We are opening an existing database.
        if (hdr->version != QK_VERSION) {
            throw("bad database version", exception_io);
        }
        // Initialize map compare function pointer that could have changed.
        hdr->maps.cmp_fn = cmp_qk_map;
    } else {
        // This is not a valid database.
        throw("corrupt or invalid database", exception_io);
    }
    // Increment session and complete first fsync to fail here if write does not work.
    hdr->session++;
    acid_fsync(ah);
    // Return with context.
    return ctx;
}

qk_map_ctx_t* qk_open_map(qk_ctx_t* ctx, fstr_t name, qk_opt_t* opt) {
    // Validate name size.
    if (name.len > PAGE_SIZE - sizeof(qk_ctx_t)) {
        throw("invalid length of qk map name", exception_arg);
    }
    // Create context.
    qk_map_ctx_t new_mctx = {
        .ctx = ctx,
    };
    qk_map_ctx_t* mctx = cln(&new_mctx);
    // Lookup the map.
    qk_map_t* map = AVLTREE_LOOKUP_KEY(qk_map_t, node, &ctx->hdr->maps, name);
    bool tune_target_ipp;
    if (map == 0) {
        // Allocate new map with this name.
        map = qk_vm_mmap_raw(ctx, PAGE_SIZE);
        memset(map, 0, sizeof(*map));
        mctx->map = map;
        // Initialize name by copying into the new allocation.
        void* name_ptr = map + 1;
        memcpy(name_ptr, name.str, name.len);
        map->name.len = name.len;
        map->name.str = name_ptr;
        // Allocate root entry partitions for all levels.
        for (uint8_t i_lvl = 0; i_lvl < LENGTHOF(map->root); i_lvl++) {
            map->root[i_lvl] = qk_part_alloc_new(mctx, i_lvl, 0);
        }
        // Insert map into the tree.
        struct avltree_node* enode = avltree_insert(&map->node, &ctx->hdr->maps);
        if (enode != 0) {
            throw("map insert failed: name already exists (data structure corrupt)", exception_fatal);
        }
        // Initialize target ipp.
        tune_target_ipp = true;
    } else {
        mctx->map = map;
        // Only override ipp if not using default.
        tune_target_ipp = (opt->target_ipp != 0);
    }
    // Tune target partition size if requested.
    if (tune_target_ipp) {
        map->target_ipp = ((opt->target_ipp != 0)? opt->target_ipp: QK_DEFAULT_TARGET_IPP);
    }
    // Write deterministic seed setting.
    map->dtrm_seed = opt->dtrm_seed;
    // Write open session and fsync.
    if (map->asession >= ctx->hdr->session) {
        throw("map open failed: map already opened this session", exception_fatal);
    }
    map->asession = ctx->hdr->session;
    acid_fsync(ctx->ah);
    // Calculate entry capacity. Clang crashes if we attempt to assign UINT128_MAX
    // to a stack variable so we complicate this implementation slightly.
    uint16_t target_ipp = map->target_ipp;
    uint128_t entry_cap = 0;
    for (uint8_t i_lvl = 0;; i_lvl++) {
        if (!arth_safe_mul_uint128(entry_cap, target_ipp, &entry_cap)) {
            mctx->entry_cap = UINT128_MAX;
            break;
        } else if (i_lvl >= LENGTHOF(map->root) - 1) {
            mctx->entry_cap = entry_cap;
            break;
        }
    }
    // Return context.
    return mctx;
}

fstr_mem_t* qk_compile_key(uint16_t n_parts, fstr_t* parts) { sub_heap {
    fstr_t* new_parts = lwt_alloc_new(sizeof(fstr_t) * n_parts);
    for (size_t i = 0; i < n_parts; i++) {
        new_parts[i] = fss(fstr_replace(parts[i], "\x00", "\x00\x01"));
    }
    return escape(fstr_concat(new_parts, n_parts, "\x00\x00"));
}}

static void qk_decompile_next_part(size_t* i_part, size_t n_parts, fstr_t* out_parts, uint8_t* w_ptr, uint8_t* part_ptr) {
    if (*i_part >= n_parts) sub_heap {
        throw(concs("key had more parts than specified (", n_parts, ")"), exception_io);
    }
    out_parts[*i_part].str = part_ptr;
    out_parts[*i_part].len = w_ptr - part_ptr;
    *i_part = *i_part + 1;
}

fstr_mem_t* qk_decompile_key(fstr_t raw_key, size_t n_parts, fstr_t* out_parts) { sub_heap {
    if (n_parts == 0) {
        throw("invalid n_parts, cannot be zero", exception_arg);
    }
    fstr_mem_t* key_mem = fstr_cpy(raw_key);
    fstr_t key = fss(key_mem);
    size_t i_part = 0;
    uint8_t* w_ptr = key.str;
    uint8_t* part_ptr = w_ptr;
    bool in_escape = false;
    for (size_t i = 0; i < key.len; i++) {
        uint8_t chr = key.str[i];
        if (in_escape) {
            if (chr == 0) {
                qk_decompile_next_part(&i_part, n_parts, out_parts, w_ptr, part_ptr);
                part_ptr = w_ptr;
            } else if (chr == 1) {
                *(w_ptr++) = '\0';
            } else {
                throw("unknown escape sequence", exception_io);
            }
            in_escape = false;
        } else {
            if (chr == 0) {
                in_escape = true;
            } else {
                *(w_ptr++) = chr;
            }
        }
    }
    if (in_escape) {
        throw("key ended during escape sequence", exception_io);
    }
    qk_decompile_next_part(&i_part, n_parts, out_parts, w_ptr, part_ptr);
    if (i_part != n_parts) sub_heap {
        throw(concs("key had less parts (", i_part, ") than specified (", n_parts, ")"), exception_io);
    }
    return escape(key_mem);
}}
