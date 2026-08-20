"""
Cyron IDE — a small development environment for the Cyron language.

Features
    - Editor with syntax highlighting, line numbers, and Tab -> 4 spaces
    - Run (F5): executes the current file with the Cyron interpreter
    - Program Input box: text there is fed to input() calls via stdin
    - Built-in documentation panel (Ctrl+D or the "Docs" button)
    - Auto-builds cyron.exe from cyron.c with gcc if it is missing
    - Bundled example programs under the Examples menu

Run with:  python ide.py
"""

import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
from tkinter import filedialog, font, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- colors

COL = {
    "bg":        "#1e1e2e",
    "bg2":       "#181825",
    "bg3":       "#11111b",
    "fg":        "#cdd6f4",
    "dim":       "#6c7086",
    "accent":    "#89b4fa",
    "green":     "#a6e3a1",
    "red":       "#f38ba8",
    "yellow":    "#f9e2af",
    "peach":     "#fab387",
    "mauve":     "#cba6f7",
    "teal":      "#94e2d5",
    "sel":       "#45475a",
    "cursorline":"#2a2b3c",
}

KEYWORDS = ("let|fn|class|if|elif|else|while|for|in|return|break|continue|"
            "and|or|not|this|super")
CONSTS   = "true|false|nil"
BUILTINS = ("print|input|len|str|num|range|push|pop|type|clock|abs|floor|ceil|"
            "sqrt|pow|min|max|random|split|join|upper|lower|ord|chr")

HIGHLIGHT_RULES = [
    ("comment",  re.compile(r"(#|//)[^\n]*")),
    ("string",   re.compile(r"\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'")),
    ("keyword",  re.compile(r"\b(%s)\b" % KEYWORDS)),
    ("const",    re.compile(r"\b(%s)\b" % CONSTS)),
    ("builtin",  re.compile(r"\b(%s)\b(?=\s*\()" % BUILTINS)),
    ("defname",  re.compile(r"(?:\bfn\s+|\bclass\s+)(\w+)")),
    ("number",   re.compile(r"\b\d+(?:\.\d+)?\b")),
]

DEFAULT_PROGRAM = '''# Welcome to Cyron -- curly braces, no semicolons, no leaks.
# Press F5 to run.  Ctrl+D opens the documentation.

fn greet(name) {
    print("Hello, " + name + "!")
}

class Counter {
    fn init(start) {
        this.value = start
    }
    fn tick() {
        this.value += 1
        return this.value
    }
}

greet("world")
let c = Counter(10)
c.tick()
c.tick()
print("counter is now", c.tick())

for i in range(1, 6) {
    if i % 2 == 0 { print(i, "is even") }
    else { print(i, "is odd") }
}
'''

EXAMPLES = {
    "Hello world": '''# The classic.
print("Hello, Cyron!")

let name = input("What is your name? ")
if len(name) > 0 {
    print("Nice to meet you, " + name + "!")
} else {
    print("Staying mysterious, I see.")
}
''',
    "FizzBuzz": '''# FizzBuzz, Cyron style.
for i in range(1, 31) {
    if i % 15 == 0 { print("FizzBuzz") }
    elif i % 3 == 0 { print("Fizz") }
    elif i % 5 == 0 { print("Buzz") }
    else { print(i) }
}
''',
    "OOP: shapes": '''# Classes, inheritance, and super.
class Shape {
    fn init(name) {
        this.name = name
    }
    fn area() { return 0 }
    fn describe() {
        print(this.name, "has area", this.area())
    }
}

class Circle : Shape {
    fn init(r) {
        super.init("circle")
        this.r = r
    }
    fn area() {
        return 3.14159265 * this.r * this.r
    }
}

class Rect : Shape {
    fn init(w, h) {
        super.init("rect")
        this.w = w
        this.h = h
    }
    fn area() { return this.w * this.h }
}

let shapes = [Circle(2), Rect(3, 4), Circle(1)]
for s in shapes {
    s.describe()
}
''',
    "Closures": '''# Functions are values and capture their environment.
fn make_counter() {
    let count = 0
    fn tick() {
        count += 1
        return count
    }
    return tick
}

let a = make_counter()
let b = make_counter()
a()
a()
print("a:", a(), " b:", b())   # a: 3  b: 1

fn twice(f, x) { return f(f(x)) }
fn double(n) { return n * 2 }
print("twice double 5 =", twice(double, 5))
''',
    "Guess the number": '''# A little game using input() -- type guesses in the
# "Program Input" box below (one per line) before pressing Run.
let secret = floor(random() * 100) + 1
let tries = 0
let won = false

while tries < 7 {
    let text = input("Guess (1-100): ")
    if text == "" { break }
    let g = num(text)
    tries += 1
    if g == secret {
        print("Correct! Got it in", tries, "tries.")
        won = true
        break
    } elif g < secret {
        print("Higher...")
    } else {
        print("Lower...")
    }
}
if not won { print("Out of tries! It was", secret) }
''',
    "String tools": '''# Strings: index, iterate, split/join, and more.
let s = "The quick brown fox"
print(upper(s))
print(s[0], s[-1], len(s))

let words = split(s, " ")
print(words)
print(join(words, "_"))

let reversed = ""
for ch in s {
    reversed = ch + reversed
}
print(reversed)

print("na" * 8 + " batman")
''',
}

# ---------------------------------------------------------------- docs
# (tag, text) pairs; tags: h1, h2, p, code

DOCS = [
("h1", "The Cyron Language"),
("p",  "Cyron is a small interpreted language that mixes Python's feel with "
       "C's look: blocks use curly braces, statements end at the end of the "
       "line (no semicolons!), and indentation never matters. Memory is fully "
       "managed by the interpreter, so there is nothing to allocate or free "
       "and nothing to leak. Source files use the .cy extension."),

("h2", "Variables"),
("p",  "Declare with let. Assign with =. Compound assignment += -= *= /= works "
       "too. Using a variable that was never declared is an error, which "
       "catches typos early."),
("code",
'''let x = 10
let name = "Cyron"
let empty            # defaults to nil
x = x + 1
x += 1               # same thing, shorter'''),

("h2", "Types"),
("p",  "num (64-bit float, prints as an integer when whole), str, bool "
       "(true/false), nil, list, fn, and object instances. Check any value "
       "with type(v)."),
("code",
'''print(type(3))        # num
print(type("hi"))     # str
print(type([1, 2]))   # list
print(type(true))     # bool'''),

("h2", "Truthiness"),
("p",  "Python-style: false, nil, 0, \"\" (empty string), and [] (empty list) "
       "count as false. Everything else is true."),

("h2", "Operators"),
("p",  "Arithmetic: + - * / %.  Comparison: == != < <= > >=.  Logic: and, or, "
       "not (C spellings && || ! also work).  and/or short-circuit and return "
       "one of their operands, so 'x or fallback' is a handy default."),
("p",  "+ also concatenates: str + anything glues strings, list + list joins "
       "lists. \"ab\" * 3 repeats a string."),

("h2", "Statements and lines"),
("p",  "One statement per line -- the newline is the terminator, and there are "
       "no semicolons. Inside ( ) and [ ] you can break lines freely. A "
       "backslash at the end of a line also continues it. Comments start with "
       "# or //."),

("h2", "If / elif / else"),
("p",  "Conditions need no parentheses. Both elif and else if are accepted. "
       "Braces are always required."),
("code",
'''if score >= 90 {
    print("A")
} elif score >= 80 {
    print("B")
} else {
    print("keep going")
}'''),

("h2", "Loops"),
("p",  "while loops on a condition. for-in walks a list or the characters of "
       "a string; use range() for counting. break and continue work as "
       "expected."),
("code",
'''let i = 0
while i < 3 {
    i += 1
}

for x in range(5) { print(x) }        # 0..4
for x in range(2, 10, 2) { print(x) } # 2,4,6,8
for ch in "hey" { print(ch) }'''),

("h2", "Functions"),
("p",  "Declared with fn. return exits with a value (or nil). Functions are "
       "first-class values: pass them around, return them, store them. "
       "Closures capture the surrounding variables."),
("code",
'''fn add(a, b) {
    return a + b
}

fn make_counter() {
    let n = 0
    fn tick() {
        n += 1
        return n
    }
    return tick        # tick remembers n
}'''),

("h2", "Classes and OOP"),
("p",  "class defines a class. init is the constructor. this refers to the "
       "current object. Inherit with a colon (class Dog : Animal), and call "
       "the parent's version of a method with super.method(). Fields are "
       "created simply by assigning to this.field."),
("code",
'''class Animal {
    fn init(name) {
        this.name = name
    }
    fn speak() {
        print(this.name, "makes a sound")
    }
}

class Dog : Animal {
    fn init(name) {
        super.init(name)
        this.tricks = []
    }
    fn speak() {
        print(this.name, "barks!")
    }
}

let d = Dog("Rex")
d.speak()
print(d.name, type(d))'''),

("h2", "Lists"),
("p",  "Ordered, growable, and mixed-type. Index from 0; negative indices "
       "count from the end. push/pop grow and shrink them; lists are passed "
       "by reference."),
("code",
'''let items = [1, "two", true]
items[0] = 99
print(items[-1])       # true
push(items, "more")
let last = pop(items)
print(len(items))'''),

("h2", "Strings"),
("p",  "Double or single quotes; escapes \\n \\t \\\\ \\\" \\'. Index and "
       "loop over characters. Strings are immutable -- build new ones with +."),

("h2", "Built-in functions"),
("code",
'''print(...)          write values, space-separated
input(prompt?)      read a line from stdin -> str
len(x)              length of a str or list
str(x)  num(x)      convert to string / number
type(x)             type name as a string
range(stop) range(start, stop, step?)  -> list
push(list, v...)    append; returns the list
pop(list)           remove and return the last item
split(s, sep)       -> list     join(list, sep) -> str
upper(s) lower(s)   change case
ord(ch) chr(n)      character <-> code 0..255
abs floor ceil sqrt pow(a,b) min(...) max(...)
random()            float in [0, 1)
clock()             seconds of CPU time (for timing)'''),

("h2", "Errors"),
("p",  "Errors stop the program and report a line number, e.g. \"Runtime "
       "error [line 3]: division by zero\". Using an undeclared variable, "
       "calling with the wrong number of arguments, or indexing out of range "
       "are all caught and explained."),

("h2", "Running programs"),
("p",  "Press F5 here in the IDE, or from a terminal: cyron.exe "
       "myprogram.cy. Run cyron.exe with no arguments to get an "
       "interactive REPL. If your program calls input(), type the responses "
       "into the Program Input box (one line each) before running."),
]

# ---------------------------------------------------------------- IDE


class CyronIDE:
    def __init__(self, root):
        self.root = root
        self.file_path = None
        self.dirty = False
        self.proc_queue = queue.Queue()
        self.running = False

        root.title("Cyron IDE")
        root.geometry("1150x760")
        root.configure(bg=COL["bg"])

        self.mono = font.Font(family="Consolas", size=12)
        self.mono_small = font.Font(family="Consolas", size=10)
        self.ui_font = font.Font(family="Segoe UI", size=10)

        self._build_menu()
        self._build_toolbar()
        self._build_body()
        self._build_statusbar()
        self._bind_keys()

        self.editor.insert("1.0", DEFAULT_PROGRAM)
        self.editor.edit_reset()
        self._highlight()
        self._update_title()
        self.root.after(100, self._poll_proc_queue)

    # ---------- layout ----------

    def _build_menu(self):
        m = tk.Menu(self.root)
        filem = tk.Menu(m, tearoff=0)
        filem.add_command(label="New", accelerator="Ctrl+N", command=self.new_file)
        filem.add_command(label="Open...", accelerator="Ctrl+O", command=self.open_file)
        filem.add_command(label="Save", accelerator="Ctrl+S", command=self.save_file)
        filem.add_command(label="Save As...", command=lambda: self.save_file(force_dialog=True))
        filem.add_separator()
        filem.add_command(label="Exit", command=self.on_close)
        m.add_cascade(label="File", menu=filem)

        runm = tk.Menu(m, tearoff=0)
        runm.add_command(label="Run", accelerator="F5", command=self.run_program)
        runm.add_command(label="Rebuild interpreter", command=lambda: self._ensure_interpreter(rebuild=True))
        m.add_cascade(label="Run", menu=runm)

        exm = tk.Menu(m, tearoff=0)
        for name in EXAMPLES:
            exm.add_command(label=name, command=lambda n=name: self.load_example(n))
        m.add_cascade(label="Examples", menu=exm)

        helpm = tk.Menu(m, tearoff=0)
        helpm.add_command(label="Documentation", accelerator="Ctrl+D", command=self.toggle_docs)
        helpm.add_command(label="About", command=self.show_about)
        m.add_cascade(label="Help", menu=helpm)
        self.root.config(menu=m)

    def _button(self, parent, text, cmd, color=None):
        b = tk.Label(parent, text=text, bg=color or COL["bg3"], fg=COL["fg"],
                     font=self.ui_font, padx=12, pady=4, cursor="hand2")
        b.bind("<Button-1>", lambda e: cmd())
        b.bind("<Enter>", lambda e: b.config(bg=COL["sel"]))
        b.bind("<Leave>", lambda e: b.config(bg=color or COL["bg3"]))
        return b

    def _build_toolbar(self):
        bar = tk.Frame(self.root, bg=COL["bg2"], pady=4, padx=6)
        bar.pack(fill="x")
        self.run_btn = self._button(bar, "▶  Run (F5)", self.run_program)
        self.run_btn.config(fg=COL["green"])
        self.run_btn.pack(side="left", padx=(0, 6))
        self._button(bar, "New", self.new_file).pack(side="left", padx=3)
        self._button(bar, "Open", self.open_file).pack(side="left", padx=3)
        self._button(bar, "Save", self.save_file).pack(side="left", padx=3)
        docs = self._button(bar, "\U0001f4d6  Docs (Ctrl+D)", self.toggle_docs)
        docs.config(fg=COL["accent"])
        docs.pack(side="right", padx=3)

    def _build_body(self):
        outer = tk.PanedWindow(self.root, orient="horizontal", bg=COL["bg3"],
                               sashwidth=5, bd=0)
        outer.pack(fill="both", expand=True)

        # ----- left: editor over output -----
        left = tk.PanedWindow(outer, orient="vertical", bg=COL["bg3"],
                              sashwidth=5, bd=0)
        outer.add(left, stretch="always", minsize=400)
        self.outer = outer

        edframe = tk.Frame(left, bg=COL["bg"])
        left.add(edframe, stretch="always", minsize=200)

        self.linenums = tk.Text(edframe, width=4, padx=6, takefocus=0, bd=0,
                                bg=COL["bg2"], fg=COL["dim"], font=self.mono,
                                state="disabled", cursor="arrow")
        self.linenums.pack(side="left", fill="y")

        self.editor = tk.Text(edframe, wrap="none", undo=True, bd=0, padx=8,
                              pady=6, bg=COL["bg"], fg=COL["fg"],
                              insertbackground=COL["fg"],
                              selectbackground=COL["sel"],
                              font=self.mono, tabs=self.mono.measure("    "))
        self.editor.pack(side="left", fill="both", expand=True)

        yscroll = ttk.Scrollbar(edframe, orient="vertical",
                                command=self._scroll_both)
        yscroll.pack(side="right", fill="y")
        self.editor.configure(yscrollcommand=lambda a, b: self._sync_scroll(yscroll, a, b))
        self.linenums.configure(yscrollcommand=lambda a, b: None)

        for tag, color in [("keyword", COL["mauve"]), ("const", COL["peach"]),
                           ("builtin", COL["accent"]), ("string", COL["green"]),
                           ("number", COL["peach"]), ("comment", COL["dim"]),
                           ("defname", COL["yellow"])]:
            self.editor.tag_configure(tag, foreground=color)
        self.editor.tag_configure("cursorline", background=COL["cursorline"])

        # ----- bottom: output + program input -----
        bottom = tk.Frame(left, bg=COL["bg2"])
        left.add(bottom, minsize=120, height=210)

        nb_bar = tk.Frame(bottom, bg=COL["bg2"])
        nb_bar.pack(fill="x")
        tk.Label(nb_bar, text=" OUTPUT", bg=COL["bg2"], fg=COL["dim"],
                 font=self.ui_font).pack(side="left")
        tk.Label(nb_bar, text="PROGRAM INPUT (one line per input() call) ",
                 bg=COL["bg2"], fg=COL["dim"], font=self.ui_font).pack(side="right")

        panes = tk.Frame(bottom, bg=COL["bg2"])
        panes.pack(fill="both", expand=True)
        panes.columnconfigure(0, weight=3)
        panes.columnconfigure(1, weight=1)
        panes.rowconfigure(0, weight=1)

        self.output = tk.Text(panes, bd=0, padx=8, pady=6, bg=COL["bg3"],
                              fg=COL["fg"], font=self.mono_small,
                              state="disabled", wrap="word")
        self.output.grid(row=0, column=0, sticky="nsew")
        self.output.tag_configure("err", foreground=COL["red"])
        self.output.tag_configure("meta", foreground=COL["dim"])
        self.output.tag_configure("ok", foreground=COL["green"])

        self.stdin_box = tk.Text(panes, bd=0, padx=8, pady=6, bg=COL["bg2"],
                                 fg=COL["teal"], font=self.mono_small,
                                 insertbackground=COL["fg"], wrap="none")
        self.stdin_box.grid(row=0, column=1, sticky="nsew", padx=(4, 0))

        # ----- right: docs panel -----
        self.docs_frame = tk.Frame(outer, bg=COL["bg2"])
        self.docs_visible = False
        self._fill_docs()

    def _fill_docs(self):
        head = tk.Frame(self.docs_frame, bg=COL["bg2"])
        head.pack(fill="x")
        tk.Label(head, text="  Cyron Documentation", bg=COL["bg2"],
                 fg=COL["accent"], font=font.Font(family="Segoe UI", size=12,
                 weight="bold")).pack(side="left", pady=6)
        close = tk.Label(head, text="✕ ", bg=COL["bg2"], fg=COL["dim"],
                         cursor="hand2", font=self.ui_font)
        close.pack(side="right", padx=6)
        close.bind("<Button-1>", lambda e: self.toggle_docs())

        wrap = tk.Frame(self.docs_frame, bg=COL["bg2"])
        wrap.pack(fill="both", expand=True)
        txt = tk.Text(wrap, bd=0, padx=14, pady=10, bg=COL["bg2"],
                      fg=COL["fg"], wrap="word",
                      font=font.Font(family="Segoe UI", size=10))
        scroll = ttk.Scrollbar(wrap, orient="vertical", command=txt.yview)
        txt.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        txt.pack(side="left", fill="both", expand=True)

        txt.tag_configure("h1", font=font.Font(family="Segoe UI", size=16,
                          weight="bold"), foreground=COL["accent"],
                          spacing1=10, spacing3=6)
        txt.tag_configure("h2", font=font.Font(family="Segoe UI", size=12,
                          weight="bold"), foreground=COL["yellow"],
                          spacing1=14, spacing3=4)
        txt.tag_configure("p", spacing3=6)
        txt.tag_configure("code", font=self.mono_small, foreground=COL["teal"],
                          background=COL["bg3"], lmargin1=12, lmargin2=12,
                          spacing1=4, spacing3=8)

        for tag, body in DOCS:
            txt.insert("end", body + "\n", tag)
        txt.config(state="disabled")

    def _build_statusbar(self):
        bar = tk.Frame(self.root, bg=COL["bg3"])
        bar.pack(fill="x", side="bottom")
        self.status = tk.Label(bar, text="Ready", bg=COL["bg3"], fg=COL["dim"],
                               font=self.ui_font, anchor="w", padx=8)
        self.status.pack(side="left")
        self.pos_label = tk.Label(bar, text="Ln 1, Col 1", bg=COL["bg3"],
                                  fg=COL["dim"], font=self.ui_font, padx=8)
        self.pos_label.pack(side="right")

    def _bind_keys(self):
        e = self.editor
        e.bind("<KeyRelease>", self._on_key)
        e.bind("<ButtonRelease-1>", lambda ev: self._update_cursor_pos())
        e.bind("<Tab>", self._tab_key)
        e.bind("<Return>", self._return_key)
        self.root.bind("<F5>", lambda ev: self.run_program())
        self.root.bind("<Control-n>", lambda ev: self.new_file())
        self.root.bind("<Control-o>", lambda ev: self.open_file())
        self.root.bind("<Control-s>", lambda ev: self.save_file())
        self.root.bind("<Control-d>", lambda ev: self.toggle_docs())
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---------- editor behaviour ----------

    def _scroll_both(self, *args):
        self.editor.yview(*args)
        self.linenums.yview(*args)

    def _sync_scroll(self, scrollbar, first, last):
        scrollbar.set(first, last)
        self.linenums.yview_moveto(first)

    def _tab_key(self, event):
        self.editor.insert("insert", "    ")
        return "break"

    def _return_key(self, event):
        # keep the previous line's leading spaces; indent after '{'
        line = self.editor.get("insert linestart", "insert")
        indent = re.match(r"[ ]*", line).group(0)
        if line.rstrip().endswith("{"):
            indent += "    "
        self.editor.insert("insert", "\n" + indent)
        self.editor.see("insert")
        self._after_edit()
        return "break"

    def _on_key(self, event):
        if event.keysym in ("Up", "Down", "Left", "Right", "Prior", "Next",
                            "Home", "End", "Control_L", "Control_R",
                            "Shift_L", "Shift_R", "Alt_L", "Alt_R"):
            self._update_cursor_pos()
            return
        self.dirty = True
        self._after_edit()

    def _after_edit(self):
        self._update_title()
        self._update_cursor_pos()
        if hasattr(self, "_hl_job"):
            self.root.after_cancel(self._hl_job)
        self._hl_job = self.root.after(60, self._highlight)

    def _update_cursor_pos(self):
        line, col = self.editor.index("insert").split(".")
        self.pos_label.config(text="Ln %s, Col %d" % (line, int(col) + 1))
        self.editor.tag_remove("cursorline", "1.0", "end")
        self.editor.tag_add("cursorline", "insert linestart",
                            "insert lineend+1c")

    def _highlight(self):
        text = self.editor.get("1.0", "end-1c")
        for tag, _ in HIGHLIGHT_RULES:
            self.editor.tag_remove(tag, "1.0", "end")

        claimed = []          # (start, end) spans taken by comments/strings

        def overlaps(s, e):
            return any(not (e <= cs or s >= ce) for cs, ce in claimed)

        for tag, rx in HIGHLIGHT_RULES:
            for mt in rx.finditer(text):
                s, e = (mt.start(1), mt.end(1)) if mt.groups() and tag == "defname" \
                       else (mt.start(), mt.end())
                if tag in ("comment", "string"):
                    if overlaps(s, e):
                        continue
                    claimed.append((s, e))
                elif overlaps(s, e):
                    continue
                self.editor.tag_add(tag, "1.0+%dc" % s, "1.0+%dc" % e)

        self._update_linenums()

    def _update_linenums(self):
        count = int(self.editor.index("end-1c").split(".")[0])
        self.linenums.config(state="normal")
        self.linenums.delete("1.0", "end")
        self.linenums.insert("1.0", "\n".join(str(i) for i in range(1, count + 1)))
        self.linenums.config(state="disabled")
        self.linenums.yview_moveto(self.editor.yview()[0])

    def _update_title(self):
        name = os.path.basename(self.file_path) if self.file_path else "untitled.cy"
        star = " ●" if self.dirty else ""
        self.root.title("%s%s — Cyron IDE" % (name, star))

    # ---------- file ops ----------

    def _confirm_discard(self):
        if not self.dirty:
            return True
        ans = messagebox.askyesnocancel("Unsaved changes",
                                        "Save changes to the current file?")
        if ans is None:
            return False
        if ans:
            return self.save_file()
        return True

    def new_file(self):
        if not self._confirm_discard():
            return
        self.editor.delete("1.0", "end")
        self.file_path = None
        self.dirty = False
        self._update_title()
        self._highlight()

    def open_file(self):
        if not self._confirm_discard():
            return
        path = filedialog.askopenfilename(
            filetypes=[("Cyron files", "*.cy"), ("All files", "*.*")],
            initialdir=HERE)
        if not path:
            return
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
        self.editor.delete("1.0", "end")
        self.editor.insert("1.0", content)
        self.file_path = path
        self.dirty = False
        self._update_title()
        self._highlight()

    def save_file(self, force_dialog=False):
        path = self.file_path
        if force_dialog or not path:
            path = filedialog.asksaveasfilename(
                defaultextension=".cy",
                filetypes=[("Cyron files", "*.cy"), ("All files", "*.*")],
                initialdir=HERE)
            if not path:
                return False
        with open(path, "w", encoding="utf-8") as f:
            f.write(self.editor.get("1.0", "end-1c"))
        self.file_path = path
        self.dirty = False
        self._update_title()
        self._set_status("Saved " + os.path.basename(path))
        return True

    def load_example(self, name):
        if not self._confirm_discard():
            return
        self.editor.delete("1.0", "end")
        self.editor.insert("1.0", EXAMPLES[name])
        self.file_path = None
        self.dirty = False
        self._update_title()
        self._highlight()
        self._set_status("Loaded example: " + name)

    # ---------- docs ----------

    def toggle_docs(self):
        if self.docs_visible:
            self.outer.remove(self.docs_frame)
        else:
            self.outer.add(self.docs_frame, minsize=340, width=420)
        self.docs_visible = not self.docs_visible

    def show_about(self):
        messagebox.showinfo(
            "About Cyron",
            "Cyron 1.0\n\nA tiny interpreted language:\n"
            "Python's heart, C's curly braces.\n\n"
            "No semicolons. No indentation rules. No memory leaks.")

    # ---------- running ----------

    def _interpreter_path(self):
        for name in ("cyron.exe", "cyron"):
            p = os.path.join(HERE, name)
            if os.path.isfile(p):
                return p
        return None

    def _ensure_interpreter(self, rebuild=False):
        path = self._interpreter_path()
        if path and not rebuild:
            return path
        src = os.path.join(HERE, "cyron.c")
        if not os.path.isfile(src):
            messagebox.showerror("Cyron", "cyron.c not found next to ide.py, "
                                 "so the interpreter cannot be built.")
            return None
        self._set_status("Building interpreter with gcc...")
        out = os.path.join(HERE, "cyron.exe" if os.name == "nt" else "cyron")
        try:
            r = subprocess.run(["gcc", src, "-o", out, "-O2", "-lm"],
                               capture_output=True, text=True, timeout=120)
        except FileNotFoundError:
            messagebox.showerror("Cyron", "gcc was not found on PATH. Install "
                                 "MinGW (or build cyron.c yourself) and retry.")
            return None
        if r.returncode != 0:
            messagebox.showerror("Cyron", "gcc failed:\n\n" + (r.stderr or "")[-2000:])
            return None
        self._set_status("Interpreter built.")
        return out

    def run_program(self):
        if self.running:
            self._set_status("Already running...")
            return
        exe = self._ensure_interpreter()
        if not exe:
            return

        # decide which file to execute
        if self.file_path:
            if not self.save_file():
                return
            path = self.file_path
        else:
            fd, path = tempfile.mkstemp(suffix=".cy", dir=HERE)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(self.editor.get("1.0", "end-1c"))
            self._temp_to_delete = path

        stdin_text = self.stdin_box.get("1.0", "end-1c")
        if stdin_text and not stdin_text.endswith("\n"):
            stdin_text += "\n"

        self._clear_output()
        self._write_output("$ cyron %s\n" % os.path.basename(path), "meta")
        self.running = True
        self.run_btn.config(fg=COL["dim"], text="⏳ Running...")
        self._set_status("Running...")

        threading.Thread(target=self._run_worker,
                         args=(exe, path, stdin_text), daemon=True).start()

    def _run_worker(self, exe, path, stdin_text):
        try:
            r = subprocess.run([exe, path], input=stdin_text,
                               capture_output=True, text=True, timeout=15)
            self.proc_queue.put(("done", r.stdout, r.stderr, r.returncode))
        except subprocess.TimeoutExpired as ex:
            self.proc_queue.put(("timeout", ex.stdout or "", ex.stderr or "", -1))
        except Exception as ex:  # noqa: BLE001
            self.proc_queue.put(("error", "", str(ex), -1))

    def _poll_proc_queue(self):
        try:
            while True:
                kind, out, err, code = self.proc_queue.get_nowait()
                self.running = False
                self.run_btn.config(fg=COL["green"], text="▶  Run (F5)")
                if isinstance(out, bytes):
                    out = out.decode("utf-8", "replace")
                if isinstance(err, bytes):
                    err = err.decode("utf-8", "replace")
                if out:
                    self._write_output(out)
                if err:
                    self._write_output(err, "err")
                if kind == "timeout":
                    self._write_output("\n[stopped: program ran longer than 15s "
                                       "-- infinite loop?]\n", "err")
                    self._set_status("Timed out.")
                elif kind == "error":
                    self._set_status("Failed to run.")
                elif code == 0:
                    self._write_output("\n[finished ok]\n", "ok")
                    self._set_status("Finished.")
                else:
                    self._write_output("\n[exited with code %d]\n" % code, "err")
                    self._set_status("Finished with errors.")
                tmp = getattr(self, "_temp_to_delete", None)
                if tmp:
                    try:
                        os.remove(tmp)
                    except OSError:
                        pass
                    self._temp_to_delete = None
        except queue.Empty:
            pass
        self.root.after(100, self._poll_proc_queue)

    # ---------- output ----------

    def _clear_output(self):
        self.output.config(state="normal")
        self.output.delete("1.0", "end")
        self.output.config(state="disabled")

    def _write_output(self, text, tag=None):
        self.output.config(state="normal")
        self.output.insert("end", text, tag)
        self.output.see("end")
        self.output.config(state="disabled")

    def _set_status(self, text):
        self.status.config(text=text)

    def on_close(self):
        if self._confirm_discard():
            self.root.destroy()


def main():
    root = tk.Tk()
    CyronIDE(root)
    root.mainloop()


if __name__ == "__main__":
    main()
