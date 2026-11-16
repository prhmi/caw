# CAW (Code Audio Workstation)

CAW is a code-based audio workstation that combines the power of **Csound** with a graphical interface for composition, sound design, and live performance. It allows you to write and organize audio code, manage projects, and export high-quality audio, bridging the gap between text-based synthesis and a traditional DAW workflow.

---

## ✨ Key Features

*   **Csound-Centric Engine**: Built around Csound for powerful, code-driven audio synthesis and processing.
*   **Integrated Code Editor**: A dedicated editor with separate program and code font size settings, tailored for writing and managing Csound code.
*   **Visual Score Mapping**: A "Map Score" view that shows explicit VST outputs and literal `cawsa`/`cawra` calls from Csound clips on a read-only patch board.
*   **Intuitive UI**: Features a maximized startup with a welcome screen, draggable master gain control (with Ctrl-click to type values), and collapsible window title bars (double-click to toggle).
*   **Efficient Workflow**: Includes an idle mode that reduces frame rates to save resources (10 fps on Windows, 20 fps on Linux/macOS) and wakes up instantly on input.
*   **MP3 Export (Windows)**: Bundles a verified FFmpeg executable for MP3 rendering without requiring a separate FFmpeg installation. Csound is still used for the actual audio render.
*   **Project Compatibility**: Uses project format 51. It can read older formats, but older builds may not be able to read projects saved in format 51.

---

## 🛠️ Building from Source

### Prerequisites

*   **CMake** (for dependency management)
*   **Csound** (required for audio rendering)
*   **C++ Compiler** (supporting C++17 or later)

### Build Instructions

1.  **Clone the repository**:
    ```bash
    git clone https://github.com/prhmi/caw.git
    cd caw