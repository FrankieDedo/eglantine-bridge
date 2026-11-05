// Reads buttons, sticks, motion and touch; plays rumble from the host.
#pragma once
#include <switch.h>
#include "ebridge_protocol.h"

void input_init(void);
void input_exit(void);

/* Fills *out from the current pad state (call padUpdate first). */
void input_read(PadState* pad, eb_input_t* out);

void input_rumble(PadState* pad, const eb_rumble_t* r);
