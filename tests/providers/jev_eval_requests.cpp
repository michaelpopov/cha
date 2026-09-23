// Used by scripts/evaluate-jev.py. Reuse production instructions and option order.
#include "providers/jev.h"
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        std::ifstream source(argv[1]);
        auto cases = nlohmann::ordered_json::parse(source);
        for (auto& item : cases) {
            cha::JevRequestInput input;
            input.prompt = item.at("prompt").get<std::string>();
            for (const auto& name : item.at("roster")) {
                const auto key = "character_" + std::to_string(input.characters.size() + 1);
                input.characters.push_back({key, key, name.get<std::string>()});
            }
            item["request"] = cha::make_jev_body(input);
        }
        std::cout << cases.dump(2) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
