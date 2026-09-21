#pragma once

/** Bootstrap-only (GameDllLoaded). */
void Pg_InitCvars();

/** SoFPlus: max printable chars in sp_sv_print_broadcast text (#~cvar path). */
int Pg_BroadcastMax();
/** SoFPlus: max printable chars in sp_sv_print_client text (slot 0-9, #~cvar). */
int Pg_ClientMax();

/** Engine: whole macro-expanded command line (not a scripter cvar). */
int Pg_MacroMax();
/** Engine: one cbuf line (not a scripter cvar). */
int Pg_CbufMax();
