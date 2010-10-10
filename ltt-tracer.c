/*
 * ltt/ltt-tracer.c
 *
 * Copyright (c) 2005-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Tracing management internal kernel API. Trace buffer allocation/free, tracing
 * start/stop.
 *
 * Author:
 *	Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Inspired from LTT :
 *  Karim Yaghmour (karim@opersys.com)
 *  Tom Zanussi (zanussi@us.ibm.com)
 *  Bob Wisniewski (bob@watson.ibm.com)
 * And from K42 :
 *  Bob Wisniewski (bob@watson.ibm.com)
 *
 * Changelog:
 *  22/09/06, Move to the marker/probes mechanism.
 *  19/10/05, Complete lockless mechanism.
 *  27/05/05, Modular redesign and rewrite.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <linux/time.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/bitops.h>
#include <linux/fs.h>
#include <linux/cpu.h>
#include <linux/kref.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <asm/atomic.h>

#include "ltt-tracer.h"

static void synchronize_trace(void)
{
	synchronize_sched();
#ifdef CONFIG_PREEMPT_RT
	synchronize_rcu();
#endif
}

static void async_wakeup(unsigned long data);

static DEFINE_TIMER(ltt_async_wakeup_timer, async_wakeup, 0, 0);

/* Default callbacks for modules */
notrace
int ltt_filter_control_default(enum ltt_filter_control_msg msg,
			       struct ltt_trace *trace)
{
	return 0;
}

int ltt_statedump_default(struct ltt_trace *trace)
{
	return 0;
}

/* Callbacks for registered modules */

int (*ltt_filter_control_functor)
	(enum ltt_filter_control_msg msg, struct ltt_trace *trace) =
					ltt_filter_control_default;
struct module *ltt_filter_control_owner;

/* These function pointers are protected by a trace activation check */
struct module *ltt_run_filter_owner;
int (*ltt_statedump_functor)(struct ltt_trace *trace) = ltt_statedump_default;
struct module *ltt_statedump_owner;

struct chan_info_struct {
	const char *name;
	unsigned int def_sb_size;
	unsigned int def_n_sb;
} chan_infos[] = {
	[LTT_CHANNEL_METADATA] = {
		LTT_METADATA_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_FD_STATE] = {
		LTT_FD_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_GLOBAL_STATE] = {
		LTT_GLOBAL_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_IRQ_STATE] = {
		LTT_IRQ_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_MODULE_STATE] = {
		LTT_MODULE_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_NETIF_STATE] = {
		LTT_NETIF_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_SOFTIRQ_STATE] = {
		LTT_SOFTIRQ_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_SWAP_STATE] = {
		LTT_SWAP_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_SYSCALL_STATE] = {
		LTT_SYSCALL_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_TASK_STATE] = {
		LTT_TASK_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_VM_STATE] = {
		LTT_VM_STATE_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_MED,
		LTT_DEFAULT_N_SUBBUFS_MED,
	},
	[LTT_CHANNEL_FS] = {
		LTT_FS_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_MED,
		LTT_DEFAULT_N_SUBBUFS_MED,
	},
	[LTT_CHANNEL_INPUT] = {
		LTT_INPUT_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_IPC] = {
		LTT_IPC_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_LOW,
		LTT_DEFAULT_N_SUBBUFS_LOW,
	},
	[LTT_CHANNEL_KERNEL] = {
		LTT_KERNEL_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_HIGH,
		LTT_DEFAULT_N_SUBBUFS_HIGH,
	},
	[LTT_CHANNEL_MM] = {
		LTT_MM_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_MED,
		LTT_DEFAULT_N_SUBBUFS_MED,
	},
	[LTT_CHANNEL_RCU] = {
		LTT_RCU_CHANNEL,
		LTT_DEFAULT_SUBBUF_SIZE_MED,
		LTT_DEFAULT_N_SUBBUFS_MED,
	},
	[LTT_CHANNEL_DEFAULT] = {
		NULL,
		LTT_DEFAULT_SUBBUF_SIZE_MED,
		LTT_DEFAULT_N_SUBBUFS_MED,
	},
};

static enum ltt_channels get_channel_type_from_name(const char *name)
{
	int i;

	if (!name)
		return LTT_CHANNEL_DEFAULT;

	for (i = 0; i < ARRAY_SIZE(chan_infos); i++)
		if (chan_infos[i].name && !strcmp(name, chan_infos[i].name))
			return (enum ltt_channels)i;

	return LTT_CHANNEL_DEFAULT;
}

/**
 * ltt_module_register - LTT module registration
 * @name: module type
 * @function: callback to register
 * @owner: module which owns the callback
 *
 * The module calling this registration function must ensure that no
 * trap-inducing code will be executed by "function". E.g. vmalloc_sync_all()
 * must be called between a vmalloc and the moment the memory is made visible to
 * "function". This registration acts as a vmalloc_sync_all. Therefore, only if
 * the module allocates virtual memory after its registration must it
 * synchronize the TLBs.
 */
int ltt_module_register(enum ltt_module_function name, void *function,
			struct module *owner)
{
	int ret = 0;

	/*
	 * Make sure no page fault can be triggered by the module about to be
	 * registered. We deal with this here so we don't have to call
	 * vmalloc_sync_all() in each module's init.
	 */
	vmalloc_sync_all();

	switch (name) {
	case LTT_FUNCTION_RUN_FILTER:
		if (ltt_run_filter_owner != NULL) {
			ret = -EEXIST;
			goto end;
		}
		ltt_filter_register((ltt_run_filter_functor)function);
		ltt_run_filter_owner = owner;
		break;
	case LTT_FUNCTION_FILTER_CONTROL:
		if (ltt_filter_control_owner != NULL) {
			ret = -EEXIST;
			goto end;
		}
		ltt_filter_control_functor =
			(int (*)(enum ltt_filter_control_msg,
			struct ltt_trace *))function;
		ltt_filter_control_owner = owner;
		break;
	case LTT_FUNCTION_STATEDUMP:
		if (ltt_statedump_owner != NULL) {
			ret = -EEXIST;
			goto end;
		}
		ltt_statedump_functor =
			(int (*)(struct ltt_trace *))function;
		ltt_statedump_owner = owner;
		break;
	}
end:
	return ret;
}
EXPORT_SYMBOL_GPL(ltt_module_register);

/**
 * ltt_module_unregister - LTT module unregistration
 * @name: module type
 */
void ltt_module_unregister(enum ltt_module_function name)
{
	switch (name) {
	case LTT_FUNCTION_RUN_FILTER:
		ltt_filter_unregister();
		ltt_run_filter_owner = NULL;
		/* Wait for preempt sections to finish */
		synchronize_trace();
		break;
	case LTT_FUNCTION_FILTER_CONTROL:
		ltt_filter_control_functor = ltt_filter_control_default;
		ltt_filter_control_owner = NULL;
		break;
	case LTT_FUNCTION_STATEDUMP:
		ltt_statedump_functor = ltt_statedump_default;
		ltt_statedump_owner = NULL;
		break;
	}

}
EXPORT_SYMBOL_GPL(ltt_module_unregister);

static LIST_HEAD(ltt_transport_list);

/**
 * ltt_transport_register - LTT transport registration
 * @transport: transport structure
 *
 * Registers a transport which can be used as output to extract the data out of
 * LTTng. The module calling this registration function must ensure that no
 * trap-inducing code will be executed by the transport functions. E.g.
 * vmalloc_sync_all() must be called between a vmalloc and the moment the memory
 * is made visible to the transport function. This registration acts as a
 * vmalloc_sync_all. Therefore, only if the module allocates virtual memory
 * after its registration must it synchronize the TLBs.
 */
void ltt_transport_register(struct ltt_transport *transport)
{
	/*
	 * Make sure no page fault can be triggered by the module about to be
	 * registered. We deal with this here so we don't have to call
	 * vmalloc_sync_all() in each module's init.
	 */
	vmalloc_sync_all();

	ltt_lock_traces();
	list_add_tail(&transport->node, &ltt_transport_list);
	ltt_unlock_traces();
}
EXPORT_SYMBOL_GPL(ltt_transport_register);

/**
 * ltt_transport_unregister - LTT transport unregistration
 * @transport: transport structure
 */
void ltt_transport_unregister(struct ltt_transport *transport)
{
	ltt_lock_traces();
	list_del(&transport->node);
	ltt_unlock_traces();
}
EXPORT_SYMBOL_GPL(ltt_transport_unregister);

static inline
int is_channel_overwrite(enum ltt_channels chan, enum trace_mode mode)
{
	switch (mode) {
	case LTT_TRACE_NORMAL:
		return 0;
	case LTT_TRACE_FLIGHT:
		switch (chan) {
		case LTT_CHANNEL_METADATA:
			return 0;
		default:
			return 1;
		}
	case LTT_TRACE_HYBRID:
		switch (chan) {
		case LTT_CHANNEL_KERNEL:
		case LTT_CHANNEL_FS:
		case LTT_CHANNEL_MM:
		case LTT_CHANNEL_RCU:
		case LTT_CHANNEL_IPC:
		case LTT_CHANNEL_INPUT:
			return 1;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

/**
 * _ltt_trace_find - find a trace by given name.
 * trace_name: trace name
 *
 * Returns a pointer to the trace structure, NULL if not found.
 */
static struct ltt_trace *_ltt_trace_find(const char *trace_name)
{
	struct ltt_trace *trace;

	list_for_each_entry(trace, &ltt_traces.head, list)
		if (!strncmp(trace->trace_name, trace_name, NAME_MAX))
			return trace;

	return NULL;
}

/* _ltt_trace_find_setup :
 * find a trace in setup list by given name.
 *
 * Returns a pointer to the trace structure, NULL if not found.
 */
struct ltt_trace *_ltt_trace_find_setup(const char *trace_name)
{
	struct ltt_trace *trace;

	list_for_each_entry(trace, &ltt_traces.setup_head, list)
		if (!strncmp(trace->trace_name, trace_name, NAME_MAX))
			return trace;

	return NULL;
}
EXPORT_SYMBOL_GPL(_ltt_trace_find_setup);

/**
 * ltt_release_trace - Release a LTT trace
 * @kref : reference count on the trace
 */
void ltt_release_trace(struct kref *kref)
{
	struct ltt_trace *trace = container_of(kref, struct ltt_trace, kref);

	trace->ops->remove_dirs(trace);
	module_put(trace->transport->owner);
	ltt_channels_trace_free(trace);
	kfree(trace);
}
EXPORT_SYMBOL_GPL(ltt_release_trace);

static inline void prepare_chan_size_num(unsigned int *subbuf_size,
					 unsigned int *n_subbufs)
{
	/* Make sure the subbuffer size is larger than a page */
	*subbuf_size = max_t(unsigned int, *subbuf_size, PAGE_SIZE);

	/* round to next power of 2 */
	*subbuf_size = 1 << get_count_order(*subbuf_size);
	*n_subbufs = 1 << get_count_order(*n_subbufs);

	/* Subbuf size and number must both be power of two */
	WARN_ON(hweight32(*subbuf_size) != 1);
	WARN_ON(hweight32(*n_subbufs) != 1);
}

int _ltt_trace_setup(const char *trace_name)
{
	int err = 0;
	struct ltt_trace *new_trace = NULL;
	int metadata_index;
	unsigned int chan;
	enum ltt_channels chantype;

	if (_ltt_trace_find_setup(trace_name)) {
		printk(KERN_ERR	"LTT : Trace name %s already used.\n",
				trace_name);
		err = -EEXIST;
		goto traces_error;
	}

	if (_ltt_trace_find(trace_name)) {
		printk(KERN_ERR	"LTT : Trace name %s already used.\n",
				trace_name);
		err = -EEXIST;
		goto traces_error;
	}

	new_trace = kzalloc(sizeof(struct ltt_trace), GFP_KERNEL);
	if (!new_trace) {
		printk(KERN_ERR
			"LTT : Unable to allocate memory for trace %s\n",
			trace_name);
		err = -ENOMEM;
		goto traces_error;
	}
	strncpy(new_trace->trace_name, trace_name, NAME_MAX);
	if (ltt_channels_trace_alloc(&new_trace->nr_channels, 0)) {
		printk(KERN_ERR
			"LTT : Unable to allocate memory for chaninfo  %s\n",
			trace_name);
		err = -ENOMEM;
		goto trace_free;
	}

	/*
	 * Force metadata channel to no overwrite.
	 */
	metadata_index = ltt_channels_get_index_from_name("metadata");
	WARN_ON(metadata_index < 0);
	new_trace->settings[metadata_index].overwrite = 0;

	/*
	 * Set hardcoded tracer defaults for some channels
	 */
	for (chan = 0; chan < new_trace->nr_channels; chan++) {
		chantype = get_channel_type_from_name(
			ltt_channels_get_name_from_index(chan));
		new_trace->settings[chan].sb_size =
			chan_infos[chantype].def_sb_size;
		new_trace->settings[chan].n_sb =
			chan_infos[chantype].def_n_sb;
	}

	list_add(&new_trace->list, &ltt_traces.setup_head);
	return 0;

trace_free:
	kfree(new_trace);
traces_error:
	return err;
}
EXPORT_SYMBOL_GPL(_ltt_trace_setup);


int ltt_trace_setup(const char *trace_name)
{
	int ret;
	ltt_lock_traces();
	ret = _ltt_trace_setup(trace_name);
	ltt_unlock_traces();
	return ret;
}
EXPORT_SYMBOL_GPL(ltt_trace_setup);

/* must be called from within a traces lock. */
static void _ltt_trace_free(struct ltt_trace *trace)
{
	list_del(&trace->list);
	kfree(trace);
}

int ltt_trace_set_type(const char *trace_name, const char *trace_type)
{
	int err = 0;
	struct ltt_trace *trace;
	struct ltt_transport *tran_iter, *transport = NULL;

	ltt_lock_traces();

	trace = _ltt_trace_find_setup(trace_name);
	if (!trace) {
		printk(KERN_ERR "LTT : Trace not found %s\n", trace_name);
		err = -ENOENT;
		goto traces_error;
	}

	list_for_each_entry(tran_iter, &ltt_transport_list, node) {
		if (!strcmp(tran_iter->name, trace_type)) {
			transport = tran_iter;
			break;
		}
	}
	if (!transport) {
		printk(KERN_ERR	"LTT : Transport %s is not present.\n",
			trace_type);
		err = -EINVAL;
		goto traces_error;
	}

	trace->transport = transport;

traces_error:
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_trace_set_type);

int ltt_trace_set_channel_subbufsize(const char *trace_name,
				     const char *channel_name,
				     unsigned int size)
{
	int err = 0;
	struct ltt_trace *trace;
	int index;

	ltt_lock_traces();

	trace = _ltt_trace_find_setup(trace_name);
	if (!trace) {
		printk(KERN_ERR "LTT : Trace not found %s\n", trace_name);
		err = -ENOENT;
		goto traces_error;
	}

	index = ltt_channels_get_index_from_name(channel_name);
	if (index < 0) {
		printk(KERN_ERR "LTT : Channel %s not found\n", channel_name);
		err = -ENOENT;
		goto traces_error;
	}
	trace->settings[index].sb_size = size;

traces_error:
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_trace_set_channel_subbufsize);

int ltt_trace_set_channel_subbufcount(const char *trace_name,
				      const char *channel_name,
				      unsigned int cnt)
{
	int err = 0;
	struct ltt_trace *trace;
	int index;

	ltt_lock_traces();

	trace = _ltt_trace_find_setup(trace_name);
	if (!trace) {
		printk(KERN_ERR "LTT : Trace not found %s\n", trace_name);
		err = -ENOENT;
		goto traces_error;
	}

	index = ltt_channels_get_index_from_name(channel_name);
	if (index < 0) {
		printk(KERN_ERR "LTT : Channel %s not found\n", channel_name);
		err = -ENOENT;
		goto traces_error;
	}
	trace->settings[index].n_sb = cnt;

traces_error:
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_trace_set_channel_subbufcount);

int ltt_trace_set_channel_switch_timer(const char *trace_name,
				       const char *channel_name,
				       unsigned long interval)
{
	int err = 0;
	struct ltt_trace *trace;
	int index;

	ltt_lock_traces();

	trace = _ltt_trace_find_setup(trace_name);
	if (!trace) {
		printk(KERN_ERR "LTT : Trace not found %s\n", trace_name);
		err = -ENOENT;
		goto traces_error;
	}

	index = ltt_channels_get_index_from_name(channel_name);
	if (index < 0) {
		printk(KERN_ERR "LTT : Channel %s not found\n", channel_name);
		err = -ENOENT;
		goto traces_error;
	}
	ltt_channels_trace_set_timer(&trace->settings[index], interval);

traces_error:
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_trace_set_channel_switch_timer);

int ltt_trace_set_channel_overwrite(const char *trace_name,
				    const char *channel_name,
				    unsigned int overwrite)
{
	int err = 0;
	struct ltt_trace *trace;
	int index;

	ltt_lock_traces();

	trace = _ltt_trace_find_setup(trace_name);
	if (!trace) {
		printk(KERN_ERR "LTT : Trace not found %s\n", trace_name);
		err = -ENOENT;
		goto traces_error;
	}

	/*
	 * Always put the metadata channel in non-overwrite mode :
	 * This is a very low traffic channel and it can't afford to have its
	 * data overwritten : this data (marker info) is necessary to be
	 * able to read the trace.
	 */
	if (overwrite && !strcmp(channel_name, "metadata")) {
		printk(KERN_ERR "LTT : Trying to set metadata channel to "
				"overwrite mode\n");
		err = -EINVAL;
		goto traces_error;
	}

	index = ltt_channels_get_index_from_name(channel_name);
	if (index < 0) {
		printk(KERN_ERR "LTT : Channel %s not found\n", channel_name);
		err = -ENOENT;
		goto traces_error;
	}

	trace->settings[index].overwrite = overwrite;

traces_error:
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_trace_set_channel_overwrite);

int ltt_trace_alloc(const char *trace_name)
{
	int err = 0;
	struct ltt_trace *trace;
	int sb_size, n_sb;
	unsigned long flags;
	int chan;
	const char *channel_name;

	ltt_lock_traces();

	trace = _ltt_trace_find_setup(trace_name);
	if (!trace) {
		printk(KERN_ERR "LTT : Trace not found %s\n", trace_name);
		err = -ENOENT;
		goto traces_error;
	}

	kref_init(&trace->kref);
	init_waitqueue_head(&trace->kref_wq);
	trace->active = 0;
	get_trace_clock();
	trace->freq_scale = trace_clock_freq_scale();

	if (!trace->transport) {
		printk(KERN_ERR "LTT : Transport is not set.\n");
		err = -EINVAL;
		goto transport_error;
	}
	if (!try_module_get(trace->transport->owner)) {
		printk(KERN_ERR	"LTT : Can't lock transport module.\n");
		err = -ENODEV;
		goto transport_error;
	}
	trace->ops = &trace->transport->ops;

	err = trace->ops->create_dirs(trace);
	if (err) {
		printk(KERN_ERR	"LTT : Can't create dir for trace %s.\n",
			trace_name);
		goto dirs_error;
	}

	local_irq_save(flags);
	trace->start_freq = trace_clock_frequency();
	trace->start_tsc = trace_clock_read64();
	do_gettimeofday(&trace->start_time);
	local_irq_restore(flags);

	for (chan = 0; chan < trace->nr_channels; chan++) {
		channel_name = ltt_channels_get_name_from_index(chan);
		WARN_ON(!channel_name);
		/*
		 * note: sb_size and n_sb will be overwritten with updated
		 * values by channel creation.
		 */
		sb_size = trace->settings[chan].sb_size;
		n_sb = trace->settings[chan].n_sb;
		prepare_chan_size_num(&sb_size, &n_sb);
		trace->channels[chan] = ltt_create_channel(channel_name,
					      trace, NULL, sb_size, n_sb,
					      trace->settings[chan].overwrite,
					      trace->settings[chan].switch_timer_interval,
					      trace->settings[chan].read_timer_interval);
		if (err != 0) {
			printk(KERN_ERR	"LTT : Can't create channel %s.\n",
				channel_name);
			goto create_channel_error;
		}
	}

	list_del(&trace->list);
	if (list_empty(&ltt_traces.head))
		set_kernel_trace_flag_all_tasks();
	list_add_rcu(&trace->list, &ltt_traces.head);
	synchronize_trace();

	ltt_unlock_traces();

	return 0;

create_channel_error:
	for (chan--; chan >= 0; chan--)
		ltt_channel_destroy(trace->channels[chan]);
	trace->ops->remove_dirs(trace);

dirs_error:
	module_put(trace->transport->owner);
transport_error:
	put_trace_clock();
traces_error:
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_trace_alloc);

/*
 * It is worked as a wrapper for current version of ltt_control.ko.
 * We will make a new ltt_control based on debugfs, and control each channel's
 * buffer.
 */
static
int ltt_trace_create(const char *trace_name, const char *trace_type,
		     enum trace_mode mode,
		     unsigned int subbuf_size_low, unsigned int n_subbufs_low,
		     unsigned int subbuf_size_med, unsigned int n_subbufs_med,
		     unsigned int subbuf_size_high, unsigned int n_subbufs_high)
{
	int err = 0;

	err = ltt_trace_setup(trace_name);
	if (IS_ERR_VALUE(err))
		return err;

	err = ltt_trace_set_type(trace_name, trace_type);
	if (IS_ERR_VALUE(err))
		return err;

	err = ltt_trace_alloc(trace_name);
	if (IS_ERR_VALUE(err))
		return err;

	return err;
}

/* Must be called while sure that trace is in the list. */
static int _ltt_trace_destroy(struct ltt_trace *trace)
{
	int err = -EPERM;

	if (trace == NULL) {
		err = -ENOENT;
		goto traces_error;
	}
	if (trace->active) {
		printk(KERN_ERR
			"LTT : Can't destroy trace %s : tracer is active\n",
			trace->trace_name);
		err = -EBUSY;
		goto active_error;
	}
	/* Everything went fine */
	list_del_rcu(&trace->list);
	synchronize_trace();
	if (list_empty(&ltt_traces.head)) {
		clear_kernel_trace_flag_all_tasks();
	}
	return 0;

	/* error handling */
active_error:
traces_error:
	return err;
}

/* Sleepable part of the destroy */
static void __ltt_trace_destroy(struct ltt_trace *trace)
{
	int i;

	for (i = 0; i < trace->nr_channels; i++)
		ltt_channel_destroy(trace->channels[i]);
	kref_put(&trace->kref, ltt_release_trace);
}

int ltt_trace_destroy(const char *trace_name)
{
	int err = 0;
	struct ltt_trace *trace;

	ltt_lock_traces();

	trace = _ltt_trace_find(trace_name);
	if (trace) {
		err = _ltt_trace_destroy(trace);
		if (err)
			goto error;

		__ltt_trace_destroy(trace);
		ltt_unlock_traces();
		put_trace_clock();

		return 0;
	}

	trace = _ltt_trace_find_setup(trace_name);
	if (trace) {
		_ltt_trace_free(trace);
		ltt_unlock_traces();
		return 0;
	}

	err = -ENOENT;

	/* Error handling */
error:
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_trace_destroy);

/* must be called from within a traces lock. */
static int _ltt_trace_start(struct ltt_trace *trace)
{
	int err = 0;

	if (trace == NULL) {
		err = -ENOENT;
		goto traces_error;
	}
	if (trace->active)
		printk(KERN_INFO "LTT : Tracing already active for trace %s\n",
				trace->trace_name);
	if (!try_module_get(ltt_run_filter_owner)) {
		err = -ENODEV;
		printk(KERN_ERR "LTT : Can't lock filter module.\n");
		goto get_ltt_run_filter_error;
	}
	trace->active = 1;
	/* Read by trace points without protection : be careful */
	ltt_traces.num_active_traces++;
	return err;

	/* error handling */
get_ltt_run_filter_error:
traces_error:
	return err;
}

int ltt_trace_start(const char *trace_name)
{
	int err = 0;
	struct ltt_trace *trace;

	ltt_lock_traces();

	trace = _ltt_trace_find(trace_name);
	err = _ltt_trace_start(trace);
	if (err)
		goto no_trace;

	ltt_unlock_traces();

	/*
	 * Call the kernel state dump.
	 * Events will be mixed with real kernel events, it's ok.
	 * Notice that there is no protection on the trace : that's exactly
	 * why we iterate on the list and check for trace equality instead of
	 * directly using this trace handle inside the logging function.
	 */

	ltt_dump_marker_state(trace);

	if (!try_module_get(ltt_statedump_owner)) {
		err = -ENODEV;
		printk(KERN_ERR
			"LTT : Can't lock state dump module.\n");
	} else {
		ltt_statedump_functor(trace);
		module_put(ltt_statedump_owner);
	}

	return err;

	/* Error handling */
no_trace:
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_trace_start);

/* must be called from within traces lock */
static int _ltt_trace_stop(struct ltt_trace *trace)
{
	int err = -EPERM;

	if (trace == NULL) {
		err = -ENOENT;
		goto traces_error;
	}
	if (!trace->active)
		printk(KERN_INFO "LTT : Tracing not active for trace %s\n",
				trace->trace_name);
	if (trace->active) {
		trace->active = 0;
		ltt_traces.num_active_traces--;
		synchronize_trace(); /* Wait for each tracing to be finished */
	}
	module_put(ltt_run_filter_owner);
	/* Everything went fine */
	return 0;

	/* Error handling */
traces_error:
	return err;
}

int ltt_trace_stop(const char *trace_name)
{
	int err = 0;
	struct ltt_trace *trace;

	ltt_lock_traces();
	trace = _ltt_trace_find(trace_name);
	err = _ltt_trace_stop(trace);
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_trace_stop);

/**
 * ltt_control - Trace control in-kernel API
 * @msg: Action to perform
 * @trace_name: Trace on which the action must be done
 * @trace_type: Type of trace (normal, flight, hybrid)
 * @args: Arguments specific to the action
 */
int ltt_control(enum ltt_control_msg msg, const char *trace_name,
		const char *trace_type, union ltt_control_args args)
{
	int err = -EPERM;

	printk(KERN_ALERT "ltt_control : trace %s\n", trace_name);
	switch (msg) {
	case LTT_CONTROL_START:
		printk(KERN_DEBUG "Start tracing %s\n", trace_name);
		err = ltt_trace_start(trace_name);
		break;
	case LTT_CONTROL_STOP:
		printk(KERN_DEBUG "Stop tracing %s\n", trace_name);
		err = ltt_trace_stop(trace_name);
		break;
	case LTT_CONTROL_CREATE_TRACE:
		printk(KERN_DEBUG "Creating trace %s\n", trace_name);
		err = ltt_trace_create(trace_name, trace_type,
			args.new_trace.mode,
			args.new_trace.subbuf_size_low,
			args.new_trace.n_subbufs_low,
			args.new_trace.subbuf_size_med,
			args.new_trace.n_subbufs_med,
			args.new_trace.subbuf_size_high,
			args.new_trace.n_subbufs_high);
		break;
	case LTT_CONTROL_DESTROY_TRACE:
		printk(KERN_DEBUG "Destroying trace %s\n", trace_name);
		err = ltt_trace_destroy(trace_name);
		break;
	}
	return err;
}
EXPORT_SYMBOL_GPL(ltt_control);

/**
 * ltt_filter_control - Trace filter control in-kernel API
 * @msg: Action to perform on the filter
 * @trace_name: Trace on which the action must be done
 */
int ltt_filter_control(enum ltt_filter_control_msg msg, const char *trace_name)
{
	int err;
	struct ltt_trace *trace;

	printk(KERN_DEBUG "ltt_filter_control : trace %s\n", trace_name);
	ltt_lock_traces();
	trace = _ltt_trace_find(trace_name);
	if (trace == NULL) {
		printk(KERN_ALERT
			"Trace does not exist. Cannot proxy control request\n");
		err = -ENOENT;
		goto trace_error;
	}
	if (!try_module_get(ltt_filter_control_owner)) {
		err = -ENODEV;
		goto get_module_error;
	}
	switch (msg) {
	case LTT_FILTER_DEFAULT_ACCEPT:
		printk(KERN_DEBUG
			"Proxy filter default accept %s\n", trace_name);
		err = (*ltt_filter_control_functor)(msg, trace);
		break;
	case LTT_FILTER_DEFAULT_REJECT:
		printk(KERN_DEBUG
			"Proxy filter default reject %s\n", trace_name);
		err = (*ltt_filter_control_functor)(msg, trace);
		break;
	default:
		err = -EPERM;
	}
	module_put(ltt_filter_control_owner);

get_module_error:
trace_error:
	ltt_unlock_traces();
	return err;
}
EXPORT_SYMBOL_GPL(ltt_filter_control);

int __init ltt_init(void)
{
	/* Make sure no page fault can be triggered by this module */
	vmalloc_sync_all();
	init_timer_deferrable(&ltt_async_wakeup_timer);
	return 0;
}

module_init(ltt_init)

static void __exit ltt_exit(void)
{
	struct ltt_trace *trace;
	struct list_head *pos, *n;

	ltt_lock_traces();
	/* Stop each trace, currently being read by RCU read-side */
	list_for_each_entry_rcu(trace, &ltt_traces.head, list)
		_ltt_trace_stop(trace);
	/* Wait for quiescent state. Readers have preemption disabled. */
	synchronize_trace();
	/* Safe iteration is now permitted. It does not have to be RCU-safe
	 * because no readers are left. */
	list_for_each_safe(pos, n, &ltt_traces.head) {
		trace = container_of(pos, struct ltt_trace, list);
		/* _ltt_trace_destroy does a synchronize_trace() */
		_ltt_trace_destroy(trace);
		__ltt_trace_destroy(trace);
	}
	/* free traces in pre-alloc status */
	list_for_each_safe(pos, n, &ltt_traces.setup_head) {
		trace = container_of(pos, struct ltt_trace, list);
		_ltt_trace_free(trace);
	}

	ltt_unlock_traces();
}

module_exit(ltt_exit)

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Mathieu Desnoyers");
MODULE_DESCRIPTION("Linux Trace Toolkit Next Generation Tracer Kernel API");
