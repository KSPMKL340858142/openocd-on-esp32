// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Copyright (C) 2016-2020 by Marc Schink <dev@zapb.de>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <helper/log.h>
#include <helper/list.h>
#include <target/target.h>
#include <target/rtt.h>

#include "rtt.h"

static struct {
	struct rtt_source source;
	/** Control block. */
	struct rtt_control ctrl;
	struct target *target;
	/** Start address to search for the control block. */
	target_addr_t addr;
	/** Size of the control block search area. */
	size_t size;
	/** Control block identifier. */
	char id[RTT_CB_MAX_ID_LENGTH];
	/** Whether RTT is configured. */
	bool configured;
	/** Whether RTT is started. */
	bool started;
	/** Whether configuration changed. */
	bool changed;
	/** Whether the control block was found. */
	bool found_cb;

	struct rtt_sink_list **sink_list;
	size_t sink_list_length;

	unsigned int polling_interval;
} rtt;

int rtt_init(void)
{
	rtt.sink_list_length = 1;
	rtt.sink_list = calloc(rtt.sink_list_length,
		sizeof(struct rtt_sink_list *));

	if (!rtt.sink_list)
		return ERROR_FAIL;

	rtt.sink_list[0] = NULL;
	rtt.started = false;

	rtt.polling_interval = 100;

	return ERROR_OK;
}

int rtt_exit(void)
{
	free(rtt.sink_list);

	return ERROR_OK;
}

static int read_channel_callback(void *user_data)
{
	int ret;

	ret = rtt.source.read(rtt.target, &rtt.ctrl, rtt.sink_list,
		rtt.sink_list_length, NULL);

	if (ret != ERROR_OK) {
		/* 目标复位/Flash 擦写期间读内存失败是暂时的:
		 * 保留轮询定时器, 下一周期自动重试 (原实现会注销轮询并关闭
		 * rtt server, 导致每次烧录后 9000 端口消失) */
		return ERROR_OK;
	}

	return ERROR_OK;
}

int rtt_setup(target_addr_t address, size_t size, const char *id)
{
	size_t id_length = strlen(id);

	if (!id_length || id_length >= RTT_CB_MAX_ID_LENGTH) {
		LOG_ERROR("rtt: Invalid control block ID");
		return ERROR_COMMAND_ARGUMENT_INVALID;
	}

	rtt.addr = address;
	rtt.size = size;
	strncpy(rtt.id, id, id_length + 1);
	rtt.changed = true;
	rtt.configured = true;

	return ERROR_OK;
}

int rtt_register_source(const struct rtt_source source,
		struct target *target)
{
	if (!source.find_cb || !source.read_cb || !source.read_channel_info)
		return ERROR_FAIL;

	if (!source.start || !source.stop)
		return ERROR_FAIL;

	if (!source.read || !source.write)
		return ERROR_FAIL;

	rtt.source = source;
	rtt.target = target;

	return ERROR_OK;
}

int rtt_start(void)
{
	int ret;
	target_addr_t addr = rtt.addr;

	if (rtt.started)
		return ERROR_OK;

	if (!rtt.found_cb || rtt.changed) {
		rtt.source.find_cb(rtt.target, &addr, rtt.size, rtt.id,
			&rtt.found_cb, NULL);

		rtt.changed = false;

		if (rtt.found_cb) {
			LOG_INFO("rtt: Control block found at 0x%" TARGET_PRIxADDR,
				addr);
			rtt.ctrl.address = addr;
		} else {
			LOG_INFO("rtt: No control block found");
			return ERROR_OK;
		}
	}

	ret = rtt.source.read_cb(rtt.target, rtt.ctrl.address, &rtt.ctrl, NULL);

	if (ret != ERROR_OK)
		return ret;

	ret = rtt.source.start(rtt.target, &rtt.ctrl, NULL);

	if (ret != ERROR_OK)
		return ret;

	target_register_timer_callback(&read_channel_callback,
		rtt.polling_interval, 1, NULL);
	rtt.started = true;

	return ERROR_OK;
}

int rtt_stop(void)
{
	int ret;

	if (!rtt.configured) {
		LOG_ERROR("rtt: Not configured");
		return ERROR_FAIL;
	}

	target_unregister_timer_callback(&read_channel_callback, NULL);
	rtt.started = false;

	ret = rtt.source.stop(rtt.target, NULL);

	if (ret != ERROR_OK)
		return ret;

	return ERROR_OK;
}

static int adjust_sink_list(size_t length)
{
	struct rtt_sink_list **tmp;

	if (length <= rtt.sink_list_length)
		return ERROR_OK;

	tmp = realloc(rtt.sink_list, sizeof(struct rtt_sink_list *) * length);

	if (!tmp)
		return ERROR_FAIL;

	for (size_t i = rtt.sink_list_length; i < length; i++)
		tmp[i] = NULL;

	rtt.sink_list = tmp;
	rtt.sink_list_length = length;

	return ERROR_OK;
}

int rtt_register_sink(unsigned int channel_index, rtt_sink_read read,
		void *user_data)
{
	struct rtt_sink_list *tmp;

	if (channel_index >= rtt.sink_list_length) {
		if (adjust_sink_list(channel_index + 1) != ERROR_OK)
			return ERROR_FAIL;
	}

	LOG_DEBUG("rtt: Registering sink for channel %u", channel_index);

	tmp = malloc(sizeof(struct rtt_sink_list));

	if (!tmp)
		return ERROR_FAIL;

	tmp->read = read;
	tmp->user_data = user_data;
	tmp->next = rtt.sink_list[channel_index];

	rtt.sink_list[channel_index] = tmp;

	return ERROR_OK;
}

int rtt_unregister_sink(unsigned int channel_index, rtt_sink_read read,
		void *user_data)
{
	struct rtt_sink_list *prev_sink;

	LOG_DEBUG("rtt: Unregistering sink for channel %u", channel_index);

	if (channel_index >= rtt.sink_list_length)
		return ERROR_FAIL;

	prev_sink = rtt.sink_list[channel_index];

	for (struct rtt_sink_list *sink = rtt.sink_list[channel_index]; sink;
			prev_sink = sink, sink = sink->next) {
		if (sink->read == read && sink->user_data == user_data) {

			if (sink == rtt.sink_list[channel_index])
				rtt.sink_list[channel_index] = sink->next;
			else
				prev_sink->next = sink->next;

			free(sink);

			return ERROR_OK;
		}
	}

	return ERROR_OK;
}

/** Black-hole sink: counts the bytes it is handed and drops them. */
static int discard_sink(unsigned int channel, const uint8_t *buffer,
		size_t length, void *user_data)
{
	size_t *discarded = user_data;

	(void)channel;
	(void)buffer;

	if (discarded)
		*discarded += length;

	return ERROR_OK;
}

int rtt_discard_channel_data(unsigned int channel_index)
{
	int ret;
	size_t discarded = 0;

	if (!rtt_started())
		return ERROR_OK;

	/* The source only visits channels that have a sink attached, so hang a
	 * black-hole sink on it temporarily to make this channel eligible for
	 * reading. Whatever comes back lands in discard_sink() instead of a client.
	 */
	ret = rtt_register_sink(channel_index, &discard_sink, &discarded);
	if (ret != ERROR_OK)
		return ret;

	/* The source returns at most 1024 bytes per call, so keep pulling until a
	 * round yields nothing (the target-side buffer is then empty). The cap is
	 * only a guard against a source that never reports progress. */
	for (unsigned int i = 0; i < 64; i++) {
		size_t before = discarded;

		ret = rtt.source.read(rtt.target, &rtt.ctrl, rtt.sink_list,
				rtt.sink_list_length, NULL);
		if (ret != ERROR_OK)
			break;

		if (discarded == before)
			break;
	}

	rtt_unregister_sink(channel_index, &discard_sink, &discarded);

	/* Only report when something was actually thrown away, so a clean buffer
	 * (client reconnecting quickly) stays quiet. */
	if (discarded)
		LOG_INFO("rtt: Discarded %zu stale bytes from up-channel %u",
				discarded, channel_index);

	return (ret == ERROR_OK) ? ERROR_OK : ret;
}

int rtt_get_polling_interval(unsigned int *interval)
{
	if (!interval)
		return ERROR_FAIL;

	*interval = rtt.polling_interval;

	return ERROR_OK;
}

int rtt_set_polling_interval(unsigned int interval)
{
	if (!interval)
		return ERROR_FAIL;

	if (rtt.polling_interval != interval) {
		target_unregister_timer_callback(&read_channel_callback, NULL);
		target_register_timer_callback(&read_channel_callback, interval, 1,
			NULL);
	}

	rtt.polling_interval = interval;

	return ERROR_OK;
}

int rtt_write_channel(unsigned int channel_index, const uint8_t *buffer,
		size_t *length)
{
	if (channel_index >= rtt.ctrl.num_up_channels) {
		LOG_WARNING("rtt: Down-channel %u is not available", channel_index);
		return ERROR_OK;
	}

	return rtt.source.write(rtt.target, &rtt.ctrl, channel_index, buffer,
		length, NULL);
}

bool rtt_started(void)
{
	return rtt.started;
}

bool rtt_configured(void)
{
	return rtt.configured;
}

bool rtt_found_cb(void)
{
	return rtt.found_cb;
}

const struct rtt_control *rtt_get_control(void)
{
	return &rtt.ctrl;
}

int rtt_read_channel_info(unsigned int channel_index,
	enum rtt_channel_type type, struct rtt_channel_info *info)
{
	return rtt.source.read_channel_info(rtt.target, &rtt.ctrl,
		channel_index, type, info, NULL);
}
