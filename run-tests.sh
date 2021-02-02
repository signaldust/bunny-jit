#!/bin/bash -e

# This is random collection of very simple tests that are crafted
# to expose potential problems mostly in CSE ruleset

cat << END | bin/bjit
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) x = x+1; x = x+1; } return x;
END

cat << END | bin/bjit
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) x = x+1; x = x+1; } return y;
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

bin/test_add_ii
bin/test_add_ff
bin/test_sub_ii # test parameter order, mostly

bin/test_ci2f_cf2i

echo "Looks like it didn't crash, at least... ;-)"

