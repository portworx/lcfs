# Enable building from outside build system
%{!?lcfsrelease:%define lcfsrelease 0.0}
%{!?release:%define release 0}
%{!?rpmdescription: %define rpmdescription This package contains %summary.}

Name: %name
Version: %lcfsrelease
Release: %release
Summary: %summary
Group: LCFS Runtime Environment
License: Proprietary
Source: %{name}-%{version}-%{release}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Obsoletes: %name
autoreqprov: no

%define debug_package %{nil}
%define __strip /bin/true

%if 0%{?required:1}
Requires: %required
%endif

%if 0%{?required_rpms:1}
BuildRequires: %required_rpms
%endif

%description
  %rpmdescription

%prep
%setup -n %specsrcdir

%build

%if 0%{?kernelpath:1}
export KERNELPATH="%kernelpath"
%endif

%if 0%{?kernelother:1}
export KERNELOTHER="%kernelother"
%endif

make clean all

%install
mkdir -p ${RPM_BUILD_ROOT}/opt/lcfs/bin
mkdir -p ${RPM_BUILD_ROOT}/opt/lcfs/services
cp -f lcfs ${RPM_BUILD_ROOT}/opt/lcfs/bin
cp -f lcfs-setup.sh ${RPM_BUILD_ROOT}/opt/lcfs/bin
cp -f fusermount3 ${RPM_BUILD_ROOT}/opt/lcfs/bin
cp -f lcfs.systemctl ${RPM_BUILD_ROOT}/opt/lcfs/services
cp -f lcfs.systemv ${RPM_BUILD_ROOT}/opt/lcfs/services

%check

%clean
/bin/rm -rf $RPM_BUILD_ROOT

%files
#%defattr(-,root,root,0755)
/opt/lcfs/bin/lcfs
/opt/lcfs/bin/lcfs-setup.sh
/opt/lcfs/bin/fusermount3
/opt/lcfs/services/lcfs.systemctl
/opt/lcfs/services/lcfs.systemv

%pre

%post

%postun

%preun
#if [ $1 = 0 ]; then
#fi

%changelog
* Sat Jan 16 2016 jrivera
- Initial spec file creation
