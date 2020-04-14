===============================
Python File-system Quota module
===============================

The Python file-system quota module allows accessing file system quotas on
UNIX platforms from Python scripts. The module is intended mainly for
system administrators who have to manage quotas for all the users on their
system.

The following operating systems and file systems are supported
transparently through a common API.

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
  (i.e. using an integrated RPC client)
* XFS on Linux and IRIX 6
* AFS (Andrew File System) on many of the above (see INSTALL)
* VxFS (Veritas File System) on Solaris 2

Historical note: The C implementation of this module is derived from the
`Quota module for Perl`_ (also at `CPAN`_). Since its beginnings in 1995,
the module was continuously extended by porting to more UNIX platforms and
file-systems. Numerous people have contributed to this process; for a
complete list of names please see the CHANGES document in the package. In
case of build issues, please refer to the INSTALL document within the
package.

.. _Quota module for Perl: https://github.com/tomzox/Perl-Quota
.. _CPAN: https://metacpan.org/pod/Quota

The following is a copy of the API documentation in file doc/FsQuota.rst
