# sv_tracktime

Measurement-only monitor of each client's claimed usercmd `msec` versus the
wall-clock rate the server actually receives `clc_move` frames. Reports
per-slot drift, rolling and total verdicts, and output cvars; it never kicks,
clamps, rewrites, or broadcasts.
