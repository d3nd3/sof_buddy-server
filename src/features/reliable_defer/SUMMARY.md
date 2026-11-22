# reliable_defer

Rewrites the reliable server-to-client channel (`Netchan_Transmit`, `SZ_Write`,
`MSG_Write*`) so bursty reliable traffic is held and drip-fed over later ticks
instead of overflowing the mailbox or suppressing snapshots. Adds per-parcel
size limits, a 256 KiB per-player backlog cap, and an optional snapshot-first
policy.
