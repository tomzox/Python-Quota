#!/usr/bin/python3
#
# Testing reading and setting 64-bit quota limits.
# This is supported by UFS on FreeBSD
#
# Author: T. Zoerner
#
# This program is in the public domain and can be used and
# redistributed without restrictions.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

import sys
import FsQuota

# set random value beyond 32-bit max.
new_bs = 0x150000000
new_bh = 0x160000000

uid = 32000
path = "."
dogrp = False

typnam = "GID" if dogrp else "UID"

try:
    # Create quota access object for the given path
    qObj = FsQuota.Quota(path)
    print("Using device \"%s\"" % qObj.dev)

    # Get quota for user
    try:
        (bc, bs, bh, bt, fc, fs, fh, ft) = qObj.query(uid, grpquota=dogrp)
        print("CUR %s:%d - %d(0x%X) - %d(0x%X) - %d(0x%X) - %d" % (typnam, uid, bc,bc, bs,bs, bh,bh, bt))
    except FsQuota.error as e:
        print("CUR %s:%d no quota yet (error %s)" % (typnam, uid, e))

    print("SET %s:%d bsoft=%d(0x%X), bhard=%d(0x%X)" % (typnam, uid, new_bs,new_bs, new_bh,new_bh))
    qObj.setqlim(uid, bsoft=new_bs, bhard=new_bh, grpquota=dogrp)
    print("SET successfully - now reading back")

    (bc, bs, bh, bt, fc, fs, fh, ft) = qObj.query(uid, grpquota=dogrp)
    print("NEW %s:%d - bsoft:%d(0x%X) bhard:%d(0x%X) (btime:%d)" % (typnam, uid, bs,bs, bh,bh, bt))
    print("OK - values match" if (bs == new_bs) and (bh==new_bh) else
          "ERROR - not matching")

except FsQuota.error as e:
    print("ERROR using %s:%d: %s" % (typnam, uid, e), file=sys.stderr)
