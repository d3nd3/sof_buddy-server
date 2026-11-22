# clsv_pipe

Stufftext-based `_sp_cl_sv_*` pipeline that delivers `.func`/`.sp` chunks to
clients through the client cvar ring, with INFO/DATA/CMD/DONE framing and
`pipeack` acknowledgements. Adds `clsv_load/send/status/cancel`, auto-send on
spawn, and a one-time `pipe.func` download/Ghoul-index bootstrap with the
`dl_unlock` allowlist.
