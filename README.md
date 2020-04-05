# Python File-system Quota module

This repository contains the sources of the Python file-system quota module,
which allows accessing file system quotas on UNIX platforms from Python scripts.
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

<B>The module is still under development.</B> Work is still required for
automated setup, especially for platforms other than Linux. Also the
interface design still needs to be made "pythonic".

List of supported operating systems and file systems:

* SunOS 4.1.3
* Solaris 2.4 - 2.10
* HP-UX 9.0x & 10.10 & 10.20 & 11.00
* IRIX 5.2 & 5.3 & 6.2 - 6.5
* OSF/1 & Digital Unix 4
* BSDi 2, FreeBSD 3.x - 4.9, OpenBSD & NetBSD (no RPC)
* Linux - kernel 2.0.30 and later, incl. Quota API v2 and XFS
* AIX 4.1, 4.2 and 5.3
* AFS (Andrew File System) on many of the above (see INSTALL)
* VxFS (Veritas File System) on Solaris 2.

## Documentation

For further information please refer to the following files:

* <A HREF="doc/Quota.pm">Quota.pm</A>: API documentation
* <A HREF="INSTALL">INSTALL</A>: Installation description
* <A HREF="CHANGES">CHANGES</A>: Change log &amp; acknowledgements
* <A HREF="LICENSE">LICENSE</A>: GPL License
