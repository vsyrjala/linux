// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <drm/drm_print.h>

#include "intel_display.h"
#include "intel_display_core.h"
#include "intel_display_device.h"
#include "intel_display_utils.h"
#include "intel_display_types.h"
#include "intel_port_map.h"

struct intel_port {
	enum phy phy;
	enum hpd_pin hpd_pin;
	u8 ddc_pin;
};

struct intel_port_map {
	const struct intel_port *ports;
	u8 num_ports;
};

static const struct intel_port xelpd_ports[] = {
	[PORT_A]       = { .phy = PHY_A,   .hpd_pin = HPD_PORT_A,   .ddc_pin = GMBUS_PIN_1,      },
	[PORT_B]       = { .phy = PHY_B,   .hpd_pin = HPD_PORT_B,   .ddc_pin = GMBUS_PIN_2,      },
	[PORT_C]       = { .phy = PHY_C,   .hpd_pin = HPD_PORT_C,   .ddc_pin = GMBUS_PIN_3,      },
	[PORT_D_XELPD] = { .phy = PHY_D,   .hpd_pin = HPD_PORT_D,   .ddc_pin = GMBUS_PIN_4,      },
	[PORT_E_XELPD] = { .phy = PHY_E,   .hpd_pin = HPD_PORT_E,   .ddc_pin = GMBUS_PIN_5,      },
	[PORT_TC1]     = { .phy = PHY_TC1, .hpd_pin = HPD_PORT_TC1, .ddc_pin = GMBUS_PIN_9_TC1,  },
	[PORT_TC2]     = { .phy = PHY_TC2, .hpd_pin = HPD_PORT_TC2, .ddc_pin = GMBUS_PIN_10_TC2, },
	[PORT_TC3]     = { .phy = PHY_TC3, .hpd_pin = HPD_PORT_TC3, .ddc_pin = GMBUS_PIN_11_TC3, },
	[PORT_TC4]     = { .phy = PHY_TC4, .hpd_pin = HPD_PORT_TC4, .ddc_pin = GMBUS_PIN_12_TC4, },
};
static const struct intel_port_map xelpd_port_map = {
	.ports = xelpd_ports,
	.num_ports = ARRAY_SIZE(xelpd_ports),
};

static const struct intel_port dg2_ports[] = {
	[PORT_A]       = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,   .ddc_pin = GMBUS_PIN_1,     },
	[PORT_B]       = { .phy = PHY_B, .hpd_pin = HPD_PORT_B,   .ddc_pin = GMBUS_PIN_2,     },
	[PORT_C]       = { .phy = PHY_C, .hpd_pin = HPD_PORT_C,   .ddc_pin = GMBUS_PIN_3,     },
	[PORT_D_XELPD] = { .phy = PHY_D, .hpd_pin = HPD_PORT_D,   .ddc_pin = GMBUS_PIN_4,     },
	[PORT_TC1]     = { .phy = PHY_E, .hpd_pin = HPD_PORT_TC1, .ddc_pin = GMBUS_PIN_9_TC1, },
};
static const struct intel_port_map dg2_port_map = {
	.ports = dg2_ports,
	.num_ports = ARRAY_SIZE(dg2_ports),
};

static const struct intel_port dg1_ports[] = {
	[PORT_A]   = { .phy = PHY_A, .hpd_pin = HPD_PORT_A, .ddc_pin = GMBUS_PIN_1, },
	[PORT_B]   = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_2, },
	[PORT_TC1] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_3, },
	[PORT_TC2] = { .phy = PHY_D, .hpd_pin = HPD_PORT_D, .ddc_pin = GMBUS_PIN_4, },
};
static const struct intel_port_map dg1_port_map = {
	.ports = dg1_ports,
	.num_ports = ARRAY_SIZE(dg1_ports),
};

static const struct intel_port rkl_tgp_ports[] = {
	[PORT_A]   = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,   .ddc_pin = GMBUS_PIN_1,      },
	[PORT_B]   = { .phy = PHY_B, .hpd_pin = HPD_PORT_B,   .ddc_pin = GMBUS_PIN_2,      },
	[PORT_TC1] = { .phy = PHY_C, .hpd_pin = HPD_PORT_TC1, .ddc_pin = GMBUS_PIN_9_TC1,  },
	[PORT_TC2] = { .phy = PHY_D, .hpd_pin = HPD_PORT_TC2, .ddc_pin = GMBUS_PIN_10_TC2, },
};
static const struct intel_port_map rkl_tgp_port_map = {
	.ports = rkl_tgp_ports,
	.num_ports = ARRAY_SIZE(rkl_tgp_ports),
};

static const struct intel_port rkl_cmp_ports[] = {
	[PORT_A]   = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,                         },
	[PORT_B]   = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_2, },
	[PORT_TC1] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_3, },
	[PORT_TC2] = { .phy = PHY_D, .hpd_pin = HPD_PORT_D, .ddc_pin = GMBUS_PIN_4, },
};
static const struct intel_port_map rkl_cmp_port_map = {
	.ports = rkl_cmp_ports,
	.num_ports = ARRAY_SIZE(rkl_cmp_ports),
};

static const struct intel_port adls_ports[] = {
	[PORT_A]   = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,   .ddc_pin = GMBUS_PIN_1,      },
	[PORT_TC1] = { .phy = PHY_B, .hpd_pin = HPD_PORT_TC1, .ddc_pin = GMBUS_PIN_9_TC1,  },
	[PORT_TC2] = { .phy = PHY_C, .hpd_pin = HPD_PORT_TC2, .ddc_pin = GMBUS_PIN_10_TC2, },
	[PORT_TC3] = { .phy = PHY_D, .hpd_pin = HPD_PORT_TC3, .ddc_pin = GMBUS_PIN_11_TC3, },
	[PORT_TC4] = { .phy = PHY_E, .hpd_pin = HPD_PORT_TC4, .ddc_pin = GMBUS_PIN_12_TC4, },
};
static const struct intel_port_map adls_port_map = {
	.ports = adls_ports,
	.num_ports = ARRAY_SIZE(adls_ports),
};

static const struct intel_port tgl_ports[] = {
	[PORT_A]   = { .phy = PHY_A,   .hpd_pin = HPD_PORT_A,   .ddc_pin = GMBUS_PIN_1,      },
	[PORT_B]   = { .phy = PHY_B,   .hpd_pin = HPD_PORT_B,   .ddc_pin = GMBUS_PIN_2,      },
	[PORT_C]   = { .phy = PHY_C,   .hpd_pin = HPD_PORT_C,   .ddc_pin = GMBUS_PIN_3,      },
	[PORT_TC1] = { .phy = PHY_TC1, .hpd_pin = HPD_PORT_TC1, .ddc_pin = GMBUS_PIN_9_TC1,  },
	[PORT_TC2] = { .phy = PHY_TC2, .hpd_pin = HPD_PORT_TC2, .ddc_pin = GMBUS_PIN_10_TC2, },
	[PORT_TC3] = { .phy = PHY_TC3, .hpd_pin = HPD_PORT_TC3, .ddc_pin = GMBUS_PIN_11_TC3, },
	[PORT_TC4] = { .phy = PHY_TC4, .hpd_pin = HPD_PORT_TC4, .ddc_pin = GMBUS_PIN_12_TC4, },
	[PORT_TC5] = { .phy = PHY_TC5, .hpd_pin = HPD_PORT_TC5, .ddc_pin = GMBUS_PIN_13_TC5, },
	[PORT_TC6] = { .phy = PHY_TC6, .hpd_pin = HPD_PORT_TC6, .ddc_pin = GMBUS_PIN_14_TC6, },
};
static const struct intel_port_map tgl_port_map = {
	.ports = tgl_ports,
	.num_ports = ARRAY_SIZE(tgl_ports),
};

static const struct intel_port ehl_mcc_ports[] = {
	[PORT_A] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,   .ddc_pin = GMBUS_PIN_1,     },
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B,   .ddc_pin = GMBUS_PIN_2,     },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_TC1, .ddc_pin = GMBUS_PIN_9_TC1, },
	[PORT_D] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,   .ddc_pin = GMBUS_PIN_1,     },
};
static const struct intel_port_map ehl_mcc_port_map = {
	.ports = ehl_mcc_ports,
	.num_ports = ARRAY_SIZE(ehl_mcc_ports),
};

static const struct intel_port ehl_jsp_ports[] = {
	[PORT_A] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A, .ddc_pin = GMBUS_PIN_1, },
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_2, },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_3, },
	[PORT_D] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A, .ddc_pin = GMBUS_PIN_1, },
};
static const struct intel_port_map ehl_jsp_port_map = {
	.ports = ehl_jsp_ports,
	.num_ports = ARRAY_SIZE(ehl_jsp_ports),
};

static const struct intel_port icl_ports[] = {
	[PORT_A] = { .phy = PHY_A,   .hpd_pin = HPD_PORT_A,   .ddc_pin = GMBUS_PIN_1,      },
	[PORT_B] = { .phy = PHY_B,   .hpd_pin = HPD_PORT_B,   .ddc_pin = GMBUS_PIN_2,      },
	[PORT_C] = { .phy = PHY_TC1, .hpd_pin = HPD_PORT_TC1, .ddc_pin = GMBUS_PIN_9_TC1,  },
	[PORT_D] = { .phy = PHY_TC2, .hpd_pin = HPD_PORT_TC2, .ddc_pin = GMBUS_PIN_10_TC2, },
	[PORT_E] = { .phy = PHY_TC3, .hpd_pin = HPD_PORT_TC3, .ddc_pin = GMBUS_PIN_11_TC3, },
	[PORT_F] = { .phy = PHY_TC4, .hpd_pin = HPD_PORT_TC4, .ddc_pin = GMBUS_PIN_12_TC4, },
};
static const struct intel_port_map icl_port_map = {
	.ports = icl_ports,
	.num_ports = ARRAY_SIZE(icl_ports),
};

static const struct intel_port bxt_ports[] = {
	[PORT_A] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,                         },
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_1, },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_2, },
};
static const struct intel_port_map bxt_port_map = {
	.ports = bxt_ports,
	.num_ports = ARRAY_SIZE(bxt_ports),
};

static const struct intel_port skl_tgp_ports[] = {
	[PORT_A] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,                                },
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B,   .ddc_pin = GMBUS_PIN_2,      },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_TC1, .ddc_pin = GMBUS_PIN_9_TC1,  },
	[PORT_D] = { .phy = PHY_D, .hpd_pin = HPD_PORT_TC2, .ddc_pin = GMBUS_PIN_10_TC2, },
	/* DDI E not supported with TGP */
};
static const struct intel_port_map skl_tgp_port_map = {
	.ports = skl_tgp_ports,
	.num_ports = ARRAY_SIZE(skl_tgp_ports),
};

static const struct intel_port skl_cnp_ports[] = {
	[PORT_A] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,                         },
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_1, },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_2, },
	[PORT_D] = { .phy = PHY_D, .hpd_pin = HPD_PORT_D, .ddc_pin = GMBUS_PIN_4, },
	[PORT_E] = { .phy = PHY_E, .hpd_pin = HPD_PORT_E,                         },
};
static const struct intel_port_map skl_cnp_port_map = {
	.ports = skl_cnp_ports,
	.num_ports = ARRAY_SIZE(skl_cnp_ports),
};

static const struct intel_port skl_spt_ports[] = {
	[PORT_A] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,                           },
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_DPB, },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_DPC, },
	[PORT_D] = { .phy = PHY_D, .hpd_pin = HPD_PORT_D, .ddc_pin = GMBUS_PIN_DPD, },
	[PORT_E] = { .phy = PHY_E, .hpd_pin = HPD_PORT_E,                           },
};
static const struct intel_port_map skl_spt_port_map = {
	.ports = skl_spt_ports,
	.num_ports = ARRAY_SIZE(skl_spt_ports),
};

static const struct intel_port hsw_ports[] = {
	[PORT_A] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,                           },
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_DPB, },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_DPC, },
	[PORT_D] = { .phy = PHY_D, .hpd_pin = HPD_PORT_D, .ddc_pin = GMBUS_PIN_DPD, },
	[PORT_E] = { .phy = PHY_E,                                                  },
};
static const struct intel_port_map hsw_port_map = {
	.ports = hsw_ports,
	.num_ports = ARRAY_SIZE(hsw_ports),
};

static const struct intel_port ilk_ports[] = {
	[PORT_A] = { .phy = PHY_A, .hpd_pin = HPD_PORT_A,                           },
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_DPB, },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_DPC, },
	[PORT_D] = { .phy = PHY_D, .hpd_pin = HPD_PORT_D, .ddc_pin = GMBUS_PIN_DPD, },
};
static const struct intel_port_map ilk_port_map = {
	.ports = ilk_ports,
	.num_ports = ARRAY_SIZE(ilk_ports),
};

static const struct intel_port chv_ports[] = {
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_DPB,     },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_DPC,     },
	[PORT_D] = { .phy = PHY_D, .hpd_pin = HPD_PORT_D, .ddc_pin = GMBUS_PIN_DPD_CHV, },
};
static const struct intel_port_map chv_port_map = {
	.ports = chv_ports,
	.num_ports = ARRAY_SIZE(chv_ports),
};

static const struct intel_port g4x_ports[] = {
	[PORT_B] = { .phy = PHY_B, .hpd_pin = HPD_PORT_B, .ddc_pin = GMBUS_PIN_DPB, },
	[PORT_C] = { .phy = PHY_C, .hpd_pin = HPD_PORT_C, .ddc_pin = GMBUS_PIN_DPC, },
	[PORT_D] = { .phy = PHY_D, .hpd_pin = HPD_PORT_D,                           },
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
	} else if (display->platform.dg1) {
		return &dg1_port_map;
	} else if (display->platform.rocketlake) {
		if (HAS_PCH_TGP(display))
			return &rkl_tgp_port_map;
		else /* CMP */
			return &rkl_cmp_port_map;
	} else if (display->platform.alderlake_s) {
		return &adls_port_map;
	} else if (DISPLAY_VER(display) == 12) {
		return &tgl_port_map;
	} else if (display->platform.jasperlake ||
		   display->platform.elkhartlake) {
		if (HAS_PCH_TGP(display))
			return &ehl_mcc_port_map;
		else /* JSP */
			return &ehl_jsp_port_map;
	} else if (DISPLAY_VER(display) == 11) {
		return &icl_port_map;
	} else if (display->platform.geminilake ||
		   display->platform.broxton) {
		return &bxt_port_map;
	} else if (DISPLAY_VER(display) == 9) {
		if (HAS_PCH_TGP(display))
			return &skl_tgp_port_map;
		else if (HAS_PCH_CNP(display))
			return &skl_cnp_port_map;
		else /* SPT */
			return &skl_spt_port_map;
	} else if (display->platform.broadwell ||
		   display->platform.haswell) {
		return &hsw_port_map;
	} else if (DISPLAY_VER(display) >= 5 && !HAS_GMCH(display)) {
		return &ilk_port_map;
	} else if (display->platform.cherryview) {
		return &chv_port_map;
	} else if (display->platform.valleyview ||
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

enum tc_port intel_port_map_tc_port(struct intel_display *display, enum port port)
{
	enum phy phy = intel_port_map_phy(display, port);

	if (phy < PHY_TC1)
		return TC_PORT_NONE;

	return phy - PHY_TC1 + TC_PORT_1;
}

enum hpd_pin intel_port_map_hpd_pin(struct intel_encoder *encoder)
{
	struct intel_display *display = to_intel_display(encoder);
	const struct intel_port_map *port_map = intel_port_map(display);
	enum port port = encoder->port;

	if (port_is_valid(display, port_map, port) &&
	    port_map->ports[port].hpd_pin != HPD_NONE)
		return port_map->ports[port].hpd_pin;

	MISSING_CASE(port);

	return HPD_NONE;
}

u8 intel_port_map_ddc_pin(struct intel_encoder *encoder)
{
	struct intel_display *display = to_intel_display(encoder);
	const struct intel_port_map *port_map = intel_port_map(display);
	enum port port = encoder->port;

	if (port_is_valid(display, port_map, port) &&
	    port_map->ports[port].ddc_pin != 0)
		return port_map->ports[port].ddc_pin;

	MISSING_CASE(port);

	return 0;
}
