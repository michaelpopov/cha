#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ChaRuntime ChaRuntime;

// Returned error strings belong to the caller and must be released with
// cha_string_free().
//
// The runtime always listens on 127.0.0.1 on an operating-system-chosen port,
// whatever the config file's [web] section says, and answers only requests
// carrying access_token as a CHA_RUNTIME cookie. Read the port back with
// cha_runtime_port().
ChaRuntime* cha_runtime_create(
    const char* config_path,
    const char* resource_path,
    const char* access_token,
    char** error);
void cha_runtime_destroy(ChaRuntime* runtime);
int32_t cha_runtime_port(const ChaRuntime* runtime);

// Seeds the database named by the config file from seed_path. Does nothing
// and reports success when that database already exists, so the launcher can
// call it on every start without knowing which file the config names.
int32_t cha_runtime_import_initial_database(
    const char* config_path,
    const char* seed_path,
    char** error);
// Return 1 on success and 0 on a failure the caller can retry. -1 means the
// workspace database could not be reopened afterwards: this process can no
// longer serve and must quit.
int32_t cha_runtime_upload(
    ChaRuntime* runtime,
    uint64_t* byte_count,
    char** error);
int32_t cha_runtime_download(
    ChaRuntime* runtime,
    uint64_t* byte_count,
    char** error);

void cha_string_free(char* value);

#ifdef __cplusplus
}
#endif
