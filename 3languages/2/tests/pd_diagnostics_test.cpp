#include "../src/pd_diagnostics.h"
#include "../src/pd_patch_editor.h"

#include <cassert>

int main() {
    const auto missingObject = pd_diagnostics::inferEditorLine(
        "osc~ 440;\nnotAnExternal~;\n*~ 0.5;\n",
        "error: couldn't create notAnExternal~");
    assert(missingObject.line == 2 && missingObject.approximate);

    const auto quotedObject = pd_diagnostics::inferEditorLine(
        "osc~ 440;\nbroken~ 1;\n",
        "error: couldn't create 'broken~'");
    assert(quotedObject.line == 2 && quotedObject.approximate);

    const auto compilerContext = pd_diagnostics::inferEditorLine(
        "SC~ 440;\n*~ 0.08;\ndac~ 1 2;\n",
        "error: ... couldn't create\nPD object text: verbose(4): SC~ 440");
    assert(compilerContext.line == 1 && !compilerContext.approximate);

    const auto badMessage = pd_diagnostics::inferEditorLine(
        "change;\nelvis;\n",
        "error: change: no method for 'elvis'");
    assert(badMessage.line == 2 && badMessage.approximate);

    const auto ambiguous = pd_diagnostics::inferEditorLine(
        "notAnExternal~;\nnotAnExternal~;\n",
        "couldn't create notAnExternal~");
    assert(ambiguous.line == 0 && ambiguous.candidates.size() == 2);

    const auto genericCreate = pd_diagnostics::inferEditorLine(
        "osc~ 440;\n*~ 0.08;\ndac~ 1 2;\n",
        "error: ... couldn't create", {1, 2, 3});
    assert(genericCreate.line == 0 && genericCreate.candidates.size() == 3);

    const std::string patch =
        "#N canvas 0 0 200 200 10;\n"
        "#X obj 10 10 osc~ 440;\n"
        "#X connect 0 0 1 0;\n"
        "#X obj 10 50 broken~;\n";
    const auto mapped = pd_diagnostics::inferEditorLine(
        pd_patch_editor::readableText(patch), "error: couldn't create broken~");
    assert(mapped.line == 2);

    const auto noClue = pd_diagnostics::inferEditorLine(
        "osc~ 440;\n*~ 0.5;\n",
        "PD audio output stopped.");
    assert(noClue.line == 0);
}
