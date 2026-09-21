#pragma once

// Cross-refs for the stufftext reconnect chain (see stufftext_reconnect.cpp
// for the engine binding, stufftext_reconnect_logic.h for the pure machine).

// Detach-time teardown, called from StuffText_Shutdown (cvar.cpp): restores
// the attractloop flag when a chain was mid-flight and parks the machine.
// Sends nothing - the engine may be half torn down on this path.
void StuffReconnect_OnDetach();

// Delivery accounting owned by stufftext.cpp (shared _sofbuddy_stufftext_sent
// / _errors outputs). The chain reports every stuff it sends through here so
// the counters stay exact instead of being fought over by two publishers.
void StuffText_NoteDelivery(bool ok);
