#include "runtime_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(char* error) {
    fprintf(stderr, "embedded runtime smoke test failed: %s\n",
        error ? error : "unknown error");
    cha_string_free(error);
    return 1;
}

static volatile int got_delivery;
static char last_delivery[8192];

static void on_delivery(void* context, const char* connection, const char* json) {
    (void)context;
    (void)connection;
    if (!json) return;
    snprintf(last_delivery, sizeof last_delivery, "%s", json);
    got_delivery = 1;
}

static int reopen_native(const char* config, const char* resources, const char* what) {
    char* error = NULL;
    int32_t password_error = 0;
    ChaRuntime* runtime = cha_runtime_create(
        config, resources, "", "", 0, &password_error, &error);
    if (!runtime) {
        fprintf(stderr, "embedded runtime smoke test failed: %s: %s\n",
            what, error ? error : "unknown error");
        cha_string_free(error);
        return 0;
    }
    const int ok = cha_runtime_is_native(runtime) == 1
        && cha_runtime_port(runtime) == 0;
    cha_runtime_request_shutdown(runtime);
    cha_runtime_join_shutdown(runtime, 2000);
    cha_runtime_destroy(runtime);
    if (!ok) {
        fprintf(stderr,
            "embedded runtime smoke test failed: %s opened a listener\n", what);
        return 0;
    }
    return 1;
}

static int write_file(const char* path, const char* contents) {
    FILE* file = fopen(path, "w");
    if (!file) return 0;
    if (fputs(contents, file) == EOF) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

int main(int argc, const char* argv[]) {
    if (argc != 3) return 2;
    if (unsetenv("CHA_R2_URL") != 0
        || unsetenv("CHA_R2_ACCESS_KEY_ID") != 0
        || unsetenv("CHA_R2_SECRET_ACCESS_KEY") != 0
        || unsetenv("CHA_DEV_ORIGIN") != 0
        || unsetenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS") != 0) {
        return 1;
    }

    char* error = NULL;
    int32_t password_error = 0;
    ChaRuntime* rejected = cha_runtime_create(
        argv[1], argv[2], "token", "", 1, &password_error, &error);
    if (rejected) {
        fprintf(stderr,
            "embedded runtime smoke test failed: http_mode started a runtime\n");
        cha_runtime_destroy(rejected);
        return 1;
    }
    cha_string_free(error);
    error = NULL;

    ChaRuntime* runtime = cha_runtime_create(
        argv[1], argv[2], "", "", 0, &password_error, &error);
    if (!runtime) return fail(error);
    if (cha_runtime_port(runtime) != 0 || cha_runtime_is_native(runtime) != 1) {
        fprintf(stderr,
            "embedded runtime smoke test failed: native runtime opened a listener\n");
        cha_runtime_destroy(runtime);
        return 1;
    }
    if (!cha_runtime_can_modify(runtime) || cha_runtime_can_transfer_r2(runtime)) {
        fprintf(stderr,
            "embedded runtime smoke test failed: incorrect menu capabilities\n");
        cha_runtime_destroy(runtime);
        return 1;
    }
    if (setenv("CHA_R2_URL", "set", 1) != 0
        || setenv("CHA_R2_ACCESS_KEY_ID", "set", 1) != 0
        || setenv("CHA_R2_SECRET_ACCESS_KEY", "set", 1) != 0
        || cha_runtime_can_transfer_r2(runtime)) {
        fprintf(stderr,
            "embedded runtime smoke test failed: environment enabled R2\n");
        cha_runtime_destroy(runtime);
        return 1;
    }

    uint64_t file_count = 0;
    if (cha_runtime_export_configuration(runtime, &file_count, &error) != 1) {
        cha_runtime_destroy(runtime);
        return fail(error);
    }
    if (cha_runtime_import_configuration(runtime, &file_count, &error) != 1) {
        cha_runtime_destroy(runtime);
        return fail(error);
    }

    got_delivery = 0;
    last_delivery[0] = '\0';
    cha_runtime_set_delivery_callback(runtime, on_delivery, NULL);
    char* connection = cha_runtime_open_connection(runtime, &error);
    if (!connection) {
        cha_runtime_destroy(runtime);
        return fail(error);
    }
    char request[512];
    if (snprintf(
            request,
            sizeof request,
            "{\"connection_id\":\"%s\",\"id\":1,\"context_epoch\":0,"
            "\"method\":\"bridge.info\",\"params\":{}}",
            connection)
        >= (int)sizeof request) {
        cha_string_free(connection);
        cha_runtime_destroy(runtime);
        fprintf(stderr, "embedded runtime smoke test failed: request too large\n");
        return 1;
    }
    cha_runtime_handle_message(runtime, connection, request);
    for (int attempt = 0; attempt < 100 && !got_delivery; ++attempt) {
        usleep(20000);
    }
    cha_string_free(connection);
    if (!got_delivery || strstr(last_delivery, "\"protocol_version\":1") == NULL
        || strstr(last_delivery, "\"ok\":true") == NULL) {
        fprintf(stderr,
            "embedded runtime smoke test failed: bridge.info was not delivered\n");
        cha_runtime_destroy(runtime);
        return 1;
    }

    cha_runtime_request_shutdown(runtime);
    if (cha_runtime_join_shutdown(runtime, 4000) != 1) {
        fprintf(stderr, "embedded runtime smoke test failed: shutdown stalled\n");
        cha_runtime_destroy(runtime);
        return 1;
    }
    cha_runtime_destroy(runtime);

    char app_path[1024];
    if (snprintf(app_path, sizeof app_path, "%s/app.toml", argv[1])
        >= (int)sizeof app_path) {
        return 1;
    }
    if (!write_file(
            app_path,
            "vault = \"Default\"\n"
            "mirror = \"mirror\"\n"
            "modify = \"modify\"\n"
            "[web]\nhost = \"127.0.0.1\"\nport = 8086\n"
            "[logging]\nfile = \"logs/cha.log\"\nlevel = \"info\"\n")
        || !reopen_native(argv[1], argv[2], "valid [web]")) {
        return 1;
    }
    if (!write_file(
            app_path,
            "vault = \"Default\"\n"
            "mirror = \"mirror\"\n"
            "modify = \"modify\"\n"
            "[web]\nhost = 1\nport = \"bad\"\nextra = true\n"
            "[logging]\nfile = \"logs/cha.log\"\nlevel = \"info\"\n")
        || !reopen_native(argv[1], argv[2], "obsolete [web]")) {
        return 1;
    }
    return 0;
}
