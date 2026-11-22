# cpu_optimizations

Experimental engine-performance bundle: tick pacing, `cmd_text` parking,
in-place `Cbuf_InsertText`, command-handler timing, high/low clamp monitoring,
hashed cvar/command/alias lookup, zone-allocation pooling, and larger UDP
receive buffers. Overrides tick/scheduler/allocator/socket paths and is known
to risk movement and timing glitches.
