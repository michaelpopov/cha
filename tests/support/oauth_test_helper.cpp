#include "providers/openai_oauth.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

char receive(int descriptor) {
    char value{};
    ssize_t count;
    do { count = read(descriptor, &value, 1); } while (count == -1 && errno == EINTR);
    if (count != 1) throw std::runtime_error("OAuth test command pipe closed");
    return value;
}

void send(int descriptor, char value) {
    ssize_t count;
    do { count = write(descriptor, &value, 1); } while (count == -1 && errno == EINTR);
    if (count != 1) throw std::runtime_error("OAuth test event pipe closed");
}

} // namespace

// A fresh process with an injected transport: no real credentials or network.
// L = loaded, A = command accepted, R = refresh entered, S = new credentials,
// O = old credentials, E = failure, D = disconnected. The parent releases a
// paused transport with 'c', after checking what the other process is doing.
int main(int argc, char** argv) {
    if (argc != 6) return 1;
    try {
        const int commands = std::stoi(argv[2]);
        const int events = std::stoi(argv[3]);
        const auto now = std::chrono::system_clock::time_point{
            std::chrono::seconds{std::stoll(argv[5])}};
        cha::OpenAiOAuth oauth{
            argv[1],
            [&](const cha::OpenAiOAuthHttpRequest&) {
                send(events, 'R');
                if (receive(commands) != 'c') throw std::runtime_error("Expected continue");
                std::ifstream input(argv[4]);
                const auto response = nlohmann::json::parse(input);
                return cha::OpenAiOAuthHttpResponse{
                    response.at("status").get<long>(), response.at("body").get<std::string>()};
            },
            [now] { return now; }};
        send(events, 'L');
        while (true) {
            const char command = receive(commands);
            send(events, 'A');
            try {
                if (command == 'r') {
                    send(events, oauth.credentials().account_id == "acct_new" ? 'S' : 'O');
                } else if (command == 'd') {
                    send(events, oauth.disconnect().error ? 'E' : 'D');
                } else {
                    return 1;
                }
            } catch (const std::exception&) {
                send(events, 'E');
            }
        }
    } catch (const std::exception&) {
        return 1;
    }
}
