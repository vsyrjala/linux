/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _INTEL_PORT_MAP_H
#define _INTEL_PORT_MAP_H

#include <linux/types.h>

enum phy;
enum port;
struct intel_display;

enum phy intel_port_map_phy(struct intel_display *display, enum port port);

#endif /* _INTEL_PORT_MAP_H */
