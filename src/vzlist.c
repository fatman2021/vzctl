/*
 *  Copyright (C) 2000-2012, Parallels, Inc. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include <getopt.h>
#include <fnmatch.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/vzcalluser.h>

#include "vzlist.h"
#include "config.h"
#include "fs.h"
#include "res.h"
#include "logger.h"
#include "util.h"
#include "types.h"

static struct Cveinfo *veinfo = NULL;
static int n_veinfo = 0;

static char g_outbuffer[4096] = "";
static char *p_outbuffer = g_outbuffer;
static char *e_buf = g_outbuffer + sizeof(g_outbuffer) - 1;
static char *host_pattern = NULL;
static char *name_pattern = NULL;
static char *desc_pattern = NULL;
static char *dumpdir = NULL;
static int vzctlfd;
static struct Cfield_order *g_field_order = NULL;
static int is_last_field;
static char *default_field_order = "veid,numproc,status,ip,hostname";
static char *default_nm_field_order = "veid,numproc,status,ip,name";
static int g_sort_field = 0;
static int *g_ve_list = NULL;
static int n_ve_list = 0;
static int veid_only = 0;
static int sort_rev = 0;
static int show_hdr = 1;
static int all_ve = 0;
static int only_stopped_ve = 0;
static int with_names = 0;
static long __clk_tck = -1;

char logbuf[32];
char *plogbuf = logbuf;
static int get_run_ve_proc(int);
#if HAVE_VZLIST_IOCTL
static int get_run_ve_ioctl(int);
static inline int get_run_ve(int update)
{
	int ret;
	ret = get_run_ve_ioctl(update);
	if (ret)
		ret = get_run_ve_proc(update);
	return ret;
}
#else
#define get_run_ve get_run_ve_proc
#endif


/* Print functions */
#define PRINT_STR_FIELD(fieldname, length) \
static void print_ ## fieldname(struct Cveinfo *p, int index) \
{ \
	int r; \
	char *str = "-"; \
 \
	if (p->fieldname != NULL) \
		str = p->fieldname; \
	r = snprintf(p_outbuffer, e_buf - p_outbuffer, \
		"%-" #length "s", str); \
	if (!is_last_field) \
		r = length; \
	p_outbuffer += r; \
}

PRINT_STR_FIELD(hostname, 32)
PRINT_STR_FIELD(name, 32)
PRINT_STR_FIELD(description, 32)
PRINT_STR_FIELD(ostemplate, 32)

static void print_ip(struct Cveinfo *p, int index)
{
	int r;
	char *str = "-";
	char *ch;

	if (p->ip != NULL)
		str = p->ip;
	if (!is_last_field)
	{
		/* Fixme: dont destroy original string */
		if ((ch = strchr(str, ' ')) != NULL)
			*ch = 0;
	}
	r = snprintf(p_outbuffer, e_buf - p_outbuffer, "%-15s", str);
	if (!is_last_field)
		r = 15;
	p_outbuffer += r;
}

static void print_veid(struct Cveinfo *p, int index)
{
	p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%10d", p->veid);
}

static void print_status(struct Cveinfo *p, int index)
{
	p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%-9s", ve_status[p->status]);
}

static void print_laverage(struct Cveinfo *p, int index)
{
	if (p->cpustat == NULL)
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%14s", "-");
	else
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%1.2f/%1.2f/%1.2f",
			p->cpustat->la[0], p->cpustat->la[1], p->cpustat->la[2]);
}

static void print_uptime(struct Cveinfo *p, int index)
{
	if (p->cpustat == NULL)
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer,
				"%15s", "-");
	else
	{
		unsigned int days, hours, min, secs;

		days  = (unsigned int)(p->cpustat->uptime / (60 * 60 * 24));
		min = (unsigned int)(p->cpustat->uptime / 60);
		hours = min / 60;
		hours = hours % 24;
		min = min % 60;
		secs = (unsigned int)(p->cpustat->uptime -
				(60ull * min + 60ull * 60 * hours +
				 60ull * 60 * 24 * days));

		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer,
				"%.3dd%.2dh:%.2dm:%.2ds",
				days, hours, min, secs);
	}
}

static void print_cpulimit(struct Cveinfo *p, int index)
{
	if (p->cpu == NULL)
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%7s", "-");
	else
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%7lu",
			p->cpu->limit[index]);
}

static void print_ioprio(struct Cveinfo *p, int index)
{
	if (p->io.ioprio < 0)
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%3s", "-");
	else
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%3d",
			p->io.ioprio);
}

static void print_onboot(struct Cveinfo *p, int index)
{
	p_outbuffer += snprintf(p_outbuffer, e_buf-p_outbuffer,
			"%6s", p->onboot == YES ? "yes" : "no");
}

static void print_bootorder(struct Cveinfo *p, int index)
{
	if (p->bootorder == NULL)
		p_outbuffer += snprintf(p_outbuffer, e_buf-p_outbuffer,
				"%10s", "-");
	else
		p_outbuffer += snprintf(p_outbuffer, e_buf-p_outbuffer,
				"%10lu", p->bootorder[index]);
}

static void print_cpunum(struct Cveinfo *p, int index)
{
	if (p->cpunum <= 0)
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer,
				"%5s", "-");
	else
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer,
				"%5d", p->cpunum);
}

#define PRINT_UBC(name)							\
static void print_ubc_ ## name(struct Cveinfo *p, int index)		\
{									\
	if (p->ubc == NULL ||						\
		(p->status != VE_RUNNING &&				\
			(index == 0 || index == 1 || index == 4)))	\
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%10s", "-");	\
	else								\
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%10lu",		\
					p->ubc->name[index]);		\
}									\

PRINT_UBC(kmemsize)
PRINT_UBC(lockedpages)
PRINT_UBC(privvmpages)
PRINT_UBC(shmpages)
PRINT_UBC(numproc)
PRINT_UBC(physpages)
PRINT_UBC(vmguarpages)
PRINT_UBC(oomguarpages)
PRINT_UBC(numtcpsock)
PRINT_UBC(numflock)
PRINT_UBC(numpty)
PRINT_UBC(numsiginfo)
PRINT_UBC(tcpsndbuf)
PRINT_UBC(tcprcvbuf)
PRINT_UBC(othersockbuf)
PRINT_UBC(dgramrcvbuf)
PRINT_UBC(numothersock)
PRINT_UBC(dcachesize)
PRINT_UBC(numfile)
PRINT_UBC(numiptent)
PRINT_UBC(swappages)

#define PRINT_DQ(name)							\
static void print_ ## name(struct Cveinfo *p, int index)		\
{									\
	if (p->quota == NULL ||						\
		(p->status != VE_RUNNING && (index == 0)))		\
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%10s", "-");	\
	else								\
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer, "%10lu",		\
					p->quota->name[index]);		\
}									\

PRINT_DQ(diskspace)
PRINT_DQ(diskinodes)

/* Sort functions */

static inline int check_empty_param(const void *val1, const void *val2)
{
	if (val1 == NULL && val2 == NULL)
		return 0;
	else if (val1 == NULL)
		return -1;
	else if (val2 == NULL)
		return 1;
	return 2;
}

static int none_sort_fn(const void *val1, const void *val2)
{
	return 0;
}

static int laverage_sort_fn(const void *val1, const void *val2)
{
	const struct Ccpustat *st1 = ((const struct Cveinfo *)val1)->cpustat;
	const struct Ccpustat *st2 = ((const struct Cveinfo *)val2)->cpustat;
	int res;

	if ((res = check_empty_param(st1, st2)) != 2)
		return res;
	res = (st1->la[0] - st2->la[0]) * 100;
	if (res != 0)
		return res;
	res = (st1->la[1] - st2->la[1]) * 100;
	if (res != 0)
		return res;
	return (st1->la[2] - st2->la[2]) * 100;
}

static int uptime_sort_fn(const void *val1, const void *val2)
{
	struct Ccpustat *st1 = ((const struct Cveinfo *)val1)->cpustat;
	struct Ccpustat *st2 = ((const struct Cveinfo *)val2)->cpustat;
	int res;

	if ((res = check_empty_param(st1, st2)) != 2)
		return res;
	return (st2->uptime - st1->uptime);
}

static int id_sort_fn(const void *val1, const void *val2)
{
	int ret;
	ret = (((const struct Cveinfo*)val1)->veid >
		((const struct Cveinfo*)val2)->veid);
	return ret;
}

static int status_sort_fn(const void *val1, const void *val2)
{
	int res;
	res = ((const struct Cveinfo*)val1)->status -
		((const struct Cveinfo*)val2)->status;
	if (!res)
		res = id_sort_fn(val1, val2);
	return res;
}

static int bootorder_sort_fn(const void *val1, const void *val2)
{
	int ret;
	unsigned long *r1 = ((const struct Cveinfo*)val1)->bootorder;
	unsigned long *r2 = ((const struct Cveinfo*)val2)->bootorder;

	ret = check_empty_param(r1, r2);
	switch (ret) {
		case 0: /* both NULL */
			return !id_sort_fn(val1, val2);
		case 2: /* both not NULL */
			break;
		default: /* one is NULL, other is not */
			return ret;
	}

	if (*r1 == *r2)
		return !id_sort_fn(val1, val2);

	return (*r1 > *r2);
}

static int ioprio_sort_fn(const void *val1, const void *val2)
{
	return ((const struct Cveinfo *)val1)->io.ioprio >
		((const struct Cveinfo *)val2)->io.ioprio;
}

static int cpunum_sort_fn(const void *val1, const void *val2)
{
	return ((const struct Cveinfo *)val1)->cpunum >
		((const struct Cveinfo *)val2)->cpunum;
}

#define SORT_STR_FN(name)						\
static int name ## _sort_fn(const void *val1, const void *val2)		\
{									\
	const char *h1 = ((const struct Cveinfo*)val1)->name;		\
	const char *h2 = ((const struct Cveinfo*)val2)->name;		\
	int ret;							\
	if ((ret = check_empty_param(h1, h2)) == 2)			\
		ret = strcmp(h1, h2);					\
	return ret;					\
}

SORT_STR_FN(hostname)
SORT_STR_FN(name)
SORT_STR_FN(description)
SORT_STR_FN(ostemplate)
SORT_STR_FN(ip)

#define SORT_UL_RES(fn, res, name, index)				\
static int fn(const void *val1, const void *val2)			\
{									\
	const struct C ## res *r1 = ((const struct Cveinfo *)val1)->res;\
	const struct C ## res *r2 = ((const struct Cveinfo *)val2)->res;\
	int ret;							\
	if ((ret = check_empty_param(r1, r2)) == 2)			\
		ret = r1->name[index] > r2->name[index];		\
	return ret;					\
}

#define SORT_UBC(res)							\
SORT_UL_RES(res ## _h_sort_fn, ubc, res, 0)				\
SORT_UL_RES(res ## _m_sort_fn, ubc, res, 1)				\
SORT_UL_RES(res ## _l_sort_fn, ubc, res, 2)				\
SORT_UL_RES(res ## _b_sort_fn, ubc, res, 3)				\
SORT_UL_RES(res ## _f_sort_fn, ubc, res, 4)

SORT_UBC(kmemsize)
SORT_UBC(lockedpages)
SORT_UBC(privvmpages)
SORT_UBC(shmpages)
SORT_UBC(numproc)
SORT_UBC(physpages)
SORT_UBC(vmguarpages)
SORT_UBC(oomguarpages)
SORT_UBC(numtcpsock)
SORT_UBC(numflock)
SORT_UBC(numpty)
SORT_UBC(numsiginfo)
SORT_UBC(tcpsndbuf)
SORT_UBC(tcprcvbuf)
SORT_UBC(othersockbuf)
SORT_UBC(dgramrcvbuf)
SORT_UBC(numothersock)
SORT_UBC(dcachesize)
SORT_UBC(numfile)
SORT_UBC(numiptent)
SORT_UBC(swappages)

#define SORT_DQ(res)							\
SORT_UL_RES(res ## _u_sort_fn, quota, res, 0)				\
SORT_UL_RES(res ## _s_sort_fn, quota, res, 1)				\
SORT_UL_RES(res ## _h_sort_fn, quota, res, 2)

SORT_DQ(diskspace)
SORT_DQ(diskinodes)

SORT_UL_RES(cpulimit_sort_fn, cpu, limit, 0)
SORT_UL_RES(cpuunits_sort_fn, cpu, limit, 1)

#define UBC_FIELD(name, header) \
{#name,      #header,      "%10s", 0, RES_UBC, print_ubc_ ## name, name ## _h_sort_fn},	\
{#name ".m", #header ".M", "%10s", 1, RES_UBC, print_ubc_ ## name, name ## _m_sort_fn},	\
{#name ".b", #header ".B", "%10s", 2, RES_UBC, print_ubc_ ## name, name ## _b_sort_fn},	\
{#name ".l", #header ".L", "%10s", 3, RES_UBC, print_ubc_ ## name, name ## _l_sort_fn},	\
{#name ".f", #header ".F", "%10s", 4, RES_UBC, print_ubc_ ## name, name ## _f_sort_fn}

static struct Cfield field_names[] =
{
/* ctid should have index 0 */
{"ctid", "CTID", "%10s", 0, RES_NONE, print_veid, id_sort_fn},
/* veid is for backward compatibility -- will be removed later */
{"veid", "CTID", "%10s", 0, RES_NONE, print_veid, id_sort_fn},
/* vpsid is for backward compatibility -- will be removed later */
{"vpsid", "CTID", "%10s", 0, RES_NONE, print_veid, id_sort_fn},

{"hostname", "HOSTNAME", "%-32s", 0, RES_HOSTNAME, print_hostname, hostname_sort_fn},
{"name", "NAME", "%-32s", 0, RES_NONE, print_name, name_sort_fn},
{"description", "DESCRIPTION", "%-32s", 0, RES_NONE, print_description, description_sort_fn },
{"ostemplate", "OSTEMPLATE", "%-32s", 0, RES_NONE, print_ostemplate, ostemplate_sort_fn },
{"ip", "IP_ADDR", "%-15s", 0, RES_IP, print_ip, ip_sort_fn},
{"status", "STATUS", "%-9s", 0, RES_NONE, print_status, status_sort_fn},
/*	UBC	*/
UBC_FIELD(kmemsize, KMEMSIZE),
UBC_FIELD(lockedpages, LOCKEDP),
UBC_FIELD(privvmpages, PRIVVMP),
UBC_FIELD(shmpages, SHMP),
UBC_FIELD(numproc, NPROC),
UBC_FIELD(physpages, PHYSP),
UBC_FIELD(vmguarpages, VMGUARP),
UBC_FIELD(oomguarpages, OOMGUARP),
UBC_FIELD(numtcpsock, NTCPSOCK),
UBC_FIELD(numflock, NFLOCK),
UBC_FIELD(numpty, NPTY),
UBC_FIELD(numsiginfo, NSIGINFO),
UBC_FIELD(tcpsndbuf, TCPSNDB),
UBC_FIELD(tcprcvbuf, TCPRCVB),
UBC_FIELD(othersockbuf, OTHSOCKB),
UBC_FIELD(dgramrcvbuf, DGRAMRB),
UBC_FIELD(numothersock, NOTHSOCK),
UBC_FIELD(dcachesize, DCACHESZ),
UBC_FIELD(numfile, NFILE),
UBC_FIELD(numiptent, NIPTENT),
UBC_FIELD(swappages, SWAPP),

{"diskspace", "DSPACE", "%10s", 0, RES_QUOTA, print_diskspace, diskspace_u_sort_fn},
{"diskspace.s", "DSPACE.S", "%10s", 1, RES_QUOTA, print_diskspace, diskspace_s_sort_fn},
{"diskspace.h", "DSPACE.H", "%10s", 2, RES_QUOTA, print_diskspace, diskspace_h_sort_fn},

{"diskinodes", "DINODES", "%10s", 0, RES_QUOTA, print_diskinodes, diskinodes_u_sort_fn},
{"diskinodes.s", "DINODES.S", "%10s", 1, RES_QUOTA, print_diskinodes, diskinodes_s_sort_fn},
{"diskinodes.h", "DINODES.H", "%10s", 2, RES_QUOTA, print_diskinodes, diskinodes_h_sort_fn},

{"laverage", "LAVERAGE", "%14s", 0, RES_CPUSTAT, print_laverage, laverage_sort_fn},
{"uptime", "UPTIME", "%15s", 0, RES_CPUSTAT, print_uptime, uptime_sort_fn},

{"cpulimit", "CPULIM", "%7s", 0, RES_CPU, print_cpulimit, cpulimit_sort_fn},
{"cpuunits", "CPUUNI", "%7s", 1, RES_CPU, print_cpulimit, cpuunits_sort_fn},
{"cpus", "CPUS", "%5s", 0, RES_CPUNUM, print_cpunum, cpunum_sort_fn},

{"ioprio", "IOP", "%3s", 0, RES_NONE, print_ioprio, ioprio_sort_fn},

{"onboot", "ONBOOT", "%6s", 0, RES_NONE, print_onboot, none_sort_fn},
{"bootorder", "BOOTORDER", "%10s", 0, RES_NONE,
	print_bootorder, bootorder_sort_fn},
};

static void *x_malloc(int size)
{
	void *p;
	if ((p = malloc(size)) == NULL) {
		fprintf(stderr, "Error: unable to allocate %d bytes\n", size);
		exit(1);
	}
	return p;
}

static void *x_realloc(void *ptr, int size)
{
	void *tmp;

	if ((tmp = realloc(ptr, size)) == NULL) {
		fprintf(stderr, "Error: unable to allocate %d bytes\n", size);
		exit(1);
	}
	return tmp;
}

static void usage()
{
	printf(
"Usage:	vzlist [-a | -S] [-n] [-H] [-o field[,field...] | -1] [-s [-]field]\n"
"	       [-h pattern] [-N pattern] [-d pattern] [CTID [CTID ...]]\n"
"	vzlist -L | --list\n"
"\n"
"Options:\n"
"	-a, --all		list all containers\n"
"	-S, --stopped		list stopped containers\n"
"	-n, --name		display containers' names\n"
"	-H, --no-header		suppress columns header\n"
"	-o, --output		output only specified fields\n"
"	-1			synonym for -H -octid\n"
"	-s, --sort		sort by the specified field\n"
"				('-field' to reverse sort order)\n"
"	-h, --hostname		filter CTs by hostname pattern\n"
"	-N, --name_filter	filter CTs by name pattern\n"
"	-d, --description	filter CTs by description pattern\n"
"	-L, --list		get possible field names\n"
	);
}

static int id_search_fn(const void* val1, const void* val2)
{
	return (*(const int *)val1 - ((const struct Cveinfo*)val2)->veid);
}

static int veid_search_fn(const void* val1, const void* val2)
{
	return (*(const int *)val1 - *(const int *)val2);
}

static char* trim_eol_space(char *sp, char *ep)
{
/*	if (ep == NULL)
		ep = sp + strlen(sp); */

	ep--;
	while (isspace(*ep) && ep >= sp) *ep-- = '\0';

	return sp;
}

static void print_hdr()
{
	struct Cfield_order *p;
	int f;

	for (p = g_field_order; p != NULL; p = p->next) {
		f = p->order;
		p_outbuffer += snprintf(p_outbuffer, e_buf - p_outbuffer,
				field_names[f].hdr_fmt, field_names[f].hdr);
		if (p_outbuffer >= e_buf)
			break;
		if (p->next != NULL)
			*p_outbuffer++ = ' ';
	}
	printf("%s\n", trim_eol_space(g_outbuffer, p_outbuffer));
	g_outbuffer[0] = 0;
	p_outbuffer = g_outbuffer;
}

/*
	1 - match
	0 - do not match
*/
static inline int check_pattern(char *str, char *pat)
{
	if (pat == NULL)
		return 1;
	if (str == NULL)
		return 0;
	return !fnmatch(pat, str, 0);
}

static void filter_by_hostname()
{
	int i;

	for (i = 0; i < n_veinfo; i++) {
		if (!check_pattern(veinfo[i].hostname, host_pattern))
			veinfo[i].hide = 1;
	}
}

static void filter_by_name()
{
	int i;

	for (i = 0; i < n_veinfo; i++) {
		if (!check_pattern(veinfo[i].name, name_pattern))
			veinfo[i].hide = 1;
	}
}

static void filter_by_description()
{
	int i;

	for (i = 0; i < n_veinfo; i++) {
		if (!check_pattern(veinfo[i].description, desc_pattern))
			veinfo[i].hide = 1;
	}
}

static void print_ve()
{
	struct Cfield_order *p;
	int i, f, idx;

	/* If sort order != veid (already sorted by) */
	if (g_sort_field) {
		qsort(veinfo, n_veinfo, sizeof(struct Cveinfo),
			field_names[g_sort_field].sort_fn);
	}
	if (!(veid_only || !show_hdr))
		print_hdr();
	for (i = 0; i < n_veinfo; i++) {
		if (sort_rev)
			idx = n_veinfo - i - 1;
		else
			idx = i;
		if (veinfo[idx].hide)
			continue;
		if (only_stopped_ve && veinfo[idx].status == VE_RUNNING)
			continue;
		is_last_field = 0;
		for (p = g_field_order; p != NULL; p = p->next) {
			f = p->order;
			if (p->next == NULL)
				is_last_field = 1;
			field_names[f].print_fn(&veinfo[idx],
						field_names[f].index);
			if (p_outbuffer >= e_buf)
				break;
			if (p->next != NULL)
				*p_outbuffer++ = ' ';
		}
		printf("%s\n", trim_eol_space(g_outbuffer, p_outbuffer));
		g_outbuffer[0] = 0;
		p_outbuffer = g_outbuffer;
	}
}

static void add_elem(struct Cveinfo *ve)
{
	veinfo = (struct Cveinfo *)x_realloc(veinfo,
				sizeof(struct Cveinfo) * ++n_veinfo);
	memcpy(&veinfo[n_veinfo - 1], ve, sizeof(struct Cveinfo));
	return;
}

static inline struct Cveinfo *find_ve(int veid)
{
	return (struct Cveinfo *) bsearch(&veid, veinfo, n_veinfo,
			sizeof(struct Cveinfo), id_search_fn);
}

static void update_ve(int veid, char *ip, int status)
{
	struct Cveinfo *tmp, ve;

	tmp = find_ve(veid);
	if (tmp == NULL) {
		memset(&ve, 0, sizeof(struct Cveinfo));
		ve.veid = veid;
		ve.status = status;
		ve.ip = ip;
		add_elem(&ve);
		qsort(veinfo, n_veinfo, sizeof(struct Cveinfo), id_sort_fn);
		return;
	} else {
		if (tmp->ip == NULL)
			tmp->ip = ip;
		else if (ip != NULL)
			free(ip);
		tmp->status = status;
	}
	return;
}

static void update_ubc(int veid, struct Cubc *ubc)
{
	struct Cveinfo *tmp;

	if ((tmp = find_ve(veid)) != NULL)
		tmp->ubc = ubc;
	return ;
}

static void update_quota(int veid, struct Cquota *quota)
{
	struct Cveinfo *tmp;

	if ((tmp = find_ve(veid)) == NULL)
		return;
	tmp->quota = x_malloc(sizeof(*quota));
	memcpy(tmp->quota, quota, sizeof(*quota));
	return;
}

static void update_cpu(int veid, unsigned long limit, unsigned long units)
{
	struct Cveinfo *tmp;
	struct Ccpu *cpu;

	if ((tmp = find_ve(veid)) == NULL)
		return;
	cpu = x_malloc(sizeof(*cpu));
	cpu->limit[0] = limit;
	cpu->limit[1] = units;
	tmp->cpu = cpu;
	return;
}

#define MERGE_QUOTA(name, quota, dq)				\
do {								\
	if (dq.name != NULL) {					\
		quota->name[1] = dq.name[0];			\
		quota->name[2] = dq.name[1];			\
	}							\
} while(0);

static void merge_conf(struct Cveinfo *ve, vps_res *res)
{
	if (ve->ubc == NULL) {
		ve->ubc = x_malloc(sizeof(struct Cubc));
		memset(ve->ubc, 0, sizeof(struct Cubc));
#define MERGE_UBC(name, ubc, res)				\
do {								\
	if (res == NULL || res->ub.name == NULL)		\
		break;						\
	ubc->name[2] = res->ub.name[0];				\
	ubc->name[3] = res->ub.name[1];				\
} while(0);

		MERGE_UBC(kmemsize, ve->ubc, res);
		MERGE_UBC(lockedpages, ve->ubc, res);
		MERGE_UBC(privvmpages, ve->ubc, res);
		MERGE_UBC(shmpages, ve->ubc, res);
		MERGE_UBC(numproc, ve->ubc, res);
		MERGE_UBC(physpages, ve->ubc, res);
		MERGE_UBC(vmguarpages, ve->ubc, res);
		MERGE_UBC(oomguarpages, ve->ubc, res);
		MERGE_UBC(numtcpsock, ve->ubc, res);
		MERGE_UBC(numflock, ve->ubc, res);
		MERGE_UBC(numpty, ve->ubc, res);
		MERGE_UBC(numsiginfo, ve->ubc, res);
		MERGE_UBC(tcpsndbuf, ve->ubc, res);
		MERGE_UBC(tcprcvbuf, ve->ubc, res);
		MERGE_UBC(othersockbuf, ve->ubc, res);
		MERGE_UBC(dgramrcvbuf, ve->ubc, res);
		MERGE_UBC(numothersock, ve->ubc, res);
		MERGE_UBC(dcachesize, ve->ubc, res);
		MERGE_UBC(numfile, ve->ubc, res);
		MERGE_UBC(numiptent, ve->ubc, res);
		MERGE_UBC(swappages, ve->ubc, res);
#undef MERGE_UBC
	}
	if (ve->ip == NULL && !list_empty(&res->net.ip)) {
		ve->ip = strdup(list2str(NULL, &res->net.ip));
	}
	if (ve->quota == NULL &&
		res->dq.diskspace != NULL &&
		res->dq.diskinodes != NULL)
	{
		ve->quota = x_malloc(sizeof(struct Cquota));
		memset(ve->quota, 0, sizeof(struct Cquota));

		MERGE_QUOTA(diskspace, ve->quota, res->dq);
		MERGE_QUOTA(diskinodes, ve->quota, res->dq);

	}
	if (ve->cpu == NULL &&
		(res->cpu.units != NULL || res->cpu.limit != NULL))
	{
		ve->cpu = x_malloc(sizeof(struct Ccpu));
		memset(ve->cpu, 0, sizeof(struct Ccpu));
		if (res->cpu.limit != NULL)
			ve->cpu->limit[0] = *res->cpu.limit;
		if (res->cpu.units != NULL)
			ve->cpu->limit[1] = *res->cpu.units;
	}
	if (res->misc.hostname != NULL)
		ve->hostname = strdup(res->misc.hostname);
	if (res->misc.description != NULL)
		ve->description = strdup(res->misc.description);
	if (res->tmpl.ostmpl != NULL)
		ve->ostemplate = strdup(res->tmpl.ostmpl);
	if (res->name.name != NULL) {
		int veid_nm = get_veid_by_name(res->name.name);
		if (veid_nm == ve->veid)
			ve->name = strdup(res->name.name);
	}
	if (res->fs.root != NULL)
		ve->ve_root = strdup(res->fs.root);
	if (res->fs.private != NULL)
		ve->ve_private = strdup(res->fs.private);
	ve->onboot = res->misc.onboot;
	if (res->misc.bootorder != NULL) {
		ve->bootorder = x_malloc(sizeof(*ve->bootorder));
		*ve->bootorder = *res->misc.bootorder;
	}
	ve->io.ioprio = res->io.ioprio;
	if (res->cpu.vcpus != NULL)
		ve->cpunum = *res->cpu.vcpus;
}

static int read_ves_param()
{
	int i;
	char buf[128];
	vps_param *param;
	char *ve_root = NULL;
	char *ve_private = NULL;
	int veid;

	param = init_vps_param();
	/* Parse global config file */
	vps_parse_config(0, GLOBAL_CFG, param, NULL);
	if (param->res.fs.root != NULL)
		ve_root = strdup(param->res.fs.root_orig);
	if (param->res.fs.private != NULL)
		ve_private = strdup(param->res.fs.private_orig);
	if (param->res.cpt.dumpdir != NULL)
		dumpdir = strdup(param->res.cpt.dumpdir);
	free_vps_param(param);
	for (i = 0; i < n_veinfo; i++) {
		veid = veinfo[i].veid;
		param = init_vps_param();
		snprintf(buf, sizeof(buf), VPS_CONF_DIR "%d.conf", veid);
		vps_parse_config(veid, buf, param, NULL);
		merge_conf(&veinfo[i], &param->res);
		if (veinfo[i].ve_root == NULL)
			veinfo[i].ve_root = subst_VEID(veinfo[i].veid, ve_root);
		if (veinfo[i].ve_private == NULL)
			veinfo[i].ve_private = subst_VEID(veinfo[i].veid,
								ve_private);
		free_vps_param(param);
	}
	free(ve_root);
	free(ve_private);

	return 0;
}


static int check_veid_restr(int veid)
{
	if (g_ve_list == NULL)
		return 1;
	return (bsearch(&veid, g_ve_list, n_ve_list,
			sizeof(*g_ve_list), veid_search_fn) != NULL);
}

#define UPDATE_UBC(str,name,ubc,held,maxheld,barrier,limit,failcnt)	\
do {						\
	if (!strcmp(str, #name)) {		\
		ubc->name[0] = held;		\
		ubc->name[1] = maxheld;		\
		ubc->name[2] = barrier;		\
		ubc->name[3] = limit;		\
		ubc->name[4] = failcnt;		\
	}					\
} while(0);					\

static int get_ub()
{
	char buf[256];
	int veid, prev_veid;
	unsigned long held, maxheld, barrier, limit, failcnt;
	char name[32];
	FILE *fp;
	char *s;
	struct Cveinfo ve;

	if ((fp = fopen(PROC_BC_RES, "r")) == NULL) {
		if ((fp = fopen(PROCUBC, "r")) == NULL) {
			fprintf(stderr, "Unable to open %s: %s\n",
					PROCUBC, strerror(errno));
			return 1;
		}
	}

	veid = 0;
	memset(&ve, 0, sizeof(struct Cveinfo));
	while (!feof(fp)) {
		if (fgets(buf, sizeof(buf), fp) == NULL)
			break;
		if ((s = strchr(buf, ':')) != NULL) {
			prev_veid = veid;
			if (sscanf(buf, "%d:", &veid) != 1)
				continue;
			if (prev_veid && check_veid_restr(prev_veid)) {
				update_ubc(prev_veid, ve.ubc);
			}
			ve.ubc = x_malloc(sizeof(struct Cubc));
			memset(ve.ubc, 0, sizeof(struct Cubc));
			s++;
		} else {
			s = buf;
		}
		if (sscanf(s, "%s %lu %lu %lu %lu %lu",
			name, &held, &maxheld, &barrier, &limit, &failcnt) != 6)
		{
			continue;
		}
		UPDATE_UBC(name, kmemsize, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, lockedpages, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, privvmpages, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, shmpages, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, numproc, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, physpages, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, vmguarpages, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, oomguarpages, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, numtcpsock, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, numflock, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, numpty, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, numsiginfo, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, tcpsndbuf, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, tcprcvbuf, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, othersockbuf, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, dgramrcvbuf, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, numothersock, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, dcachesize, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, numfile, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, numiptent, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
		UPDATE_UBC(name, swappages, ve.ubc, held, maxheld, barrier, \
			limit, failcnt)
	}
	if (veid && check_veid_restr(veid)) {
		update_ubc(veid, ve.ubc);
	}
	fclose(fp);
	return 0;
}

static char *invert_ip(char *ips)
{
	char *tmp, *p, *ep, *tp;
	size_t len;
	int rc;
	unsigned int ip[4];
	int family;
	char ip_str[INET6_ADDRSTRLEN];

	if (ips == NULL)
		return NULL;
	len = strlen(ips);
	tp = tmp = x_malloc(len + 2);
	tmp[0] = 0;
	p = ep = ips + len;
	/* Iterate in reverse order */
	while (p-- > ips) {
		/* Skip spaces */
		while (p > ips && isspace(*p)) {--p;}
		ep = p;
		/* find the string begin from */
		while (p > ips && !isspace(*(p - 1))) { --p;}
		len = ep - p + 1;
		if (len >= sizeof(ip_str))
			continue;
		strncpy(ip_str, p, len);
		ip_str[len] = 0;
		if ((family = get_netaddr(ip_str, ip)) == -1)
			continue;
		if ((inet_ntop(family, ip, ip_str, sizeof(ip_str) - 1)) == NULL)
			continue;
		rc = sprintf(tp, "%s ", ip_str);
		if (rc == -1)
			break;
		tp += rc;
	}
	return tmp;
}

static int get_run_ve_proc(int update)
{
	FILE *fp;
	struct Cveinfo ve;
	char buf[16384];
	char ips[16384];
	int res, veid, classid, nproc;

	if ((fp = fopen(PROCVEINFO, "r")) == NULL) {
		fprintf(stderr, "Unable to open %s: %s\n",
				PROCVEINFO, strerror(errno));
		return 1;
	}
	memset(&ve, 0, sizeof(struct Cveinfo));
	while (!feof(fp)) {
		if (fgets(buf, sizeof(buf), fp) == NULL)
			break;
		res = sscanf(buf, "%d %d %d %[^\n]",
			&veid, &classid, &nproc, ips);
		if (res < 3 || !veid)
			continue;
		if (!check_veid_restr(veid))
			continue;
		memset(&ve, 0, sizeof(struct Cveinfo));
		if (res == 4) {
			ve.ip = invert_ip(ips);

		}
		ve.veid = veid;
		ve.status = VE_RUNNING;
		if (update)
			update_ve(veid, ve.ip, ve.status);
		else
			add_elem(&ve);
	}
	if (!update)
		qsort(veinfo, n_veinfo, sizeof(struct Cveinfo), id_sort_fn);
	fclose(fp);
	return 0;
}

#if HAVE_VZLIST_IOCTL
static inline int get_ve_ips(unsigned int id, char **str)
{
	int ret = -1;
	struct vzlist_veipv4ctl veip;
	uint32_t *addr;

	veip.veid = id;
	veip.num = 256;
	addr = x_malloc(veip.num * sizeof(*veip.ip));
	for (;;) {
		veip.ip = addr;
		ret = ioctl(vzctlfd, VZCTL_GET_VEIPS, &veip);
		if (ret < 0)
			goto out;
		else if (ret <= veip.num)
			break;
		veip.num = ret;
		addr = x_realloc(addr, veip.num * sizeof(*veip.ip));
	}
	if (ret > 0) {
		char *buf, *cp;
		int i;

		buf = x_malloc(ret * (16 * sizeof(char)) + 1);
		cp = buf;
		for (i = ret - 1; i >= 0; i--)
			cp += sprintf(cp, "%d.%d.%d.%d ", NIPQUAD(addr[i]));
		*cp = '\0';
		*str = buf;
	} else
		*str = strdup("");
out:
	free(addr);
	return ret;
}

static int get_run_ve_ioctl(int update)
{
	int ret = -1;
	struct vzlist_veidctl veid;
	int nves;
	void *buf = NULL;
	int i;

	vzctlfd = open(VZCTLDEV, O_RDWR);
	if (vzctlfd < 0)
		goto error;
	veid.num = 256;
	buf = x_malloc(veid.num * sizeof(envid_t));
	while (1) {
		veid.id = buf;
		ret = ioctl(vzctlfd, VZCTL_GET_VEIDS, &veid);
		if (ret < 0)
			goto out;
		if (ret <= veid.num)
			break;
		veid.num = ret + 20;
		buf = x_realloc(buf, veid.num * sizeof(envid_t));
	}
	nves = ret;
	for (i = 0; i < nves; i++) {
		struct Cveinfo ve;
		envid_t id;

		id = veid.id[i];
		if (!id || !check_veid_restr(id))
			continue;
		memset(&ve, '\0', sizeof(ve));
		ve.veid = id;
		ve.status = VE_RUNNING;
		ret = get_ve_ips(id, &ve.ip);
		if (ret < 0) {
			if (errno != ESRCH)
				goto out;
			continue;
		}
		if (update)
			update_ve(id, ve.ip, ve.status);
		else
			add_elem(&ve);
	}
	if (!update)
		qsort(veinfo, n_veinfo, sizeof(struct Cveinfo), id_sort_fn);
	ret = 0;
out:
	free(buf);
	close(vzctlfd);
error:
	return ret;
}
#endif

static int get_run_quota_stat()
{
	unsigned long usage, softlimit, hardlimit, time, exp;
	int veid = 0, prev_veid = 0;
	struct Cquota quota;
	FILE *fp;
	char buf[128];
	char str[11];

	if ((fp = fopen(PROCQUOTA, "r")) == NULL) {
		return 1;
	}
	veid = 0;
	while (!feof(fp)) {
		if (fgets(buf, sizeof(buf), fp) == NULL)
			break;
		if (strchr(buf, ':') != NULL) {
			prev_veid = veid;
			if (sscanf(buf, "%d:", &veid) != 1)
				continue;
			if (prev_veid)
				update_quota(prev_veid, &quota);
			memset(&quota, 0, sizeof(quota));
			continue;

		}
		if (sscanf(buf, "%10s %lu %lu %lu %lu %lu", str, &usage,
			&softlimit, &hardlimit, &time, &exp) == 6)
		{
			if (!strcmp(str, "1k-blocks")) {
				quota.diskspace[0] = usage;
				quota.diskspace[1] = softlimit;
				quota.diskspace[2] = hardlimit;
			} else if (!strcmp(str, "inodes")) {
				quota.diskinodes[0] = usage;
				quota.diskinodes[1] = softlimit;
				quota.diskinodes[2] = hardlimit;
			}
		}
	}
	if (veid)
		update_quota(veid, &quota);
	fclose(fp);
	return 0;
}

static long get_clk_tck()
{
	if (__clk_tck != -1)
		return __clk_tck;
	__clk_tck = sysconf(_SC_CLK_TCK);
	return __clk_tck;
}

static int get_ve_cpustat(struct Cveinfo *ve)
{
	struct vz_cpu_stat stat;
	struct vzctl_cpustatctl statctl;
	struct Ccpustat st;

	statctl.veid = ve->veid;
	statctl.cpustat = &stat;
	if (ioctl(vzctlfd, VZCTL_GET_CPU_STAT, &statctl) != 0)
		return 1;
	st.la[0] = stat.avenrun[0].val_int + (stat.avenrun[0].val_frac * 0.01);
	st.la[1] = stat.avenrun[1].val_int + (stat.avenrun[1].val_frac * 0.01);
	st.la[2] = stat.avenrun[2].val_int + (stat.avenrun[2].val_frac * 0.01);

	st.uptime = (float) stat.uptime_jif / get_clk_tck();

	ve->cpustat = x_malloc(sizeof(st));
	memcpy(ve->cpustat, &st, sizeof(st));
	return 0;
}

static int get_ves_cpustat()
{
	int i;

	if ((vzctlfd = open(VZCTLDEV, O_RDWR)) < 0)
		return 1;
	for (i = 0; i < n_veinfo; i++) {
		if (veinfo[i].hide)
			continue;
		get_ve_cpustat(&veinfo[i]);
	}
	close(vzctlfd);
	return 0;
}

static int get_mounted_status()
{
	int i;
	char buf[512];

	for (i = 0; i < n_veinfo; i++) {
		if (veinfo[i].status == VE_RUNNING)
			continue;
		if (veinfo[i].ve_private == NULL ||
			!stat_file(veinfo[i].ve_private))
		{
			veinfo[i].hide = 1;
			continue;
		}
		get_dump_file(veinfo[i].veid, dumpdir, buf, sizeof(buf));
		if (stat_file(buf))
			veinfo[i].status = VE_SUSPENDED;
		if (veinfo[i].ve_root == NULL)
			continue;
		if (vps_is_mounted(veinfo[i].ve_root))
			veinfo[i].status = VE_MOUNTED;
	}
	return 0;
}

static int get_ve_cpunum(struct Cveinfo *ve) {
	char path[] = "/proc/vz/fairsched/2147483647/cpu.nr_cpus";
	int veid = ve->veid;
	FILE *fp;
	int ret = -1;

	snprintf(path, sizeof(path),
			"/proc/vz/fairsched/%d/cpu.nr_cpus", veid);
	fp = fopen(path, "r");
	if (fp == NULL) {
		fprintf(stderr, "Unable to open %s: %s\n",
				path, strerror(errno));
		return -1;
	}
	if (fscanf(fp, "%d", &ve->cpunum) != 1)
		goto out;

	ret = 0;
out:
	fclose(fp);

	return ret;
}

static int get_ves_cpunum()
{
	int i;

	for (i = 0; i < n_veinfo; i++) {
		if ((veinfo[i].hide) || (veinfo[i].status != VE_RUNNING))
			continue;
		get_ve_cpunum(&veinfo[i]);
	}
	return 0;
}

static int get_ves_cpu()
{
	unsigned long tmp;
	int veid, id, weight, rate;
	FILE *fp;
	char buf[128];

	if ((fp = fopen(PROCFSHED, "r")) == NULL) {
		fprintf(stderr, "Unable to open %s: %s\n",
				PROCFSHED, strerror(errno));
		return 1;
	}
	veid = 0;
	while (!feof(fp)) {
		if (fgets(buf, sizeof(buf), fp) == NULL)
			break;
		if (sscanf(buf, "%d %d %lu %d %d",
			&veid, &id, &tmp, &weight, &rate) != 5)
		{
			continue;
		}
		if (id && !veid) {
			rate = (rate * 100) / 1024;
			weight = MAXCPUUNITS / weight;
			update_cpu(id, rate, weight);
		}
	}
	fclose(fp);
	return 0;
}

static int get_ve_list()
{
	DIR *dp;
	struct dirent *ep;
	int veid, res;
	struct Cveinfo ve;
	char str[6];

	dp = opendir(VPS_CONF_DIR);
	if (dp == NULL) {
		return -1;
	}
	memset(&ve, 0, sizeof(struct Cveinfo));
	ve.status = VE_STOPPED;
	while ((ep = readdir (dp))) {
		res = sscanf(ep->d_name, "%d.%5s", &veid, str);
		if (!(res == 2 && !strcmp(str, "conf")))
			continue;
		if (!check_veid_restr(veid))
			continue;
		ve.veid = veid;
		add_elem(&ve);
	}
	closedir(dp);
	if (veinfo != NULL)
		qsort(veinfo, n_veinfo, sizeof(struct Cveinfo), id_sort_fn);
	return 0;
}

static int search_field(char *name)
{
	unsigned int i;

	if (name == NULL)
		return -1;
	for (i = 0; i < ARRAY_SIZE(field_names); i++) {
		if (!strcmp(name, field_names[i].name))
			return i;
	}
	return -1;
}

static int build_field_order(char *fields)
{
	struct Cfield_order *tmp, *prev = NULL;
	char *sp, *ep, *p;
	char name[32];
	int order;
	size_t nm_len;

	sp = fields;
	if (fields == NULL)
		sp = default_field_order;
	ep = sp + strlen(sp);
	do {
		if ((p = strchr(sp, ',')) == NULL)
			p = ep;
		nm_len = p - sp + 1;
		if (nm_len > sizeof(name) - 1) {
			fprintf(stderr, "Invalid field: %s\n", sp);
			return 1;
		}
		snprintf(name, nm_len, "%s", sp);
		sp = p + 1;
		if ((order = search_field(name)) < 0) {
			fprintf(stderr, "Unknown field: %s\n", name);
			return 1;
		}
		tmp = x_malloc(sizeof(struct Cfield_order));
		tmp->order = order;
		tmp->next = NULL;
		if (prev == NULL)
			g_field_order = tmp;
		else
			prev->next = tmp;
		prev = tmp;
	} while (sp < ep);
	return 0;
}

static inline int check_param(int res_type)
{
	struct Cfield_order *p;

	for (p = g_field_order; p != NULL; p = p->next) {
		if (field_names[p->order].res_type == res_type)
			return 1;
	}
	return 0;
}

static int collect()
{
	int update = 0;
	int ret;

	if (all_ve || g_ve_list != NULL || only_stopped_ve) {
		get_ve_list();
		update = 1;
	}
	get_run_ve(update);
	if (!only_stopped_ve && (ret = get_ub()))
		return ret;
	/* No CT found, exit with error */
	if (!n_veinfo) {
		fprintf(stderr, "Container(s) not found\n");
		return 1;
	}
	if (check_param(RES_QUOTA))
		get_run_quota_stat();
	if (check_param(RES_CPUSTAT))
		get_ves_cpustat();
	if (check_param(RES_CPU))
		if (!only_stopped_ve && (ret = get_ves_cpu()))
			return ret;
	if (check_param(RES_CPUNUM))
		if (!only_stopped_ve && (ret = get_ves_cpunum()))
			return ret;
	read_ves_param();
	get_mounted_status();
	if (host_pattern != NULL)
		filter_by_hostname();
	if (name_pattern != NULL)
		filter_by_name();
	if (desc_pattern != NULL)
		filter_by_description();
	return 0;
}

static void print_names()
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(field_names); i++)
		printf("%-15s %-15s\n", field_names[i].name,
					field_names[i].hdr);
	return;
}

static void free_veinfo()
{
	int i;

	for (i = 0; i < n_veinfo; i++) {
		if (veinfo[i].ip != NULL)
			free(veinfo[i].ip);
		if (veinfo[i].hostname != NULL)
			free(veinfo[i].hostname);
		if (veinfo[i].name != NULL)
			free(veinfo[i].name);
		if (veinfo[i].description != NULL)
			free(veinfo[i].description);
		if (veinfo[i].ostemplate != NULL)
			free(veinfo[i].ostemplate);
		if (veinfo[i].ubc != NULL)
			free(veinfo[i].ubc);
		if (veinfo[i].quota != NULL)
			free(veinfo[i].quota);
		if (veinfo[i].cpustat != NULL)
			free(veinfo[i].cpustat);
		if (veinfo[i].cpu != NULL)
			free(veinfo[i].cpu);
		if (veinfo[i].ve_root != NULL)
			free(veinfo[i].ve_root);
		if (veinfo[i].ve_private != NULL)
			free(veinfo[i].ve_private);
		if (veinfo[i].bootorder != NULL)
			free(veinfo[i].bootorder);
	}
}

static struct option list_options[] =
{
	{"no-header",	no_argument, NULL, 'H'},
	{"stopped",	no_argument, NULL, 'S'},
	{"all",		no_argument, NULL, 'a'},
	{"name",	no_argument, NULL, 'n'},
	{"name_filter", required_argument, NULL, 'N'},
	{"hostname",	required_argument, NULL, 'h'},
	{"description", required_argument, NULL, 'd'},
	{"output",	required_argument, NULL, 'o'},
	{"sort",	required_argument, NULL, 's'},
	{"list",	no_argument, NULL, 'L'},
	{"help",	no_argument, NULL, 'e'},
	{ NULL, 0, NULL, 0 }
};

int main(int argc, char **argv)
{
	int ret;
	char *f_order = NULL;
	char *ep, *p;
	int veid, c;

	while (1) {
		int option_index = -1;
		c = getopt_long(argc, argv, "HSanN:h:d:o:s:Le1", list_options,
				&option_index);
		if (c == -1)
			break;

		switch(c) {
		case 'S'	:
			only_stopped_ve = 1;
			break;
		case 'H'	:
			show_hdr = 0;
			break;
		case 'L'	:
			print_names();
			return 0;
		case 'a'	:
			all_ve = 1;
			break;
		case 'h'	:
			host_pattern = strdup(optarg);
			break;
		case 'd'	:
			desc_pattern = strdup(optarg);
			break;
		case 'o'	:
			f_order = strdup(optarg);
			break;
		case 's'	:
			p = optarg;
			if (p[0] == '-') {
				p++;
				sort_rev = 1;
			}
			if ((g_sort_field = search_field(p)) < 0) {
				fprintf(stderr, "Invalid sort field name: "
						"%s\n", optarg);
				return 1;
			}
			break;
		case '1'	:
			f_order = strdup("veid");
			veid_only = 1;
			break;
		case 'n'	:
			f_order = strdup(default_nm_field_order);
			with_names = 1;
			break;
		case 'N'	:
			name_pattern = strdup(optarg);
			break;
		default		:
			/* "Unknown option" error msg is printed by getopt */
			usage();
			return 1;
		}
	}
	if (optind < argc) {
		while (optind < argc) {
			veid = strtol(argv[optind], &ep, 10);
			if (*ep != 0 || !veid) {
				veid = get_veid_by_name(argv[optind]);
				if (veid < 0) {
					fprintf(stderr,
						"CT ID %s is invalid.\n",
						argv[optind]);
					return 1;
				}
			}
			optind++;
			g_ve_list = x_realloc(g_ve_list,
				sizeof(*g_ve_list) * ++n_ve_list);
			g_ve_list[n_ve_list - 1] = veid;
		}
		qsort(g_ve_list, n_ve_list, sizeof(*g_ve_list), id_sort_fn);
	}
	init_log(NULL, 0, 0, 0, 0, NULL);
	if (build_field_order(f_order))
		return 1;
	if (getuid()) {
		fprintf(stderr, "This program can only be run under root.\n");
		return 1;
	}
	if ((ret = collect())) {
		/* If no specific CTIDs are specified in arguments,
		 * 'no containers found' is not an error (bug #2149)
		 */
		if (g_ve_list == NULL)
			return 0;
		else
			return ret;
	}
	print_ve();
	free_veinfo();
	free(host_pattern);
	free(name_pattern);
	free(desc_pattern);
	free(f_order);

	return 0;
}
