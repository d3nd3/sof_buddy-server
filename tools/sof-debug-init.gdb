# SoF-spsv.exe debugging under Wine 10 wow64 (experimental)
# -------------------------------------------------------
# Symptom: any breakpoint (soft or hard, any address) segfaults on the
# next 'continue', looking like breakpoints are broken.
#
# Root cause (verified 2026-09-14): after the server finishes init, the
# Wine debuggee has TWO threads:
#   Thread 1 (main): SoF server loop, e.g. Sleep -> Qcommon_frame -> SV_Frame
#   Thread 2 (helper): stuck at unmapped 0xFFD3961C ("Cannot access memory"),
#     backtrace: call_thread_func(entry=0xFFD3961C) <- WriteTapemark.
#     It exists with AND without sof_buddy-server (tested stock gamex86.dll),
#     so it is a Wine wow64 helper, not the shim.
# Default GDB 'continue' resumes ALL threads, so the helper faults with
# SIGSEGV immediately, preempting the real breakpoint hit.
#
# Fix: only resume the main server thread.
#   set scheduler-locking on  -> 'continue' resumes current thread only
#   hook-stop -> after ANY stop (Ctrl-C lands in Thread 2), pin back to
#     Thread 1 so the next 'continue' resumes the server, not the helper.
# Breakpoint hits themselves land in Thread 1 already, so auto-continue
# scripts (timer.gdb etc.) are safe once this file is sourced first.
#
# Usage (winedbg --gdb):
#   Wine-gdb> source tools/sof-debug-init.gdb
#   Wine-gdb> break *0x20018530
#   Wine-gdb> continue
#   # after Ctrl-C, you are already back on Thread 1, just 'continue'
#
# NOTE: do NOT use 'set architecture i386:x64-32' with this Wine/GDB combo.
# The proxy reports i386; forcing x64-32 gives:
#   "Selected architecture i386:x64-32 is not compatible with reported target"
# and breaks unwinding. Default (auto=i386) is correct.

set pagination off
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
set scheduler-locking on

define hook-stop
  thread 1
end
