// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)

#include <linux/module.h>
#include <linux/ntb.h>
#include <linux/msi.h>
#include <linux/pci.h>

MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("0.1");
MODULE_AUTHOR("Logan Gunthorpe <logang@deltatee.com>");
MODULE_DESCRIPTION("NTB MSI Interrupt Library");

struct ntb_msi {
	u64 base_addr;
	u64 end_addr;
	u32 *peer_mws[];
};

/**
 * ntb_msi_init() - Initialize the MSI context
 * @ntb:	NTB device context.
 *
 * This function must be called before any other ntb_msi function.
 * It initializes the context for MSI operations and maps
 * the peer memory windows.
 *
 * This function reserves the last N outbound memory windows (where N
 * is the number of peers).
 *
 * Return: Zero on success, otherwise a negative error number.
 */
int ntb_msi_init(struct ntb_dev *ntb)
{
	phys_addr_t mw_phys_addr;
	resource_size_t mw_size;
	size_t struct_size;
	int peer_widx;
	int peers;
	int ret;
	int i;

	peers = ntb_peer_port_count(ntb);
	if (peers <= 0)
		return -EINVAL;

	struct_size = sizeof(*ntb->msi) + sizeof(*ntb->msi->peer_mws) * peers;

	ntb->msi = devm_kzalloc(&ntb->dev, struct_size, GFP_KERNEL);
	if (!ntb->msi)
		return -ENOMEM;

	for (i = 0; i < peers; i++) {
		peer_widx = ntb_peer_mw_count(ntb) - 1 - i;

		ret = ntb_peer_mw_get_addr(ntb, peer_widx, &mw_phys_addr,
					   &mw_size);
		if (ret)
			goto unroll;

		ntb->msi->peer_mws[i] = devm_ioremap(&ntb->dev, mw_phys_addr,
						     mw_size);
		if (!ntb->msi->peer_mws[i]) {
			ret = -EFAULT;
			goto unroll;
		}
	}

	return 0;

unroll:
	for (i = 0; i < peers; i++)
		if (ntb->msi->peer_mws[i])
			devm_iounmap(&ntb->dev, ntb->msi->peer_mws[i]);

	devm_kfree(&ntb->dev, ntb->msi);
	ntb->msi = NULL;
	return ret;
}
EXPORT_SYMBOL(ntb_msi_init);

/**
 * ntb_msi_setup_mws() - Initialize the MSI inbound memory windows
 * @ntb:	NTB device context.
 *
 * This function sets up the required inbound memory windows. It should be
 * called from a work function after a link up event.
 *
 * Over the entire network, this function will reserves the last N
 * inbound memory windows for each peer (where N is the number of peers).
 *
 * ntb_msi_init() must be called before this function.
 *
 * Return: Zero on success, otherwise a negative error number.
 */
int ntb_msi_setup_mws(struct ntb_dev *ntb)
{
	struct msi_desc *desc;
	u64 addr;
	int peer, peer_widx;
	resource_size_t addr_align, size_align, size_max;
	resource_size_t mw_size = SZ_32K;
	resource_size_t mw_min_size = mw_size;
	int i;
	int ret;

	if (!ntb->msi)
		return -EINVAL;

	desc = first_msi_entry(&ntb->pdev->dev);
	addr = desc->msg.address_lo + ((uint64_t)desc->msg.address_hi << 32);

	for (peer = 0; peer < ntb_peer_port_count(ntb); peer++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0)
			return peer_widx;

		ret = ntb_mw_get_align(ntb, peer, peer_widx, &addr_align,
				       NULL, NULL);
		if (ret)
			return ret;

		addr &= ~(addr_align - 1);
	}

	for (peer = 0; peer < ntb_peer_port_count(ntb); peer++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0) {
			ret = peer_widx;
			goto error_out;
		}

		ret = ntb_mw_get_align(ntb, peer, peer_widx, NULL,
				       &size_align, &size_max);
		if (ret)
			goto error_out;

		mw_size = round_up(mw_size, size_align);
		mw_size = max(mw_size, size_max);
		if (mw_size < mw_min_size)
			mw_min_size = mw_size;

		ret = ntb_mw_set_trans(ntb, peer, peer_widx,
				       addr, mw_size);
		if (ret)
			goto error_out;
	}

	ntb->msi->base_addr = addr;
	ntb->msi->end_addr = addr + mw_min_size;

	return 0;

error_out:
	for (i = 0; i < peer; i++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0)
			continue;

		ntb_mw_clear_trans(ntb, i, peer_widx);
	}

	return ret;
}
EXPORT_SYMBOL(ntb_msi_setup_mws);

/**
 * ntb_msi_clear_mws() - Clear all inbound memory windows
 * @ntb:	NTB device context.
 *
 * This function tears down the resources used by ntb_msi_setup_mws().
 */
void ntb_msi_clear_mws(struct ntb_dev *ntb)
{
	int peer;
	int peer_widx;

	for (peer = 0; peer < ntb_peer_port_count(ntb); peer++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0)
			continue;

		ntb_mw_clear_trans(ntb, peer, peer_widx);
	}
}
EXPORT_SYMBOL(ntb_msi_clear_mws);

/**
 * ntbm_msi_request_threaded_irq() - allocate an MSI interrupt
 * @ntb:	NTB device context.
 * @handler:	Function to be called when the IRQ occurs
 * @thread_fn:  Function to be called in a threaded interrupt context. NULL
 *              for clients which handle everything in @handler
 * @devname:    An ascii name for the claiming device, dev_name(dev) if NULL
 * @dev_id:     A cookie passed back to the handler function
 *
 * This function assigns an interrupt handler to an unused
 * MSI interrupt and returns the descriptor used to trigger
 * it. The descriptor can then be sent to a peer to trigger
 * the interrupt.
 *
 * The interrupt resource is managed with devres so it will
 * be automatically freed when the ntb device is torn down.
 *
 * Return: IRQ number assigned on success, otherwise a negative error number.
 */
int ntbm_msi_request_threaded_irq(struct ntb_dev *ntb, irq_handler_t handler,
				  irq_handler_t thread_fn,
				  const char *name, void *dev_id,
				  struct ntb_msi_desc *msi_desc)
{
	struct msi_desc *entry;
	struct irq_desc *desc;
	u64 addr;
	int ret;

	if (!ntb->msi)
		return -EINVAL;

	for_each_pci_msi_entry(entry, ntb->pdev) {
		desc = irq_to_desc(entry->irq);
		if (desc->action)
			continue;

		ret = devm_request_threaded_irq(&ntb->dev, entry->irq, handler,
						thread_fn, 0, name, dev_id);
		if (ret)
			continue;

		addr = entry->msg.address_lo +
			((uint64_t)entry->msg.address_hi << 32);

		if (addr < ntb->msi->base_addr || addr >= ntb->msi->end_addr) {
			devm_free_irq(&ntb->dev, entry->irq, dev_id);
			dev_warn(&ntb->dev,
				 "IRQ %d: MSI Address not within the memory window (%llx, [%llx %llx])\n",
				 entry->irq, addr, ntb->msi->base_addr,
				 ntb->msi->end_addr);
			continue;
		}

		msi_desc->addr_offset = addr - ntb->msi->base_addr;
		msi_desc->data = entry->msg.data;

		dev_dbg(&ntb->dev, "Assigned IRQ %d\n", entry->irq);

		return entry->irq;
	}

	return -ENODEV;
}
EXPORT_SYMBOL(ntbm_msi_request_threaded_irq);

/**
 * ntb_msi_peer_trigger() - Trigger an interrupt handler on a peer
 * @ntb:	NTB device context.
 * @peer:	Peer index
 * @desc:	MSI descriptor data which triggers the interrupt
 *
 * This function triggers an interrupt on a peer. It requires
 * the descriptor structure to have been passed from that peer
 * by some other means.
 *
 * Return: Zero on success, otherwise a negative error number.
 */
int ntb_msi_peer_trigger(struct ntb_dev *ntb, int peer,
			 struct ntb_msi_desc *desc)
{
	int idx;

	if (!ntb->msi)
		return -EINVAL;

	idx = desc->addr_offset / sizeof(*ntb->msi->peer_mws[peer]);

	ntb->msi->peer_mws[peer][idx] = desc->data;

	return 0;
}
EXPORT_SYMBOL(ntb_msi_peer_trigger);

/**
 * ntb_msi_peer_addr() - Get the DMA address to trigger a peers MSI interrupt
 * @ntb:	NTB device context.
 * @peer:	Peer index
 * @desc:	MSI descriptor data which triggers the interrupt
 * @msi_addr:   Physical address to trigger the interrupt
 *
 * This function allows using DMA engines to trigger an interrupt
 * (for example, trigger an interrupt to process the data after
 * sending it). To trigger the interupt write @desc.data to the address
 * returned in @msi_addr
 *
 * Return: Zero on success, otherwise a negative error number.
 */
int ntb_msi_peer_addr(struct ntb_dev *ntb, int peer,
		      struct ntb_msi_desc *desc,
		      phys_addr_t *msi_addr)
{
	int peer_widx = ntb_peer_mw_count(ntb) - 1 - peer;
	phys_addr_t mw_phys_addr;
	int ret;

	ret = ntb_peer_mw_get_addr(ntb, peer_widx, &mw_phys_addr, NULL);
	if (ret)
		return ret;

	if (msi_addr)
		*msi_addr = mw_phys_addr + desc->addr_offset;

	return 0;
}
EXPORT_SYMBOL(ntb_msi_peer_addr);
