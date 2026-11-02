#!/bin/bash
# run.sh <name> <seconds> <envfile>: the worktree's runner on TMB US with a fresh user folder; the log and outputs in
# det/runs/<name>*. envfile: one VAR=value a line; @file as a value reads it from det/<file>; {OUT} is det/runs/<name>
# (Windows form).
S=/c/Users/thoma/AppData/Local/Temp/claude/D--TMB-Recomp/f9ac768a-d142-41f2-8a93-41a627c0d8f1/scratchpad/det
WT=/d/TMB_Recomp/.claude/worktrees/agent-a2e280bdeeacc19f2
name=$1; secs=$2; envfile=$3
OUT=$S/runs/$name
rm -rf "$OUT" "$OUT.user" "$OUT.ref" "$OUT.ref.calls"; mkdir -p "$OUT" "$OUT.user"
WOUT="$(cygpath -m "$OUT")"
cd $WT
export PATH=/c/msys64/ucrt64/bin:$PATH
export PS2X_USER_DIR="$(cygpath -w "$OUT.user")" PS2X_PAD_NO_HOST=1
while IFS= read -r line || [ -n "$line" ]; do
    [ -z "$line" ] && continue
    var=${line%%=*}; val=${line#*=}
    if [ "${val:0:1}" = "@" ]; then val="$(cat "$S/${val:1}")"; fi
    val=${val//\{OUT\}/$WOUT}
    export "$var=$val"
done < "$S/$envfile"
timeout $secs $WT/build/game_us/PS2Recomp/ps2xRuntime/ps2EntryRunner.exe D:/TMB_Recomp/game_us/SCUS_971.01 > "$OUT.log" 2>&1
echo "exit $?"
