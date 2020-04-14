# Python File-system Quota module

This repository contains the sources of the Python file-system quota module,
which has its official home at [PyPi](https://pypi.org/project/FsQuota/).

The quota module allows accessing file system quotas on UNIX platforms.
This works both for locally mounted file systems and network file systems (via
RPC, i.e. Remote Procedure Call) for all the operating systems listed below.
The interface is designed to be independent of UNIX flavours as well as file
system types.

The C implementation of this module is derived from the
[Quota module for Perl](https://github.com/tomzox/Perl-Quota)
(also at [CPAN](https://metacpan.org/pod/Quota)).
I started developing the Perl module 1995, while working as a UNIX system
administrator at university and kept maintaining it even after no longer
working in this capacity. Since its beginnings, the module was continuously
extended by porting to more UNIX platforms and file-systems. Numerous people
have contributed to this process; for a complete list of names please see the
CHANGES document in the repository. All this effort is now available also to
Python users.

## Module information

The following operating systems and file systems are supported transparently
through a common API.

Supported operating systems:

* Linux - kernel 2.0.30 - 4.15
* FreeBSD 3 - 12.1, OpenBSD 2.2 - 6.6 & NetBSD 5 - 9
* SunOS 4.1.3 (aka Solaris 1)
* Solaris 2.4 - 2.10
* HP-UX 9.0x & 10.10 & 10.20 & 11.00
* IRIX 5.2 & 5.3 & 6.2 - 6.5
* OSF/1 & Digital Unix 4
* AIX 4.1, 4.2 and 5.3

Supported file systems:

* Standard file systems of the platforms listed above
* NFS (Network file system) on all of the above
* XFS on Linux and IRIX 6
* AFS (Andrew File System) on many of the above (see INSTALL)
* VxFS (Veritas File System) on Solaris 2

## Documentation

For further information please refer to the following files:

* <A HREF="doc/FsQuota.rst">FsQuota.rst</A>: API documentation
* <A HREF="INSTALL">INSTALL</A>: Installation description
* <A HREF="CHANGES">CHANGES</A>: Change log &amp; acknowledgements
* <A HREF="LICENSE">LICENSE</A>: GPL License
