#!/bin/sh
gobjdump --insn-width=16 -mi386:x86-64:intel -d -D -b binary out.bin
