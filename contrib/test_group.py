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
dogrp = 1
setq  = [123, 124, 50, 100]

typnam = "GID" if dogrp else "UID"

def fmt_quota_vals(qtup):
    tm = time.localtime(qtup[3])
    bt_str = ("%04d-%02d-%02d/%02d:%02d" % (tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min))

    tm = time.localtime(qtup[7])
    ft_str = ("%04d-%02d-%02d/%02d:%02d" % (tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min))

    return ("%d (%d,%d,%s) %d (%d,%d,%s)" %
                (qtup[0], qtup[1], qtup[2], bt_str, qtup[4], qtup[5], qtup[6], ft_str))

try:
    dev = FsQuota.getqcarg(path)
    print("Using device/argument \"dev\"")

    print("Checking quota sync (may fail if quotas not enabled)")
    FsQuota.sync(dev)

    try:
        print("\nQuery quotas for %s %d" % (typnam, ugid))
        qtup = FsQuota.query(dev, ugid, dogrp)
        print("Quota usage and limits for %s %d are %s\n" % (typnam, ugid, fmt_quota_vals(qtup)))

    except FsQuota.error as e:
        print("Query %s %d failed: %s" % (typnam, ugid, e), file=sys.stderr)

    ##
    ##  set quota block & file limits for user
    ##

    FsQuota.setqlim(dev, ugid, *setq, 1, dogrp)
    print("Quotas set successfully for %s %d" % (typnam, ugid))

    print("Reading back new quota limits...")
    qtup = FsQuota.query(dev, ugid, dogrp)
    print("Quota usage and limits for %s %d are %s\n" % (typnam, ugid, fmt_quota_vals(qtup)))

    print("Finally checking quota sync again")
    FsQuota.sync(dev)

except FsQuota.error as e:
    print("ERROR using %s %d: %s" % (typnam, ugid, e), file=sys.stderr)
