# PD clip simple editor

The PD clip editor displays the contents of single-line objects, messages,
and comments, for example `osc~ 440;`. Their `#X` types and coordinates,
canvas records, connections, and other PD records remain in the `.pd` file.
Each visible line edits the corresponding original record.

Keep lines in their original order, keep the final semicolon, and do not
add or remove lines in this view. Use **Open in Pure Data** from the clip's
menu to change patch structure or edit other record types. Changing an
object's type or ports may require adjusting connections in Pure Data.
Edits autosave after a short pause; external changes are not overwritten.

To run the focused test:

```sh
g++ -std=c++17 tests/pd_patch_editor_test.cpp -o pd_patch_editor_test
./pd_patch_editor_test
```
