// ------------------------------------------------------------------------
// FsQuota.c - Copyright (C) 1995-2020 T. Zoerner
// ------------------------------------------------------------------------
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by the
// Free Software Foundation.  (Either version 2 of the GPL, or any later
// version, see http://www.opensource.org/licenses/).
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// Perl Artistic License or GNU General Public License for more details.
// ------------------------------------------------------------------------
#define PY_SSIZE_T_CLEAN
#include "Python.h"

#define NAMED_TUPLE_GC_BUG 1
#if !defined (NAMED_TUPLE_GC_BUG)
PyTypeObject * FsQuota_MntEntType = NULL;
#endif

#include "myconfig.h"

#ifdef AFSQUOTA
#include "include/afsquota.h"
#endif

#ifdef SOLARIS_VXFS
#include "include/vxquotactl.h"
#endif

typedef struct
{
#ifndef AIX
#ifndef NO_MNTENT
    FILE *mtab;
#else /* NO_MNTENT */
#ifdef HAVE_STATVFS
    struct statvfs *mntp;
    struct statvfs *mtab;
#else
    struct statfs *mntp;
    struct statfs *mtab;
#endif
    int mtab_size;
    char flag_str_buf[16];
#endif /* NO_MNTENT */
#else /* AIX */
    struct vmount *mtab;
    int aix_mtab_idx;
    int aix_mtab_count;
#endif
} T_STATE_MNTENT;

static T_STATE_MNTENT module_state_mntent;

#define RPC_DEFAULT_TIMEOUT 4000

#ifndef NO_RPC
static struct
{
    char            use_tcp;
    unsigned short  port;
    unsigned        timeout;
} quota_rpc_cfg = {FALSE, 0, 4000};

static struct
{
    int           uid;
    int           gid;
    char          hostname[MAX_MACHINE_NAME + 1];
} quota_rpc_auth = {-1, -1, {0} };

struct quota_xs_nfs_rslt
{
    double bhard;
    double bsoft;
    double bcur;
    time_t btime;
    double fhard;
    double fsoft;
    double fcur;
    time_t ftime;
};

/*
 * fetch quotas from remote host
 */

static int
callaurpc(host, prognum, versnum, procnum, inproc, in, outproc, out)
    char *host;
    int prognum, versnum, procnum;
    xdrproc_t inproc, outproc;
    char *in, *out;
{
    struct sockaddr_in remaddr;
    struct hostent *hp;
    enum clnt_stat clnt_stat;
    struct timeval rep_time, timeout;
    CLIENT *client;
    int socket = RPC_ANYSOCK;

    /*
     *  Get IP address; by default the port is determined via remote
     *  portmap daemon; different ports and protocols can be configured
     */
    hp = gethostbyname(host);
    if (hp == NULL)
        return ((int) RPC_UNKNOWNHOST);

    rep_time.tv_sec = quota_rpc_cfg.timeout / 1000;
    rep_time.tv_usec = (quota_rpc_cfg.timeout % 1000) * 1000;
    memcpy((char *)&remaddr.sin_addr, (char *)hp->h_addr, hp->h_length);
    remaddr.sin_family = AF_INET;
    remaddr.sin_port = htons(quota_rpc_cfg.port);

    /*
     *  Create client RPC handle
     */
    client = NULL;
    if (!quota_rpc_cfg.use_tcp)
    {
        client = (CLIENT *)clntudp_create(&remaddr, prognum,
                                        versnum, rep_time, &socket);
    }
    else
    {
        client = (CLIENT *)clnttcp_create(&remaddr, prognum,
                                          versnum, &socket, 0, 0);
    }

    if (client == NULL)
        return ((int) rpc_createerr.cf_stat);

    /*
     *  Create an authentication handle
     */
    if ((quota_rpc_auth.uid != -1) && (quota_rpc_auth.gid != -1))
    {
        client->cl_auth = authunix_create(quota_rpc_auth.hostname,
                                          quota_rpc_auth.uid,
                                          quota_rpc_auth.gid, 0, 0);
    }
    else
    {
        client->cl_auth = authunix_create_default();
    }

    /*
     *  Call remote server
     */
    timeout.tv_sec = quota_rpc_cfg.timeout / 1000;
    timeout.tv_usec = (quota_rpc_cfg.timeout % 1000) * 1000;
    clnt_stat = clnt_call(client, procnum,
                          inproc, in, outproc, out, timeout);

    if (client->cl_auth)
    {
        auth_destroy(client->cl_auth);
        client->cl_auth = NULL;
    }
    clnt_destroy(client);

    return ((int) clnt_stat);
}

static int
getnfsquota( char *hostp, char *fsnamep, int uid, int kind,
             struct quota_xs_nfs_rslt *rslt )
{
    struct getquota_args gq_args;
    struct getquota_rslt gq_rslt;
#ifdef USE_EXT_RQUOTA
    ext_getquota_args ext_gq_args;

    /*
     * First try USE_EXT_RQUOTAPROG (Extended quota RPC)
     */
    ext_gq_args.gqa_pathp = fsnamep;
    ext_gq_args.gqa_id = uid;
    ext_gq_args.gqa_type = ((kind != 0) ? GQA_TYPE_GRP : GQA_TYPE_USR);

    if (callaurpc(hostp, RQUOTAPROG, EXT_RQUOTAVERS, RQUOTAPROC_GETQUOTA,
                  xdr_ext_getquota_args, &ext_gq_args,
                  xdr_getquota_rslt, &gq_rslt) != 0)
#endif
    {
        /*
         * Fall back to RQUOTAPROG if the server (or client via compile switch)
         * don't support extended quota RPC
         */
        gq_args.gqa_pathp = fsnamep;
        gq_args.gqa_uid = uid;

        if (callaurpc(hostp, RQUOTAPROG, RQUOTAVERS, RQUOTAPROC_GETQUOTA,
                      xdr_getquota_args, &gq_args,
                      xdr_getquota_rslt, &gq_rslt) != 0) {
          return (-1);
        }
    }

    switch (gq_rslt.GQR_STATUS)
    {
    case Q_OK:
    {
        struct timeval tv;
        int qb_fac;

        gettimeofday(&tv, NULL);
#ifdef LINUX_RQUOTAD_BUG
        /* Since Linux reports a bogus block size value (4k), we must not
         * use it. Thankfully Linux at least always uses 1k block sizes
         * for quota reports, so we just leave away all conversions.
         * If you have a mixed environment, you have a problem though.
         * Complain to the Linux authors or apply my patch (see INSTALL)
         */
        rslt->bhard = gq_rslt.GQR_RQUOTA.rq_bhardlimit;
        rslt->bsoft = gq_rslt.GQR_RQUOTA.rq_bsoftlimit;
        rslt->bcur = gq_rslt.GQR_RQUOTA.rq_curblocks;
#else /* not buggy */
        if (gq_rslt.GQR_RQUOTA.rq_bsize >= DEV_QBSIZE) {
          /* assign first, multiply later:
          ** so that mult works with the possibly larger type in rslt */
          rslt->bhard = gq_rslt.GQR_RQUOTA.rq_bhardlimit;
          rslt->bsoft = gq_rslt.GQR_RQUOTA.rq_bsoftlimit;
          rslt->bcur = gq_rslt.GQR_RQUOTA.rq_curblocks;

          /* we rely on the fact that block sizes are always powers of 2 */
          /* so the conversion factor will never be a fraction */
          qb_fac = gq_rslt.GQR_RQUOTA.rq_bsize / DEV_QBSIZE;
          rslt->bhard *= qb_fac;
          rslt->bsoft *= qb_fac;
          rslt->bcur *= qb_fac;
        }
        else {
          qb_fac = DEV_QBSIZE / gq_rslt.GQR_RQUOTA.rq_bsize;
          rslt->bhard = gq_rslt.GQR_RQUOTA.rq_bhardlimit / qb_fac;
          rslt->bsoft = gq_rslt.GQR_RQUOTA.rq_bsoftlimit / qb_fac;
          rslt->bcur = gq_rslt.GQR_RQUOTA.rq_curblocks / qb_fac;
        }
#endif /* LINUX_RQUOTAD_BUG */
        rslt->fhard = gq_rslt.GQR_RQUOTA.rq_fhardlimit;
        rslt->fsoft = gq_rslt.GQR_RQUOTA.rq_fsoftlimit;
        rslt->fcur = gq_rslt.GQR_RQUOTA.rq_curfiles;

        /* if time is given relative to actual time, add actual time */
        /* Note: all systems except Linux return relative times */
        if (gq_rslt.GQR_RQUOTA.rq_btimeleft == 0)
          rslt->btime = 0;
        else if (gq_rslt.GQR_RQUOTA.rq_btimeleft + 10*365*24*60*60 < tv.tv_sec)
          rslt->btime = tv.tv_sec + gq_rslt.GQR_RQUOTA.rq_btimeleft;
        else
          rslt->btime = gq_rslt.GQR_RQUOTA.rq_btimeleft;

        if (gq_rslt.GQR_RQUOTA.rq_ftimeleft == 0)
          rslt->ftime = 0;
        else if (gq_rslt.GQR_RQUOTA.rq_ftimeleft + 10*365*24*60*60 < tv.tv_sec)
          rslt->ftime = tv.tv_sec + gq_rslt.GQR_RQUOTA.rq_ftimeleft;
        else
          rslt->ftime = gq_rslt.GQR_RQUOTA.rq_ftimeleft;

        if ((gq_rslt.GQR_RQUOTA.rq_bhardlimit == 0) &&
           (gq_rslt.GQR_RQUOTA.rq_bsoftlimit == 0) &&
           (gq_rslt.GQR_RQUOTA.rq_fhardlimit == 0) &&
           (gq_rslt.GQR_RQUOTA.rq_fsoftlimit == 0)) {
          errno = ESRCH;
          return(-1);
        }
        return (0);
    }

    case Q_NOQUOTA:
        errno = ESRCH;
        break;

    case Q_EPERM:
        errno = EPERM;
        break;

    default:
        errno = EINVAL;
        break;
    }
    return -1;
}

#ifdef MY_XDR

static struct xdr_discrim gq_des[2] =
{
    { (int)Q_OK, (xdrproc_t)xdr_rquota },
    { 0, NULL }
};

bool_t
xdr_getquota_args( XDR *xdrs, struct getquota_args *gqp )
{
    return (xdr_string(xdrs, &gqp->gqa_pathp, 1024) &&
            xdr_int(xdrs, &gqp->gqa_uid));
}

bool_t
xdr_getquota_rslt( XDR *xdrs, struct getquota_rslt *gqp )
{
    return (xdr_union(xdrs,
                      (int *) &gqp->GQR_STATUS, (char *) &gqp->GQR_RQUOTA,
                      gq_des, (xdrproc_t) xdr_void));
}

bool_t
xdr_rquota( XDR *xdrs, struct rquota *rqp )
{
    return (xdr_int(xdrs, &rqp->rq_bsize) &&
            xdr_bool(xdrs, &rqp->rq_active) &&
            xdr_u_long(xdrs, (unsigned long *)&rqp->rq_bhardlimit) &&
            xdr_u_long(xdrs, (unsigned long *)&rqp->rq_bsoftlimit) &&
            xdr_u_long(xdrs, (unsigned long *)&rqp->rq_curblocks) &&
            xdr_u_long(xdrs, (unsigned long *)&rqp->rq_fhardlimit) &&
            xdr_u_long(xdrs, (unsigned long *)&rqp->rq_fsoftlimit) &&
            xdr_u_long(xdrs, (unsigned long *)&rqp->rq_curfiles) &&
            xdr_u_long(xdrs, (unsigned long *)&rqp->rq_btimeleft) &&
            xdr_u_long(xdrs, (unsigned long *)&rqp->rq_ftimeleft) );
}
#endif /* MY_XDR */

#ifdef USE_EXT_RQUOTA
bool_t
xdr_ext_getquota_args( XDR *xdrs, ext_getquota_args *objp )
{
    return (xdr_string(xdrs, &objp->gqa_pathp, RQ_PATHLEN) &&
            xdr_int(xdrs, &objp->gqa_type) &&
            xdr_int(xdrs, &objp->gqa_id));
}
#endif /* USE_EXT_RQUOTA */

#endif /* !NO_RPC */

/* ----------------------------------------------------------------------------
 *
 *  External interfaces
 *
 */

static PyObject *
FsQuota_query(PyObject *self, PyObject *args)
{
    char *  dev = NULL;
    int     uid = getuid();
    int     kind = 0;

    if (!PyArg_ParseTuple(args, "s|ii", &dev, &uid, &kind))
        return NULL;

    PyObject * RETVAL = Py_None;
    char *p = NULL;
    int err;
#ifdef SGI_XFS
    if (!strncmp(dev, "(XFS)", 5))
    {
        fs_disk_quota_t xfs_dqblk;
#ifndef linux
        err = quotactl(Q_XGETQUOTA, dev+5, uid, CADR &xfs_dqblk);
#else
        err = quotactl(QCMD(Q_XGETQUOTA, ((kind == 2) ? XQM_PRJQUOTA : ((kind == 1) ? XQM_GRPQUOTA : XQM_USRQUOTA))), dev+5, uid, CADR &xfs_dqblk);
#endif
        if (!err)
        {
            RETVAL = Py_BuildValue("KKKiKKKi",
                                   (long long) QX_DIV(xfs_dqblk.d_bcount),
                                   (long long) QX_DIV(xfs_dqblk.d_blk_softlimit),
                                   (long long) QX_DIV(xfs_dqblk.d_blk_hardlimit),
                                   xfs_dqblk.d_btimer,
                                   (long long) xfs_dqblk.d_icount,
                                   (long long) xfs_dqblk.d_ino_softlimit,
                                   (long long) xfs_dqblk.d_ino_hardlimit,
                                   xfs_dqblk.d_itimer);
        }
    }
    else
#endif
#ifdef SOLARIS_VXFS
    if (!strncmp(dev, "(VXFS)", 6))
    {
        struct vx_dqblk vxfs_dqb;
        err = vx_quotactl(VX_GETQUOTA, dev+6, uid, CADR &vxfs_dqb);
        if (!err)
        {
            RETVAL = Py_BuildValue("KKKiKKKi",
                                   (long long) Q_DIV(vxfs_dqb.dqb_curblocks),
                                   (long long) Q_DIV(vxfs_dqb.dqb_bsoftlimit),
                                   (long long) Q_DIV(vxfs_dqb.dqb_bhardlimit),
                                   vxfs_dqb.dqb_btimelimit,
                                   (long long) vxfs_dqb.dqb_curfiles,
                                   (long long) vxfs_dqb.dqb_fsoftlimit,
                                   (long long) vxfs_dqb.dqb_fhardlimit,
                                   vxfs_dqb.dqb_ftimelimit);
        }
    }
    else
#endif
#ifdef AFSQUOTA
    if (!strncmp(dev, "(AFS)", 5))
    {
        if (!afs_check())  /* check is *required* as setup! */
        {
            errno = EINVAL;
        }
        else
        {
            int maxQuota, blocksUsed;

            err = afs_getquota(dev + 5, &maxQuota, &blocksUsed);
            if (!err)
            {
                RETVAL = Py_BuildValue("iiiiiiii",
                                       blocksUsed,
                                       maxQuota,
                                       maxQuota,
                                       0,
                                       0,
                                       0,
                                       0,
                                       0);
            }
        }
    }
    else
#endif
    {
        if ((*dev != '/') && (p = strchr(dev, ':')))
        {
#ifndef NO_RPC
            struct quota_xs_nfs_rslt rslt;
            *p = '\0';
            err = getnfsquota(dev, p+1, uid, kind, &rslt);
            if (!err)
            {
                RETVAL = Py_BuildValue("KKKiKKKi",
                                       (long long) Q_DIV(rslt.bcur),
                                       (long long) Q_DIV(rslt.bsoft),
                                       (long long) Q_DIV(rslt.bhard),
                                       rslt.btime,
                                       (long long) rslt.fcur,
                                       (long long) rslt.fsoft,
                                       (long long) rslt.fhard,
                                       rslt.ftime);
            }
            *p = ':';
#else /* NO_RPC */
            errno = ENOSYS;
            err = -1;
#endif /* NO_RPC */
        }
        else
        {
#ifdef NETBSD_LIBQUOTA
            struct quotahandle *qh = quota_open(dev);
            if (qh != NULL)
            {
                struct quotakey qk_blocks, qk_files;
                struct quotaval qv_blocks, qv_files;

                qk_blocks.qk_idtype = /*fall-through*/
                qk_files.qk_idtype = kind ? QUOTA_IDTYPE_GROUP : QUOTA_IDTYPE_USER;
                qk_blocks.qk_id = qk_files.qk_id = uid;
                qk_blocks.qk_objtype = QUOTA_OBJTYPE_BLOCKS;
                qk_files.qk_objtype = QUOTA_OBJTYPE_FILES;

                if (   (quota_get(qh, &qk_blocks, &qv_blocks) >= 0)
                    && (quota_get(qh, &qk_files, &qv_files) >= 0) )
                {
                    RETVAL = Py_BuildValue("KKKiKKKi",
                                           (long long) Q_DIV(qv_blocks.qv_usage),
                                           (long long) Q_DIV(qv_blocks.qv_softlimit),
                                           (long long) Q_DIV(qv_blocks.qv_hardlimit),
                                           qv_blocks.qv_expiretime,
                                           (long long) qv_files.qv_usage,
                                           (long long) qv_files.qv_softlimit,
                                           (long long) qv_files.qv_hardlimit,
                                           qv_files.qv_expiretime);
                }
                quota_close(qh);
            }
#else /* not NETBSD_LIBQUOTA */
            struct dqblk dqblk;
#ifdef USE_IOCTL
            struct quotactl qp;
            int fd = -1;

            qp.op = Q_GETQUOTA;
            qp.uid = uid;
            qp.addr = (char *)&dqblk;
            if ((fd = open(dev, O_RDONLY)) != -1)
            {
                err = (ioctl(fd, Q_QUOTACTL, &qp) == -1);
                close(fd);
            }
            else
            {
                err = 1;
            }
#else /* not USE_IOCTL */
#ifdef Q_CTL_V3  /* Linux */
            err = linuxquota_query(dev, uid, (kind != 0), &dqblk);
#else /* not Q_CTL_V3 */
#ifdef Q_CTL_V2
#ifdef AIX
            /* AIX quotactl doesn't fail if path does not exist!? */
            struct stat st;
#if defined(HAVE_JFS2)
            if (strncmp(dev, "(JFS2)", 6) == 0) {
                if (stat(dev + 6, &st) == 0) {
                    quota64_t user_quota;

                    err = quotactl(dev + 6, QCMD(Q_J2GETQUOTA, ((kind != 0) ? GRPQUOTA : USRQUOTA)),
                                   uid, CADR &user_quota);
                    if (!err) {
                        RETVAL = Py_BuildValue("KKKiKKKi",
                                               user_quota.bused,
                                               user_quota.bsoft,
                                               user_quota.bhard,
                                               user_quota.btime,
                                               user_quota.ihard,
                                               user_quota.isoft,
                                               user_quota.iused,
                                               user_quota.itime);
                    }
                }
                err = 1; /* dummy to suppress duplicate push below */
            }
#endif /* HAVE_JFS2 */
            else if (stat(dev, &st))
            {
                err = 1;
            }
            else
#endif /* AIX */
            err = quotactl(dev, QCMD(Q_GETQUOTA, ((kind != 0) ? GRPQUOTA : USRQUOTA)), uid, CADR &dqblk);
#else /* not Q_CTL_V2 */
            err = quotactl(Q_GETQUOTA, dev, uid, CADR &dqblk);
#endif /* not Q_CTL_V2 */
#endif /* Q_CTL_V3 */
#endif /* not USE_IOCTL */
            if (!err)
            {
                RETVAL = Py_BuildValue("KKKiKKKi",
                                       (long long) Q_DIV(dqblk.QS_BCUR),
                                       (long long) Q_DIV(dqblk.QS_BSOFT),
                                       (long long) Q_DIV(dqblk.QS_BHARD),
                                       dqblk.QS_BTIME,
                                       (long long) dqblk.QS_FCUR,
                                       (long long) dqblk.QS_FSOFT,
                                       (long long) dqblk.QS_FHARD,
                                       dqblk.QS_FTIME);
            }
#endif /* not NETBSD_LIBQUOTA */
        }
    }
    return RETVAL;
}

static PyObject *
FsQuota_setqlim(PyObject *self, PyObject *args)
{
    char *  dev = NULL;
    int     uid = -1;
    unsigned long long  bs = 0;
    unsigned long long  bh = 0;
    unsigned long long  fs = 0;
    unsigned long long  fh = 0;
    int     timelimflag = 0;
    int     kind = 0;

    if (!PyArg_ParseTuple(args, "siKKKK|dd", &dev, &uid, &bs, &bh, &fs, &fh, &timelimflag, &kind))
        return NULL;

    int RETVAL;

#ifndef NETBSD_LIBQUOTA
    struct dqblk dqblk;
#endif
#ifdef USE_IOCTL
    struct quotactl qp;
    int fd;

    qp.op = Q_SETQLIM;
    qp.uid = uid;
    qp.addr = (char *)&dqblk;
#endif
    if (timelimflag != 0)
        timelimflag = 1;
#ifdef SGI_XFS
    if (!strncmp(dev, "(XFS)", 5))
    {
        fs_disk_quota_t xfs_dqblk;

        xfs_dqblk.d_blk_softlimit = QX_MUL(bs);
        xfs_dqblk.d_blk_hardlimit = QX_MUL(bh);
        xfs_dqblk.d_btimer        = timelimflag;
        xfs_dqblk.d_ino_softlimit = fs;
        xfs_dqblk.d_ino_hardlimit = fh;
        xfs_dqblk.d_itimer        = timelimflag;
        xfs_dqblk.d_fieldmask     = FS_DQ_LIMIT_MASK;
        xfs_dqblk.d_flags         = XFS_USER_QUOTA;
#ifndef linux
        RETVAL = quotactl(Q_XSETQLIM, dev+5, uid, CADR &xfs_dqblk);
#else
        RETVAL = quotactl(QCMD(Q_XSETQLIM, ((kind == 2) ? XQM_PRJQUOTA : ((kind == 1) ? XQM_GRPQUOTA : XQM_USRQUOTA))), dev+5, uid, CADR &xfs_dqblk);
#endif
    }
    else
      /* if not xfs, than it's a classic IRIX efs file system */
#endif
#ifdef SOLARIS_VXFS
    if (!strncmp(dev, "(VXFS)", 6))
    {
        struct vx_dqblk vxfs_dqb;

        vxfs_dqb.dqb_bsoftlimit = Q_MUL(bs);
        vxfs_dqb.dqb_bhardlimit = Q_MUL(bh);
        vxfs_dqb.dqb_btimelimit = timelimflag;
        vxfs_dqb.dqb_fsoftlimit = fs;
        vxfs_dqb.dqb_fhardlimit = fh;
        vxfs_dqb.dqb_ftimelimit = timelimflag;
        RETVAL = vx_quotactl(VX_SETQUOTA, dev+6, uid, CADR &vxfs_dqb);
    }
    else
#endif
#ifdef AFSQUOTA
    if (!strncmp(dev, "(AFS)", 5))
    {
        if (!afs_check())  /* check is *required* as setup! */
        {
            errno = EINVAL;
            RETVAL = -1;
        }
        else
            RETVAL = afs_setqlim(dev + 5, bh);
    }
    else
#endif
#if defined(HAVE_JFS2)
    if (strncmp(dev, "(JFS2)", 6) == 0)
    {
        quota64_t user_quota;

        RETVAL = quotactl(dev + 6, QCMD(Q_J2GETQUOTA, ((kind != 0) ? GRPQUOTA : USRQUOTA)),
                          uid, CADR &user_quota);
        if (RETVAL == 0)
        {
            user_quota.bsoft = bs;
            user_quota.bhard = bh;
            user_quota.btime = timelimflag;
            user_quota.isoft = fs;
            user_quota.ihard = fh;
            user_quota.itime = timelimflag;
            RETVAL = quotactl(dev + 6, QCMD(Q_J2PUTQUOTA, ((kind != 0) ? GRPQUOTA : USRQUOTA)),
                              uid, CADR &user_quota);
        }
    }
    else
#endif /* HAVE_JFS2 */
    {
#ifdef NETBSD_LIBQUOTA
        struct quotahandle *qh;
        struct quotakey qk;
        struct quotaval qv;

        RETVAL = -1;
        qh = quota_open(dev);
        if (qh != NULL)
        {
            qk.qk_idtype = kind ? QUOTA_IDTYPE_GROUP : QUOTA_IDTYPE_USER;
            qk.qk_id = uid;

            qk.qk_objtype = QUOTA_OBJTYPE_BLOCKS;

            /* set the grace period for blocks */
            if (timelimflag)  /* seven days */
            {
                qv.qv_grace = 7*24*60*60;
            }
            else if (quota_get(qh, &qk, &qv) >= 0)  /* use user's current setting */
            {
                /* OK */
            }
            else if (qk.qk_id = QUOTA_DEFAULTID, quota_get(qh, &qk, &qv) >= 0)  /* use default setting */
            {
                /* OK, reset qk_id */
                qk.qk_id = uid;
            }
            else
            {
                qv.qv_grace = 0;
            }

            qv.qv_usage = 0;
            qv.qv_hardlimit = Q_MUL(bh);
            qv.qv_softlimit = Q_MUL(bs);
            qv.qv_expiretime = 0;
            if (quota_put(qh, &qk, &qv) >= 0)
            {
                qk.qk_objtype = QUOTA_OBJTYPE_FILES;

                /* set the grace period for files, see comments above */
                if (timelimflag)
                {
                    qv.qv_grace = 7*24*60*60;
                }
                else if (quota_get(qh, &qk, &qv) >= 0)
                {
                    /* OK */
                }
                else if (qk.qk_id = QUOTA_DEFAULTID, quota_get(qh, &qk, &qv) >= 0)
                {
                    /* OK, reset qk_id */
                    qk.qk_id = uid;
                }
                else
                {
                    qv.qv_grace = 0;
                }

                qv.qv_usage = 0;
                qv.qv_hardlimit = fh;
                qv.qv_softlimit = fs;
                qv.qv_expiretime = 0;
                if (quota_put(qh, &qk, &qv) >= 0)
                {
                    RETVAL = 0;
                }
            }
            quota_close(qh);
        }
#else /* not NETBSD_LIBQUOTA */
        memset(&dqblk, 0, sizeof(dqblk));
        dqblk.QS_BSOFT = Q_MUL(bs);
        dqblk.QS_BHARD = Q_MUL(bh);
        dqblk.QS_BTIME = timelimflag;
        dqblk.QS_FSOFT = fs;
        dqblk.QS_FHARD = fh;
        dqblk.QS_FTIME = timelimflag;
#ifdef USE_IOCTL
        if ((fd = open(dev, O_RDONLY)) != -1)
        {
            RETVAL = (ioctl(fd, Q_QUOTACTL, &qp) != 0);
            close(fd);
        }
        else
            RETVAL = -1;
#else
#ifdef Q_CTL_V3  /* Linux */
        RETVAL = linuxquota_setqlim (dev, uid, (kind != 0), &dqblk);
#else
#ifdef Q_CTL_V2
        RETVAL = quotactl (dev, QCMD(Q_SETQUOTA,((kind != 0) ? GRPQUOTA : USRQUOTA)), uid, CADR &dqblk);
#else
        RETVAL = quotactl (Q_SETQLIM, dev, uid, CADR &dqblk);
#endif
#endif
#endif
#endif /* not NETBSD_LIBQUOTA */
    }

    return PyLong_FromLong(RETVAL);
}

static PyObject *
FsQuota_sync(PyObject *self, PyObject *args)
{
    char *  dev = NULL;

    if (!PyArg_ParseTuple(args, "|s", &dev))
        return NULL;

    int RETVAL;
#ifdef SOLARIS_VXFS
    if ((dev != NULL) && !strncmp(dev, "(VXFS)", 6))
    {
        RETVAL = vx_quotactl(VX_QSYNCALL, dev+6, 0, NULL);
    }
    else
#endif
#ifdef AFSQUOTA
    if ((dev != NULL) && !strncmp(dev, "(AFS)", 5))
    {
        if (!afs_check())
        {
            errno = EINVAL;
            RETVAL = -1;
        }
        else
        {
            int foo1, foo2;
            RETVAL = (afs_getquota(dev + 5, &foo1, &foo2) ? -1 : 0);
        }
    }
    else
#endif
#ifdef NETBSD_LIBQUOTA
    RETVAL = 0;
#else /* not NETBSD_LIBQUOTA */
#ifdef USE_IOCTL
    {
        struct quotactl qp;
        int fd;

        if (dev == NULL)
        {
            qp.op = Q_ALLSYNC;
            dev = "/";   /* is probably ignored anyways */
        }
        else
            qp.op = Q_SYNC;

        if ((fd = open(dev, O_RDONLY)) != -1)
        {
            RETVAL = (ioctl(fd, Q_QUOTACTL, &qp) != 0);
            if (errno == ESRCH) errno = EINVAL;
            close(fd);
        }
        else
            RETVAL = -1;
    }
#else
    {
#ifdef Q_CTL_V3  /* Linux */
#ifdef SGI_XFS
        if ((dev != NULL) && (!strncmp(dev, "(XFS)", 5)))
        {
            struct fs_quota_stat fsq_stat;

            if (!quotactl(QCMD(Q_XGETQSTAT, USRQUOTA), dev+5, 0, CADR &fsq_stat))
            {
                if (fsq_stat.qs_flags & (XFS_QUOTA_UDQ_ACCT | XFS_QUOTA_GDQ_ACCT))
                {
                    RETVAL = 0;
                }
                else if (   (strcmp(dev+5, "/") == 0)
                         && (  ((fsq_stat.qs_flags & 0xff00) >> 8)
                             & (XFS_QUOTA_UDQ_ACCT | XFS_QUOTA_GDQ_ACCT)) )
                {
                    RETVAL = 0;
                }
                else
                {
                    errno = ENOENT;
                    RETVAL = -1;
                }
            }
            else
            {
                errno = ENOENT;
                RETVAL = -1;
            }
        }
        else
#endif
        RETVAL = linuxquota_sync (dev, FALSE);
#else
#ifdef Q_CTL_V2
#ifdef AIX
        struct stat st;
#endif
        if (dev == NULL)
            dev = "/";
#ifdef AIX
#if defined(HAVE_JFS2)
        if (strncmp(dev, "(JFS2)", 6) == 0)
            dev += 6;
#endif
        if (stat(dev, &st))
        {
            RETVAL = -1;
        }
        else
#endif
        RETVAL = quotactl(dev, QCMD(Q_SYNC, USRQUOTA), 0, NULL);
#else
#ifdef SGI_XFS
#define XFS_UQUOTA (XFS_QUOTA_UDQ_ACCT|XFS_QUOTA_UDQ_ENFD)
        /* Q_SYNC is not supported on XFS filesystems, so emulate it */
        if ((dev != NULL) && (!strncmp(dev, "(XFS)", 5)))
        {
            fs_quota_stat_t fsq_stat;

            sync();

            RETVAL = quotactl(Q_GETQSTAT, dev+5, 0, CADR &fsq_stat);

            if (!RETVAL && ((fsq_stat.qs_flags & XFS_UQUOTA) != XFS_UQUOTA))
            {
                errno = ENOENT;
                RETVAL = -1;
            }
        }
        else
#endif
        RETVAL = quotactl(Q_SYNC, dev, 0, NULL);
#endif
#endif
    }
#endif
#endif /* NETBSD_LIBQUOTA */

    return PyLong_FromLong(RETVAL);
}

static PyObject *
FsQuota_rpcquery(PyObject *self, PyObject *args)
{
    char *  host = NULL;
    char *  path = NULL;
    int     uid = getuid();
    int     kind = 0;

    if (!PyArg_ParseTuple(args, "ss|ii", &host, &path, &uid, &kind))
        return NULL;

    PyObject * RETVAL = Py_None;

#ifndef NO_RPC
    struct quota_xs_nfs_rslt rslt;
    if (getnfsquota(host, path, uid, kind, &rslt) == 0)
    {
        RETVAL = Py_BuildValue("LLLiLLLi",
                               (long long) Q_DIV(rslt.bcur),
                               (long long) Q_DIV(rslt.bsoft),
                               (long long) Q_DIV(rslt.bhard),
                               rslt.btime,
                               (long long) rslt.fcur,
                               (long long) rslt.fsoft,
                               (long long) rslt.fhard,
                               rslt.ftime);
    }
#else
    errno = ENOSYS;
#endif
    return RETVAL;
}

static PyObject *
FsQuota_rpcpeer(PyObject *self, PyObject *args)
{
    unsigned port = 0;
    int      use_tcp = FALSE;
    unsigned timeout = RPC_DEFAULT_TIMEOUT;

    if (!PyArg_ParseTuple(args, "|IpI", &port, &use_tcp, &timeout))
        return NULL;

#ifndef NO_RPC
    quota_rpc_cfg.port = port;
    quota_rpc_cfg.use_tcp = use_tcp;
    quota_rpc_cfg.timeout = timeout;
#endif

    return Py_None;
}

static PyObject *
FsQuota_rpcauth(PyObject *self, PyObject *args)
{
    int uid = -1;
    int gid = -1;
    char * hostname = NULL;
    int RETVAL;

    if (!PyArg_ParseTuple(args, "|iis", &uid, &gid, &hostname))
        return NULL;

#ifndef NO_RPC
    if ((uid == -1) && (gid == -1) && (hostname == NULL))
    {
        /* reset to default values */
        quota_rpc_auth.uid = uid;
        quota_rpc_auth.gid = gid;
        quota_rpc_auth.hostname[0] = 0;
        RETVAL = 0;
    }
    else
    {
        if (uid == -1)
            quota_rpc_auth.uid = getuid();
        else
            quota_rpc_auth.uid = uid;

        if (gid == -1)
            quota_rpc_auth.gid = getgid();
        else
            quota_rpc_auth.gid = gid;

        if (hostname == NULL)
        {
            RETVAL = gethostname(quota_rpc_auth.hostname, MAX_MACHINE_NAME);
        }
        else if (strlen(hostname) < MAX_MACHINE_NAME)
        {
            strcpy(quota_rpc_auth.hostname, hostname);
            RETVAL = 0;
        }
        else
        {
            errno = EINVAL;
            RETVAL = -1;
        }
    }
#endif
    return PyLong_FromLong(RETVAL);
}

// ----------------------------------------------------------------------------

int my_setmntent(T_STATE_MNTENT * state)
{
    int RETVAL = 0;

#ifndef AIX
#ifndef NO_MNTENT
#ifndef NO_OPEN_MNTTAB
    if (state->mtab != NULL) endmntent(state->mtab);
    if ((state->mtab = setmntent(MOUNTED, "r")) == NULL)
#else
    if (state->mtab != NULL) fclose(state->mtab);
    if ((state->mtab = fopen (MOUNTED,"r")) == NULL)
#endif
        RETVAL = -1;
    else
        RETVAL = 0;
#else /* NO_MNTENT */

    /* if (state->mtab != NULL) free(state->mtab); */
    state->mtab_size = getmntinfo(&state->mtab, MNT_NOWAIT);
    if (state->mtab_size <= 0)
        RETVAL = -1;
    else
        RETVAL = 0;
    state->mntp = state->mtab;
#endif
#else /* AIX */
    int count, space;

    if (state->mtab != NULL)
        free(state->mtab);

    count = mntctl(MCTL_QUERY, sizeof(space), (char *) &space);
    if (count == 0)
    {
        state->mtab = (struct vmount *) malloc(space);
        if (state->mtab != NULL)
        {
            count = mntctl(MCTL_QUERY, space, (char *) state->mtab);
            if (count > 0)
            {
                state->aix_mtab_count = count;
                state->aix_mtab_idx   = 0;
                RETVAL = 0;
            }
            else  /* error, or size changed between calls */
            {
                if (count == 0) errno = EINTR;
                RETVAL = -1;
            }
        }
        else
            RETVAL = -1;
    }
    else if (count < 0)
    {
        RETVAL = -1;
    }
    else  /* should never happen */
    {
        errno = ENOENT;
        RETVAL = -1;
    }
#endif
    return RETVAL;
}

int my_getmntent(T_STATE_MNTENT * state, char **str_buf)
{
    int RETVAL = -1;
#ifndef AIX
#ifndef NO_MNTENT
#ifndef NO_OPEN_MNTTAB
    struct mntent *mntp;
    if (state->mtab != NULL)
    {
        mntp = getmntent(state->mtab);
        if (mntp != NULL)
        {
            str_buf[0] = mntp->mnt_fsname;
            str_buf[1] = mntp->mnt_dir;
            str_buf[2] = mntp->mnt_type;
            str_buf[3] = mntp->mnt_opts;
            RETVAL = 0;
        }
    }
    else
        errno = EBADF;

#else /* NO_OPEN_MNTTAB */
    struct mnttab mntp;
    if (state->mtab != NULL)
    {
        if (getmntent(state->mtab, &mntp) == 0)
        {
            str_buf[0] = mntp->mnt_special;
            str_buf[1] = mntp->mnt_mountp;
            str_buf[2] = mntp->mnt_ftype;
            str_buf[3] = mntp->mnt_mntopts;
            RETVAL = 0;
        }
    }

#endif /* NO_OPEN_MNTTAB */
#else /* NO_MNTENT */
#ifdef OSF_QUOTA
    char *fstype = getvfsbynumber((int)state->mntp->f_type);
#endif
    if ((state->mtab != NULL) && state->mtab_size)
    {
        str_buf[0] = state->mntp->f_mntfromname;
        str_buf[1] = state->mntp->f_mntonname;
#ifdef OSF_QUOTA
        if (fstype != (char *) -1)
        {
            str_buf[2] = fstype;
        }
        else
#endif
        {
#ifdef __OpenBSD__
            /* OpenBSD struct statfs lacks the f_type member (starting with release 2.7) */
            str_buf[2] = "";
#else /* !__OpenBSD__ */
#ifdef HAVE_STATVFS
            str_buf[2] = state->mntp->f_fstypename;
#else
            str_buf[2] = state->mntp->f_type;
#endif /* HAVE_STATVFS */
#endif /* !__OpenBSD__ */
        }
#ifdef HAVE_STATVFS
        snprintf(state->flag_str_buf, sizeof(state->flag_str_buf), "%d", state->mntp->f_flag);
#else
        snprintf(state->flag_str_buf, sizeof(state->flag_str_buf), "%d", state->mntp->f_flags);
#endif
        str_buf[3] = state->flag_str_buf;
        RETVAL = 0;

        state->mtab_size--;
        state->mntp++;
    }
#endif
#else /* AIX */
    struct vmount *vmp;
    char *cp;
    int i;

    if ((state->mtab != NULL) && (state->aix_mtab_idx < state->aix_mtab_count))
    {
        cp = (char *) state->mtab;
        for (i=0; i<state->aix_mtab_idx; i++)
        {
            vmp = (struct vmount *) cp;
            cp += vmp->vmt_length;
        }
        vmp = (struct vmount *) cp;
        state->aix_mtab_idx += 1;

        if ((vmp->vmt_gfstype != MNT_NFS) && (vmp->vmt_gfstype != MNT_NFS3))
        {
            cp = vmt2dataptr(vmp, VMT_OBJECT);
            str_buf[0] = cp;
        }
        else
        {
            uchar *mp, *cp2;
            cp = vmt2dataptr(vmp, VMT_HOST);
            cp2 = vmt2dataptr(vmp, VMT_OBJECT);
            mp = malloc(strlen(cp) + strlen(cp2) + 2);
            if (mp != NULL)
            {
                strcpy(mp, cp);
                strcat(mp, ":");
                strcat(mp, cp2);
                str_buf[0] = mp;
                free(mp);
            }
            else
            {
                cp = "?";
                str_buf[0] = cp;
            }
        }
        cp = vmt2dataptr(vmp, VMT_STUB);
        str_buf[1] = cp;

        switch(vmp->vmt_gfstype)
        {
            case MNT_NFS:   cp = "nfs"; break;
            case MNT_NFS3:  cp = "nfs"; break;
            case MNT_JFS:   cp = "jfs"; break;
#if defined(MNT_AIX) && defined(MNT_J2) && (MNT_AIX==MNT_J2)
            case MNT_J2:    cp = "jfs2"; break;
#else
#if defined(MNT_J2)
            case MNT_J2:    cp = "jfs2"; break;
#endif
            case MNT_AIX:   cp = "aix"; break;
#endif
            case 4:         cp = "afs"; break;
            case MNT_CDROM: cp = "cdrom,ignore"; break;
            default:        cp = "unknown,ignore"; break;
        }
        str_buf[2] = cp;

        cp = vmt2dataptr(vmp, VMT_ARGS);
        str_buf[3] = cp;
        RETVAL = 0;
    }
#endif  /* AIX */
    return RETVAL;
}

void my_endmntent(T_STATE_MNTENT * state)
{
    if (state->mtab != NULL)
    {
#ifndef AIX
#ifndef NO_MNTENT
#ifndef NO_OPEN_MNTTAB
        endmntent(state->mtab);   /* returns always 1 in SunOS */
#else
        fclose (state->mtab);
#endif
        /* #else: if (state->mtab != NULL) free(state->mtab); */
#endif
#else /* AIX */
        free(state->mtab);
#endif
        state->mtab = NULL;
    }
}

// ----------------------------------------------------------------------------

static PyObject *
FsQuota_setmntent(PyObject *self, PyObject *args)  // METH_NOARGS
{
    int RETVAL = my_setmntent(&module_state_mntent);

    return PyLong_FromLong(RETVAL);
}

static PyObject *
FsQuota_getmntent(PyObject *self, PyObject *args)  // METH_NOARGS
{
    PyObject * RETVAL = NULL;

    char *str_buf[4];
    if (my_getmntent(&module_state_mntent, str_buf) == 0)
    {
#if defined (NAMED_TUPLE_GC_BUG)
        RETVAL = Py_BuildValue("ssss", str_buf[0], str_buf[1], str_buf[2], str_buf[3]);
#else
        RETVAL = PyStructSequence_New(FsQuota_MntEntType);
        if (str_buf[0] != NULL)
            PyStructSequence_SetItem(RETVAL, 0, PyUnicode_FromString(str_buf[0]));
        if (str_buf[1] != NULL)
            PyStructSequence_SetItem(RETVAL, 1, PyUnicode_FromString(str_buf[1]));
        if (str_buf[2] != NULL)
            PyStructSequence_SetItem(RETVAL, 2, PyUnicode_FromString(str_buf[2]));
        if (str_buf[3] != NULL)
            PyStructSequence_SetItem(RETVAL, 3, PyUnicode_FromString(str_buf[3]));
#endif
        return RETVAL;
    }
    else
        return Py_None;
}

static PyObject *
FsQuota_endmntent(PyObject *self, PyObject *args)  // METH_NOARGS
{
    my_endmntent(&module_state_mntent);

    return Py_None;
}

// ----------------------------------------------------------------------------

//
//  Get "device" argument for this module's Quota-functions
//

static PyObject *
FsQuota_getqcarg(PyObject *self, PyObject *args)
{
    char * target_path = (char*) ".";
    struct stat statbuf;

    if (!PyArg_ParseTuple(args, "|s", &target_path))
        return NULL;

    PyObject * RETVAL = Py_None;
    int target_dev;
    if (stat(target_path, &statbuf) == 0)
        target_dev = statbuf.st_dev;
    else
        target_dev = -1;

    T_STATE_MNTENT l_mntab;
    memset(&l_mntab, 0, sizeof(l_mntab));
    if ((target_dev != -1) && (my_setmntent(&l_mntab) == 0))
    {
        char *mnt_str_buf[4];
        while (my_getmntent(&l_mntab, mnt_str_buf) == 0)
        {
            const char * const fsname = mnt_str_buf[0];
            const char * const path   = mnt_str_buf[1];
            const char * const fstyp  = mnt_str_buf[2];
            const char * const fsopt  = mnt_str_buf[3];

            if (   (strcmp(fstyp, "lofs") == 0)
                || (strcmp(fstyp, "ignore") == 0)
                || (strcmp(fstyp, "proc") == 0)
                || (strcmp(fstyp, "rootfs") == 0)
                || (strncmp(fstyp, "auto", 4) == 0) )
            {
                continue;
            }

            if ((stat(path, &statbuf) == 0) && (target_dev == statbuf.st_dev))
            {
                const char * p = NULL;

                // NFS host:/path
                if (   (fsname[0] != '/')
                    && ((p = strchr(fsname, ':')) != NULL) && (p[1] == '/'))
                {
                    RETVAL = PyUnicode_FromString(fsname);
                }
                // NFS /path@host -> swap to "host:/path"
                else if (   (strncmp(fstyp, "nfs", 3) == 0)
                         && (fsname[0] == '/')
                         && ((p = strchr(fsname, '@')) != NULL)
                         && (strchr(p + 1, '/') == NULL) )
                {
                    RETVAL = PyUnicode_FromFormat("%s:%.*s", p + 1, p - fsname, fsname);
                }
                else
                {
                    // XFS, VxFS and AFS quotas require separate access methods
                    const char * prefix = "";
#if defined (SGI_XFS)
                    // (optional for VxFS: later versions use 'normal' quota interface)
                    if (strcmp(fstyp, "xfs") == 0)
                        prefix = "(XFS)";
#endif
#if defined (SOLARIS_VXFS)
                    if (strcmp(fstyp, "vxfs") == 0)
                        prefix = "(VXFS)";
#endif
#ifdef AFSQUOTA
                    if ((strcmp(fstyp, "afs") == 0) && (strcmp(fsname, "AFS") == 0))
                        prefix = "(AFS)";
#endif
#if defined(HAVE_JFS2)
                    if (strcmp(fstyp, "jfs2") == 0)
                        prefix = "(JFS2)";
#endif

#if defined(USE_IOCTL) || defined(QCARG_MNTPT)
                    // return mount point
                    RETVAL = PyUnicode_FromFormat("%s%s", prefix, path);
#elif defined(HAVE_JFS2) || defined(AIX) || defined(OSF_QUOTA)
                    // return path of any file in the file system
                    RETVAL = PyUnicode_FromFormat("%s%s", prefix, target_path);
#elif defined (Q_CTL_V2)
                    // return path of "quotas" file directly under fs root path
                    RETVAL = PyUnicode_FromFormat("%s%s/quotas", prefix, path);
#else
                    // return device path
                    // check for special case: Linux mount -o loop
                    if (   ((p = strstr(fsopt, "loop=/dev/")) != NULL)
                        && ((p == fsopt) || (*(p - 1) == ',')))
                    {
                        const char * pe = strchr(p, ',');
                        if (pe != NULL)
                            RETVAL = PyUnicode_FromFormat("%s%.*s", prefix, pe - p, p);
                        else
                            RETVAL = PyUnicode_FromFormat("%s%s", prefix, p);
                    }
                    else
                    {
                        RETVAL = PyUnicode_FromFormat("%s%s", prefix, fsname);
                    }
#endif
                }
                // getmntent loop done
                break;
            }
        }
        my_endmntent(&l_mntab);
        errno = 0;
    }
    return RETVAL;
}

// ----------------------------------------------------------------------------

//
//  Translate error codes of quotactl syscall and ioctl
//

static PyObject *
FsQuota_strerr(PyObject *self, PyObject *args)  // METH_NOARGS
{
    const char * str;

    if ((errno == EINVAL) || (errno == ENOTTY) || (errno == ENOENT) || (errno == ENOSYS))
        str = "No quotas on this system";
    else if (errno == ENODEV)
        str = "Not a standard file system";
    else if (errno == EPERM)
        str = "Not privileged";
    else if (errno == EACCES)
        str = "Access denied";
    else if (errno == ESRCH)
        str = "No quota for this user";
    else if (errno == EUSERS)
        str = "Quota table overflow";
    else
        str = strerror(errno);

    return PyUnicode_FromString(str);
}

// ----------------------------------------------------------------------------

static PyMethodDef FsQuota_Methods[] =
{
    {"setmntent", FsQuota_setmntent, METH_NOARGS,  "Init mount table iteration"},
    {"getmntent", FsQuota_getmntent, METH_NOARGS,  "Get next entry from mount table"},
    {"endmntent", FsQuota_endmntent, METH_NOARGS,  "End mount table iteration"},

    {"getqcarg",  FsQuota_getqcarg,  METH_VARARGS, "Get device parameter for quota op"},
    {"strerr",    FsQuota_strerr,    METH_NOARGS,  "Get error string for failed quota op"},

    {"query",     FsQuota_query,     METH_VARARGS, "Query quota limits for a given user/group"},
    {"setqlim",   FsQuota_setqlim,   METH_VARARGS, "Set quota limits for a given user/group"},
    {"sync",      FsQuota_sync,      METH_VARARGS, "Sync quota changes to disk"},

    {"rpcquery",  FsQuota_rpcquery,  METH_VARARGS, "Query quota limits via RPC"},
    {"rpcauth",   FsQuota_rpcauth,   METH_VARARGS, "Set authentication parameters for RPC"},
    {"rpcpeer",   FsQuota_rpcpeer,   METH_VARARGS, "Set networking parameters for RPC"},

    {NULL, NULL, 0, NULL}       // Sentinel
};

static struct PyModuleDef FsQuota_module =
{
    PyModuleDef_HEAD_INIT,
    "FsQuota",                  // name of module
    NULL,                       // module documentation, may be NULL
    -1,                         // size of per-interpreter state of the module
                                //  or -1 if the module keeps state in global variables.
    FsQuota_Methods
};

#if !defined (NAMED_TUPLE_GC_BUG)
// TODO same for quota query & setqlim

static PyStructSequence_Field FsQuota_MntEntType_Members[] =
{
    { "mnt_fsname", NULL },
    { "mnt_dir", NULL },
    { "mnt_type", NULL },
    { "mnt_opts", NULL },
    { NULL, NULL }
};
static PyStructSequence_Desc FsQuota_MntEntType_Desc =
{
    "mntent",
    "Mount table entry",
    FsQuota_MntEntType_Members,
    4
};
#endif

PyMODINIT_FUNC
PyInit_FsQuota(void)
{
    PyObject *m;

    m = PyModule_Create(&FsQuota_module);
    if (m == NULL)
        return NULL;

#if !defined (NAMED_TUPLE_GC_BUG)
     FsQuota_MntEntType = PyStructSequence_NewType(&FsQuota_MntEntType_Desc);
     if (FsQuota_MntEntType == NULL)
         ; //TODO
#endif

#if 0
    // TODO replace RETVAL & errno
    static PyObject * FsQuotaError;
    FsQuotaError = PyErr_NewException("FsQuota.error", NULL, NULL);
    Py_XINCREF(FsQuotaError);
    if (PyModule_AddObject(m, "error", FsQuotaError) < 0) {
        Py_XDECREF(FsQuotaError);
        Py_CLEAR(FsQuotaError);
        Py_DECREF(m);
        return NULL;
    }
#endif

    return m;
}
