#!/bin/bash
#  Copyright (C) 2000-2009, Parallels, Inc. All rights reserved.
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
#
# This script configures IP alias(es) inside Gentoo based CT with
# baselayout-1/openrc used as services/startup/shutdown system.
#
# Parameters are passed in environment variables.
# Required parameters:
#   IP_ADDR       - IP address(es) to add
#                   (several addresses should be divided by space)
# Optional parameters:
#   VE_STATE      - state of CT; could be one of:
#                     starting | stopping | running | stopped
#   IPDELALL	  - delete all old interfaces
#
VENET_DEV=venet0
IFCFG=/etc/conf.d/netif.${VENET_DEV}
SCRIPT=/etc/runlevels/default/netif.${VENET_DEV}

HOSTFILE=/etc/hosts

function set_config()
{
	echo "
template=\"interface\"
ipaddrs=\"\"
" > $IFCFG || error "Can't write to file $IFCFG"
}

function set_rc()
{
	[ -f "${SCRIPT}" ] && return 0
	ln -sf /etc/init.d/netif.tmpl /etc/init.d/netif.${VENET_DEV}
	rc-update add netif.${VENET_DEV} default &>/dev/null
}

function init_netconfig()
{
	set_config
	set_rc
	# Set up /etc/hosts
	if [ ! -f ${HOSTFILE} ]; then
		echo "127.0.0.1 localhost.localdomain localhost" > $HOSTFILE
	fi
}

function add_ip()
{
	local ip
	if [ "x${VE_STATE}" = "xstarting" -o "x${IPDELALL}" = "xyes" ]; then
		init_netconfig
		if [ "x${IPDELALL}" = "xyes" ]; then
			/etc/init.d/netif.${VENET_DEV} stop >/dev/null 2>&1
			return 0
		fi
	fi
	for ip in ${IP_ADDR}; do
		add_param ${IFCFG} "ipaddrs" "${ip}/32"
	done
	if [ "x${VE_STATE}" = "xrunning" ]; then
		# synchronyze config files & interfaces
		/etc/init.d/netif.${VENET_DEV} restart >/dev/null 2>&1
	fi
}

add_ip
exit 0
# end of script
