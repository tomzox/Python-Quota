#!/usr/bin/python3
#
# Author: T. Zoerner
#
# Testing group quota support (Apr/02/1999)
#
# This program is in the public domain and can be used and
# redistributed without restrictions.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

import sys
import time
import FsQuota

##
## insert your test case constants here:
##
path  = "."
ugid  = 2001
dogrp = True
setq  = [123, 124, 50, 100]

typnam = "GID" if dogrp else "UID"

def fmt_quota_vals(qtup):
    if qtup.btime:
        tm = time.localtime(qtup.btime)
        bt_str = ("%04d-%02d-%02d/%02d:%02d" % (tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min))
    else:
        bt_str = "0"

    if qtup.itime:
        tm = time.localtime(qtup.itime)
        ft_str = ("%04d-%02d-%02d/%02d:%02d" % (tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min))
    else:
        ft_str = "0"

    return ("%d (%d,%d,%s) %d (%d,%d,%s)" %
                (qtup.bcount, qtup.bsoft, qtup.bhard, bt_str,
                 qtup.icount, qtup.isoft, qtup.ihard, ft_str))

try:
    qObj = FsQuota.Quota(path)
    print("Using device \"%s\"" % qObj.dev)

    print("Checking quota sync (may fail if quotas not enabled)...")
    qObj.sync()

    try:
        print("Query quotas for %s %d" % (typnam, ugid))
        qtup = qObj.query(ugid, grpquota=dogrp)
        print("Quota usage and limits for %s %d are %s" % (typnam, ugid, fmt_quota_vals(qtup)))

    except FsQuota.error as e:
        print("Query %s %d failed: %s" % (typnam, ugid, e), file=sys.stderr)

    ##
    ##  set quota block & file limits for user
    ##

    print("Setting new quota limits...")
    qObj.setqlim(ugid, *setq, timereset=1, grpquota=dogrp)
    print("Quotas set successfully for %s %d" % (typnam, ugid))

    print("Reading back new quota limits...")
    qtup = qObj.query(ugid, grpquota=dogrp)
    print("Quota usage and limits for %s %d are %s" % (typnam, ugid, fmt_quota_vals(qtup)))

    print("Finally checking quota sync again")
    qObj.sync()

except FsQuota.error as e:
    print("ERROR using %s %d: %s" % (typnam, ugid, e), file=sys.stderr)
