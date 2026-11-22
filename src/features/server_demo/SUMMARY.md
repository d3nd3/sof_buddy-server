# server_demo

Records a server-side state demo: after every `ClientEndServerFrame`, for every
connected client, it saves the finished 200-byte `player_state_t` (position,
velocity, view angles, view offsets, weapon state, HUD stats) and optionally the
116-byte player `entity_state_t` (body origin/angles/model/frame/effects) into a
versioned `.sbd` container. At recording start it also snapshots the non-empty
server configstrings so a renderer can resolve model/skin/image indexes. It does
not modify gameplay, does not touch the network channel, and buffers writes so
a 10 Hz recording does not block the tick. Files are written under
`<userdir>/<dir>/<name>.sbd`; recording is manual (`svdemo_start`) or automatic
per map (`_sofbuddy_svdemo_auto 1`).
