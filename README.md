# Cyron

A tiny interpreted programming language: **Python's heart, C's curly braces.**

- Curly braces for blocks — **no semicolons** (a newline ends a statement)
- Indentation never matters
- No manual memory management, **no leaks** — the interpreter tracks every
  allocation and releases all of it
- Variables, functions, closures, **classes with inheritance**, lists,
  strings, `if/elif/else`, `while`, `for-in`, and 24 built-in functions

## Files

| File            | What it is                                                    |
|-----------------|---------------------------------------------------------------|
| `cyron.c`     | The whole language: lexer, parser, interpreter (single C file)|
| `cyron.exe`   | The compiled interpreter                                      |
| `ide.py`        | The Cyron IDE (Python/tkinter) with built-in documentation  |
| `examples/`     | Sample `.cy` programs                                       |

## Quick start

```bash
python D:/Cyron/ide.py
```

Press **F5** to run, **Ctrl+D** for the full language documentation.

Or from a terminal:

```bash
D:/Cyron/cyron.exe D:/Cyron/examples/fizzbuzz.cy
```

Run `cyron.exe` with no arguments for an interactive REPL.

To rebuild the interpreter:

```bash
gcc D:/Cyron/cyron.c -o D:/Cyron/cyron.exe -O2 -lm
```

## Taste of the language

```text
class Animal {
    fn init(name) {
        this.name = name
    }
    fn speak() {
        print(this.name, "makes a sound")
    }
}

class Dog : Animal {
    fn speak() {
        print(this.name, "barks!")
    }
}

let pets = [Dog("Rex"), Animal("Generic")]
for p in pets {
    p.speak()
}

fn fib(n) {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
print("fib(15) =", fib(15))
```

Full documentation lives inside the IDE (Ctrl+D).
