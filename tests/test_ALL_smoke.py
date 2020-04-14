#!/usr/bin/python3
#
# Smoke-test for automated testing:
# - iterate across mount table
# - for each entry try creating a Quota class instance
# - when successful, try sync and query UID twice, GID once
# - note setqlim is omitted intentionally (usually will fail as no sane
#   automation would run as root, but if so quotas would be corrupted)
# - test may fail only upon crash or mismatch in repeated UID query;
#   cannot verify failures or query results otherwise
# - tester should manually compare output with that of "quota -v"
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
import subprocess
import FsQuota

my_uid = os.getuid()
my_gid = os.getgid()

print("OS: ");
subprocess.call(['uname', '-rs']);
print("------------------------------------------------------------------\n" +
      "Output of quota -v:")
subprocess.call(['quota', '-v']);
print("------------------------------------------------------------------\n" +
      "Output of quota -v -g %s:" % my_gid)
subprocess.call(['quota', '-v', '-g', str(my_gid)]);
print("------------------------------------------------------------------")

for fsname, path, fstyp, opt in FsQuota.MntTab():
    print("\n%s:\n- fsname/typ: %s, %s\n- options: %s"
                % (path, fsname, fstyp, opt))

    try:
        qObj = FsQuota.Quota(path)

        print("- Quota.dev: " + qObj.dev)

        try:
            qObj.sync();
            print("- Quota.sync: OK")
        except FsQuota.error as e:
            print("- Quota.sync failed: %s" % e)

        try:
            qtup = qObj.query(my_uid)
            print("- Quota.query default (EUID): " + str(qtup))

            qtup2 = qObj.query(my_uid);
            try:
                print("- Quota.query UID %d: %s" % (my_uid, str(qtup)))
                if not qtup == qtup2:
                  print("ERROR: mismatching query results")
                  exit(1)
            except FsQuota.error as e:
                print("- repeated Quota.query failed: %s" % e)
                exit(1)
        except FsQuota.error as e:
            print("- Quota.query failed: %s" % e)

        try:
            qtup = qObj.query(my_gid, grpquota=True)
            print("- Quota.query GID %d: %s" % (my_gid, str(qtup)))
        except FsQuota.error as e:
            print("- Quota.query GID %d failed: %s" % (my_gid, e))
    except:
        print("- Quota::getqcarg: UNDEF")
