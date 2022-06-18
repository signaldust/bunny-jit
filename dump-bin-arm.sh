#!/bin/sh
gobjdump --insn-width=16 -maarch64 -d -D -b binary out.bin
