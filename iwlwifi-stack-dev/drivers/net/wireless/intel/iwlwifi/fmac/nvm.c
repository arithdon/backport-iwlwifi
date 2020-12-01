// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (C) 2016 Intel Deutschland GmbH
 * Copyright (C) 2018-2019 Intel Corporation
 */
#include <linux/firmware.h>
#include <linux/rtnetlink.h>

#include "iwl-trans.h"
#include "iwl-csr.h"
#include "iwl-eeprom-parse.h"
#include "iwl-nvm-parse.h"

#include "fmac.h"

/* Default NVM size to read */
#define IWL_NVM_DEFAULT_CHUNK_SIZE (2 * 1024)

#define NVM_WRITE_OPCODE 1
#define NVM_READ_OPCODE 0

/* load nvm chunk response */
enum {
	READ_NVM_CHUNK_SUCCEED = 0,
	READ_NVM_CHUNK_NOT_VALID_ADDRESS = 1
};

/*
 * prepare the NVM host command w/ the pointers to the nvm buffer
 * and send it to fw
 */
static int iwl_nvm_write_chunk(struct iwl_fmac *fmac, u16 section,
			       u16 offset, u16 length, const u8 *data)
{
	struct iwl_nvm_access_cmd nvm_access_cmd = {
		.offset = cpu_to_le16(offset),
		.length = cpu_to_le16(length),
		.type = cpu_to_le16(section),
		.op_code = NVM_WRITE_OPCODE,
	};
	struct iwl_host_cmd cmd = {
		.id = NVM_ACCESS_CMD,
		.len = { sizeof(struct iwl_nvm_access_cmd), length },
		.flags = CMD_WANT_SKB | CMD_SEND_IN_RFKILL,
		.data = { &nvm_access_cmd, data },
		/* data may come from vmalloc, so use _DUP */
		.dataflags = { 0, IWL_HCMD_DFL_DUP },
	};
	struct iwl_rx_packet *pkt;
	struct iwl_nvm_access_resp *nvm_resp;
	int ret;

	ret = iwl_fmac_send_cmd(fmac, &cmd);
	if (ret)
		return ret;

	pkt = cmd.resp_pkt;
	/* Extract & check NVM write response */
	nvm_resp = (void *)pkt->data;
	if (le16_to_cpu(nvm_resp->status) != READ_NVM_CHUNK_SUCCEED) {
		IWL_ERR(fmac,
			"NVM access write command failed for section %u (status = 0x%x)\n",
			section, le16_to_cpu(nvm_resp->status));
		ret = -EIO;
	}

	iwl_free_resp(&cmd);
	return ret;
}

static int iwl_nvm_read_chunk(struct iwl_fmac *fmac, u16 section,
			      u16 offset, u16 length, u8 *data)
{
	struct iwl_nvm_access_cmd nvm_access_cmd = {
		.offset = cpu_to_le16(offset),
		.length = cpu_to_le16(length),
		.type = cpu_to_le16(section),
		.op_code = NVM_READ_OPCODE,
		.target = NVM_ACCESS_TARGET_CACHE
	};
	struct iwl_nvm_access_resp *nvm_resp;
	struct iwl_rx_packet *pkt;
	struct iwl_host_cmd cmd = {
		.id = NVM_ACCESS_CMD,
		.flags = CMD_WANT_SKB | CMD_SEND_IN_RFKILL,
		.data = { &nvm_access_cmd, },
	};
	int ret, bytes_read, offset_read;
	u8 *resp_data;

	cmd.len[0] = sizeof(struct iwl_nvm_access_cmd);

	ret = iwl_fmac_send_cmd(fmac, &cmd);
	if (ret)
		return ret;

	pkt = cmd.resp_pkt;

	/* Extract NVM response */
	nvm_resp = (void *)pkt->data;
	ret = le16_to_cpu(nvm_resp->status);
	bytes_read = le16_to_cpu(nvm_resp->length);
	offset_read = le16_to_cpu(nvm_resp->offset);
	resp_data = nvm_resp->data;
	if (ret) {
		if ((offset != 0) &&
		    (ret == READ_NVM_CHUNK_NOT_VALID_ADDRESS)) {
			/*
			 * meaning of NOT_VALID_ADDRESS:
			 * driver try to read chunk from address that is
			 * multiple of 2K and got an error since addr is empty.
			 * meaning of (offset != 0): driver already
			 * read valid data from another chunk so this case
			 * is not an error.
			 */
			IWL_DEBUG_EEPROM(fmac->trans->dev,
					 "NVM access command failed on offset 0x%x since that section size is multiple 2K\n",
					 offset);
			ret = 0;
		} else {
			IWL_DEBUG_EEPROM(fmac->trans->dev,
					 "NVM access command failed with status %d\n",
					 ret);
			ret = -EIO;
		}
		goto exit;
	}

	if (offset_read != offset) {
		IWL_ERR(fmac, "NVM ACCESS response with invalid offset %d\n",
			offset_read);
		ret = -EINVAL;
		goto exit;
	}

	/* Write data to NVM */
	memcpy(data + offset, resp_data, bytes_read);
	ret = bytes_read;

exit:
	iwl_free_resp(&cmd);
	return ret;
}

static int iwl_nvm_write_section(struct iwl_fmac *fmac, u16 section,
				 const u8 *data, u16 length)
{
	int offset = 0;

	/* copy data in chunks of 2k (and remainder if any) */

	while (offset < length) {
		int chunk_size, ret;

		chunk_size = min(IWL_NVM_DEFAULT_CHUNK_SIZE,
				 length - offset);

		ret = iwl_nvm_write_chunk(fmac, section, offset,
					  chunk_size, data + offset);
		if (ret < 0)
			return ret;

		offset += chunk_size;
	}

	return 0;
}

/*
 * Reads an NVM section completely.
 */
static int iwl_nvm_read_section(struct iwl_fmac *fmac, u16 section,
				u8 *data, u32 size_read)
{
	u16 length, offset = 0;
	int ret;

	/* Set nvm section read length */
	length = IWL_NVM_DEFAULT_CHUNK_SIZE;
	ret = length;

	/* Read the NVM until exhausted (reading less than requested) */
	while (ret == length) {
		ret = iwl_nvm_read_chunk(fmac, section, offset, length, data);
		if (ret < 0) {
			IWL_DEBUG_EEPROM(fmac->trans->dev,
					 "Cannot read NVM from section %d offset %d, length %d\n",
					 section, offset, length);
			return ret;
		}
		offset += ret;
	}

	IWL_DEBUG_EEPROM(fmac->trans->dev,
			 "NVM section %d read completed\n", section);
	return offset;
}

static struct iwl_nvm_data *
iwl_parse_nvm_sections(struct iwl_fmac *fmac)
{
	struct iwl_nvm_section *sections = fmac->nvm_sections;
	const __be16 *hw;
	const __le16 *sw, *calib, *regulatory, *mac_override, *phy_sku;

	/* SW and REGULATORY sections are mandatory */
	if (!fmac->nvm_sections[NVM_SECTION_TYPE_SW].data ||
	    !fmac->nvm_sections[NVM_SECTION_TYPE_REGULATORY].data) {
		IWL_ERR(fmac,
			"Can't parse empty family 8000 OTP/NVM sections\n");
		return NULL;
	}

	/* MAC_OVERRIDE or at least HW section must exist */
	if (!fmac->nvm_sections[fmac->cfg->nvm_hw_section_num].data &&
	    !fmac->nvm_sections[NVM_SECTION_TYPE_MAC_OVERRIDE].data) {
		IWL_ERR(fmac,
			"Can't parse mac_address, empty sections\n");
		return NULL;
	}

	/* PHY_SKU section is mandatory in B0 */
	if (!fmac->nvm_sections[NVM_SECTION_TYPE_PHY_SKU].data) {
		IWL_ERR(fmac,
			"Can't parse phy_sku in B0, empty sections\n");
		return NULL;
	}

	hw = (const __be16 *)sections[fmac->cfg->nvm_hw_section_num].data;
	sw = (const __le16 *)sections[NVM_SECTION_TYPE_SW].data;
	calib = (const __le16 *)sections[NVM_SECTION_TYPE_CALIBRATION].data;
	regulatory = (const __le16 *)sections[NVM_SECTION_TYPE_REGULATORY].data;
	mac_override =
		(const __le16 *)sections[NVM_SECTION_TYPE_MAC_OVERRIDE].data;
	phy_sku = (const __le16 *)sections[NVM_SECTION_TYPE_PHY_SKU].data;

	return iwl_parse_nvm_data(fmac->trans, fmac->cfg, fmac->fw, hw, sw,
				  calib, regulatory, mac_override, phy_sku,
				  fmac->fw->valid_tx_ant,
				  fmac->fw->valid_rx_ant);
}

/* Loads the NVM data stored in fmac->nvm_sections into the NIC */
int iwl_fmac_load_nvm_to_nic(struct iwl_fmac *fmac)
{
	int i, ret = 0;
	struct iwl_nvm_section *sections = fmac->nvm_sections;

	IWL_DEBUG_EEPROM(fmac->trans->dev, "'Write to NVM\n");

	for (i = 0; i < ARRAY_SIZE(fmac->nvm_sections); i++) {
		if (!fmac->nvm_sections[i].data ||
		    !fmac->nvm_sections[i].length)
			continue;
		ret = iwl_nvm_write_section(fmac, i, sections[i].data,
					    sections[i].length);
		if (ret < 0) {
			IWL_ERR(fmac, "iwl_fmac_send_cmd failed: %d\n", ret);
			break;
		}
	}
	return ret;
}

int iwl_fmac_nvm_init(struct iwl_fmac *fmac, bool read_nvm_from_nic)
{
	int ret, section;
	u32 size_read = 0;
	u8 *nvm_buffer, *temp;

	if (WARN_ON_ONCE(fmac->cfg->nvm_hw_section_num >= NVM_MAX_NUM_SECTIONS))
		return -EINVAL;

	/* load NVM values from nic */
	if (read_nvm_from_nic) {
		/* Read From FW NVM */
		IWL_DEBUG_EEPROM(fmac->trans->dev, "Read from NVM\n");

		nvm_buffer = kmalloc(fmac->trans->trans_cfg->base_params->eeprom_size,
				     GFP_KERNEL);
		if (!nvm_buffer)
			return -ENOMEM;
		for (section = 0; section < NVM_MAX_NUM_SECTIONS; section++) {
			/* we override the constness for initial read */
			ret = iwl_nvm_read_section(fmac, section, nvm_buffer,
						   size_read);
			if (ret < 0)
				continue;
			size_read += ret;
			temp = kmemdup(nvm_buffer, ret, GFP_KERNEL);
			if (!temp) {
				ret = -ENOMEM;
				break;
			}

			fmac->nvm_sections[section].data = temp;
			fmac->nvm_sections[section].length = ret;
		}
		if (!size_read)
			IWL_ERR(fmac, "OTP is blank\n");
		kfree(nvm_buffer);
	}

	/* Only if PNVM selected in the mod param - load external NVM  */
	if (fmac->nvm_file_name) {
		/* read External NVM file from the mod param */
		ret = iwl_read_external_nvm(fmac->trans, fmac->nvm_file_name,
					    fmac->nvm_sections);
		if (ret)
			return ret;
	}

	/* parse the relevant nvm sections */
	fmac->nvm_data = iwl_parse_nvm_sections(fmac);
	if (!fmac->nvm_data)
		return -ENODATA;
	IWL_DEBUG_EEPROM(fmac->trans->dev, "nvm version = %x\n",
			 fmac->nvm_data->nvm_version);

	return 0;
}

static void iwl_fmac_nvm_cmd_sku_cap(struct iwl_nvm_data *nvm_data,
				     struct iwl_fmac_nvm_cmd *cmd)
{
	if (nvm_data->sku_cap_band_24ghz_enable)
		cmd->sku_cap |= NVM_SKU_CAP_BAND_24GHZ_ENABLED;
	if (nvm_data->sku_cap_band_52ghz_enable)
		cmd->sku_cap |= NVM_SKU_CAP_BAND_52GHZ_ENABLED;
	if (nvm_data->sku_cap_11n_enable)
		cmd->sku_cap |= NVM_SKU_CAP_11N_ENABLED;
	if (nvm_data->sku_cap_11ac_enable)
		cmd->sku_cap |= NVM_SKU_CAP_11AC_ENABLED;
	if (nvm_data->sku_cap_amt_enable)
		cmd->sku_cap |= NVM_SKU_CAP_AMT_ENABLED;
	if (nvm_data->sku_cap_mimo_disabled)
		cmd->sku_cap |= NVM_SKU_CAP_MIMO_DISABLED;
	if (nvm_data->sku_cap_11ax_enable)
		cmd->sku_cap |= NVM_SKU_CAP_11AX_ENABLED;
}

static void iwl_fmac_nvm_cmd_ht(struct iwl_nvm_data *nvm_data,
				struct iwl_fmac_nvm_cmd *cmd)
{
	struct ieee80211_sta_ht_cap *ht_cap;
	struct iwl_fmac_nvm_ht *ht_cmd;
	int i;

	for (i = 0; i < NVM_NUM_BANDS; i++) {
		ht_cap = &nvm_data->bands[i].ht_cap;
		ht_cmd = &cmd->ht[i];

		ht_cmd->ht_supported = ht_cap->ht_supported;
		if (!ht_cap->ht_supported)
			continue;

		ht_cmd->cap = cpu_to_le16(ht_cap->cap);
		ht_cmd->ampdu_density = ht_cap->ampdu_density;
		ht_cmd->ampdu_factor = ht_cap->ampdu_factor;
		memcpy(ht_cmd->mcs.rx_mask, ht_cap->mcs.rx_mask,
		       sizeof(ht_cmd->mcs.rx_mask));
		ht_cmd->mcs.rx_highest = ht_cap->mcs.rx_highest;
		ht_cmd->mcs.tx_params = ht_cap->mcs.tx_params;
	}
}

static void iwl_fmac_nvm_cmd_vht(struct iwl_nvm_data *nvm_data,
				 struct iwl_fmac_nvm_cmd *cmd)
{
	struct ieee80211_sta_vht_cap *vht_cap;
	struct iwl_fmac_nvm_vht *vht_cmd;
	int i;

	for (i = 0; i < NVM_NUM_BANDS; i++) {
		vht_cap = &nvm_data->bands[i].vht_cap;
		vht_cmd = &cmd->vht[i];

		vht_cmd->vht_supported = vht_cap->vht_supported;
		if (!vht_cap->vht_supported)
			continue;

		vht_cmd->cap = cpu_to_le32(vht_cap->cap);
		vht_cmd->vht_mcs.rx_mcs_map = vht_cap->vht_mcs.rx_mcs_map;
		vht_cmd->vht_mcs.rx_highest = vht_cap->vht_mcs.rx_highest;
		vht_cmd->vht_mcs.tx_mcs_map = vht_cap->vht_mcs.tx_mcs_map;
		vht_cmd->vht_mcs.tx_highest = vht_cap->vht_mcs.tx_highest;
	}
}

int iwl_fmac_send_nvm_cmd(struct iwl_fmac *fmac)
{
	struct iwl_fmac_nvm_cmd cmd = {};
	struct iwl_host_cmd hcmd = {
		.id = iwl_cmd_id(FMAC_NVM, FMAC_GROUP, 0),
		.data = { &cmd, },
		.len = { sizeof(cmd), },
	};

	if (WARN_ON(!fmac->nvm_data)) {
		IWL_ERR(fmac, "nvm_data is NULL!!\n");
		return -EINVAL;
	}

	iwl_fmac_nvm_cmd_sku_cap(fmac->nvm_data, &cmd);

	cmd.n_addr = fmac->nvm_data->n_hw_addrs;
	ether_addr_copy(cmd.hw_addr, fmac->nvm_data->hw_addr);
	cmd.valid_ant = fmac->fw->valid_tx_ant;
	cmd.valid_ant |= fmac->fw->valid_rx_ant << 4;

	iwl_fmac_nvm_cmd_ht(fmac->nvm_data, &cmd);
	iwl_fmac_nvm_cmd_vht(fmac->nvm_data, &cmd);

	return iwl_fmac_send_cmd(fmac, &hcmd);
}
