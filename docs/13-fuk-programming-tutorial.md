# FUK1 application programming tutorial

This tutorial builds applications step by step. Use [`12-external-fuk-apps.md`](12-external-fuk-apps.md) as the complete instruction reference. The full Tetris source is in [`examples/fuk/tetris.fuk`](../examples/fuk/tetris.fuk). A smaller typed-values example is in [`examples/fuk/types-and-branches.fuk`](../examples/fuk/types-and-branches.fuk).

## 1. Mental model

FUK1 is a small interpreted language. It is closer to a simple assembly language than to C or Python:

- one instruction per line;
- dynamically typed integer, float, and string variables;
- execution normally moves from top to bottom;
- labels and jumps implement branches and loops;
- `call` and `return` provide basic reusable routines;
- a 4,096-element integer array stores larger data sets;
- terminal, keyboard, random, and timing operations are exposed through VM instructions.

Applications are stored on the FAT drive and loaded at runtime. Adding a new `.fuk` file does not require rebuilding the kernel.

## 2. Create and run the first application

Create `hello.fuk` as ASCII or UTF-8 without a BOM:

```text
FUK1
println Hello from fukOS!
exit
```

Copy it to:

```text
/apps/hello.fuk
```

Boot fukOS and run:

```text
start hello
```

The first line must be exactly `FUK1`. Blank lines are allowed after it. A full-line comment starts with `#`.

```text
# This comment explains why the next instruction exists.
println Ready.
```

Inline comments are supported outside quoted strings:

```text
set lives 3 # player starts with three lives
str tag "#ready" # the first # is part of the string
```

## 3. Variables

A destination operand creates a variable if it does not exist:

```text
set score 0
set lives 3
mov backup score
```

FUK1 supports 256 global variables. Names are case-sensitive, may contain letters, digits, and `_`, and may be up to 47 characters long. A variable can hold an integer, a float, or a string; assigning another type replaces its previous value.

A value operand can be a decimal integer or an existing variable:

```text
set width 10
set height 20
mul area width height
```

Use `inc` and `dec` for counters:

```text
inc score
dec lives
```

Create floats and strings with type-specific assignments:

```text
float speed 2.5
str name dan
str full_name "Dan Smith"
```

Quoted strings may contain spaces. String values are limited to 127 bytes.

## 4. Output

Text output:

```text
print Loading...
println Done
println
```

Number output:

```text
set score 125
print Score:
printv score
println
```

Single-character output:

```text
putc 65
println
```

`putc 65` prints `A`.

Useful text escapes:

```text
print first\nsecond
print left\tright
print \s
print \\
```

`\s` is useful when a trailing space would otherwise be trimmed from a source line.

## 5. Blocking input

Read an integer:

```text
input age Enter your age:
print Age:
printv age
println
```

Read the first character code:

```text
inputc answer Continue (y/n):
if_eq answer 121 accepted
println Cancelled
exit
label accepted
println Accepted
exit
```

`input` and `inputc` block until Enter is pressed. Games should use `key_poll` instead.

## 6. Arithmetic

Binary arithmetic uses `operation destination left right`:

```text
add sum a b
sub difference a b
mul area width height
div half value 2
mod digit value 10
min lower a b
max higher a b
```

Unary arithmetic:

```text
neg opposite value
abs magnitude value
```

Division is signed integer division. Division or remainder by zero stops the application with a runtime error.

### Calculator example

```text
FUK1
input a First number:
inputc op Operation (+ - * / %):
input b Second number:
if_eq op 43 do_add
if_eq op 45 do_sub
if_eq op 42 do_mul
if_eq op 47 do_div
if_eq op 37 do_mod
println Unknown operation
exit

label do_add
add result a b
goto show
label do_sub
sub result a b
goto show
label do_mul
mul result a b
goto show
label do_div
div result a b
goto show
label do_mod
mod result a b
label show
print Result:
printv result
println
exit
```

## 7. Strings, floats, and direct if/else

Read and compare a name:

```text
FUK1
inputs name Your name:
if_seq name dan success exit

label success
printsn Welcome, dan!
exit
```

The last operand is the false target. It is optional for backward compatibility, and `exit` is a reserved branch target. The same true/false form works for integer, float, string, and Boolean branches.

String tools:

```text
str first "Dan"
str space " "
str last "Smith"
strcat full first space
strcat full full last
strlen size full
strcmp order first last
printsn full
```

Float arithmetic and comparison:

```text
float price 19.95
float tax_rate 0.05
fmul tax price tax_rate
fadd total price tax
printfn total
if_fnear total 20.9475 0.0001 correct wrong
```

Use `if_fnear` for calculated floats. Exact `if_feq` is useful for values that were assigned directly, but normal floating-point rounding rules still apply.

Conversions and a convenience helper:

```text
stoi count "42"
stof ratio "0.75"
itof count_float count
ftoi whole ratio
clamp safe_x x 0 79
```

## 8. Branches

Conditional jumps compare two values and jump when the condition is true:

```text
if_eq a b equal
if_ne a b different
if_lt a b less
if_le a b less_or_equal
if_gt a b greater
if_ge a b greater_or_equal
```

Example:

```text
FUK1
input number Enter a number:
if_gt number 0 positive
if_lt number 0 negative
println Zero
exit
label positive
println Positive
exit
label negative
println Negative
exit
```

Comparison instructions store a Boolean result instead of jumping:

```text
cmp_ge can_enter age 18
printv can_enter
println
```

The result is `1` for true and `0` for false.

## 9. Loops

FUK1 builds loops from labels and jumps:

```text
FUK1
set i 1
label loop
printv i
println
inc i
if_le i 10 loop
exit
```

Always give a real-time loop a `sleep`; otherwise it can consume all available CPU time:

```text
label frame
sleep 50
# Update state and draw here.
goto frame
```

## 10. Reusable routines

`call` jumps to a label and remembers where execution should resume. `return` resumes after the latest `call`.

```text
FUK1
set value 21
call print_double
println
set value 50
call print_double
println
exit

label print_double
mul result value 2
printv result
return
```

Variables are global. Pass values by assigning agreed variable names before `call`, and read results from agreed variables after it. The call stack supports 64 nested calls.

Do not enter a function by normal fall-through. Place `exit` or `goto` before function definitions when necessary.

## 11. Bit operations

FUK1 supports integer bit manipulation:

```text
bit_and masked value 255
bit_or flags flags 4
bit_xor toggled flags 1
bit_not inverted value
shl doubled value 1
shr halved value 1
```

Shift counts must be between 0 and 31. `shr` is a logical right shift.

## 12. Arrays

Each launch receives 4,096 signed integer cells initialized to zero.

```text
array_set 0 42
array_get value 0
printv value
```

Use variables as indices:

```text
set index 100
array_set index 7
array_get value index
```

Clear or initialize all cells:

```text
array_fill 0
array_size capacity
```

### Store a two-dimensional grid

For a grid with width `10`, convert `(x, y)` to a linear index:

```text
mul index y 10
add index index x
array_set index 1
```

Read it later:

```text
mul index y 10
add index index x
array_get cell index
```

A 10×20 Tetris board uses cells `0..199`; the remaining array remains available for other state.

## 13. Terminal drawing

Clear the screen:

```text
clear
```

Place the next output at a zero-based character coordinate:

```text
cursor 10 5
print @
```

Select a color:

```text
color 11
println Yellow
color 15
```

Query terminal dimensions instead of assuming a fixed resolution:

```text
screen_cols width
screen_rows height
sub center_x width 1
div center_x center_x 2
sub center_y height 1
div center_y center_y 2
cursor center_x center_y
print @
```

### Update one cell without clearing

```text
cursor x y
print \s
inc x
cursor x y
print @
```

This avoids flicker and unnecessary redraws.

## 14. Non-blocking keyboard input

`key_poll` returns immediately:

```text
key_poll key
if_eq key 0 no_event
if_eq key 113 quit
if_eq key 130 move_left
if_eq key 131 move_right
```

Important codes:

| Key | Code |
|---|---:|
| Up | 128 |
| Down | 129 |
| Left | 130 |
| Right | 131 |
| Escape | 27 |
| Enter | 10 |
| `q` | 113 |

## 15. Random numbers and time

Inclusive random range:

```text
rand die 1 6
rand piece 0 6
```

Delay execution:

```text
sleep 50
```

Read a millisecond counter:

```text
time_ms now
```

The counter wraps because variables are signed 32-bit integers. Use it for short elapsed-time measurements, not calendar time.

## 16. A small real-time application

```text
FUK1
clear
screen_cols width
screen_rows height
div x width 2
div y height 2
color 11
cursor x y
print @

label frame
sleep 40
key_poll key
if_eq key 113 quit
if_eq key 27 quit
if_eq key 130 left
if_eq key 131 right
goto frame

label erase
cursor x y
print \s
return

label left
call erase
if_le x 0 frame
dec x
cursor x y
print @
goto frame

label right
call erase
sub limit width 1
if_ge x limit frame
inc x
cursor x y
print @
goto frame

label quit
clear
exit
```

This example introduces screen queries, a frame delay, non-blocking input, a reusable erase routine, bounds checks, and partial redraw.

## 17. How the Tetris example works

The complete source is [`examples/fuk/tetris.fuk`](../examples/fuk/tetris.fuk). Copy it to `/apps/tetris.fuk` and run `start tetris`.

### Board storage

The 10×20 board occupies 200 array cells. Cell index is `y * 10 + x`.

```text
mul idx y 10
add idx idx x
array_get cell idx
```

### Active tetromino

Four `(x, y)` pairs hold the active blocks:

```text
set ax 0
set ay 0
set bx 0
set by 0
set cx 0
set cy 0
set dx 0
set dy 0
```

Candidate coordinates are calculated separately. The current coordinates are updated only after every candidate block passes bounds and collision checks.

### Random spawn

```text
rand piece 0 6
if_eq piece 0 spawn_i
if_eq piece 1 spawn_o
if_eq piece 2 spawn_t
```

### Collision test

```text
mul idx nay 10
add idx idx nax
array_get cell idx
if_ne cell 0 lock
```

### Locking a piece

```text
mul idx ay 10
add idx idx ax
array_set idx 1
```

The same operation is applied to all four blocks.

### Detecting a complete row

```text
set col 0
set count 0
label scan_col
if_ge col 10 scan_done
mul idx row 10
add idx idx col
array_get cell idx
if_eq cell 0 next_col
inc count
label next_col
inc col
goto scan_col
label scan_done
if_eq count 10 remove_row
```

The full example then shifts every row above the completed row down by one.

## 18. Debugging workflow

Start with a minimal program and add one feature at a time. Temporary output is the simplest debugger:

```text
print x=
printv x
print  y=
printv y
println
```

Common failures:

- typo in an instruction or label;
- variable read before creation;
- index outside `0..4095`;
- cursor outside the current terminal dimensions;
- division by zero;
- `return` without a matching `call`;
- more than 64 nested calls;
- source saved with a BOM;
- application written for a newer VM than the kernel on the drive.

## 19. Development checklist

1. Write the application state as a list of variables and array regions.
2. Keep variable names meaningful; 256 are available, but clear state design still matters.
3. Query screen dimensions for portable terminal layouts.
4. Validate array indices and coordinates before use.
5. Add `sleep` to every continuous real-time loop.
6. Calculate candidate state before committing movement.
7. Use `call` for repeated routines that have one clear entry and return point.
8. Add temporary `printv` diagnostics around failing branches.
9. Test error paths, not only the successful path.
10. Copy the finished file to `/apps` and test on the target hardware.

## 20. Quick reference

```text
FUK1

print text
println text
printv value
printf value
printfn value
prints value
printsn value
putc value
input dst prompt
inputc dst prompt
inputf dst prompt
inputs dst prompt
key_poll dst

set dst value
mov dst value
float dst value
fset dst value
str dst value
sset dst value
inc variable
dec variable
neg dst value
abs dst value
add dst a b
sub dst a b
mul dst a b
div dst a b
mod dst a b
min dst a b
max dst a b
clamp dst value low high

fneg dst value
fabs dst value
fadd dst a b
fsub dst a b
fmul dst a b
fdiv dst a b
fmin dst a b
fmax dst a b

strlen dst value
strcmp dst a b
strcat dst a b
stoi dst string
stof dst string
itof dst integer
ftoi dst float

bit_not dst value
bit_and dst a b
bit_or dst a b
bit_xor dst a b
shl dst value bits
shr dst value bits

array_set index value
array_get dst index
array_fill value
array_size dst

clear
cursor x y
color id
screen_cols dst
screen_rows dst
sleep milliseconds
time_ms dst
rand dst min max

label name
goto name
call name
return
if_eq a b label
if_ne a b label
if_lt a b label
if_le a b label
if_gt a b label
if_ge a b true_label [false_label]
if_feq a b true_label [false_label]
if_fne a b true_label [false_label]
if_flt a b true_label [false_label]
if_fle a b true_label [false_label]
if_fgt a b true_label [false_label]
if_fge a b true_label [false_label]
if_fnear a b epsilon true_label [false_label]
if_seq a b true_label [false_label]
if_sne a b true_label [false_label]
if_slt a b true_label [false_label]
if_sle a b true_label [false_label]
if_sgt a b true_label [false_label]
if_sge a b true_label [false_label]
if_true value true_label [false_label]
if_false value true_label [false_label]
cmp_eq dst a b
cmp_ne dst a b
cmp_lt dst a b
cmp_le dst a b
cmp_gt dst a b
cmp_ge dst a b
cmp_feq dst a b
cmp_fne dst a b
cmp_flt dst a b
cmp_fle dst a b
cmp_fgt dst a b
cmp_fge dst a b
cmp_fnear dst a b epsilon
cmp_seq dst a b
cmp_sne dst a b
cmp_slt dst a b
cmp_sle dst a b
cmp_sgt dst a b
cmp_sge dst a b

exit
```
