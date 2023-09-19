/*
 *  mm/userfaultfd.c
 *
 *  Copyright (C) 2015  Red Hat, Inc.
 *
 *  This work is licensed under the terms of the GNU GPL, version 2. See
 *  the COPYING file in the top-level directory.
 */

#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/userfaultfd_k.h>
#include <linux/mmu_notifier.h>
#include <linux/hugetlb.h>
#include <linux/shmem_fs.h>
#include <asm/tlbflush.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/wait.h>
#include <linux/string.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/dmaengine.h>
#include "internal.h"
#include <linux/delay.h>
#include <linux/pci.h>
#include <asm/pgtable.h>
#include <linux/mutex.h>

static DECLARE_WAIT_QUEUE_HEAD(wq);
//For simplicity, request/release a fixed number of channs
struct dma_chan *chans[MAX_DMA_CHANS] = {NULL};
#define MAX_SIZE_PER_DMA_REQUEST (2*1024*1024)
u32 size_per_dma_request =  MAX_SIZE_PER_DMA_REQUEST;
u32 dma_channs = 0;

struct tx_dma_param {
    struct mutex tx_dma_mutex;
	u64 expect_count;
	volatile u64 wakeup_count;
};

/*
 * Find a valid userfault enabled VMA region that covers the whole
 * address range, or NULL on failure.  Must be called with mmap_sem
 * held.
 */
static struct vm_area_struct *vma_find_uffd(struct mm_struct *mm,
					    unsigned long start,
					    unsigned long len)
{
	struct vm_area_struct *vma = find_vma(mm, start);

	if (!vma) {
		printk("mm/userfaultfd.c: vma_find_uffd: !vma\n");
		return NULL;
	}

//	printk("mm/userfaultfd.c: vma_find_uffd: start: 0x%lx\tlen: %ld\n", start, len);
//  printk("mm/userfaultfd.c: vma_find_uffd: found vma 0x%p: 0x%lx - 0x%lx\tlen: %ld\n", vma, vma->vm_start, vma->vm_end, (vma->vm_end - vma->vm_start));
  /*
	 * Check the vma is registered in uffd, this is required to
	 * enforce the VM_MAYWRITE check done at uffd registration
	 * time.
	 */
	if (!vma->vm_userfaultfd_ctx.ctx) {
		printk("mm/userfaultfd.c: vma_find_uffd: !vma->vm_userfaultfd_ctx.ctx\n");
		return NULL;
	}

	if (start < vma->vm_start || start + len > vma->vm_end) {
		printk("mm/userfaultfd.c: vma_find_uffd: region out of vma range\n");
	  printk("mm/userfaultfd.c: vma_find_uffd: start: 0x%lx\tlen: %ld\n", start, len);
    printk("mm/userfaultfd.c: vma_find_uffd: found vma 0x%p: 0x%lx - 0x%lx\tlen: %ld\n", vma, vma->vm_start, vma->vm_end, (vma->vm_end - vma->vm_start));
		return NULL;
	}

	return vma;
}

static int mcopy_atomic_pte(struct mm_struct *dst_mm,
			    pmd_t *dst_pmd,
			    struct vm_area_struct *dst_vma,
			    unsigned long dst_addr,
			    unsigned long src_addr,
			    struct page **pagep,
			    bool wp_copy)
{
	struct mem_cgroup *memcg;
	pte_t _dst_pte, *dst_pte;
	spinlock_t *ptl;
	void *page_kaddr;
	int ret;
	struct page *page;
	pgoff_t offset, max_off;
	struct inode *inode;

	if (!*pagep) {
		ret = -ENOMEM;
		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, dst_vma, dst_addr);
		if (!page)
			goto out;

		page_kaddr = kmap_atomic(page);
		ret = copy_from_user(page_kaddr,
				     (const void __user *) src_addr,
				     PAGE_SIZE);
		kunmap_atomic(page_kaddr);

		/* fallback to copy_from_user outside mmap_sem */
		if (unlikely(ret)) {
			ret = -ENOENT;
			*pagep = page;
			/* don't free the page */
			goto out;
		}
	} else {
		page = *pagep;
		*pagep = NULL;
	}

	/*
	 * The memory barrier inside __SetPageUptodate makes sure that
	 * preceeding stores to the page contents become visible before
	 * the set_pte_at() write.
	 */
	__SetPageUptodate(page);

	ret = -ENOMEM;
	if (mem_cgroup_try_charge(page, dst_mm, GFP_KERNEL, &memcg, false))
		goto out_release;

	_dst_pte = pte_mkdirty(mk_pte(page, dst_vma->vm_page_prot));
	if (dst_vma->vm_flags & VM_WRITE) {
		if (wp_copy) {
			printk("mm/userfaultfd.c: mcopy_atomic_pte: wp_copy pte_mkuffd_wp\n");
			_dst_pte = pte_mkuffd_wp(_dst_pte);
		}
		else
			_dst_pte = pte_mkwrite(_dst_pte);
	}

	dst_pte = pte_offset_map_lock(dst_mm, dst_pmd, dst_addr, &ptl);
	if (dst_vma->vm_file) {
		/* the shmem MAP_PRIVATE case requires checking the i_size */
		inode = dst_vma->vm_file->f_inode;
		offset = linear_page_index(dst_vma, dst_addr);
		max_off = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
		ret = -EFAULT;
		if (unlikely(offset >= max_off))
			goto out_release_uncharge_unlock;
	}
	ret = -EEXIST;
	if (!pte_none(*dst_pte))
		goto out_release_uncharge_unlock;

	inc_mm_counter(dst_mm, MM_ANONPAGES);
	page_add_new_anon_rmap(page, dst_vma, dst_addr, false);
	mem_cgroup_commit_charge(page, memcg, false, false);
	lru_cache_add_active_or_unevictable(page, dst_vma);

	set_pte_at(dst_mm, dst_addr, dst_pte, _dst_pte);

	/* No need to invalidate - it was non-present before */
	update_mmu_cache(dst_vma, dst_addr, dst_pte);

	pte_unmap_unlock(dst_pte, ptl);
	ret = 0;
out:
	return ret;
out_release_uncharge_unlock:
	pte_unmap_unlock(dst_pte, ptl);
	mem_cgroup_cancel_charge(page, memcg, false);
out_release:
	put_page(page);
	goto out;
}

static int mfill_zeropage_pte(struct mm_struct *dst_mm,
			      pmd_t *dst_pmd,
			      struct vm_area_struct *dst_vma,
			      unsigned long dst_addr)
{
	pte_t _dst_pte, *dst_pte;
	spinlock_t *ptl;
	int ret;
	pgoff_t offset, max_off;
	struct inode *inode;

	_dst_pte = pte_mkspecial(pfn_pte(my_zero_pfn(dst_addr),
					 dst_vma->vm_page_prot));
	dst_pte = pte_offset_map_lock(dst_mm, dst_pmd, dst_addr, &ptl);
	if (dst_vma->vm_file) {
		/* the shmem MAP_PRIVATE case requires checking the i_size */
		inode = dst_vma->vm_file->f_inode;
		offset = linear_page_index(dst_vma, dst_addr);
		max_off = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
		ret = -EFAULT;
		if (unlikely(offset >= max_off))
			goto out_unlock;
	}
	ret = -EEXIST;
	if (!pte_none(*dst_pte))
		goto out_unlock;
	set_pte_at(dst_mm, dst_addr, dst_pte, _dst_pte);
	/* No need to invalidate - it was non-present before */
	update_mmu_cache(dst_vma, dst_addr, dst_pte);
	ret = 0;
out_unlock:
	pte_unmap_unlock(dst_pte, ptl);
	return ret;
}

static pmd_t *mm_alloc_pmd(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(mm, address);
	p4d = p4d_alloc(mm, pgd, address);
	if (!p4d)
		return NULL;
	pud = pud_alloc(mm, p4d, address);
	if (!pud)
		return NULL;
	/*
	 * Note that we didn't run this because the pmd was
	 * missing, the *pmd may be already established and in
	 * turn it may also be a trans_huge_pmd.
	 */
	return pmd_alloc(mm, pud, address);
}

#ifdef CONFIG_HUGETLB_PAGE
/*
 * __mcopy_atomic processing for HUGETLB vmas.  Note that this routine is
 * called with mmap_sem held, it will release mmap_sem before returning.
 */
static __always_inline ssize_t __mcopy_atomic_hugetlb(struct mm_struct *dst_mm,
					      struct vm_area_struct *dst_vma,
					      unsigned long dst_start,
					      unsigned long src_start,
					      unsigned long len,
					      bool zeropage)
{
	int vm_alloc_shared = dst_vma->vm_flags & VM_SHARED;
	int vm_shared = dst_vma->vm_flags & VM_SHARED;
	ssize_t err;
	pte_t *dst_pte;
	unsigned long src_addr, dst_addr;
	long copied;
	struct page *page;
	struct hstate *h;
	unsigned long vma_hpagesize;
	pgoff_t idx;
	u32 hash;
	struct address_space *mapping;

	/*
	 * There is no default zero huge page for all huge page sizes as
	 * supported by hugetlb.  A PMD_SIZE huge pages may exist as used
	 * by THP.  Since we can not reliably insert a zero page, this
	 * feature is not supported.
	 */
	if (zeropage) {
		up_read(&dst_mm->mmap_sem);
		return -EINVAL;
	}

	src_addr = src_start;
	dst_addr = dst_start;
	copied = 0;
	page = NULL;
	vma_hpagesize = vma_kernel_pagesize(dst_vma);

	/*
	 * Validate alignment based on huge page size
	 */
	err = -EINVAL;
	if (dst_start & (vma_hpagesize - 1) || len & (vma_hpagesize - 1))
		goto out_unlock;

retry:
	/*
	 * On routine entry dst_vma is set.  If we had to drop mmap_sem and
	 * retry, dst_vma will be set to NULL and we must lookup again.
	 */
	if (!dst_vma) {
		err = -ENOENT;
		dst_vma = vma_find_uffd(dst_mm, dst_start, len);
		if (!dst_vma || !is_vm_hugetlb_page(dst_vma))
			goto out_unlock;

		err = -EINVAL;
		if (vma_hpagesize != vma_kernel_pagesize(dst_vma))
			goto out_unlock;

		vm_shared = dst_vma->vm_flags & VM_SHARED;
	}

	if (WARN_ON(dst_addr & (vma_hpagesize - 1) ||
		    (len - copied) & (vma_hpagesize - 1)))
		goto out_unlock;

	/*
	 * If not shared, ensure the dst_vma has a anon_vma.
	 */
	err = -ENOMEM;
	if (!vm_shared) {
		if (unlikely(anon_vma_prepare(dst_vma)))
			goto out_unlock;
	}

	h = hstate_vma(dst_vma);

	while (src_addr < src_start + len) {
		pte_t dst_pteval;

		BUG_ON(dst_addr >= dst_start + len);
		VM_BUG_ON(dst_addr & ~huge_page_mask(h));

		/*
		 * Serialize via hugetlb_fault_mutex
		 */
		idx = linear_page_index(dst_vma, dst_addr);
		mapping = dst_vma->vm_file->f_mapping;
		hash = hugetlb_fault_mutex_hash(h, dst_mm, dst_vma, mapping,
								idx, dst_addr);
		mutex_lock(&hugetlb_fault_mutex_table[hash]);

		err = -ENOMEM;
		dst_pte = huge_pte_alloc(dst_mm, dst_addr, huge_page_size(h));
		if (!dst_pte) {
			mutex_unlock(&hugetlb_fault_mutex_table[hash]);
			goto out_unlock;
		}

		err = -EEXIST;
		dst_pteval = huge_ptep_get(dst_pte);
		if (!huge_pte_none(dst_pteval)) {
			mutex_unlock(&hugetlb_fault_mutex_table[hash]);
			goto out_unlock;
		}

		err = hugetlb_mcopy_atomic_pte(dst_mm, dst_pte, dst_vma,
						dst_addr, src_addr, &page);

		mutex_unlock(&hugetlb_fault_mutex_table[hash]);
		vm_alloc_shared = vm_shared;

		cond_resched();

		if (unlikely(err == -ENOENT)) {
			up_read(&dst_mm->mmap_sem);
			BUG_ON(!page);

			err = copy_huge_page_from_user(page,
						(const void __user *)src_addr,
						pages_per_huge_page(h), true);
			if (unlikely(err)) {
				err = -EFAULT;
				goto out;
			}
			down_read(&dst_mm->mmap_sem);

			dst_vma = NULL;
			goto retry;
		} else
			BUG_ON(page);

		if (!err) {
			dst_addr += vma_hpagesize;
			src_addr += vma_hpagesize;
			copied += vma_hpagesize;

			if (fatal_signal_pending(current))
				err = -EINTR;
		}
		if (err)
			break;
	}

out_unlock:
	up_read(&dst_mm->mmap_sem);
out:
	if (page) {
		/*
		 * We encountered an error and are about to free a newly
		 * allocated huge page.
		 *
		 * Reservation handling is very subtle, and is different for
		 * private and shared mappings.  See the routine
		 * restore_reserve_on_error for details.  Unfortunately, we
		 * can not call restore_reserve_on_error now as it would
		 * require holding mmap_sem.
		 *
		 * If a reservation for the page existed in the reservation
		 * map of a private mapping, the map was modified to indicate
		 * the reservation was consumed when the page was allocated.
		 * We clear the PagePrivate flag now so that the global
		 * reserve count will not be incremented in free_huge_page.
		 * The reservation map will still indicate the reservation
		 * was consumed and possibly prevent later page allocation.
		 * This is better than leaking a global reservation.  If no
		 * reservation existed, it is still safe to clear PagePrivate
		 * as no adjustments to reservation counts were made during
		 * allocation.
		 *
		 * The reservation map for shared mappings indicates which
		 * pages have reservations.  When a huge page is allocated
		 * for an address with a reservation, no change is made to
		 * the reserve map.  In this case PagePrivate will be set
		 * to indicate that the global reservation count should be
		 * incremented when the page is freed.  This is the desired
		 * behavior.  However, when a huge page is allocated for an
		 * address without a reservation a reservation entry is added
		 * to the reservation map, and PagePrivate will not be set.
		 * When the page is freed, the global reserve count will NOT
		 * be incremented and it will appear as though we have leaked
		 * reserved page.  In this case, set PagePrivate so that the
		 * global reserve count will be incremented to match the
		 * reservation map entry which was created.
		 *
		 * Note that vm_alloc_shared is based on the flags of the vma
		 * for which the page was originally allocated.  dst_vma could
		 * be different or NULL on error.
		 */
		if (vm_alloc_shared)
			SetPagePrivate(page);
		else
			ClearPagePrivate(page);
		put_page(page);
	}
	BUG_ON(copied < 0);
	BUG_ON(err > 0);
	BUG_ON(!copied && !err);
	return copied ? copied : err;
}
#else /* !CONFIG_HUGETLB_PAGE */
/* fail at build time if gcc attempts to use this */
extern ssize_t __mcopy_atomic_hugetlb(struct mm_struct *dst_mm,
				      struct vm_area_struct *dst_vma,
				      unsigned long dst_start,
				      unsigned long src_start,
				      unsigned long len,
				      bool zeropage);
#endif /* CONFIG_HUGETLB_PAGE */

static __always_inline ssize_t mfill_atomic_pte(struct mm_struct *dst_mm,
						pmd_t *dst_pmd,
						struct vm_area_struct *dst_vma,
						unsigned long dst_addr,
						unsigned long src_addr,
						struct page **page,
						bool zeropage,
						bool wp_copy)
{
	ssize_t err;

	/*
	 * The normal page fault path for a shmem will invoke the
	 * fault, fill the hole in the file and COW it right away. The
	 * result generates plain anonymous memory. So when we are
	 * asked to fill an hole in a MAP_PRIVATE shmem mapping, we'll
	 * generate anonymous memory directly without actually filling
	 * the hole. For the MAP_PRIVATE case the robustness check
	 * only happens in the pagetable (to verify it's still none)
	 * and not in the radix tree.
	 */
	if (!(dst_vma->vm_flags & VM_SHARED)) {
		if (!zeropage)
			err = mcopy_atomic_pte(dst_mm, dst_pmd, dst_vma,
					       dst_addr, src_addr, page,
					       wp_copy);
		else
			err = mfill_zeropage_pte(dst_mm, dst_pmd,
						 dst_vma, dst_addr);
	} else {
		VM_WARN_ON_ONCE(wp_copy);
		if (!zeropage)
			err = shmem_mcopy_atomic_pte(dst_mm, dst_pmd,
						     dst_vma, dst_addr,
						     src_addr, page);
		else
			err = shmem_mfill_zeropage_pte(dst_mm, dst_pmd,
						       dst_vma, dst_addr);
	}

	return err;
}

static __always_inline ssize_t __mcopy_atomic(struct mm_struct *dst_mm,
					      unsigned long dst_start,
					      unsigned long src_start,
					      unsigned long len,
					      bool zeropage,
					      bool *mmap_changing,
					      __u64 mode)
{
	struct vm_area_struct *dst_vma;
	ssize_t err;
	pmd_t *dst_pmd;
	unsigned long src_addr, dst_addr;
	long copied;
	struct page *page;
	bool wp_copy;

	/*
	 * Sanitize the command parameters:
	 */
	BUG_ON(dst_start & ~PAGE_MASK);
	BUG_ON(len & ~PAGE_MASK);

	/* Does the address range wrap, or is the span zero-sized? */
	BUG_ON(src_start + len <= src_start);
	BUG_ON(dst_start + len <= dst_start);

	src_addr = src_start;
	dst_addr = dst_start;
	copied = 0;
	page = NULL;
retry:
	down_read(&dst_mm->mmap_sem);

	/*
	 * If memory mappings are changing because of non-cooperative
	 * operation (e.g. mremap) running in parallel, bail out and
	 * request the user to retry later
	 */
	err = -EAGAIN;
	if (mmap_changing && READ_ONCE(*mmap_changing))
		goto out_unlock;

	/*
	 * Make sure the vma is not shared, that the dst range is
	 * both valid and fully within a single existing vma.
	 */
	err = -ENOENT;
	dst_vma = vma_find_uffd(dst_mm, dst_start, len);
	if (!dst_vma)
		goto out_unlock;

	err = -EINVAL;
	/*
	 * shmem_zero_setup is invoked in mmap for MAP_ANONYMOUS|MAP_SHARED but
	 * it will overwrite vm_ops, so vma_is_anonymous must return false.
	 */
	if (WARN_ON_ONCE(vma_is_anonymous(dst_vma) &&
	    dst_vma->vm_flags & VM_SHARED))
		goto out_unlock;

	/*
	 * validate 'mode' now that we know the dst_vma: don't allow
	 * a wrprotect copy if the userfaultfd didn't register as WP.
	 */
	wp_copy = mode & UFFDIO_COPY_MODE_WP;
	if (wp_copy && !(dst_vma->vm_flags & VM_UFFD_WP))
		goto out_unlock;

	/*
	 * If this is a HUGETLB vma, pass off to appropriate routine
	 */
	if (is_vm_hugetlb_page(dst_vma))
		return  __mcopy_atomic_hugetlb(dst_mm, dst_vma, dst_start,
						src_start, len, zeropage);

	if (!vma_is_anonymous(dst_vma) && !vma_is_shmem(dst_vma))
		goto out_unlock;

	/*
	 * Ensure the dst_vma has a anon_vma or this page
	 * would get a NULL anon_vma when moved in the
	 * dst_vma.
	 */
	err = -ENOMEM;
	if (!(dst_vma->vm_flags & VM_SHARED) &&
	    unlikely(anon_vma_prepare(dst_vma)))
		goto out_unlock;

	while (src_addr < src_start + len) {
		pmd_t dst_pmdval;

		BUG_ON(dst_addr >= dst_start + len);

		dst_pmd = mm_alloc_pmd(dst_mm, dst_addr);
		if (unlikely(!dst_pmd)) {
			err = -ENOMEM;
			break;
		}

		dst_pmdval = pmd_read_atomic(dst_pmd);
		/*
		 * If the dst_pmd is mapped as THP don't
		 * override it and just be strict.
		 */
		if (unlikely(pmd_trans_huge(dst_pmdval))) {
			err = -EEXIST;
			break;
		}
		if (unlikely(pmd_none(dst_pmdval)) &&
		    unlikely(__pte_alloc(dst_mm, dst_pmd))) {
			err = -ENOMEM;
			break;
		}
		/* If an huge pmd materialized from under us fail */
		if (unlikely(pmd_trans_huge(*dst_pmd))) {
			err = -EFAULT;
			break;
		}

		BUG_ON(pmd_none(*dst_pmd));
		BUG_ON(pmd_trans_huge(*dst_pmd));

		err = mfill_atomic_pte(dst_mm, dst_pmd, dst_vma, dst_addr,
				       src_addr, &page, zeropage, wp_copy);
		cond_resched();

		if (unlikely(err == -ENOENT)) {
			void *page_kaddr;

			up_read(&dst_mm->mmap_sem);
			BUG_ON(!page);

			page_kaddr = kmap(page);
			err = copy_from_user(page_kaddr,
					     (const void __user *) src_addr,
					     PAGE_SIZE);
			kunmap(page);
			if (unlikely(err)) {
				err = -EFAULT;
				goto out;
			}
			goto retry;
		} else
			BUG_ON(page);

		if (!err) {
			dst_addr += PAGE_SIZE;
			src_addr += PAGE_SIZE;
			copied += PAGE_SIZE;

			if (fatal_signal_pending(current))
				err = -EINTR;
		}
		if (err)
			break;
	}

out_unlock:
	up_read(&dst_mm->mmap_sem);
out:
	if (page)
		put_page(page);
	BUG_ON(copied < 0);
	BUG_ON(err > 0);
	BUG_ON(!copied && !err);
	return copied ? copied : err;
}

static void hemem_dma_tx_callback(void *dma_async_param)
{
	struct tx_dma_param *tx_dma_param = (struct tx_dma_param*)dma_async_param;
    struct mutex *tx_dma_mutex = &(tx_dma_param->tx_dma_mutex);
    mutex_lock(tx_dma_mutex);
	(tx_dma_param->wakeup_count)++;
	if (tx_dma_param->wakeup_count < tx_dma_param->expect_count) {
        mutex_unlock(tx_dma_mutex);
		return;
	}
    mutex_unlock(tx_dma_mutex);
	wake_up_interruptible(&wq);
}

static int bad_address(void *p)
{
	unsigned long dummy;

	return probe_kernel_address((unsigned long *)p, dummy);
}

static void  page_walk(u64 address, u64* phy_addr)
{

	pgd_t *base = __va(read_cr3_pa());
	pgd_t *pgd = base + pgd_index(address);
        p4d_t *p4d;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *pte;
	u64 page_addr;
	u64 page_offset;
 	int present = 0;	
	int write = 0;

	if (bad_address(pgd))
		goto out;

	if (!pgd_present(*pgd))
		goto out;

	p4d = p4d_offset(pgd, address);
	if (bad_address(p4d))
		goto out;

	if (!p4d_present(*p4d) || p4d_large(*p4d))
		goto out;

	pud = pud_offset(p4d, address);
	if (bad_address(pud))
		goto out;

	if (!pud_present(*pud) || pud_large(*pud))
		goto out;

	pmd = pmd_offset(pud, address);
	if (bad_address(pmd))
		goto out;

    if (pmd_large(*pmd)) {
	    page_addr = pmd_val(*pmd) & 0x000FFFFFFFE00000;
	    page_offset = address & ~HPAGE_MASK;
    }
    else {
        if (!pmd_present(*pmd))
            goto out;

        pte = pte_offset_kernel(pmd, address);
        if (bad_address(pte))
            goto out;

        present = pte_present(*pte);
        write = pte_write(*pte);
        
        if (present != 1 || write == 0) {
            printk("in page_walk, address=%llu, present=%d, write=%d\n", address, present, write); 
        }

        page_addr = pte_val(*pte) & 0x000FFFFFFFFFF000;
        page_offset = address & ~PAGE_MASK;
    }

	*phy_addr = page_addr | page_offset;
    return;

out:
	pr_info("BAD\n");
}

int dma_request_channs(struct uffdio_dma_channs* uffdio_dma_channs)
{
    struct dma_chan *chan = NULL;
    dma_cap_mask_t mask;
    int index;
    int num_channs;

    if (uffdio_dma_channs == NULL) {
        return -1;
    }

    num_channs = uffdio_dma_channs->num_channs;
    if (num_channs > MAX_DMA_CHANS) {
        num_channs = MAX_DMA_CHANS;
    }

    size_per_dma_request = uffdio_dma_channs->size_per_dma_request;
    if (size_per_dma_request > MAX_SIZE_PER_DMA_REQUEST) {
        size_per_dma_request = MAX_SIZE_PER_DMA_REQUEST;
    }

    dma_cap_zero(mask);
    dma_cap_set(DMA_MEMCPY, mask);
    for (index = 0; index < num_channs; index++) {
        if (chans[index]) {
            continue;
        }

        chan = dma_request_channel(mask, NULL, NULL);
        if (chan == NULL) {
            printk("wei: error when dma_request_channel, index=%d, num_channs=%u\n", index, num_channs);
            goto out;
        }

        chans[index] = chan;
    }

    dma_channs = num_channs;
    return 0;
out:
    while (index >= 0) {
        if (chans[index]) {
            dma_release_channel(chans[index]);
            chans[index] = NULL;
        }
    }

    return -1;
}

int dma_release_channs(void)
{
    int index;

    for (index = 0; index < dma_channs; index++) {
        if (chans[index]) {
            dma_release_channel(chans[index]);
            chans[index] = NULL;
        }
    }

    dma_channs = 0;
    size_per_dma_request = MAX_SIZE_PER_DMA_REQUEST;
    return 0;
}

static __always_inline ssize_t __dma_mcopy_pages(struct mm_struct *dst_mm,
					      struct uffdio_dma_copy *uffdio_dma_copy,
					      bool *mmap_changing)
{
	struct vm_area_struct *dst_vma;
	ssize_t err;
	long copied = 0;
	bool wp_copy;
	u64 src_start;
	u64 dst_start;
    u64 src_cur;
    u64 dst_cur;
	dma_addr_t src_phys;
	dma_addr_t dst_phys;
	u64 len;
    u64 len_cur;
	struct dma_chan *chan = NULL;
	dma_cap_mask_t mask;
	struct dma_async_tx_descriptor *tx = NULL;
	dma_cookie_t dma_cookie;
	struct dma_device *dma;
	struct device *dev;
	struct mm_struct *mm = current->mm;
	int present;
    int index = 0;
	u64 count = 0;
    u64 expect_count = 0;
    static u64 dma_assign_index = 0;
	struct tx_dma_param tx_dma_param;
    u64 dma_len = 0;
    u64 start, end;
    u64 start_walk, end_walk;
    u64 start_copy, end_copy;

    #ifdef DEBUG_TM
    start = rdtsc();
    #endif
	down_read(&dst_mm->mmap_sem);
    /*
	 * If memory mappings are changing because of non-cooperative
	 * operation (e.g. mremap) running in parallel, bail out and
	 * request the user to retry later
	 */
	err = -EAGAIN;
	if (mmap_changing && READ_ONCE(*mmap_changing))
		goto out_unlock;

	BUG_ON(uffdio_dma_copy == NULL);
	count = uffdio_dma_copy->count;
    for (index = 0; index < count; index++) {
        if (uffdio_dma_copy->len[index] % size_per_dma_request == 0) {
            expect_count += uffdio_dma_copy->len[index] / size_per_dma_request;
        }
        else {
            expect_count += uffdio_dma_copy->len[index] / size_per_dma_request + 1;
        }
    }

	tx_dma_param.wakeup_count = 0;
	tx_dma_param.expect_count = expect_count;
    mutex_init(&(tx_dma_param.tx_dma_mutex));
        
    for (index  = 0; index < count; index++) {
		dst_start = uffdio_dma_copy->dst[index];
		src_start = uffdio_dma_copy->src[index];
		len = uffdio_dma_copy->len[index];

		/*
		 * Sanitize the command parameters:
		 */
		BUG_ON(dst_start & ~PAGE_MASK);
		BUG_ON(len & ~PAGE_MASK);

		/* Does the address range wrap, or is the span zero-sized? */
		BUG_ON(src_start + len <= src_start);
		BUG_ON(dst_start + len <= dst_start);

        #ifdef DEBUG_TM
        start_walk = rdtsc();
        #endif
        page_walk(src_start, &src_phys);
        page_walk(dst_start, &dst_phys);
        #ifdef DEBUG_TM
        end_walk = rdtsc();
        start_copy = rdtsc();
        #endif
        for (src_cur = src_start, dst_cur = dst_start, len_cur = 0; len_cur < len;) {
            err = 0;
		    chan = chans[dma_assign_index++ % dma_channs];
            if (len_cur + size_per_dma_request > len) {
                dma_len = len - len_cur; 
            }
            else {
                dma_len = size_per_dma_request;
            }

            tx = dmaengine_prep_dma_memcpy(chan, dst_phys, src_phys, dma_len, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
            if (tx == NULL) {
                printk("wei: error when dmaengine_prep_dma_memcpy, dma_chans=%d\n", dma_channs);
                goto out_unlock;
            }

            tx->callback = hemem_dma_tx_callback;
            tx->callback_param = &tx_dma_param;
            tx->cookie = chan->cookie;
            dma_cookie = dmaengine_submit(tx);
            if (dma_submit_error(dma_cookie)) {
                printk("wei: Failed to do DMA tx_submit\n");
                goto out_unlock;
            }

            dma_async_issue_pending(chan);
            len_cur += dma_len;
            src_cur += dma_len;
            dst_cur += dma_len;
            src_phys += dma_len;
            dst_phys += dma_len;
        }
	}

	wait_event_interruptible(wq, tx_dma_param.wakeup_count >= tx_dma_param.expect_count);
    #ifdef DEBUG_TM
    end_copy = rdtsc();
    #endif
	for (index = 0; index < count; index++) {
		copied += uffdio_dma_copy->len[index];
	}

out_unlock:
	up_read(&dst_mm->mmap_sem);
out:
   	BUG_ON(copied < 0);
	BUG_ON(err > 0);
	BUG_ON(!copied && !err);
    #ifdef DEBUG_TM
    end = rdtsc();
    printk("dma_memcpy_fun: %llu, page_walk: %llu, only_dma_op:%llu\n", end - start, end_copy - start_copy, end_walk - start_walk);
    #endif
	return copied ? copied : err;
}

ssize_t mcopy_atomic(struct mm_struct *dst_mm, unsigned long dst_start,
		     unsigned long src_start, unsigned long len,
		     bool *mmap_changing, __u64 mode)
{
	return __mcopy_atomic(dst_mm, dst_start, src_start, len, false,
			      mmap_changing, mode);
}

ssize_t dma_mcopy_pages(struct mm_struct *dst_mm,
		     struct uffdio_dma_copy *uffdio_dma_copy,
		     bool *mmap_changing)
{
	return __dma_mcopy_pages(dst_mm, 
			      uffdio_dma_copy,
			      mmap_changing);
}

ssize_t mfill_zeropage(struct mm_struct *dst_mm, unsigned long start,
		       unsigned long len, bool *mmap_changing)
{
	return __mcopy_atomic(dst_mm, start, 0, len, true, mmap_changing, 0);
}

int mwriteprotect_range(struct mm_struct *dst_mm, unsigned long start,
			unsigned long len, bool enable_wp, bool *mmap_changing)
{
	struct vm_area_struct *dst_vma;
	pgprot_t newprot;
	int err;

	/*
	 * Sanitize the command parameters:
	 */
	BUG_ON(start & ~PAGE_MASK);
	BUG_ON(len & ~PAGE_MASK);

	/* Does the address range wrap, or is the span zero-sized? */
	BUG_ON(start + len <= start);

	down_read(&dst_mm->mmap_sem);

	/*
	 * If memory mappings are changing because of non-cooperative
	 * operation (e.g. mremap) running in parallel, bail out and
	 * request the user to retry later
	 */
	err = -EAGAIN;
	if (mmap_changing && READ_ONCE(*mmap_changing))
		goto out_unlock;

	err = -ENOENT;
	dst_vma = vma_find_uffd(dst_mm, start, len);
	/*
	 * Make sure the vma is not shared, that the dst range is
	 * both valid and fully within a single existing vma.
	 */
	if (!dst_vma || ((dst_vma->vm_flags & VM_SHARED) && !vma_is_dax(dst_vma))) {
		printk("mm/userfaultfd.c: mwriteprotect_range: dst_vma is null\n");
		goto out_unlock;
	}
	if (!userfaultfd_wp(dst_vma)) {
		printk("mm/userfaultfd.c: mwriteprotect_range: dst_vma is not userfaultfd_wp\n");
		goto out_unlock;
	}
	if (!(vma_is_anonymous(dst_vma) || vma_is_dax(dst_vma))) {
		printk("mm/userfaultfd.c: mwriteprotect_range: dst_vma is not anonymous or dax\n");
		goto out_unlock;
	}

	if (enable_wp)
		newprot = vm_get_page_prot(dst_vma->vm_flags & ~(VM_WRITE));
	else
		newprot = vm_get_page_prot(dst_vma->vm_flags);

	//printk("mm/userfaultfd.c: mwriteprotect_range: changing protection: enable_wp: [%d] is vm hugetlb_page: [%d]\n", enable_wp, is_vm_hugetlb_page(dst_vma));
	change_protection(dst_vma, start, start + len, newprot,
			  enable_wp ? MM_CP_UFFD_WP : MM_CP_UFFD_WP_RESOLVE);

	err = 0;
out_unlock:
	up_read(&dst_mm->mmap_sem);
	return err;
}
