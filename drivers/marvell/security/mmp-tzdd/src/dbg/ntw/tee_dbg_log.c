/*
 * Copyright (c) [2009-2013] Marvell International Ltd. and its affiliates.
 * All rights reserved.
 * This software file (the "File") is owned and distributed by Marvell
 * International Ltd. and/or its affiliates ("Marvell") under the following
 * licensing terms.
 * If you received this File from Marvell, you may opt to use, redistribute
 * and/or modify this File in accordance with the terms and conditions of
 * the General Public License Version 2, June 1991 (the "GPL License"), a
 * copy of which is available along with the File in the license.txt file
 * or by writing to the Free Software Foundation, Inc., 59 Temple Place,
 * Suite 330, Boston, MA 02111-1307 or on the worldwide web at
 * http://www.gnu.org/licenses/gpl.txt. THE FILE IS DISTRIBUTED AS-IS,
 * WITHOUT WARRANTY OF ANY KIND, AND THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE ARE EXPRESSLY
 * DISCLAIMED. The GPL License provides additional details about this
 * warranty disclaimer.
 */
#include "tzdd_internal.h"
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define _TEE_DBG_PROC_LEVEL0	(0x00000000)
#define _TEE_DBG_PROC_LEVEL1	(0x55aa55aa)
#define _TEE_DBG_PROC_LEVEL2	(0x66bb66bb)
#define _TEE_DBG_PROC_LEVEL3	(0x77cc77cc)

#define IS_DBG_PROC_MAGIC_VALID(_m) \
	(('P' == _m[0]) &&  \
	 ('r' == _m[1]) &&   \
	 ('O' == _m[2]) &&   \
	 ('c' == _m[3]))

#define INIT_DBG_PROC_CTRL_MAGIC(_m)	do {	\
		_m[0] = 'C';	\
		_m[1] = 't';	\
		_m[2] = 'R';	\
		_m[3] = 'l';	\
	} while (0)

typedef struct _tee_dbg_log_t {
	uint8_t magic[4];
	uint32_t write_offset;
	uint64_t round;
	uint32_t log_buf_sz;
} tee_dbg_log_t;

typedef struct _tee_dbg_log_ctl_t {
	volatile uint8_t magic[4];
	volatile uint32_t level;
} tee_dbg_log_ctl_t;

static tee_dbg_log_t *_g_tee_dbg_log_header;
static tee_dbg_log_ctl_t *_g_tee_dbg_log_ctl_header;
static uint8_t *_g_tee_dbg_log_buf;
static uint32_t _g_tee_dbg_log_buf_sz;

void tee_dbg_log_init(ulong_t buffer, ulong_t ctl)
{
	_g_tee_dbg_log_header = (tee_dbg_log_t *) buffer;
	_g_tee_dbg_log_ctl_header = (tee_dbg_log_ctl_t *) ctl;
	_g_tee_dbg_log_buf = (uint8_t *) (_g_tee_dbg_log_header + 1);

	if (!IS_DBG_PROC_MAGIC_VALID(_g_tee_dbg_log_header->magic)) {
		printk(KERN_ERR "proc log invalid magic\n");
		return;
	}
	_g_tee_dbg_log_buf_sz = _g_tee_dbg_log_header->log_buf_sz;

	INIT_DBG_PROC_CTRL_MAGIC(_g_tee_dbg_log_ctl_header->magic);

	printk(KERN_ERR "proc log buf: 0x%lx, size: 0x%x, ctl: 0x%lx\n",
		buffer, _g_tee_dbg_log_buf_sz, ctl);
	return;
}

static uint32_t g_snapshot_write_offset;
static uint64_t g_snapshot_round;
static uint64_t g_read_round;
static uint32_t g_read_offset;
static uint32_t g_read_count;
static uint32_t g_proc_log_level;

static void *log_seq_start(struct seq_file *s, loff_t *pos)
{
	g_snapshot_write_offset = _g_tee_dbg_log_header->write_offset;
	g_snapshot_round = _g_tee_dbg_log_header->round;

	if ((!IS_DBG_PROC_MAGIC_VALID(_g_tee_dbg_log_header->magic)) ||
		(!_g_tee_dbg_log_buf_sz))
		return NULL;

	if (g_read_round == g_snapshot_round
		&& g_read_offset == g_snapshot_write_offset)
		/* nothing to read */
		return NULL;

	if ((g_snapshot_round > g_read_round + 1) ||
		(g_snapshot_round == g_read_round + 1
		&& g_snapshot_write_offset > g_read_offset)) {
		g_read_offset = g_snapshot_write_offset;
		g_read_round = g_snapshot_round - 1;
		seq_printf(s,
				"--------------------------rollback--------------------------\n");
	}

	g_read_count = s->count;

	return _g_tee_dbg_log_buf + g_read_offset;
}

static void *log_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	g_read_count = s->count;

	if (g_read_round == g_snapshot_round
		&& g_read_offset == g_snapshot_write_offset)
		/* nothing to read */
		return NULL;

	return _g_tee_dbg_log_buf + g_read_offset;
}

static void log_seq_stop(struct seq_file *s, void *v)
{
	return;
}

static int log_seq_show(struct seq_file *s, void *v)
{
	int ret;
	ret = seq_printf(s, "%s", (int8_t *) v);
	if (ret) {
		/* over flow */
	} else {
		g_read_offset += s->count - g_read_count + 1;
	}

	if (g_read_offset > _g_tee_dbg_log_buf_sz) {
		g_read_offset -= _g_tee_dbg_log_buf_sz;
		g_read_round++;
	}

	return ret;
}

static const struct seq_operations log_seq_ops = {
	.start = log_seq_start,
	.next = log_seq_next,
	.stop = log_seq_stop,
	.show = log_seq_show
};

static int log_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &log_seq_ops);
}

static const struct file_operations log_file_ops = {
	.owner = THIS_MODULE,
	.open = log_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release
};

static int ctl_seq_show(struct seq_file *m, void *v)
{
	seq_printf(m, "tee proc log level %d\n", g_proc_log_level);
	return 0;
}

static int ctl_open(struct inode *inode, struct file *file)
{
	return single_open(file, ctl_seq_show, NULL);
}

static ssize_t ctl_write(struct file *file, const char *buf,
			 size_t count, loff_t *pos)
{
	char val;

	get_user(val, buf);
	switch (val) {
	case '0':
		_g_tee_dbg_log_ctl_header->level = _TEE_DBG_PROC_LEVEL0;
		g_proc_log_level = 0;
		break;
	case '1':
		_g_tee_dbg_log_ctl_header->level = _TEE_DBG_PROC_LEVEL1;
		g_proc_log_level = 1;
		break;
	case '2':
		_g_tee_dbg_log_ctl_header->level = _TEE_DBG_PROC_LEVEL2;
		g_proc_log_level = 2;
		break;
	case '3':
		_g_tee_dbg_log_ctl_header->level = _TEE_DBG_PROC_LEVEL3;
		g_proc_log_level = 3;
		break;
	default:
		printk(KERN_ERR "value should be 0/1/2/3\n");
		return count;
	}

	return count;
}

static const struct file_operations ctl_file_ops = {
	.owner = THIS_MODULE,
	.open = ctl_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = ctl_write,
};

extern uint32_t g_msg_sent;
extern uint32_t g_msg_recv;
extern uint32_t g_msg_fake;
extern uint32_t g_msg_ignd;
extern uint32_t g_pre_ipi_num;
extern uint32_t g_pst_ipi_num;
extern uint32_t g_pre_dmy_num;
extern uint32_t g_pst_dmy_num;

extern uint32_t tzdd_get_req_num_in_list(void);

static int stat_seq_show(struct seq_file *m, void *v)
{
	seq_printf(m, "lst: %d\n"
		   "msg: sent %d, recv %d, fake %d, ignd %d\n"
		   "smc: pre-ipi %d, pst-ipi %d, pre-dmy %d, pst-dmy %d\n",
		   tzdd_get_req_num_in_list(),
		   g_msg_sent, g_msg_recv, g_msg_fake, g_msg_ignd,
		   g_pre_ipi_num, g_pst_ipi_num, g_pre_dmy_num, g_pst_dmy_num);
	return 0;
}

static int stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, stat_seq_show, NULL);
}

static const struct file_operations stat_file_ops = {
	.owner = THIS_MODULE,
	.open = stat_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

struct proc_dir_entry *_g_tzdd_entry;
void tee_dbg_proc_fs_init(void)
{
	_g_tzdd_entry = proc_mkdir("tee", NULL);

	proc_create("log", S_IRUGO, _g_tzdd_entry, &log_file_ops);
	proc_create("enable", S_IRUGO | S_IWUGO, _g_tzdd_entry, &ctl_file_ops);
	proc_create("stat", S_IRUGO, _g_tzdd_entry, &stat_file_ops);
}

void tee_dbg_proc_fs_cleanup(void)
{
	if (_g_tzdd_entry) {
		remove_proc_entry("log", _g_tzdd_entry);
		remove_proc_entry("enable", _g_tzdd_entry);
		remove_proc_entry("stat", _g_tzdd_entry);
		remove_proc_entry("tee", NULL);
	}
}
