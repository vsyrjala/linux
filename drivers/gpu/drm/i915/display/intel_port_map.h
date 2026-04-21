/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _INTEL_PORT_MAP_H
#define _INTEL_PORT_MAP_H

#include <linux/types.h>

enum hpd_pin;
enum phy;
enum port;
enum tc_port;
struct intel_display;
struct intel_encoder;

enum port intel_port_map_port(struct intel_display *display, enum port port);
enum phy intel_port_map_phy(struct intel_display *display, enum port port);
enum tc_port intel_port_map_tc_port(struct intel_display *display, enum port port);
enum hpd_pin intel_port_map_hpd_pin(struct intel_encoder *encoder);
u8 intel_port_map_ddc_pin(struct intel_encoder *encoder);

#endif /* _INTEL_PORT_MAP_H */
