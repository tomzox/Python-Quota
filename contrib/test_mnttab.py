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

try:
    FsQuota.setmntent()
    while True:
        ent = FsQuota.getmntent()
        if ent is None: break

        #print("# %s # %s # %s #" % (ent.mnt_fsname, ent.mnt_dir, ent.mnt_type))
        print("# %s # %s # %s # %s #" % (ent[0], ent[1], ent[2], ent[3]))

    FsQuota.endmntent()

except FsQuota.error as e:
    print("ERROR: %s" % e, file=sys.stderr)
