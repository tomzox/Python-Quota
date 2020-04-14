===========================================================
FsQuota - Interface to file system quotas on UNIX platforms
===========================================================

SYNOPSIS
========

::

    import FsQuota

    qObj = FsQuota.Quota(path [,rpc_host=hostname])

    (bcount, bsoft, bhard, btime,
     icount, isoft, ihard, itime) =
        qObj.query(uid [,grpquota=1] [,prjquota=1])

    qObj.setqlim(uid, bsoft, bhard, isoft, ihard
                 [,timelimit_reset={0|1}]
                 [,grpquota=1] [,prjquota=1])

    qObj.sync()

    qObj.rpc_opt([option keywords])

    for dev, path, type, opts in FsQuota.MntTab(): ...

FsQuota Module
==============

The **FsQuota** module provides two classes that allow accessing file
system quotas from Python scripts:

Instances of the **Quota** class take as main init parameter a path of a
mount point (or any path below the mount point). The instance can then be
used to query or modify quota of users or groups within that file system.
The class is designed portably, so that the same interfaces work across
all file system types and UNIX platforms. (Although there are some extra
options during initialization for certain file systems.)

Instances of the *MntTab* class allow iterating across the mount
table.  For each entry in the table, it provides the file system type,
mount  point and options. (Note this class is usually not required to work
with the Quota class.  It is provided here just for convenience, as the
functionality is actually used internally by the Quota class.)

Class FsQuota.Quota
===================

::

    qObj = FsQuota.Quota(path)
    qObj = FsQuota.Quota(remote_path, rpc_host=remote_host)

Creates a Quota object that then is used for querying or modifying
quotas. In case of special file systems which are known not to suport
quota, the creation may raise exception **FsQuota.error**. However note
the absence of an exception is not a guarantee that the file system
actually supports quota limits.

When parameter **rpc_host** is specified, the automatic detection of file
system types is omitted. In this case the following operations will
address the file system containing the given path on the given remote host
using RPC. This could be used to query even file systems that are not
mounted locally. See also the **rpc_opt()** method for RPC configuration
options.

Internal behavior: Most importantly, the initialization determines the
file system type and thus the access method to be used in following
quota operations. Many platforms use the **quotactl** syscall, but even
then the type of device parameter to be passed varies from system to
system. It may be the path of a device file (e.g. **/dev/sda1**) or the
path of the mount point or the quotas file at the top of the file system
(e.g. **/home/quotas**). For the rare cases you need this information,
it can be queried via the **dev** attribute.

The given mount point may also be on a remove file system (e.g. mounted
via Network File System, NFS), which has the module transparently query
the given host via a remote procedure call (RPC).  Note: RPC queries
require *rquotad(1m)* to be running on the target system. If the daemon
or host are down, the operations time out after a configurable delay.

Quota.query()
-------------

::

    (bcount,bsoft,bhard,btime, icount,isoft,ihard,itime)
        = qObj.query(uid, [keyword_options...])

Get current usage and quota limits for blocks and files respectively,
owned by the given user. The user is specified by a numeric UID.
The result is a named tuple of type **FsQuota.QueryResult**, so that
members can be accessed via name as well as via indices:

0. **bcount**: Number of 1 kB blocks currently used by inodes owned by the user.
1. **bsoft**: Soft limit for block count (or 0 if none)
2. **bhard**: Hard limit for block count (or 0 if none)
3. **btime**: Time when an exceeded soft block limit turns into a hard limit.
   This value is meaningless when the soft limit is not exceeded.
4. **icount**: Number of inodes (i.e. files) currently owned by the user.
5. **isoft**: Soft limit for inode count (or 0 if none)
6. **ihard**: Hard limit for inode count (or 0 if none)
7. **itime**: Time when an exceeded soft inode limit turns into a hard limit.
   This value is meaningless when the soft limit is not exceeded.

When a hard limit is reached, the OS will reject any further write with
errno *EDQUOT* (or *ENOSPC* on older systems).  If the soft limit is
exceeded, but hard limit not exceeded, writes by this user will fail only
after the time indicated by *btime* or *itime* respectively is
reached.  The time is usually set to 7 days after exceeding the soft limit
for the first time. These times are expressed as elapsed seconds since
00:00 1/Jan/1970 GMT.

Note when hard and soft limits are both zero, this means there is no limit
for that user. (On some platforms the query may fail with error code
*ESRCH* in that case; most however still report valid usage values in
that case.)

Optional keyword-only parameters:

:grpquota:
    When parameter **grpquota** is present and set to a value that evaluates to
    *True*, the value in *uid* is taken as GID and group quotas are queried.
    Group quotas may not be supported across all platforms (e.g. Linux and
    other BSD based Unix variants, OSF/1 and  AIX - check the quotactl(2) man
    page on your systems).

:prjquota:
    When parameter **prjquota** is present and set to a value that evaluates to
    *True*, project quotas are queried; this is currently only supported for
    XFS. Exception **FsQuota.error(ENOTSUP)** is raised for unsupported
    file-systems.

Method Quota.setqlim()
----------------------

::

    qObj.setqlim(uid, bsoft, bhard, isoft, ihard [,keyword options...])

Sets quota limits for the given user. Meanings of parameters *uid*,
*bsoft*, *bhard*, *isoft* and *ihard* are the same as for the **query**
method.

Note all the limit values are optional and default to zero. The parameters
can also be passed in form of keyword parameters. For example
*qObj.setqlim(uid, isoft=10,ihard=20)* would limit inode counts to 10
soft, 20 hard, but remove limits for block count. (Note it's not possible
to set only block or inode limits repsectively; to do so query current
limits first and then pass those values to setqlim if you want to keep
them unchanged.)

Note: if you want to set the quota of a particular user to zero, i.e.
no write permission, you must not set all limits to zero, since that
is equivalent to unlimited access. Instead set only the hard limit
to 0 and the soft limit to a non-zero value.

Optional keyword-only parameters:

:timelimit_reset:
    Optional parameter *timelimit_reset* defines how time limits are
    initialized: When the assigned value is *False*, time limits are set to
    **NOT STARTED** (i.e. the time limits are not initialized until the first
    write attempt by this user). This is the default when the parameter is
    omitted. When assigned *True*, the time limits are set to **7.0 days**.
    More alternatives (i.e. setting a specific time) aren't available in most
    implementations.

:grpquota:
    When parameter **grpquota** is present and set to True, parameter *uid* is
    interpreted as GID and the the limit of the corresponding group is
    modified. This is not supported on all platforms.

:prjquota:
    When parameter **prjquota** is present and set to True, project quotas are
    modified; this is currently only supported for XFS.  Exception
    **FsQuota.error(ENOTSUP)** is raised for unsupported file-systems.

Note that the module does not support setting quotas via RPC (even
though some implementations of *rpc.rquotad(8)* allow optionally
enabling this, but it seems a bad idea for security.)

Method Quota.sync()
-------------------

::

    qObj.sync()

Have the kernel update the quota file on disk, in particular after
modifying quota limits.

A secondary purpose of this method is checking if quota support is
enabled in the kernel (and on some platforms, for a particular file
system; on others however the call succeeds even if quota is not enabled
in the given file system.) Read the **quotaon(1m)** man page on how to
enable quotas on a file system.

Method Quota.rpc_opt()
----------------------

::

    qObj.rpc_opt([keyword options...])

This method allows configuring networking and authentication parameters
for queries of network file system quotas via RPC. The options have no
effect when targeting other file system types. The following keyword-only
parameters are available:

:rpc_port:
    Sets the port used by *rpc.rquotad(8)*; default value is zero, which
    which means the remote host's portmapper (aka rpcbind) is used. (Note
    in case of the latter you can find out the port using *rpcinfo -p host*)

:rpc_use_tcp:
    If *True*, use TCP; if *False* use UDP (default).

:rpc_timeout:
    Timeout value in milliseconds in case the remote host does not respond.

:auth_uid:
    UID value (i.e. user identifier) to provide for authentication.
    If not specified, this defaults to the UID of the current process.
    For example, you could set the UID here that you later want to
    query, for circumventing a permission error.

:auth_gid:
    GID value (i.e. group identifier) to provide for authentication.
    If not specified, this defaults to the GID of the current process.

:auth_hostname:
    Hostname to provide for authentication.
    If not specified or empty, this defaults to the name of the local machine.

Note for resetting to default authentication, set both **auth_uid** and
**auth_gid** to value -1 (even if you previously changed only one, as the
opposite is filled in automatically if missing).

Attribute Quota.dev
-------------------

This attribute provides the device argument used internally by **query()**
and **setqlim()** methods for the selected file system.

Attribute Quota.is_nfs
----------------------

This attribute indicates 1 is the file system is NFS, else 0.

Class FsQuota.MntTab()
======================

This class defines objects that can be used as an iterator which lists all
entries in the mount table. Each object returned by iteration is a named
tuple of type **FsQuota.MntEnt** with the following entries of type
string:

0. **mnt_fsname**: Name of the filesystem (e.g. device name)
1. **mnt_dir**: Filesystem path prefix (aka mount point)
2. **mnt_type**: Mount type (aka file system type)
3. **mnt_opts**: Mount options, separated by colon.

Note the mount table contains information about all currently mounted
(local or remote) file systems.  The format and location of this table
varies from system to system (e.g. it may be in file **/etc/mtab**).
This iterator provides a portable way to read it. (On some systems,
like **OSF/1**, this table isn't accessible as a file at all, i.e. only
via C library interfaces). Internally, the iterator will call
*setmntent(3)* or the equivalent of your platform upon initialization,
call *getmntent(3)* during iteration, and call *endmntent(3)* upon
deallocation.

Hint: For finding the mount table entry corresponding to a given path
(e.g. to determine the file system type), you can compare the device ID
indicated by *os.stat(path).st_dev* of the mount points returned from
iteration with that of the path in question.

ERROR HANDLING
==============

All methods raise exception *FsQuota.error* upon errors. The exception
class is derived from **OSError** and thus contains firstly a numerical
error code in attribute *errno* (copied from *errno* in most cases), and
secondly a derived error message in attribute *strerror*.

Note the error string is adapted to the context of quota operations and
therefore not always identical to the text returned by **strerror(3)**.
The normal error descriptions don't always make sense for quota errors
(e.g. **ESRCH**: *No such process*, here: *No quota for this user*)

AUTHORS
=======

This module is derived from an equivalent extension module for Perl,
created 1995 by T. Zoerner (email: tomzo AT users.sourceforge.net)
and since then continuously improved and ported to many more
operating systems and file systems - and now ported to Python.
Numerous people have contributed to this process in the past;
for a complete list of names please see the CHANGES document.

LICENSE
=======

Copyright (C) 1995-2020 T. Zoerner

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by the
Free Software Foundation. (Either version 2 of the GPL, or any later
version, see http://www.opensource.org/licenses/).

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

SEE ALSO
========

python3(1), edquota(8),
quotactl(2) or quotactl(7I),
mount(8), mtab(4) or mnttab(4), quotaon(8),
setmntent(3), getmntent(3) or getmntinfo(3), endmntent(3),
rpc(3), rquotad(8) or rpc.rquotad(8), rpcinfo(7).
