/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _UAPI_LINUX_VIRTIO_PMEM_H
#define _UAPI_LINUX_VIRTIO_PMEM_H

struct virtio_pmem_config {
	__le64 start;
	__le64 size;
};
#endif
