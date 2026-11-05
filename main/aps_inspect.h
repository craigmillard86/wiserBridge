/*
 * aps_inspect.h - Log every APS frame addressed to this node (ZDO and ZCL requests from the hub).
 *
 * Purpose: answer "what did the hub ask this device before it sent Leave?". Uses the ZBOSS
 * APS data-indication hook, observes only, never consumes a frame. Frames are already
 * decrypted by the stack at this point; only headers and command identifiers are logged,
 * never keys.
 */
#pragma once

#include <stdbool.h>

/* Install the hook. Must be called from the Zigbee task (e.g. on SKIP_STARTUP). */
void wb_aps_inspect_install(void);

void wb_aps_inspect_enable(bool on);
bool wb_aps_inspect_enabled(void);
