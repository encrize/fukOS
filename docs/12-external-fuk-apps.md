# External `.fuk` applications

This document is the FUK1 VM reference. If you are writing your first application, start with the step-by-step [`13-fuk-programming-tutorial.md`](13-fuk-programming-tutorial.md). A complete Tetris example is available at [`examples/fuk/tetris.fuk`](../examples/fuk/tetris.fuk).

## Overview

fukOS loads applications directly from the FAT partition on the boot drive. Applications are ordinary text files stored in `/apps` and are not linked into the kernel.

```text
/
├── KERNEL.BIN
├── limine.conf
└── apps/
    ├── hello.fuk
    └── tetris.fuk
```

Run an application without its path or extension:

```text
start hello
start tetris
```

FUK1 is an interpreted format, not native ELF machine code. This provides a bounded application environment before fukOS has processes, Ring 3, virtual memory, system calls, or an ELF loader.

## Runtime implementation

Relevant files:

- `kernel/app.c` — loader and FUK1 interpreter;
- `kernel/app.h` — `app_start()` interface;
- `kernel/fat.c` and `kernel/fat.h` — path-based FAT loading;
- `kernel/shell.c` — `start <name>` command;
- `examples/fuk/tetris.fuk` — complete game example;
- `examples/fuk/types-and-branches.fuk` — strings, floats, conversions, and direct if/else.

Launch sequence:

1. The shell validates the application name.
2. `start demo` resolves to `/apps/demo.fuk`.
3. The FAT driver opens the file without changing the shell working directory.
4. The loader validates the file size and `FUK1` header.
5. Source and VM state are allocated from the kernel heap.
6. The interpreter executes one instruction at a time.
7. `exit`, end of file, or a runtime error returns control to the shell.
8. VM state and source memory are released.

## Current limits

| Resource | Limit |
|---|---:|
| Application size | 512 KiB |
| Variables | 256 |
| Variable/label name | 47 characters |
| String value | 127 bytes |
| Integer array | 4,096 elements |
| Source line | 2,047 bytes |
| Executed instructions | 100,000,000 |
| Function call depth | 64 |
| Indexed labels | 1,024 |
| One `sleep` | 60,000 ms |

VM state is heap-allocated, so increasing variable and array limits no longer consumes the shell stack.
Labels are indexed once at startup, so loops and function calls do not rescan a large source file on every jump.

## Installing an application

1. Mount the FAT partition of the fukOS drive on another computer.
2. Create `/apps` if it does not exist.
3. Copy the `.fuk` file into that directory.
4. Safely eject the drive and boot fukOS.
5. Run `start <name>`.

For example:

```text
/apps/my_game.fuk -> start my_game
```

Application names may contain ASCII letters, digits, and `_`. Paths and extensions are intentionally rejected by `start`.

## Source format

The first line must be:

```text
FUK1
```

Use ASCII or UTF-8 without a BOM. Each instruction occupies one line. Blank lines are ignored. A comment is a line whose first non-space character is `#`.

```text
FUK1
# A useful comment
println Hello!
exit
```

Inline comments start with `#` outside quoted strings. A `#` inside a quoted string is ordinary text.

## Names and values

Names may contain `A-Z`, `a-z`, `0-9`, and `_`, are case-sensitive, and may be up to 47 characters long. Variables are dynamically typed as signed 32-bit integers, 32-bit floats, or strings up to 127 bytes. Assigning a new value changes the variable type.

```text
set score 100
float gravity 9.81
str name "Dan Smith"
```

Integer operands accept a decimal integer or an existing integer variable. Float operands accept decimal/scientific notation, a float variable, or an integer variable. String operands accept an existing string variable or a literal token. Quote strings that contain spaces.

Quoted tokens support `\n`, `\t`, `\"`, and `\\`.

## Text output

| Instruction | Meaning |
|---|---|
| `print text` | Print text without a newline |
| `println text` | Print text and a newline |
| `printv value` | Print an integer |
| `printf value` | Print a float |
| `printfn value` | Print a float and a newline |
| `prints value` | Print a string |
| `printsn value` | Print a string and a newline |
| `putc value` | Print one byte/character, `0..255` |

Escape sequences in text:

| Sequence | Result |
|---|---|
| `\n` | Newline |
| `\t` | Tab |
| `\s` | Space, including a trailing space |
| `\\` | Backslash |

## Input

| Instruction | Meaning |
|---|---|
| `input dst prompt` | Blocking signed-integer input |
| `inputc dst prompt` | Blocking input; store the first character code |
| `inputf dst prompt` | Blocking float input |
| `inputs dst prompt` | Blocking string input, up to 127 bytes |
| `key_poll dst` | Non-blocking keyboard poll; store `0` if no event exists |

Special key codes:

| Key | Code |
|---|---:|
| Up | 128 |
| Down | 129 |
| Left | 130 |
| Right | 131 |
| Page Up | 132 |
| Page Down | 133 |
| Home | 134 |
| End | 135 |
| Delete | 136 |
| F12 | 139 |
| Escape | 27 |
| Enter | 10 |

These are decoded VM key values, not raw PS/2 scan codes.

## Variables and arithmetic

| Instruction | Result |
|---|---|
| `set dst value` | Assignment |
| `mov dst value` | Assignment alias |
| `inc variable` | Add one in place |
| `dec variable` | Subtract one in place |
| `neg dst value` | Arithmetic negation |
| `abs dst value` | Absolute value |
| `add dst a b` | `a + b` |
| `sub dst a b` | `a - b` |
| `mul dst a b` | `a * b` |
| `div dst a b` | Signed integer division |
| `mod dst a b` | Signed remainder |
| `min dst a b` | Smaller value |
| `max dst a b` | Larger value |

Division or remainder by zero is a runtime error.

### Float arithmetic

| Instruction | Result |
|---|---|
| `float dst value` | Create or assign a float |
| `fset dst value` | Float assignment alias |
| `fneg dst value` | Negation |
| `fabs dst value` | Absolute value |
| `fadd dst a b` | `a + b` |
| `fsub dst a b` | `a - b` |
| `fmul dst a b` | `a * b` |
| `fdiv dst a b` | `a / b` |
| `fmin dst a b` | Smaller float |
| `fmax dst a b` | Larger float |

Float literals support forms such as `3.14`, `-0.5`, and `1.25e3`. `fdiv` rejects zero divisors. Use `if_fnear` or `cmp_fnear` instead of exact equality when a calculation can introduce rounding.

### Strings

| Instruction | Result |
|---|---|
| `str dst value` | Create or assign a string |
| `sset dst value` | String assignment alias |
| `strlen dst value` | String length in bytes |
| `strcmp dst a b` | Store `-1`, `0`, or `1` |
| `strcat dst a b` | Concatenate two strings |

`strcat` is safe when `dst` is also one of the inputs. A result longer than 127 bytes is a runtime error instead of being truncated.

### Conversions and convenience operations

| Instruction | Result |
|---|---|
| `itof dst integer` | Convert integer to float |
| `ftoi dst float` | Truncate float toward zero |
| `stoi dst string` | Parse a signed integer |
| `stof dst string` | Parse a float |
| `clamp dst value low high` | Restrict an integer to an inclusive range |

Invalid parsing and out-of-range conversion stop with a descriptive runtime error.

## Bit operations

| Instruction | Result |
|---|---|
| `bit_not dst value` | Bitwise NOT |
| `bit_and dst a b` | Bitwise AND |
| `bit_or dst a b` | Bitwise OR |
| `bit_xor dst a b` | Bitwise XOR |
| `shl dst value bits` | Logical left shift |
| `shr dst value bits` | Logical right shift |

Shift counts must be between `0` and `31`.

## Comparisons

Integer conditional jumps:

```text
if_eq a b true_label [false_label]
if_ne a b true_label [false_label]
if_lt a b true_label [false_label]
if_le a b true_label [false_label]
if_gt a b true_label [false_label]
if_ge a b true_label [false_label]
```

The optional fourth operand adds a direct `else` branch. Any branch target may be the reserved word `exit`. This makes the common case compact:

```text
inputs name Your name:
if_seq name dan success exit
label success
printsn Welcome, dan!
```

String branches use `if_seq`, `if_sne`, `if_slt`, `if_sle`, `if_sgt`, and `if_sge`. Float branches use `if_feq`, `if_fne`, `if_flt`, `if_fle`, `if_fgt`, and `if_fge`. Every branch accepts the same optional false target.

For tolerant float equality:

```text
if_fnear measured expected 0.001 close not_close
```

Boolean shortcuts branch on an integer value:

```text
if_true condition yes no
if_false condition empty present
```

Comparison instructions store `1` for true and `0` for false:

```text
cmp_eq dst a b
cmp_ne dst a b
cmp_lt dst a b
cmp_le dst a b
cmp_gt dst a b
cmp_ge dst a b
```

Float comparisons use `cmp_feq`, `cmp_fne`, `cmp_flt`, `cmp_fle`, `cmp_fgt`, and `cmp_fge`. `cmp_fnear dst a b epsilon` performs tolerant equality. String comparisons use `cmp_seq`, `cmp_sne`, `cmp_slt`, `cmp_sle`, `cmp_sgt`, and `cmp_sge`. All stored comparisons produce integer `1` or `0`.

## Labels, jumps, and functions

```text
label name
goto name
call name
return
```

`call` pushes the address of the following instruction and jumps to a label. `return` resumes after the latest `call`. The call stack holds 64 return addresses. Variables are global; FUK1 does not yet have local variables or function parameters.

Example:

```text
FUK1
set value 7
call print_double
println
exit

label print_double
mul result value 2
printv result
return
```

## Integer array

Every application receives 4,096 signed 32-bit elements initialized to zero.

| Instruction | Meaning |
|---|---|
| `array_set index value` | Store one element |
| `array_get dst index` | Read one element |
| `array_fill value` | Fill all elements |
| `array_size dst` | Store `4096` |

Valid indices are `0..4095`. A two-dimensional grid of width `w` uses `index = y * w + x`.

## Random numbers

```text
rand dst minimum maximum
```

Both bounds are inclusive. The xorshift32 generator is suitable for games, not cryptography.

## Terminal control

| Instruction | Meaning |
|---|---|
| `clear` | Clear the terminal and reset the cursor |
| `cursor x y` | Set zero-based text column and row |
| `color id` | Select foreground color `0..15` |
| `screen_cols dst` | Read terminal width in characters |
| `screen_rows dst` | Read terminal height in characters |

Color IDs follow the standard 16-color console palette. Applications should query screen dimensions before using dynamic coordinates.

## Time

| Instruction | Meaning |
|---|---|
| `sleep milliseconds` | Bounded delay, `0..60000` |
| `time_ms dst` | Read the low signed 32 bits of milliseconds since TSC epoch |

During `sleep`, background HDA audio and xHCI events continue to be serviced.

## Program termination

```text
exit
```

Reaching end of file also succeeds, but explicit `exit` is recommended.

## Common errors

| Error | Cause |
|---|---|
| `app not found` | File is not present in `/apps` |
| `missing FUK1 header` | Invalid first line or BOM |
| `unknown instruction` | Typo or older VM version |
| `unknown value` | Variable read before creation |
| `invalid variable` | Invalid name or variable limit reached |
| `label not found` | Missing or misspelled label |
| `call stack overflow` | More than 64 nested calls |
| `call stack underflow` | `return` without `call` |
| `array index outside 0..4095` | Invalid array access |
| `cursor outside screen` | Invalid terminal coordinates |
| `division by zero` | Invalid integer division or remainder |
| `float division by zero` | Invalid float division |
| `string is longer than 127 bytes` | String assignment or concatenation exceeded the limit |
| `integer expected` / `float expected` | Input or conversion has the wrong format |
| `instruction limit exceeded` | More than 100,000,000 instructions |

Runtime errors include the current execution line counter.

## Security model

FUK1 code cannot access arbitrary kernel memory, I/O ports, or hardware pointers. It can only use explicitly implemented VM instructions. Applications still run synchronously in the kernel context, so limits and validation remain necessary until fukOS gains isolated user processes.
