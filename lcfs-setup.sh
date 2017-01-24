#!/bin/bash

[ "$1" == "-debug" ] && DEBUG="yes" && set -x && shift

SUDO=sudo
DOCKER_BIN=docker
DOCKER_SRV_BIN=dockerd

PXGRAPH_IMG=${PXGRAPH_IMG:-"jvinod/lcfs:latest"}
DOCKER_MNT=${DOCKER_MNT:-"/var/lib/docker"}
PLUGIN_MNT=${PLUGIN_MNT:-"/lcfs"}
DEV=${DEV:-"/dev/sdc"}

function cleanup_and_exit()
{
    [ "${DEBUG}" == "yes" ] && set +x
    exit $1
}

function clean_mount()
{
    [ -z "$1" ] && return 0
    mountpoint -q "$1"
    [ $? -eq 0 ] && ${SUDO} fusermount -q -u "$1" && sleep 3
    for mnt in $(cat /proc/mounts | awk '{print $2}' | egrep ".*$1$"); do ${SUDO} umount -f "${mnt}"; done
    return 0
}

function killprocess()
{
    local pid=$(ps -jC "$1" | egrep -v '^ *PID|egrep' | awk '{print $1}')
    [ -z "${pid}" ] && return 0
    ${SUDO} pkill "$1"
    timeout 60 bash -c "while ${SUDO} kill -0 \"${pid}\"; do sleep 0.5; done" &> /dev/null
    pid=$(ps -jC "$1" | egrep -v '^ *PID|egrep' | awk '{print $1}')
    [ -n "${pid}" ] && echo "Failed to kill process for $1." && cleanup_and_exit 1
    return 0
}

function install_lcfs_binary()
{
    local lcfs_dnld_url=http://yum.portworx.com/repo/rpms/lcfs
    local flg_fl=/opt/pwx/.lcfs

    [ -z "$(ps -jC "dockerd" | egrep -v '^ *PID|egrep' | awk '{print $1}')" ] && dockerd_manual_start "${SUDO} ${DOCKER_SRV_BIN}"

    local centos_exists=$(${SUDO} ${DOCKER_BIN} images -q centos:latest)

    ${SUDO} \rm -f ${flg_fl}
    ${SUDO} ${DOCKER_BIN} run --rm --name centos -v /opt:/opt centos bash -c "rpm -Uvh --nodeps ${lcfs_dnld_url}/lcfs.rpm && touch ${flg_fl}"
    [ -z "${centos_exists}" ] && ${SUDO} ${DOCKER_BIN} rmi centos:latest &> /dev/null
    [ ! -f ${flg_fl} ] && echo "Failed to install LCFS binaries." && cleanup_and_exit 1
}

function remove_lcfs_plugin()
{
    ${SUDO} ${DOCKER_BIN} plugin ls | egrep -q "${PXGRAPH_IMG}"
    [ $? -ne 0 ] && return 0
    ${SUDO} ${DOCKER_BIN} plugin disable ${PXGRAPH_IMG} &> /dev/null
    ${SUDO} ${DOCKER_BIN} plugin rm ${PXGRAPH_IMG} &> /dev/null
}

function dockerd_manual_start()
{
    local dcmd=($1)
    local out_fl=/tmp/ldocker.out.$$

    ${dcmd[@]} >> ${out_fl} 2>&1 &
    while [ ! -e ${out_fl} ] || tail -n 1 ${out_fl} | egrep -q 'listen on .*docker.sock$'; do echo "checking docker start..."; sleep 0.5; done
    ${SUDO} \rm ${out_fl}
}

function system_docker_stop()
{
    local sysd_pid=$(ps -jC "systemd" | egrep -v '^ *PID|egrep' | awk '{print $1}')
    local sysV_docker="/etc/init.d/docker"

    [ -n "${sysd_pid}" ] && sudo systemctl stop docker          # Systemd stop
    [ -e "${sysV_docker}" ] && ${SUDO} /etc/init.d/docker stop; # SystemV stop
    killprocess ${DOCKER_SRV_BIN};                              # last resort
}

# Install lcfs binary
[ "$1" != "-stop" -a "$1" != "-stop-docker" ] && install_lcfs_binary

system_docker_stop

# Stop docker && cleanup
if [ "$1" != "-stop-docker" ]; then
    clean_mount "${PLUGIN_MNT}"
    clean_mount "${DOCKER_MNT}"
    killprocess lcsf
fi

if [ "$1" == "-stop" -o "$1" ==  "-stop-docker" ]; then
    if [ "$1" == "-stop" ]; then
	dockerd_manual_start "${SUDO} ${DOCKER_SRV_BIN} -s vfs"
	remove_lcfs_plugin
	killprocess ${DOCKER_SRV_BIN}
	read -p "Zero out the lcfs device: ${DEV} (y/n)? " yn
	if [ "${yn,,}" = "y" ]; then
	    ${SUDO} dd if=/dev/zero of=${DEV} count=1 bs=4096
	fi
    fi
    cleanup_and_exit 0
fi

${SUDO} \rm -rf ${PLUGIN_MNT} ${DOCKER_MNT}
${SUDO} mkdir -p ${PLUGIN_MNT} ${DOCKER_MNT}

# Mount lcfs
${SUDO} /opt/pwx/bin/lcfs ${DEV} ${DOCKER_MNT} ${PLUGIN_MNT} &
sleep 3
[ -z "$(ps -jC "lcfs" | egrep -v '^ *PID|egrep' | awk '{print $1}')" ] && echo "Failed to start LCFS binary." && cleanup_and_exit 1

# Restart docker
dockerd_manual_start "${SUDO} ${DOCKER_SRV_BIN} -s vfs"
remove_lcfs_plugin
${SUDO} ${DOCKER_BIN} plugin install --grant-all-permissions ${PXGRAPH_IMG}
killprocess ${DOCKER_SRV_BIN}
dockerd_manual_start "${SUDO} ${DOCKER_SRV_BIN} --experimental -s ${PXGRAPH_IMG}"
${SUDO} ${DOCKER_BIN} info
cleanup_and_exit $?
