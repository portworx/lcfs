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
mkdir -p ${RPM_BUILD_ROOT}/opt/pwx/bin
mkdir -p ${RPM_BUILD_ROOT}/opt/pwx/services
cp -f lcfs ${RPM_BUILD_ROOT}/opt/pwx/bin
cp -f lcfs-setup.sh ${RPM_BUILD_ROOT}/opt/pwx/bin
cp -f lcfs.systemctl ${RPM_BUILD_ROOT}/opt/pwx/services
cp -f lcfs.systemv ${RPM_BUILD_ROOT}/opt/pwx/services

%check

%clean
/bin/rm -rf $RPM_BUILD_ROOT

%files
#%defattr(-,root,root,0755)
/opt/pwx/bin/lcfs
/opt/pwx/bin/lcfs-setup.sh
/opt/pwx/services/lcfs.systemctl
/opt/pwx/services/lcfs.systemv

%pre

%post

%postun

%preun
#if [ $1 = 0 ]; then
#fi

%changelog
* Sat Jan 16 2016 jrivera
- Initial spec file creation
