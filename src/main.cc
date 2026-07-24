#include <iostream>
#include <vector>

#include "config_eval.hh"
#include "pure.hh"

int main(int argc, char ** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);

    auto parsed = nixciconfig::parseArgs(arguments);
    if (std::holds_alternative<nixciconfig::ArgsHelp>(parsed)) {
        std::cout << nixciconfig::usage();
        return 0;
    }
    if (auto * error = std::get_if<nixciconfig::ArgsError>(&parsed)) {
        std::cerr << error->message << "\n\n" << nixciconfig::usage();
        return 2;
    }

    nixciconfig::initNixRuntime();
    return nixciconfig::run(std::get<nixciconfig::Args>(parsed), std::cout, std::cerr);
}
