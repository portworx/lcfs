#!/bin/bash

[ -z "${VERSION}" ] && VERSION="0.0.0"
[ -z "${REVISION}" ] && REVISION="0"


if [ "x$1" == "x" ] ; then
	NAME=lcfs
else
	NAME=lcfs-$1
fi
SUMMARY="LCFS storage driver"
DESCRIPTION="LCFS is a storage driver for Docker that uses a new filesystem built specifically for container image management."

export PLATFORM=$(/bin/uname -m)
export PROCESSOR=$(/bin/uname -p)
export BUILDDIR=${PWD}

export TOPDIR=${BUILDDIR}/lcfs
export TMPPATH=${BUILDDIR}/rpmtmp

BLD_MACROS="--define '_topdir "${TOPDIR}"' --define '_tmppath "${TMPPATH}"' --macros=FILE:/dev/null"


RPMVERSION=${VERSION}-${REVISION}
RPMVERSION_DEFINES="--define 'lcfsrelease "${VERSION}"' --define 'release "${REVISION}"'"

MBUILDROOT=${BUILDDIR}/tmp/buildroot

RPMBLDROOT=${TOPDIR}/BUILD
RPMSRCROOT=${TOPDIR}/SOURCES
RPMRPMSROOT=${TOPDIR}/RPMS
RPMSRPMSROOT=${TOPDIR}/SRPMS
RPMSPECSROOT=${TOPDIR}/SPECS

BLDDIRS=" \
  ${TOPDIR} \
  ${TMPPATH} \
  ${MBUILDROOT} \
  ${RPMBLDROOT} \
  ${RPMSRCROOT} \
  ${RPMRPMSROOT} \
  ${RPMSRPMSROOT} \
  ${RPMSPECSROOT}"

for dir in ${BLDDIRS}; do mkdir -p ${dir}; done

SPEC=lcfs.spec
cp -a ${BUILDDIR}/${SPEC} ${RPMSPECSROOT}/${SPEC}

[ -n "${KERNELPATH}" ] && KERNELPATHDEFINE=" --define 'kernelpath "${KERNELPATH}"'"
[ -n "${KERNELOTHER}" ] && KERNELOTHERDEFINE=" --define 'kernelother "${KERNELOTHER}"'"
EXTRA_DEFINES=${KERNELPATHDEFINE}" "${KERNELOTHERDEFINE}" --define 'rpmdescription "${DESCRIPTION}"' --define 'required kernel >= 3.10'"

SOURCE_ROOT=${BUILDDIR}/..
RPM_NAME="${NAME}"
RPM_SUMMARY="${SUMMARY}"
RPM_DESCRIPTION="${DESCRIPTION}"
RPM_DEFINES="--define 'name "${RPM_NAME}"' --define 'summary "${RPM_SUMMARY}"' --define 'specsrcdir "${RPM_NAME}-src"' ${EXTRA_DEFINES}"

echo "--- Building target for ${RPM_NAME} ---"
mkdir -p ${MBUILDROOT}/${RPM_NAME}-src
cd ${SOURCE_ROOT} && tar --exclude .git --exclude rpm -czf - * | (cd ${MBUILDROOT}/${RPM_NAME}-src; tar -xzf -)
cd ${SOURCE_ROOT} && cp -a ../lcfs-setup.sh ${MBUILDROOT}/${RPM_NAME}-src
cd ${SOURCE_ROOT} && cp -a ../lcfs.systemv ${MBUILDROOT}/${RPM_NAME}-src
cd ${SOURCE_ROOT} && cp -a ../lcfs.systemctl ${MBUILDROOT}/${RPM_NAME}-src
cd ${MBUILDROOT} && tar -czf ${RPMSRCROOT}/${RPM_NAME}-${RPMVERSION}.tar.gz ${RPM_NAME}-src
cd ${RPMSPECSROOT} && eval rpmbuild -vv -ba ${BLD_MACROS[@]} ${RPMVERSION_DEFINES[@]} ${RPM_DEFINES[@]} ${SPEC}
if [ $? -eq 0 ]; then
    ALIEN=$(which alien)
    [ -z "${ALIEN}" ] && echo "Error: Debian 'alien' package not installed.  Please install using apt-get install alien and rerun this script." && exit 1;
    cd ${RPMRPMSROOT}/${PLATFORM} && DEBPKG=$(sudo ${ALIEN} -k ${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.rpm --scripts)
    [ $? -ne 0 ] && echo "Error: Failed to build debian package." && exit 1
    DEBPKG=$(echo "${DEBPKG}" | /bin/sed 's/ generated.*//')
else
    echo "Error: Failed to build rpm package." && exit 1
fi

RPMPATH="${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.rpm"
if [ -e ${RPMPATH} ]; then
    [ -e ${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}.rpm ] && \rm -f ${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}.rpm
    cd ${RPMRPMSROOT}/${PLATFORM} && ln -s ${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.rpm ${RPM_NAME}.rpm
    cd ${MBUILDROOT} && rpm2cpio ${RPMPATH} | cpio -ivd && tar -czvf ${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.binaries.tgz opt
    if [ $? -eq 0 ]; then
	[ -e ${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}.binaries.tgz ] && \rm -f ${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}.binaries.tgz
	cd ${RPMRPMSROOT}/${PLATFORM} && ln -s ${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.binaries.tgz ${RPM_NAME}.binaries.tgz
    fi
    \rm -rf ${MBUILDROOT}/opt
fi

DPKGPATH="${RPMRPMSROOT}/${PLATFORM}/${DEBPKG}"
if [ -e ${DPKGPATH} ]; then
    [ -e ${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}.deb ] && \rm -f ${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}.deb
    cd ${RPMRPMSROOT}/${PLATFORM} && ln -s ${DEBPKG} ${RPM_NAME}.deb
fi

if [ ! -z "${OUTPATH}" ]; then
	cp -a ${RPMPATH} ${OUTPATH}
	RPMPATH="${OUTPATH}/${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.rpm"
	[ -e ${OUTPATH}/${RPM_NAME}.rpm ] && \rm -f ${OUTPATH}/${RPM_NAME}.rpm
	[ -e ${RPMPATH} ] && cd ${OUTPATH} && ln -s ${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.rpm ${RPM_NAME}.rpm

	if [ -e ${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.binaries.tgz ]; then
	    cp -a ${RPMRPMSROOT}/${PLATFORM}/${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.binaries.tgz ${OUTPATH}
	    [ -e ${OUTPATH}/${RPM_NAME}.binaries.tgz ] && \rm -f ${OUTPATH}/${RPM_NAME}.binaries.tgz
	    cd ${OUTPATH} && ln -s ${RPM_NAME}-${VERSION}-${REVISION}.${PLATFORM}.binaries.tgz ${RPM_NAME}.binaries.tgz
	fi

	if [ -n "${DPKPKG}" ]; then
		cp -a ${DPKGPATH} ${OUTPATH}
		DPKGPATH="${OUTPATH}/${DEBPKG}"
		[ -e ${OUTPATH}/${RPM_NAME}.deb ] && \rm -f ${OUTPATH}/${RPM_NAME}.deb
		[ -e ${DPKGPATH} ] && cd ${OUTPATH} && ln -s ${DEBPKG} ${RPM_NAME}.deb
	fi
fi

echo
echo "Install commands for the built ${RPM_NAME} packages:"
echo
echo "  RHEL/Centos: rpm -Uvh ${RPMPATH}"
[ -n "${DEBPKG}" ] && echo "       Debian: dpkg --install ${DPKGPATH}"
echo
echo "Uninstall commands for the built ${RPM_NAME} packages:"
echo
echo "  RHEL/Centos: rpm -e ${RPM_NAME}"
[ -n "${DEBPKG}" ] && echo "       Debian: dpkg --purge ${RPM_NAME}"
echo
