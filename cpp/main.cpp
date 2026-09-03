// main.cpp — leanoff CLI entry point. See cli.h.
#include <iostream>
#include <string>
#include <vector>

#include "cli.h"

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; i++) {
        args.emplace_back(argv[i]);
    }
    return leanoff::cliMain(args, std::cout, std::cerr);
}
