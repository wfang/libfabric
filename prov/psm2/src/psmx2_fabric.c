/*
 * Copyright (c) 2013-2014 Intel Corporation. All rights reserved.
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

#include "psmx2.h"

struct psmx2_fid_fabric *psmx2_active_fabric = NULL;

static int psmx2_fabric_close(fid_t fid)
{
	struct psmx2_fid_fabric *fabric;
	void *exit_code;
	int ret;

	fabric = container_of(fid, struct psmx2_fid_fabric,
			      util_fabric.fabric_fid.fid);

	FI_INFO(&psmx2_prov, FI_LOG_CORE, "refcnt=%d\n",
		atomic_get(&fabric->util_fabric.ref));

	psmx2_fabric_release(fabric);

	if (ofi_fabric_close(&fabric->util_fabric))
		return 0;

	if (psmx2_env.name_server &&
	    !pthread_equal(fabric->name_server_thread, pthread_self())) {
		ret = pthread_cancel(fabric->name_server_thread);
		if (ret) {
			FI_INFO(&psmx2_prov, FI_LOG_CORE,
				"pthread_cancel returns %d\n", ret);
		}
		ret = pthread_join(fabric->name_server_thread, &exit_code);
		if (ret) {
			FI_INFO(&psmx2_prov, FI_LOG_CORE,
				"pthread_join returns %d\n", ret);
		} else {
			FI_INFO(&psmx2_prov, FI_LOG_CORE,
				"name server thread exited with code %ld (%s)\n",
				(uintptr_t)exit_code,
				(exit_code == PTHREAD_CANCELED) ? "PTHREAD_CANCELED" : "?");
		}
	}
	if (fabric->active_domain) {
		FI_WARN(&psmx2_prov, FI_LOG_CORE, "forced closing of active_domain\n");
		fi_close(&fabric->active_domain->util_domain.domain_fid.fid);
	}
	assert(fabric == psmx2_active_fabric);
	psmx2_active_fabric = NULL;
	free(fabric);

	return 0;
}

static struct fi_ops psmx2_fabric_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = psmx2_fabric_close,
};

static struct fi_ops_fabric psmx2_fabric_ops = {
	.size = sizeof(struct fi_ops_fabric),
	.domain = psmx2_domain_open,
	.passive_ep = fi_no_passive_ep,
	.eq_open = ofi_eq_create,
	.wait_open = psmx2_wait_open,
	.trywait = psmx2_wait_trywait
};

static struct fi_fabric_attr psmx2_fabric_attr = {
	.name = PSMX2_FABRIC_NAME,
	.prov_version = PSMX2_VERSION,
};

int psmx2_fabric(struct fi_fabric_attr *attr,
		 struct fid_fabric **fabric, void *context)
{
	struct psmx2_fid_fabric *fabric_priv;
	int ret;

	FI_INFO(&psmx2_prov, FI_LOG_CORE, "\n");

	if (strcmp(attr->name, PSMX2_FABRIC_NAME))
		return -FI_ENODATA;

	if (psmx2_active_fabric) {
		psmx2_fabric_acquire(psmx2_active_fabric);
		*fabric = &psmx2_active_fabric->util_fabric.fabric_fid;
		return 0;
	}

	fabric_priv = calloc(1, sizeof(*fabric_priv));
	if (!fabric_priv)
		return -FI_ENOMEM;

	ret = ofi_fabric_init(&psmx2_prov, &psmx2_fabric_attr, attr,
			     &fabric_priv->util_fabric, context,
			     FI_MATCH_EXACT);
	if (ret) {
		FI_INFO(&psmx2_prov, FI_LOG_CORE, "ofi_fabric_init returns %d\n", ret);
		free(fabric_priv);
		return ret;
	}

	/* fclass & context initialized in ofi_fabric_init */
	fabric_priv->util_fabric.fabric_fid.fid.ops = &psmx2_fabric_fi_ops;
	fabric_priv->util_fabric.fabric_fid.ops = &psmx2_fabric_ops;

	psmx2_get_uuid(fabric_priv->uuid);

	if (psmx2_env.name_server) {
		ret = pthread_create(&fabric_priv->name_server_thread, NULL,
				     psmx2_name_server, (void *)fabric_priv);
		if (ret) {
			FI_INFO(&psmx2_prov, FI_LOG_CORE, "pthread_create returns %d\n", ret);
			/* use the main thread's ID as invalid value for the new thread */
			fabric_priv->name_server_thread = pthread_self();
		}
	}

	psmx2_query_mpi();

	/* take the reference to count for multiple fabric open calls */
	psmx2_fabric_acquire(fabric_priv);

	*fabric = &fabric_priv->util_fabric.fabric_fid;
	psmx2_active_fabric = fabric_priv;

	return 0;
}

