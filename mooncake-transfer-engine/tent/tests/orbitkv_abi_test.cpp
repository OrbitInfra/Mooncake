// Copyright 2026 KVCache.AI
// Licensed under the Apache License, Version 2.0.
#include "tent/transfer_engine.h"

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <cstddef>
#include <type_traits>

// OrbitKV's repr(C) layouts and dynamic symbol types, read from
// crates/orbitkv-mooncake-sys/src/lib.rs at the v0.3.14 integration upgrade.
static_assert(sizeof(void*) == 8);
static_assert(sizeof(tent_request_t) == 88);
static_assert(offsetof(tent_request_t, expected_source_epoch) == 48);
static_assert(sizeof(tent_status_t) == 16);
static_assert(offsetof(tent_status_t, transferred_bytes) == 8);
static_assert(sizeof(tent_memory_options_t) == 344);
static_assert(offsetof(tent_memory_options_t, shm_offset) == 328);
static_assert(offsetof(tent_memory_options_t, internal) == 336);
static_assert(sizeof(tent_notifi_record_t) == 4360);
static_assert(sizeof(tent_notifi_info) == 16);
static_assert(sizeof(tent_nic_load_stat_t) == 80);

#define CHECK_ABI(name, ...) \
    static_assert(std::is_same_v<decltype(&name), __VA_ARGS__>)
CHECK_ABI(tent_set_config, void (*)(const char*, const char*));
CHECK_ABI(tent_create_engine, void* (*)());
CHECK_ABI(tent_destroy_engine, void (*)(void*));
CHECK_ABI(tent_available, int (*)(void*));
CHECK_ABI(tent_segment_name, int (*)(void*, char*, size_t));
CHECK_ABI(tent_open_segment, int (*)(void*, uint64_t*, const char*));
CHECK_ABI(tent_close_segment, int (*)(void*, uint64_t));
CHECK_ABI(tent_register_memory_ex,
          int (*)(void*, void*, size_t, tent_memory_options_t*));
CHECK_ABI(tent_unregister_memory, int (*)(void*, void*, size_t));
CHECK_ABI(tent_close_source, int (*)(void*, uint64_t, char*, uint64_t*));
CHECK_ABI(tent_source_epoch, int (*)(void*, char*, size_t));
CHECK_ABI(tent_validate_remote_epoch, int (*)(void*, uint64_t, const char*));
CHECK_ABI(tent_allocate_batch, uint64_t (*)(void*, size_t));
CHECK_ABI(tent_submit, int (*)(void*, uint64_t, tent_request_t*, size_t));
CHECK_ABI(tent_submit_notif, int (*)(void*, uint64_t, tent_request_t*, size_t,
                                     const char*, const char*));
CHECK_ABI(tent_task_status, int (*)(void*, uint64_t, size_t, tent_status_t*));
CHECK_ABI(tent_overall_status, int (*)(void*, uint64_t, tent_status_t*));
CHECK_ABI(tent_cancel_task, int (*)(void*, uint64_t, size_t));
CHECK_ABI(tent_free_batch, int (*)(void*, uint64_t));
CHECK_ABI(tent_recv_notifs, int (*)(void*, tent_notifi_info*));
CHECK_ABI(tent_free_notifs, void (*)(tent_notifi_info*));
CHECK_ABI(tent_send_notifs, int (*)(void*, uint64_t, const char*, const char*));
CHECK_ABI(tent_get_nic_load_stats,
          int (*)(void*, tent_nic_load_stat_t*, size_t*));
#undef CHECK_ABI

TEST(OrbitKvAbi, SharedLibraryExportsEveryConsumedSymbol) {
    void* library = dlopen(TENT_SHARED_PATH, RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(library, nullptr) << dlerror();
    for (const char* name :
         {"tent_set_config",        "tent_create_engine",
          "tent_destroy_engine",    "tent_available",
          "tent_segment_name",      "tent_open_segment",
          "tent_close_segment",     "tent_register_memory_ex",
          "tent_unregister_memory", "tent_close_source",
          "tent_source_epoch",      "tent_validate_remote_epoch",
          "tent_allocate_batch",    "tent_submit",
          "tent_submit_notif",      "tent_task_status",
          "tent_overall_status",    "tent_cancel_task",
          "tent_free_batch",        "tent_recv_notifs",
          "tent_free_notifs",       "tent_send_notifs",
          "tent_get_nic_load_stats"}) {
        EXPECT_NE(dlsym(library, name), nullptr) << name;
    }
    EXPECT_EQ(dlclose(library), 0);
}
