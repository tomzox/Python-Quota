# ----------------------------------------------------------------------------
# Copyright (C) 1995-2020 T. Zoerner
# ----------------------------------------------------------------------------
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by the
# Free Software Foundation.  (Either version 2 of the GPL, or any later
# version, see http://www.opensource.org/licenses/).
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
# ----------------------------------------------------------------------------

from setuptools import setup, Extension
from distutils.command.install import install

import os
import os.path
import sys
import subprocess
import shutil
import re

this_directory = os.path.abspath(os.path.dirname(__file__))
myconfig_h = os.path.join(this_directory, 'myconfig.h')

# ----------------------------------------------------------------------------
# Note most configuration (including compile-switches) is done via includes
# in the hints/ directory. The following only manages source lists and libs
# ----------------------------------------------------------------------------

extrasrc = []
extrainc = []
extradef = []
extralibs = []
extralibdirs = []

#
# Select a configuration header file based on OS & revision
#
osr = subprocess.run(['uname', '-rs'], stdout=subprocess.PIPE).stdout.decode('utf-8')

if   re.match(r"^SunOS 4\.1", osr) : config='sunos_4_1.h'
elif re.match(r"^SunOS 5", osr)    : config='solaris_2.h'
elif re.match(r"^HP-UX (A\.09|B\.10|[BC]\.11)", osr): config='hpux.h'
elif re.match(r"^IRIX 5", osr)     : config='irix_5.h'
elif re.match(r"^IRIX\d* 6", osr)  : config='irix_6.h'
elif re.match(r"^OSF1", osr)       : config='dec_osf.h'
elif re.match(r"^Linux", osr)      : config='linux.h'
elif re.match(r"^AIX", osr)        : config='aix_4_1.h'
elif (re.match(r"^BSD\/OS 2", osr) or
      re.match(r"^Darwin", osr)    or
      re.match(r"^FreeBSD", osr)   or
      re.match(r"^NetBSD", osr)    or
      re.match(r"^OpenBSD", osr))  : config='bsd.h'
else:
    print("FATAL: No appropriate hints found for this OS/revision: \"" + osr + "\"\n" +
          "Please see instructions in file INSTALL", file=sys.stderr)
    exit(1)

config = "hints/" + config
print("Using %s for myconfig.h" % config)

if (    os.path.isfile(myconfig_h)
    and (not os.path.islink(myconfig_h) or not (os.readlink(myconfig_h) == config))):
    print("\nFATAL: myconfig.h already exists.\n\n" +
         "You need to do a \"make clean\" before configuring for a new platform.\n" +
         "If that doesn't help, remove myconfig.h manually.")
    exit(1)


# check whether the Andrew File System (AFS) is installed and running
if os.path.isdir("/afs"):
    df_afs = subprocess.run(['df', '/afs'], stdout=subprocess.PIPE).stdout.decode('utf-8')
    if re.match(r"\nAFS|\(AFS/", df_afs):
        AFSHOME = "/usr/afsws" if os.path.isdir("/usr/afsws") else "/usr"
        extradef += [('AFSQUOTA', 1)]
        extrainc += [AFSHOME+"/include", AFSHOME+"/include/afs"]
        extralibdirs += [AFSHOME+"/lib", AFSHOME+"/lib/afs"]
        extralibs += ["sys", "rx", "rxkad", "lwp"]
        extrasrc += ["src/afsquota.c"]

# check to see if we have a kernel module for the Veritas file system
if re.match(r"^SunOS", osr):
    if os.path.isfile('/usr/include/sys/fs/vx_quota.h'):
        extradef += [('SOLARIS_VXFS', 1)];
        extrasrc += ["src/vxquotactl.c"]
        print("Configured with the VERITAS File System on Solaris")
    else:
        # no warning because newer versions of Solaris have internal VxFS support
        #print("Configured without VxFS support")
        pass

# check whether we are using the NetBSD quota library
match1 = re.match(r"^NetBSD 5\.99\.(\d+)", osr, flags=re.IGNORECASE)
match2 = re.match(r"^NetBSD (\d)(\.|$)", osr, flags=re.IGNORECASE)
if (   (match1 and (int(match1.group(1)) >= 59))
    or (match2 and (int(match2.group(1)) >= 6))):
    extralibs += ["quota"]

# check whether RPCSVC is included within libc
# - SUN RPC/XDR support was split off from glibc, see:
#   https://lists.fedoraproject.org/archives/list/devel@lists.fedoraproject.org/thread/F2NRCEXDDUF6WWNPSOMXRJS6BPMTEEVJ/
# - in RHEL apparently the rpc/rpc.h header was moved too;
#   Debian has libtirpc, but headers and implementation are still in glibc too
if re.match(r"^Linux", osr):
    extrasrc += ["src/linuxapi.c"]

    if os.path.isdir('/usr/include/tirpc'):
        print("Configured to use tirpc library instead of rpcsvc")
        extrainc  += ["/usr/include/tirpc"]
        extralibs += ["tirpc"]
    else:
        if not os.path.isfile('/usr/include/rpc/rpc.h'):
            print("WARNING: Header file /usr/include/rpc/rpc.h not present on this system.\n" +
                  "         Likely compilation will fail. Recommend to either install package\n" +
                  "         \"libtirpc-dev\", or disable RPC (network file system) support by\n" +
                  "         adding the following switch to myconfig.h:\n" +
                  "         #define NO_RPC\n")
        extralibs += ["rpcsvc"]

# ----------------------------------------------------------------------------

class MyClean(install):
    cwd = os.path.abspath(os.path.dirname(__file__))
    def rmfile(self, apath):
        p = os.path.join(MyClean.cwd, apath)
        if os.path.isfile(p):
            os.remove(p)
    def rmtree(self, apath):
        p = os.path.join(MyClean.cwd, apath)
        if os.path.isdir(p):
            shutil.rmtree(p)
    def run(self):
        # files created by configuration stage
        self.rmfile('myconfig.h')
        # files created by build stage
        self.rmtree('build')
        # files created by test stage
        self.rmtree('FsQuota.egg-info')
        self.rmtree('__pycache__')
        for name in os.listdir(MyClean.cwd):
            if re.match(r"^.*\.so$", name):
                os.remove(os.path.join(MyClean.cwd, name))

# ----------------------------------------------------------------------------
# Finally execute the setup command

with open(os.path.join(this_directory, 'README.md'), encoding='utf-8') as fh:
    long_description = fh.read()

if not os.path.isfile(myconfig_h):
    os.symlink(config, myconfig_h)

# Disable use of named tuples in C module as this causes crash in GC:
# "Fatal Python error: type_traverse() called for non-heap type"
# most likely known issue: https://bugs.python.org/issue28709 - fixed in 3.8
extradef += [('NAMED_TUPLE_GC_BUG', 1)]

ext = Extension('FsQuota',
                sources       = ['src/FsQuota.c'] + extrasrc,
                include_dirs  = ['.'] + extrainc,
                define_macros = extradef,
                libraries     = extralibs,
                library_dirs  = extralibdirs,
                undef_macros  = ["NDEBUG"]   # TODO not for release
               )

setup(name='FsQuota',
      version='0.0.2',
      description='Interface to file system quotas on UNIX platforms',
      long_description=long_description,
      long_description_content_type="text/markdown",
      author='T. Zoerner',
      author_email='tomzo@users.sourceforge.net',
      url='https://github.com/tomzox/Python-Quota',
      classifiers=[
          'Development Status :: 3 - Alpha',
          "Programming Language :: C",
          'Topic :: System :: Filesystems',
          'Topic :: System :: Systems Administration',
          'Intended Audience :: Developers',
          'Intended Audience :: System Administrators',
          "Operating System :: POSIX :: Linux",
          "Operating System :: POSIX :: AIX",
          "Operating System :: POSIX :: BSD",
          "Operating System :: POSIX :: HP-UX",
          "Operating System :: POSIX :: IRIX",
          "Operating System :: POSIX :: SunOS/Solaris",
          "Operating System :: UNIX",
          "License :: OSI Approved :: GNU General Public License v2 or later (GPLv2+)"
         ],
      platforms=['posix'],
      ext_modules=[ext],
      cmdclass={'clean': MyClean},
     )
