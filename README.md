# Kilo: C++ Text Editor

A lightweight, terminal-based text editor written in C++ with no external dependencies. Supports basic editing, file I/O, and syntax highlighting in under 1,000 lines of code.

> [!WARNING]
> This software is not production ready and is intended for learning and experimentation.

## Features

* Minimal design with no external libraries
* Raw terminal input/output handling
* Syntax highlighting for C/C++ files
* File open, edit, and save functionality
* Incremental search with match highlighting

## Getting Started

### Prerequisites

* C++23 compatible compiler (e.g., GCC 13+ or Clang 17+)
* CMake 3.28+

### Build & Run

```bash
git clone https://github.com/talz-a/kilo
cd kilo
cmake -B build
cmake --build build
./build/kilo <file_name>
```

### Keybindings

| Key | Action |
|-----|--------|
| Ctrl-S | Save |
| Ctrl-Q | Quit (press 3 times if unsaved changes) |
| Ctrl-F | Find |
| Arrow keys | Move cursor |
| Page Up/Down | Scroll |
| Home/End | Jump to start/end of line |
