from setuptools import setup, Extension

#-----------------------------------------------------------------------------#
# Note most configuration (including compile-switches) is done via includes
# in the hints/ directory. The following only manages source lists and libs.
#-----------------------------------------------------------------------------#
#
# Automagically choose the right configuration
#
#X#chop($os = `uname -rs 2>/dev/null`);
#X#if   ($os =~ /^SunOS 4\.1/){ $config='sunos_4_1.h'; }
#X#elsif($os =~ /^SunOS 5/)   { $config='solaris_2.h'; }
#X#elsif($os =~ /^HP-UX (A\.09|B\.10|[BC]\.11)/) { $config='hpux.h'; }
#X#elsif($os =~ /^IRIX 5/)    { $config='irix_5.h'; }
#X#elsif($os =~ /^IRIX\d* 6/) { $config='irix_6.h'; }
#X#elsif($os =~ /^OSF1/)      { $config='dec_osf.h'; }
#X#elsif($os =~ /^Linux/)     { $config='linux.h'; $picobj='linuxapi.o'; }
#X#elsif($os =~ /^AIX/)       { $config='aix_4_1.h'; }
#X#elsif($os =~ /^BSD\/OS 2/ ||
#X#      $os =~ /^Darwin/    ||
#X#      $os =~ /^FreeBSD/   ||
#X#      $os =~ /^NetBSD/    ||
#X#      $os =~ /^OpenBSD/)   { $config='bsd.h'; }
#X#
#X#if (defined($config)) {
#X#  print "Using hints/$config for myconfig.h\n";
#X#  if (-e "myconfig.h" && (!(-l "myconfig.h") || (readlink("myconfig.h") ne "hints/$config"))) {
#X#     die "\nFATAL: myconfig.h already exists.\n\n" .
#X#         "You need to do a `make clean' before you configure for a new platform.\n".
#X#	 "If that doesn't help, remove myconfig.h manually.\n";
#X#  }
#X#}
#X#else {
#X#  warn "WARNING: No appropriate hints found for this OS: '$os - see INSTALL'\n";
#X#}
#X#
#X#my $extralibs = "";
#X#
#X## check whether the Andrew File System (AFS) is installed and running
#X#
#X#if ( -d "/afs" ) {
#X#  my $afs = `df /afs 2>/dev/null`;
#X#  if ($afs =~ /\nAFS|\(AFS/) {
#X#    $hasafs = '-DAFSQUOTA';
#X#    $AFSHOME = -d "/usr/afsws" ? "/usr/afsws" : "/usr";
#X#    $extrainc = "-I$AFSHOME/include -I$AFSHOME/include/afs";
#X#    $extralibs .= " -L$AFSHOME/lib -L$AFSHOME/lib/afs -lsys -lrx -lrxkad -llwp";
#X#    $afsquota = "afsquota.o";
#X#  }
#X#}
#X#
#X## check to see if we have a kernel module for the Veritas file system
#X#if ( $os =~ /^SunOS/ ) {
#X#   if ( -f '/usr/include/sys/fs/vx_quota.h' ) {
#X#     $hasvxfs = '-DSOLARIS_VXFS';
#X#     $extraobj = "$extraobj vxquotactl.o";
#X#     print "Configured with the VERITAS File System on Solaris\n";
#X#   }
#X#   # no warning because newer versions of Solaris have internal VxFS support
#X#   # else {
#X#   #   print "Configured without VxFS support\n";
#X#   # }
#X#}
#X#
#X## check whether we are using the NetBSD quota library
#X#if (   (($os =~ /^NetBSD 5\.99\.(\d+)/i) && ($1 >= 59))
#X#    || (($os =~ /^NetBSD (\d)(\.|$)/i) && ($1 >= 6)) ) {
#X#  $extralibs .= " -lquota";
#X#}
#X#
#X## check whether RPCSVC is included within libc
#X## - SUN RPC/XDR support was split off from glibc, see:
#X##   https://lists.fedoraproject.org/archives/list/devel@lists.fedoraproject.org/thread/F2NRCEXDDUF6WWNPSOMXRJS6BPMTEEVJ/
#X## - in RHEL apparently the rpc/rpc.h header was moved too;
#X##   Debian has libtirpc, but headers and implementation are still in glibc too
#X#if (($os =~ /^Linux/) && (-d '/usr/include/tirpc')) {
#X#  print "Configured to use tirpc library instead of rpcsvc\n";
#X#  $extrainc = "-I/usr/include/tirpc";
#X#  $rpclibs .= "-ltirpc";
#X#}
#X#else {
#X#  if (($os =~ /^Linux/) && (!-e '/usr/include/rpc/rpc.h')) {
#X#    print "WARNING: Header file /usr/include/rpc/rpc.h not present on this system.\n" .
#X#          "         Likely compilation will fail. Recommend to either install package\n" .
#X#          "         \"libtirpc-dev\", or disable RPC (network file system) support by\n" .
#X#          "         adding the following switch to myconfig.h:\n" .
#X#          "         #define NO_RPC\n";
#X#  }
#X#  $rpclibs .= "-lrpcsvc";
#X#}

#X#&WriteMakefile('NAME'         => 'Quota',
#X#               'OBJECT'       => '$(BASEEXT)$(OBJ_EXT) stdio_wrap.o '.
#X#                                 "$afsquota $picobj $extraobj ". $hint{'OBJ'},
#X#               'INC'          => $extrainc .' '. $hint{'INC'},
#X#               'DEFINE'       => "$hasafs $hasvxfs",
#X#               'LIBS'         => [ "$rpclibs $extralibs" ],
#X#               'H'            => [ 'myconfig.h' ],
#X#               'VERSION_FROM' => 'Quota.pm',
#X#               'clean'        => { FILES => 'myconfig.h' },
#X#);

#-----------------------------------------------------------------------------#

with open("README.md") as fh:
    long_description = fh.read()

ext = Extension('FsQuota',
                sources=['src/FsQuota.c', 'src/linuxapi.c'],
                include_dirs=['src', '.'])

setup(name='FsQuota',
      version='0.0.1',
      description='Interface to file system quotas on UNIX platforms',
      long_description=long_description,
      long_description_content_type="text/markdown",
      author='T. Zoerner',
      author_email='tomzo@users.sourceforge.net',
      url='https://github.com/tomzox/Python-Quota',
      ext_modules=[ext],
      platforms=[],
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
          "License :: OSI Approved :: BSD 2-Clause",
      ])
