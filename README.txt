
# Small Shell

## Introduction

A small shell implementation. Features include:

- A prompt
- Blank lines and comments
- Variable expansion of `$$` into the current process ID
- Built-in implementations of `exit`, `cd`, and `status`
- I/O redirection
- Foreground and background processes
- Custom handlers for `SIGINT` and `SIGTSTP`

## Compilation

```bash
gcc --std=gnu99 -g -o small-shell small-shell.c
```

