#include "../src/sc_diagnostics.h"

#include <cassert>
#include <string>

int main() {
    assert(sc_diagnostics::reportedLine("ERROR: syntax error\nline 3 char 8:") == 3);
    assert(sc_diagnostics::reportedLine("line 2:") == 2);
    assert(sc_diagnostics::reportedLine("line 2 is mentioned here") == 0);

    const std::string methodSource =
        "/* .r in this comment is ignored */\n"
        "\"another .r in a string\";\n"
        "someObject.r;\n";
    const auto method = sc_diagnostics::inferRuntimeLine(
        methodSource, "ERROR: Message 'r' not understood.");
    assert(method.line == 3 && method.approximate);

    const auto ambiguous = sc_diagnostics::inferRuntimeLine(
        "one.r;\ntwo.r;\n", "Message 'r' not understood.");
    assert(ambiguous.line == 0);

    const auto variable = sc_diagnostics::inferRuntimeLine(
        "var a = 1;\na = kr;\n", "Variable 'kr' not defined.");
    assert(variable.line == 2 && variable.approximate);

    const auto rateMethodIsNotAVariable = sc_diagnostics::inferRuntimeLine(
        "{ SinOsc.kr(440) ! 2 }.play;\n", "Variable 'kr' not defined.");
    assert(rateMethodIsNotAVariable.line == 0);
}
