/*
 * This software program is licensed subject to the GNU General Public License
 * (GPL).Version 2,June 1991, available at http://www.fsf.org/copyleft/gpl.html

 * (C) Copyright 2014 Marvell International Ltd.
 * All Rights Reserved
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/aio.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/moduleparam.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/regs-addr.h>
#include <linux/pm_wakeup.h>

#include <linux/pxa9xx_amipc.h>

#define pmu_readl(off)	__raw_readl(amipc->pmu_base + (off))
#define pmu_writel(off, v)	__raw_writel((v), amipc->pmu_base + (off))
#define ciu_readl(off)	__raw_readl(amipc->ciu_base + (off))
#define ciu_writel(off, v)	__raw_writel((v), amipc->ciu_base + (off))

#define E_TO_OFF(v) ((v) - 1)
#define OFF_TO_E(v) ((v) + 1)

/* APMU register */
#define GNSS_WAKEUP_CTRL	(0x1B8)

/* CIU register */
#define GNSS_HANDSHAKE		(0x168)

/* GNSS_WAKEUP_CTRL */
#define GNSS_WAKEUP_STATUS	(1 << 2)
#define GNSS_WAKEUP_CLR		(1 << 1)
#define GNSS_WAKEUP_EN		(1 << 0)

/* GNSS_HANDSHAKE */
#define GNSS_IRQ_OUT_ST		(1 << 4)
#define GNSS_CODE_INIT_RDY	(1 << 3)
#define GNSS_CODE_INIT_DONE	(1 << 2)
#define AP_GNSS_WAKEUP		(1 << 1)
#define GNSS_IRQ_CLR		(1 << 0)

#define MSG_BUFFER_LEN		(16)

struct ipc_event_type {
	enum amipc_events event;
	u32 data1, data2;
	u32 ack;
};

struct amipc_database_cell {
	enum amipc_events event;
	amipc_rec_event_callback cb;
	int tx_cnt;
	int rx_cnt;
};

struct amipc_msg_item {
	enum amipc_events event;
	u32 data1, data2;
};

struct amipc_msg_buffer {
	int ipc_wptr;
	int sdk_rptr;
	struct amipc_msg_item msg[MSG_BUFFER_LEN];
	int ov_cnt;
};

struct amipc_acq_infor {
	int wr_num;
	int rd_num;
};

struct amipc_dbg_info {
	int irq_num;
	int irq_thread_num;
};

struct pxa9xx_amipc {
	int irq;
	void __iomem *pmu_base;
	void __iomem *ciu_base;
	struct ipc_event_type *ipc_tx, *ipc_rx;
	struct amipc_database_cell amipc_db[AMIPC_EVENT_LAST];
	struct ipc_event_type user_bakup;
	wait_queue_head_t amipc_wait_q;
	int poll_status;
	struct dentry *dbg_dir;
	struct dentry *dbg_shm;
	struct dentry *dbg_pkg;
	struct dentry *dbg_ctrl;
	struct amipc_msg_buffer msg_buffer;
	struct amipc_acq_infor acq_info;
	struct amipc_dbg_info dbg_info;
};
static struct pxa9xx_amipc *amipc;
static DEFINE_SPINLOCK(amipc_lock);
static void amipc_ping_worker(struct work_struct *work);
static DECLARE_WORK(ping_work, amipc_ping_worker);

static void init_statistic_info(void)
{
	int i;

	for (i = 0; i < AMIPC_EVENT_LAST; i++) {
		amipc->amipc_db[i].tx_cnt = 0;
		amipc->amipc_db[i].rx_cnt = 0;
	}
	memset((void *)&amipc->msg_buffer, 0,
			sizeof(struct amipc_msg_buffer));
	memset((void *)&amipc->acq_info, 0,
			sizeof(struct amipc_acq_infor));
	memset((void *)&amipc->dbg_info, 0,
			sizeof(struct amipc_dbg_info));
}

static void amipc_ping_worker(struct work_struct *work)
{
	amipc_datasend(AMIPC_LL_PING, 2, 0, DEFAULT_TIMEOUT);
}

static int amipc_txaddr_inval(void)
{
	if (amipc->ipc_tx)
		return 0;
	pr_err("tx shared mem invalid\n");
	return -EINVAL;
}

static int amipc_rxaddr_inval(void)
{
	if (amipc->ipc_rx)
		return 0;
	pr_err("rx shared mem invalid\n");
	return -EINVAL;
}

static u32 amipc_default_callback(u32 status)
{
	IPC_ENTER();

	pr_info("event %d not binded\n", status);

	IPC_LEAVE();
	return 0;
}

static u32 amipc_ll_ping_callback(u32 event)
{
	u32 data1;

	IPC_ENTER();
	if (AMIPC_RC_OK == amipc_dataread(event, &data1, NULL)) {
		if (1 == data1)
			schedule_work(&ping_work);
		else if (2 == data1)
			pr_info("get ping response\n");
		else {
			pr_err("invalid ping packet\n");
			IPC_LEAVE();
			return -EINVAL;
		}
	} else {
		IPC_LEAVE();
		return -EINVAL;
	}
	IPC_LEAVE();
	return 0;
}

static u32 amipc_acq_callback(u32 event)
{
	u32 data1, data2;
	unsigned long flags;

	IPC_ENTER();
	if (AMIPC_RC_OK == amipc_dataread(event, &data1, &data2)) {
		amipc->msg_buffer.msg[amipc->msg_buffer.ipc_wptr].event = event;
		amipc->msg_buffer.msg[amipc->msg_buffer.ipc_wptr].data1 = data1;
		amipc->msg_buffer.msg[amipc->msg_buffer.ipc_wptr].data2 = data2;
		amipc->msg_buffer.ipc_wptr = (amipc->msg_buffer.ipc_wptr + 1) %
							MSG_BUFFER_LEN;
		if (unlikely(amipc->msg_buffer.ipc_wptr ==
					amipc->msg_buffer.sdk_rptr))
			amipc->msg_buffer.ov_cnt++;

		if (AMIPC_ACQ_COMMAND == event)
			amipc->acq_info.wr_num++;

		spin_lock_irqsave(&amipc_lock, flags);
		amipc->poll_status = 1;
		spin_unlock_irqrestore(&amipc_lock, flags);
		wake_up_interruptible(&amipc->amipc_wait_q);
	} else {
		IPC_LEAVE();
		return -EINVAL;
	}
	IPC_LEAVE();
	return 0;
}

static void amipc_notify_peer(void)
{
	u32 ciu_reg;

	IPC_ENTER();
	ciu_reg = ciu_readl(GNSS_HANDSHAKE);
	ciu_reg |= AP_GNSS_WAKEUP;
	ciu_writel(GNSS_HANDSHAKE, ciu_reg);
	IPC_LEAVE();
}

static enum amipc_return_code amipc_event_set(enum amipc_events user_event,
						int timeout_ms)
{
	unsigned long end_time;

	IPC_ENTER();
	if (amipc_txaddr_inval())
		return AMIPC_RC_FAILURE;

	if (timeout_ms > 0) {
		end_time = jiffies + msecs_to_jiffies(timeout_ms);
		while (true) {
			if (!(amipc->ipc_tx[E_TO_OFF(user_event)].ack))
				break;
			if (time_after(jiffies, end_time)) {
				pr_warn("tx: wait rdy timeout\n");
				IPC_LEAVE();
				return AMIPC_RC_TIMEOUT;
			}
			msleep(20);
		}
	}
	if (!(amipc->ipc_tx[E_TO_OFF(user_event)].ack)) {
		amipc->ipc_tx[E_TO_OFF(user_event)].event = user_event;
		amipc->ipc_tx[E_TO_OFF(user_event)].data1 = 0;
		amipc->ipc_tx[E_TO_OFF(user_event)].data2 = 0;
		amipc->ipc_tx[E_TO_OFF(user_event)].ack = 1;
		amipc->amipc_db[E_TO_OFF(user_event)].tx_cnt++;
		amipc_notify_peer();
	} else {
		IPC_LEAVE();
		return AMIPC_RC_AGAIN;
	}

	IPCTRACE("amipc_event_set userEvent 0x%x\n", user_event);

	IPC_LEAVE();
	return AMIPC_RC_OK;
}

static enum amipc_return_code amipc_data_send(enum amipc_events user_event,
					u32 data1, u32 data2, int timeout_ms)
{
	unsigned long end_time;
	IPC_ENTER();
	if (amipc_txaddr_inval()) {
		IPC_LEAVE();
		return AMIPC_RC_FAILURE;
	}

	if (timeout_ms > 0) {
		end_time = jiffies + msecs_to_jiffies(timeout_ms);
		while (true) {
			if (!(amipc->ipc_tx[E_TO_OFF(user_event)].ack))
				break;
			if (time_after(jiffies, end_time)) {
				pr_warn("tx: wait rdy timeout\n");
				IPC_LEAVE();
				return AMIPC_RC_TIMEOUT;
			}
			msleep(20);
		}
	}
	if (!(amipc->ipc_tx[E_TO_OFF(user_event)].ack)) {
		amipc->ipc_tx[E_TO_OFF(user_event)].event = user_event;
		amipc->ipc_tx[E_TO_OFF(user_event)].data1 = data1;
		amipc->ipc_tx[E_TO_OFF(user_event)].data2 = data2;
		amipc->ipc_tx[E_TO_OFF(user_event)].ack = 1;
		amipc->amipc_db[E_TO_OFF(user_event)].tx_cnt++;
		amipc_notify_peer();
	} else {
		IPC_LEAVE();
		return AMIPC_RC_AGAIN;
	}

	IPCTRACE("amipc_data_send userEvent 0x%x, data 0x%x\n",
		 user_event, data);

	IPC_LEAVE();
	return AMIPC_RC_OK;
}

static enum amipc_return_code amipc_data_read(enum amipc_events user_event,
						u32 *data1, u32 *data2)
{
	IPC_ENTER();

	if (amipc_rxaddr_inval()) {
		IPC_LEAVE();
		return AMIPC_RC_FAILURE;
	}

	if (data1)
		*data1 = amipc->ipc_rx[E_TO_OFF(user_event)].data1;
	if (data2)
		*data2 = amipc->ipc_rx[E_TO_OFF(user_event)].data2;
	IPC_LEAVE();

	return AMIPC_RC_OK;
}

static enum amipc_return_code amipc_event_bind(u32 user_event,
					       amipc_rec_event_callback cb)
{
	IPC_ENTER();

	if (amipc->amipc_db[E_TO_OFF(user_event)].cb !=
			amipc_default_callback) {
		IPC_LEAVE();
		return AMIPC_EVENT_ALREADY_BIND;
	} else
		amipc->amipc_db[E_TO_OFF(user_event)].cb = cb;

	IPC_LEAVE();
	return AMIPC_RC_OK;
}

static enum amipc_return_code amipc_event_unbind(u32 user_event)
{
	IPC_ENTER();

	amipc->amipc_db[E_TO_OFF(user_event)].cb = amipc_default_callback;

	IPC_LEAVE();
	return AMIPC_RC_OK;
}

static u32 amipc_handle_events(void)
{
	int i;

	IPC_ENTER();
	if (amipc_rxaddr_inval())
		goto skip_cb;

	for (i = 0; i < AMIPC_EVENT_LAST; i++) {
		if (amipc->ipc_rx[i].ack) {
			/* clients fetch possible data in cb */
			amipc->amipc_db[i].cb(OFF_TO_E(i));
			amipc->ipc_rx[i].ack = 0;
			amipc->amipc_db[i].rx_cnt++;
		}
	}

skip_cb:
	IPC_LEAVE();
	return 0;
}

static irqreturn_t amipc_int_thread_handler(int irq, void *dev_id)
{
	unsigned long end_time;

	IPC_ENTER();
	end_time = jiffies + msecs_to_jiffies(1000);
	while (ciu_readl(GNSS_HANDSHAKE) & GNSS_IRQ_OUT_ST) {
		if (time_after(jiffies, end_time)) {
			pr_err("wait GNSS clr int timeout, GNSS irq disabled\n");
			disable_irq_nosync(irq);
			break;
		}
		msleep(20);
	}
	amipc_handle_events();
	IPC_LEAVE();
	return IRQ_HANDLED;
}

static irqreturn_t amipc_interrupt_handler(int irq, void *dev_id)
{
	u32 ciu_reg, pmu_reg;

	IPC_ENTER();
	/* clr interrupt */
	ciu_reg = ciu_readl(GNSS_HANDSHAKE);
	ciu_reg |= GNSS_IRQ_CLR;
	ciu_writel(GNSS_HANDSHAKE, ciu_reg);
	/* clr wakeup */
	pmu_reg = pmu_readl(GNSS_WAKEUP_CTRL);
	if (pmu_reg & GNSS_WAKEUP_STATUS) {
		pmu_reg |= GNSS_WAKEUP_CLR;
		pmu_writel(GNSS_WAKEUP_CTRL, pmu_reg);
	}

	IPC_LEAVE();
	if (ciu_readl(GNSS_HANDSHAKE) & GNSS_IRQ_OUT_ST) {
		amipc->dbg_info.irq_thread_num++;
		return IRQ_WAKE_THREAD;
	} else {
		amipc_handle_events();
		amipc->dbg_info.irq_num++;
		return IRQ_HANDLED;
	}
}

static u32 user_callback(u32 event)
{
	u32 data1, data2;
	unsigned long flags;

	IPC_ENTER();
	amipc->user_bakup.event = event;
	amipc_dataread(event, &data1, &data2);
	amipc->user_bakup.data1 = data1;
	amipc->user_bakup.data2 = data2;
	spin_lock_irqsave(&amipc_lock, flags);
	amipc->poll_status = 1;
	spin_unlock_irqrestore(&amipc_lock, flags);
	wake_up_interruptible(&amipc->amipc_wait_q);
	IPC_LEAVE();

	return 0;
}

static long amipc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct amipc_ioctl_arg amipc_arg;
	int ret = 0;

	IPC_ENTER();
	if (copy_from_user(&amipc_arg,
			   (void __user *)arg, sizeof(struct amipc_ioctl_arg)))
		return -EFAULT;

	switch (cmd) {
	case AMIPC_SET_EVENT:
		amipc_event_set(amipc_arg.event, DEFAULT_TIMEOUT);
		break;
	case AMIPC_GET_EVENT:
		amipc_arg.event = amipc->user_bakup.event;
		break;
	case AMIPC_SEND_DATA:
		amipc_data_send(amipc_arg.event, amipc_arg.data1,
				amipc_arg.data2, DEFAULT_TIMEOUT);
		break;
	case AMIPC_READ_DATA:
		amipc_data_read(amipc_arg.event, &(amipc_arg.data1),
				&(amipc_arg.data2));
		break;
	case AMIPC_BIND_EVENT:
		if (AMIPC_RC_OK != amipc_event_bind(amipc_arg.event,
					user_callback))
			pr_warn("User cb bind failed\n");
		break;
	case AMIPC_UNBIND_EVENT:
		amipc_event_unbind(amipc_arg.event);
		break;
	case AMIPC_IPC_CRL:
		if (AMIPC_ACQ_COMMAND == amipc_arg.event) {
			/* 16M continues memory */
			if (AMIPC_ACQ_GET_MEM == amipc_arg.data1) {
				amipc_arg.data2 = 0x0b000000;
				pr_info("Return phy ACQ mem 0x%x\n",
						amipc_arg.data2);
			} else if (AMIPC_ACQ_REL_MEM == amipc_arg.data1)
				pr_info("Free ACQ mem\n");

		}
		break;
	case AMIPC_IPC_MSG_GET:
		if (amipc->msg_buffer.ipc_wptr == amipc->msg_buffer.sdk_rptr) {
			/* no message available */
			memset((void *)&amipc_arg, 0, sizeof(amipc_arg));
		} else {
			amipc_arg.event =
		amipc->msg_buffer.msg[amipc->msg_buffer.sdk_rptr].event;
			amipc_arg.data1 =
		amipc->msg_buffer.msg[amipc->msg_buffer.sdk_rptr].data1;
			amipc_arg.data2 =
		amipc->msg_buffer.msg[amipc->msg_buffer.sdk_rptr].data2;
			amipc->msg_buffer.sdk_rptr =
		(amipc->msg_buffer.sdk_rptr + 1) % MSG_BUFFER_LEN;
			amipc_arg.more_msgs = (amipc->msg_buffer.ipc_wptr +
		MSG_BUFFER_LEN - amipc->msg_buffer.sdk_rptr) % MSG_BUFFER_LEN;
			if (AMIPC_ACQ_COMMAND == amipc_arg.event)
				amipc->acq_info.rd_num++;
		}
		break;
	case AMIPC_TEST_CRL:
		if (TEST_CRL_CLR == amipc_arg.event) {
			memset((void *)amipc->ipc_tx, 0,
			sizeof(struct ipc_event_type) * AMIPC_EVENT_LAST);
			memset((void *)amipc->ipc_rx, 0,
			sizeof(struct ipc_event_type) * AMIPC_EVENT_LAST);
		} else if (TEST_CRL_SET_TX == amipc_arg.event) {
			if (0 == amipc_arg.data1)
				amipc_data_send(AMIPC_SHM_PACKET_NOTIFY,
						1, 1, DEFAULT_TIMEOUT);
			else if (1 == amipc_arg.data1)
				amipc_data_send(AMIPC_RINGBUF_FC,
						2, 2, DEFAULT_TIMEOUT);
			else if (2 == amipc_arg.data1)
				amipc_data_send(AMIPC_ACQ_COMMAND,
						3, 3, DEFAULT_TIMEOUT);
			else if (3 == amipc_arg.data1)
				amipc_data_send(AMIPC_LL_PING,
						4, 4, DEFAULT_TIMEOUT);
		} else if (TEST_CRL_SET_RX == amipc_arg.event) {
			if (4 == amipc_arg.data1) {
				amipc->ipc_rx[E_TO_OFF(AMIPC_LL_PING)].event =
					AMIPC_LL_PING;
				amipc->ipc_rx[E_TO_OFF(AMIPC_LL_PING)].data1
									= 1;
				amipc->ipc_rx[E_TO_OFF(AMIPC_LL_PING)].ack = 1;
			} else if (5 == amipc_arg.data1) {
				amipc->ipc_rx[E_TO_OFF(AMIPC_LL_PING)].event =
					AMIPC_LL_PING;
				amipc->ipc_rx[E_TO_OFF(AMIPC_LL_PING)].data1
									= 2;
				amipc->ipc_rx[E_TO_OFF(AMIPC_LL_PING)].ack = 1;
			}
		} else if (TEST_CRL_GEN_IRQ == amipc_arg.event) {
			amipc_interrupt_handler(0, NULL);
		}
		break;
	default:
		ret = -1;
		break;
	}

	if (copy_to_user((void __user *)arg, &amipc_arg,
			 sizeof(struct amipc_ioctl_arg)))
		return -EFAULT;

	IPC_LEAVE();

	return ret;
}

static unsigned int amipc_poll(struct file *file, poll_table *wait)
{
	unsigned long flags;

	IPC_ENTER();
	poll_wait(file, &amipc->amipc_wait_q, wait);
	IPC_LEAVE();

	if (amipc->poll_status == 0) {
		return 0;
	} else {
		spin_lock_irqsave(&amipc_lock, flags);
		amipc->poll_status = 0;
		spin_unlock_irqrestore(&amipc_lock, flags);
		return POLLIN | POLLRDNORM;
	}
}

static void amipc_vma_open(struct vm_area_struct *vma)
{
	pr_info("AMIPC OPEN 0x%lx -> 0x%lx\n", vma->vm_start,
			vma->vm_pgoff << PAGE_SHIFT);
}

static void amipc_vma_close(struct vm_area_struct *vma)
{
	pr_info("AMIPC CLOSE 0x%lx -> 0x%lx\n", vma->vm_start,
			vma->vm_pgoff << PAGE_SHIFT);
}

/* These are mostly for debug: do nothing useful otherwise */
static struct vm_operations_struct vm_ops = {
	.open = amipc_vma_open,
	.close = amipc_vma_close
};

int amipc_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pa = vma->vm_pgoff;

	/* we do not want to have this area swapped out, lock it */
	vma->vm_flags |= (VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	/* see linux/drivers/char/mem.c */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	/*
	 * TBD: without this vm_page_prot=0x53 and write seem to not
	 * reach the destination:
	 * - no fault on write
	 * - read immediately (same user process) return the new value
	 * - read after a second by another user process instance return
	 *   original value
	 *  Why PROT_WRITE specified by mmap caller does not take effect?
	 *  MAP_PRIVATE used by app results in copy-on-write behaviour,
	 *  which is irrelevant for this application
	 *  vma->vm_page_prot|=L_PTE_WRITE;
	 */

	if (io_remap_pfn_range(vma, vma->vm_start, pa,/* physical page index */
				size, vma->vm_page_prot)) {
		pr_err("remap page range failed\n");
		return -ENXIO;
	}
	vma->vm_ops = &vm_ops;
	amipc_vma_open(vma);
	return 0;
}

static int shm_block_show(struct seq_file *m, void *unused)
{
	int i;
	rcu_read_lock();
	if (amipc_txaddr_inval())
		seq_puts(m, "tx shm invalid\n");
	else {
		seq_puts(m, "tx shm status:\n");
		seq_puts(m, "event\tdata1\tdata2\tack\tcount\n");
		for (i = 0; i < AMIPC_EVENT_LAST; i++)
			seq_printf(m, "%x\t%x\t%x\t%x\t%d\n",
					amipc->ipc_tx[i].event,
					amipc->ipc_tx[i].data1,
					amipc->ipc_tx[i].data2,
					amipc->ipc_tx[i].ack,
					amipc->amipc_db[i].tx_cnt);
	}
	if (amipc_rxaddr_inval())
		seq_puts(m, "rx shm invalid\n");
	else {
		seq_puts(m, "rx shm status:\n");
		seq_puts(m, "event\tdata1\tdata2\tack\tcount\n");
		for (i = 0; i < AMIPC_EVENT_LAST; i++)
			seq_printf(m, "%x\t%x\t%x\t%x\t%d\n",
					amipc->ipc_rx[i].event,
					amipc->ipc_rx[i].data1,
					amipc->ipc_rx[i].data2,
					amipc->ipc_rx[i].ack,
					amipc->amipc_db[i].rx_cnt);
	}

	rcu_read_unlock();
	return 0;
}

static int shm_block_open(struct inode *inode, struct file *file)
{
	return single_open(file, shm_block_show, NULL);
}

const struct file_operations amipc_shm_fops = {
	.owner = THIS_MODULE,
	.open = shm_block_open,
	.read = seq_read,
	.llseek = seq_lseek,
};

static int pkgstat_block_show(struct seq_file *m, void *unused)
{
	rcu_read_lock();
	seq_printf(m, "msg buffer sdk rptr: %d ipc wprt: %d over flow: %d\n",
			amipc->msg_buffer.sdk_rptr, amipc->msg_buffer.ipc_wptr,
			amipc->msg_buffer.ov_cnt);
	seq_printf(m, "acq wr_num: %d rd_num %d\n", amipc->acq_info.wr_num,
						amipc->acq_info.rd_num);
	seq_printf(m, "reg handshake 0x%x, wakeup 0x%x\n",
			ciu_readl(GNSS_HANDSHAKE), pmu_readl(GNSS_WAKEUP_CTRL));
	seq_printf(m, "hw irq %d, thread irq %d, total = %d\n",
		amipc->dbg_info.irq_num, amipc->dbg_info.irq_thread_num,
		amipc->dbg_info.irq_num + amipc->dbg_info.irq_thread_num);
	rcu_read_unlock();
	return 0;
}

static int pkgstat_block_open(struct inode *inode, struct file *file)
{
	return single_open(file, pkgstat_block_show, NULL);
}

const struct file_operations amipc_pkgstat_fops = {
	.owner = THIS_MODULE,
	.open = pkgstat_block_open,
	.read = seq_read,
	.llseek = seq_lseek,
};

static u32 dbg_cmd_index;
static int command_block_show(struct seq_file *m, void *unused)
{
	rcu_read_lock();
	if (0 == dbg_cmd_index) {
		seq_puts(m, "echo x > cmd_index firstly, then cat again\n");
		seq_puts(m, "x:0 --> disable debug feature\n");
		seq_puts(m, "x:1 --> init shared memory and debug info\n");
		seq_puts(m, "x:2 --> send ping command\n");
	} else if (1 == dbg_cmd_index) {
		if (amipc->ipc_tx)
			memset((void *)amipc->ipc_tx, 0,
			sizeof(struct ipc_event_type) * AMIPC_EVENT_LAST * 2);
		init_statistic_info();
	} else if (2 == dbg_cmd_index) {
		pr_info("send ping request\n");
		amipc_data_send(AMIPC_LL_PING,
			1, 0, DEFAULT_TIMEOUT);
	}
	rcu_read_unlock();
	return 0;
}

static int command_block_open(struct inode *inode, struct file *file)
{
	return single_open(file, command_block_show, NULL);
}

const struct file_operations amipc_command_fops = {
	.owner = THIS_MODULE,
	.open = command_block_open,
	.read = seq_read,
	.llseek = seq_lseek,
};

static const struct file_operations amipc_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = amipc_ioctl,
	.poll = amipc_poll,
	.mmap = amipc_mmap,
};

static struct miscdevice amipc_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "amipc",
	.fops = &amipc_fops,
};

#ifdef CONFIG_PM_SLEEP
static int pxa9xx_amipc_suspend(struct device *dev)
{
	if (device_may_wakeup(dev))
		enable_irq_wake(amipc->irq);
	return 0;
}

static int pxa9xx_amipc_resume(struct device *dev)
{
	if (device_may_wakeup(dev))
		disable_irq_wake(amipc->irq);
	return 0;
}

static SIMPLE_DEV_PM_OPS(pxa9xx_amipc_pm_ops,
		pxa9xx_amipc_suspend, pxa9xx_amipc_resume);
#endif

static int pxa9xx_amipc_probe(struct platform_device *pdev)
{
	int ret, irq, i;
	u32 pmu_reg;

	amipc =
	    devm_kzalloc(&pdev->dev, sizeof(struct pxa9xx_amipc), GFP_KERNEL);
	if (!amipc)
		return -ENOMEM;

	amipc->pmu_base = get_apmu_base_va();
	amipc->ciu_base = get_ciu_base_va();

	for (i = 0; i < AMIPC_EVENT_LAST; i++)
		amipc->amipc_db[i].cb = amipc_default_callback;
	amipc->amipc_db[E_TO_OFF(AMIPC_LL_PING)].cb = amipc_ll_ping_callback;
	amipc->amipc_db[E_TO_OFF(AMIPC_ACQ_COMMAND)].cb = amipc_acq_callback;

	init_waitqueue_head(&amipc->amipc_wait_q);

	platform_set_drvdata(pdev, amipc);

	irq = platform_get_irq(pdev, 0);
	if (irq >= 0) {
		ret = devm_request_threaded_irq(&pdev->dev, irq,
				amipc_interrupt_handler,
				amipc_int_thread_handler,
				IRQF_NO_SUSPEND | IRQF_ONESHOT,
				"PXA9XX-amipc", amipc);
	}
	if (irq < 0 || ret < 0)
		return -ENXIO;
	amipc->irq = irq;

	ret = misc_register(&amipc_miscdev);
	if (ret < 0)
		return ret;

	/* enable GNSS wakeup AP */
	pmu_reg = pmu_readl(GNSS_WAKEUP_CTRL);
	pmu_reg |= GNSS_WAKEUP_EN;
	pmu_writel(GNSS_WAKEUP_CTRL, pmu_reg);

	amipc->dbg_dir = debugfs_create_dir("amipc", NULL);
	if (amipc->dbg_dir) {
		amipc->dbg_shm = debugfs_create_file("sharemem", S_IRUGO,
				amipc->dbg_dir, NULL, &amipc_shm_fops);
		amipc->dbg_pkg = debugfs_create_file("packetstat", S_IRUGO,
				amipc->dbg_dir, NULL, &amipc_pkgstat_fops);
		amipc->dbg_ctrl = debugfs_create_file("command", S_IRUGO,
				amipc->dbg_dir, NULL, &amipc_command_fops);
		debugfs_create_u32("cmd_index", 0644, amipc->dbg_dir,
					&dbg_cmd_index);
	} else
		pr_warn("pxa9xx amipc create debugfs fail\n");

	device_init_wakeup(&pdev->dev, 1);

	pr_info("pxa9xx AM-IPC initialized!\n");

	return 0;
}

static int pxa9xx_amipc_remove(struct platform_device *pdev)
{
	debugfs_remove(amipc->dbg_shm);
	debugfs_remove(amipc->dbg_pkg);
	debugfs_remove(amipc->dbg_dir);
	misc_deregister(&amipc_miscdev);
	return 0;
}

static struct of_device_id pxa9xx_amipc_dt_ids[] = {
	{ .compatible = "marvell,mmp-amipc", },
	{ .compatible = "marvell,pxa-amipc", },
	{}
};

static struct platform_driver pxa9xx_amipc_driver = {
	.driver = {
		   .name = "pxa9xx-amipc",
		   .of_match_table = pxa9xx_amipc_dt_ids,
#ifdef CONFIG_PM_SLEEP
		   .pm = &pxa9xx_amipc_pm_ops,
#endif
		   },
	.probe = pxa9xx_amipc_probe,
	.remove = pxa9xx_amipc_remove
};

static int __init pxa9xx_amipc_init(void)
{
	return platform_driver_register(&pxa9xx_amipc_driver);
}

static void __exit pxa9xx_amipc_exit(void)
{
	platform_driver_unregister(&pxa9xx_amipc_driver);
}

enum amipc_return_code amipc_setbase(void *base_addr, int len)
{
	bool ddr_mem;
	if (len < sizeof(struct ipc_event_type) * AMIPC_EVENT_LAST * 2) {
		pr_err("share memory too small\n");
		return AMIPC_RC_FAILURE;
	}
	pr_info("amipc: set base address phys:0x%x, len:%d\n",
			(unsigned int)base_addr, len);
	/* init debug info */
	init_statistic_info();
	if ((uint32_t)base_addr < PAGE_OFFSET)
		ddr_mem = true;
	else
		ddr_mem = false;
	base_addr = ioremap_nocache((uint32_t)base_addr, len);
	if (base_addr) {
		amipc->ipc_tx = (struct ipc_event_type *)base_addr;
		amipc->ipc_rx = amipc->ipc_tx + AMIPC_EVENT_LAST;
		if (ddr_mem)
			memset((void *)amipc->ipc_tx, 0, len);
		return AMIPC_RC_OK;
	} else {
		pr_err("share memory remap fail\n");
		return AMIPC_RC_FAILURE;
	}
}
EXPORT_SYMBOL(amipc_setbase);

enum amipc_return_code amipc_eventbind(u32 user_event,
				      amipc_rec_event_callback cb)
{
	return amipc_event_bind(user_event, cb);
}
EXPORT_SYMBOL(amipc_eventbind);

enum amipc_return_code amipc_eventunbind(u32 user_event)
{
	return amipc_event_unbind(user_event);
}
EXPORT_SYMBOL(amipc_eventunbind);

enum amipc_return_code amipc_eventset(enum amipc_events user_event,
						int timeout_ms)
{
	return amipc_event_set(user_event, timeout_ms);
}
EXPORT_SYMBOL(amipc_eventset);

enum amipc_return_code amipc_datasend(enum amipc_events user_event,
					u32 data1, u32 data2, int timeout_ms)
{
	return amipc_data_send(user_event, data1, data2, timeout_ms);
}
EXPORT_SYMBOL(amipc_datasend);

enum amipc_return_code amipc_dataread(enum amipc_events user_event,
				u32 *data1, u32 *data2)
{
	return amipc_data_read(user_event, data1, data2);
}
EXPORT_SYMBOL(amipc_dataread);

module_init(pxa9xx_amipc_init);
module_exit(pxa9xx_amipc_exit);
MODULE_AUTHOR("MARVELL");
MODULE_DESCRIPTION("AM-IPC driver");
MODULE_LICENSE("GPL");