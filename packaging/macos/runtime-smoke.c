#include "runtime_bridge.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fail(char* error) {
    fprintf(stderr, "embedded runtime smoke test failed: %s\n",
        error ? error : "unknown error");
    cha_string_free(error);
    return 1;
}

// Returns the HTTP status the embedded runtime answers with, or -1. Written
// against sockets so the whole check stays inside one process, which is the
// point of running the runtime in-process in the first place.
static int http_status(int port, const char* path, const char* token) {
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection == -1) return -1;
    if (connect(connection, (struct sockaddr*)&address, sizeof address) != 0) {
        close(connection);
        return -1;
    }

    char request[512];
    const int length = snprintf(request, sizeof request,
        "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n%s%s%s\r\n",
        path,
        token ? "Cookie: CHA_RUNTIME=" : "", token ? token : "",
        token ? "\r\n" : "");
    if (length < 0 || (size_t)length >= sizeof request
        || write(connection, request, (size_t)length) != length) {
        close(connection);
        return -1;
    }

    char response[64];
    const ssize_t received = read(connection, response, sizeof response - 1);
    close(connection);
    if (received <= 0) return -1;
    response[received] = '\0';

    int status = -1;
    if (sscanf(response, "HTTP/1.1 %d", &status) != 1) return -1;
    return status;
}

static int check(const char* what, int status, int expected) {
    if (status == expected) return 1;
    fprintf(stderr,
        "embedded runtime smoke test failed: %s answered %d, expected %d\n",
        what, status, expected);
    return 0;
}

int main(int argc, const char* argv[]) {
    if (argc != 4) return 2;
    if (setenv("OPENAI_API_KEY", "package-check", 1) != 0) return 1;

    static const char* const token = "package-private-token";
    char* error = NULL;
    if (!cha_runtime_import_initial_database(argv[1], argv[2], &error)) {
        return fail(error);
    }
    ChaRuntime* runtime = cha_runtime_create(argv[1], argv[3], token, &error);
    if (!runtime) return fail(error);

    const int port = cha_runtime_port(runtime);
    int served = port > 0;
    if (!served) {
        fprintf(stderr, "embedded runtime smoke test failed: no port\n");
    } else {
        // The bundle layout is what this checks: the browser application is
        // served out of CHA.app/Contents/Resources/web, and only to the
        // launcher's private cookie.
        served = check("/ without the cookie", http_status(port, "/", NULL), 404)
            & check("/health", http_status(port, "/health", token), 200)
            & check("/", http_status(port, "/", token), 200);
    }

    cha_runtime_destroy(runtime);
    return served ? 0 : 1;
}
