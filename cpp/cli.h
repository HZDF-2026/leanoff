// cli.h — command line interface, byte-compatible with the Python reference's
// argparse surface (the Go port uses a hand-rolled usage text instead):
//
//	leanoff verify [options]
//	leanoff build  [options]
//
// Reproduced argparse behaviors: frozen help/usage texts, unique-prefix
// option abbreviations, ambiguity detection, --opt=value / --opt value forms,
// negative numbers and a lone "-" accepted as option values, choices and
// int validation, left-to-right processing (help and errors fire where they
// appear), and the exact error precedence between the top-level parser and
// the subparsers.
//
// Exit codes: 0 ok, 1 runtime failure, 2 usage error.
#ifndef LEANOFF_CLI_H
#define LEANOFF_CLI_H

#include <ostream>
#include <string>
#include <vector>

namespace leanoff {

// CLI entry: args without the program name, exit code returned. Never throws:
// runtime errors are caught here, printed to err, and turned into exit 1.
int cliMain(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace leanoff

#endif  // LEANOFF_CLI_H
