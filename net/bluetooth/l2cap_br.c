/* l2cap_br.c - L2CAP BREDR oriented handling */

/*
 * Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <nanokernel.h>
#include <arch/cpu.h>
#include <toolchain.h>
#include <string.h>
#include <errno.h>
#include <atomic.h>
#include <misc/byteorder.h>
#include <misc/util.h>
#include <misc/nano_work.h>

#include <bluetooth/log.h>
#include <bluetooth/hci.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/driver.h>

#include "hci_core.h"
#include "conn_internal.h"
#include "l2cap_internal.h"

#if !defined(CONFIG_BLUETOOTH_DEBUG_L2CAP)
#undef BT_DBG
#define BT_DBG(fmt, ...)
#endif

#define BR_CHAN(_ch) CONTAINER_OF(_ch, struct bt_l2cap_br_chan, chan)

#define L2CAP_BR_PSM_START	0x0001
#define L2CAP_BR_PSM_END	0xffff

#define L2CAP_BR_DYN_CID_START	0x0040
#define L2CAP_BR_DYN_CID_END	0xffff

#define L2CAP_BR_MIN_MTU	48
#define L2CAP_BR_DEFAULT_MTU	672

#define L2CAP_BR_PSM_SDP	0x0001

/*
 * L2CAP extended feature mask:
 * BR/EDR fixed channel support enabled
 */
#define L2CAP_FEAT_FIXED_CHAN_MASK	0x00000080

/* Auxiliary L2CAP CoC flags making channel context */
enum {
	L2CAP_FLAG_LCONF_DONE,	/* local config accepted by remote */
	L2CAP_FLAG_RCONF_DONE,	/* remote config accepted by local */
	L2CAP_FLAG_ACCEPTOR,	/* getting incoming connection req on PSM */
};

static struct bt_l2cap_server *br_servers;
static struct bt_l2cap_fixed_chan *br_channels;

/* Pool for outgoing BR/EDR signaling packets, min MTU is 48 */
static struct nano_fifo br_sig;
static NET_BUF_POOL(br_sig_pool, CONFIG_BLUETOOTH_MAX_CONN,
		    BT_L2CAP_BUF_SIZE(L2CAP_BR_MIN_MTU), &br_sig, NULL,
		    BT_BUF_USER_DATA_MIN);

/* Set of flags applicable on "flags" member of bt_l2cap_br context */
enum {
	BT_L2CAP_FLAG_INFO_PENDING,	/* retrieving remote l2cap info */
	BT_L2CAP_FLAG_INFO_DONE,	/* remote l2cap info is done */
};

/* BR/EDR L2CAP signalling channel specific context */
struct bt_l2cap_br {
	/* The channel this context is associated with */
	struct bt_l2cap_br_chan	chan;
	atomic_t		flags[1];
	uint8_t			info_ident;
	uint8_t			info_fixed_chan;
	uint32_t		info_feat_mask;
};

static struct bt_l2cap_br bt_l2cap_br_pool[CONFIG_BLUETOOTH_MAX_CONN];

#if defined(CONFIG_BLUETOOTH_DEBUG_L2CAP)
static const char *state2str(bt_l2cap_chan_state_t state)
{
	switch (state) {
	case BT_L2CAP_DISCONNECTED:
		return "disconnected";
	case BT_L2CAP_CONNECT:
		return "connect";
	case BT_L2CAP_CONFIG:
		return "config";
	case BT_L2CAP_CONNECTED:
		return "connected";
	case BT_L2CAP_DISCONNECT:
		return "disconnect";
	default:
		return "unknown";
	}
}
#endif /* CONFIG_BLUETOOTH_DEBUG_L2CAP */

static void l2cap_br_state_set(struct bt_l2cap_chan *ch,
			       bt_l2cap_chan_state_t state)
{
	bt_l2cap_chan_state_t old_state = ch->state;

	BT_DBG("scid 0x%04x %s -> %s", BR_CHAN(ch)->rx.cid,
	       state2str(old_state), state2str(state));

	if (old_state == state) {
		BT_WARN("no transition");
		return;
	}

	/* check transitions validness */
	switch (state) {
	case BT_L2CAP_DISCONNECTED:
		/* regardless of old state always allows this state */
		break;
	case BT_L2CAP_CONNECT:
		if (old_state == BT_L2CAP_DISCONNECTED) {
			break;
		}

		BT_WARN("no valid transition");
		return;
	case BT_L2CAP_CONFIG:
		if (old_state == BT_L2CAP_CONNECT) {
			break;
		}

		BT_WARN("no valid transition");
		return;
	case BT_L2CAP_CONNECTED:
		if (old_state == BT_L2CAP_CONFIG) {
			break;
		}

		BT_WARN("no valid transition");
		return;
	case BT_L2CAP_DISCONNECT:
		if (old_state == BT_L2CAP_CONFIG ||
		    old_state == BT_L2CAP_CONNECTED) {
			break;
		}

		BT_WARN("no valid transition");
		return;
	default:
		BT_WARN("no valid (%u) state was set", state);
		return;
	}

	/* apply new valid channel state */
	ch->state = state;
}

struct bt_l2cap_chan *bt_l2cap_br_lookup_rx_cid(struct bt_conn *conn,
						uint16_t cid)
{
	struct bt_l2cap_chan *chan;

	for (chan = conn->channels; chan; chan = chan->_next) {
		struct bt_l2cap_br_chan *ch = BR_CHAN(chan);

		if (ch->rx.cid == cid) {
			return chan;
		}
	}

	return NULL;
}

static struct bt_l2cap_chan *bt_l2cap_br_lookup_tx_cid(struct bt_conn *conn,
						       uint16_t cid)
{
	struct bt_l2cap_chan *chan;

	for (chan = conn->channels; chan; chan = chan->_next) {
		struct bt_l2cap_br_chan *ch = BR_CHAN(chan);

		if (ch->tx.cid == cid) {
			return chan;
		}
	}

	return NULL;
}

static struct bt_l2cap_br_chan*
l2cap_br_chan_alloc_cid(struct bt_conn *conn, struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_br_chan *ch = BR_CHAN(chan);
	uint16_t cid;

	/*
	 * No action needed if there's already a CID allocated, e.g. in
	 * the case of a fixed channel.
	 */
	if (ch->rx.cid > 0) {
		return ch;
	}

	for (cid = L2CAP_BR_DYN_CID_START; cid <= L2CAP_BR_DYN_CID_END; cid++) {
		if (!bt_l2cap_br_lookup_rx_cid(conn, cid)) {
			ch->rx.cid = cid;
			return ch;
		}
	}

	return NULL;
}

static bool l2cap_br_chan_add(struct bt_conn *conn, struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_br_chan *ch = l2cap_br_chan_alloc_cid(conn, chan);

	if (!ch) {
		BT_DBG("Unable to allocate L2CAP CID");
		return false;
	}

	bt_l2cap_chan_add(conn, chan, NULL);

	return true;
}

static uint8_t l2cap_br_get_ident(void)
{
	static uint8_t ident;

	ident++;
	/* handle integer overflow (0 is not valid) */
	if (!ident) {
		ident++;
	}

	return ident;
}

static void l2cap_br_get_info(struct bt_l2cap_br *l2cap, uint16_t info_type)
{
	struct bt_l2cap_info_req *info;
	struct net_buf *buf;
	struct bt_l2cap_sig_hdr *hdr;

	BT_DBG("info type %u", info_type);

	if (atomic_test_bit(l2cap->flags, BT_L2CAP_FLAG_INFO_PENDING)) {
		return;
	}

	switch (info_type) {
	case BT_L2CAP_INFO_FEAT_MASK:
	case BT_L2CAP_INFO_FIXED_CHAN:
		break;
	default:
		BT_WARN("Unsupported info type %u", info_type);
		return;
	}

	buf = bt_l2cap_create_pdu(&br_sig);
	if (!buf) {
		BT_ERR("No buffers");
		return;
	}

	atomic_set_bit(l2cap->flags, BT_L2CAP_FLAG_INFO_PENDING);
	l2cap->info_ident = l2cap_br_get_ident();

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_INFO_REQ;
	hdr->ident = l2cap->info_ident;
	hdr->len = sys_cpu_to_le16(sizeof(*info));

	info = net_buf_add(buf, sizeof(*info));
	info->type = sys_cpu_to_le16(info_type);

	/* TODO: add command timeout guard */
	bt_l2cap_send(l2cap->chan.chan.conn, BT_L2CAP_CID_BR_SIG, buf);
}

static int l2cap_br_info_rsp(struct bt_l2cap_br *l2cap, uint8_t ident,
			     struct net_buf *buf)
{
	struct bt_l2cap_info_rsp *rsp = (void *)buf->data;
	uint16_t type, result;
	int err = 0;

	if (atomic_test_bit(l2cap->flags, BT_L2CAP_FLAG_INFO_DONE)) {
		return 0;
	}

	atomic_clear_bit(l2cap->flags, BT_L2CAP_FLAG_INFO_PENDING);

	if (buf->len < sizeof(*rsp)) {
		BT_ERR("Too small info rsp packet size");
		err = -EINVAL;
		goto done;
	}

	if (ident != l2cap->info_ident) {
		BT_WARN("Idents mismatch");
		err = -EINVAL;
		goto done;
	}

	result = sys_le16_to_cpu(rsp->result);
	if (result != BT_L2CAP_INFO_SUCCESS) {
		BT_WARN("Result unsuccessful");
		err = -EINVAL;
		goto done;
	}

	type = sys_le16_to_cpu(rsp->type);
	net_buf_pull(buf, sizeof(*rsp));

	switch (type) {
	case BT_L2CAP_INFO_FEAT_MASK:
		l2cap->info_feat_mask = net_buf_pull_le32(buf);
		BT_DBG("remote info mask 0x%08x", l2cap->info_feat_mask);

		if (!(l2cap->info_feat_mask & L2CAP_FEAT_FIXED_CHAN_MASK)) {
			break;
		}

		l2cap_br_get_info(l2cap, BT_L2CAP_INFO_FIXED_CHAN);
		return 0;
	case BT_L2CAP_INFO_FIXED_CHAN:
		l2cap->info_fixed_chan = net_buf_pull_u8(buf);
		BT_DBG("remote fixed channel mask 0x%02x",
		       l2cap->info_fixed_chan);
		break;
	default:
		BT_WARN("type 0x%04x unsupported", type);
		err = -EINVAL;
		break;
	}
done:
	atomic_set_bit(l2cap->flags, BT_L2CAP_FLAG_INFO_DONE);
	l2cap->info_ident = 0;
	return err;
}

static int l2cap_br_info_req(struct bt_l2cap_br *l2cap, uint8_t ident,
			     struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.chan.conn;
	struct bt_l2cap_info_req *req = (void *)buf->data;
	struct bt_l2cap_info_rsp *rsp;
	struct net_buf *rsp_buf;
	struct bt_l2cap_sig_hdr *hdr_info;
	uint16_t type;

	if (buf->len < sizeof(*req)) {
		BT_ERR("Too small info req packet size");
		return -EINVAL;
	}

	rsp_buf = bt_l2cap_create_pdu(&br_sig);
	if (!rsp_buf) {
		BT_ERR("No buffers");
		return -ENOMEM;
	}

	type = sys_le16_to_cpu(req->type);
	BT_DBG("type 0x%04x", type);

	hdr_info = net_buf_add(rsp_buf, sizeof(*hdr_info));
	hdr_info->code = BT_L2CAP_INFO_RSP;
	hdr_info->ident = ident;

	rsp = net_buf_add(rsp_buf, sizeof(*rsp));

	switch (type) {
	case BT_L2CAP_INFO_FEAT_MASK:
		rsp->type = sys_cpu_to_le16(BT_L2CAP_INFO_FEAT_MASK);
		rsp->result = sys_cpu_to_le16(BT_L2CAP_INFO_SUCCESS);
		net_buf_add_le32(rsp_buf, L2CAP_FEAT_FIXED_CHAN_MASK);
		hdr_info->len = sys_cpu_to_le16(sizeof(*rsp) + sizeof(uint32_t));
		break;
	case BT_L2CAP_INFO_FIXED_CHAN:
		rsp->type = sys_cpu_to_le16(BT_L2CAP_INFO_FIXED_CHAN);
		rsp->result = sys_cpu_to_le16(BT_L2CAP_INFO_SUCCESS);
		/* fixed channel mask protocol data is 8 octets wide */
		memset(net_buf_add(rsp_buf, 8), 0, 8);
		/* signaling channel is mandatory on BR/EDR transport */
		rsp->data[0] = BT_L2CAP_MASK_BR_SIG;
		hdr_info->len = sys_cpu_to_le16(sizeof(*rsp) + 8);
		break;
	default:
		rsp->type = req->type;
		rsp->result = sys_cpu_to_le16(BT_L2CAP_INFO_NOTSUPP);
		hdr_info->len = sys_cpu_to_le16(sizeof(*rsp));
		break;
	}

	bt_l2cap_send(conn, BT_L2CAP_CID_BR_SIG, rsp_buf);

	return 0;
}

void bt_l2cap_br_connected(struct bt_conn *conn)
{
	struct bt_l2cap_fixed_chan *fchan;
	struct bt_l2cap_chan *chan;
	struct bt_l2cap_br *l2cap;

	fchan = br_channels;

	for (; fchan; fchan = fchan->_next) {
		struct bt_l2cap_br_chan *ch;

		if (!fchan->accept) {
			continue;
		}

		if (fchan->accept(conn, &chan) < 0) {
			continue;
		}

		ch = BR_CHAN(chan);

		ch->rx.cid = fchan->cid;
		ch->tx.cid = fchan->cid;

		if (!l2cap_br_chan_add(conn, chan)) {
			return;
		}

		if (chan->ops && chan->ops->connected) {
			chan->ops->connected(chan);
		}
	}

	l2cap = CONTAINER_OF(chan, struct bt_l2cap_br, chan.chan);
	l2cap_br_get_info(l2cap, BT_L2CAP_INFO_FEAT_MASK);
}

static struct bt_l2cap_server *l2cap_br_server_lookup_psm(uint16_t psm)
{
	struct bt_l2cap_server *server;

	for (server = br_servers; server; server = server->_next) {
		if (server->psm == psm) {
			return server;
		}
	}

	return NULL;
}

static void l2cap_br_conf_add_mtu(struct net_buf *buf, const uint16_t mtu)
{
	net_buf_add_u8(buf, BT_L2CAP_CONF_OPT_MTU);
	net_buf_add_u8(buf, sizeof(mtu));
	net_buf_add_le16(buf, mtu);
}

static void l2cap_br_conf(struct bt_l2cap_chan *chan)
{
	struct bt_conn *conn = chan->conn;
	struct bt_l2cap_sig_hdr *hdr;
	struct bt_l2cap_conf_req *conf;
	struct net_buf *buf;

	buf = bt_l2cap_create_pdu(&br_sig);
	if (!buf) {
		return;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_CONF_REQ;
	hdr->ident = l2cap_br_get_ident();
	conf = net_buf_add(buf, sizeof(*conf));
	memset(conf, 0, sizeof(*conf));

	conf->dcid = sys_cpu_to_le16(BR_CHAN(chan)->tx.cid);
	/*
	 * Add MTU option if app set non default BR/EDR L2CAP MTU,
	 * otherwise sent emtpy configuration data meaning default MTU
	 * to be used.
	 */
	if (BR_CHAN(chan)->rx.mtu != L2CAP_BR_DEFAULT_MTU) {
		l2cap_br_conf_add_mtu(buf, BR_CHAN(chan)->rx.mtu);
	}

	hdr->len = sys_cpu_to_le16(buf->len - sizeof(*hdr));

	/*
	 * TODO:
	 * 1. start tracking number of configuration iterations on
	 *    on both directions
	 * 2. add individual command timeout guard
	 * 3. might be the option to add overall configuration phase
	 *    timeout (max 120sec)
	 */
	bt_l2cap_send(conn, BT_L2CAP_CID_BR_SIG, buf);
}

static bool l2cap_br_security_check(struct bt_l2cap_chan *chan,
				    const uint16_t psm)
{
	int check;

	if (psm == L2CAP_BR_PSM_SDP) {
		return false;
	};

	/* No link key needed and legacy devices */
	if (chan->required_sec_level == BT_SECURITY_LOW &&
	    !lmp_ssp_host_supported(chan->conn)) {
		return false;
	}

	check = bt_conn_security(chan->conn, chan->required_sec_level);

	/*
	 * Get case when connection security level already covers channel
	 * demands and bt_conn_security returns 0 and differentiate it to
	 * the case when HCI authentication request in internal subcall was send
	 * and bt_conn_security returns as well 0.
	 */
	if (check == 0 &&
	    chan->conn->sec_level >= chan->required_sec_level) {
		return false;
	}

	return (check == 0) ? true : false;
}

static void l2cap_br_conn_req(struct bt_l2cap_br *l2cap, uint8_t ident,
			      struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.chan.conn;
	struct bt_l2cap_chan *chan;
	struct bt_l2cap_server *server;
	struct bt_l2cap_conn_req *req = (void *)buf->data;
	struct bt_l2cap_conn_rsp *rsp;
	struct bt_l2cap_sig_hdr *hdr;
	uint16_t psm, scid, dcid, result, status = BT_L2CAP_CS_NO_INFO;

	if (buf->len < sizeof(*req)) {
		BT_ERR("Too small L2CAP conn req packet size");
		return;
	}

	psm = sys_le16_to_cpu(req->psm);
	scid = sys_le16_to_cpu(req->scid);
	dcid = 0;

	BT_DBG("psm 0x%02x scid 0x%04x", psm, scid);

	buf = bt_l2cap_create_pdu(&br_sig);
	if (!buf) {
		return;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_CONN_RSP;
	hdr->ident = ident;
	hdr->len = sys_cpu_to_le16(sizeof(*rsp));

	rsp = net_buf_add(buf, sizeof(*rsp));
	memset(rsp, 0, sizeof(*rsp));

	/* Check if there is a server registered */
	server = l2cap_br_server_lookup_psm(psm);
	if (!server) {
		result = BT_L2CAP_ERR_PSM_NOT_SUPP;
		goto done;
	}

	/*
	 * Report security violation for non SDP channel without encryption when
	 * remote supports SSP.
	 */
	if (psm != L2CAP_BR_PSM_SDP && lmp_ssp_host_supported(conn) &&
	    !conn->encrypt) {
		result = BT_L2CAP_ERR_SEC_BLOCK;
		goto done;
	}

	if (scid < L2CAP_BR_DYN_CID_START || scid > L2CAP_BR_DYN_CID_END) {
		result = BT_L2CAP_ERR_INVALID_SCID;
		goto done;
	}

	chan = bt_l2cap_br_lookup_tx_cid(conn, scid);
	if (chan) {
		result = BT_L2CAP_ERR_SCID_IN_USE;
		goto done;
	}

	/*
	 * Request server to accept the new connection and allocate the
	 * channel.
	 */
	if (server->accept(conn, &chan) < 0) {
		result = BT_L2CAP_ERR_NO_RESOURCES;
		goto done;
	}

	l2cap_br_chan_add(conn, chan);
	BR_CHAN(chan)->tx.cid = scid;
	dcid = BR_CHAN(chan)->rx.cid;
	l2cap_br_state_set(chan, BT_L2CAP_CONNECT);
	atomic_set_bit(BR_CHAN(chan)->flags, L2CAP_FLAG_ACCEPTOR);

	if (l2cap_br_security_check(chan, psm)) {
		result = BT_L2CAP_PENDING;
		status = BT_L2CAP_CS_AUTHEN_PEND;
		/* store ident for connection response after GAP done */
		chan->ident = ident;
		/* TODO: auth timeout */
	} else {
		result = BT_L2CAP_SUCCESS;
	}
done:
	rsp->dcid = sys_cpu_to_le16(dcid);
	rsp->scid = req->scid;
	rsp->result = sys_cpu_to_le16(result);
	rsp->status = sys_cpu_to_le16(status);

	bt_l2cap_send(conn, BT_L2CAP_CID_BR_SIG, buf);

	/* Disconnect link when security rules were violated */
	if (result == BT_L2CAP_ERR_SEC_BLOCK) {
		l2cap_br_state_set(chan, BT_L2CAP_DISCONNECTED);
		atomic_clear(BR_CHAN(chan)->flags);
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTHENTICATION_FAIL);
		return;
	}

	if (result == BT_L2CAP_SUCCESS) {
		l2cap_br_state_set(chan, BT_L2CAP_CONFIG);
		l2cap_br_conf(chan);
	}
}

static void l2cap_br_conf_rsp(struct bt_l2cap_br *l2cap, uint8_t ident,
			      uint16_t len, struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.chan.conn;
	struct bt_l2cap_chan *chan;
	struct bt_l2cap_conf_rsp *rsp = (void *)buf->data;
	uint16_t flags, scid, result, opt_len;

	if (buf->len < sizeof(*rsp)) {
		BT_ERR("Too small L2CAP conf rsp packet size");
		return;
	}

	flags = sys_le16_to_cpu(rsp->flags);
	scid = sys_le16_to_cpu(rsp->scid);
	result = sys_le16_to_cpu(rsp->result);
	opt_len = len - sizeof(*rsp);

	BT_DBG("scid 0x%04x flags 0x%02x result 0x%02x len %u", scid, flags,
	       result, opt_len);

	chan = bt_l2cap_br_lookup_rx_cid(conn, scid);
	if (!chan) {
		BT_ERR("channel mismatch!");
		return;
	}

	/*
	 * TODO: handle other results than success and parse response data if
	 * available
	 */
	switch (result) {
	case BT_L2CAP_CONF_SUCCESS:
		atomic_set_bit(BR_CHAN(chan)->flags, L2CAP_FLAG_LCONF_DONE);

		if (chan->state == BT_L2CAP_CONFIG &&
		    atomic_test_bit(BR_CHAN(chan)->flags,
				    L2CAP_FLAG_RCONF_DONE)) {
			BT_DBG("scid 0x%04x rx MTU %u dcid 0x%04x tx MTU %u",
			       BR_CHAN(chan)->rx.cid, BR_CHAN(chan)->rx.mtu,
			       BR_CHAN(chan)->tx.cid, BR_CHAN(chan)->tx.mtu);

			l2cap_br_state_set(chan, BT_L2CAP_CONNECTED);
			if (chan->ops && chan->ops->connected) {
				chan->ops->connected(chan);
			}
		}
		break;
	default:
		/* currently disconnect channel on non success result */
		bt_l2cap_chan_disconnect(chan);
		break;
	}
}

int bt_l2cap_br_server_register(struct bt_l2cap_server *server)
{
	if (server->psm < L2CAP_BR_PSM_START || !server->accept) {
		return -EINVAL;
	}

	/* PSM must be odd and lsb of upper byte must be 0 */
	if ((server->psm & 0x0101) != 0x0001) {
		return -EINVAL;
	}

	/* Check if given PSM is already in use */
	if (l2cap_br_server_lookup_psm(server->psm)) {
		BT_DBG("PSM already registered");
		return -EADDRINUSE;
	}

	BT_DBG("PSM 0x%04x", server->psm);

	server->_next = br_servers;
	br_servers = server;

	return 0;
}

static void l2cap_br_send_reject(struct bt_conn *conn, uint8_t ident,
				 uint16_t reason, void *data, uint8_t data_len)
{
	struct bt_l2cap_cmd_reject *rej;
	struct bt_l2cap_sig_hdr *hdr;
	struct net_buf *buf;

	buf = bt_l2cap_create_pdu(&br_sig);
	if (!buf) {
		return;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_CMD_REJECT;
	hdr->ident = ident;
	hdr->len = sys_cpu_to_le16(sizeof(*rej) + data_len);

	rej = net_buf_add(buf, sizeof(*rej));
	rej->reason = sys_cpu_to_le16(reason);

	/*
	 * optional data if available must be already in little-endian format
	 * made by caller.and be compliant with Core 4.2 [Vol 3, Part A, 4.1,
	 * table 4.4]
	 */
	if (data) {
		memcpy(net_buf_add(buf, data_len), data, data_len);
	}

	bt_l2cap_send(conn, BT_L2CAP_CID_BR_SIG, buf);
}

static uint16_t l2cap_br_conf_opt_mtu(struct bt_l2cap_chan *chan,
				      struct net_buf *buf, size_t len)
{
	uint16_t mtu, result = BT_L2CAP_CONF_SUCCESS;

	/* Core 4.2 [Vol 3, Part A, 5.1] MTU payload length */
	if (len != 2) {
		BT_ERR("tx MTU length %u invalid", len);
		result = BT_L2CAP_CONF_REJECT;
		goto done;
	}

	/* pulling MTU value moves buf data to next option item */
	mtu = net_buf_pull_le16(buf);
	if (mtu < L2CAP_BR_MIN_MTU) {
		result = BT_L2CAP_CONF_UNACCEPT;
		BR_CHAN(chan)->tx.mtu = L2CAP_BR_MIN_MTU;
		BT_DBG("tx MTU %u invalid", mtu);
		goto done;
	}

	BR_CHAN(chan)->tx.mtu = mtu;
	BT_DBG("tx MTU %u", mtu);
done:
	return result;
}

static void l2cap_br_conf_req(struct bt_l2cap_br *l2cap, uint8_t ident,
			      uint16_t len, struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.chan.conn;
	struct bt_l2cap_chan *chan;
	struct bt_l2cap_conf_req *req = (void *)buf->data;
	struct bt_l2cap_sig_hdr *hdr;
	struct bt_l2cap_conf_rsp *rsp;
	struct bt_l2cap_conf_opt *opt;
	uint16_t flags, dcid, opt_len, hint, result = BT_L2CAP_CONF_SUCCESS;

	if (buf->len < sizeof(*req)) {
		BT_ERR("Too small L2CAP conf req packet size");
		return;
	}

	flags = sys_le16_to_cpu(req->flags);
	dcid = sys_le16_to_cpu(req->dcid);
	opt_len = len - sizeof(*req);

	BT_DBG("dcid 0x%04x flags 0x%02x len %u", dcid, flags, opt_len);

	chan = bt_l2cap_br_lookup_rx_cid(conn, dcid);
	if (!chan) {
		BT_ERR("rx channel mismatch!");
		struct bt_l2cap_cmd_reject_cid_data data = {.scid = req->dcid,
							    .dcid = 0,
							   };

		l2cap_br_send_reject(conn, ident, BT_L2CAP_REJ_INVALID_CID,
				     &data, sizeof(data));
		return;
	}

	if (!opt_len) {
		BT_DBG("tx default MTU %u", L2CAP_BR_DEFAULT_MTU);
		BR_CHAN(chan)->tx.mtu = L2CAP_BR_DEFAULT_MTU;
		goto send_rsp;
	}

	/*
	 * initialize config option data dedicated object with proper
	 * offset set to beginnig of config options data
	 */
	opt = net_buf_pull(buf, sizeof(*req));

	while (buf->len >= sizeof(*opt)) {
		/* pull buf to always point to option data item */
		net_buf_pull(buf, sizeof(*opt));

		/* make sure opt object can get safe dereference in iteration */
		if (buf->len < opt->len) {
			BT_ERR("Received too short option data");
			result = BT_L2CAP_CONF_REJECT;
			break;
		}

		hint = opt->type & BT_L2CAP_CONF_HINT;

		switch (opt->type & BT_L2CAP_CONF_MASK) {
		case BT_L2CAP_CONF_OPT_MTU:
			/* getting MTU modifies buf internals */
			result = l2cap_br_conf_opt_mtu(chan, buf, opt->len);
			/*
			 * MTU is done. For now bailout the loop but later on
			 * there can be a need to continue checking next options
			 * that are after MTU value and then goto is not proper
			 * way out here.
			 */
			goto send_rsp;
		default:
			if (!hint) {
				BT_DBG("option %u not handled", opt->type);
				goto send_rsp;
			}

			/* set opt object to next option chunk */
			opt = net_buf_pull(buf, opt->len);
			break;
		}
	}

send_rsp:
	buf = bt_l2cap_create_pdu(&br_sig);
	if (!buf) {
		return;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_CONF_RSP;
	hdr->ident = ident;
	rsp = net_buf_add(buf, sizeof(*rsp));
	memset(rsp, 0, sizeof(*rsp));

	rsp->result = sys_cpu_to_le16(result);
	rsp->scid = sys_cpu_to_le16(BR_CHAN(chan)->tx.cid);

	/*
	 * TODO: If options other than MTU bacame meaningful then processing
	 * the options chain need to be modified and taken into account when
	 * sending back to peer.
	 */
	if (result == BT_L2CAP_CONF_UNACCEPT) {
		l2cap_br_conf_add_mtu(buf, BR_CHAN(chan)->tx.mtu);
	}

	hdr->len = sys_cpu_to_le16(buf->len - sizeof(*hdr));

	bt_l2cap_send(conn, BT_L2CAP_CID_BR_SIG, buf);

	if (result != BT_L2CAP_CONF_SUCCESS) {
		return;
	}

	atomic_set_bit(BR_CHAN(chan)->flags, L2CAP_FLAG_RCONF_DONE);

	if (atomic_test_bit(BR_CHAN(chan)->flags, L2CAP_FLAG_LCONF_DONE) &&
	    chan->state == BT_L2CAP_CONFIG) {
		BT_DBG("scid 0x%04x rx MTU %u dcid 0x%04x tx MTU %u",
		       BR_CHAN(chan)->rx.cid, BR_CHAN(chan)->rx.mtu,
		       BR_CHAN(chan)->tx.cid, BR_CHAN(chan)->tx.mtu);

		l2cap_br_state_set(chan, BT_L2CAP_CONNECTED);
		if (chan->ops && chan->ops->connected) {
			chan->ops->connected(chan);
		}
	}
}

static struct bt_l2cap_br_chan *l2cap_br_remove_tx_cid(struct bt_conn *conn,
						       uint16_t cid)
{
	struct bt_l2cap_chan *chan, *prev;

	for (chan = conn->channels, prev = NULL; chan;
	     prev = chan, chan = chan->_next) {
		/* get the app's l2cap object wherein this chan is contained */
		struct bt_l2cap_br_chan *ch = BR_CHAN(chan);

		if (ch->tx.cid != cid) {
			continue;
		}

		if (!prev) {
			conn->channels = chan->_next;
		} else {
			prev->_next = chan->_next;
		}

		return ch;
	}

	return NULL;
}

static void l2cap_br_disconn_req(struct bt_l2cap_br *l2cap, uint8_t ident,
				 struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.chan.conn;
	struct bt_l2cap_br_chan *chan;
	struct bt_l2cap_disconn_req *req = (void *)buf->data;
	struct bt_l2cap_disconn_rsp *rsp;
	struct bt_l2cap_sig_hdr *hdr;
	uint16_t scid, dcid;

	if (buf->len < sizeof(*req)) {
		BT_ERR("Too small disconn req packet size");
		return;
	}

	dcid = sys_le16_to_cpu(req->dcid);
	scid = sys_le16_to_cpu(req->scid);

	BT_DBG("scid 0x%04x dcid 0x%04x", dcid, scid);

	chan = l2cap_br_remove_tx_cid(conn, scid);
	if (!chan) {
		struct bt_l2cap_cmd_reject_cid_data data;

		data.scid = req->scid;
		data.dcid = req->dcid;
		l2cap_br_send_reject(conn, ident, BT_L2CAP_REJ_INVALID_CID,
				     &data, sizeof(data));
		return;
	}

	buf = bt_l2cap_create_pdu(&br_sig);
	if (!buf) {
		return;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_DISCONN_RSP;
	hdr->ident = ident;
	hdr->len = sys_cpu_to_le16(sizeof(*rsp));

	rsp = net_buf_add(buf, sizeof(*rsp));
	rsp->dcid = sys_cpu_to_le16(chan->rx.cid);
	rsp->scid = sys_cpu_to_le16(chan->tx.cid);

	bt_l2cap_chan_del(&chan->chan);

	bt_l2cap_send(conn, BT_L2CAP_CID_BR_SIG, buf);
}

static void l2cap_br_connected(struct bt_l2cap_chan *chan)
{
	BT_DBG("ch %p cid 0x%04x", BR_CHAN(chan), BR_CHAN(chan)->rx.cid);
}

static void l2cap_br_disconnected(struct bt_l2cap_chan *chan)
{
	BT_DBG("ch %p cid 0x%04x", BR_CHAN(chan), BR_CHAN(chan)->rx.cid);
}

int bt_l2cap_br_chan_disconnect(struct bt_l2cap_chan *chan)
{
	struct bt_conn *conn = chan->conn;
	struct net_buf *buf;
	struct bt_l2cap_disconn_req *req;
	struct bt_l2cap_sig_hdr *hdr;
	struct bt_l2cap_br_chan *ch;

	if (!conn) {
		return -ENOTCONN;
	}

	if (chan->state == BT_L2CAP_DISCONNECT) {
		return -EALREADY;
	}

	ch = BR_CHAN(chan);

	BT_DBG("chan %p scid 0x%04x dcid 0x%04x", chan, ch->rx.cid,
	       ch->tx.cid);

	buf = bt_l2cap_create_pdu(&br_sig);
	if (!buf) {
		BT_ERR("Unable to send L2CAP disconnect request");
		return -ENOMEM;
	}

	hdr = net_buf_add(buf, sizeof(*hdr));
	hdr->code = BT_L2CAP_DISCONN_REQ;
	hdr->ident = l2cap_br_get_ident();
	hdr->len = sys_cpu_to_le16(sizeof(*req));

	req = net_buf_add(buf, sizeof(*req));
	req->dcid = sys_cpu_to_le16(ch->tx.cid);
	req->scid = sys_cpu_to_le16(ch->rx.cid);

	bt_l2cap_send(chan->conn, BT_L2CAP_CID_BR_SIG, buf);
	l2cap_br_state_set(chan, BT_L2CAP_DISCONNECT);

	return 0;
}

static void l2cap_br_disconn_rsp(struct bt_l2cap_br *l2cap, uint8_t ident,
				 struct net_buf *buf)
{
	struct bt_conn *conn = l2cap->chan.chan.conn;
	struct bt_l2cap_br_chan *chan;
	struct bt_l2cap_disconn_rsp *rsp = (void *)buf->data;
	uint16_t dcid, scid;

	if (buf->len < sizeof(*rsp)) {
		BT_ERR("Too small disconn rsp packet size");
		return;
	}

	dcid = sys_le16_to_cpu(rsp->dcid);
	scid = sys_le16_to_cpu(rsp->scid);

	BT_DBG("dcid 0x%04x scid 0x%04x", dcid, scid);

	chan = l2cap_br_remove_tx_cid(conn, dcid);
	if (!chan) {
		BT_WARN("No dcid 0x%04x channel found", dcid);
		return;
	}

	bt_l2cap_chan_del(&chan->chan);
}

int bt_l2cap_br_chan_connect(struct bt_conn *conn, struct bt_l2cap_chan *chan,
			     uint16_t psm)
{
	return -ENOTSUP;
}

int bt_l2cap_br_chan_send(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	return -ENOTSUP;
}

static void l2cap_br_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	struct bt_l2cap_br *l2cap = CONTAINER_OF(chan, struct bt_l2cap_br, chan);
	struct bt_l2cap_sig_hdr *hdr = (void *)buf->data;
	uint16_t len;

	if (buf->len < sizeof(*hdr)) {
		BT_ERR("Too small L2CAP signaling PDU");
		return;
	}

	len = sys_le16_to_cpu(hdr->len);
	net_buf_pull(buf, sizeof(*hdr));

	BT_DBG("Signaling code 0x%02x ident %u len %u", hdr->code,
	       hdr->ident, len);

	if (buf->len != len) {
		BT_ERR("L2CAP length mismatch (%u != %u)", buf->len, len);
		return;
	}

	if (!hdr->ident) {
		BT_ERR("Invalid ident value in L2CAP PDU");
		return;
	}

	switch (hdr->code) {
	case BT_L2CAP_INFO_RSP:
		l2cap_br_info_rsp(l2cap, hdr->ident, buf);
		break;
	case BT_L2CAP_INFO_REQ:
		l2cap_br_info_req(l2cap, hdr->ident, buf);
		break;
	case BT_L2CAP_DISCONN_REQ:
		l2cap_br_disconn_req(l2cap, hdr->ident, buf);
		break;
	case BT_L2CAP_CONN_REQ:
		l2cap_br_conn_req(l2cap, hdr->ident, buf);
		break;
	case BT_L2CAP_CONF_RSP:
		l2cap_br_conf_rsp(l2cap, hdr->ident, len, buf);
		break;
	case BT_L2CAP_CONF_REQ:
		l2cap_br_conf_req(l2cap, hdr->ident, len, buf);
		break;
	case BT_L2CAP_DISCONN_RSP:
		l2cap_br_disconn_rsp(l2cap, hdr->ident, buf);
		break;
	default:
		BT_WARN("Unknown/Unsupported L2CAP PDU code 0x%02x", hdr->code);
		l2cap_br_send_reject(chan->conn, hdr->ident,
				     BT_L2CAP_REJ_NOT_UNDERSTOOD, NULL, 0);
		break;
	}
}

static void l2cap_br_conn_pend(struct bt_l2cap_chan *chan)
{
	struct net_buf *buf;
	struct bt_l2cap_conn_rsp *rsp;
	struct bt_l2cap_sig_hdr *hdr;

	if (chan->state != BT_L2CAP_CONNECT) {
		return;
	}

	/*
	 * For incoming connection state send confirming outstanding
	 * response and initiate configuration request.
	 */
	if (atomic_test_bit(BR_CHAN(chan)->flags, L2CAP_FLAG_ACCEPTOR)) {
		buf = bt_l2cap_create_pdu(&br_sig);
		if (!buf) {
			BT_ERR("No buffers for PDU");
			return;
		}

		hdr = net_buf_add(buf, sizeof(*hdr));
		hdr->code = BT_L2CAP_CONN_RSP;
		hdr->ident = chan->ident;
		hdr->len = sys_cpu_to_le16(sizeof(*rsp));

		rsp = net_buf_add(buf, sizeof(*rsp));
		memset(rsp, 0, sizeof(*rsp));

		rsp->dcid = sys_cpu_to_le16(BR_CHAN(chan)->rx.cid);
		rsp->scid = sys_cpu_to_le16(BR_CHAN(chan)->tx.cid);
		rsp->result = BT_L2CAP_SUCCESS;

		bt_l2cap_send(chan->conn, BT_L2CAP_CID_BR_SIG, buf);

		chan->ident = 0;
		l2cap_br_state_set(chan, BT_L2CAP_CONFIG);
		/*
		 * Initialize config request since remote needs to know
		 * local MTU segmentation.
		 */
		l2cap_br_conf(chan);
	}
}

void l2cap_br_encrypt_change(struct bt_conn *conn)
{
	struct bt_l2cap_chan *chan;

	for (chan = conn->channels; chan; chan = chan->_next) {
		l2cap_br_conn_pend(chan);

		if (chan->ops && chan->ops->encrypt_change) {
			chan->ops->encrypt_change(chan);
		}
	}
}

static int l2cap_br_accept(struct bt_conn *conn, struct bt_l2cap_chan **chan)
{
	int i;
	static struct bt_l2cap_chan_ops ops = {
		.connected = l2cap_br_connected,
		.disconnected = l2cap_br_disconnected,
		.recv = l2cap_br_recv,
	};

	BT_DBG("conn %p handle %u", conn, conn->handle);

	for (i = 0; i < ARRAY_SIZE(bt_l2cap_br_pool); i++) {
		struct bt_l2cap_br *l2cap = &bt_l2cap_br_pool[i];

		if (l2cap->chan.chan.conn) {
			continue;
		}

		l2cap->chan.chan.ops = &ops;
		*chan = &l2cap->chan.chan;
		atomic_set(l2cap->flags, 0);
		return 0;
	}

	BT_ERR("No available L2CAP context for conn %p", conn);

	return -ENOMEM;
}

static void bt_l2cap_br_fixed_chan_register(struct bt_l2cap_fixed_chan *chan)
{
	BT_DBG("CID 0x%04x", chan->cid);

	chan->_next = br_channels;
	br_channels = chan;
}

void bt_l2cap_br_init(void)
{
	static struct bt_l2cap_fixed_chan chan_br = {
			.cid	= BT_L2CAP_CID_BR_SIG,
			.mask	= BT_L2CAP_MASK_BR_SIG,
			.accept = l2cap_br_accept,
			};

	net_buf_pool_init(br_sig_pool);
	bt_l2cap_br_fixed_chan_register(&chan_br);
}
