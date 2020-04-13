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

import os
import sys
from stat import *
import FsQuota

try:
    for fsname, path, fstyp, opt in FsQuota.MntTab():
        try:
            qObj = FsQuota.Quota(path)
            qcarg = qObj.dev
        except:
            qcarg = "*UNDEF*"

        try:
            st_dev = os.stat(path).st_dev
        except OSError:
           st_dev = -1

        print("%s # %s # %s # %s # %d # %s #"
                    % (fsname, path, fstyp, opt, st_dev, qcarg))

except FsQuota.error as e:
    print("ERROR: %s" % e, file=sys.stderr)
