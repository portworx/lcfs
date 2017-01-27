#!/bin/bash

[ "$1" == "--debug" -o -n "${DEBUG}" ] && DEBUG="yes" && set -x && shift

SUDO=sudo
DOCKER_BIN=docker
DOCKER_SRV_BIN=dockerd
LCFS_ENV_FL=${LCFS_ENV_FL:-"/etc/pwx/lcfs.env"}

[ -e "${LCFS_ENV_FL}" ] && source ${LCFS_ENV_FL}

LCFS_PKG=${LCFS_PKG:-"http://yum.portworx.com/repo/rpms/lcfs/lcfs.rpm"}
LCFS_IMG=${LCFS_IMG:-"portworx/lcfs:latest"}
DOCKER_MNT=${DOCKER_MNT:-"/var/lib/docker"}
PLUGIN_MNT=${PLUGIN_MNT:-"/lcfs"}
DEVFL=${DEVFL:-"/lcfs-dev-file"}
DEV=${DEV:-"/dev/sdNN"}
DSZ=${DSZ:-"500M"}

LOCAL_DNLD=/opt/pwx/dnld
LOCAL_PKG_NM=lcfs.rpm

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
    for mnt in $(cat /proc/mounts | awk '{print $2}' | egrep "^$1"); do ${SUDO} umount -f "${mnt}"; done
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

function download_lcfs_binary()
{
    ${SUDO} mkdir -p ${LOCAL_DNLD}
    ${SUDO} curl --fail --netrc -s -o ${LOCAL_DNLD}/${LOCAL_PKG_NM} ${LCFS_PKG}
    [ $? -ne 0 ] && echo "Failed to download LCFS package ${LCFS_PKG}." && cleanup_and_exit 1
    LCFS_PKG="${LOCAL_DNLD}/${LOCAL_PKG_NM}"
}

function install_lcfs_binary()
{
    local flg_fl=/opt/pwx/.lcfs

    [ -z "$(ps -jC "dockerd" | egrep -v '^ *PID|egrep' | awk '{print $1}')" ] && dockerd_manual_start "${SUDO} ${DOCKER_SRV_BIN}"

    local centos_exists=$(${SUDO} ${DOCKER_BIN} images -q centos:latest)

    ${SUDO} \rm -f ${flg_fl}
    ${SUDO} ${DOCKER_BIN} run --rm --name centos -v /opt:/opt centos bash -c "rpm -Uvh --nodeps ${LCFS_PKG} && touch ${flg_fl}"
    [ -z "${centos_exists}" ] && ${SUDO} ${DOCKER_BIN} rmi centos:latest &> /dev/null
    [ ! -f ${flg_fl} ] && echo "Failed to install LCFS binaries." && cleanup_and_exit 1
}

function remove_lcfs_plugin()
{
    ${SUDO} ${DOCKER_BIN} plugin ls | egrep -q "${LCFS_IMG}"
    [ $? -ne 0 ] && return 0
    ${SUDO} ${DOCKER_BIN} plugin disable ${LCFS_IMG} &> /dev/null
    ${SUDO} ${DOCKER_BIN} plugin rm ${LCFS_IMG} &> /dev/null
}

function dockerd_manual_start()
{
    local dcmd=($1)
    local out_fl=/tmp/ldocker.out.$$

    ${dcmd[@]} >> ${out_fl} 2>&1 &
    ${SUDO} timeout 15 bash -c "while [ ! -e ${out_fl} ] || ! (tail -n 5 ${out_fl} | egrep -q 'listen on .*docker.sock\".*$') ; do echo 'checking docker start...' ; sleep 1; done"
    local status=$?

    [ ${status} -ne 0 ] && echo "Error: failed to start docker." && cat ${out_fl}
    ${SUDO} \rm ${out_fl}
    [ ${status} -ne 0 ] && cleanup_and_exit 1
}

function system_docker_stop()
{
    local sysd_pid=$(ps -jC "systemd" | egrep -v '^ *PID|egrep' | awk '{print $1}')
    local sysV_docker="/etc/init.d/docker"

    [ -n "${sysd_pid}" ] && sudo systemctl stop docker          # Systemd stop
    [ -e "${sysV_docker}" ] && ${SUDO} /etc/init.d/docker stop; # SystemV stop
    killprocess ${DOCKER_SRV_BIN};                              # last resort
}

function lcfs_configure_save()
{
    ${SUDO} mkdir -p /etc/pwx
    ${SUDO} bash -c "cat <<EOF > ${LCFS_ENV_FL}
LCFS_IMG=${LCFS_IMG}
DOCKER_MNT=${DOCKER_MNT}
PLUGIN_MNT=${PLUGIN_MNT}
DEVFL=${DEVFL}
DEV=${DEV}
DSZ=${DSZ}
EOF"
}

function lcfs_configure()
{
    local limg dmnt lmnt ldev lsz ploc dyn

    read -p "LCFS install package (full filename|URL) [${LCFS_PKG}]: " ploc
    [ -n "${ploc}" ] && LCFS_PKG="${ploc}"

    read -p "LCFS docker plugin [${LCFS_IMG}]: " limg
    [ -n "${limg}" ] && LCFS_IMG="${limg}"

    read -p "LCFS device or file [${DEV}]: " ldev
    [ -z "${ldev}" ] && ldev="${DEV}"

    if [ ! -e "${ldev}" ]; then
	read -p  "LCFS device/file does not exist. Create file (y/n)? " dyn
        if [ "${dyn,,}" = "y" ]; then
	    read -p "LCFS file: ${ldev} size [${DSZ}]: " lsz
	    [ -z "${lsz}" ] && lsz="${DSZ}"
            ${SUDO} dd if=/dev/zero of=${ldev} count=1 bs=${lsz} &> /dev/null
	    [ $? -ne 0 ] && echo "Error: Failed to create LCFS device file ${ldev}." && cleanup_and_exit 0
#	    DEVFL="${ldev}"
	    DSZ="${lsz}"
	else
	    echo "LCFS device or file required." && cleanup_and_exit 0
        fi
    fi
    [ -n "${ldev}" ] && DEV="${ldev}"
    [ -f "${ldev}" ] && DEVFL="${ldev}"

    read -p "LCFS mount point [${PLUGIN_MNT}]: " lmnt
    [ -n "${lmnt}" ] && PLUGIN_MNT="${lmnt}"

    read -p "Docker mount point [${DOCKER_MNT}]: " dmnt
    [ -n "${dmnt}" ] && DOCKER_MNT="${dmnt}"

    echo "Saving LCFS configuration...."
    lcfs_configure_save
}

function stop_remove_lcfs
{
    local rcode=$1

    system_docker_stop

    # Stop docker && cleanup
    if [ -z "${STOP_DOCKER}" ]; then
	clean_mount "${PLUGIN_MNT}"
	clean_mount "${DOCKER_MNT}"
	killprocess lcfs
    fi

    if [ -n "${REMOVE}" ]; then
	dockerd_manual_start "${SUDO} ${DOCKER_SRV_BIN} -s vfs"
	remove_lcfs_plugin
	killprocess ${DOCKER_SRV_BIN}
	[ -z "${ZERODEV}" ] && read -p "Initialize the lcfs device (y/n)? " yn
	if [ "${yn,,}" = "y" -o -n "${ZERODEV}"  ]; then
	    [ "${DEV}" != "/dev/sdNN" ] && ${SUDO} dd if=/dev/zero of=${DEV} count=1 bs=4096 &> /dev/null
	    ${SUDO} \rm -f ${DEVFL}
	fi
    fi
    [ -z "${rcode}" ] && rcode=0
    [ -n "${STOP}" -o -n "${REMOVE}" -o -n "${STOP_DOCKER}" ] && cleanup_and_exit ${rcode}

    return 0
}

function help()
{
    echo "Usage: $0 [--help] [--configure] [--start] [--stop] [--stop-docker] [--remove]"
    echo -e "\t--configure: \tCreate and use configuration file."
    echo -e "\t--start: \tStart docker and lcfs."
    echo -e "\t--stop: \tStop docker and lcfs."
    echo -e "\t--stop-docker: \tStop the docker process."
    echo -e "\t--remove: \tStop and remove lcfs."
    echo -e "\t--help: \tDisplay this message."
    cleanup_and_exit $?
}

while [ "$1" != "" ]; do
    case $1 in
        -h |--help)
            help
            ;;
        --start)
	    START="$1"
            ;;
        --stop)
	    STOP="$1"
	    stop_remove_lcfs
            ;;
        --remove)
	    REMOVE="$1"
	    stop_remove_lcfs
            ;;
        --stop-docker)
	    STOP_DOCKER="$1"
	    stop_remove_lcfs
            ;;
	--configure)
	    lcfs_configure
            ;;
        *)
            echo "Error: invalid input parameter."
            help
            ;;
    esac
    shift
done

# Install lcfs binary
if [ -z "${START}" ]; then
    download_lcfs_binary
    install_lcfs_binary
elif [ ! -e "${LCFS_ENV_FL}" ]; then # --start was executed. Check config
    echo "LCFS not configured.  Re-execute this command with no options for a default configuration or with the '--configure' option."
    cleanup_and_exit 1
fi

stop_remove_lcfs  # Stop existing docker if setup or --configure.

# * Setup LCFS and start *

if [ ! -e "${DEV}" ]; then
    echo "LCFS device: ${DEV} not found.  Creating device file: ${DEVFL} ${DSZ}."
    ${SUDO} dd if=/dev/zero of=${DEVFL} count=1 bs=${DSZ}
    [ $? -ne 0 ] && echo "Error: Failed to create LCFS device file ${ldev}." && cleanup_and_exit 0
    DEV=${DEVFL}
fi

[ -z "${START}" ] && ${SUDO} \rm -rf ${PLUGIN_MNT} ${DOCKER_MNT}
${SUDO} mkdir -p ${PLUGIN_MNT} ${DOCKER_MNT}

# Mount lcfs
${SUDO} /opt/pwx/bin/lcfs ${DEV} ${DOCKER_MNT} ${PLUGIN_MNT} &
sleep 3
[ -z "$(ps -jC "lcfs" | egrep -v '^ *PID|egrep' | awk '{print $1}')" ] && echo "Failed to start LCFS binary." && cleanup_and_exit 1

# Restart docker
if [ -z "${START}" ]; then
    dockerd_manual_start "${SUDO} ${DOCKER_SRV_BIN} -s vfs"
    remove_lcfs_plugin
    ${SUDO} ${DOCKER_BIN} plugin install --grant-all-permissions ${LCFS_IMG}
    killprocess ${DOCKER_SRV_BIN}
fi
dockerd_manual_start "${SUDO} ${DOCKER_SRV_BIN} --experimental -s ${LCFS_IMG}"
${SUDO} ${DOCKER_BIN} info
if [ -z "${START}" ]; then
    lcfs_configure_save
    [ $? -ne 0 ] && echo "Error: LCFS save configuration failed. Setup failed." && REMOVE=yes && stop_remove_lcfs 1  # exit(1)
fi
cleanup_and_exit $?
