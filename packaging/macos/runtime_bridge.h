#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ChaRuntime ChaRuntime;

// Delivery JSON is valid only for the duration of the callback. Copy it if
// the host posts the work to another thread. The callback runs on the runtime
// pump thread, never the platform UI thread.
typedef void (*cha_runtime_delivery_fn)(
    void* context,
    const char* connection_id,
    const char* json);

// Returned strings belong to the caller and must be released with
// cha_string_free().
//
// http_mode 0 starts Application and the common bridge with no listener.
// http_mode 1 is the temporary HTTP runtime: loopback on an OS-chosen port,
// answering only requests carrying access_token as a CHA_RUNTIME cookie.
// Native mode ignores access_token. config_path is the configuration directory.
ChaRuntime* cha_runtime_create(
    const char* config_path,
    const char* resource_path,
    const char* access_token,
    const char* vault_password,
    int32_t http_mode,
    int32_t* password_error,
    char** error);
// Returns 1 when the selected vault is protected, 0 when it is not, and -1
// when the application configuration could not be loaded.
// Also returns the selected vault's name through vault_name when provided.
int32_t cha_runtime_requires_password(
    const char* config_path,
    const char* resource_path,
    char** vault_name,
    char** error);
void cha_runtime_destroy(ChaRuntime* runtime);
int32_t cha_runtime_port(const ChaRuntime* runtime);
int32_t cha_runtime_is_native(const ChaRuntime* runtime);
int32_t cha_runtime_can_modify(const ChaRuntime* runtime);
int32_t cha_runtime_can_transfer_r2(const ChaRuntime* runtime);
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
int32_t cha_runtime_import_configuration(
    ChaRuntime* runtime,
    uint64_t* file_count,
    char** error);
int32_t cha_runtime_export_configuration(
    ChaRuntime* runtime,
    uint64_t* file_count,
    char** error);
// Hosts pick the destination on the UI thread, then call this off that thread
// after revalidating context. Writes through a temporary file and replaces
// the destination only after the write succeeds.
int32_t cha_runtime_save_file(
    ChaRuntime* runtime,
    uint64_t context_epoch,
    const char* destination_utf8,
    const char* data,
    uint64_t size,
    char** error);
int32_t cha_runtime_context_epoch(
    const ChaRuntime* runtime,
    uint64_t* epoch);
// Export a session as markdown and write it through save_file.
int32_t cha_runtime_export_session(
    ChaRuntime* runtime,
    uint64_t context_epoch,
    const char* forum_id,
    const char* session_id,
    const char* destination_utf8,
    char** error);

void cha_runtime_set_delivery_callback(
    ChaRuntime* runtime,
    cha_runtime_delivery_fn callback,
    void* context);
char* cha_runtime_open_connection(ChaRuntime* runtime, char** error);
void cha_runtime_close_connection(
    ChaRuntime* runtime,
    const char* connection_id);
void cha_runtime_handle_message(
    ChaRuntime* runtime,
    const char* connection_id,
    const char* json);
void cha_runtime_request_shutdown(ChaRuntime* runtime);
// Joins the pump thread and application owners. Do not call from the UI
// thread. Returns 1 when every owner finished inside grace_ms.
int32_t cha_runtime_join_shutdown(ChaRuntime* runtime, int32_t grace_ms);

void cha_string_free(char* value);

#ifdef __cplusplus
}
#endif
