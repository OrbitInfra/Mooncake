// Copyright 2026 KVCache.AI
// SPDX-License-Identifier: Apache-2.0

#include "tent/transfer_engine.h"

_Static_assert(sizeof(tent_file_options_t) == 20, "file options ABI");
_Static_assert(sizeof(tent_file_range_t) == 16, "file range ABI");
_Static_assert(sizeof(tent_file_owner_t) == 24, "file owner ABI");
_Static_assert(sizeof(tent_file_result_t) == 16, "file receipt ABI");
_Static_assert(sizeof(tent_file_element_t) == 24, "file element ABI");
_Static_assert(offsetof(tent_file_element_t, address) == 8, "element address");
_Static_assert(offsetof(tent_file_element_t, length) == 16, "element length");

/* Compiled as C, linked against the public shared library. */
int file_c_api_read(tent_engine_t engine, tent_file_id_t file,
                    tent_file_range_t* ranges, size_t count,
                    const tent_file_owner_t* owner,
                    tent_file_result_t* result) {
    tent_file_batch_t batch = 0;
    int rc = tent_file_submit(engine, file, OPCODE_READ, 4096, ranges, count,
                              owner, &batch);
    if (rc != TENT_FILE_OK) return rc;
    rc = tent_file_drain(engine, batch, 5000, 0, result);
    if (!result->drained) return rc;
    int released = tent_file_release_batch(engine, batch);
    return released == TENT_FILE_OK ? rc : released;
}

int file_c_api_vector(tent_engine_t engine, tent_file_id_t file, int opcode,
                      uint64_t offset, const tent_file_element_t* elements,
                      size_t count, const tent_file_owner_t* owner,
                      tent_file_result_t* result) {
    tent_file_batch_t batch = 0;
    int rc = tent_file_submitv(engine, file, opcode, offset, elements, count,
                               owner, &batch);
    if (rc != TENT_FILE_OK) return rc;
    rc = tent_file_drain(engine, batch, 5000, 0, result);
    if (!result->drained) return rc;
    int released = tent_file_release_batch(engine, batch);
    return released == TENT_FILE_OK ? rc : released;
}
