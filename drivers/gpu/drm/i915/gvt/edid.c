/*
 * Copyright(c) 2011-2016 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Ke Yu
 *    Zhiyuan Lv <zhiyuan.lv@intel.com>
 *
 * Contributors:
 *    Terrence Xu <terrence.xu@intel.com>
 *    Changbin Du <changbin.du@intel.com>
 *    Bing Niu <bing.niu@intel.com>
 *    Zhi Wang <zhi.a.wang@intel.com>
 *
 */

#include "i915_drv.h"
#include "display/intel_dp.h"
#include "gvt.h"

#define GMBUS1_TOTAL_BYTES_SHIFT 16
#define GMBUS1_TOTAL_BYTES_MASK 0x1ff
#define gmbus1_total_byte_count(v) (((v) >> \
	GMBUS1_TOTAL_BYTES_SHIFT) & GMBUS1_TOTAL_BYTES_MASK)
#define gmbus1_slave_addr(v) (((v) & 0xff) >> 1)
#define gmbus1_slave_index(v) (((v) >> 8) & 0xff)
#define gmbus1_bus_cycle(v) (((v) >> 25) & 0x7)

/* GMBUS0 bits definitions */
#define _GMBUS_PIN_SEL_MASK     (0x7)

static unsigned char edid_get_byte(struct intel_vgpu *vgpu,
				   struct intel_vgpu_display_path *disp_path)
{
	struct intel_vgpu_i2c_edid *edid = NULL;
	unsigned char chr = 0;

	if (!disp_path) {
		gvt_err("vgpu-%d invalid vgpu display path\n", vgpu->id);
		return 0;
	}

	edid = &disp_path->i2c_edid;

	if (edid->state == I2C_NOT_SPECIFIED || !edid->slave_selected) {
		gvt_vgpu_err("Driver tries to read EDID without proper sequence!\n");
		return 0;
	}

	if (!edid->edid_available) {
		gvt_vgpu_err("Reading EDID but EDID is not available!\n");
		return 0;
	}

	if (intel_vgpu_display_has_monitor(disp_path)) {
		struct intel_vgpu_edid_data *edid_data = disp_path->edid;

		chr = edid_data->edid_block[edid->current_edid_read];
		edid->current_edid_read++;
	} else {
		gvt_vgpu_err("No EDID available during the reading?\n");
	}
	return chr;
}

static inline int get_port_from_gmbus0(struct intel_vgpu *vgpu)
{
	struct drm_i915_private *dev_priv = vgpu->gvt->dev_priv;
	int port_select = vgpu_vreg_t(vgpu, PCH_GMBUS0) & _GMBUS_PIN_SEL_MASK;
	enum port port = PORT_NONE;

	if (IS_BROXTON(dev_priv)) {
		if (port_select == GMBUS_PIN_1_BXT)
			port = PORT_B;
		else if (port_select == GMBUS_PIN_2_BXT)
			port = PORT_C;
		else if (port_select == GMBUS_PIN_3_BXT)
			port = PORT_D;
	} else if (IS_COFFEELAKE(dev_priv)) {
		if (port_select == GMBUS_PIN_1_BXT)
			port = PORT_B;
		else if (port_select == GMBUS_PIN_2_BXT)
			port = PORT_C;
		else if (port_select == GMBUS_PIN_3_BXT)
			port = PORT_D;
		else if (port_select == GMBUS_PIN_4_CNP)
			port = PORT_E;
	} else {
		if (port_select == GMBUS_PIN_VGADDC)
			port = PORT_E;
		else if (port_select == GMBUS_PIN_DPC)
			port = PORT_C;
		else if (port_select == GMBUS_PIN_DPB)
			port = PORT_B;
		else if (port_select == GMBUS_PIN_DPD)
			port = PORT_D;
	}
	return port;
}

static void reset_gmbus_controller(struct intel_vgpu *vgpu)
{
	struct intel_vgpu_display *disp_cfg = &vgpu->disp_cfg;
	struct intel_vgpu_display_path *disp_path = NULL, *n;

	vgpu_vreg_t(vgpu, PCH_GMBUS2) = GMBUS_HW_RDY;
	list_for_each_entry_safe(disp_path, n, &disp_cfg->path_list, list) {
		if (!disp_path->i2c_edid.edid_available)
			vgpu_vreg_t(vgpu, PCH_GMBUS2) |= GMBUS_SATOER;
		disp_path->i2c_edid.gmbus.phase = GMBUS_IDLE_PHASE;
	}
}

/* GMBUS0 */
static int gmbus0_mmio_write(struct intel_vgpu *vgpu,
			unsigned int offset, void *p_data, unsigned int bytes)
{
	struct intel_vgpu_display *disp_cfg = &vgpu->disp_cfg;
	struct intel_vgpu_display_path *disp_path = NULL, *n;
	enum port port;

	memcpy(&vgpu_vreg(vgpu, offset), p_data, bytes);

	if ((vgpu_vreg_t(vgpu, PCH_GMBUS0) & _GMBUS_PIN_SEL_MASK) == 0)
		return 0;

	port = get_port_from_gmbus0(vgpu);
	if (WARN_ON(port == PORT_NONE))
		return 0;

	vgpu_vreg_t(vgpu, PCH_GMBUS2) &= ~GMBUS_ACTIVE;
	vgpu_vreg_t(vgpu, PCH_GMBUS2) |= GMBUS_HW_RDY | GMBUS_HW_WAIT_PHASE | GMBUS_SATOER;

	list_for_each_entry_safe(disp_path, n, &disp_cfg->path_list, list) {
		intel_vgpu_init_i2c_edid(vgpu, &disp_path->i2c_edid);
		disp_path->i2c_edid.state = I2C_GMBUS;
		disp_path->i2c_edid.gmbus.phase = GMBUS_IDLE_PHASE;
		if (disp_path->port == port &&
		    intel_vgpu_display_has_monitor(disp_path) &&
		    !intel_vgpu_display_is_dp(disp_path)) {
			disp_path->i2c_edid.port = port;
			disp_path->i2c_edid.edid_available = true;
			vgpu_vreg_t(vgpu, PCH_GMBUS2) &= ~GMBUS_SATOER;
		}
	}

	return 0;
}

static int gmbus1_mmio_write(struct intel_vgpu *vgpu, unsigned int offset,
		void *p_data, unsigned int bytes)
{
	struct intel_vgpu_display *disp_cfg = &vgpu->disp_cfg;
	struct intel_vgpu_display_path *disp_path = NULL, *n;
	struct intel_vgpu_i2c_edid *i2c_edid = NULL;
	enum port port;
	u32 slave_addr;
	u32 wvalue = *(u32 *)p_data;

	port = get_port_from_gmbus0(vgpu);
	list_for_each_entry_safe(disp_path, n, &disp_cfg->path_list, list) {
		if (disp_path->port == port)
			i2c_edid = &disp_path->i2c_edid;
	}

	if (!disp_path) {
		gvt_err("vgpu-%d invalid vgpu display path\n", vgpu->id);
		return -EINVAL;
	}

	if (vgpu_vreg(vgpu, offset) & GMBUS_SW_CLR_INT) {
		if (!(wvalue & GMBUS_SW_CLR_INT)) {
			vgpu_vreg(vgpu, offset) &= ~GMBUS_SW_CLR_INT;
			reset_gmbus_controller(vgpu);
		}
		/*
		 * TODO: "This bit is cleared to zero when an event
		 * causes the HW_RDY bit transition to occur "
		 */
	} else {
		/*
		 * per bspec setting this bit can cause:
		 * 1) INT status bit cleared
		 * 2) HW_RDY bit asserted
		 */
		if (wvalue & GMBUS_SW_CLR_INT) {
			vgpu_vreg_t(vgpu, PCH_GMBUS2) &= ~GMBUS_INT;
			vgpu_vreg_t(vgpu, PCH_GMBUS2) |= GMBUS_HW_RDY;
		}

		/* For virtualization, we suppose that HW is always ready,
		 * so GMBUS_SW_RDY should always be cleared
		 */
		if (wvalue & GMBUS_SW_RDY)
			wvalue &= ~GMBUS_SW_RDY;

		i2c_edid->gmbus.total_byte_count =
			gmbus1_total_byte_count(wvalue);
		slave_addr = gmbus1_slave_addr(wvalue);

		/* vgpu gmbus only support EDID */
		if (slave_addr == EDID_ADDR) {
			i2c_edid->slave_selected = true;
		} else if (slave_addr != 0) {
			gvt_dbg_dpy(
				"vgpu%d: unsupported gmbus slave addr(0x%x)\n"
				"	gmbus operations will be ignored.\n",
					vgpu->id, slave_addr);
		}

		if (wvalue & GMBUS_CYCLE_INDEX)
			i2c_edid->current_edid_read =
				gmbus1_slave_index(wvalue);

		i2c_edid->gmbus.cycle_type = gmbus1_bus_cycle(wvalue);
		switch (gmbus1_bus_cycle(wvalue)) {
		case GMBUS_NOCYCLE:
			break;
		case GMBUS_STOP:
			/* From spec:
			 * This can only cause a STOP to be generated
			 * if a GMBUS cycle is generated, the GMBUS is
			 * currently in a data/wait/idle phase, or it is in a
			 * WAIT phase
			 */
			if (gmbus1_bus_cycle(vgpu_vreg(vgpu, offset))
				!= GMBUS_NOCYCLE) {
				intel_vgpu_init_i2c_edid(vgpu, i2c_edid);
				/* After the 'stop' cycle, hw state would become
				 * 'stop phase' and then 'idle phase' after a
				 * few milliseconds. In emulation, we just set
				 * it as 'idle phase' ('stop phase' is not
				 * visible in gmbus interface)
				 */
				i2c_edid->gmbus.phase = GMBUS_IDLE_PHASE;
				vgpu_vreg_t(vgpu, PCH_GMBUS2) &= ~GMBUS_ACTIVE;
			}
			break;
		case NIDX_NS_W:
		case IDX_NS_W:
		case NIDX_STOP:
		case IDX_STOP:
			/* From hw spec the GMBUS phase
			 * transition like this:
			 * START (-->INDEX) -->DATA
			 */
			i2c_edid->gmbus.phase = GMBUS_DATA_PHASE;
			vgpu_vreg_t(vgpu, PCH_GMBUS2) |= GMBUS_ACTIVE;
			break;
		default:
			gvt_vgpu_err("Unknown/reserved GMBUS cycle detected!\n");
			break;
		}
		/*
		 * From hw spec the WAIT state will be
		 * cleared:
		 * (1) in a new GMBUS cycle
		 * (2) by generating a stop
		 */
		vgpu_vreg(vgpu, offset) = wvalue;
	}
	return 0;
}

static int gmbus3_mmio_write(struct intel_vgpu *vgpu, unsigned int offset,
	void *p_data, unsigned int bytes)
{
	WARN_ON(1);
	return 0;
}

static int gmbus3_mmio_read(struct intel_vgpu *vgpu, unsigned int offset,
		void *p_data, unsigned int bytes)
{
	struct intel_vgpu_display *disp_cfg = &vgpu->disp_cfg;
	struct intel_vgpu_display_path *disp_path = NULL, *n;
	struct intel_vgpu_i2c_edid *i2c_edid = NULL;
	unsigned char byte_data;
	int i = 0, byte_left = 0, byte_count = 0;
	u32 reg_data = 0;
	enum port port = get_port_from_gmbus0(vgpu);

	list_for_each_entry_safe(disp_path, n, &disp_cfg->path_list, list) {
		if (disp_path->port == port) {
			i2c_edid = &disp_path->i2c_edid;
			byte_left = i2c_edid->gmbus.total_byte_count -
				i2c_edid->current_edid_read;
			byte_count = byte_left;
			break;
		}
	}

	if (!disp_path) {
		gvt_err("vgpu-%d invalid vgpu display path\n", vgpu->id);
		return -EINVAL;
	}

	/* Data can only be recevied if previous settings correct */
	if (vgpu_vreg_t(vgpu, PCH_GMBUS1) & GMBUS_SLAVE_READ) {
		if (byte_left <= 0) {
			memcpy(p_data, &vgpu_vreg(vgpu, offset), bytes);
			return 0;
		}

		if (byte_count > 4)
			byte_count = 4;
		for (i = 0; i < byte_count; i++) {
			byte_data = edid_get_byte(vgpu, disp_path);
			reg_data |= (byte_data << (i << 3));
		}

		memcpy(&vgpu_vreg(vgpu, offset), &reg_data, byte_count);
		memcpy(p_data, &vgpu_vreg(vgpu, offset), bytes);

		if (byte_left <= 4) {
			switch (i2c_edid->gmbus.cycle_type) {
			case NIDX_STOP:
			case IDX_STOP:
				i2c_edid->gmbus.phase = GMBUS_IDLE_PHASE;
				break;
			case NIDX_NS_W:
			case IDX_NS_W:
			default:
				i2c_edid->gmbus.phase = GMBUS_WAIT_PHASE;
				break;
			}
			intel_vgpu_init_i2c_edid(vgpu, i2c_edid);
		}
		/*
		 * Read GMBUS3 during send operation,
		 * return the latest written value
		 */
	} else {
		memcpy(p_data, &vgpu_vreg(vgpu, offset), bytes);
		gvt_vgpu_err("warning: gmbus3 read with nothing returned\n");
	}
	return 0;
}

static int gmbus2_mmio_read(struct intel_vgpu *vgpu, unsigned int offset,
		void *p_data, unsigned int bytes)
{
	u32 value = vgpu_vreg(vgpu, offset);

	if (!(vgpu_vreg(vgpu, offset) & GMBUS_INUSE))
		vgpu_vreg(vgpu, offset) |= GMBUS_INUSE;
	memcpy(p_data, (void *)&value, bytes);
	return 0;
}

static int gmbus2_mmio_write(struct intel_vgpu *vgpu, unsigned int offset,
		void *p_data, unsigned int bytes)
{
	u32 wvalue = *(u32 *)p_data;

	if (wvalue & GMBUS_INUSE)
		vgpu_vreg(vgpu, offset) &= ~GMBUS_INUSE;
	/* All other bits are read-only */
	return 0;
}

/**
 * intel_gvt_i2c_handle_gmbus_read - emulate gmbus register mmio read
 * @vgpu: a vGPU
 * @offset: reg offset
 * @p_data: data return buffer
 * @bytes: access data length
 *
 * This function is used to emulate gmbus register mmio read
 *
 * Returns:
 * Zero on success, negative error code if failed.
 *
 */
int intel_gvt_i2c_handle_gmbus_read(struct intel_vgpu *vgpu,
	unsigned int offset, void *p_data, unsigned int bytes)
{
	if (WARN_ON(bytes > 8 && (offset & (bytes - 1))))
		return -EINVAL;

	if (offset == i915_mmio_reg_offset(PCH_GMBUS2))
		return gmbus2_mmio_read(vgpu, offset, p_data, bytes);
	else if (offset == i915_mmio_reg_offset(PCH_GMBUS3))
		return gmbus3_mmio_read(vgpu, offset, p_data, bytes);

	memcpy(p_data, &vgpu_vreg(vgpu, offset), bytes);
	return 0;
}

/**
 * intel_gvt_i2c_handle_gmbus_write - emulate gmbus register mmio write
 * @vgpu: a vGPU
 * @offset: reg offset
 * @p_data: data return buffer
 * @bytes: access data length
 *
 * This function is used to emulate gmbus register mmio write
 *
 * Returns:
 * Zero on success, negative error code if failed.
 *
 */
int intel_gvt_i2c_handle_gmbus_write(struct intel_vgpu *vgpu,
		unsigned int offset, void *p_data, unsigned int bytes)
{
	if (WARN_ON(bytes > 8 && (offset & (bytes - 1))))
		return -EINVAL;

	if (offset == i915_mmio_reg_offset(PCH_GMBUS0))
		return gmbus0_mmio_write(vgpu, offset, p_data, bytes);
	else if (offset == i915_mmio_reg_offset(PCH_GMBUS1))
		return gmbus1_mmio_write(vgpu, offset, p_data, bytes);
	else if (offset == i915_mmio_reg_offset(PCH_GMBUS2))
		return gmbus2_mmio_write(vgpu, offset, p_data, bytes);
	else if (offset == i915_mmio_reg_offset(PCH_GMBUS3))
		return gmbus3_mmio_write(vgpu, offset, p_data, bytes);

	memcpy(&vgpu_vreg(vgpu, offset), p_data, bytes);
	return 0;
}

enum {
	AUX_CH_CTL = 0,
	AUX_CH_DATA1,
	AUX_CH_DATA2,
	AUX_CH_DATA3,
	AUX_CH_DATA4,
	AUX_CH_DATA5
};

static inline int get_aux_ch_reg(unsigned int offset)
{
	int reg;

	switch (offset & 0xff) {
	case 0x10:
		reg = AUX_CH_CTL;
		break;
	case 0x14:
		reg = AUX_CH_DATA1;
		break;
	case 0x18:
		reg = AUX_CH_DATA2;
		break;
	case 0x1c:
		reg = AUX_CH_DATA3;
		break;
	case 0x20:
		reg = AUX_CH_DATA4;
		break;
	case 0x24:
		reg = AUX_CH_DATA5;
		break;
	default:
		reg = -1;
		break;
	}
	return reg;
}

#define AUX_CTL_MSG_LENGTH(reg) \
	((reg & DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) >> \
		DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT)

/**
 * intel_gvt_i2c_handle_aux_ch_write - emulate AUX channel register write
 * @vgpu: a vGPU
 * @port_idx: port index
 * @offset: reg offset
 * @p_data: write ptr
 *
 * This function is used to emulate AUX channel register write
 *
 */
void intel_gvt_i2c_handle_aux_ch_write(struct intel_vgpu *vgpu,
				       enum port port,
				       unsigned int offset,
				       void *p_data)
{
	struct intel_vgpu_display *disp_cfg = &vgpu->disp_cfg;
	struct intel_vgpu_display_path *disp_path = NULL, *n;
	struct intel_vgpu_i2c_edid *i2c_edid = NULL;
	int msg_length, ret_msg_size;
	int msg, addr, ctrl, op;
	u32 value = *(u32 *)p_data;
	int aux_data_for_write = 0;
	int reg = get_aux_ch_reg(offset);
	uint8_t rxbuf[20];
	size_t rxsize;

	if (reg != AUX_CH_CTL) {
		vgpu_vreg(vgpu, offset) = value;
		return;
	}

	list_for_each_entry_safe(disp_path, n, &disp_cfg->path_list, list) {
		if (disp_path->port == port) {
			i2c_edid = &disp_path->i2c_edid;
			break;
		}
	}

	if (!disp_path) {
		gvt_err("vgpu-%d invalid vgpu display path\n", vgpu->id);
		return;
	}

	msg_length = AUX_CTL_MSG_LENGTH(value);
	for (rxsize = 0; rxsize < msg_length; rxsize += 4)
		intel_dp_unpack_aux(vgpu_vreg(vgpu, offset + 4 + rxsize),
				rxbuf + rxsize, msg_length - rxsize);
	// check the msg in DATA register.
	msg = vgpu_vreg(vgpu, offset + 4);
	addr = (msg >> 8) & 0xffff;
	ctrl = (msg >> 24) & 0xff;
	op = ctrl >> 4;
	if (!(value & DP_AUX_CH_CTL_SEND_BUSY)) {
		/* The ctl write to clear some states */
		return;
	}

	/* Always set the wanted value for vms. */
	ret_msg_size = (((op & 0x1) == GVT_AUX_I2C_READ) ? 2 : 1);
	vgpu_vreg(vgpu, offset) =
		DP_AUX_CH_CTL_DONE |
		((ret_msg_size << DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT) &
		DP_AUX_CH_CTL_MESSAGE_SIZE_MASK);

	if (msg_length == 3) {
		if (!(op & GVT_AUX_I2C_MOT)) {
			/* stop */
			intel_vgpu_init_i2c_edid(vgpu, i2c_edid);
		} else {
			/* start or restart */
			i2c_edid->aux_ch.i2c_over_aux_ch = true;
			i2c_edid->aux_ch.aux_ch_mot = true;
			if (addr == 0) {
				/* reset the address */
				intel_vgpu_init_i2c_edid(vgpu, i2c_edid);
			} else if (addr == EDID_ADDR) {
				i2c_edid->state = I2C_AUX_CH;
				i2c_edid->port = port;
				i2c_edid->slave_selected = true;
				if (intel_vgpu_display_has_monitor(disp_path) &&
				    intel_vgpu_display_is_dp(disp_path))
					i2c_edid->edid_available = true;
			}
		}
	} else if ((op & 0x1) == GVT_AUX_I2C_WRITE) {
		/* We only support EDID reading from I2C_over_AUX.
		 * But if EDID has extension blocks, we use this write
		 * operation to set block starting address
		 */
		if (addr == EDID_ADDR) {
			i2c_edid->current_edid_read = rxbuf[4];
		}
	} else {
		if (WARN_ON((op & 0x1) != GVT_AUX_I2C_READ))
			return;
		if (WARN_ON(msg_length != 4))
			return;
		if (i2c_edid->edid_available && i2c_edid->slave_selected) {
			unsigned char val = edid_get_byte(vgpu, disp_path);

			aux_data_for_write = (val << 16);
		} else
			aux_data_for_write = (0xff << 16);
	}
	/* write the return value in AUX_CH_DATA reg which includes:
	 * ACK of I2C_WRITE
	 * returned byte if it is READ
	 */
	aux_data_for_write |= GVT_AUX_I2C_REPLY_ACK << 24;
	vgpu_vreg(vgpu, offset + 4) = aux_data_for_write;
}

/**
 * intel_vgpu_init_i2c_edid - initialize vGPU i2c edid emulation
 * @vgpu: a vGPU
 *
 * This function is used to initialize vGPU i2c edid emulation stuffs
 *
 */
void intel_vgpu_init_i2c_edid(struct intel_vgpu *vgpu,
			      struct intel_vgpu_i2c_edid *edid)
{
	edid->state = I2C_NOT_SPECIFIED;

	edid->port = -1;
	edid->slave_selected = false;
	edid->edid_available = false;
	edid->current_edid_read = 0;

	memset(&edid->gmbus, 0, sizeof(struct intel_vgpu_i2c_gmbus));

	edid->aux_ch.i2c_over_aux_ch = false;
	edid->aux_ch.aux_ch_mot = false;
}
