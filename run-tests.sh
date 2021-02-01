#!/bin/bash -e

BJIT=bin/bjit

# This is random collection of very simple tests that are crafted
# to expose potential problems mostly in CSE ruleset

echo "Testing with output to 'test.out' (last test only)"

cat << END | $BJIT > test.out
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) x = x+1; x = x+1; } return x;
END
echo "Test pass"

cat << END | $BJIT > test.out
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) x = x+1; x = x+1; } return y;
END
echo "Test pass"

cat << END | $BJIT > test.out
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) y = x+1; x = x+1; } return x;
END
echo "Test pass"

cat << END | $BJIT > test.out
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) x = x+1; else x = x+1; x=x+1; } return x;
END
echo "Test pass"

cat << END | $BJIT > test.out
    x := 0/0; y := x/1u;
    while(x < 10) { if(y != 2) x = x+1; else x = x+1; } return x+1;
END
echo "Test pass"

echo "Looks like it didn't crash, at least... ;-)"

