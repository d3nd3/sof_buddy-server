Streams native lifecycle, map outcome, player match stats and optional combat,
flag and sample events from the game DLL to a bounded JSONL spool. The game
thread only serializes and queues events; a background writer owns the file, so
no database or network I/O runs in the game loop. The spool is consumed by
`sof-db-push` and projected by `sof-db-backend`.
