// SPDX-License-Identifier: GPL-2.0
/*
 * virtio_pmem.c: Virtio pmem Driver
 *
 * Discovers persistent memory range information
 * from host and provides a virtio based flushing
 * interface.
 */
#include "virtio_pmem.h"
#include "nd.h"

 /* The interrupt handler */
void host_ack(struct virtqueue *vq)
{
	struct virtio_pmem *vpmem = vq->vdev->priv;
	struct virtio_pmem_request *req, *req_buf;
	unsigned long flags;
	unsigned int len;

	spin_lock_irqsave(&vpmem->pmem_lock, flags);
	while ((req = virtqueue_get_buf(vq, &len)) != NULL) {
		req->done = true;
		wake_up(&req->host_acked);

		if (!list_empty(&vpmem->req_list)) {
			req_buf = list_first_entry(&vpmem->req_list,
					struct virtio_pmem_request, list);
			req_buf->wq_buf_avail = true;
			wake_up(&req_buf->wq_buf);
			list_del(&req_buf->list);
		}
	}
	spin_unlock_irqrestore(&vpmem->pmem_lock, flags);
}
EXPORT_SYMBOL_GPL(host_ack);

 /* The request submission function */
int virtio_pmem_flush(struct nd_region *nd_region)
{
	struct virtio_device *vdev = nd_region->provider_data;
	struct virtio_pmem *vpmem = vdev->priv;
	struct scatterlist *sgs[2], sg, ret;
	struct virtio_pmem_request *req;
	unsigned long flags;
	int err, err1;

	might_sleep();
	req = kmalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->done = false;
	strcpy(req->name, "FLUSH");
	init_waitqueue_head(&req->host_acked);
	init_waitqueue_head(&req->wq_buf);
	INIT_LIST_HEAD(&req->list);
	sg_init_one(&sg, req->name, strlen(req->name));
	sgs[0] = &sg;
	sg_init_one(&ret, &req->ret, sizeof(req->ret));
	sgs[1] = &ret;

	spin_lock_irqsave(&vpmem->pmem_lock, flags);
	 /*
	  * If virtqueue_add_sgs returns -ENOSPC then req_vq virtual
	  * queue does not have free descriptor. We add the request
	  * to req_list and wait for host_ack to wake us up when free
	  * slots are available.
	  */
	while ((err = virtqueue_add_sgs(vpmem->req_vq, sgs, 1, 1, req,
					GFP_ATOMIC)) == -ENOSPC) {

		dev_err(&vdev->dev, "failed to send command to virtio pmem" \
			"device, no free slots in the virtqueue\n");
		req->wq_buf_avail = false;
		list_add_tail(&req->list, &vpmem->req_list);
		spin_unlock_irqrestore(&vpmem->pmem_lock, flags);

		/* A host response results in "host_ack" getting called */
		wait_event(req->wq_buf, req->wq_buf_avail);
		spin_lock_irqsave(&vpmem->pmem_lock, flags);
	}
	err1 = virtqueue_kick(vpmem->req_vq);
	spin_unlock_irqrestore(&vpmem->pmem_lock, flags);

	/*
	 * virtqueue_add_sgs failed with error different than -ENOSPC, we can't
	 * do anything about that.
	 */
	if (err || !err1) {
		dev_info(&vdev->dev, "failed to send command to virtio pmem device\n");
		err = -EIO;
	} else {
		/* A host repsonse results in "host_ack" getting called */
		wait_event(req->host_acked, req->done);
		err = req->ret;
	}

	kfree(req);
	return err;
};

/* The asynchronous flush callback function */
int async_pmem_flush(struct nd_region *nd_region, struct bio *bio)
{
	/* Create child bio for asynchronous flush and chain with
	 * parent bio. Otherwise directly call nd_region flush.
	 */
	if (bio && bio->bi_iter.bi_sector != -1) {
		struct bio *child = bio_alloc(GFP_ATOMIC, 0);

		if (!child)
			return -ENOMEM;
		bio_copy_dev(child, bio);
		child->bi_opf = REQ_PREFLUSH;
		child->bi_iter.bi_sector = -1;
		bio_chain(child, bio);
		submit_bio(child);
		return 0;
	}
	if (virtio_pmem_flush(nd_region))
		return -EIO;

	return 0;
};
EXPORT_SYMBOL_GPL(async_pmem_flush);
MODULE_LICENSE("GPL");
