/**
 * @file clock.c
 * @note Copyright (C) 2011 Richard Cochran <richardcochran@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bmc.h"
#include "clock.h"
#include "foreign.h"
#include "mave.h"
#include "missing.h"
#include "msg.h"
#include "phc.h"
#include "port.h"
#include "servo.h"
#include "print.h"
#include "tmv.h"
#include "util.h"

#define MAVE_LENGTH 10

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct clock {
	clockid_t clkid;
	struct servo *servo;
	struct defaultDS dds;
	struct dataset default_dataset;
	struct currentDS cur;
	struct parentDS dad;
	struct timePropertiesDS tds;
	struct foreign_clock *best;
	struct port *port[MAX_PORTS];
	struct pollfd pollfd[MAX_PORTS*N_POLLFD];
	int nports;
	tmv_t master_offset;
	tmv_t path_delay;
	struct mave *avg_delay;
	tmv_t c1;
	tmv_t c2;
	tmv_t t1;
	tmv_t t2;
};

struct clock the_clock;

static void handle_state_decision_event(struct clock *c);

static void clock_destroy(struct clock *c)
{
	int i;
	for (i = 0; i < c->nports; i++) {
		port_close(c->port[i]);
	}
	if (c->clkid != CLOCK_REALTIME) {
		phc_close(c->clkid);
	}
	memset(c, 0, sizeof(*c));
}

static void clock_ppb(clockid_t clkid, double ppb)
{
	struct timex tx;
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_FREQUENCY;
	tx.freq = (long) (ppb * 65.536);
	if (clock_adjtime(clkid, &tx) < 0)
		pr_err("failed to adjust the clock: %m");
}

static void clock_step(clockid_t clkid, int64_t ns)
{
	struct timex tx;
	int sign = 1;
	if (ns < 0) {
		sign = -1;
		ns *= -1;
	}
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_SETOFFSET | ADJ_NANO;
	tx.time.tv_sec  = sign * (ns / NS_PER_SEC);
	tx.time.tv_usec = sign * (ns % NS_PER_SEC);
	/*
	 * The value of a timeval is the sum of its fields, but the
	 * field tv_usec must always be non-negative.
	 */
	if (tx.time.tv_usec < 0) {
		tx.time.tv_sec  -= 1;
		tx.time.tv_usec += 1000000000;
	}
	if (clock_adjtime(clkid, &tx) < 0)
		pr_err("failed to step clock: %m");
}

static void clock_update_grandmaster(struct clock *c)
{
	memset(&c->cur, 0, sizeof(c->cur));
	c->dad.parentPortIdentity.clockIdentity = c->dds.clockIdentity;
	c->dad.parentPortIdentity.portNumber    = 0;
	c->dad.grandmasterIdentity              = c->dds.clockIdentity;
	c->dad.grandmasterClockQuality          = c->dds.clockQuality;
	c->dad.grandmasterPriority1             = c->dds.priority1;
	c->dad.grandmasterPriority2             = c->dds.priority2;
	c->tds.currentUtcOffset                 = CURRENT_UTC_OFFSET;
	c->tds.currentUtcOffsetValid            = FALSE;
	c->tds.leap61                           = FALSE;
	c->tds.leap59                           = FALSE;
	c->tds.timeTraceable                    = FALSE;
	c->tds.frequencyTraceable               = FALSE;
	c->tds.ptpTimescale                     = TRUE;
	c->tds.timeSource                       = INTERNAL_OSCILLATOR;
}

static void clock_update_slave(struct clock *c)
{
	struct ptp_message *msg        = TAILQ_FIRST(&c->best->messages);
	c->cur.stepsRemoved            = 1 + c->best->dataset.stepsRemoved;
	c->dad.parentPortIdentity      = c->best->dataset.sender;
	c->dad.grandmasterIdentity     = msg->announce.grandmasterIdentity;
	c->dad.grandmasterClockQuality = msg->announce.grandmasterClockQuality;
	c->dad.grandmasterPriority1    = msg->announce.grandmasterPriority1;
	c->dad.grandmasterPriority2    = msg->announce.grandmasterPriority2;
	c->tds.currentUtcOffset        = msg->announce.currentUtcOffset;
	c->tds.currentUtcOffsetValid   = field_is_set(msg, 1, UTC_OFF_VALID);
	c->tds.leap61                  = field_is_set(msg, 1, LEAP_61);
	c->tds.leap59                  = field_is_set(msg, 1, LEAP_59);
	c->tds.timeTraceable           = field_is_set(msg, 1, TIME_TRACEABLE);
	c->tds.frequencyTraceable      = field_is_set(msg, 1, FREQ_TRACEABLE);
	c->tds.ptpTimescale            = field_is_set(msg, 1, PTP_TIMESCALE);
	c->tds.timeSource              = msg->announce.timeSource;
}

/* public methods */

UInteger8 clock_class(struct clock *c)
{
	return c->dds.clockQuality.clockClass;
}

struct clock *clock_create(char *phc, struct interface *iface, int count,
			   struct defaultDS *ds)
{
	int i, max_adj, sw_ts = 0;
	struct clock *c = &the_clock;

	srandom(time(NULL));

	if (c->nports)
		clock_destroy(c);

	if (phc) {
		c->clkid = phc_open(phc);
		if (c->clkid == CLOCK_INVALID) {
			pr_err("Failed to open %s: %m", phc);
			return NULL;
		}
		max_adj = phc_max_adj(c->clkid);
		if (!max_adj) {
			pr_err("clock is not adjustable");
			return NULL;
		}
	} else {
		c->clkid = CLOCK_REALTIME;
		max_adj = 512000;
	}

	for (i = 0; i < count; i++) {
		if (iface[i].timestamping == TS_SOFTWARE) {
			sw_ts = 1;
			break;
		}
	}

	c->servo = servo_create("pi", max_adj, sw_ts);
	if (!c->servo) {
		pr_err("Failed to create clock servo");
		return NULL;
	}
	c->avg_delay = mave_create(MAVE_LENGTH);
	if (!c->avg_delay) {
		pr_err("Failed to create moving average");
		return NULL;
	}

	c->dds = *ds;

	/* Initialize the parentDS. */
	c->dad.parentPortIdentity.clockIdentity      = c->dds.clockIdentity;
	c->dad.parentPortIdentity.portNumber         = 0;
	c->dad.parentStats                           = 0;
	c->dad.observedParentOffsetScaledLogVariance = 0xffff;
	c->dad.observedParentClockPhaseChangeRate    = 0x7fffffff;
	c->dad.grandmasterPriority1                  = c->dds.priority1;
	c->dad.grandmasterClockQuality               = c->dds.clockQuality;
	c->dad.grandmasterPriority2                  = c->dds.priority1;
	c->dad.grandmasterIdentity                   = c->dds.clockIdentity;

	for (i = 0; i < ARRAY_SIZE(c->pollfd); i++) {
		c->pollfd[i].fd = -1;
		c->pollfd[i].events = 0;
	}

	for (i = 0; i < count; i++) {
		c->port[i] = port_open(iface[i].name, iface[i].transport,
				       iface[i].timestamping, 1+i, DM_E2E, c);
		if (!c->port[i]) {
			pr_err("failed to open port %s", iface[i].name);
			return NULL;
		}
	}

	c->dds.numberPorts = c->nports = count;

	for (i = 0; i < c->nports; i++)
		port_dispatch(c->port[i], EV_INITIALIZE);

	return c;
}

struct dataset *clock_best_foreign(struct clock *c)
{
	return c->best ? &c->best->dataset : NULL;
}

struct port *clock_best_port(struct clock *c)
{
	return c->best ? c->best->port : NULL;
}

struct dataset *clock_default_ds(struct clock *c)
{
	struct dataset *out = &c->default_dataset;
	struct defaultDS *in = &c->dds;

	out->priority1              = in->priority1;
	out->identity               = in->clockIdentity;
	out->quality                = in->clockQuality;
	out->priority2              = in->priority2;
	out->stepsRemoved           = 0;
	out->sender.clockIdentity   = in->clockIdentity;
	out->sender.portNumber      = 0;
	out->receiver.clockIdentity = in->clockIdentity;
	out->receiver.portNumber    = 0;

	return out;
}

UInteger8 clock_domain_number(struct clock *c)
{
	return c->dds.domainNumber;
}

struct ClockIdentity clock_identity(struct clock *c)
{
	return c->dds.clockIdentity;
}

void clock_install_fda(struct clock *c, struct port *p, struct fdarray fda)
{
	int i, j, k;
	for (i = 0; i < c->nports; i++) {
		if (p == c->port[i])
			break;
	}
	for (j = 0; j < fda.cnt; j++) {
		k = N_POLLFD * i + j;
		c->pollfd[k].fd = fda.fd[j];
		c->pollfd[k].events = POLLIN|POLLPRI;
	}
}

struct PortIdentity clock_parent_identity(struct clock *c)
{
	return c->dad.parentPortIdentity;
}

int clock_poll(struct clock *c)
{
	int cnt, i, j, k, sde = 0;
	enum fsm_event event;

	cnt = poll(c->pollfd, ARRAY_SIZE(c->pollfd), -1);
	if (cnt < 0) {
		if (EINTR == errno) {
			return 0;
		} else {
			pr_emerg("poll failed");
			return -1;
		}
	} else if (!cnt) {
		return 0;
	}

	for (i = 0; i < c->nports; i++) {
		for (j = 0; j < N_POLLFD; j++) {
			k = N_POLLFD * i + j;
			if (c->pollfd[k].revents & (POLLIN|POLLPRI)) {
				event = port_event(c->port[i], j);
				if (EV_STATE_DECISION_EVENT == event)
					sde = 1;
				else
					port_dispatch(c->port[i], event);
			}
		}
	}

	if (sde)
		handle_state_decision_event(c);

	return 0;
}

void clock_path_delay(struct clock *c, struct timespec req, struct timestamp rx,
		      Integer64 correction)
{
	tmv_t c1, c2, c3, pd, t1, t2, t3, t4;

	c1 = c->c1;
	c2 = c->c2;
	c3 = correction_to_tmv(correction);
	t1 = c->t1;
	t2 = c->t2;
	t3 = timespec_to_tmv(req);
	t4 = timestamp_to_tmv(rx);

	/*
	 * c->path_delay = (t2 - t3) + (t4 - t1);
	 * c->path_delay -= c_sync + c_fup + c_delay_resp;
	 * c->path_delay /= 2.0;
	 */
	pd = tmv_add(tmv_sub(t2, t3), tmv_sub(t4, t1));
	pd = tmv_sub(pd, tmv_add(c1, tmv_add(c2, c3)));
	pd = tmv_div(pd, 2);

	if (pd < 0) {
		pr_debug("negative path delay %10lld", pd);
		pr_debug("path_delay = (t2 - t3) + (t4 - t1)");
		pr_debug("t2 - t3 = %+10lld", t2 - t3);
		pr_debug("t4 - t1 = %+10lld", t4 - t1);
		pr_debug("c1 %10lld", c1);
		pr_debug("c2 %10lld", c2);
		pr_debug("c3 %10lld", c3);
		return;
	}

	c->path_delay = mave_accumulate(c->avg_delay, pd);

	pr_debug("path delay    %10lld %10lld", c->path_delay, pd);
}

int clock_slave_only(struct clock *c)
{
	return c->dds.slaveOnly;
}

void clock_synchronize(struct clock *c,
		       struct timespec ingress_ts, struct timestamp origin_ts,
		       Integer64 correction1, Integer64 correction2)
{
	double adj;
	tmv_t ingress, origin;
	enum servo_state state;

	ingress = timespec_to_tmv(ingress_ts);
	origin  = timestamp_to_tmv(origin_ts);

	c->t1 = origin;
	c->t2 = ingress;

	c->c1 = correction_to_tmv(correction1);
	c->c2 = correction_to_tmv(correction2);

	/*
	 * c->master_offset = ingress - origin - c->path_delay - c->c1 - c->c2;
	 */
	c->master_offset = tmv_sub(ingress,
		tmv_add(origin, tmv_add(c->path_delay, tmv_add(c->c1, c->c2))));

	if (!c->path_delay)
		return;

	adj = servo_sample(c->servo, c->master_offset, ingress, &state);

	pr_debug("master offset %10lld s%d adj %+7.0f",
		 c->master_offset, state, adj);

	switch (state) {
	case SERVO_UNLOCKED:
		break;
	case SERVO_JUMP:
		clock_step(c->clkid, -c->master_offset);
		break;
	case SERVO_LOCKED:
		clock_ppb(c->clkid, -adj);
		break;
	}
}

static void handle_state_decision_event(struct clock *c)
{
	struct foreign_clock *best = NULL, *fc;
	int i;

	for (i = 0; i < c->nports; i++) {
		fc = port_compute_best(c->port[i]);
		if (!fc)
			continue;
		if (!best || dscmp(&fc->dataset, &best->dataset) > 0)
			best = fc;
	}

	if (!best)
		return;

	pr_info("selected best master clock %s",
		cid2str(&best->dataset.identity));

	if (c->best != best)
		mave_reset(c->avg_delay);

	c->best = best;

	for (i = 0; i < c->nports; i++) {
		enum port_state ps;
		enum fsm_event event;
		ps = bmc_state_decision(c, c->port[i]);
		switch (ps) {
		case PS_LISTENING:
			event = EV_NONE;
			break;
		case PS_GRAND_MASTER:
			clock_update_grandmaster(c);
			/*fall through*/
		case PS_MASTER:
			event = EV_RS_MASTER;
			break;
		case PS_PASSIVE:
			event = EV_RS_PASSIVE;
			break;
		case PS_SLAVE:
			clock_update_slave(c);
			event = EV_RS_SLAVE;
			break;
		default:
			event = EV_INITIALIZE;
			break;
		}
		port_dispatch(c->port[i], event);
	}
}
