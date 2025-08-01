// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2013 - 2018 Intel Corporation. */

#include <linux/net/intel/libie/rx.h>

#include "iavf.h"
#include "iavf_ptp.h"
#include "iavf_prototype.h"

/**
 * iavf_send_pf_msg
 * @adapter: adapter structure
 * @op: virtual channel opcode
 * @msg: pointer to message buffer
 * @len: message length
 *
 * Send message to PF and print status if failure.
 **/
static int iavf_send_pf_msg(struct iavf_adapter *adapter,
			    enum virtchnl_ops op, u8 *msg, u16 len)
{
	struct iavf_hw *hw = &adapter->hw;
	enum iavf_status status;

	if (adapter->flags & IAVF_FLAG_PF_COMMS_FAILED)
		return 0; /* nothing to see here, move along */

	status = iavf_aq_send_msg_to_pf(hw, op, 0, msg, len, NULL);
	if (status)
		dev_dbg(&adapter->pdev->dev, "Unable to send opcode %d to PF, status %s, aq_err %s\n",
			op, iavf_stat_str(hw, status),
			libie_aq_str(hw->aq.asq_last_status));
	return iavf_status_to_errno(status);
}

/**
 * iavf_send_api_ver
 * @adapter: adapter structure
 *
 * Send API version admin queue message to the PF. The reply is not checked
 * in this function. Returns 0 if the message was successfully
 * sent, or one of the IAVF_ADMIN_QUEUE_ERROR_ statuses if not.
 **/
int iavf_send_api_ver(struct iavf_adapter *adapter)
{
	struct virtchnl_version_info vvi;

	vvi.major = VIRTCHNL_VERSION_MAJOR;
	vvi.minor = VIRTCHNL_VERSION_MINOR;

	return iavf_send_pf_msg(adapter, VIRTCHNL_OP_VERSION, (u8 *)&vvi,
				sizeof(vvi));
}

/**
 * iavf_poll_virtchnl_msg
 * @hw: HW configuration structure
 * @event: event to populate on success
 * @op_to_poll: requested virtchnl op to poll for
 *
 * Initialize poll for virtchnl msg matching the requested_op. Returns 0
 * if a message of the correct opcode is in the queue or an error code
 * if no message matching the op code is waiting and other failures.
 */
static int
iavf_poll_virtchnl_msg(struct iavf_hw *hw, struct iavf_arq_event_info *event,
		       enum virtchnl_ops op_to_poll)
{
	enum virtchnl_ops received_op;
	enum iavf_status status;
	u32 v_retval;

	while (1) {
		/* When the AQ is empty, iavf_clean_arq_element will return
		 * nonzero and this loop will terminate.
		 */
		status = iavf_clean_arq_element(hw, event, NULL);
		if (status != IAVF_SUCCESS)
			return iavf_status_to_errno(status);
		received_op =
		    (enum virtchnl_ops)le32_to_cpu(event->desc.cookie_high);

		if (received_op == VIRTCHNL_OP_EVENT) {
			struct iavf_adapter *adapter = hw->back;
			struct virtchnl_pf_event *vpe =
				(struct virtchnl_pf_event *)event->msg_buf;

			if (vpe->event != VIRTCHNL_EVENT_RESET_IMPENDING)
				continue;

			dev_info(&adapter->pdev->dev, "Reset indication received from the PF\n");
			if (!(adapter->flags & IAVF_FLAG_RESET_PENDING))
				iavf_schedule_reset(adapter,
						    IAVF_FLAG_RESET_PENDING);

			return -EIO;
		}

		if (op_to_poll == received_op)
			break;
	}

	v_retval = le32_to_cpu(event->desc.cookie_low);
	return virtchnl_status_to_errno((enum virtchnl_status_code)v_retval);
}

/**
 * iavf_verify_api_ver
 * @adapter: adapter structure
 *
 * Compare API versions with the PF. Must be called after admin queue is
 * initialized. Returns 0 if API versions match, -EIO if they do not,
 * IAVF_ERR_ADMIN_QUEUE_NO_WORK if the admin queue is empty, and any errors
 * from the firmware are propagated.
 **/
int iavf_verify_api_ver(struct iavf_adapter *adapter)
{
	struct iavf_arq_event_info event;
	int err;

	event.buf_len = IAVF_MAX_AQ_BUF_SIZE;
	event.msg_buf = kzalloc(IAVF_MAX_AQ_BUF_SIZE, GFP_KERNEL);
	if (!event.msg_buf)
		return -ENOMEM;

	err = iavf_poll_virtchnl_msg(&adapter->hw, &event, VIRTCHNL_OP_VERSION);
	if (!err) {
		struct virtchnl_version_info *pf_vvi =
			(struct virtchnl_version_info *)event.msg_buf;
		adapter->pf_version = *pf_vvi;

		if (pf_vvi->major > VIRTCHNL_VERSION_MAJOR ||
		    (pf_vvi->major == VIRTCHNL_VERSION_MAJOR &&
		     pf_vvi->minor > VIRTCHNL_VERSION_MINOR))
			err = -EIO;
	}

	kfree(event.msg_buf);

	return err;
}

/**
 * iavf_send_vf_config_msg
 * @adapter: adapter structure
 *
 * Send VF configuration request admin queue message to the PF. The reply
 * is not checked in this function. Returns 0 if the message was
 * successfully sent, or one of the IAVF_ADMIN_QUEUE_ERROR_ statuses if not.
 **/
int iavf_send_vf_config_msg(struct iavf_adapter *adapter)
{
	u32 caps;

	caps = VIRTCHNL_VF_OFFLOAD_L2 |
	       VIRTCHNL_VF_OFFLOAD_RSS_PF |
	       VIRTCHNL_VF_OFFLOAD_RSS_AQ |
	       VIRTCHNL_VF_OFFLOAD_RSS_REG |
	       VIRTCHNL_VF_OFFLOAD_VLAN |
	       VIRTCHNL_VF_OFFLOAD_WB_ON_ITR |
	       VIRTCHNL_VF_OFFLOAD_RSS_PCTYPE_V2 |
	       VIRTCHNL_VF_OFFLOAD_ENCAP |
	       VIRTCHNL_VF_OFFLOAD_TC_U32 |
	       VIRTCHNL_VF_OFFLOAD_VLAN_V2 |
	       VIRTCHNL_VF_OFFLOAD_RX_FLEX_DESC |
	       VIRTCHNL_VF_OFFLOAD_CRC |
	       VIRTCHNL_VF_OFFLOAD_ENCAP_CSUM |
	       VIRTCHNL_VF_OFFLOAD_REQ_QUEUES |
	       VIRTCHNL_VF_CAP_PTP |
	       VIRTCHNL_VF_OFFLOAD_ADQ |
	       VIRTCHNL_VF_OFFLOAD_USO |
	       VIRTCHNL_VF_OFFLOAD_FDIR_PF |
	       VIRTCHNL_VF_OFFLOAD_ADV_RSS_PF |
	       VIRTCHNL_VF_CAP_ADV_LINK_SPEED |
	       VIRTCHNL_VF_OFFLOAD_QOS;

	adapter->current_op = VIRTCHNL_OP_GET_VF_RESOURCES;
	adapter->aq_required &= ~IAVF_FLAG_AQ_GET_CONFIG;
	if (PF_IS_V11(adapter))
		return iavf_send_pf_msg(adapter, VIRTCHNL_OP_GET_VF_RESOURCES,
					(u8 *)&caps, sizeof(caps));
	else
		return iavf_send_pf_msg(adapter, VIRTCHNL_OP_GET_VF_RESOURCES,
					NULL, 0);
}

int iavf_send_vf_offload_vlan_v2_msg(struct iavf_adapter *adapter)
{
	adapter->aq_required &= ~IAVF_FLAG_AQ_GET_OFFLOAD_VLAN_V2_CAPS;

	if (!VLAN_V2_ALLOWED(adapter))
		return -EOPNOTSUPP;

	adapter->current_op = VIRTCHNL_OP_GET_OFFLOAD_VLAN_V2_CAPS;

	return iavf_send_pf_msg(adapter, VIRTCHNL_OP_GET_OFFLOAD_VLAN_V2_CAPS,
				NULL, 0);
}

int iavf_send_vf_supported_rxdids_msg(struct iavf_adapter *adapter)
{
	adapter->aq_required &= ~IAVF_FLAG_AQ_GET_SUPPORTED_RXDIDS;

	if (!IAVF_RXDID_ALLOWED(adapter))
		return -EOPNOTSUPP;

	adapter->current_op = VIRTCHNL_OP_GET_SUPPORTED_RXDIDS;

	return iavf_send_pf_msg(adapter, VIRTCHNL_OP_GET_SUPPORTED_RXDIDS,
				NULL, 0);
}

/**
 * iavf_send_vf_ptp_caps_msg - Send request for PTP capabilities
 * @adapter: private adapter structure
 *
 * Send the VIRTCHNL_OP_1588_PTP_GET_CAPS command to the PF to request the PTP
 * capabilities available to this device. This includes the following
 * potential access:
 *
 * * READ_PHC - access to read the PTP hardware clock time
 * * RX_TSTAMP - access to request Rx timestamps on all received packets
 *
 * The PF will reply with the same opcode a filled out copy of the
 * virtchnl_ptp_caps structure which defines the specifics of which features
 * are accessible to this device.
 *
 * Return: 0 if success, error code otherwise.
 */
int iavf_send_vf_ptp_caps_msg(struct iavf_adapter *adapter)
{
	struct virtchnl_ptp_caps hw_caps = {
		.caps = VIRTCHNL_1588_PTP_CAP_READ_PHC |
			VIRTCHNL_1588_PTP_CAP_RX_TSTAMP
	};

	adapter->aq_required &= ~IAVF_FLAG_AQ_GET_PTP_CAPS;

	if (!IAVF_PTP_ALLOWED(adapter))
		return -EOPNOTSUPP;

	adapter->current_op = VIRTCHNL_OP_1588_PTP_GET_CAPS;

	return iavf_send_pf_msg(adapter, VIRTCHNL_OP_1588_PTP_GET_CAPS,
				(u8 *)&hw_caps, sizeof(hw_caps));
}

/**
 * iavf_validate_num_queues
 * @adapter: adapter structure
 *
 * Validate that the number of queues the PF has sent in
 * VIRTCHNL_OP_GET_VF_RESOURCES is not larger than the VF can handle.
 **/
static void iavf_validate_num_queues(struct iavf_adapter *adapter)
{
	if (adapter->vf_res->num_queue_pairs > IAVF_MAX_REQ_QUEUES) {
		struct virtchnl_vsi_resource *vsi_res;
		int i;

		dev_info(&adapter->pdev->dev, "Received %d queues, but can only have a max of %d\n",
			 adapter->vf_res->num_queue_pairs,
			 IAVF_MAX_REQ_QUEUES);
		dev_info(&adapter->pdev->dev, "Fixing by reducing queues to %d\n",
			 IAVF_MAX_REQ_QUEUES);
		adapter->vf_res->num_queue_pairs = IAVF_MAX_REQ_QUEUES;
		for (i = 0; i < adapter->vf_res->num_vsis; i++) {
			vsi_res = &adapter->vf_res->vsi_res[i];
			vsi_res->num_queue_pairs = IAVF_MAX_REQ_QUEUES;
		}
	}
}

/**
 * iavf_get_vf_config
 * @adapter: private adapter structure
 *
 * Get VF configuration from PF and populate hw structure. Must be called after
 * admin queue is initialized. Busy waits until response is received from PF,
 * with maximum timeout. Response from PF is returned in the buffer for further
 * processing by the caller.
 **/
int iavf_get_vf_config(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = &adapter->hw;
	struct iavf_arq_event_info event;
	u16 len;
	int err;

	len = IAVF_VIRTCHNL_VF_RESOURCE_SIZE;
	event.buf_len = len;
	event.msg_buf = kzalloc(len, GFP_KERNEL);
	if (!event.msg_buf)
		return -ENOMEM;

	err = iavf_poll_virtchnl_msg(hw, &event, VIRTCHNL_OP_GET_VF_RESOURCES);
	memcpy(adapter->vf_res, event.msg_buf, min(event.msg_len, len));

	/* some PFs send more queues than we should have so validate that
	 * we aren't getting too many queues
	 */
	if (!err)
		iavf_validate_num_queues(adapter);
	iavf_vf_parse_hw_config(hw, adapter->vf_res);

	kfree(event.msg_buf);

	return err;
}

int iavf_get_vf_vlan_v2_caps(struct iavf_adapter *adapter)
{
	struct iavf_arq_event_info event;
	int err;
	u16 len;

	len = sizeof(struct virtchnl_vlan_caps);
	event.buf_len = len;
	event.msg_buf = kzalloc(len, GFP_KERNEL);
	if (!event.msg_buf)
		return -ENOMEM;

	err = iavf_poll_virtchnl_msg(&adapter->hw, &event,
				     VIRTCHNL_OP_GET_OFFLOAD_VLAN_V2_CAPS);
	if (!err)
		memcpy(&adapter->vlan_v2_caps, event.msg_buf,
		       min(event.msg_len, len));

	kfree(event.msg_buf);

	return err;
}

int iavf_get_vf_supported_rxdids(struct iavf_adapter *adapter)
{
	struct iavf_arq_event_info event;
	u64 rxdids;
	int err;

	event.msg_buf = (u8 *)&rxdids;
	event.buf_len = sizeof(rxdids);

	err = iavf_poll_virtchnl_msg(&adapter->hw, &event,
				     VIRTCHNL_OP_GET_SUPPORTED_RXDIDS);
	if (!err)
		adapter->supp_rxdids = rxdids;

	return err;
}

int iavf_get_vf_ptp_caps(struct iavf_adapter *adapter)
{
	struct virtchnl_ptp_caps caps = {};
	struct iavf_arq_event_info event;
	int err;

	event.msg_buf = (u8 *)&caps;
	event.buf_len = sizeof(caps);

	err = iavf_poll_virtchnl_msg(&adapter->hw, &event,
				     VIRTCHNL_OP_1588_PTP_GET_CAPS);
	if (!err)
		adapter->ptp.hw_caps = caps;

	return err;
}

/**
 * iavf_configure_queues
 * @adapter: adapter structure
 *
 * Request that the PF set up our (previously allocated) queues.
 **/
void iavf_configure_queues(struct iavf_adapter *adapter)
{
	struct virtchnl_vsi_queue_config_info *vqci;
	int pairs = adapter->num_active_queues;
	struct virtchnl_queue_pair_info *vqpi;
	u32 i, max_frame;
	u8 rx_flags = 0;
	size_t len;

	max_frame = LIBIE_MAX_RX_FRM_LEN(adapter->rx_rings->pp->p.offset);
	max_frame = min_not_zero(adapter->vf_res->max_mtu, max_frame);

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot configure queues, command %d pending\n",
			adapter->current_op);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_CONFIG_VSI_QUEUES;
	len = virtchnl_struct_size(vqci, qpair, pairs);
	vqci = kzalloc(len, GFP_KERNEL);
	if (!vqci)
		return;

	if (iavf_ptp_cap_supported(adapter, VIRTCHNL_1588_PTP_CAP_RX_TSTAMP))
		rx_flags |= VIRTCHNL_PTP_RX_TSTAMP;

	vqci->vsi_id = adapter->vsi_res->vsi_id;
	vqci->num_queue_pairs = pairs;
	vqpi = vqci->qpair;
	/* Size check is not needed here - HW max is 16 queue pairs, and we
	 * can fit info for 31 of them into the AQ buffer before it overflows.
	 */
	for (i = 0; i < pairs; i++) {
		vqpi->txq.vsi_id = vqci->vsi_id;
		vqpi->txq.queue_id = i;
		vqpi->txq.ring_len = adapter->tx_rings[i].count;
		vqpi->txq.dma_ring_addr = adapter->tx_rings[i].dma;
		vqpi->rxq.vsi_id = vqci->vsi_id;
		vqpi->rxq.queue_id = i;
		vqpi->rxq.ring_len = adapter->rx_rings[i].count;
		vqpi->rxq.dma_ring_addr = adapter->rx_rings[i].dma;
		vqpi->rxq.max_pkt_size = max_frame;
		vqpi->rxq.databuffer_size = adapter->rx_rings[i].rx_buf_len;
		if (IAVF_RXDID_ALLOWED(adapter))
			vqpi->rxq.rxdid = adapter->rxdid;
		if (CRC_OFFLOAD_ALLOWED(adapter))
			vqpi->rxq.crc_disable = !!(adapter->netdev->features &
						   NETIF_F_RXFCS);
		vqpi->rxq.flags = rx_flags;
		vqpi++;
	}

	adapter->aq_required &= ~IAVF_FLAG_AQ_CONFIGURE_QUEUES;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_CONFIG_VSI_QUEUES,
			 (u8 *)vqci, len);
	kfree(vqci);
}

/**
 * iavf_enable_queues
 * @adapter: adapter structure
 *
 * Request that the PF enable all of our queues.
 **/
void iavf_enable_queues(struct iavf_adapter *adapter)
{
	struct virtchnl_queue_select vqs;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot enable queues, command %d pending\n",
			adapter->current_op);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_ENABLE_QUEUES;
	vqs.vsi_id = adapter->vsi_res->vsi_id;
	vqs.tx_queues = BIT(adapter->num_active_queues) - 1;
	vqs.rx_queues = vqs.tx_queues;
	adapter->aq_required &= ~IAVF_FLAG_AQ_ENABLE_QUEUES;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_ENABLE_QUEUES,
			 (u8 *)&vqs, sizeof(vqs));
}

/**
 * iavf_disable_queues
 * @adapter: adapter structure
 *
 * Request that the PF disable all of our queues.
 **/
void iavf_disable_queues(struct iavf_adapter *adapter)
{
	struct virtchnl_queue_select vqs;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot disable queues, command %d pending\n",
			adapter->current_op);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_DISABLE_QUEUES;
	vqs.vsi_id = adapter->vsi_res->vsi_id;
	vqs.tx_queues = BIT(adapter->num_active_queues) - 1;
	vqs.rx_queues = vqs.tx_queues;
	adapter->aq_required &= ~IAVF_FLAG_AQ_DISABLE_QUEUES;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_DISABLE_QUEUES,
			 (u8 *)&vqs, sizeof(vqs));
}

/**
 * iavf_map_queues
 * @adapter: adapter structure
 *
 * Request that the PF map queues to interrupt vectors. Misc causes, including
 * admin queue, are always mapped to vector 0.
 **/
void iavf_map_queues(struct iavf_adapter *adapter)
{
	struct virtchnl_irq_map_info *vimi;
	struct virtchnl_vector_map *vecmap;
	struct iavf_q_vector *q_vector;
	int v_idx, q_vectors;
	size_t len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot map queues to vectors, command %d pending\n",
			adapter->current_op);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_CONFIG_IRQ_MAP;

	q_vectors = adapter->num_msix_vectors - NONQ_VECS;

	len = virtchnl_struct_size(vimi, vecmap, adapter->num_msix_vectors);
	vimi = kzalloc(len, GFP_KERNEL);
	if (!vimi)
		return;

	vimi->num_vectors = adapter->num_msix_vectors;
	/* Queue vectors first */
	for (v_idx = 0; v_idx < q_vectors; v_idx++) {
		q_vector = &adapter->q_vectors[v_idx];
		vecmap = &vimi->vecmap[v_idx];

		vecmap->vsi_id = adapter->vsi_res->vsi_id;
		vecmap->vector_id = v_idx + NONQ_VECS;
		vecmap->txq_map = q_vector->ring_mask;
		vecmap->rxq_map = q_vector->ring_mask;
		vecmap->rxitr_idx = IAVF_RX_ITR;
		vecmap->txitr_idx = IAVF_TX_ITR;
	}
	/* Misc vector last - this is only for AdminQ messages */
	vecmap = &vimi->vecmap[v_idx];
	vecmap->vsi_id = adapter->vsi_res->vsi_id;
	vecmap->vector_id = 0;
	vecmap->txq_map = 0;
	vecmap->rxq_map = 0;

	adapter->aq_required &= ~IAVF_FLAG_AQ_MAP_VECTORS;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_CONFIG_IRQ_MAP,
			 (u8 *)vimi, len);
	kfree(vimi);
}

/**
 * iavf_set_mac_addr_type - Set the correct request type from the filter type
 * @virtchnl_ether_addr: pointer to requested list element
 * @filter: pointer to requested filter
 **/
static void
iavf_set_mac_addr_type(struct virtchnl_ether_addr *virtchnl_ether_addr,
		       const struct iavf_mac_filter *filter)
{
	virtchnl_ether_addr->type = filter->is_primary ?
		VIRTCHNL_ETHER_ADDR_PRIMARY :
		VIRTCHNL_ETHER_ADDR_EXTRA;
}

/**
 * iavf_add_ether_addrs
 * @adapter: adapter structure
 *
 * Request that the PF add one or more addresses to our filters.
 **/
void iavf_add_ether_addrs(struct iavf_adapter *adapter)
{
	struct virtchnl_ether_addr_list *veal;
	struct iavf_mac_filter *f;
	int i = 0, count = 0;
	bool more = false;
	size_t len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot add filters, command %d pending\n",
			adapter->current_op);
		return;
	}

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	list_for_each_entry(f, &adapter->mac_filter_list, list) {
		if (f->add)
			count++;
	}
	if (!count) {
		adapter->aq_required &= ~IAVF_FLAG_AQ_ADD_MAC_FILTER;
		spin_unlock_bh(&adapter->mac_vlan_list_lock);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_ADD_ETH_ADDR;

	len = virtchnl_struct_size(veal, list, count);
	if (len > IAVF_MAX_AQ_BUF_SIZE) {
		dev_warn(&adapter->pdev->dev, "Too many add MAC changes in one request\n");
		while (len > IAVF_MAX_AQ_BUF_SIZE)
			len = virtchnl_struct_size(veal, list, --count);
		more = true;
	}

	veal = kzalloc(len, GFP_ATOMIC);
	if (!veal) {
		spin_unlock_bh(&adapter->mac_vlan_list_lock);
		return;
	}

	veal->vsi_id = adapter->vsi_res->vsi_id;
	veal->num_elements = count;
	list_for_each_entry(f, &adapter->mac_filter_list, list) {
		if (f->add) {
			ether_addr_copy(veal->list[i].addr, f->macaddr);
			iavf_set_mac_addr_type(&veal->list[i], f);
			i++;
			f->add = false;
			if (i == count)
				break;
		}
	}
	if (!more)
		adapter->aq_required &= ~IAVF_FLAG_AQ_ADD_MAC_FILTER;

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	iavf_send_pf_msg(adapter, VIRTCHNL_OP_ADD_ETH_ADDR, (u8 *)veal, len);
	kfree(veal);
}

/**
 * iavf_del_ether_addrs
 * @adapter: adapter structure
 *
 * Request that the PF remove one or more addresses from our filters.
 **/
void iavf_del_ether_addrs(struct iavf_adapter *adapter)
{
	struct virtchnl_ether_addr_list *veal;
	struct iavf_mac_filter *f, *ftmp;
	int i = 0, count = 0;
	bool more = false;
	size_t len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot remove filters, command %d pending\n",
			adapter->current_op);
		return;
	}

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	list_for_each_entry(f, &adapter->mac_filter_list, list) {
		if (f->remove)
			count++;
	}
	if (!count) {
		adapter->aq_required &= ~IAVF_FLAG_AQ_DEL_MAC_FILTER;
		spin_unlock_bh(&adapter->mac_vlan_list_lock);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_DEL_ETH_ADDR;

	len = virtchnl_struct_size(veal, list, count);
	if (len > IAVF_MAX_AQ_BUF_SIZE) {
		dev_warn(&adapter->pdev->dev, "Too many delete MAC changes in one request\n");
		while (len > IAVF_MAX_AQ_BUF_SIZE)
			len = virtchnl_struct_size(veal, list, --count);
		more = true;
	}
	veal = kzalloc(len, GFP_ATOMIC);
	if (!veal) {
		spin_unlock_bh(&adapter->mac_vlan_list_lock);
		return;
	}

	veal->vsi_id = adapter->vsi_res->vsi_id;
	veal->num_elements = count;
	list_for_each_entry_safe(f, ftmp, &adapter->mac_filter_list, list) {
		if (f->remove) {
			ether_addr_copy(veal->list[i].addr, f->macaddr);
			iavf_set_mac_addr_type(&veal->list[i], f);
			i++;
			list_del(&f->list);
			kfree(f);
			if (i == count)
				break;
		}
	}
	if (!more)
		adapter->aq_required &= ~IAVF_FLAG_AQ_DEL_MAC_FILTER;

	spin_unlock_bh(&adapter->mac_vlan_list_lock);

	iavf_send_pf_msg(adapter, VIRTCHNL_OP_DEL_ETH_ADDR, (u8 *)veal, len);
	kfree(veal);
}

/**
 * iavf_mac_add_ok
 * @adapter: adapter structure
 *
 * Submit list of filters based on PF response.
 **/
static void iavf_mac_add_ok(struct iavf_adapter *adapter)
{
	struct iavf_mac_filter *f, *ftmp;

	spin_lock_bh(&adapter->mac_vlan_list_lock);
	list_for_each_entry_safe(f, ftmp, &adapter->mac_filter_list, list) {
		f->is_new_mac = false;
		if (!f->add && !f->add_handled)
			f->add_handled = true;
	}
	spin_unlock_bh(&adapter->mac_vlan_list_lock);
}

/**
 * iavf_mac_add_reject
 * @adapter: adapter structure
 *
 * Remove filters from list based on PF response.
 **/
static void iavf_mac_add_reject(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct iavf_mac_filter *f, *ftmp;

	spin_lock_bh(&adapter->mac_vlan_list_lock);
	list_for_each_entry_safe(f, ftmp, &adapter->mac_filter_list, list) {
		if (f->remove && ether_addr_equal(f->macaddr, netdev->dev_addr))
			f->remove = false;

		if (!f->add && !f->add_handled)
			f->add_handled = true;

		if (f->is_new_mac) {
			list_del(&f->list);
			kfree(f);
		}
	}
	spin_unlock_bh(&adapter->mac_vlan_list_lock);
}

/**
 * iavf_vlan_add_reject
 * @adapter: adapter structure
 *
 * Remove VLAN filters from list based on PF response.
 **/
static void iavf_vlan_add_reject(struct iavf_adapter *adapter)
{
	struct iavf_vlan_filter *f, *ftmp;

	spin_lock_bh(&adapter->mac_vlan_list_lock);
	list_for_each_entry_safe(f, ftmp, &adapter->vlan_filter_list, list) {
		if (f->state == IAVF_VLAN_IS_NEW) {
			list_del(&f->list);
			kfree(f);
			adapter->num_vlan_filters--;
		}
	}
	spin_unlock_bh(&adapter->mac_vlan_list_lock);
}

/**
 * iavf_add_vlans
 * @adapter: adapter structure
 *
 * Request that the PF add one or more VLAN filters to our VSI.
 **/
void iavf_add_vlans(struct iavf_adapter *adapter)
{
	int len, i = 0, count = 0;
	struct iavf_vlan_filter *f;
	bool more = false;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot add VLANs, command %d pending\n",
			adapter->current_op);
		return;
	}

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	list_for_each_entry(f, &adapter->vlan_filter_list, list) {
		if (f->state == IAVF_VLAN_ADD)
			count++;
	}
	if (!count || !VLAN_FILTERING_ALLOWED(adapter)) {
		adapter->aq_required &= ~IAVF_FLAG_AQ_ADD_VLAN_FILTER;
		spin_unlock_bh(&adapter->mac_vlan_list_lock);
		return;
	}

	if (VLAN_ALLOWED(adapter)) {
		struct virtchnl_vlan_filter_list *vvfl;

		adapter->current_op = VIRTCHNL_OP_ADD_VLAN;

		len = virtchnl_struct_size(vvfl, vlan_id, count);
		if (len > IAVF_MAX_AQ_BUF_SIZE) {
			dev_warn(&adapter->pdev->dev, "Too many add VLAN changes in one request\n");
			while (len > IAVF_MAX_AQ_BUF_SIZE)
				len = virtchnl_struct_size(vvfl, vlan_id,
							   --count);
			more = true;
		}
		vvfl = kzalloc(len, GFP_ATOMIC);
		if (!vvfl) {
			spin_unlock_bh(&adapter->mac_vlan_list_lock);
			return;
		}

		vvfl->vsi_id = adapter->vsi_res->vsi_id;
		vvfl->num_elements = count;
		list_for_each_entry(f, &adapter->vlan_filter_list, list) {
			if (f->state == IAVF_VLAN_ADD) {
				vvfl->vlan_id[i] = f->vlan.vid;
				i++;
				f->state = IAVF_VLAN_IS_NEW;
				if (i == count)
					break;
			}
		}
		if (!more)
			adapter->aq_required &= ~IAVF_FLAG_AQ_ADD_VLAN_FILTER;

		spin_unlock_bh(&adapter->mac_vlan_list_lock);

		iavf_send_pf_msg(adapter, VIRTCHNL_OP_ADD_VLAN, (u8 *)vvfl, len);
		kfree(vvfl);
	} else {
		u16 max_vlans = adapter->vlan_v2_caps.filtering.max_filters;
		u16 current_vlans = iavf_get_num_vlans_added(adapter);
		struct virtchnl_vlan_filter_list_v2 *vvfl_v2;

		adapter->current_op = VIRTCHNL_OP_ADD_VLAN_V2;

		if ((count + current_vlans) > max_vlans &&
		    current_vlans < max_vlans) {
			count = max_vlans - iavf_get_num_vlans_added(adapter);
			more = true;
		}

		len = virtchnl_struct_size(vvfl_v2, filters, count);
		if (len > IAVF_MAX_AQ_BUF_SIZE) {
			dev_warn(&adapter->pdev->dev, "Too many add VLAN changes in one request\n");
			while (len > IAVF_MAX_AQ_BUF_SIZE)
				len = virtchnl_struct_size(vvfl_v2, filters,
							   --count);
			more = true;
		}

		vvfl_v2 = kzalloc(len, GFP_ATOMIC);
		if (!vvfl_v2) {
			spin_unlock_bh(&adapter->mac_vlan_list_lock);
			return;
		}

		vvfl_v2->vport_id = adapter->vsi_res->vsi_id;
		vvfl_v2->num_elements = count;
		list_for_each_entry(f, &adapter->vlan_filter_list, list) {
			if (f->state == IAVF_VLAN_ADD) {
				struct virtchnl_vlan_supported_caps *filtering_support =
					&adapter->vlan_v2_caps.filtering.filtering_support;
				struct virtchnl_vlan *vlan;

				if (i == count)
					break;

				/* give priority over outer if it's enabled */
				if (filtering_support->outer)
					vlan = &vvfl_v2->filters[i].outer;
				else
					vlan = &vvfl_v2->filters[i].inner;

				vlan->tci = f->vlan.vid;
				vlan->tpid = f->vlan.tpid;

				i++;
				f->state = IAVF_VLAN_IS_NEW;
			}
		}

		if (!more)
			adapter->aq_required &= ~IAVF_FLAG_AQ_ADD_VLAN_FILTER;

		spin_unlock_bh(&adapter->mac_vlan_list_lock);

		iavf_send_pf_msg(adapter, VIRTCHNL_OP_ADD_VLAN_V2,
				 (u8 *)vvfl_v2, len);
		kfree(vvfl_v2);
	}
}

/**
 * iavf_del_vlans
 * @adapter: adapter structure
 *
 * Request that the PF remove one or more VLAN filters from our VSI.
 **/
void iavf_del_vlans(struct iavf_adapter *adapter)
{
	struct iavf_vlan_filter *f, *ftmp;
	int len, i = 0, count = 0;
	bool more = false;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot remove VLANs, command %d pending\n",
			adapter->current_op);
		return;
	}

	spin_lock_bh(&adapter->mac_vlan_list_lock);

	list_for_each_entry_safe(f, ftmp, &adapter->vlan_filter_list, list) {
		/* since VLAN capabilities are not allowed, we dont want to send
		 * a VLAN delete request because it will most likely fail and
		 * create unnecessary errors/noise, so just free the VLAN
		 * filters marked for removal to enable bailing out before
		 * sending a virtchnl message
		 */
		if (f->state == IAVF_VLAN_REMOVE &&
		    !VLAN_FILTERING_ALLOWED(adapter)) {
			list_del(&f->list);
			kfree(f);
			adapter->num_vlan_filters--;
		} else if (f->state == IAVF_VLAN_DISABLE &&
		    !VLAN_FILTERING_ALLOWED(adapter)) {
			f->state = IAVF_VLAN_INACTIVE;
		} else if (f->state == IAVF_VLAN_REMOVE ||
			   f->state == IAVF_VLAN_DISABLE) {
			count++;
		}
	}
	if (!count || !VLAN_FILTERING_ALLOWED(adapter)) {
		adapter->aq_required &= ~IAVF_FLAG_AQ_DEL_VLAN_FILTER;
		spin_unlock_bh(&adapter->mac_vlan_list_lock);
		return;
	}

	if (VLAN_ALLOWED(adapter)) {
		struct virtchnl_vlan_filter_list *vvfl;

		adapter->current_op = VIRTCHNL_OP_DEL_VLAN;

		len = virtchnl_struct_size(vvfl, vlan_id, count);
		if (len > IAVF_MAX_AQ_BUF_SIZE) {
			dev_warn(&adapter->pdev->dev, "Too many delete VLAN changes in one request\n");
			while (len > IAVF_MAX_AQ_BUF_SIZE)
				len = virtchnl_struct_size(vvfl, vlan_id,
							   --count);
			more = true;
		}
		vvfl = kzalloc(len, GFP_ATOMIC);
		if (!vvfl) {
			spin_unlock_bh(&adapter->mac_vlan_list_lock);
			return;
		}

		vvfl->vsi_id = adapter->vsi_res->vsi_id;
		vvfl->num_elements = count;
		list_for_each_entry_safe(f, ftmp, &adapter->vlan_filter_list, list) {
			if (f->state == IAVF_VLAN_DISABLE) {
				vvfl->vlan_id[i] = f->vlan.vid;
				f->state = IAVF_VLAN_INACTIVE;
				i++;
				if (i == count)
					break;
			} else if (f->state == IAVF_VLAN_REMOVE) {
				vvfl->vlan_id[i] = f->vlan.vid;
				list_del(&f->list);
				kfree(f);
				adapter->num_vlan_filters--;
				i++;
				if (i == count)
					break;
			}
		}

		if (!more)
			adapter->aq_required &= ~IAVF_FLAG_AQ_DEL_VLAN_FILTER;

		spin_unlock_bh(&adapter->mac_vlan_list_lock);

		iavf_send_pf_msg(adapter, VIRTCHNL_OP_DEL_VLAN, (u8 *)vvfl, len);
		kfree(vvfl);
	} else {
		struct virtchnl_vlan_filter_list_v2 *vvfl_v2;

		adapter->current_op = VIRTCHNL_OP_DEL_VLAN_V2;

		len = virtchnl_struct_size(vvfl_v2, filters, count);
		if (len > IAVF_MAX_AQ_BUF_SIZE) {
			dev_warn(&adapter->pdev->dev, "Too many add VLAN changes in one request\n");
			while (len > IAVF_MAX_AQ_BUF_SIZE)
				len = virtchnl_struct_size(vvfl_v2, filters,
							   --count);
			more = true;
		}

		vvfl_v2 = kzalloc(len, GFP_ATOMIC);
		if (!vvfl_v2) {
			spin_unlock_bh(&adapter->mac_vlan_list_lock);
			return;
		}

		vvfl_v2->vport_id = adapter->vsi_res->vsi_id;
		vvfl_v2->num_elements = count;
		list_for_each_entry_safe(f, ftmp, &adapter->vlan_filter_list, list) {
			if (f->state == IAVF_VLAN_DISABLE ||
			    f->state == IAVF_VLAN_REMOVE) {
				struct virtchnl_vlan_supported_caps *filtering_support =
					&adapter->vlan_v2_caps.filtering.filtering_support;
				struct virtchnl_vlan *vlan;

				/* give priority over outer if it's enabled */
				if (filtering_support->outer)
					vlan = &vvfl_v2->filters[i].outer;
				else
					vlan = &vvfl_v2->filters[i].inner;

				vlan->tci = f->vlan.vid;
				vlan->tpid = f->vlan.tpid;

				if (f->state == IAVF_VLAN_DISABLE) {
					f->state = IAVF_VLAN_INACTIVE;
				} else {
					list_del(&f->list);
					kfree(f);
					adapter->num_vlan_filters--;
				}
				i++;
				if (i == count)
					break;
			}
		}

		if (!more)
			adapter->aq_required &= ~IAVF_FLAG_AQ_DEL_VLAN_FILTER;

		spin_unlock_bh(&adapter->mac_vlan_list_lock);

		iavf_send_pf_msg(adapter, VIRTCHNL_OP_DEL_VLAN_V2,
				 (u8 *)vvfl_v2, len);
		kfree(vvfl_v2);
	}
}

/**
 * iavf_set_promiscuous
 * @adapter: adapter structure
 *
 * Request that the PF enable promiscuous mode for our VSI.
 **/
void iavf_set_promiscuous(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct virtchnl_promisc_info vpi;
	unsigned int flags;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot set promiscuous mode, command %d pending\n",
			adapter->current_op);
		return;
	}

	/* prevent changes to promiscuous flags */
	spin_lock_bh(&adapter->current_netdev_promisc_flags_lock);

	/* sanity check to prevent duplicate AQ calls */
	if (!iavf_promiscuous_mode_changed(adapter)) {
		adapter->aq_required &= ~IAVF_FLAG_AQ_CONFIGURE_PROMISC_MODE;
		dev_dbg(&adapter->pdev->dev, "No change in promiscuous mode\n");
		/* allow changes to promiscuous flags */
		spin_unlock_bh(&adapter->current_netdev_promisc_flags_lock);
		return;
	}

	/* there are 2 bits, but only 3 states */
	if (!(netdev->flags & IFF_PROMISC) &&
	    netdev->flags & IFF_ALLMULTI) {
		/* State 1  - only multicast promiscuous mode enabled
		 * - !IFF_PROMISC && IFF_ALLMULTI
		 */
		flags = FLAG_VF_MULTICAST_PROMISC;
		adapter->current_netdev_promisc_flags |= IFF_ALLMULTI;
		adapter->current_netdev_promisc_flags &= ~IFF_PROMISC;
		dev_info(&adapter->pdev->dev, "Entering multicast promiscuous mode\n");
	} else if (!(netdev->flags & IFF_PROMISC) &&
		   !(netdev->flags & IFF_ALLMULTI)) {
		/* State 2 - unicast/multicast promiscuous mode disabled
		 * - !IFF_PROMISC && !IFF_ALLMULTI
		 */
		flags = 0;
		adapter->current_netdev_promisc_flags &=
			~(IFF_PROMISC | IFF_ALLMULTI);
		dev_info(&adapter->pdev->dev, "Leaving promiscuous mode\n");
	} else {
		/* State 3 - unicast/multicast promiscuous mode enabled
		 * - IFF_PROMISC && IFF_ALLMULTI
		 * - IFF_PROMISC && !IFF_ALLMULTI
		 */
		flags = FLAG_VF_UNICAST_PROMISC | FLAG_VF_MULTICAST_PROMISC;
		adapter->current_netdev_promisc_flags |= IFF_PROMISC;
		if (netdev->flags & IFF_ALLMULTI)
			adapter->current_netdev_promisc_flags |= IFF_ALLMULTI;
		else
			adapter->current_netdev_promisc_flags &= ~IFF_ALLMULTI;

		dev_info(&adapter->pdev->dev, "Entering promiscuous mode\n");
	}

	adapter->aq_required &= ~IAVF_FLAG_AQ_CONFIGURE_PROMISC_MODE;

	/* allow changes to promiscuous flags */
	spin_unlock_bh(&adapter->current_netdev_promisc_flags_lock);

	adapter->current_op = VIRTCHNL_OP_CONFIG_PROMISCUOUS_MODE;
	vpi.vsi_id = adapter->vsi_res->vsi_id;
	vpi.flags = flags;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_CONFIG_PROMISCUOUS_MODE,
			 (u8 *)&vpi, sizeof(vpi));
}

/**
 * iavf_request_stats
 * @adapter: adapter structure
 *
 * Request VSI statistics from PF.
 **/
void iavf_request_stats(struct iavf_adapter *adapter)
{
	struct virtchnl_queue_select vqs;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* no error message, this isn't crucial */
		return;
	}

	adapter->aq_required &= ~IAVF_FLAG_AQ_REQUEST_STATS;
	adapter->current_op = VIRTCHNL_OP_GET_STATS;
	vqs.vsi_id = adapter->vsi_res->vsi_id;
	/* queue maps are ignored for this message - only the vsi is used */
	if (iavf_send_pf_msg(adapter, VIRTCHNL_OP_GET_STATS, (u8 *)&vqs,
			     sizeof(vqs)))
		/* if the request failed, don't lock out others */
		adapter->current_op = VIRTCHNL_OP_UNKNOWN;
}

/**
 * iavf_get_rss_hashcfg
 * @adapter: adapter structure
 *
 * Request RSS Hash enable bits from PF
 **/
void iavf_get_rss_hashcfg(struct iavf_adapter *adapter)
{
	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot get RSS hash capabilities, command %d pending\n",
			adapter->current_op);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_GET_RSS_HASHCFG_CAPS;
	adapter->aq_required &= ~IAVF_FLAG_AQ_GET_RSS_HASHCFG;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_GET_RSS_HASHCFG_CAPS, NULL, 0);
}

/**
 * iavf_set_rss_hashcfg
 * @adapter: adapter structure
 *
 * Request the PF to set our RSS hash capabilities
 **/
void iavf_set_rss_hashcfg(struct iavf_adapter *adapter)
{
	struct virtchnl_rss_hashcfg vrh;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot set RSS hash enable, command %d pending\n",
			adapter->current_op);
		return;
	}
	vrh.hashcfg = adapter->rss_hashcfg;
	adapter->current_op = VIRTCHNL_OP_SET_RSS_HASHCFG;
	adapter->aq_required &= ~IAVF_FLAG_AQ_SET_RSS_HASHCFG;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_SET_RSS_HASHCFG, (u8 *)&vrh,
			 sizeof(vrh));
}

/**
 * iavf_set_rss_key
 * @adapter: adapter structure
 *
 * Request the PF to set our RSS hash key
 **/
void iavf_set_rss_key(struct iavf_adapter *adapter)
{
	struct virtchnl_rss_key *vrk;
	int len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot set RSS key, command %d pending\n",
			adapter->current_op);
		return;
	}
	len = virtchnl_struct_size(vrk, key, adapter->rss_key_size);
	vrk = kzalloc(len, GFP_KERNEL);
	if (!vrk)
		return;
	vrk->vsi_id = adapter->vsi.id;
	vrk->key_len = adapter->rss_key_size;
	memcpy(vrk->key, adapter->rss_key, adapter->rss_key_size);

	adapter->current_op = VIRTCHNL_OP_CONFIG_RSS_KEY;
	adapter->aq_required &= ~IAVF_FLAG_AQ_SET_RSS_KEY;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_CONFIG_RSS_KEY, (u8 *)vrk, len);
	kfree(vrk);
}

/**
 * iavf_set_rss_lut
 * @adapter: adapter structure
 *
 * Request the PF to set our RSS lookup table
 **/
void iavf_set_rss_lut(struct iavf_adapter *adapter)
{
	struct virtchnl_rss_lut *vrl;
	int len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot set RSS LUT, command %d pending\n",
			adapter->current_op);
		return;
	}
	len = virtchnl_struct_size(vrl, lut, adapter->rss_lut_size);
	vrl = kzalloc(len, GFP_KERNEL);
	if (!vrl)
		return;
	vrl->vsi_id = adapter->vsi.id;
	vrl->lut_entries = adapter->rss_lut_size;
	memcpy(vrl->lut, adapter->rss_lut, adapter->rss_lut_size);
	adapter->current_op = VIRTCHNL_OP_CONFIG_RSS_LUT;
	adapter->aq_required &= ~IAVF_FLAG_AQ_SET_RSS_LUT;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_CONFIG_RSS_LUT, (u8 *)vrl, len);
	kfree(vrl);
}

/**
 * iavf_set_rss_hfunc
 * @adapter: adapter structure
 *
 * Request the PF to set our RSS Hash function
 **/
void iavf_set_rss_hfunc(struct iavf_adapter *adapter)
{
	struct virtchnl_rss_hfunc *vrh;
	int len = sizeof(*vrh);

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot set RSS Hash function, command %d pending\n",
			adapter->current_op);
		return;
	}
	vrh = kzalloc(len, GFP_KERNEL);
	if (!vrh)
		return;
	vrh->vsi_id = adapter->vsi.id;
	vrh->rss_algorithm = adapter->hfunc;
	adapter->current_op = VIRTCHNL_OP_CONFIG_RSS_HFUNC;
	adapter->aq_required &= ~IAVF_FLAG_AQ_SET_RSS_HFUNC;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_CONFIG_RSS_HFUNC, (u8 *)vrh, len);
	kfree(vrh);
}

/**
 * iavf_enable_vlan_stripping
 * @adapter: adapter structure
 *
 * Request VLAN header stripping to be enabled
 **/
void iavf_enable_vlan_stripping(struct iavf_adapter *adapter)
{
	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot enable stripping, command %d pending\n",
			adapter->current_op);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_ENABLE_VLAN_STRIPPING;
	adapter->aq_required &= ~IAVF_FLAG_AQ_ENABLE_VLAN_STRIPPING;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_ENABLE_VLAN_STRIPPING, NULL, 0);
}

/**
 * iavf_disable_vlan_stripping
 * @adapter: adapter structure
 *
 * Request VLAN header stripping to be disabled
 **/
void iavf_disable_vlan_stripping(struct iavf_adapter *adapter)
{
	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot disable stripping, command %d pending\n",
			adapter->current_op);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_DISABLE_VLAN_STRIPPING;
	adapter->aq_required &= ~IAVF_FLAG_AQ_DISABLE_VLAN_STRIPPING;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_DISABLE_VLAN_STRIPPING, NULL, 0);
}

/**
 * iavf_tpid_to_vc_ethertype - transform from VLAN TPID to virtchnl ethertype
 * @tpid: VLAN TPID (i.e. 0x8100, 0x88a8, etc.)
 */
static u32 iavf_tpid_to_vc_ethertype(u16 tpid)
{
	switch (tpid) {
	case ETH_P_8021Q:
		return VIRTCHNL_VLAN_ETHERTYPE_8100;
	case ETH_P_8021AD:
		return VIRTCHNL_VLAN_ETHERTYPE_88A8;
	}

	return 0;
}

/**
 * iavf_set_vc_offload_ethertype - set virtchnl ethertype for offload message
 * @adapter: adapter structure
 * @msg: message structure used for updating offloads over virtchnl to update
 * @tpid: VLAN TPID (i.e. 0x8100, 0x88a8, etc.)
 * @offload_op: opcode used to determine which support structure to check
 */
static int
iavf_set_vc_offload_ethertype(struct iavf_adapter *adapter,
			      struct virtchnl_vlan_setting *msg, u16 tpid,
			      enum virtchnl_ops offload_op)
{
	struct virtchnl_vlan_supported_caps *offload_support;
	u16 vc_ethertype = iavf_tpid_to_vc_ethertype(tpid);

	/* reference the correct offload support structure */
	switch (offload_op) {
	case VIRTCHNL_OP_ENABLE_VLAN_STRIPPING_V2:
	case VIRTCHNL_OP_DISABLE_VLAN_STRIPPING_V2:
		offload_support =
			&adapter->vlan_v2_caps.offloads.stripping_support;
		break;
	case VIRTCHNL_OP_ENABLE_VLAN_INSERTION_V2:
	case VIRTCHNL_OP_DISABLE_VLAN_INSERTION_V2:
		offload_support =
			&adapter->vlan_v2_caps.offloads.insertion_support;
		break;
	default:
		dev_err(&adapter->pdev->dev, "Invalid opcode %d for setting virtchnl ethertype to enable/disable VLAN offloads\n",
			offload_op);
		return -EINVAL;
	}

	/* make sure ethertype is supported */
	if (offload_support->outer & vc_ethertype &&
	    offload_support->outer & VIRTCHNL_VLAN_TOGGLE) {
		msg->outer_ethertype_setting = vc_ethertype;
	} else if (offload_support->inner & vc_ethertype &&
		   offload_support->inner & VIRTCHNL_VLAN_TOGGLE) {
		msg->inner_ethertype_setting = vc_ethertype;
	} else {
		dev_dbg(&adapter->pdev->dev, "opcode %d unsupported for VLAN TPID 0x%04x\n",
			offload_op, tpid);
		return -EINVAL;
	}

	return 0;
}

/**
 * iavf_clear_offload_v2_aq_required - clear AQ required bit for offload request
 * @adapter: adapter structure
 * @tpid: VLAN TPID
 * @offload_op: opcode used to determine which AQ required bit to clear
 */
static void
iavf_clear_offload_v2_aq_required(struct iavf_adapter *adapter, u16 tpid,
				  enum virtchnl_ops offload_op)
{
	switch (offload_op) {
	case VIRTCHNL_OP_ENABLE_VLAN_STRIPPING_V2:
		if (tpid == ETH_P_8021Q)
			adapter->aq_required &=
				~IAVF_FLAG_AQ_ENABLE_CTAG_VLAN_STRIPPING;
		else if (tpid == ETH_P_8021AD)
			adapter->aq_required &=
				~IAVF_FLAG_AQ_ENABLE_STAG_VLAN_STRIPPING;
		break;
	case VIRTCHNL_OP_DISABLE_VLAN_STRIPPING_V2:
		if (tpid == ETH_P_8021Q)
			adapter->aq_required &=
				~IAVF_FLAG_AQ_DISABLE_CTAG_VLAN_STRIPPING;
		else if (tpid == ETH_P_8021AD)
			adapter->aq_required &=
				~IAVF_FLAG_AQ_DISABLE_STAG_VLAN_STRIPPING;
		break;
	case VIRTCHNL_OP_ENABLE_VLAN_INSERTION_V2:
		if (tpid == ETH_P_8021Q)
			adapter->aq_required &=
				~IAVF_FLAG_AQ_ENABLE_CTAG_VLAN_INSERTION;
		else if (tpid == ETH_P_8021AD)
			adapter->aq_required &=
				~IAVF_FLAG_AQ_ENABLE_STAG_VLAN_INSERTION;
		break;
	case VIRTCHNL_OP_DISABLE_VLAN_INSERTION_V2:
		if (tpid == ETH_P_8021Q)
			adapter->aq_required &=
				~IAVF_FLAG_AQ_DISABLE_CTAG_VLAN_INSERTION;
		else if (tpid == ETH_P_8021AD)
			adapter->aq_required &=
				~IAVF_FLAG_AQ_DISABLE_STAG_VLAN_INSERTION;
		break;
	default:
		dev_err(&adapter->pdev->dev, "Unsupported opcode %d specified for clearing aq_required bits for VIRTCHNL_VF_OFFLOAD_VLAN_V2 offload request\n",
			offload_op);
	}
}

/**
 * iavf_send_vlan_offload_v2 - send offload enable/disable over virtchnl
 * @adapter: adapter structure
 * @tpid: VLAN TPID used for the command (i.e. 0x8100 or 0x88a8)
 * @offload_op: offload_op used to make the request over virtchnl
 */
static void
iavf_send_vlan_offload_v2(struct iavf_adapter *adapter, u16 tpid,
			  enum virtchnl_ops offload_op)
{
	struct virtchnl_vlan_setting *msg;
	int len = sizeof(*msg);

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot send %d, command %d pending\n",
			offload_op, adapter->current_op);
		return;
	}

	adapter->current_op = offload_op;

	msg = kzalloc(len, GFP_KERNEL);
	if (!msg)
		return;

	msg->vport_id = adapter->vsi_res->vsi_id;

	/* always clear to prevent unsupported and endless requests */
	iavf_clear_offload_v2_aq_required(adapter, tpid, offload_op);

	/* only send valid offload requests */
	if (!iavf_set_vc_offload_ethertype(adapter, msg, tpid, offload_op))
		iavf_send_pf_msg(adapter, offload_op, (u8 *)msg, len);
	else
		adapter->current_op = VIRTCHNL_OP_UNKNOWN;

	kfree(msg);
}

/**
 * iavf_enable_vlan_stripping_v2 - enable VLAN stripping
 * @adapter: adapter structure
 * @tpid: VLAN TPID used to enable VLAN stripping
 */
void iavf_enable_vlan_stripping_v2(struct iavf_adapter *adapter, u16 tpid)
{
	iavf_send_vlan_offload_v2(adapter, tpid,
				  VIRTCHNL_OP_ENABLE_VLAN_STRIPPING_V2);
}

/**
 * iavf_disable_vlan_stripping_v2 - disable VLAN stripping
 * @adapter: adapter structure
 * @tpid: VLAN TPID used to disable VLAN stripping
 */
void iavf_disable_vlan_stripping_v2(struct iavf_adapter *adapter, u16 tpid)
{
	iavf_send_vlan_offload_v2(adapter, tpid,
				  VIRTCHNL_OP_DISABLE_VLAN_STRIPPING_V2);
}

/**
 * iavf_enable_vlan_insertion_v2 - enable VLAN insertion
 * @adapter: adapter structure
 * @tpid: VLAN TPID used to enable VLAN insertion
 */
void iavf_enable_vlan_insertion_v2(struct iavf_adapter *adapter, u16 tpid)
{
	iavf_send_vlan_offload_v2(adapter, tpid,
				  VIRTCHNL_OP_ENABLE_VLAN_INSERTION_V2);
}

/**
 * iavf_disable_vlan_insertion_v2 - disable VLAN insertion
 * @adapter: adapter structure
 * @tpid: VLAN TPID used to disable VLAN insertion
 */
void iavf_disable_vlan_insertion_v2(struct iavf_adapter *adapter, u16 tpid)
{
	iavf_send_vlan_offload_v2(adapter, tpid,
				  VIRTCHNL_OP_DISABLE_VLAN_INSERTION_V2);
}

#if IS_ENABLED(CONFIG_PTP_1588_CLOCK)
/**
 * iavf_virtchnl_send_ptp_cmd - Send one queued PTP command
 * @adapter: adapter private structure
 *
 * De-queue one PTP command request and send the command message to the PF.
 * Clear IAVF_FLAG_AQ_SEND_PTP_CMD if no more messages are left to send.
 */
void iavf_virtchnl_send_ptp_cmd(struct iavf_adapter *adapter)
{
	struct iavf_ptp_aq_cmd *cmd;
	int err;

	if (!adapter->ptp.clock) {
		/* This shouldn't be possible to hit, since no messages should
		 * be queued if PTP is not initialized.
		 */
		pci_err(adapter->pdev, "PTP is not initialized\n");
		adapter->aq_required &= ~IAVF_FLAG_AQ_SEND_PTP_CMD;
		return;
	}

	mutex_lock(&adapter->ptp.aq_cmd_lock);
	cmd = list_first_entry_or_null(&adapter->ptp.aq_cmds,
				       struct iavf_ptp_aq_cmd, list);
	if (!cmd) {
		/* no further PTP messages to send */
		adapter->aq_required &= ~IAVF_FLAG_AQ_SEND_PTP_CMD;
		goto out_unlock;
	}

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		pci_err(adapter->pdev,
			"Cannot send PTP command %d, command %d pending\n",
			cmd->v_opcode, adapter->current_op);
		goto out_unlock;
	}

	err = iavf_send_pf_msg(adapter, cmd->v_opcode, cmd->msg, cmd->msglen);
	if (!err) {
		/* Command was sent without errors, so we can remove it from
		 * the list and discard it.
		 */
		list_del(&cmd->list);
		kfree(cmd);
	} else {
		/* We failed to send the command, try again next cycle */
		pci_err(adapter->pdev, "Failed to send PTP command %d\n",
			cmd->v_opcode);
	}

	if (list_empty(&adapter->ptp.aq_cmds))
		/* no further PTP messages to send */
		adapter->aq_required &= ~IAVF_FLAG_AQ_SEND_PTP_CMD;

out_unlock:
	mutex_unlock(&adapter->ptp.aq_cmd_lock);
}
#endif /* IS_ENABLED(CONFIG_PTP_1588_CLOCK) */

/**
 * iavf_print_link_message - print link up or down
 * @adapter: adapter structure
 *
 * Log a message telling the world of our wonderous link status
 */
static void iavf_print_link_message(struct iavf_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int link_speed_mbps;
	char *speed;

	if (!adapter->link_up) {
		netdev_info(netdev, "NIC Link is Down\n");
		return;
	}

	if (ADV_LINK_SUPPORT(adapter)) {
		link_speed_mbps = adapter->link_speed_mbps;
		goto print_link_msg;
	}

	switch (adapter->link_speed) {
	case VIRTCHNL_LINK_SPEED_40GB:
		link_speed_mbps = SPEED_40000;
		break;
	case VIRTCHNL_LINK_SPEED_25GB:
		link_speed_mbps = SPEED_25000;
		break;
	case VIRTCHNL_LINK_SPEED_20GB:
		link_speed_mbps = SPEED_20000;
		break;
	case VIRTCHNL_LINK_SPEED_10GB:
		link_speed_mbps = SPEED_10000;
		break;
	case VIRTCHNL_LINK_SPEED_5GB:
		link_speed_mbps = SPEED_5000;
		break;
	case VIRTCHNL_LINK_SPEED_2_5GB:
		link_speed_mbps = SPEED_2500;
		break;
	case VIRTCHNL_LINK_SPEED_1GB:
		link_speed_mbps = SPEED_1000;
		break;
	case VIRTCHNL_LINK_SPEED_100MB:
		link_speed_mbps = SPEED_100;
		break;
	default:
		link_speed_mbps = SPEED_UNKNOWN;
		break;
	}

print_link_msg:
	if (link_speed_mbps > SPEED_1000) {
		if (link_speed_mbps == SPEED_2500) {
			speed = kasprintf(GFP_KERNEL, "%s", "2.5 Gbps");
		} else {
			/* convert to Gbps inline */
			speed = kasprintf(GFP_KERNEL, "%d Gbps",
					  link_speed_mbps / 1000);
		}
	} else if (link_speed_mbps == SPEED_UNKNOWN) {
		speed = kasprintf(GFP_KERNEL, "%s", "Unknown Mbps");
	} else {
		speed = kasprintf(GFP_KERNEL, "%d Mbps", link_speed_mbps);
	}

	netdev_info(netdev, "NIC Link is Up Speed is %s Full Duplex\n", speed);
	kfree(speed);
}

/**
 * iavf_get_vpe_link_status
 * @adapter: adapter structure
 * @vpe: virtchnl_pf_event structure
 *
 * Helper function for determining the link status
 **/
static bool
iavf_get_vpe_link_status(struct iavf_adapter *adapter,
			 struct virtchnl_pf_event *vpe)
{
	if (ADV_LINK_SUPPORT(adapter))
		return vpe->event_data.link_event_adv.link_status;
	else
		return vpe->event_data.link_event.link_status;
}

/**
 * iavf_set_adapter_link_speed_from_vpe
 * @adapter: adapter structure for which we are setting the link speed
 * @vpe: virtchnl_pf_event structure that contains the link speed we are setting
 *
 * Helper function for setting iavf_adapter link speed
 **/
static void
iavf_set_adapter_link_speed_from_vpe(struct iavf_adapter *adapter,
				     struct virtchnl_pf_event *vpe)
{
	if (ADV_LINK_SUPPORT(adapter))
		adapter->link_speed_mbps =
			vpe->event_data.link_event_adv.link_speed;
	else
		adapter->link_speed = vpe->event_data.link_event.link_speed;
}

/**
 * iavf_get_qos_caps - get qos caps support
 * @adapter: iavf adapter struct instance
 *
 * This function requests PF for Supported QoS Caps.
 */
void iavf_get_qos_caps(struct iavf_adapter *adapter)
{
	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev,
			"Cannot get qos caps, command %d pending\n",
			adapter->current_op);
		return;
	}

	adapter->current_op = VIRTCHNL_OP_GET_QOS_CAPS;
	adapter->aq_required &= ~IAVF_FLAG_AQ_GET_QOS_CAPS;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_GET_QOS_CAPS, NULL, 0);
}

/**
 * iavf_set_quanta_size - set quanta size of queue chunk
 * @adapter: iavf adapter struct instance
 * @quanta_size: quanta size in bytes
 * @queue_index: starting index of queue chunk
 * @num_queues: number of queues in the queue chunk
 *
 * This function requests PF to set quanta size of queue chunk
 * starting at queue_index.
 */
static void
iavf_set_quanta_size(struct iavf_adapter *adapter, u16 quanta_size,
		     u16 queue_index, u16 num_queues)
{
	struct virtchnl_quanta_cfg quanta_cfg;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev,
			"Cannot set queue quanta size, command %d pending\n",
			adapter->current_op);
		return;
	}

	adapter->current_op = VIRTCHNL_OP_CONFIG_QUANTA;
	quanta_cfg.quanta_size = quanta_size;
	quanta_cfg.queue_select.type = VIRTCHNL_QUEUE_TYPE_TX;
	quanta_cfg.queue_select.start_queue_id = queue_index;
	quanta_cfg.queue_select.num_queues = num_queues;
	adapter->aq_required &= ~IAVF_FLAG_AQ_CFG_QUEUES_QUANTA_SIZE;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_CONFIG_QUANTA,
			 (u8 *)&quanta_cfg, sizeof(quanta_cfg));
}

/**
 * iavf_cfg_queues_quanta_size - configure quanta size of queues
 * @adapter: adapter structure
 *
 * Request that the PF configure quanta size of allocated queues.
 **/
void iavf_cfg_queues_quanta_size(struct iavf_adapter *adapter)
{
	int quanta_size = IAVF_DEFAULT_QUANTA_SIZE;

	/* Set Queue Quanta Size to default */
	iavf_set_quanta_size(adapter, quanta_size, 0,
			     adapter->num_active_queues);
}

/**
 * iavf_cfg_queues_bw - configure bandwidth of allocated queues
 * @adapter: iavf adapter structure instance
 *
 * This function requests PF to configure queue bandwidth of allocated queues
 */
void iavf_cfg_queues_bw(struct iavf_adapter *adapter)
{
	struct virtchnl_queues_bw_cfg *qs_bw_cfg;
	struct net_shaper *q_shaper;
	int qs_to_update = 0;
	int i, inx = 0;
	size_t len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev,
			"Cannot set tc queue bw, command %d pending\n",
			adapter->current_op);
		return;
	}

	for (i = 0; i < adapter->num_active_queues; i++) {
		if (adapter->tx_rings[i].q_shaper_update)
			qs_to_update++;
	}
	len = struct_size(qs_bw_cfg, cfg, qs_to_update);
	qs_bw_cfg = kzalloc(len, GFP_KERNEL);
	if (!qs_bw_cfg)
		return;

	qs_bw_cfg->vsi_id = adapter->vsi.id;
	qs_bw_cfg->num_queues = qs_to_update;

	for (i = 0; i < adapter->num_active_queues; i++) {
		struct iavf_ring *tx_ring = &adapter->tx_rings[i];

		q_shaper = &tx_ring->q_shaper;
		if (tx_ring->q_shaper_update) {
			qs_bw_cfg->cfg[inx].queue_id = i;
			qs_bw_cfg->cfg[inx].shaper.peak = q_shaper->bw_max;
			qs_bw_cfg->cfg[inx].shaper.committed = q_shaper->bw_min;
			qs_bw_cfg->cfg[inx].tc = 0;
			inx++;
		}
	}

	adapter->current_op = VIRTCHNL_OP_CONFIG_QUEUE_BW;
	adapter->aq_required &= ~IAVF_FLAG_AQ_CONFIGURE_QUEUES_BW;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_CONFIG_QUEUE_BW,
			 (u8 *)qs_bw_cfg, len);
	kfree(qs_bw_cfg);
}

/**
 * iavf_enable_channels
 * @adapter: adapter structure
 *
 * Request that the PF enable channels as specified by
 * the user via tc tool.
 **/
void iavf_enable_channels(struct iavf_adapter *adapter)
{
	struct virtchnl_tc_info *vti = NULL;
	size_t len;
	int i;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot configure mqprio, command %d pending\n",
			adapter->current_op);
		return;
	}

	len = virtchnl_struct_size(vti, list, adapter->num_tc);
	vti = kzalloc(len, GFP_KERNEL);
	if (!vti)
		return;
	vti->num_tc = adapter->num_tc;
	for (i = 0; i < vti->num_tc; i++) {
		vti->list[i].count = adapter->ch_config.ch_info[i].count;
		vti->list[i].offset = adapter->ch_config.ch_info[i].offset;
		vti->list[i].pad = 0;
		vti->list[i].max_tx_rate =
				adapter->ch_config.ch_info[i].max_tx_rate;
	}

	adapter->ch_config.state = __IAVF_TC_RUNNING;
	adapter->flags |= IAVF_FLAG_REINIT_ITR_NEEDED;
	adapter->current_op = VIRTCHNL_OP_ENABLE_CHANNELS;
	adapter->aq_required &= ~IAVF_FLAG_AQ_ENABLE_CHANNELS;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_ENABLE_CHANNELS, (u8 *)vti, len);
	kfree(vti);
}

/**
 * iavf_disable_channels
 * @adapter: adapter structure
 *
 * Request that the PF disable channels that are configured
 **/
void iavf_disable_channels(struct iavf_adapter *adapter)
{
	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot configure mqprio, command %d pending\n",
			adapter->current_op);
		return;
	}

	adapter->ch_config.state = __IAVF_TC_INVALID;
	adapter->flags |= IAVF_FLAG_REINIT_ITR_NEEDED;
	adapter->current_op = VIRTCHNL_OP_DISABLE_CHANNELS;
	adapter->aq_required &= ~IAVF_FLAG_AQ_DISABLE_CHANNELS;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_DISABLE_CHANNELS, NULL, 0);
}

/**
 * iavf_print_cloud_filter
 * @adapter: adapter structure
 * @f: cloud filter to print
 *
 * Print the cloud filter
 **/
static void iavf_print_cloud_filter(struct iavf_adapter *adapter,
				    struct virtchnl_filter *f)
{
	switch (f->flow_type) {
	case VIRTCHNL_TCP_V4_FLOW:
		dev_info(&adapter->pdev->dev, "dst_mac: %pM src_mac: %pM vlan_id: %hu dst_ip: %pI4 src_ip %pI4 dst_port %hu src_port %hu\n",
			 &f->data.tcp_spec.dst_mac,
			 &f->data.tcp_spec.src_mac,
			 ntohs(f->data.tcp_spec.vlan_id),
			 &f->data.tcp_spec.dst_ip[0],
			 &f->data.tcp_spec.src_ip[0],
			 ntohs(f->data.tcp_spec.dst_port),
			 ntohs(f->data.tcp_spec.src_port));
		break;
	case VIRTCHNL_TCP_V6_FLOW:
		dev_info(&adapter->pdev->dev, "dst_mac: %pM src_mac: %pM vlan_id: %hu dst_ip: %pI6 src_ip %pI6 dst_port %hu src_port %hu\n",
			 &f->data.tcp_spec.dst_mac,
			 &f->data.tcp_spec.src_mac,
			 ntohs(f->data.tcp_spec.vlan_id),
			 &f->data.tcp_spec.dst_ip,
			 &f->data.tcp_spec.src_ip,
			 ntohs(f->data.tcp_spec.dst_port),
			 ntohs(f->data.tcp_spec.src_port));
		break;
	}
}

/**
 * iavf_add_cloud_filter
 * @adapter: adapter structure
 *
 * Request that the PF add cloud filters as specified
 * by the user via tc tool.
 **/
void iavf_add_cloud_filter(struct iavf_adapter *adapter)
{
	struct iavf_cloud_filter *cf;
	struct virtchnl_filter *f;
	int len = 0, count = 0;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot add cloud filter, command %d pending\n",
			adapter->current_op);
		return;
	}
	list_for_each_entry(cf, &adapter->cloud_filter_list, list) {
		if (cf->add) {
			count++;
			break;
		}
	}
	if (!count) {
		adapter->aq_required &= ~IAVF_FLAG_AQ_ADD_CLOUD_FILTER;
		return;
	}
	adapter->current_op = VIRTCHNL_OP_ADD_CLOUD_FILTER;

	len = sizeof(struct virtchnl_filter);
	f = kzalloc(len, GFP_KERNEL);
	if (!f)
		return;

	list_for_each_entry(cf, &adapter->cloud_filter_list, list) {
		if (cf->add) {
			memcpy(f, &cf->f, sizeof(struct virtchnl_filter));
			cf->add = false;
			cf->state = __IAVF_CF_ADD_PENDING;
			iavf_send_pf_msg(adapter, VIRTCHNL_OP_ADD_CLOUD_FILTER,
					 (u8 *)f, len);
		}
	}
	kfree(f);
}

/**
 * iavf_del_cloud_filter
 * @adapter: adapter structure
 *
 * Request that the PF delete cloud filters as specified
 * by the user via tc tool.
 **/
void iavf_del_cloud_filter(struct iavf_adapter *adapter)
{
	struct iavf_cloud_filter *cf, *cftmp;
	struct virtchnl_filter *f;
	int len = 0, count = 0;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot remove cloud filter, command %d pending\n",
			adapter->current_op);
		return;
	}
	list_for_each_entry(cf, &adapter->cloud_filter_list, list) {
		if (cf->del) {
			count++;
			break;
		}
	}
	if (!count) {
		adapter->aq_required &= ~IAVF_FLAG_AQ_DEL_CLOUD_FILTER;
		return;
	}
	adapter->current_op = VIRTCHNL_OP_DEL_CLOUD_FILTER;

	len = sizeof(struct virtchnl_filter);
	f = kzalloc(len, GFP_KERNEL);
	if (!f)
		return;

	list_for_each_entry_safe(cf, cftmp, &adapter->cloud_filter_list, list) {
		if (cf->del) {
			memcpy(f, &cf->f, sizeof(struct virtchnl_filter));
			cf->del = false;
			cf->state = __IAVF_CF_DEL_PENDING;
			iavf_send_pf_msg(adapter, VIRTCHNL_OP_DEL_CLOUD_FILTER,
					 (u8 *)f, len);
		}
	}
	kfree(f);
}

/**
 * iavf_add_fdir_filter
 * @adapter: the VF adapter structure
 *
 * Request that the PF add Flow Director filters as specified
 * by the user via ethtool.
 **/
void iavf_add_fdir_filter(struct iavf_adapter *adapter)
{
	struct iavf_fdir_fltr *fdir;
	struct virtchnl_fdir_add *f;
	bool process_fltr = false;
	int len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot add Flow Director filter, command %d pending\n",
			adapter->current_op);
		return;
	}

	len = sizeof(struct virtchnl_fdir_add);
	f = kzalloc(len, GFP_KERNEL);
	if (!f)
		return;

	spin_lock_bh(&adapter->fdir_fltr_lock);
	list_for_each_entry(fdir, &adapter->fdir_list_head, list) {
		if (fdir->state == IAVF_FDIR_FLTR_ADD_REQUEST) {
			process_fltr = true;
			fdir->state = IAVF_FDIR_FLTR_ADD_PENDING;
			memcpy(f, &fdir->vc_add_msg, len);
			break;
		}
	}
	spin_unlock_bh(&adapter->fdir_fltr_lock);

	if (!process_fltr) {
		/* prevent iavf_add_fdir_filter() from being called when there
		 * are no filters to add
		 */
		adapter->aq_required &= ~IAVF_FLAG_AQ_ADD_FDIR_FILTER;
		kfree(f);
		return;
	}
	adapter->current_op = VIRTCHNL_OP_ADD_FDIR_FILTER;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_ADD_FDIR_FILTER, (u8 *)f, len);
	kfree(f);
}

/**
 * iavf_del_fdir_filter
 * @adapter: the VF adapter structure
 *
 * Request that the PF delete Flow Director filters as specified
 * by the user via ethtool.
 **/
void iavf_del_fdir_filter(struct iavf_adapter *adapter)
{
	struct virtchnl_fdir_del f = {};
	struct iavf_fdir_fltr *fdir;
	bool process_fltr = false;
	int len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot remove Flow Director filter, command %d pending\n",
			adapter->current_op);
		return;
	}

	len = sizeof(struct virtchnl_fdir_del);

	spin_lock_bh(&adapter->fdir_fltr_lock);
	list_for_each_entry(fdir, &adapter->fdir_list_head, list) {
		if (fdir->state == IAVF_FDIR_FLTR_DEL_REQUEST) {
			process_fltr = true;
			f.vsi_id = fdir->vc_add_msg.vsi_id;
			f.flow_id = fdir->flow_id;
			fdir->state = IAVF_FDIR_FLTR_DEL_PENDING;
			break;
		} else if (fdir->state == IAVF_FDIR_FLTR_DIS_REQUEST) {
			process_fltr = true;
			f.vsi_id = fdir->vc_add_msg.vsi_id;
			f.flow_id = fdir->flow_id;
			fdir->state = IAVF_FDIR_FLTR_DIS_PENDING;
			break;
		}
	}
	spin_unlock_bh(&adapter->fdir_fltr_lock);

	if (!process_fltr) {
		adapter->aq_required &= ~IAVF_FLAG_AQ_DEL_FDIR_FILTER;
		return;
	}

	adapter->current_op = VIRTCHNL_OP_DEL_FDIR_FILTER;
	iavf_send_pf_msg(adapter, VIRTCHNL_OP_DEL_FDIR_FILTER, (u8 *)&f, len);
}

/**
 * iavf_add_adv_rss_cfg
 * @adapter: the VF adapter structure
 *
 * Request that the PF add RSS configuration as specified
 * by the user via ethtool.
 **/
void iavf_add_adv_rss_cfg(struct iavf_adapter *adapter)
{
	struct virtchnl_rss_cfg *rss_cfg;
	struct iavf_adv_rss *rss;
	bool process_rss = false;
	int len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot add RSS configuration, command %d pending\n",
			adapter->current_op);
		return;
	}

	len = sizeof(struct virtchnl_rss_cfg);
	rss_cfg = kzalloc(len, GFP_KERNEL);
	if (!rss_cfg)
		return;

	spin_lock_bh(&adapter->adv_rss_lock);
	list_for_each_entry(rss, &adapter->adv_rss_list_head, list) {
		if (rss->state == IAVF_ADV_RSS_ADD_REQUEST) {
			process_rss = true;
			rss->state = IAVF_ADV_RSS_ADD_PENDING;
			memcpy(rss_cfg, &rss->cfg_msg, len);
			iavf_print_adv_rss_cfg(adapter, rss,
					       "Input set change for",
					       "is pending");
			break;
		}
	}
	spin_unlock_bh(&adapter->adv_rss_lock);

	if (process_rss) {
		adapter->current_op = VIRTCHNL_OP_ADD_RSS_CFG;
		iavf_send_pf_msg(adapter, VIRTCHNL_OP_ADD_RSS_CFG,
				 (u8 *)rss_cfg, len);
	} else {
		adapter->aq_required &= ~IAVF_FLAG_AQ_ADD_ADV_RSS_CFG;
	}

	kfree(rss_cfg);
}

/**
 * iavf_del_adv_rss_cfg
 * @adapter: the VF adapter structure
 *
 * Request that the PF delete RSS configuration as specified
 * by the user via ethtool.
 **/
void iavf_del_adv_rss_cfg(struct iavf_adapter *adapter)
{
	struct virtchnl_rss_cfg *rss_cfg;
	struct iavf_adv_rss *rss;
	bool process_rss = false;
	int len;

	if (adapter->current_op != VIRTCHNL_OP_UNKNOWN) {
		/* bail because we already have a command pending */
		dev_err(&adapter->pdev->dev, "Cannot remove RSS configuration, command %d pending\n",
			adapter->current_op);
		return;
	}

	len = sizeof(struct virtchnl_rss_cfg);
	rss_cfg = kzalloc(len, GFP_KERNEL);
	if (!rss_cfg)
		return;

	spin_lock_bh(&adapter->adv_rss_lock);
	list_for_each_entry(rss, &adapter->adv_rss_list_head, list) {
		if (rss->state == IAVF_ADV_RSS_DEL_REQUEST) {
			process_rss = true;
			rss->state = IAVF_ADV_RSS_DEL_PENDING;
			memcpy(rss_cfg, &rss->cfg_msg, len);
			break;
		}
	}
	spin_unlock_bh(&adapter->adv_rss_lock);

	if (process_rss) {
		adapter->current_op = VIRTCHNL_OP_DEL_RSS_CFG;
		iavf_send_pf_msg(adapter, VIRTCHNL_OP_DEL_RSS_CFG,
				 (u8 *)rss_cfg, len);
	} else {
		adapter->aq_required &= ~IAVF_FLAG_AQ_DEL_ADV_RSS_CFG;
	}

	kfree(rss_cfg);
}

/**
 * iavf_request_reset
 * @adapter: adapter structure
 *
 * Request that the PF reset this VF. No response is expected.
 **/
int iavf_request_reset(struct iavf_adapter *adapter)
{
	int err;
	/* Don't check CURRENT_OP - this is always higher priority */
	err = iavf_send_pf_msg(adapter, VIRTCHNL_OP_RESET_VF, NULL, 0);
	adapter->current_op = VIRTCHNL_OP_UNKNOWN;
	return err;
}

/**
 * iavf_netdev_features_vlan_strip_set - update vlan strip status
 * @netdev: ptr to netdev being adjusted
 * @enable: enable or disable vlan strip
 *
 * Helper function to change vlan strip status in netdev->features.
 */
static void iavf_netdev_features_vlan_strip_set(struct net_device *netdev,
						const bool enable)
{
	if (enable)
		netdev->features |= NETIF_F_HW_VLAN_CTAG_RX;
	else
		netdev->features &= ~NETIF_F_HW_VLAN_CTAG_RX;
}

/**
 * iavf_activate_fdir_filters - Reactivate all FDIR filters after a reset
 * @adapter: private adapter structure
 *
 * Called after a reset to re-add all FDIR filters and delete some of them
 * if they were pending to be deleted.
 */
static void iavf_activate_fdir_filters(struct iavf_adapter *adapter)
{
	struct iavf_fdir_fltr *f, *ftmp;
	bool add_filters = false;

	spin_lock_bh(&adapter->fdir_fltr_lock);
	list_for_each_entry_safe(f, ftmp, &adapter->fdir_list_head, list) {
		if (f->state == IAVF_FDIR_FLTR_ADD_REQUEST ||
		    f->state == IAVF_FDIR_FLTR_ADD_PENDING ||
		    f->state == IAVF_FDIR_FLTR_ACTIVE) {
			/* All filters and requests have been removed in PF,
			 * restore them
			 */
			f->state = IAVF_FDIR_FLTR_ADD_REQUEST;
			add_filters = true;
		} else if (f->state == IAVF_FDIR_FLTR_DIS_REQUEST ||
			   f->state == IAVF_FDIR_FLTR_DIS_PENDING) {
			/* Link down state, leave filters as inactive */
			f->state = IAVF_FDIR_FLTR_INACTIVE;
		} else if (f->state == IAVF_FDIR_FLTR_DEL_REQUEST ||
			   f->state == IAVF_FDIR_FLTR_DEL_PENDING) {
			/* Delete filters that were pending to be deleted, the
			 * list on PF is already cleared after a reset
			 */
			list_del(&f->list);
			iavf_dec_fdir_active_fltr(adapter, f);
			kfree(f);
		}
	}
	spin_unlock_bh(&adapter->fdir_fltr_lock);

	if (add_filters)
		adapter->aq_required |= IAVF_FLAG_AQ_ADD_FDIR_FILTER;
}

/**
 * iavf_virtchnl_ptp_get_time - Respond to VIRTCHNL_OP_1588_PTP_GET_TIME
 * @adapter: private adapter structure
 * @data: the message from the PF
 * @len: length of the message from the PF
 *
 * Handle the VIRTCHNL_OP_1588_PTP_GET_TIME message from the PF. This message
 * is sent by the PF in response to the same op as a request from the VF.
 * Extract the 64bit nanoseconds time from the message and store it in
 * cached_phc_time. Then, notify any thread that is waiting for the update via
 * the wait queue.
 */
static void iavf_virtchnl_ptp_get_time(struct iavf_adapter *adapter,
				       void *data, u16 len)
{
	struct virtchnl_phc_time *msg = data;

	if (len != sizeof(*msg)) {
		dev_err_once(&adapter->pdev->dev,
			     "Invalid VIRTCHNL_OP_1588_PTP_GET_TIME from PF. Got size %u, expected %zu\n",
			     len, sizeof(*msg));
		return;
	}

	adapter->ptp.cached_phc_time = msg->time;
	adapter->ptp.cached_phc_updated = jiffies;
	adapter->ptp.phc_time_ready = true;

	wake_up(&adapter->ptp.phc_time_waitqueue);
}

/**
 * iavf_virtchnl_completion
 * @adapter: adapter structure
 * @v_opcode: opcode sent by PF
 * @v_retval: retval sent by PF
 * @msg: message sent by PF
 * @msglen: message length
 *
 * Asynchronous completion function for admin queue messages. Rather than busy
 * wait, we fire off our requests and assume that no errors will be returned.
 * This function handles the reply messages.
 **/
void iavf_virtchnl_completion(struct iavf_adapter *adapter,
			      enum virtchnl_ops v_opcode,
			      enum iavf_status v_retval, u8 *msg, u16 msglen)
{
	struct net_device *netdev = adapter->netdev;

	if (v_opcode == VIRTCHNL_OP_EVENT) {
		struct virtchnl_pf_event *vpe =
			(struct virtchnl_pf_event *)msg;
		bool link_up = iavf_get_vpe_link_status(adapter, vpe);

		switch (vpe->event) {
		case VIRTCHNL_EVENT_LINK_CHANGE:
			iavf_set_adapter_link_speed_from_vpe(adapter, vpe);

			/* we've already got the right link status, bail */
			if (adapter->link_up == link_up)
				break;

			if (link_up) {
				/* If we get link up message and start queues
				 * before our queues are configured it will
				 * trigger a TX hang. In that case, just ignore
				 * the link status message,we'll get another one
				 * after we enable queues and actually prepared
				 * to send traffic.
				 */
				if (adapter->state != __IAVF_RUNNING)
					break;

				/* For ADq enabled VF, we reconfigure VSIs and
				 * re-allocate queues. Hence wait till all
				 * queues are enabled.
				 */
				if (adapter->flags &
				    IAVF_FLAG_QUEUES_DISABLED)
					break;
			}

			adapter->link_up = link_up;
			if (link_up) {
				netif_tx_start_all_queues(netdev);
				netif_carrier_on(netdev);
			} else {
				netif_tx_stop_all_queues(netdev);
				netif_carrier_off(netdev);
			}
			iavf_print_link_message(adapter);
			break;
		case VIRTCHNL_EVENT_RESET_IMPENDING:
			dev_info(&adapter->pdev->dev, "Reset indication received from the PF\n");
			if (!(adapter->flags & IAVF_FLAG_RESET_PENDING)) {
				dev_info(&adapter->pdev->dev, "Scheduling reset task\n");
				iavf_schedule_reset(adapter, IAVF_FLAG_RESET_PENDING);
			}
			break;
		default:
			dev_err(&adapter->pdev->dev, "Unknown event %d from PF\n",
				vpe->event);
			break;
		}
		return;
	}
	if (v_retval) {
		switch (v_opcode) {
		case VIRTCHNL_OP_ADD_VLAN:
			dev_err(&adapter->pdev->dev, "Failed to add VLAN filter, error %s\n",
				iavf_stat_str(&adapter->hw, v_retval));
			break;
		case VIRTCHNL_OP_ADD_ETH_ADDR:
			dev_err(&adapter->pdev->dev, "Failed to add MAC filter, error %s\n",
				iavf_stat_str(&adapter->hw, v_retval));
			iavf_mac_add_reject(adapter);
			/* restore administratively set MAC address */
			ether_addr_copy(adapter->hw.mac.addr, netdev->dev_addr);
			wake_up(&adapter->vc_waitqueue);
			break;
		case VIRTCHNL_OP_DEL_VLAN:
			dev_err(&adapter->pdev->dev, "Failed to delete VLAN filter, error %s\n",
				iavf_stat_str(&adapter->hw, v_retval));
			break;
		case VIRTCHNL_OP_DEL_ETH_ADDR:
			dev_err(&adapter->pdev->dev, "Failed to delete MAC filter, error %s\n",
				iavf_stat_str(&adapter->hw, v_retval));
			break;
		case VIRTCHNL_OP_ENABLE_CHANNELS:
			dev_err(&adapter->pdev->dev, "Failed to configure queue channels, error %s\n",
				iavf_stat_str(&adapter->hw, v_retval));
			adapter->flags &= ~IAVF_FLAG_REINIT_ITR_NEEDED;
			adapter->ch_config.state = __IAVF_TC_INVALID;
			netdev_reset_tc(netdev);
			netif_tx_start_all_queues(netdev);
			break;
		case VIRTCHNL_OP_DISABLE_CHANNELS:
			dev_err(&adapter->pdev->dev, "Failed to disable queue channels, error %s\n",
				iavf_stat_str(&adapter->hw, v_retval));
			adapter->flags &= ~IAVF_FLAG_REINIT_ITR_NEEDED;
			adapter->ch_config.state = __IAVF_TC_RUNNING;
			netif_tx_start_all_queues(netdev);
			break;
		case VIRTCHNL_OP_ADD_CLOUD_FILTER: {
			struct iavf_cloud_filter *cf, *cftmp;

			list_for_each_entry_safe(cf, cftmp,
						 &adapter->cloud_filter_list,
						 list) {
				if (cf->state == __IAVF_CF_ADD_PENDING) {
					cf->state = __IAVF_CF_INVALID;
					dev_info(&adapter->pdev->dev, "Failed to add cloud filter, error %s\n",
						 iavf_stat_str(&adapter->hw,
							       v_retval));
					iavf_print_cloud_filter(adapter,
								&cf->f);
					list_del(&cf->list);
					kfree(cf);
					adapter->num_cloud_filters--;
				}
			}
			}
			break;
		case VIRTCHNL_OP_DEL_CLOUD_FILTER: {
			struct iavf_cloud_filter *cf;

			list_for_each_entry(cf, &adapter->cloud_filter_list,
					    list) {
				if (cf->state == __IAVF_CF_DEL_PENDING) {
					cf->state = __IAVF_CF_ACTIVE;
					dev_info(&adapter->pdev->dev, "Failed to del cloud filter, error %s\n",
						 iavf_stat_str(&adapter->hw,
							       v_retval));
					iavf_print_cloud_filter(adapter,
								&cf->f);
				}
			}
			}
			break;
		case VIRTCHNL_OP_ADD_FDIR_FILTER: {
			struct iavf_fdir_fltr *fdir, *fdir_tmp;

			spin_lock_bh(&adapter->fdir_fltr_lock);
			list_for_each_entry_safe(fdir, fdir_tmp,
						 &adapter->fdir_list_head,
						 list) {
				if (fdir->state == IAVF_FDIR_FLTR_ADD_PENDING) {
					dev_info(&adapter->pdev->dev, "Failed to add Flow Director filter, error %s\n",
						 iavf_stat_str(&adapter->hw,
							       v_retval));
					iavf_print_fdir_fltr(adapter, fdir);
					if (msglen)
						dev_err(&adapter->pdev->dev,
							"%s\n", msg);
					list_del(&fdir->list);
					iavf_dec_fdir_active_fltr(adapter, fdir);
					kfree(fdir);
				}
			}
			spin_unlock_bh(&adapter->fdir_fltr_lock);
			}
			break;
		case VIRTCHNL_OP_DEL_FDIR_FILTER: {
			struct iavf_fdir_fltr *fdir;

			spin_lock_bh(&adapter->fdir_fltr_lock);
			list_for_each_entry(fdir, &adapter->fdir_list_head,
					    list) {
				if (fdir->state == IAVF_FDIR_FLTR_DEL_PENDING ||
				    fdir->state == IAVF_FDIR_FLTR_DIS_PENDING) {
					fdir->state = IAVF_FDIR_FLTR_ACTIVE;
					dev_info(&adapter->pdev->dev, "Failed to del Flow Director filter, error %s\n",
						 iavf_stat_str(&adapter->hw,
							       v_retval));
					iavf_print_fdir_fltr(adapter, fdir);
				}
			}
			spin_unlock_bh(&adapter->fdir_fltr_lock);
			}
			break;
		case VIRTCHNL_OP_ADD_RSS_CFG: {
			struct iavf_adv_rss *rss, *rss_tmp;

			spin_lock_bh(&adapter->adv_rss_lock);
			list_for_each_entry_safe(rss, rss_tmp,
						 &adapter->adv_rss_list_head,
						 list) {
				if (rss->state == IAVF_ADV_RSS_ADD_PENDING) {
					iavf_print_adv_rss_cfg(adapter, rss,
							       "Failed to change the input set for",
							       NULL);
					list_del(&rss->list);
					kfree(rss);
				}
			}
			spin_unlock_bh(&adapter->adv_rss_lock);
			}
			break;
		case VIRTCHNL_OP_DEL_RSS_CFG: {
			struct iavf_adv_rss *rss;

			spin_lock_bh(&adapter->adv_rss_lock);
			list_for_each_entry(rss, &adapter->adv_rss_list_head,
					    list) {
				if (rss->state == IAVF_ADV_RSS_DEL_PENDING) {
					rss->state = IAVF_ADV_RSS_ACTIVE;
					dev_err(&adapter->pdev->dev, "Failed to delete RSS configuration, error %s\n",
						iavf_stat_str(&adapter->hw,
							      v_retval));
				}
			}
			spin_unlock_bh(&adapter->adv_rss_lock);
			}
			break;
		case VIRTCHNL_OP_ENABLE_VLAN_STRIPPING:
			dev_warn(&adapter->pdev->dev, "Changing VLAN Stripping is not allowed when Port VLAN is configured\n");
			/* Vlan stripping could not be enabled by ethtool.
			 * Disable it in netdev->features.
			 */
			iavf_netdev_features_vlan_strip_set(netdev, false);
			break;
		case VIRTCHNL_OP_DISABLE_VLAN_STRIPPING:
			dev_warn(&adapter->pdev->dev, "Changing VLAN Stripping is not allowed when Port VLAN is configured\n");
			/* Vlan stripping could not be disabled by ethtool.
			 * Enable it in netdev->features.
			 */
			iavf_netdev_features_vlan_strip_set(netdev, true);
			break;
		case VIRTCHNL_OP_ADD_VLAN_V2:
			iavf_vlan_add_reject(adapter);
			dev_warn(&adapter->pdev->dev, "Failed to add VLAN filter, error %s\n",
				 iavf_stat_str(&adapter->hw, v_retval));
			break;
		case VIRTCHNL_OP_CONFIG_RSS_HFUNC:
			dev_warn(&adapter->pdev->dev, "Failed to configure hash function, error %s\n",
				 iavf_stat_str(&adapter->hw, v_retval));

			if (adapter->hfunc ==
					VIRTCHNL_RSS_ALG_TOEPLITZ_SYMMETRIC)
				adapter->hfunc =
					VIRTCHNL_RSS_ALG_TOEPLITZ_ASYMMETRIC;
			else
				adapter->hfunc =
					VIRTCHNL_RSS_ALG_TOEPLITZ_SYMMETRIC;

			break;
		case VIRTCHNL_OP_GET_QOS_CAPS:
			dev_warn(&adapter->pdev->dev, "Failed to Get Qos CAPs, error %s\n",
				 iavf_stat_str(&adapter->hw, v_retval));
			break;
		case VIRTCHNL_OP_CONFIG_QUANTA:
			dev_warn(&adapter->pdev->dev, "Failed to Config Quanta, error %s\n",
				 iavf_stat_str(&adapter->hw, v_retval));
			break;
		case VIRTCHNL_OP_CONFIG_QUEUE_BW:
			dev_warn(&adapter->pdev->dev, "Failed to Config Queue BW, error %s\n",
				 iavf_stat_str(&adapter->hw, v_retval));
			break;
		default:
			dev_err(&adapter->pdev->dev, "PF returned error %d (%s) to our request %d\n",
				v_retval, iavf_stat_str(&adapter->hw, v_retval),
				v_opcode);
		}
	}
	switch (v_opcode) {
	case VIRTCHNL_OP_ADD_ETH_ADDR:
		if (!v_retval)
			iavf_mac_add_ok(adapter);
		if (!ether_addr_equal(netdev->dev_addr, adapter->hw.mac.addr))
			if (!ether_addr_equal(netdev->dev_addr,
					      adapter->hw.mac.addr)) {
				netif_addr_lock_bh(netdev);
				eth_hw_addr_set(netdev, adapter->hw.mac.addr);
				netif_addr_unlock_bh(netdev);
			}
		wake_up(&adapter->vc_waitqueue);
		break;
	case VIRTCHNL_OP_GET_STATS: {
		struct iavf_eth_stats *stats =
			(struct iavf_eth_stats *)msg;
		netdev->stats.rx_packets = stats->rx_unicast +
					   stats->rx_multicast +
					   stats->rx_broadcast;
		netdev->stats.tx_packets = stats->tx_unicast +
					   stats->tx_multicast +
					   stats->tx_broadcast;
		netdev->stats.rx_bytes = stats->rx_bytes;
		netdev->stats.tx_bytes = stats->tx_bytes;
		netdev->stats.tx_errors = stats->tx_errors;
		netdev->stats.rx_dropped = stats->rx_discards;
		netdev->stats.tx_dropped = stats->tx_discards;
		adapter->current_stats = *stats;
		}
		break;
	case VIRTCHNL_OP_GET_VF_RESOURCES: {
		u16 len = IAVF_VIRTCHNL_VF_RESOURCE_SIZE;

		memcpy(adapter->vf_res, msg, min(msglen, len));
		iavf_validate_num_queues(adapter);
		iavf_vf_parse_hw_config(&adapter->hw, adapter->vf_res);
		if (is_zero_ether_addr(adapter->hw.mac.addr)) {
			/* restore current mac address */
			ether_addr_copy(adapter->hw.mac.addr, netdev->dev_addr);
		} else {
			netif_addr_lock_bh(netdev);
			/* refresh current mac address if changed */
			ether_addr_copy(netdev->perm_addr,
					adapter->hw.mac.addr);
			netif_addr_unlock_bh(netdev);
		}
		spin_lock_bh(&adapter->mac_vlan_list_lock);
		iavf_add_filter(adapter, adapter->hw.mac.addr);

		if (VLAN_ALLOWED(adapter)) {
			if (!list_empty(&adapter->vlan_filter_list)) {
				struct iavf_vlan_filter *vlf;

				/* re-add all VLAN filters over virtchnl */
				list_for_each_entry(vlf,
						    &adapter->vlan_filter_list,
						    list)
					vlf->state = IAVF_VLAN_ADD;

				adapter->aq_required |=
					IAVF_FLAG_AQ_ADD_VLAN_FILTER;
			}
		}

		spin_unlock_bh(&adapter->mac_vlan_list_lock);

		iavf_activate_fdir_filters(adapter);

		iavf_parse_vf_resource_msg(adapter);

		/* negotiated VIRTCHNL_VF_OFFLOAD_VLAN_V2, so wait for the
		 * response to VIRTCHNL_OP_GET_OFFLOAD_VLAN_V2_CAPS to finish
		 * configuration
		 */
		if (VLAN_V2_ALLOWED(adapter))
			break;
		/* fallthrough and finish config if VIRTCHNL_VF_OFFLOAD_VLAN_V2
		 * wasn't successfully negotiated with the PF
		 */
		}
		fallthrough;
	case VIRTCHNL_OP_GET_OFFLOAD_VLAN_V2_CAPS: {
		struct iavf_mac_filter *f;
		bool was_mac_changed;
		u64 aq_required = 0;

		if (v_opcode == VIRTCHNL_OP_GET_OFFLOAD_VLAN_V2_CAPS)
			memcpy(&adapter->vlan_v2_caps, msg,
			       min_t(u16, msglen,
				     sizeof(adapter->vlan_v2_caps)));

		iavf_process_config(adapter);
		adapter->flags |= IAVF_FLAG_SETUP_NETDEV_FEATURES;
		iavf_schedule_finish_config(adapter);

		iavf_set_queue_vlan_tag_loc(adapter);

		was_mac_changed = !ether_addr_equal(netdev->dev_addr,
						    adapter->hw.mac.addr);

		spin_lock_bh(&adapter->mac_vlan_list_lock);

		/* re-add all MAC filters */
		list_for_each_entry(f, &adapter->mac_filter_list, list) {
			if (was_mac_changed &&
			    ether_addr_equal(netdev->dev_addr, f->macaddr))
				ether_addr_copy(f->macaddr,
						adapter->hw.mac.addr);

			f->is_new_mac = true;
			f->add = true;
			f->add_handled = false;
			f->remove = false;
		}

		/* re-add all VLAN filters */
		if (VLAN_FILTERING_ALLOWED(adapter)) {
			struct iavf_vlan_filter *vlf;

			if (!list_empty(&adapter->vlan_filter_list)) {
				list_for_each_entry(vlf,
						    &adapter->vlan_filter_list,
						    list)
					vlf->state = IAVF_VLAN_ADD;

				aq_required |= IAVF_FLAG_AQ_ADD_VLAN_FILTER;
			}
		}

		spin_unlock_bh(&adapter->mac_vlan_list_lock);

		netif_addr_lock_bh(netdev);
		eth_hw_addr_set(netdev, adapter->hw.mac.addr);
		netif_addr_unlock_bh(netdev);

		adapter->aq_required |= IAVF_FLAG_AQ_ADD_MAC_FILTER |
			aq_required;
		}
		break;
	case VIRTCHNL_OP_GET_SUPPORTED_RXDIDS:
		if (msglen != sizeof(u64))
			return;

		adapter->supp_rxdids = *(u64 *)msg;

		break;
	case VIRTCHNL_OP_1588_PTP_GET_CAPS:
		if (msglen != sizeof(adapter->ptp.hw_caps))
			return;

		adapter->ptp.hw_caps = *(struct virtchnl_ptp_caps *)msg;

		/* process any state change needed due to new capabilities */
		iavf_ptp_process_caps(adapter);
		break;
	case VIRTCHNL_OP_1588_PTP_GET_TIME:
		iavf_virtchnl_ptp_get_time(adapter, msg, msglen);
		break;
	case VIRTCHNL_OP_ENABLE_QUEUES:
		/* enable transmits */
		iavf_irq_enable(adapter, true);
		wake_up(&adapter->reset_waitqueue);
		adapter->flags &= ~IAVF_FLAG_QUEUES_DISABLED;
		break;
	case VIRTCHNL_OP_DISABLE_QUEUES:
		iavf_free_all_tx_resources(adapter);
		iavf_free_all_rx_resources(adapter);
		if (adapter->state == __IAVF_DOWN_PENDING) {
			iavf_change_state(adapter, __IAVF_DOWN);
			wake_up(&adapter->down_waitqueue);
		}
		break;
	case VIRTCHNL_OP_VERSION:
	case VIRTCHNL_OP_CONFIG_IRQ_MAP:
		/* Don't display an error if we get these out of sequence.
		 * If the firmware needed to get kicked, we'll get these and
		 * it's no problem.
		 */
		if (v_opcode != adapter->current_op)
			return;
		break;
	case VIRTCHNL_OP_GET_RSS_HASHCFG_CAPS: {
		struct virtchnl_rss_hashcfg *vrh =
			(struct virtchnl_rss_hashcfg *)msg;

		if (msglen == sizeof(*vrh))
			adapter->rss_hashcfg = vrh->hashcfg;
		else
			dev_warn(&adapter->pdev->dev,
				 "Invalid message %d from PF\n", v_opcode);
		}
		break;
	case VIRTCHNL_OP_REQUEST_QUEUES: {
		struct virtchnl_vf_res_request *vfres =
			(struct virtchnl_vf_res_request *)msg;

		if (vfres->num_queue_pairs != adapter->num_req_queues) {
			dev_info(&adapter->pdev->dev,
				 "Requested %d queues, PF can support %d\n",
				 adapter->num_req_queues,
				 vfres->num_queue_pairs);
			adapter->num_req_queues = 0;
			adapter->flags &= ~IAVF_FLAG_REINIT_ITR_NEEDED;
		}
		}
		break;
	case VIRTCHNL_OP_ADD_CLOUD_FILTER: {
		struct iavf_cloud_filter *cf;

		list_for_each_entry(cf, &adapter->cloud_filter_list, list) {
			if (cf->state == __IAVF_CF_ADD_PENDING)
				cf->state = __IAVF_CF_ACTIVE;
		}
		}
		break;
	case VIRTCHNL_OP_DEL_CLOUD_FILTER: {
		struct iavf_cloud_filter *cf, *cftmp;

		list_for_each_entry_safe(cf, cftmp, &adapter->cloud_filter_list,
					 list) {
			if (cf->state == __IAVF_CF_DEL_PENDING) {
				cf->state = __IAVF_CF_INVALID;
				list_del(&cf->list);
				kfree(cf);
				adapter->num_cloud_filters--;
			}
		}
		}
		break;
	case VIRTCHNL_OP_ADD_FDIR_FILTER: {
		struct virtchnl_fdir_add *add_fltr = (struct virtchnl_fdir_add *)msg;
		struct iavf_fdir_fltr *fdir, *fdir_tmp;

		spin_lock_bh(&adapter->fdir_fltr_lock);
		list_for_each_entry_safe(fdir, fdir_tmp,
					 &adapter->fdir_list_head,
					 list) {
			if (fdir->state == IAVF_FDIR_FLTR_ADD_PENDING) {
				if (add_fltr->status == VIRTCHNL_FDIR_SUCCESS) {
					if (!iavf_is_raw_fdir(fdir))
						dev_info(&adapter->pdev->dev, "Flow Director filter with location %u is added\n",
							 fdir->loc);
					else
						dev_info(&adapter->pdev->dev, "Flow Director filter (raw) for TC handle %x is added\n",
							 TC_U32_USERHTID(fdir->cls_u32_handle));
					fdir->state = IAVF_FDIR_FLTR_ACTIVE;
					fdir->flow_id = add_fltr->flow_id;
				} else {
					dev_info(&adapter->pdev->dev, "Failed to add Flow Director filter with status: %d\n",
						 add_fltr->status);
					iavf_print_fdir_fltr(adapter, fdir);
					list_del(&fdir->list);
					iavf_dec_fdir_active_fltr(adapter, fdir);
					kfree(fdir);
				}
			}
		}
		spin_unlock_bh(&adapter->fdir_fltr_lock);
		}
		break;
	case VIRTCHNL_OP_DEL_FDIR_FILTER: {
		struct virtchnl_fdir_del *del_fltr = (struct virtchnl_fdir_del *)msg;
		struct iavf_fdir_fltr *fdir, *fdir_tmp;

		spin_lock_bh(&adapter->fdir_fltr_lock);
		list_for_each_entry_safe(fdir, fdir_tmp, &adapter->fdir_list_head,
					 list) {
			if (fdir->state == IAVF_FDIR_FLTR_DEL_PENDING) {
				if (del_fltr->status == VIRTCHNL_FDIR_SUCCESS ||
				    del_fltr->status ==
				    VIRTCHNL_FDIR_FAILURE_RULE_NONEXIST) {
					if (!iavf_is_raw_fdir(fdir))
						dev_info(&adapter->pdev->dev, "Flow Director filter with location %u is deleted\n",
							 fdir->loc);
					else
						dev_info(&adapter->pdev->dev, "Flow Director filter (raw) for TC handle %x is deleted\n",
							 TC_U32_USERHTID(fdir->cls_u32_handle));
					list_del(&fdir->list);
					iavf_dec_fdir_active_fltr(adapter, fdir);
					kfree(fdir);
				} else {
					fdir->state = IAVF_FDIR_FLTR_ACTIVE;
					dev_info(&adapter->pdev->dev, "Failed to delete Flow Director filter with status: %d\n",
						 del_fltr->status);
					iavf_print_fdir_fltr(adapter, fdir);
				}
			} else if (fdir->state == IAVF_FDIR_FLTR_DIS_PENDING) {
				if (del_fltr->status == VIRTCHNL_FDIR_SUCCESS ||
				    del_fltr->status ==
				    VIRTCHNL_FDIR_FAILURE_RULE_NONEXIST) {
					fdir->state = IAVF_FDIR_FLTR_INACTIVE;
				} else {
					fdir->state = IAVF_FDIR_FLTR_ACTIVE;
					dev_info(&adapter->pdev->dev, "Failed to disable Flow Director filter with status: %d\n",
						 del_fltr->status);
					iavf_print_fdir_fltr(adapter, fdir);
				}
			}
		}
		spin_unlock_bh(&adapter->fdir_fltr_lock);
		}
		break;
	case VIRTCHNL_OP_ADD_RSS_CFG: {
		struct iavf_adv_rss *rss;

		spin_lock_bh(&adapter->adv_rss_lock);
		list_for_each_entry(rss, &adapter->adv_rss_list_head, list) {
			if (rss->state == IAVF_ADV_RSS_ADD_PENDING) {
				iavf_print_adv_rss_cfg(adapter, rss,
						       "Input set change for",
						       "successful");
				rss->state = IAVF_ADV_RSS_ACTIVE;
			}
		}
		spin_unlock_bh(&adapter->adv_rss_lock);
		}
		break;
	case VIRTCHNL_OP_DEL_RSS_CFG: {
		struct iavf_adv_rss *rss, *rss_tmp;

		spin_lock_bh(&adapter->adv_rss_lock);
		list_for_each_entry_safe(rss, rss_tmp,
					 &adapter->adv_rss_list_head, list) {
			if (rss->state == IAVF_ADV_RSS_DEL_PENDING) {
				list_del(&rss->list);
				kfree(rss);
			}
		}
		spin_unlock_bh(&adapter->adv_rss_lock);
		}
		break;
	case VIRTCHNL_OP_ADD_VLAN_V2: {
		struct iavf_vlan_filter *f;

		spin_lock_bh(&adapter->mac_vlan_list_lock);
		list_for_each_entry(f, &adapter->vlan_filter_list, list) {
			if (f->state == IAVF_VLAN_IS_NEW)
				f->state = IAVF_VLAN_ACTIVE;
		}
		spin_unlock_bh(&adapter->mac_vlan_list_lock);
		}
		break;
	case VIRTCHNL_OP_ENABLE_VLAN_STRIPPING:
		/* PF enabled vlan strip on this VF.
		 * Update netdev->features if needed to be in sync with ethtool.
		 */
		if (!v_retval)
			iavf_netdev_features_vlan_strip_set(netdev, true);
		break;
	case VIRTCHNL_OP_DISABLE_VLAN_STRIPPING:
		/* PF disabled vlan strip on this VF.
		 * Update netdev->features if needed to be in sync with ethtool.
		 */
		if (!v_retval)
			iavf_netdev_features_vlan_strip_set(netdev, false);
		break;
	case VIRTCHNL_OP_GET_QOS_CAPS: {
		u16 len = struct_size(adapter->qos_caps, cap,
				      IAVF_MAX_QOS_TC_NUM);

		memcpy(adapter->qos_caps, msg, min(msglen, len));

		adapter->aq_required |= IAVF_FLAG_AQ_CFG_QUEUES_QUANTA_SIZE;
		}
		break;
	case VIRTCHNL_OP_CONFIG_QUANTA:
		break;
	case VIRTCHNL_OP_CONFIG_QUEUE_BW: {
		int i;
		/* shaper configuration is successful for all queues */
		for (i = 0; i < adapter->num_active_queues; i++)
			adapter->tx_rings[i].q_shaper_update = false;
	}
		break;
	default:
		if (adapter->current_op && (v_opcode != adapter->current_op))
			dev_warn(&adapter->pdev->dev, "Expected response %d from PF, received %d\n",
				 adapter->current_op, v_opcode);
		break;
	} /* switch v_opcode */
	adapter->current_op = VIRTCHNL_OP_UNKNOWN;
}
