#!/bin/bash -e

# This is random collection of very simple tests that are crafted
# to expose potential problems mostly in CSE ruleset

bin/test_add_ii
bin/test_add_ff
bin/test_sub_ii # test parameter order, mostly

bin/test_shift
bin/test_divmod

bin/test_ci2f_cf2i
bin/test_sx_zx
bin/test_load_store

bin/test_callp
bin/test_calln

bin/test_fib
bin/test_call_stub
bin/test_loop   # this tries to confuse opt_jump_be

bin/test_mem_opt

cat << END | bin/bjit
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) x = x+1; x = x+1; } return x;
END

cat << END | bin/bjit
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) y = x+1; x = x+1; } return x;
END

cat << END | bin/bjit
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) x = x+1; else x = x+1; x=x+1; } return x;
END

cat << END | bin/bjit
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) x = x+1; else x = x+1; } return x+1;
END

cat << END | bin/bjit
    x := 0; y := 0/0; while(x < 10) { if(y != 2) x = x+1; else x = x+1; } return (x+1);
END

cat << END | bin/bjit
    x := 0; y := 0/0; while(x < 10) { x = x+(y/0); } return (x+1);
END

cat << END | bin/bjit
x := 1; while(1) { x = x+1; if (x < 10) continue; break; }
END

cat << END | bin/bjit
y := 2/0; x := 1; while(1) { x = x+1; if ((y+x+y) < (y+10+y)) continue; break; }
END

cat << END | bin/bjit
y := 2/0; z := 3/0; x := 1; while(1) { x = x+1; if (((y+x)+(x+z)) < ((y+10)+(z+10))) continue; break; }
END

# fuzzfold generates tons of garbage, so throw it into /dev/null
# and then run the thing manually if it fails
echo "Fuzzing..."
bin/test_fuzzfold 2> /dev/null
echo "Fuzz passed."

bin/test_sieve

echo "Looks like it didn't crash, at least... ;-)"

