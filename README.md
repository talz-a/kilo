# Kilo: Simple C Text Editor

A lightweight, terminal-based text editor written in **C** with no external dependencies. Supports basic editing, file I/O, and syntax highlighting in under 1,000 lines of code.

> [!WARNING]
> This software is not production ready and is intended for learning and experimentation.

## Features

* Minimal design with no external libraries
* Raw terminal input/output handling
* Syntax highlighting
* File open, edit, and save functionality

## Getting Started

### Prerequisites

* C compiler (e.g., gcc or clang)
* Make

### Build & Run

```bash
git clone https://github.com/talz-a/kilo
cd kilo
make
./kilo <file_name>
