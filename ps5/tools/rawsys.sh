#!/usr/bin/env bash
# Lists the raw system call stubs (syscall instructions) in a linked title and how many direct callers each has. A title
# may make no system call itself (the console ends it: "directly issued a syscall"); every stub with callers needs a
# wrap onto libkernel (title/libc_ps5.cpp).   ps5/tools/rawsys.sh [linked-elf]
B=${1:-/home/kai/pkp-ps5-build/game/pokopia_ps5}
O=${OBJDUMP:-llvm-objdump}
$O -d --no-show-raw-insn $B 2>/dev/null | awk '
  /^[0-9a-f]+ <.*>:$/ { fn=$2; gsub(/[<>:]/, "", fn); next }
  /\tsyscall/ { raw[fn]=1; next }
  /\t(call|jmp)[a-z]*\t/ { if (match($0, /<[^>+]+>/)) { t=substr($0, RSTART+1, RLENGTH-2); if (t != fn) calls[t]++ } }
  END { for (f in raw) printf "%6d %s\n", calls[f]+0, f }' | sort -rn > /tmp/rawsys.txt
echo "raw-syscall functions: $(wc -l < /tmp/rawsys.txt), with callers: $(awk '$1>0' /tmp/rawsys.txt | wc -l)"
awk '$1>0' /tmp/rawsys.txt
