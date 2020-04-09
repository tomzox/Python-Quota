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
    for fsname, path, fstyp, opt in FsQuota.getmntent():
        print("# %s # %s # %s # %s #" % (fsname, path, fstyp, opt))

except FsQuota.error as e:
    print("ERROR: %s" % e, file=sys.stderr)
