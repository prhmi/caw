#include "../src/pd_patch_editor.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    using namespace pd_patch_editor;
    const std::string patch =
        "#N canvas 200 100 500 320 10;\r\n"
        "#X obj 90 75 osc~ 440;\r\n"
        "#X obj 90 120 *~ 0.08;\r\n"
        "#X msg 15 25 hello \\, world;\r\n"
        "#X text 10 30 note \\; test;\r\n"
        "#X connect 0 0 1 0;\r\n"
        "#N canvas 0 0 100 100 inner 0;\r\n"
        "#X obj -20 +50 r \\$0-thing;\r\n"
        "#X restore 5 6 pd inner;\r\n";
    const std::string simple =
        "osc~ 440;\n*~ 0.08;\nhello \\, world;\nnote \\; test;\nr \\$0-thing;\n";
    assert(readableText(patch) == simple);
    std::string out, error;
    assert(applyReadable(patch, simple, out, error) && out == patch);
    const std::string edited =
        "osc~ 220;\n*~ 0.04;\nhello \\, world;\nnote \\; test;\nr $1-thing;\n";
    assert(applyReadable(patch, edited, out, error));
    auto expected = patch;
    expected.replace(expected.find("osc~ 440"), 8, "osc~ 220");
    expected.replace(expected.find("*~ 0.08"), 7, "*~ 0.04");
    expected.replace(expected.find("r \\$0-thing"), 11, "r \\$1-thing");
    assert(out == expected);
    assert(!applyReadable(patch, "osc~ 440;\n", out, error));
    assert(!applyReadable(patch, simple + "new object;\n", out, error));
    assert(!applyReadable(patch, "osc~ 440\n*~ 0.08;\nhello;\nnote;\nr x;\n", out, error));
    assert(!applyReadable(patch, "osc~ 440; evil;\n*~ 0.08;\nhello;\nnote;\nr x;\n", out, error));
    assert(!applyReadable(patch, "osc~ 440\\;\n*~ 0.08;\nhello;\nnote;\nr x;\n", out, error));

    auto file = std::filesystem::temp_directory_path() / "caw-pd-editor-test.pd";
    { std::ofstream f(file, std::ios::binary); f << patch; }
    Buffer b;
    assert(load(file, b) && !b.dirty());
    b.readable = edited;
    assert(b.dirty() && save(file, b) && !b.dirty());
    std::string disk;
    assert(read(file, disk, error) && disk == expected);
    b.readable = simple;
    { std::ofstream f(file, std::ios::binary); f << patch; }
    assert(!save(file, b) && b.dirty() && b.text == expected);
    assert(read(file, disk, error) && disk == patch);
    std::filesystem::remove(file);
    std::cout << "PD editor tests passed\n";
}
