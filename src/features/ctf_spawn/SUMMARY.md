# ctf_spawn

CTF-only override of `SelectTeamDeathmatchSpawnPoint`: chooses the team spawn
farthest from the nearest living enemy while avoiding spots with a teammate
within roughly 56 units, falling back to the farthest enemy-distance spawn
when every point is blocked. Non-CTF modes call the stock function unchanged.
