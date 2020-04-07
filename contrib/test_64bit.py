#!/usr/bin/python3
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

new_bs = 0x150000000
new_bh = 0x160000000

uid = 32000
path = "."

try:
    # Get device from filesystem path
    dev = FsQuota.getqcarg(path)

    # Get quota for user
    try:
        (bc, bs, bh, bt, fc, fs, fh, ft) = FsQuota.query(dev, uid)
        print("CUR UID:%d - %s - %d - %d - %d - %d" % (uid, dev, bc, bs, bh, bt))
    except FsQuota.error:
        print("CUR UID:%d no quota yet" % (uid))
        fs = 0
        fh = 0

    print("SET UID:%d - %s - %d - %d" % (uid, dev, new_bs, new_bh))
    FsQuota.setqlim(dev, uid, new_bs, new_bh, fs, fh, 1)
    print("SET successfully - now reading back")

    (bc, bs, bh, bt, fc, fs, fh, ft) = FsQuota.query(dev, uid)
    print("NEW UID:%d - %s - %d - %d - %d - %d" % (uid, dev, bc, bs, bh, bt))

except FsQuota.error as e:
    print("ERROR using UID:%d: %s" % (uid, e), file=sys.stderr)
