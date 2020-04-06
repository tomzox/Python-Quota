#!/usr/bin/python3
# ----------------------------------------------------------------------------
# Interactive test and demo script for the Python FsQuota extension module
# ----------------------------------------------------------------------------
# Author: T. Zoerner 1995-2020
#
# This program (test.py) is in the public domain and can be used and
# redistributed without restrictions.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# ----------------------------------------------------------------------------

import os
import sys
import errno
import time
import re
import FsQuota

def fmt_quota_vals(qtup):
    tm = time.localtime(qtup[3])
    bt_str = format("%04d-%02d-%02d/%02d:%02d" % (tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min))

    tm = time.localtime(qtup[7])
    ft_str = format("%04d-%02d-%02d/%02d:%02d" % (tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min))

    return format("%d (%d,%d,%s) %d (%d,%d,%s)" %
                    (qtup[0], qtup[1], qtup[2], bt_str, qtup[4], qtup[5], qtup[6], ft_str))

if not sys.stdin.isatty() or not sys.stdout.isatty():
    print("\nThis is an interactive test script - input and output must be a tty\nExiting now.\n", file=sys.stderr)
    sys.exit(0)

is_group = False
while True:
    is_group = input("\nQuery user [u] or group [g] quota? (default: user)? ")
    match = re.match(r"^([ug]?)$", is_group)
    if match:
        is_group = (match.group(1) == "g")
        break
    print("invalid response (not 'u' or 'g'), please try again", file=sys.stderr)

n_uid_gid = "GID" if is_group else "UID"

while True:
    path = input("\nEnter path to get quota for (NFS possible; default '.'): ")
    if path == "":
        path = "."

    while True:
        dev = FsQuota.getqcarg(path)
        if dev == "":
            print("%s: mount point not found" % path, file=sys.stderr)
            if os.path.isdir(path) and not path.endswith("/."):
                #
                # try to append "/." to get past automounter fs
                #
                path += "/."
                print("Trying %s instead..." % path)
                # continue loop
            else: break
        else: break

    if dev: break
    print("Using device/argument \"%s\"" % dev)

    ##
    ##  Check if quotas are present on this filesystem
    ##
    match = re.match(r"^[^/]+:", dev)
    if match:
        print("Is a remote file system")
        break

    elif FsQuota.sync(dev) and (sys.errno is not errno.EPERM):  # ignore EPERM
        print("FsQuota.sync: " + FsQuota.strerr(), file=sys.stderr)
        print("Choose another file system - quotas not functional on this one", file=sys.stderr)

    else:
        print("Quotas are present on this filesystem (sync ok)")
        break

##
##  call with one argument (uid defaults to getuid()
##

uid_val = os.getuid() if is_group else os.getgid()
print("\nQuery this fs with default %s (which is real %s) %d" % (n_uid_gid, n_uid_gid, uid_val))

if is_group == 0:
    qtup = FsQuota.query(dev)
else:
    qtup = FsQuota.query(dev, uid_val, is_group)

if qtup:
    print("Your usage and limits are %s\n" % fmt_quota_vals(qtup))
else:
    print("FsQuota.query(%s) failed: %s\n" % (dev, FsQuota.strerr()), file=sys.stderr)


##
##  call with two arguments
##

while True:
    uid_val = input("Enter a %s to get quota for: " % n_uid_gid)
    try:
        uid_val = int(uid_val)
        break
    except:
        print("You have to enter a decimal 32-bit value here.")

qtup = FsQuota.query(dev, uid_val, is_group)
if qtup:
    print("Usage and limits for %s %d are %s\n" % (n_uid_gid, uid_val, fmt_quota_vals(qtup)))
else:
    print("FsQuota.query(%s,%d,%d) failed: %s\n" % (dev, uid_val, is_group, FsQuota.strerr()), file=sys.stderr)


##
##  get quotas via RPC
##

if dev.startswith("/"):
    path = os.path.abspath(path)
    print("Query localhost:%s via RPC." % path)

    if is_group == 0:
        qtup = FsQuota.rpcquery('localhost', path)
    else:
        qtup = FsQuota.rpcquery('localhost', path, uid_val, is_group)

    if qtup is not None:
        print("Your usage and limits are %s\n" % fmt_quota_vals(qtup))
    else:
        print("Failed to query localhost: " + FsQuota.strerr())

    print("\nQuery localhost via RPC for %s %d." % (n_uid_gid, uid_val))

    qtup = FsQuota.rpcquery('localhost', path, uid_val, is_group)
    if qtup:
        print("Usage and limits for %s %d are %s\n" % (n_uid_gid, uid_val, fmt_quota_vals(qtup)))
    else:
        print("Failed to query RPC: " + FsQuota.strerr())
        print("Retrying with fake authentication for UID %d." % uid_val)
        FsQuota.rpcauth(uid_val);
        qtup = FsQuota.rpcquery('localhost', path, uid_val, is_group)
        FsQuota.rpcauth();
        if qtup:
            print("Usage and limits for %s %d are %s\n" % (n_uid_gid, uid_val, fmt_quota_vals(qtup)))
        else:
            print("Failed to query RPC again")

else:
    print("Skipping RPC query test - already done above.")


##
##  set quota block & file limits for user
##

while True:
    path = input("\nEnter path to set quota (empty to skip): ")
    if path == "":
        break

    dev = FsQuota.getqcarg(path)
    if dev:
        match = re.match(r"^[^/]+:", dev)
        if match:
            print("Heads-up: Trying to set quota for remote path will fail")
        break
    else:
        print("%s: mount point not found\n" % path, file=sys.stderr)

if path:
    bs = None
    while True:
        line = input("Enter new quota limits bs,bh,fs,fh for %s %d (empty to abort): " % (n_uid_gid, uid_val))
        match = re.match(r"^\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*$", line)
        if match:
            (bs,bh,fs,fh) = (int(match.group(1)), int(match.group(2)), int(match.group(3)), int(match.group(4)))
            break
        print("Invalid parameters: expect 4 comma-separated numerical values")

    if bs is not None:
        dev = FsQuota.getqcarg(path)
        if FsQuota.setqlim(dev, uid_val, bs,bh,fs,fh, 1) == 0:
            print("Quota set successfully for %s %d" % (n_uid_gid, uid_val))
        else:
            print("Failed to set quota: " + FsQuota.strerr(), file=sys.stderr)

##
##  Force immediate update on disk
##

match = re.match(r"^[^/]+:", dev)
if not match:
    if not FsQuota.sync(dev) == 0:
        if sys.errno != errno.EPERM:
            print("FsQuota.sync: " + FsQuota.strerr(), file=sys.stderr)
