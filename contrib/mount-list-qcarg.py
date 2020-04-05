#!/usr/bin/python3
#
# Author: T. Zoerner
#
# This program (mount-list-qcarg.py) is in the public domain and can
# be used and redistributed without restrictions.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

import FsQuota
import os
from stat import *

#print("Quota arg type=" + FsQuota.getqcargtype() + "\n\n")

Mtab = []
if FsQuota.setmntent() == 0:
    while True:
        mnt = FsQuota.getmntent()
        if not mnt: break
        Mtab.append(mnt)
else:
    print("Error in setmntent\n");
FsQuota.endmntent();

for ent in Mtab:
    (fsname, path, fstyp, opt) = ent
    qcarg = FsQuota.getqcarg(path)
    if qcarg is None:
        qcarg = "*UNDEF*"
    print("%s # %s # %s # %s # %s # %d"
                % (fsname, path, fstyp, opt, qcarg, os.stat(path).st_dev))
