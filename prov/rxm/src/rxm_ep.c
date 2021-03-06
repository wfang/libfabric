/*
 * Copyright (c) 2013-2016 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <inttypes.h>

#include "fi.h"
#include <fi_iov.h>
#include <fi_util.h>

#include "rxm.h"

static void rxm_mr_buf_close(void *pool_ctx, void *context)
{
	/* We would get a (fid_mr *) in context but it is safe to cast it into (fid *) */
	fi_close((struct fid *)context);
}

static int rxm_mr_buf_reg(void *pool_ctx, void *addr, size_t len, void **context)
{
	int ret;
	struct fid_mr *mr;
	struct fid_domain *msg_domain = (struct fid_domain *)pool_ctx;

	ret = fi_mr_reg(msg_domain, addr, len, FI_SEND | FI_RECV, 0, 0, 0, &mr, NULL);
	*context = mr;
	return ret;
}

static int rxm_buf_pool_create(int local_mr, size_t count, size_t size,
		struct util_buf_pool **pool, void *pool_ctx)
{
	*pool = local_mr ? util_buf_pool_create_ex(RXM_BUF_SIZE + size, 16, 0, count,
				rxm_mr_buf_reg, rxm_mr_buf_close, pool_ctx) :
		util_buf_pool_create(RXM_BUF_SIZE, 16, 0, count);
	if (!(*pool)) {
		FI_WARN(&rxm_prov, FI_LOG_EP_DATA, "Unable to create buf pool\n");
		return -FI_ENOMEM;
	}
	return 0;
}

static int rxm_recv_queue_init(struct rxm_recv_queue *recv_queue, size_t size)
{
	recv_queue->recv_fs = rxm_recv_fs_create(size);
	if (!recv_queue->recv_fs)
		return -FI_ENOMEM;
	dlist_init(&recv_queue->recv_list);
	dlist_init(&recv_queue->unexp_msg_list);
	return 0;
}

static void rxm_recv_queue_close(struct rxm_recv_queue *recv_queue)
{
	if (recv_queue->recv_fs)
		rxm_recv_fs_free(recv_queue->recv_fs);
	// TODO cleanup recv_list and unexp msg list
}

static int rxm_ep_txrx_res_open(struct rxm_ep *rxm_ep)
{
	struct rxm_domain *rxm_domain;
	uint8_t local_mr;
	int ret;

	rxm_domain = container_of(rxm_ep->util_ep.domain, struct rxm_domain, util_domain);
	local_mr = rxm_ep->msg_info->mode & FI_LOCAL_MR ? 1 : 0;

	FI_DBG(&rxm_prov, FI_LOG_EP_CTRL, "MSG provider mode & FI_LOCAL_MR: %d\n",
			local_mr);

	ret = rxm_buf_pool_create(local_mr, rxm_ep->msg_info->tx_attr->size,
			sizeof(struct rxm_pkt), &rxm_ep->tx_pool, rxm_domain->msg_domain);
	if (ret)
	        return ret;

	ret = rxm_buf_pool_create(local_mr, rxm_ep->msg_info->rx_attr->size,
			sizeof(struct rxm_rx_buf), &rxm_ep->rx_pool, rxm_domain->msg_domain);
	if (ret)
		goto err1;

	rxm_ep->txe_fs = rxm_txe_fs_create(rxm_ep->rxm_info->tx_attr->size);
	if (!rxm_ep->txe_fs) {
		ret = -FI_ENOMEM;
		goto err2;
	}

	ofi_key_idx_init(&rxm_ep->tx_key_idx, fi_size_bits(rxm_ep->rxm_info->tx_attr->size));

	ret = rxm_recv_queue_init(&rxm_ep->recv_queue, rxm_ep->rxm_info->rx_attr->size);
	if (ret)
		goto err3;

	ret = rxm_recv_queue_init(&rxm_ep->trecv_queue, rxm_ep->rxm_info->rx_attr->size);
	if (ret)
		goto err4;

	return 0;
err4:
	rxm_recv_queue_close(&rxm_ep->recv_queue);
err3:
	rxm_txe_fs_free(rxm_ep->txe_fs);
err2:
	util_buf_pool_destroy(rxm_ep->tx_pool);
err1:
	util_buf_pool_destroy(rxm_ep->rx_pool);
	return ret;
}

static void rxm_ep_txrx_res_close(struct rxm_ep *rxm_ep)
{
	struct slist_entry *entry;
	struct rxm_rx_buf *rx_buf;

	rxm_recv_queue_close(&rxm_ep->trecv_queue);
	rxm_recv_queue_close(&rxm_ep->recv_queue);

	if (rxm_ep->txe_fs)
		rxm_txe_fs_free(rxm_ep->txe_fs);

	while(!slist_empty(&rxm_ep->rx_buf_list)) {
		entry = slist_remove_head(&rxm_ep->rx_buf_list);
		rx_buf = container_of(entry, struct rxm_rx_buf, entry);
		util_buf_release(rxm_ep->rx_pool, rx_buf);
	}

	util_buf_pool_destroy(rxm_ep->rx_pool);
	util_buf_pool_destroy(rxm_ep->tx_pool);
}

int rxm_ep_repost_buf(struct rxm_rx_buf *rx_buf)
{
	struct fid_mr *mr;
	void *desc = NULL;
	int ret;

	rx_buf->conn = NULL;
	rx_buf->recv_fs = NULL;
	rx_buf->recv_entry = NULL;
	memset(&rx_buf->unexp_msg, 0, sizeof(rx_buf->unexp_msg));
	rx_buf->state = RXM_LMT_NONE;
	rx_buf->rma_iov = NULL;

	if (rx_buf->ep->msg_info->mode & FI_LOCAL_MR) {
		mr = util_buf_get_ctx(rx_buf->ep->rx_pool, rx_buf);
		desc = fi_mr_desc(mr);
	}

	FI_DBG(&rxm_prov, FI_LOG_EP_CTRL, "Re-posting rx buf\n");
	ret = fi_recv(rx_buf->ep->srx_ctx, &rx_buf->pkt, RXM_BUF_SIZE, desc,
			FI_ADDR_UNSPEC,	rx_buf);
	if (ret)
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to repost buf\n");
	return ret;
}

int rxm_ep_prepost_buf(struct rxm_ep *rxm_ep)
{
	struct rxm_rx_buf *rx_buf;
	int ret, i;

	for (i = 0; i < rxm_ep->rx_pool->chunk_cnt; i++) {
		rx_buf = util_buf_get(rxm_ep->rx_pool);
		rx_buf->ctx_type = RXM_RX_BUF;
		rx_buf->ep = rxm_ep;

		ret = rxm_ep_repost_buf(rx_buf);
		if (ret) {
			util_buf_release(rxm_ep->rx_pool, rx_buf);
			return ret;
		}
		slist_insert_tail(&rx_buf->entry, &rxm_ep->rx_buf_list);
	}
	return 0;
}

int rxm_setname(fid_t fid, void *addr, size_t addrlen)
{
	struct rxm_ep *rxm_ep;

	rxm_ep = container_of(fid, struct rxm_ep, util_ep.ep_fid.fid);
	return fi_setname(&rxm_ep->msg_pep->fid, addr, addrlen);
}

int rxm_getname(fid_t fid, void *addr, size_t *addrlen)
{
	struct rxm_ep *rxm_ep;

	rxm_ep = container_of(fid, struct rxm_ep, util_ep.ep_fid.fid);
	return fi_getname(&rxm_ep->msg_pep->fid, addr, addrlen);
}

static struct fi_ops_cm rxm_ops_cm = {
	.size = sizeof(struct fi_ops_cm),
	.setname = rxm_setname,
	.getname = rxm_getname,
	.getpeer = fi_no_getpeer,
	.connect = fi_no_connect,
	.listen = fi_no_listen,
	.accept = fi_no_accept,
	.reject = fi_no_reject,
	.shutdown = fi_no_shutdown,
};

int rxm_getopt(fid_t fid, int level, int optname,
		void *optval, size_t *optlen)
{
	return -FI_ENOPROTOOPT;
}

int rxm_setopt(fid_t fid, int level, int optname,
		const void *optval, size_t optlen)
{
	return -FI_ENOPROTOOPT;
}

static struct fi_ops_ep rxm_ops_ep = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = fi_no_cancel,
	.getopt = rxm_getopt,
	.setopt = rxm_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};

static inline uint64_t rxm_ep_tx_flags(struct fid_ep *ep_fid) {
	struct rxm_ep *rxm_ep = container_of(ep_fid, struct rxm_ep, util_ep.ep_fid);
	return rxm_ep->rxm_info->tx_attr->op_flags;
}

static inline uint64_t rxm_ep_rx_flags(struct fid_ep *ep_fid) {
	struct rxm_ep *rxm_ep = container_of(ep_fid, struct rxm_ep, util_ep.ep_fid);
	return rxm_ep->rxm_info->rx_attr->op_flags;
}

static int ofi_match_unexp_msg(struct dlist_entry *item, const void *arg)
{
	struct rxm_recv_match_attr *attr = (struct rxm_recv_match_attr *)arg;
	struct rxm_unexp_msg *unexp_msg;

	unexp_msg = container_of(item, struct rxm_unexp_msg, entry);
	return rxm_match_addr(unexp_msg->addr, attr->addr);
}

static int ofi_match_unexp_msg_tagged(struct dlist_entry *item, const void *arg)
{
	struct rxm_recv_match_attr *attr = (struct rxm_recv_match_attr *)arg;
	struct rxm_unexp_msg *unexp_msg;

	unexp_msg = container_of(item, struct rxm_unexp_msg, entry);
	return rxm_match_addr(attr->tag, unexp_msg->addr) &&
		rxm_match_tag(attr->tag, attr->ignore, unexp_msg->tag);
}

static int rxm_check_unexp_msg_list(struct util_cq *util_cq, struct rxm_recv_queue *recv_queue,
		struct rxm_recv_entry *recv_entry, dlist_func_t *match)
{
	struct dlist_entry *entry;
	struct rxm_unexp_msg *unexp_msg;
	struct rxm_recv_match_attr match_attr;
	struct rxm_rx_buf *rx_buf;
	int ret = 0;

	fastlock_acquire(&util_cq->cq_lock);
	if (ofi_cirque_isfull(util_cq->cirq)) {
		ret = -FI_EAGAIN;
		goto out;
	}

	match_attr.addr = recv_entry->addr;
	match_attr.tag = recv_entry->tag;
	match_attr.ignore = recv_entry->ignore;

	entry = dlist_remove_first_match(&recv_queue->unexp_msg_list, match, &match_attr);
	if (!entry)
		goto out;
	FI_DBG(&rxm_prov, FI_LOG_EP_DATA, "Match for posted recv found in unexp msg list\n");

	unexp_msg = container_of(entry, struct rxm_unexp_msg, entry);
	rx_buf = container_of(unexp_msg, struct rxm_rx_buf, unexp_msg);
	rx_buf->recv_entry = recv_entry;

	ret = rxm_cq_handle_data(rx_buf);
	free(unexp_msg);
out:
	fastlock_release(&util_cq->cq_lock);
	return ret;
}

int rxm_ep_recv_common(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
		size_t count, fi_addr_t src_addr, uint64_t tag, uint64_t ignore,
		void *context, uint64_t flags, int op)
{
	struct rxm_recv_entry *recv_entry;
	struct rxm_ep *rxm_ep;
	struct rxm_recv_queue *recv_queue;
	dlist_func_t *match;
	int ret, i;

	rxm_ep = container_of(ep_fid, struct rxm_ep, util_ep.ep_fid.fid);

	// TODO pass recv_queue as arg
	if (op == ofi_op_msg) {
		recv_queue = &rxm_ep->recv_queue;
		match = ofi_match_unexp_msg;
	} else if (op == ofi_op_tagged) {
		recv_queue = &rxm_ep->trecv_queue;
		match = ofi_match_unexp_msg_tagged;
	} else {
		FI_WARN(&rxm_prov, FI_LOG_EP_DATA, "Unknown op!\n");
		return -FI_EINVAL;
	}

	if (freestack_isempty(recv_queue->recv_fs)) {
		FI_DBG(&rxm_prov, FI_LOG_CQ, "Exhaused recv_entry freestack\n");
		return -FI_EAGAIN;
	}

	recv_entry = freestack_pop(recv_queue->recv_fs);

	for (i = 0; i < count; i++) {
		recv_entry->iov[i].iov_base = iov[i].iov_base;
		recv_entry->iov[i].iov_len = iov[i].iov_len;
		recv_entry->desc[i] = desc[i];
		FI_DBG(&rxm_prov, FI_LOG_EP_CTRL, "post recv: %u\n",
			iov[i].iov_len);
	}
	recv_entry->count = count;
	recv_entry->addr = (rxm_ep->rxm_info->caps & FI_DIRECTED_RECV) ?
		src_addr : FI_ADDR_UNSPEC;
	recv_entry->flags = flags;
	if (op == ofi_op_tagged) {
		recv_entry->tag = tag;
		recv_entry->ignore = ignore;
	}

	if (!dlist_empty(&recv_queue->unexp_msg_list)) {
		ret = rxm_check_unexp_msg_list(rxm_ep->util_ep.rx_cq, recv_queue,
				recv_entry, match);
		if (ret) {
			FI_WARN(&rxm_prov, FI_LOG_EP_DATA,
					"Unable to check unexp msg list\n");
			return ret;
		}
	}

	dlist_insert_tail(&recv_entry->entry, &recv_queue->recv_list);
	return 0;
}

static ssize_t rxm_ep_recvmsg(struct fid_ep *ep_fid, const struct fi_msg *msg,
			       uint64_t flags)
{
	return rxm_ep_recv_common(ep_fid, msg->msg_iov, msg->desc, msg->iov_count,
			msg->addr, 0, 0, msg->context,
			flags  | (rxm_ep_rx_flags(ep_fid) & FI_COMPLETION),
			ofi_op_msg);
}

static ssize_t rxm_ep_recv(struct fid_ep *ep_fid, void *buf, size_t len, void *desc,
			    fi_addr_t src_addr, void *context)
{
	struct iovec iov;
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = buf;
	iov.iov_len = len;

	return rxm_ep_recv_common(ep_fid, &iov, &desc, 1, src_addr, 0, 0,
			context, rxm_ep_rx_flags(ep_fid), ofi_op_msg);
}

static ssize_t rxm_ep_recvv(struct fid_ep *ep_fid, const struct iovec *iov,
		void **desc, size_t count, fi_addr_t src_addr, void *context)
{
	return rxm_ep_recv_common(ep_fid, iov, desc, count, src_addr, 0, 0,
			context, rxm_ep_rx_flags(ep_fid), ofi_op_msg);
}

static void rxm_op_hdr_process_flags(struct ofi_op_hdr *hdr, uint64_t flags,
		uint64_t data)
{
	if (flags & FI_REMOTE_CQ_DATA) {
		hdr->flags = OFI_REMOTE_CQ_DATA;
		hdr->data = data;
	}
	if (flags & FI_TRANSMIT_COMPLETE)
		hdr->flags |= OFI_TRANSMIT_COMPLETE;
	if (flags & FI_DELIVERY_COMPLETE)
		hdr->flags |= OFI_DELIVERY_COMPLETE;
}

void rxm_pkt_init(struct rxm_pkt *pkt)
{
	memset(pkt, 0, sizeof(*pkt));
	pkt->ctrl_hdr.version = OFI_CTRL_VERSION;
	pkt->hdr.version = OFI_OP_VERSION;
}

// TODO handle all flags
static ssize_t rxm_ep_send_common(struct fid_ep *ep_fid, const struct iovec *iov,
		void **desc, size_t count, fi_addr_t dest_addr, void *context,
		uint64_t data, uint64_t tag, uint64_t flags, int op)
{
	struct rxm_ep *rxm_ep;
	struct rxm_conn *rxm_conn;
	struct rxm_tx_entry *tx_entry;
	struct rxm_pkt *pkt;
	struct fid_mr *mr;
	void *desc_tx_buf = NULL;
	struct rxm_rma_iov *rma_iov;
	int pkt_size = 0;
	int i, ret;

	rxm_ep = container_of(ep_fid, struct rxm_ep, util_ep.ep_fid.fid);

	ret = rxm_get_conn(rxm_ep, dest_addr, &rxm_conn);
	if (ret)
		return ret;

	if (freestack_isempty(rxm_ep->txe_fs)) {
		FI_DBG(&rxm_prov, FI_LOG_CQ, "Exhaused tx_entry freestack\n");
		return -FI_ENOMEM;
	}

	tx_entry = freestack_pop(rxm_ep->txe_fs);

	tx_entry->ctx_type = RXM_TX_ENTRY;
	tx_entry->ep = rxm_ep;
	tx_entry->context = context;
	tx_entry->flags = flags;

	if (rxm_ep->msg_info->mode & FI_LOCAL_MR) {
		pkt = util_buf_get_ex(rxm_ep->tx_pool, (void **)&mr);
		desc_tx_buf = fi_mr_desc(mr);
	} else {
		pkt = util_buf_get(rxm_ep->tx_pool);
	}
	assert(pkt);

	tx_entry->pkt = pkt;

	rxm_pkt_init(pkt);
	pkt->ctrl_hdr.conn_id = rxm_conn->handle.remote_key;
	pkt->hdr.op = op;
	pkt->hdr.size = ofi_get_iov_len(iov, count);
	rxm_op_hdr_process_flags(&pkt->hdr, flags, data);

	if (op == ofi_op_tagged)
		pkt->hdr.tag = tag;

	if (pkt->hdr.size > RXM_TX_DATA_SIZE) {
		if (flags & FI_INJECT) {
			FI_WARN(&rxm_prov, FI_LOG_EP_DATA,
					"inject size supported: %d, msg size: %d\n",
					rxm_tx_attr.inject_size,
					pkt->hdr.size);
			ret = -FI_EMSGSIZE;
			goto err;
		}
		tx_entry->msg_id = ofi_idx2key(&rxm_ep->tx_key_idx,
				rxm_txe_fs_index(rxm_ep->txe_fs, tx_entry));
		pkt->ctrl_hdr.msg_id = tx_entry->msg_id;
		pkt->ctrl_hdr.type = ofi_ctrl_large_data;
		rma_iov = (struct rxm_rma_iov *)pkt->data;
		rma_iov->count = count;
		for (i = 0; i < count; i++) {
			rma_iov->iov[i].addr = rxm_ep->msg_info->domain_attr->mr_mode == FI_MR_SCALABLE ?
				0 : (uintptr_t)iov->iov_base;
			rma_iov->iov[i].len = (uint64_t)iov->iov_len;
			rma_iov->iov[i].key = fi_mr_key(desc[i]);
		}
		pkt_size = sizeof(*pkt) + sizeof(*rma_iov) + sizeof(*rma_iov->iov) * count;
		FI_DBG(&rxm_prov, FI_LOG_CQ,
				"Sending large msg. msg_id: 0x%" PRIx64 "\n",
				tx_entry->msg_id);
		FI_DBG(&rxm_prov, FI_LOG_CQ, "tx_entry->state -> RXM_LMT_START\n");
		tx_entry->state = RXM_LMT_START;
	} else {
		pkt->ctrl_hdr.type = ofi_ctrl_data;
		ofi_copy_iov_buf(iov, count, pkt->data, pkt->hdr.size, 0,
				OFI_COPY_IOV_TO_BUF);
		pkt_size = sizeof(*pkt) + pkt->hdr.size;
	}

	ret = fi_send(rxm_conn->msg_ep, pkt, pkt_size, desc_tx_buf, 0, tx_entry);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_DATA, "fi_send for MSG provider failed\n");
		goto err;
	}
	return 0;
err:
	util_buf_release(rxm_ep->tx_pool, pkt);
	freestack_push(rxm_ep->txe_fs, tx_entry);
	return ret;
}

static ssize_t rxm_ep_sendmsg(struct fid_ep *ep_fid, const struct fi_msg *msg,
			       uint64_t flags)
{

	return rxm_ep_send_common(ep_fid, msg->msg_iov, msg->desc, msg->iov_count,
			msg->addr, msg->context, msg->data, 0,
			flags | (rxm_ep_tx_flags(ep_fid) & FI_COMPLETION), ofi_op_msg);
}

static ssize_t rxm_ep_send(struct fid_ep *ep_fid, const void *buf, size_t len,
		void *desc, fi_addr_t dest_addr, void *context)
{
	struct iovec iov;
	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxm_ep_send_common(ep_fid, &iov, &desc, 1, dest_addr, context, 0,
			0, rxm_ep_tx_flags(ep_fid), ofi_op_msg);
}

static ssize_t rxm_ep_sendv(struct fid_ep *ep_fid, const struct iovec *iov,
		void **desc, size_t count, fi_addr_t dest_addr, void *context)
{
	return rxm_ep_send_common(ep_fid, iov, desc, count, dest_addr, context,
			0, 0, rxm_ep_tx_flags(ep_fid), ofi_op_msg);
}

static ssize_t	rxm_ep_inject(struct fid_ep *ep_fid, const void *buf, size_t len,
			       fi_addr_t dest_addr)
{
	struct iovec iov;

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxm_ep_send_common(ep_fid, &iov, NULL, 1, dest_addr, NULL, 0, 0,
			(rxm_ep_tx_flags(ep_fid) & ~FI_COMPLETION) | FI_INJECT,
			ofi_op_msg);
}

static ssize_t rxm_ep_senddata(struct fid_ep *ep_fid, const void *buf, size_t len, void *desc,
				uint64_t data, fi_addr_t dest_addr, void *context)
{
	struct iovec iov;

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxm_ep_send_common(ep_fid, &iov, desc, 1, dest_addr, context, data,
			0, rxm_ep_tx_flags(ep_fid), ofi_op_msg);
}

static ssize_t	rxm_ep_injectdata(struct fid_ep *ep_fid, const void *buf, size_t len,
				   uint64_t data, fi_addr_t dest_addr)
{
	struct iovec iov;

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxm_ep_send_common(ep_fid, &iov, NULL, 1, dest_addr, NULL, data, 0,
			(rxm_ep_tx_flags(ep_fid) & ~FI_COMPLETION) | FI_INJECT,
			ofi_op_msg);
}

static struct fi_ops_msg rxm_ops_msg = {
	.size = sizeof(struct fi_ops_msg),
	.recv = rxm_ep_recv,
	.recvv = rxm_ep_recvv,
	.recvmsg = rxm_ep_recvmsg,
	.send = rxm_ep_send,
	.sendv = rxm_ep_sendv,
	.sendmsg = rxm_ep_sendmsg,
	.inject = rxm_ep_inject,
	.senddata = rxm_ep_senddata,
	.injectdata = rxm_ep_injectdata,
};

ssize_t rxm_ep_trecvmsg(struct fid_ep *ep_fid, const struct fi_msg_tagged *msg,
			 uint64_t flags)
{
	return rxm_ep_recv_common(ep_fid, msg->msg_iov, msg->desc, msg->iov_count,
			msg->addr, msg->tag, msg->ignore, msg->context,
			flags | (rxm_ep_rx_flags(ep_fid) & FI_COMPLETION),
			ofi_op_tagged);
}

static ssize_t rxm_ep_trecv(struct fid_ep *ep_fid, void *buf, size_t len, void *desc,
		fi_addr_t src_addr, uint64_t tag, uint64_t ignore, void *context)
{
	struct iovec iov;
	iov.iov_base = buf;
	iov.iov_len = len;

	return rxm_ep_recv_common(ep_fid, &iov, &desc, 1, src_addr, tag, ignore,
			context, rxm_ep_rx_flags(ep_fid), ofi_op_tagged);
}

ssize_t rxm_ep_trecvv(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
		size_t count, fi_addr_t src_addr, uint64_t tag, uint64_t ignore,
		void *context)
{
	return rxm_ep_recv_common(ep_fid, iov, desc, count, src_addr, tag, ignore,
			context, rxm_ep_rx_flags(ep_fid), ofi_op_tagged);
}

ssize_t rxm_ep_tsendmsg(struct fid_ep *ep_fid, const struct fi_msg_tagged *msg,
			 uint64_t flags)
{
	return rxm_ep_send_common(ep_fid, msg->msg_iov, msg->desc, msg->iov_count,
			msg->addr, msg->context, msg->data, msg->tag,
			flags | (rxm_ep_tx_flags(ep_fid) & FI_COMPLETION),
			ofi_op_tagged);
}

ssize_t rxm_ep_tsend(struct fid_ep *ep_fid, const void *buf, size_t len, void *desc,
		      fi_addr_t dest_addr, uint64_t tag, void *context)
{
	struct iovec iov;
	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxm_ep_send_common(ep_fid, &iov, &desc, 1, dest_addr, context, 0,
			tag, rxm_ep_tx_flags(ep_fid), ofi_op_tagged);
}

ssize_t rxm_ep_tsendv(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
		       size_t count, fi_addr_t dest_addr, uint64_t tag, void *context)
{
	return rxm_ep_send_common(ep_fid, iov, desc, count, dest_addr, context,
			0, tag, rxm_ep_tx_flags(ep_fid), ofi_op_tagged);
}

ssize_t	rxm_ep_tinject(struct fid_ep *ep_fid, const void *buf, size_t len,
			fi_addr_t dest_addr, uint64_t tag)
{
	struct iovec iov;

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxm_ep_send_common(ep_fid, &iov, NULL, 1, dest_addr, NULL, 0, tag,
			(rxm_ep_tx_flags(ep_fid) & ~FI_COMPLETION) | FI_INJECT,
			ofi_op_tagged);
}

ssize_t rxm_ep_tsenddata(struct fid_ep *ep_fid, const void *buf, size_t len, void *desc,
			  uint64_t data, fi_addr_t dest_addr, uint64_t tag, void *context)
{
	struct iovec iov;

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxm_ep_send_common(ep_fid, &iov, desc, 1, dest_addr, context, data,
			tag, rxm_ep_tx_flags(ep_fid), ofi_op_tagged);
}

ssize_t	rxm_ep_tinjectdata(struct fid_ep *ep_fid, const void *buf, size_t len,
			    uint64_t data, fi_addr_t dest_addr, uint64_t tag)
{
	struct iovec iov;

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return rxm_ep_send_common(ep_fid, &iov, NULL, 1, dest_addr, NULL, data,
			tag, (rxm_ep_tx_flags(ep_fid) & ~FI_COMPLETION) | FI_INJECT,
			ofi_op_tagged);
}

struct fi_ops_tagged rxm_ops_tagged = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = rxm_ep_trecv,
	.recvv = rxm_ep_trecvv,
	.recvmsg = rxm_ep_trecvmsg,
	.send = rxm_ep_tsend,
	.sendv = rxm_ep_tsendv,
	.sendmsg = rxm_ep_tsendmsg,
	.inject = rxm_ep_tinject,
	.senddata = rxm_ep_tsenddata,
	.injectdata = rxm_ep_tinjectdata,
};

static int rxm_ep_msg_res_close(struct rxm_ep *rxm_ep)
{
	int ret, retv = 0;

	ret = fi_close(&rxm_ep->msg_cq->fid);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to close msg CQ\n");
		retv = ret;
	}

	ret = fi_close(&rxm_ep->srx_ctx->fid);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to close msg shared ctx\n");
		retv = ret;
	}

	ret = fi_close(&rxm_ep->msg_pep->fid);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "Unable to close msg passive EP\n");
		retv = ret;
	}

	fi_freeinfo(rxm_ep->msg_info);
	return retv;
}

static int rxm_ep_close(struct fid *fid)
{
	struct rxm_ep *rxm_ep;
	int ret;

	rxm_ep = container_of(fid, struct rxm_ep, util_ep.ep_fid.fid);

	if (rxm_ep->util_ep.cmap)
		ofi_cmap_free(rxm_ep->util_ep.cmap);

	rxm_ep_txrx_res_close(rxm_ep);
	ret = rxm_ep_msg_res_close(rxm_ep);

	if (rxm_ep->util_ep.tx_cq) {
		fid_list_remove(&rxm_ep->util_ep.tx_cq->ep_list,
				&rxm_ep->util_ep.tx_cq->ep_list_lock,
				&rxm_ep->util_ep.ep_fid.fid);
		atomic_dec(&rxm_ep->util_ep.tx_cq->ref);
	}

	if (rxm_ep->util_ep.rx_cq) {
		fid_list_remove(&rxm_ep->util_ep.rx_cq->ep_list,
				&rxm_ep->util_ep.rx_cq->ep_list_lock,
				&rxm_ep->util_ep.ep_fid.fid);
		atomic_dec(&rxm_ep->util_ep.rx_cq->ref);
	}

	ofi_endpoint_close(&rxm_ep->util_ep);
	free(rxm_ep);
	return ret;
}

static int rxm_ep_bind_cq(struct rxm_ep *rxm_ep, struct util_cq *util_cq, uint64_t flags)
{
	int ret;

	if (flags & ~(FI_TRANSMIT | FI_RECV)) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "unsupported flags\n");
		return -FI_EBADFLAGS;
	}

	if (((flags & FI_TRANSMIT) && rxm_ep->util_ep.tx_cq) ||
	    ((flags & FI_RECV) && rxm_ep->util_ep.rx_cq)) {
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL, "duplicate CQ binding\n");
		return -FI_EINVAL;
	}

	if (flags & FI_TRANSMIT) {
		rxm_ep->util_ep.tx_cq = util_cq;

		if (!(flags & FI_SELECTIVE_COMPLETION))
			rxm_ep->rxm_info->tx_attr->op_flags |= FI_COMPLETION;

		atomic_inc(&util_cq->ref);
	}

	if (flags & FI_RECV) {
		rxm_ep->util_ep.rx_cq = util_cq;

		if (!(flags & FI_SELECTIVE_COMPLETION))
			rxm_ep->rxm_info->rx_attr->op_flags |= FI_COMPLETION;

		atomic_inc(&util_cq->ref);
	}
	if (flags & (FI_TRANSMIT | FI_RECV)) {
		ret = fid_list_insert(&util_cq->ep_list,
				      &util_cq->ep_list_lock,
				      &rxm_ep->util_ep.ep_fid.fid);
		if (ret)
			return ret;
	}
	return 0;
}

static int rxm_ep_bind(struct fid *ep_fid, struct fid *bfid, uint64_t flags)
{
	struct rxm_ep *rxm_ep;
	struct util_av *util_av;
	int ret = 0;

	rxm_ep = container_of(ep_fid, struct rxm_ep, util_ep.ep_fid.fid);
	switch (bfid->fclass) {
	case FI_CLASS_AV:
		util_av = container_of(bfid, struct util_av, av_fid.fid);
		ret = ofi_ep_bind_av(&rxm_ep->util_ep, util_av);
		if (ret)
			return ret;
		rxm_ep->util_ep.cmap = ofi_cmap_alloc(util_av, rxm_conn_close);
		if (!rxm_ep->util_ep.cmap)
			return -FI_ENOMEM;
		break;
	case FI_CLASS_CQ:
		ret = rxm_ep_bind_cq(rxm_ep, container_of(bfid, struct util_cq,
					cq_fid.fid), flags);
		break;
	case FI_CLASS_EQ:
		break;
	default:
		FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
			"invalid fid class\n");
		ret = -FI_EINVAL;
		break;
	}
	return ret;
}

static int rxm_ep_ctrl(struct fid *fid, int command, void *arg)
{
	struct rxm_ep *rxm_ep;
	struct rxm_fabric *rxm_fabric;
	int ret;

	rxm_ep = container_of(fid, struct rxm_ep, util_ep.ep_fid.fid);
	rxm_fabric = container_of(rxm_ep->util_ep.domain->fabric,
			struct rxm_fabric, util_fabric);
	switch (command) {
	case FI_ENABLE:
		if (!rxm_ep->util_ep.rx_cq || !rxm_ep->util_ep.tx_cq)
			return -FI_ENOCQ;
		if (!rxm_ep->util_ep.av)
			return -FI_EOPBADSTATE;

		ret = rxm_ep_prepost_buf(rxm_ep);
		if (ret) {
			FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
					"Unable to prepost recv bufs\n");
			return ret;
		}
		ret = fi_pep_bind(rxm_ep->msg_pep, &rxm_fabric->msg_eq->fid, 0);
		if (ret) {
			FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
					"Unable to bind msg PEP to msg EQ\n");
			return ret;
		}
		ret = fi_listen(rxm_ep->msg_pep);
		if (ret) {
			FI_WARN(&rxm_prov, FI_LOG_EP_CTRL,
					"Unable to set msg PEP to listen state\n");
			return ret;
		}
		break;
	default:
		return -FI_ENOSYS;
	}
	return 0;
}

static struct fi_ops rxm_ep_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = rxm_ep_close,
	.bind = rxm_ep_bind,
	.control = rxm_ep_ctrl,
	.ops_open = fi_no_ops_open,
};

static int rxm_ep_msg_res_open(struct fi_info *rxm_info,
		struct util_domain *util_domain, struct rxm_ep *rxm_ep)
{
	struct rxm_fabric *rxm_fabric;
	struct rxm_domain *rxm_domain;
	struct fi_cq_attr cq_attr;
	int ret;

	ret = ofix_getinfo(rxm_prov.version, NULL, NULL, 0, &rxm_util_prov,
			rxm_info, rxm_alter_layer_info, rxm_alter_base_info,
			1, &rxm_ep->msg_info);
	if (ret)
		return ret;

	rxm_domain = container_of(util_domain, struct rxm_domain, util_domain);
	rxm_fabric = container_of(util_domain->fabric, struct rxm_fabric, util_fabric);

	ret = fi_passive_ep(rxm_fabric->msg_fabric, rxm_ep->msg_info, &rxm_ep->msg_pep, rxm_ep);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to open msg PEP\n");
		goto err1;
	}

	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.size = rxm_info->tx_attr->size + rxm_info->rx_attr->size;
	cq_attr.format = FI_CQ_FORMAT_MSG;

	ret = fi_cq_open(rxm_domain->msg_domain, &cq_attr, &rxm_ep->msg_cq, NULL);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_CQ, "Unable to open MSG CQ\n");
		goto err1;
	}

	ret = fi_srx_context(rxm_domain->msg_domain, rxm_ep->msg_info->rx_attr,
			&rxm_ep->srx_ctx, NULL);
	if (ret) {
		FI_WARN(&rxm_prov, FI_LOG_FABRIC, "Unable to open shared receive context\n");
		goto err2;
	}

	/* We don't care what's in the dest_addr at this point. We go by AV. */
	if (rxm_ep->msg_info->dest_addr) {
		free(rxm_ep->msg_info->dest_addr);
		rxm_ep->msg_info->dest_addr = NULL;
		rxm_ep->msg_info->dest_addrlen = 0;
	}

	/* Zero out the port as we would be creating multiple MSG EPs for a single
	 * RXM EP and we don't want address conflicts. */
	if (rxm_ep->msg_info->src_addr)
		((struct sockaddr_in *)(rxm_ep->msg_info->src_addr))->sin_port = 0;

	return 0;
err2:
	fi_close(&rxm_ep->msg_pep->fid);
err1:
	fi_freeinfo(rxm_ep->msg_info);
	return ret;
}

void rxm_ep_progress(struct util_ep *util_ep)
{
	struct rxm_ep *rxm_ep;

	rxm_ep = container_of(util_ep, struct rxm_ep, util_ep);
	rxm_cq_progress(rxm_ep->msg_cq);
}

int rxm_endpoint(struct fid_domain *domain, struct fi_info *info,
		  struct fid_ep **ep_fid, void *context)
{
	struct util_domain *util_domain;
	struct rxm_ep *rxm_ep;
	int ret;

	rxm_ep = calloc(1, sizeof(*rxm_ep));
	if (!rxm_ep)
		return -FI_ENOMEM;

	if (!(rxm_ep->rxm_info = fi_dupinfo(info))) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	ret = ofi_endpoint_init(domain, &rxm_util_prov, info, &rxm_ep->util_ep,
			context, &rxm_ep_progress, FI_MATCH_PREFIX);
	if (ret)
		goto err1;

	util_domain = container_of(domain, struct util_domain, domain_fid);

	ret = rxm_ep_msg_res_open(info, util_domain, rxm_ep);
	if (ret)
		goto err2;

	ret = rxm_ep_txrx_res_open(rxm_ep);
	if (ret)
		goto err3;

	*ep_fid = &rxm_ep->util_ep.ep_fid;
	(*ep_fid)->fid.ops = &rxm_ep_fi_ops;
	(*ep_fid)->ops = &rxm_ops_ep;
	(*ep_fid)->cm = &rxm_ops_cm;
	(*ep_fid)->msg = &rxm_ops_msg;
	(*ep_fid)->tagged = &rxm_ops_tagged;

	return 0;
err3:
	rxm_ep_msg_res_close(rxm_ep);
err2:
	ofi_endpoint_close(&rxm_ep->util_ep);
err1:
	if (rxm_ep->rxm_info)
		fi_freeinfo(rxm_ep->rxm_info);
	free(rxm_ep);
	return ret;
}
