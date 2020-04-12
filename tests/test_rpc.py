#!/usr/bin/python3
#
# Author: T. Zoerner
#
# Testing RPC support
#
# This program is in the public domain and can be used and
# redistributed without restrictions.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

import os
import sys
import time
import FsQuota

##
## insert your test case constants here:
##
path  = "/mnt"
remote_path  = "/data/tmp/qtest"
remote_host  = "localhost"
ugid  = os.getuid()
other_ugid  = 32000   # for permission test when not run by admin
dogrp = False

typnam = "GID" if dogrp else "UID"

def fmt_quota_vals(qtup):
    tm = time.localtime(qtup[3])
    bt_str = ("%04d-%02d-%02d/%02d:%02d" % (tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min))

    tm = time.localtime(qtup[7])
    ft_str = ("%04d-%02d-%02d/%02d:%02d" % (tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min))

    return ("%d (%d,%d,%s) %d (%d,%d,%s)" %
                (qtup[0], qtup[1], qtup[2], bt_str, qtup[4], qtup[5], qtup[6], ft_str))

# ----------------------------------------------------------------------------

try:
    print(">>> stage 1: test locally mounted NFS fs: %s" % path)
    qObj = FsQuota.Quota(path)
    print("Using device/argument \"%s\"" % qObj.dev)

    try:
        print("Query quotas for %s %d" % (typnam, ugid))
        qtup = qObj.query(ugid, grpquota=dogrp)
        print("Quota usage and limits for %s %d are %s" % (typnam, ugid, fmt_quota_vals(qtup)))

        print(">>> stage 1b: Repeat with TCP")
        qObj.rpc_opt(rpc_use_tcp=True)
        qtup2 = qObj.query(ugid, grpquota=dogrp)
        if qtup != qtup2:
            print("ERROR - result not equal: %s" % fmt_quota_vals(qtup));

        print(">>> stage 1c: Repeat with explicit authentication")
        qObj.rpc_opt(rpc_use_tcp=False, auth_uid=os.getuid(), auth_gid=os.getgid(), auth_hostname="localhost")
        qtup2 = qObj.query(ugid, grpquota=dogrp)
        if qtup != qtup2:
            print("ERROR - result not equal: %s" % fmt_quota_vals(qtup));

    except FsQuota.error as e:
        print("Query %s %d failed: %s" % (typnam, ugid, e), file=sys.stderr)

    # -------------------------------------------------------------------------

    try:
        print(">>> state 2: repeat with different %s %d" % (typnam, other_ugid))
        qtup = qObj.query(other_ugid, grpquota=dogrp)
        print("Quota usage and limits for %s %d are %s" % (typnam, other_ugid, fmt_quota_vals(qtup)))
    except FsQuota.error as e:
        print("Query %s %d failed: %s" % (typnam, other_ugid, e), file=sys.stderr)

    try:
        print(">>> stage 2b: Same with fake authentication")
        auth_pat = {'auth_gid': other_ugid} if dogrp else {'auth_uid': other_ugid}
        qObj.rpc_opt(rpc_use_tcp=False, **auth_pat, auth_hostname="localhost")
        qtup = qObj.query(other_ugid, grpquota=dogrp)
        print("Quota usage and limits for %s %d are %s" % (typnam, other_ugid, fmt_quota_vals(qtup)))

    except FsQuota.error as e:
        print("Query %s %d failed: %s" % (typnam, other_ugid, e), file=sys.stderr)

    # -------------------------------------------------------------------------

    print(">>> stage 3: force use of RPC to %s:%s" % (remote_host, remote_path))
    qObj = FsQuota.Quota(remote_path, rpc_host=remote_host)
    print("Using device/argument \"%s\"" % qObj.dev)

    try:
        print("Query quotas for %s %d" % (typnam, ugid))
        qtup = qObj.query(ugid, grpquota=dogrp)
        print("Quota usage and limits for %s %d are %s" % (typnam, ugid, fmt_quota_vals(qtup)))

    except FsQuota.error as e:
        print("Query %s %d failed: %s" % (typnam, ugid, e), file=sys.stderr)

    # -------------------------------------------------------------------------

    print(">>> stage 4: force use of non-existing remote port")
    qObj.rpc_opt(rpc_port=29875, rpc_timeout=2000, rpc_use_tcp=True)
    try:
        qtup = qObj.query(ugid, grpquota=dogrp)
    except FsQuota.error as e:
        print("Query failed (expected): %s" % (e), file=sys.stderr)

except FsQuota.error as e:
    print("ERROR instantiating: %s" % e, file=sys.stderr)
