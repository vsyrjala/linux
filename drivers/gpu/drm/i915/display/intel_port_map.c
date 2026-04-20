// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "intel_display.h"
#include "intel_display_core.h"
#include "intel_display_device.h"
#include "intel_display_utils.h"
#include "intel_port_map.h"

struct intel_port {
	enum phy phy;
};

struct intel_port_map {
	const struct intel_port *ports;
	u8 num_ports;
};

static const struct intel_port xelpd_ports[] = {
	[PORT_A]       = { .phy = PHY_A,   },
	[PORT_B]       = { .phy = PHY_B,   },
	[PORT_C]       = { .phy = PHY_C,   },
	[PORT_D_XELPD] = { .phy = PHY_D,   },
	[PORT_E_XELPD] = { .phy = PHY_E,   },
	[PORT_TC1]     = { .phy = PHY_TC1, },
	[PORT_TC2]     = { .phy = PHY_TC2, },
	[PORT_TC3]     = { .phy = PHY_TC3, },
	[PORT_TC4]     = { .phy = PHY_TC4, },
};
static const struct intel_port_map xelpd_port_map = {
	.ports = xelpd_ports,
	.num_ports = ARRAY_SIZE(xelpd_ports),
};

static const struct intel_port dg2_ports[] = {
	[PORT_A]       = { .phy = PHY_A, },
	[PORT_B]       = { .phy = PHY_B, },
	[PORT_C]       = { .phy = PHY_C, },
	[PORT_D_XELPD] = { .phy = PHY_D, },
	[PORT_TC1]     = { .phy = PHY_E, },
};
static const struct intel_port_map dg2_port_map = {
	.ports = dg2_ports,
	.num_ports = ARRAY_SIZE(dg2_ports),
};

static const struct intel_port dg1_ports[] = {
	[PORT_A]   = { .phy = PHY_A, },
	[PORT_B]   = { .phy = PHY_B, },
	[PORT_TC1] = { .phy = PHY_C, },
	[PORT_TC2] = { .phy = PHY_D, },
};
static const struct intel_port_map dg1_port_map = {
	.ports = dg1_ports,
	.num_ports = ARRAY_SIZE(dg1_ports),
};

static const struct intel_port adls_ports[] = {
	[PORT_A]   = { .phy = PHY_A, },
	[PORT_TC1] = { .phy = PHY_B, },
	[PORT_TC2] = { .phy = PHY_C, },
	[PORT_TC3] = { .phy = PHY_D, },
	[PORT_TC4] = { .phy = PHY_E, },
};
static const struct intel_port_map adls_port_map = {
	.ports = adls_ports,
	.num_ports = ARRAY_SIZE(adls_ports),
};

static const struct intel_port tgl_ports[] = {
	[PORT_A]   = { .phy = PHY_A,   },
	[PORT_B]   = { .phy = PHY_B,   },
	[PORT_C]   = { .phy = PHY_C,   },
	[PORT_TC1] = { .phy = PHY_TC1, },
	[PORT_TC2] = { .phy = PHY_TC2, },
	[PORT_TC3] = { .phy = PHY_TC3, },
	[PORT_TC4] = { .phy = PHY_TC4, },
	[PORT_TC5] = { .phy = PHY_TC5, },
	[PORT_TC6] = { .phy = PHY_TC6, },
};
static const struct intel_port_map tgl_port_map = {
	.ports = tgl_ports,
	.num_ports = ARRAY_SIZE(tgl_ports),
};

static const struct intel_port ehl_ports[] = {
	[PORT_A] = { .phy = PHY_A, },
	[PORT_B] = { .phy = PHY_B, },
	[PORT_C] = { .phy = PHY_C, },
	[PORT_D] = { .phy = PHY_A, },
};
static const struct intel_port_map ehl_port_map = {
	.ports = ehl_ports,
	.num_ports = ARRAY_SIZE(ehl_ports),
};

static const struct intel_port icl_ports[] = {
	[PORT_A] = { .phy = PHY_A,   },
	[PORT_B] = { .phy = PHY_B,   },
	[PORT_C] = { .phy = PHY_TC1, },
	[PORT_D] = { .phy = PHY_TC2, },
	[PORT_E] = { .phy = PHY_TC3, },
	[PORT_F] = { .phy = PHY_TC4, },
};
static const struct intel_port_map icl_port_map = {
	.ports = icl_ports,
	.num_ports = ARRAY_SIZE(icl_ports),
};

static const struct intel_port bxt_ports[] = {
	[PORT_A] = { .phy = PHY_A, },
	[PORT_B] = { .phy = PHY_B, },
	[PORT_C] = { .phy = PHY_C, },
};
static const struct intel_port_map bxt_port_map = {
	.ports = bxt_ports,
	.num_ports = ARRAY_SIZE(bxt_ports),
};

static const struct intel_port skl_tgp_ports[] = {
	[PORT_A] = { .phy = PHY_A, },
	[PORT_B] = { .phy = PHY_B, },
	[PORT_C] = { .phy = PHY_C, },
	[PORT_D] = { .phy = PHY_D, },
	/* DDI E not supported with TGP */
};
static const struct intel_port_map skl_tgp_port_map = {
	.ports = skl_tgp_ports,
	.num_ports = ARRAY_SIZE(skl_tgp_ports),
};

static const struct intel_port skl_spt_ports[] = {
	[PORT_A] = { .phy = PHY_A, },
	[PORT_B] = { .phy = PHY_B, },
	[PORT_C] = { .phy = PHY_C, },
	[PORT_D] = { .phy = PHY_D, },
	[PORT_E] = { .phy = PHY_E, },
};
static const struct intel_port_map skl_spt_port_map = {
	.ports = skl_spt_ports,
	.num_ports = ARRAY_SIZE(skl_spt_ports),
};

static const struct intel_port hsw_ports[] = {
	[PORT_A] = { .phy = PHY_A, },
	[PORT_B] = { .phy = PHY_B, },
	[PORT_C] = { .phy = PHY_C, },
	[PORT_D] = { .phy = PHY_D, },
	[PORT_E] = { .phy = PHY_E, },
};
static const struct intel_port_map hsw_port_map = {
	.ports = hsw_ports,
	.num_ports = ARRAY_SIZE(hsw_ports),
};

static const struct intel_port ilk_ports[] = {
	[PORT_A] = { .phy = PHY_A, },
	[PORT_B] = { .phy = PHY_B, },
	[PORT_C] = { .phy = PHY_C, },
	[PORT_D] = { .phy = PHY_D, },
};
static const struct intel_port_map ilk_port_map = {
	.ports = ilk_ports,
	.num_ports = ARRAY_SIZE(ilk_ports),
};

static const struct intel_port g4x_ports[] = {
	[PORT_B] = { .phy = PHY_B, },
	[PORT_C] = { .phy = PHY_C, },
	[PORT_D] = { .phy = PHY_D, },
};
static const struct intel_port_map g4x_port_map = {
	.ports = g4x_ports,
	.num_ports = ARRAY_SIZE(g4x_ports),
};

static const struct intel_port_map *
intel_port_map(struct intel_display *display)
{
	if (DISPLAY_VER(display) >= 13 ||
	    display->platform.alderlake_p) {
		return &xelpd_port_map;
	} else if (display->platform.dg2) {
		return &dg2_port_map;
	} else if (display->platform.dg1 ||
		   display->platform.rocketlake) {
		return &dg1_port_map;
	} else if (display->platform.alderlake_s) {
		return &adls_port_map;
	} else if (DISPLAY_VER(display) == 12) {
		return &tgl_port_map;
	} else if (display->platform.jasperlake ||
		   display->platform.elkhartlake) {
		return &ehl_port_map;
	} else if (DISPLAY_VER(display) == 11) {
		return &icl_port_map;
	} else if (display->platform.geminilake ||
		   display->platform.broxton) {
		return &bxt_port_map;
	} else if (DISPLAY_VER(display) == 9) {
		if (HAS_PCH_TGP(display))
			return &skl_tgp_port_map;
		else /* SPT/CNP */
			return &skl_spt_port_map;
	} else if (display->platform.broadwell ||
		   display->platform.haswell) {
		return &hsw_port_map;
	} else if (DISPLAY_VER(display) >= 5 && !HAS_GMCH(display)) {
		return &ilk_port_map;
	} else if (display->platform.cherryview ||
		   display->platform.valleyview ||
		   display->platform.g4x) {
		return &g4x_port_map;
	}

	MISSING_CASE(DISPLAY_VER(display));

	return NULL;
}

static bool port_is_valid(struct intel_display *display,
			  const struct intel_port_map *port_map,
			  enum port port)
{
	return port > PORT_NONE && port < port_map->num_ports &&
		DISPLAY_RUNTIME_INFO(display)->port_mask & BIT(port);
}

enum phy intel_port_map_phy(struct intel_display *display, enum port port)
{
	const struct intel_port_map *port_map = intel_port_map(display);

	if (port_is_valid(display, port_map, port))
		return port_map->ports[port].phy;

	MISSING_CASE(port);

	return PHY_NONE;
}
