# Cg vector arithmetic compatibility

Cg component-wise arithmetic between vectors of unequal widths uses the smaller
width. GLSL ES requires matching widths, so otherwise valid legacy Cg programs
could fail after translation. The translator now inserts explicit truncation
for known scalar/vector expressions, preserving operator precedence and scalar
broadcasting. Matrices, comparisons and unsupported expressions are left alone.
Reductions such as dot retain their full inputs.

The translated shader-cache version increases from 5 to 6 so existing cached
programs are regenerated. This is a general Cg compatibility fix, not a dedicated
thermal-vision implementation.

Run the GPU pixel regression from the repository root:

```sh
python3 port/tests/cg_vector_arithmetic.py
```

Requires a desktop C++ compiler, SDL2 and GLES development libraries available
through pkg-config, and an offscreen GLES 3 context. Checks mixed-vector operand
order, precedence, division, nested saturation, scalar multiplication, swizzles
and dot reduction against expected framebuffer pixels.

Desktop and ARM builds passed during development. The shader corpus check
compiled 418/418 programs with no translation or compilation failures. That
establishes compiler compatibility, not complete visual coverage of all effects.
The change is included in the installed H700 build and the user reports it works
in gameplay; thermal vision has not received a separate acceptance test here.
