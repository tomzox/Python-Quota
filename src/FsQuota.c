// ------------------------------------------------------------------------
// Copyright (C) 1995-2020 T. Zoerner
// ------------------------------------------------------------------------
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by the
// Free Software Foundation.  (Either version 2 of the GPL, or any later
// version, see http://www.opensource.org/licenses/).
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// ------------------------------------------------------------------------

#define PY_SSIZE_T_CLEAN
#include "Python.h"

#include "myconfig.h"

#ifdef AFSQUOTA
#include "include/afsquota.h"
#endif

#ifdef SOLARIS_VXFS
#include "include/vxquotactl.h"
#endif

#if defined (NAMED_TUPLE_GC_BUG)
static PyTypeObject FsQuota_QuotaQueryTypeBuf;
static PyTypeObject FsQuota_MntTabTypeBuf;
static PyTypeObject * const FsQuota_QuotaQueryType = &FsQuota_QuotaQueryTypeBuf;
static PyTypeObject * const FsQuota_MntTabType = &FsQuota_MntTabTypeBuf;
#else
static PyTypeObject FsQuota_QuotaQueryType = NULL;
static PyTypeObject * FsQuota_MntTabType = NULL;
#endif

static PyObject * FsQuotaError;

//
// This data structure encapsulates the entire internal state of an ongoing
// mount table iteration. Member variables are specific to the implementation
// for different platforms.
//
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
} T_MY_MNTENT_STATE;


#ifndef NO_RPC
// ----------------------------------------------------------------------------
//
// This data structure contains configurable options for RPC
//
typedef struct
{
    unsigned        use_tcp;
    unsigned        port;
    unsigned        timeout;
    int             auth_uid;
    int             auth_gid;
    char            auth_hostname[MAX_MACHINE_NAME + 1];
} T_QUOTA_RPC_OPT;

#define RPC_DEFAULT_TIMEOUT     4000
#define RPC_AUTH_UGID_NON_INIT  -1

//
// This data structure defines the implementation-independent container used
// internally for returning results from quota queries via RPC sub-functions.
//
typedef struct
{
    uint64_t bhard;
    uint64_t bsoft;
    uint64_t bcur;
    time_t   btime;
    uint64_t fhard;
    uint64_t fsoft;
    uint64_t fcur;
    time_t   ftime;
} T_QUOTA_RPC_RESULT;

//
// Execute RPC to remote host
//

static int
callaurpc(char *host, int prognum, int versnum, int procnum,
          xdrproc_t inproc, char *in, xdrproc_t outproc, char *out,
          const T_QUOTA_RPC_OPT * opt, char ** p_errstr)
{
    struct sockaddr_in remaddr;
    struct hostent *hp;
    enum clnt_stat clnt_stat;
    struct timeval rep_time, timeout;
    CLIENT *client;
    int socket = RPC_ANYSOCK;

    //
    //  Get IP address; by default the port is determined via remote
    //  portmap daemon; different ports and protocols can be configured
    //
    hp = gethostbyname(host);
    if (hp == NULL)
    {
        *p_errstr = clnt_sperrno(RPC_UNKNOWNHOST);
        return -1;
    }

    rep_time.tv_sec = opt->timeout / 1000;
    rep_time.tv_usec = (opt->timeout % 1000) * 1000;
    memcpy((char *)&remaddr.sin_addr, (char *)hp->h_addr, hp->h_length);
    remaddr.sin_family = AF_INET;
    remaddr.sin_port = htons(opt->port);

    //
    //  Create client RPC handle
    //
    client = NULL;
    if (!opt->use_tcp)
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
    {
        if (rpc_createerr.cf_stat != RPC_SUCCESS)
            *p_errstr = clnt_sperrno(rpc_createerr.cf_stat);
        else  // should never happen (may be due to inconsistent symbol resolution)
            *p_errstr = "RPC creation failed for unknown reasons";
        return -1;
    }

    //
    //  Create an authentication handle
    //
    if ((opt->auth_uid >= 0) && (opt->auth_gid >= 0))
    {
        client->cl_auth = authunix_create((char*)opt->auth_hostname, // cast to remove const
                                          opt->auth_uid, opt->auth_gid, 0, 0);
    }
    else
    {
        client->cl_auth = authunix_create_default();
    }

    //
    //  Call remote server
    //
    timeout.tv_sec = opt->timeout / 1000;
    timeout.tv_usec = (opt->timeout % 1000) * 1000;
    clnt_stat = clnt_call(client, procnum,
                          inproc, in, outproc, out, timeout);

    if (client->cl_auth)
    {
        auth_destroy(client->cl_auth);
        client->cl_auth = NULL;
    }
    clnt_destroy(client);

    if (clnt_stat != RPC_SUCCESS)
    {
        *p_errstr = clnt_sperrno(clnt_stat);
        return -1;
    }
    else
    {
        *p_errstr = NULL;
        return 0;
    }
}

//
// Fetch quota limits for NFS mount via RPC
//

static int
getnfsquota( char *hostp, char *fsnamep, int uid, int is_grpquota,
             const T_QUOTA_RPC_OPT * opt, char ** rpc_err_str,
             T_QUOTA_RPC_RESULT *rslt )
{
    struct getquota_args gq_args;
    struct getquota_rslt gq_rslt;
#ifdef USE_EXT_RQUOTA
    ext_getquota_args ext_gq_args;

    //
    // First try USE_EXT_RQUOTAPROG (Extended quota RPC)
    //
    ext_gq_args.gqa_pathp = fsnamep;
    ext_gq_args.gqa_id = uid;
    ext_gq_args.gqa_type = (is_grpquota ? GQA_TYPE_GRP : GQA_TYPE_USR);

    if (callaurpc(hostp, RQUOTAPROG, EXT_RQUOTAVERS, RQUOTAPROC_GETQUOTA,
                  xdr_ext_getquota_args, &ext_gq_args,
                  xdr_getquota_rslt, &gq_rslt,
                  opt, rpc_err_str) != 0)
#endif
    {
        //
        // Fall back to RQUOTAPROG if the server (or client via compile switch)
        // don't support extended quota RPC
        //
        gq_args.gqa_pathp = fsnamep;
        gq_args.gqa_uid = uid;

        if (callaurpc(hostp, RQUOTAPROG, RQUOTAVERS, RQUOTAPROC_GETQUOTA,
                      (xdrproc_t)xdr_getquota_args, (char*) &gq_args,
                      (xdrproc_t)xdr_getquota_rslt, (char*) &gq_rslt,
                      opt, rpc_err_str) != 0)
        {
            return -1;
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
        // Since Linux reports a bogus block size value (4k), we must not
        // use it. Thankfully Linux at least always uses 1k block sizes
        // for quota reports, so we just leave away all conversions.
        // If you have a mixed environment, you have a problem though.
        // Complain to the Linux authors or apply my patch (see INSTALL)
        //
        rslt->bhard = gq_rslt.GQR_RQUOTA.rq_bhardlimit;
        rslt->bsoft = gq_rslt.GQR_RQUOTA.rq_bsoftlimit;
        rslt->bcur = gq_rslt.GQR_RQUOTA.rq_curblocks;
#else /* not buggy */
        if (gq_rslt.GQR_RQUOTA.rq_bsize >= DEV_QBSIZE)
        {
            // assign first, multiply later:
            // so that mult works with the possibly larger type in rslt
            rslt->bhard = gq_rslt.GQR_RQUOTA.rq_bhardlimit;
            rslt->bsoft = gq_rslt.GQR_RQUOTA.rq_bsoftlimit;
            rslt->bcur = gq_rslt.GQR_RQUOTA.rq_curblocks;

            // we rely on the fact that block sizes are always powers of 2
            // so the conversion factor will never be a fraction
            qb_fac = gq_rslt.GQR_RQUOTA.rq_bsize / DEV_QBSIZE;
            rslt->bhard *= qb_fac;
            rslt->bsoft *= qb_fac;
            rslt->bcur *= qb_fac;
        }
        else
        {
            if (gq_rslt.GQR_RQUOTA.rq_bsize != 0)
                qb_fac = DEV_QBSIZE / gq_rslt.GQR_RQUOTA.rq_bsize;
            else
                qb_fac = 1;
            rslt->bhard = gq_rslt.GQR_RQUOTA.rq_bhardlimit / qb_fac;
            rslt->bsoft = gq_rslt.GQR_RQUOTA.rq_bsoftlimit / qb_fac;
            rslt->bcur = gq_rslt.GQR_RQUOTA.rq_curblocks / qb_fac;
        }
#endif /* LINUX_RQUOTAD_BUG */
        rslt->fhard = gq_rslt.GQR_RQUOTA.rq_fhardlimit;
        rslt->fsoft = gq_rslt.GQR_RQUOTA.rq_fsoftlimit;
        rslt->fcur = gq_rslt.GQR_RQUOTA.rq_curfiles;

        // if time is given relative to actual time, add actual time
        // Note: all systems except Linux return relative times
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

        return 0;
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
//
// Transport encoding for quota RPC, in case not provided by system libraries
//

static const struct xdr_discrim gq_des[2] =
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

// ----------------------------------------------------------------------------
//  Class "Quota"
// ----------------------------------------------------------------------------

//
// This enumeration is used to identify file systems that require special
// handling in the query() and setqlim() methods. File systems for which the
// default handling of the respective platform can be used are marked
// "regular".
//
typedef enum
{
    QUOTA_DEV_INVALID,
    QUOTA_DEV_REGULAR,
    QUOTA_DEV_NFS,
    QUOTA_DEV_XFS,
    QUOTA_DEV_VXFS,
    QUOTA_DEV_AFS,
    QUOTA_DEV_JFS2,
} T_QUOTA_DEV_FS_TYPE;

//
// Container for instance state variables
//
typedef struct
{
    PyObject_HEAD
    char * m_path;                      // path parameter passed to the constructor
    char * m_qcarg;                     // device parameter derived from path
    char * m_rpc_host;                  // rpc_host parameter passed to the constructor
    T_QUOTA_DEV_FS_TYPE m_dev_fs_type;  // file system types that need special handling
#ifndef NO_RPC
    T_QUOTA_RPC_OPT m_rpc_opt;          // container for parameters set via rpc_opt()
#endif
} Quota_ObjectType;

// forward declaration
static int Quota_setqcarg(Quota_ObjectType *self);

//
// Helper function for raising an exception upon quotactl() error
//
static void *
FsQuota_QuotaCtlException(Quota_ObjectType * self, int errnum, const char * str)
{
    if (str == NULL)
    {
        if ((errnum == ENOENT) && (self->m_dev_fs_type == QUOTA_DEV_XFS))
            str = "No quota for this user";
        else if ((errnum == EINVAL) || (errnum == ENOTTY) ||
                 (errnum == ENOENT) || (errnum == ENOSYS))
            str = "No quotas on this system";
        else if (errnum == ENODEV)
            str = "Not a standard file system";
        else if (errnum == EPERM)
            str = "Not privileged";
        else if (errnum == EACCES)
            str = "Access denied";
        else if (errnum == ESRCH)
#ifdef Q_CTL_V3  /* Linux */
            str = "Quotas not enabled, no quota for this user";
#else
            str = "No quota for this user";
#endif
        else if (errnum == EUSERS)
            str = "Quota table overflow";
        else
            str = strerror(errnum);
    }

    PyObject * tuple = PyTuple_New(2);
    PyTuple_SetItem(tuple, 0, PyLong_FromLong(errnum));
    PyTuple_SetItem(tuple, 1, PyUnicode_DecodeFSDefault(str));

    PyErr_SetObject(FsQuotaError, tuple);

    // for convenience: to be assiged to caller's RETVAL
    return NULL;
}

//
// Helper function for raising an exception upon errors returned by C library
// functions. Meaning of parameters is equivalent to that of exception base
// class "OSError".
//
static void *
FsQuota_OsException(int errnum, const char * desc, const char * path)
{
    PyObject * strerr = PyUnicode_DecodeFSDefault(strerror(errnum));

    PyObject * tuple = PyTuple_New((path == NULL) ? 2 : 3);
    PyTuple_SetItem(tuple, 0, PyLong_FromLong(errnum));
    PyTuple_SetItem(tuple, 1, PyUnicode_FromFormat("%s: %U", desc, strerr));
    if (path != NULL)
        PyTuple_SetItem(tuple, 2, PyUnicode_DecodeFSDefault(path));

    PyErr_SetObject(FsQuotaError, tuple);
    Py_DECREF(strerr);

    // for convenience: to be assiged to caller's RETVAL
    return NULL;
}

//
// Helper function for allocating and filling a query result tuple with given
// values.
//
static PyObject *
FsQuota_BuildQuotaResult(uint64_t bc, uint64_t bs, uint64_t bh, uint32_t bt,
                         uint64_t ic, uint64_t is, uint64_t ih, uint32_t it)
{
    PyObject * RETVAL;

    RETVAL = PyStructSequence_New(FsQuota_QuotaQueryType);

    PyStructSequence_SetItem(RETVAL, 0, PyLong_FromLongLong(bc));
    PyStructSequence_SetItem(RETVAL, 1, PyLong_FromLongLong(bs));
    PyStructSequence_SetItem(RETVAL, 2, PyLong_FromLongLong(bh));
    PyStructSequence_SetItem(RETVAL, 3, PyLong_FromLong    (bt));

    PyStructSequence_SetItem(RETVAL, 4, PyLong_FromLongLong(ic));
    PyStructSequence_SetItem(RETVAL, 5, PyLong_FromLongLong(is));
    PyStructSequence_SetItem(RETVAL, 6, PyLong_FromLongLong(ih));
    PyStructSequence_SetItem(RETVAL, 7, PyLong_FromLong    (it));

    return RETVAL;
}

//
// Implementation of the Quota.query() method
//
PyDoc_STRVAR(Quota_query__doc__,
"query(uid, *, grpquota=False, projquota=False) -> FsQuota.QueryResult\n\n"
"Query quota usage and limits for the given user.\n\n"
"When either grpquota or projquota is set to True, the query returns "
"group or project quotas instead of user quotas. Only one of these "
"options should be True. Project quotas are supported only by XFS "
"file systems.");

static PyObject *
Quota_query(Quota_ObjectType *self, PyObject *args, PyObject *kwds)
{
    int     uid = getuid();
    int     is_grpquota = FALSE;
    int     is_prjquota = FALSE;

    static char * kwlist[] = {"uid", "grpquota", "prjquota", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|$pp", kwlist,
                                     &uid, &is_grpquota, &is_prjquota))
    {
        return NULL;
    }

    PyObject * RETVAL = NULL;
    int err;

    if (self->m_dev_fs_type == QUOTA_DEV_INVALID)
    {
        RETVAL = FsQuota_QuotaCtlException(self, EINVAL, "FsQuota.Quota instance is uninitialized");
    }
    else if (is_prjquota && (self->m_dev_fs_type != QUOTA_DEV_XFS))
    {
        RETVAL = FsQuota_QuotaCtlException(self, ENOTSUP, "Project quotas are only supported by XFS");
    }
    else
#ifdef SGI_XFS
    if (self->m_dev_fs_type == QUOTA_DEV_XFS)
    {
        fs_disk_quota_t xfs_dqblk;
#ifndef linux
        err = quotactl(Q_XGETQUOTA, self->m_qcarg, uid, CADR &xfs_dqblk);
#else
        err = quotactl(QCMD(Q_XGETQUOTA, (is_prjquota ? XQM_PRJQUOTA :
                                          is_grpquota ? XQM_GRPQUOTA : XQM_USRQUOTA)),
                       self->m_qcarg, uid, CADR &xfs_dqblk);
#endif
        if (!err)
        {
            RETVAL = FsQuota_BuildQuotaResult(QX_DIV(xfs_dqblk.d_bcount),
                                              QX_DIV(xfs_dqblk.d_blk_softlimit),
                                              QX_DIV(xfs_dqblk.d_blk_hardlimit),
                                              xfs_dqblk.d_btimer,
                                              xfs_dqblk.d_icount,
                                              xfs_dqblk.d_ino_softlimit,
                                              xfs_dqblk.d_ino_hardlimit,
                                              xfs_dqblk.d_itimer);
        }
        else
        {
            FsQuota_QuotaCtlException(self, errno, NULL);
        }
    }
    else
#endif
#ifdef SOLARIS_VXFS
    if (self->m_dev_fs_type == QUOTA_DEV_VXFS)
    {
        struct vx_dqblk vxfs_dqb;
        err = vx_quotactl(VX_GETQUOTA, self->m_qcarg, uid, CADR &vxfs_dqb);
        if (!err)
        {
            RETVAL = FsQuota_BuildQuotaResult(Q_DIV(vxfs_dqb.dqb_curblocks),
                                              Q_DIV(vxfs_dqb.dqb_bsoftlimit),
                                              Q_DIV(vxfs_dqb.dqb_bhardlimit),
                                              vxfs_dqb.dqb_btimelimit,
                                              vxfs_dqb.dqb_curfiles,
                                              vxfs_dqb.dqb_fsoftlimit,
                                              vxfs_dqb.dqb_fhardlimit,
                                              vxfs_dqb.dqb_ftimelimit);
        }
        else
        {
            FsQuota_QuotaCtlException(self, errno, NULL);
        }
    }
    else
#endif
#ifdef AFSQUOTA
    if (self->m_dev_fs_type == QUOTA_DEV_AFS)
    {
        if (!afs_check())  // check is *required* as setup!
        {
            FsQuota_OsException(EINVAL, "AFS setup failed", NULL);
        }
        else
        {
            int maxQuota, blocksUsed;

            err = afs_getquota(self->m_qcarg, &maxQuota, &blocksUsed);
            if (!err)
            {
                RETVAL = FsQuota_BuildQuotaResult(blocksUsed,
                                                  maxQuota,
                                                  maxQuota,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  0);
            }
            else
            {
                FsQuota_QuotaCtlException(self, errno, NULL);
            }
        }
    }
    else
#endif
    {
        if (self->m_dev_fs_type == QUOTA_DEV_NFS)
        {
#ifndef NO_RPC
            T_QUOTA_RPC_RESULT rslt;
            char * rpc_err_str = NULL;
            err = getnfsquota(self->m_rpc_host, self->m_qcarg, uid, is_grpquota, &self->m_rpc_opt, &rpc_err_str, &rslt);
            if (!err)
            {
                RETVAL = FsQuota_BuildQuotaResult(Q_DIV(rslt.bcur),
                                                  Q_DIV(rslt.bsoft),
                                                  Q_DIV(rslt.bhard),
                                                  rslt.btime,
                                                  rslt.fcur,
                                                  rslt.fsoft,
                                                  rslt.fhard,
                                                  rslt.ftime);
            }
            else if (rpc_err_str != NULL)
            {
                FsQuota_QuotaCtlException(self, ECOMM, rpc_err_str);
            }
            else
            {
                FsQuota_QuotaCtlException(self, errno, NULL);
            }
#else /* NO_RPC */
            FsQuota_QuotaCtlException(self, ENOSYS, "RPC not supported for this platform");
            err = -1;
#endif /* NO_RPC */
        }
        else
        {
#ifdef NETBSD_LIBQUOTA
            struct quotahandle *qh = quota_open(self->m_qcarg);
            if (qh != NULL)
            {
                struct quotakey qk_blocks, qk_files;
                struct quotaval qv_blocks, qv_files;

                qk_blocks.qk_idtype = /*fall-through*/
                qk_files.qk_idtype = is_grpquota ? QUOTA_IDTYPE_GROUP : QUOTA_IDTYPE_USER;
                qk_blocks.qk_id = qk_files.qk_id = uid;
                qk_blocks.qk_objtype = QUOTA_OBJTYPE_BLOCKS;
                qk_files.qk_objtype = QUOTA_OBJTYPE_FILES;

                if ((quota_get(qh, &qk_blocks, &qv_blocks) >= 0) &&
                    (quota_get(qh, &qk_files, &qv_files) >= 0) )
                {
                    RETVAL = FsQuota_BuildQuotaResult(Q_DIV(qv_blocks.qv_usage),
                                                      Q_DIV(qv_blocks.qv_softlimit),
                                                      Q_DIV(qv_blocks.qv_hardlimit),
                                                      qv_blocks.qv_expiretime,
                                                      qv_files.qv_usage,
                                                      qv_files.qv_softlimit,
                                                      qv_files.qv_hardlimit,
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
            if ((fd = open(self->m_qcarg, O_RDONLY)) != -1)
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
            err = linuxquota_query(self->m_qcarg, uid, is_grpquota, &dqblk);
#else /* not Q_CTL_V3 */
#ifdef Q_CTL_V2
#ifdef AIX
            // AIX quotactl doesn't fail if path does not exist!?
            struct stat st;
#if defined(HAVE_JFS2)
            if (self->m_dev_fs_type == QUOTA_DEV_JFS2)
            {
                if (stat(self->m_qcarg, &st) == 0)
                {
                    quota64_t user_quota;

                    err = quotactl(self->m_qcarg, QCMD(Q_J2GETQUOTA, (is_grpquota ? GRPQUOTA : USRQUOTA)),
                                   uid, CADR &user_quota);
                    if (!err)
                    {
                        RETVAL = FsQuota_BuildQuotaResult(user_quota.bused,
                                                          user_quota.bsoft,
                                                          user_quota.bhard,
                                                          user_quota.btime,
                                                          user_quota.ihard,
                                                          user_quota.isoft,
                                                          user_quota.iused,
                                                          user_quota.itime);
                    }
                }
                else
                    err = 1;
            }
#endif /* HAVE_JFS2 */
            else if (stat(self->m_qcarg, &st) != 0)
            {
                err = 1;
            }
            else
#endif /* AIX */
            err = quotactl(self->m_qcarg, QCMD(Q_GETQUOTA, (is_grpquota ? GRPQUOTA : USRQUOTA)), uid, CADR &dqblk);
#else /* not Q_CTL_V2 */
            err = quotactl(Q_GETQUOTA, self->m_qcarg, uid, CADR &dqblk);
#endif /* not Q_CTL_V2 */
#endif /* Q_CTL_V3 */
#endif /* not USE_IOCTL */
            if (!err && (RETVAL == NULL))
            {
                RETVAL = FsQuota_BuildQuotaResult(Q_DIV(dqblk.QS_BCUR),
                                                  Q_DIV(dqblk.QS_BSOFT),
                                                  Q_DIV(dqblk.QS_BHARD),
                                                  dqblk.QS_BTIME,
                                                  dqblk.QS_FCUR,
                                                  dqblk.QS_FSOFT,
                                                  dqblk.QS_FHARD,
                                                  dqblk.QS_FTIME);
            }
            else if (err)
            {
                FsQuota_QuotaCtlException(self, errno, NULL);
            }
#endif /* not NETBSD_LIBQUOTA */
        }
    }
    return RETVAL;
}

//
// Implementation of the Quota.seqlim() method
//
PyDoc_STRVAR(Quota_setqlim__doc__,
"setqlim(uid, bsoft, bhard, isoft, ihard, *, grpquota=False, projquota=False)\n\n"
"Set the given block and inode quota limits for the given user\n\n"
"When either grpquota or projquota is set to True, the query returns "
"group or project quotas instead of user quotas. Only one of these "
"options should be True. Project quotas are supported only by XFS "
"file systems.\n\n"
"Limit parameters may also be specified in form of keyword parameters "
"using the names given in the signature above. Omitted values default "
"to zero.");

static PyObject *
Quota_setqlim(Quota_ObjectType *self, PyObject *args, PyObject *kwds)
{
    int     uid = -1;
    unsigned long long  bs = 0;
    unsigned long long  bh = 0;
    unsigned long long  fs = 0;
    unsigned long long  fh = 0;
    int     timelimflag = 0;
    int     is_grpquota = FALSE;
    int     is_prjquota = FALSE;

    static char * kwlist[] = {"uid", "bsoft", "bhard", "isoft", "ihard",
                              "timelimit_reset", "grpquota", "prjquota", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|KKKK$ppp", kwlist,
                                     &uid, &bs, &bh, &fs, &fh,
                                     &timelimflag, &is_grpquota, &is_prjquota))
    {
        return NULL;
    }

    PyObject * RETVAL = Py_None;

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

    if (self->m_dev_fs_type == QUOTA_DEV_INVALID)
    {
        RETVAL = FsQuota_QuotaCtlException(self, EINVAL, "FsQuota.Quota instance is uninitialized");
    }
    else if (is_prjquota && (self->m_dev_fs_type != QUOTA_DEV_XFS))
    {
        RETVAL = FsQuota_QuotaCtlException(self, ENOTSUP, "Project quotas are only supported by XFS");
    }
    else
#ifdef SGI_XFS
    if (self->m_dev_fs_type == QUOTA_DEV_XFS)
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
        int err = quotactl(Q_XSETQLIM, self->m_qcarg, uid, CADR &xfs_dqblk);
#else
        int err = quotactl(QCMD(Q_XSETQLIM, (is_prjquota ? XQM_PRJQUOTA : (is_grpquota ? XQM_GRPQUOTA : XQM_USRQUOTA))), self->m_qcarg, uid, CADR &xfs_dqblk);
#endif
        if (err)
        {
            RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
        }
    }
    else
    // if not xfs, than it's a classic IRIX efs file system
#endif
#ifdef SOLARIS_VXFS
    if (self->m_dev_fs_type == QUOTA_DEV_VXFS)
    {
        struct vx_dqblk vxfs_dqb;

        vxfs_dqb.dqb_bsoftlimit = Q_MUL(bs);
        vxfs_dqb.dqb_bhardlimit = Q_MUL(bh);
        vxfs_dqb.dqb_btimelimit = timelimflag;
        vxfs_dqb.dqb_fsoftlimit = fs;
        vxfs_dqb.dqb_fhardlimit = fh;
        vxfs_dqb.dqb_ftimelimit = timelimflag;
        int err = vx_quotactl(VX_SETQUOTA, self->m_qcarg, uid, CADR &vxfs_dqb);
        if (err)
        {
            RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
        }
    }
    else
#endif
#ifdef AFSQUOTA
    if (self->m_dev_fs_type == QUOTA_DEV_AFS)
    {
        if (!afs_check())  // check is *required* as setup!
        {
            RETVAL = FsQuota_QuotaCtlException(self, EINVAL, "AFS setup via afc_check failed");
        }
        else
        {
            int err = afs_setqlim(self->m_qcarg, bh);
            if (err)
            {
                RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
            }
        }
    }
    else
#endif
#if defined(HAVE_JFS2)
    if (self->m_dev_fs_type == QUOTA_DEV_JFS2)
    {
        quota64_t user_quota;

        int err = quotactl(self->m_qcarg, QCMD(Q_J2GETQUOTA, (is_grpquota ? GRPQUOTA : USRQUOTA)),
                           uid, CADR &user_quota);
        if (err == 0)
        {
            user_quota.bsoft = bs;
            user_quota.bhard = bh;
            user_quota.btime = timelimflag;
            user_quota.isoft = fs;
            user_quota.ihard = fh;
            user_quota.itime = timelimflag;
            err = quotactl(self->m_qcarg, QCMD(Q_J2PUTQUOTA, (is_grpquota ? GRPQUOTA : USRQUOTA)),
                           uid, CADR &user_quota);
        }
        if (err)
        {
            RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
        }
    }
    else
#endif /* HAVE_JFS2 */
    {
#ifdef NETBSD_LIBQUOTA
        struct quotahandle *qh;
        struct quotakey qk;
        struct quotaval qv;

        qh = quota_open(self->m_qcarg);
        if (qh != NULL)
        {
            qk.qk_idtype = is_grpquota ? QUOTA_IDTYPE_GROUP : QUOTA_IDTYPE_USER;
            qk.qk_id = uid;

            qk.qk_objtype = QUOTA_OBJTYPE_BLOCKS;

            // set the grace period for blocks
            if (timelimflag)  // seven days
            {
                qv.qv_grace = 7*24*60*60;
            }
            else if (quota_get(qh, &qk, &qv) >= 0)  // use user's current setting
            {
                // OK
            }
            else if (qk.qk_id = QUOTA_DEFAULTID, quota_get(qh, &qk, &qv) >= 0)  // use default setting
            {
                // OK, reset qk_id
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

                // set the grace period for files, see comments above
                if (timelimflag)
                {
                    qv.qv_grace = 7*24*60*60;
                }
                else if (quota_get(qh, &qk, &qv) >= 0)
                {
                    // OK
                }
                else if (qk.qk_id = QUOTA_DEFAULTID, quota_get(qh, &qk, &qv) >= 0)
                {
                    // OK, reset qk_id
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

                if (quota_put(qh, &qk, &qv) < 0)
                {
                    RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
                }
            }
            else
            {
                RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
            }
            quota_close(qh);
        }
        else
        {
            RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
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
        if ((fd = open(self->m_qcarg, O_RDONLY)) != -1)
        {
            if (ioctl(fd, Q_QUOTACTL, &qp) != 0)
            {
                RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
            }
            close(fd);
        }
        else
        {
            RETVAL = FsQuota_OsException(errno, "opening device", self->m_qcarg);
        }
#else
#ifdef Q_CTL_V3  /* Linux */
        int err = linuxquota_setqlim (self->m_qcarg, uid, is_grpquota, &dqblk);
#else
#ifdef Q_CTL_V2
        int err = quotactl (self->m_qcarg, QCMD(Q_SETQUOTA,(is_grpquota ? GRPQUOTA : USRQUOTA)), uid, CADR &dqblk);
#else
        int err = quotactl (Q_SETQLIM, self->m_qcarg, uid, CADR &dqblk);
#endif /* Q_CTL_V2 */
#endif /* Q_CTL_V3 */
        if (err)
        {
            RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
        }
#endif /* USE_IOCTL */
#endif /* not NETBSD_LIBQUOTA */
    }

    return RETVAL;
}

//
// Implementation of the Quota.sync() method
//
PyDoc_STRVAR(Quota_sync__doc__,
"quota()\n\n"
"Sync quota changes to disk.");

static PyObject *
Quota_sync(Quota_ObjectType *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ""))
    {
        return NULL;
    }
    PyObject * RETVAL = Py_None;

    if (self->m_dev_fs_type == QUOTA_DEV_INVALID)
    {
        RETVAL = FsQuota_QuotaCtlException(self, EINVAL, "FsQuota.Quota instance is uninitialized");
    }
    else
#ifdef SOLARIS_VXFS
    if (self->m_dev_fs_type == QUOTA_DEV_VXFS)
    {
        if (vx_quotactl(VX_QSYNCALL, self->m_qcarg, 0, NULL) != 0)
        {
            RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
        }
    }
    else
#endif
#ifdef AFSQUOTA
    if (self->m_dev_fs_type == QUOTA_DEV_AFS)
    {
        if (!afs_check())
        {
            RETVAL = FsQuota_QuotaCtlException(self, EINVAL, "AFS setup via afc_check failed");
        }
        else
        {
            int foo1, foo2;
            if (afs_getquota(self->m_qcarg, &foo1, &foo2) != 0)
            {
                RETVAL = FsQuota_QuotaCtlException(self, EINVAL, NULL);
            }
        }
    }
    else
#endif
#ifdef NETBSD_LIBQUOTA
    // NOP / not supported
#else /* !NETBSD_LIBQUOTA */
#ifdef USE_IOCTL
    {
        struct quotactl qp;
        int fd;

        qp.op = Q_SYNC;

        if ((fd = open(self->m_qcarg, O_RDONLY)) != -1)
        {
            if (ioctl(fd, Q_QUOTACTL, &qp) != 0)
            {
                if (errno == ESRCH)
                {
                    RETVAL = FsQuota_QuotaCtlException(self, EINVAL, NULL);
                }
                else
                {
                    RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
                }
            }
            close(fd);
        }
        else
        {
            RETVAL = FsQuota_OsException(errno, "opening device", self->m_qcarg);
        }
    }
#else /* !USE_IOCTL */
    {
#ifdef Q_CTL_V3  /* Linux */
#ifdef SGI_XFS
        if (self->m_dev_fs_type == QUOTA_DEV_XFS)
        {
            if (quotactl(QCMD(Q_XQUOTASYNC, XQM_USRQUOTA), self->m_qcarg, 0, NULL) != 0)
            {
                RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
            }
        }
        else
#endif /* SGI_XFS */
        if (linuxquota_sync(self->m_qcarg, FALSE) != 0)
        {
            RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
        }
#else /* !Q_CTL_V3 */
#ifdef Q_CTL_V2
#ifdef AIX
        struct stat st;
#endif
#ifdef AIX
        if (stat(self->m_qcarg, &st))
        {
            RETVAL = FsQuota_OsException(errno, "accessing device", self->m_qcarg);
        }
        else
#endif /* AIX */
        if (quotactl(self->m_qcarg, QCMD(Q_SYNC, USRQUOTA), 0, NULL) != 0)
        {
            RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
        }
#else /* !Q_CTL_V2 */
#ifdef SGI_XFS
#define XFS_UQUOTA (XFS_QUOTA_UDQ_ACCT|XFS_QUOTA_UDQ_ENFD)
        // Q_SYNC is not supported on XFS filesystems, so emulate it
        if (self->m_dev_fs_type == QUOTA_DEV_XFS)
        {
            fs_quota_stat_t fsq_stat;

            sync();

            if (quotactl(Q_GETQSTAT, self->m_qcarg, 0, CADR &fsq_stat) != 0)
            {
                if ((fsq_stat.qs_flags & XFS_UQUOTA) != XFS_UQUOTA)
                {
                    RETVAL = FsQuota_QuotaCtlException(self, ENOENT, NULL);
                }
                else
                {
                    RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
                }
            }
        }
        else
#endif /* SGI_XFS */
        if (quotactl(Q_SYNC, self->m_qcarg, 0, NULL) != 0)
        {
            RETVAL = FsQuota_QuotaCtlException(self, errno, NULL);
        }
#endif /* !Q_CTL_V2 */
#endif /* !Q_CTL_V3 */
    }
#endif /* !USE_IOCTL */
#endif /* NETBSD_LIBQUOTA */

    return RETVAL;
}

//
// Implementation of the Quota.rpc_opt() method
//
PyDoc_STRVAR(Quota_rpc_opt__doc__,
"rpc_opt(*)\n\n"
"Set networking and authentication parameters for RPC\n"
"Please refer to the documentation for a list of options.");

static PyObject *
Quota_rpc_opt(Quota_ObjectType *self, PyObject *args, PyObject *kwds)
{
    PyObject * RETVAL = Py_None;
#ifndef NO_RPC
    static char * kwlist[] = {"rpc_port", "rpc_use_tcp", "rpc_timeout",
                              "auth_uid", "auth_gid", "auth_hostname", NULL};
    char * p_hostname = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|$IpIiis", kwlist,
                                     &self->m_rpc_opt.port,
                                     &self->m_rpc_opt.use_tcp,
                                     &self->m_rpc_opt.timeout,
                                     &self->m_rpc_opt.auth_uid,
                                     &self->m_rpc_opt.auth_gid,
                                     &p_hostname
                                    ))
    {
        return NULL;
    }

    if ((self->m_rpc_opt.auth_uid < 0) && (self->m_rpc_opt.auth_gid >= 0))
    {
        self->m_rpc_opt.auth_uid = getuid();
    }

    if ((self->m_rpc_opt.auth_gid < 0) && (self->m_rpc_opt.auth_uid >= 0))
    {
        self->m_rpc_opt.auth_gid = getgid();
    }

    if ((p_hostname != NULL) && (*p_hostname != 0))
    {
        if (strlen(p_hostname) < MAX_MACHINE_NAME)
        {
            strcpy(self->m_rpc_opt.auth_hostname, p_hostname);
        }
        else
        {
            RETVAL = FsQuota_OsException(ENAMETOOLONG, "hostname is too long", p_hostname);
        }
    }
    else if (self->m_rpc_opt.auth_uid >= 0)
    {
        if (gethostname(self->m_rpc_opt.auth_hostname, MAX_MACHINE_NAME) != 0)
        {
            RETVAL = FsQuota_OsException(errno, "gethostname", NULL);
        }
    }
#endif

    return RETVAL;
}

//
// Allocate a new "Quota" object and initialize the C state struct
//
static PyObject *
Quota_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    Quota_ObjectType *self;
    self = (Quota_ObjectType *) type->tp_alloc(type, 0);

#ifndef NO_RPC
    self->m_rpc_opt.timeout = RPC_DEFAULT_TIMEOUT;
    self->m_rpc_opt.auth_uid = RPC_AUTH_UGID_NON_INIT;
    self->m_rpc_opt.auth_gid = RPC_AUTH_UGID_NON_INIT;
#endif

    return (PyObject *) self;
}

//
// De-allocate a "Quota" object and internal resources
//
static void
Quota_dealloc(Quota_ObjectType *self)
{
    if (self->m_path != NULL)
    {
        free(self->m_path);
    }
    if (self->m_qcarg != NULL)
    {
        free(self->m_qcarg);
    }
    if (self->m_rpc_host != NULL)
    {
        free(self->m_rpc_host);
    }

    Py_TYPE(self)->tp_free((PyObject *) self);
}

//
// Implementation of the standard "__init__" function: Usually called after
// "new"; may be called again to re-initialize the object.
//
static int
Quota_init(Quota_ObjectType *self, PyObject *args, PyObject *kwds)
{
    static char * kwlist[] = {"path", "rpc_host", NULL};
    char * p_path = NULL;
    char * p_rpc_host = NULL;

    // reset state in case the module is already initialized
    if (self->m_path != NULL)
    {
        free(self->m_path);
        self->m_path = NULL;
    }
    if (self->m_qcarg != NULL)
    {
        free(self->m_qcarg);
        self->m_qcarg = NULL;
    }
    if (self->m_rpc_host != NULL)
    {
        free(self->m_rpc_host);
        self->m_rpc_host = NULL;
    }
    self->m_dev_fs_type = QUOTA_DEV_INVALID;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|s", kwlist,
                                     &p_path, &p_rpc_host))
    {
        return -1;
    }

    if (p_rpc_host != NULL)
    {
        self->m_rpc_host = strdup(p_rpc_host);
        self->m_qcarg = strdup(p_path);
        self->m_path = strdup("n/a");
        self->m_dev_fs_type = QUOTA_DEV_NFS;
    }
    else
    {
        // dup is needed as the string buffer lives only as long as object passed as parameter
        self->m_path = strdup(p_path);

        if (Quota_setqcarg(self) != 0)
        {
            return -1;
        }
    }

    return 0;
}

//
// Implementation of the standard "repr" function: Returns a "string
// representation" of the object. Should include all parameters.
//
static PyObject *
Quota_Repr(Quota_ObjectType *self)
{
    if (self->m_dev_fs_type == QUOTA_DEV_INVALID)
    {
        return PyUnicode_FromString("<FsQuota.Quota()>");
    }
#ifndef NO_RPC
    else if (self->m_dev_fs_type == QUOTA_DEV_NFS)
    {
        return PyUnicode_FromFormat("<FsQuota.Quota(%s), qcarg=%s:%s, special:NFS, "
                                    "use_tcp:%d, port:%d, timeout:%d, "
                                    "auth_uid:%d, auth_gid:%d, auth_hostname:%s>",
                                    self->m_path, self->m_rpc_host, self->m_qcarg,
                                    self->m_rpc_opt.port,
                                    self->m_rpc_opt.use_tcp,
                                    self->m_rpc_opt.timeout,
                                    self->m_rpc_opt.auth_uid,
                                    self->m_rpc_opt.auth_gid,
                                    self->m_rpc_opt.auth_hostname
                                    );
    }
#endif
    else
    {
        const char * typn;
        switch (self->m_dev_fs_type)
        {
            case QUOTA_DEV_NFS:  typn = "NFS"; break;  // never reached, handled above
            case QUOTA_DEV_XFS:  typn = "XFS"; break;
            case QUOTA_DEV_VXFS: typn = "VXFS"; break;
            case QUOTA_DEV_AFS:  typn = "AFS"; break;
            case QUOTA_DEV_JFS2: typn = "JFS2"; break;
            default:             typn = "no"; break;
        }
        return PyUnicode_FromFormat("<FsQuota.Quota(%s), qcarg=%s, special:%s>",
                                    self->m_path, self->m_qcarg, typn);
    }
}

//
// Implementation of the standard "__getattr__" function: This function adds
// "virtual" attributes "dev" and "is_nfs" on top of the regular attributes
// returned by the generic handler of this interface.
//
static PyObject *
Quota_GetAttr(Quota_ObjectType *self, PyObject * attr)
{
    PyObject * RETVAL = PyObject_GenericGetAttr((PyObject*)self, attr);
    if (RETVAL == NULL)
    {
        if (PyUnicode_Check(attr) == 1)
        {
            if (strcmp("dev", PyUnicode_AsUTF8(attr)) == 0)
            {
                PyErr_Clear();
                if (self->m_rpc_host != NULL)
                    RETVAL = PyUnicode_FromFormat("%s:%s", self->m_rpc_host, self->m_qcarg);
                else if (self->m_qcarg != NULL)
                    RETVAL = PyUnicode_FromString(self->m_qcarg);
            }
            else if (strcmp("is_nfs", PyUnicode_AsUTF8(attr)) == 0)
            {
                PyErr_Clear();
                RETVAL = PyLong_FromLong(self->m_dev_fs_type == QUOTA_DEV_NFS);
            }
        }
    }
    return RETVAL;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static PyMethodDef Quota_MethodsDef[] =
{
    {"query",     (PyCFunction) Quota_query,     METH_VARARGS | METH_KEYWORDS, Quota_query__doc__ },
    {"setqlim",   (PyCFunction) Quota_setqlim,   METH_VARARGS | METH_KEYWORDS, Quota_setqlim__doc__ },
    {"sync",      (PyCFunction) Quota_sync,      METH_VARARGS,                 Quota_sync__doc__ },
    {"rpc_opt",   (PyCFunction) Quota_rpc_opt,   METH_VARARGS | METH_KEYWORDS, Quota_rpc_opt__doc__ },
    {NULL}  /* Sentinel */
};

static PyTypeObject QuotaTypeDef =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "FsQuota.Quota",
    .tp_doc = PyDoc_STR("Class providing access to file-system quota"),
    .tp_basicsize = sizeof(Quota_ObjectType),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = Quota_new,
    .tp_init = (initproc) Quota_init,
    .tp_dealloc = (destructor) Quota_dealloc,
    .tp_repr = (PyObject * (*)(PyObject*)) Quota_Repr,
    .tp_getattro = (PyObject * (*)(PyObject*, PyObject*))Quota_GetAttr,
    .tp_methods = Quota_MethodsDef,
    //.tp_members = Quota_Members,
};

static PyStructSequence_Field QuotaQueryType_Members[] =
{
    { "bcount", PyDoc_STR("Number of blocks currently used") },
    { "bsoft",  PyDoc_STR("Soft limit for block count (or 0 if none)") },
    { "bhard",  PyDoc_STR("Hard limit for block count (or 0 if none)") },
    { "btime",  PyDoc_STR("Time when an exceeded soft block limit turns into "
                          "a hard limit (or n/a when not exceeded)") },
    { "icount", PyDoc_STR("Number of inodes (i.e. files) currently used") },
    { "isoft",  PyDoc_STR("Soft limit for inode count (or 0 if none)") },
    { "ihard",  PyDoc_STR("Hard limit for inode count (or 0 if none)") },
    { "itime",  PyDoc_STR("Time when an exceeded soft inode limit turns into "
                          "a hard limit (or n/a when not exceeded)") },
    { NULL, NULL }
};

static PyStructSequence_Desc QuotaQuery_Desc =
{
    "FsQuota.QueryResult",
    PyDoc_STR("Named tuple type returned by Quota.query(), containing quota usage and limits"),
    QuotaQueryType_Members,
    8
};


// ----------------------------------------------------------------------------
// Sub-functions for iterating across the mount table

typedef struct
{
    const char * fsname;
    const char * path;
    const char * fstyp;
    const char * fsopt;
} T_MY_MNTENT_BUF;

//
// Portable setmntent(): This function must be called once at the start of
// iteration.
//
int
my_setmntent(T_MY_MNTENT_STATE * state)
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

    // if (state->mtab != NULL) free(state->mtab);
    state->mtab_size = getmntinfo(&state->mtab, MNT_NOWAIT);
    RETVAL = ((state->mtab_size <= 0) ? -1 : 0);
    state->mntp = state->mtab;
#endif
#else /* AIX */
    int count, space;

    if (state->mtab != NULL)
    {
        free(state->mtab);
    }

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
            else  // error, or size changed between calls
            {
                if (count == 0) errno = EINTR;
                RETVAL = -1;
            }
        }
        else
        {
            RETVAL = -1;
        }
    }
    else if (count < 0)
    {
        RETVAL = -1;
    }
    else  // should never happen
    {
        errno = ENOENT;
        RETVAL = -1;
    }
#endif
    return RETVAL;
}

//
// Portable getmntent(): This function fills the given buffers with string
// pointers describing the next mount table entry. Note the strings are
// located in static memory owned by the function and must not be freed by
// the caller; they are invalidated upon the next call.
//
int
my_getmntent(T_MY_MNTENT_STATE * state, T_MY_MNTENT_BUF * str_buf)
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
            str_buf->fsname = mntp->mnt_fsname;
            str_buf->path = mntp->mnt_dir;
            str_buf->fstyp = mntp->mnt_type;
            str_buf->fsopt = mntp->mnt_opts;
            RETVAL = 0;
        }
    }
    else
    {
        errno = EBADF;
    }

#else /* NO_OPEN_MNTTAB */
    struct mnttab mntp;
    if (state->mtab != NULL)
    {
        if (getmntent(state->mtab, &mntp) == 0)
        {
            str_buf->fsname = mntp->mnt_special;
            str_buf->path = mntp->mnt_mountp;
            str_buf->fstyp = mntp->mnt_ftype;
            str_buf->fsopt = mntp->mnt_mntopts;
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
        str_buf->fsname = state->mntp->f_mntfromname;
        str_buf->path = state->mntp->f_mntonname;
#ifdef OSF_QUOTA
        if (fstype != (char *) -1)
        {
            str_buf->fstyp = fstype;
        }
        else
#endif
        {
#ifdef __OpenBSD__
            /* OpenBSD struct statfs lacks the f_type member (starting with release 2.7) */
            str_buf->fstyp = "";
#else /* !__OpenBSD__ */
#ifdef HAVE_STATVFS
            str_buf->fstyp = state->mntp->f_fstypename;
#else
            str_buf->fstyp = state->mntp->f_type;
#endif /* HAVE_STATVFS */
#endif /* !__OpenBSD__ */
        }
#ifdef HAVE_STATVFS
        snprintf(state->flag_str_buf, sizeof(state->flag_str_buf), "%d", state->mntp->f_flag);
#else
        snprintf(state->flag_str_buf, sizeof(state->flag_str_buf), "%d", state->mntp->f_flags);
#endif
        str_buf->fsopt = state->flag_str_buf;
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
            str_buf->fsname = cp;
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
                str_buf->fsname = mp;
                free(mp);
            }
            else
            {
                cp = "?";
                str_buf->fsname = cp;
            }
        }
        cp = vmt2dataptr(vmp, VMT_STUB);
        str_buf->path = cp;

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
        str_buf->fstyp = cp;

        cp = vmt2dataptr(vmp, VMT_ARGS);
        str_buf->fsopt = cp;
        RETVAL = 0;
    }
#endif  /* AIX */
    return RETVAL;
}

//
// Portable endmntent(): This function end the iteration and frees resources
// (if needed). This function should be called once for each call to
// setmntent(). getmntent must not be called afterward.
//
void
my_endmntent(T_MY_MNTENT_STATE * state)
{
    if (state->mtab != NULL)
    {
#ifndef AIX
#ifndef NO_MNTENT
#ifndef NO_OPEN_MNTTAB
        endmntent(state->mtab);   // returns always 1 in SunOS
#else
        fclose (state->mtab);
#endif
        // else: if (state->mtab != NULL) free(state->mtab);
#endif
#else /* AIX */
        free(state->mtab);
#endif
        state->mtab = NULL;
    }
}

// ----------------------------------------------------------------------------
//   Class "MntTab"
// ----------------------------------------------------------------------------

//
// Container for instance state variables
//
typedef struct
{
    PyObject_HEAD
    T_MY_MNTENT_STATE mntent;   // transparent state used by my_getmntent()
    int iterIndex;              // index tracking calls of __next__()
} MntTab_ObjectType;

//
// Allocate a new object and initialize the iteration state.
// Raise an exception if setmntent reports an error.
//
static PyObject *
MntTab_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    MntTab_ObjectType *self;
    self = (MntTab_ObjectType *) type->tp_alloc(type, 0);

    if (my_setmntent(&self->mntent) != 0)
    {
        Py_DECREF(self);
        return FsQuota_OsException(errno, "setmntent", NULL);
    }
    self->iterIndex = 0;
    return (PyObject *) self;
}

//
// De-allocate an object and internal resources
//
static void
MntTab_dealloc(MntTab_ObjectType *self)
{
    my_endmntent(&self->mntent);

    Py_TYPE(self)->tp_free((PyObject *) self);
}

//
// Implementation of the standard "__init__" function: Usually called after
// "new": in this case the function does nothing as initialization is already
// done within "new"; may be called again to re-initialize the object: if
// __next__() was called at least once, iteration is re-initialized.
//
static int
MntTab_init(MntTab_ObjectType *self, PyObject *args, PyObject *kwds)
{
    char *kwlist[] = {NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "", kwlist))
    {
        return -1;
    }

    if (self->iterIndex != 0)
    {
        // re-initialize iterator
        if (my_setmntent(&self->mntent) != 0)
        {
            FsQuota_OsException(errno, "setmntent", NULL);
            return -1;
        }
        self->iterIndex = 0;
    }

    return 0;
}

//
// Implementation of the standard "repr" function: Returns a "string
// representation" of the object. Should include all parameters; in this
// case we can only show the iteration index.
//
static PyObject *
MntTab_Repr(MntTab_ObjectType *self)
{
    if (self->iterIndex >= 0)
    {
        return PyUnicode_FromFormat("<FsQuota.MntTab iterator at index %d>", self->iterIndex);
    }
    else
    {
        return PyUnicode_FromFormat("<FsQuota.MntTab iterator at EOL>");
    }
}

//
// Implementation of the standard "__iter__" function: Simply returns a
// reference to itself, as the object is already set up for iteration.
//
static PyObject *
MntTab_Iter(MntTab_ObjectType *self)
{
    Py_INCREF(self);
    return (PyObject *) self;
}

//
// Implementation of the standard "__next__" function: Call C getmntent() and
// return the resulting strings in form of a tuple, or raise an exception upon
// end of iteration.
//
static PyObject *
MntTab_IterNext(MntTab_ObjectType *self)
{
    T_MY_MNTENT_BUF str_buf;
    PyObject * RETVAL = NULL;

    if ((self->iterIndex >= 0) && my_getmntent(&self->mntent, &str_buf) == 0)
    {
        RETVAL = PyStructSequence_New(FsQuota_MntTabType);
        if (str_buf.fsname != NULL)
            PyStructSequence_SetItem(RETVAL, 0, PyUnicode_DecodeFSDefault(str_buf.fsname));
        if (str_buf.path != NULL)
            PyStructSequence_SetItem(RETVAL, 1, PyUnicode_DecodeFSDefault(str_buf.path));
        if (str_buf.fstyp != NULL)
            PyStructSequence_SetItem(RETVAL, 2, PyUnicode_DecodeFSDefault(str_buf.fstyp));
        if (str_buf.fsopt != NULL)
            PyStructSequence_SetItem(RETVAL, 3, PyUnicode_DecodeFSDefault(str_buf.fsopt));

        self->iterIndex += 1;
    }
    else
    {
        PyErr_SetNone(PyExc_StopIteration);
        self->iterIndex = -1;
    }
    return RETVAL;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static PyTypeObject MntTabTypeDef =
{
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "FsQuota.MntTab",
    .tp_doc = PyDoc_STR("Class providing iterator for the file-system mount table"),
    .tp_basicsize = sizeof(MntTab_ObjectType),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = MntTab_new,
    .tp_init = (initproc) MntTab_init,
    .tp_dealloc = (destructor) MntTab_dealloc,
    .tp_repr = (PyObject * (*)(PyObject*)) MntTab_Repr,
    .tp_iter = (PyObject * (*)(PyObject*)) MntTab_Iter,
    .tp_iternext = (PyObject * (*)(PyObject*)) MntTab_IterNext,
    //.tp_members = MntTab_Members,
    //.tp_methods = MntTab_MethodsDef,
};

static PyStructSequence_Field MntEntType_Members[] =
{
    { "mnt_fsname", PyDoc_STR("Name of the filesystem (e.g. device name)") },
    { "mnt_dir",    PyDoc_STR("Filesystem path prefix (aka mount point)") },
    { "mnt_type",   PyDoc_STR("Mount type (aka file system type)") },
    { "mnt_opts",   PyDoc_STR("Mount options, separated by colon.") },
    { NULL, NULL }
};

static PyStructSequence_Desc MntEntType_Desc =
{
    "FsQuota.MntEnt",
    PyDoc_STR("Mount table entry type, as returned by iterator of class FsQuota.MntTab"),
    MntEntType_Members,
    4
};

// ----------------------------------------------------------------------------
//
//  Determine "device" argument for the "Quota" class methods
//

static int
Quota_setqcarg(Quota_ObjectType *self)
{
    struct stat statbuf;

    // determine device ID at the given path for later comparison with mount points
    int target_dev;
    if (stat(self->m_path, &statbuf) != 0)
    {
        FsQuota_OsException(errno, "Failed to access path", self->m_path);
        return -1;
    }
    target_dev = statbuf.st_dev;

    T_MY_MNTENT_STATE l_mntab;
    memset(&l_mntab, 0, sizeof(l_mntab));
    if (my_setmntent(&l_mntab) != 0)
    {
        FsQuota_OsException(errno, "setmntent", NULL);
        return -1;
    }

    // loop to search the given path's entry in the mount table
    T_MY_MNTENT_BUF mntent;
    while (my_getmntent(&l_mntab, &mntent) == 0)
    {
        if ((strcmp(mntent.fstyp, "lofs") == 0) ||
            (strcmp(mntent.fstyp, "ignore") == 0) ||
            (strcmp(mntent.fstyp, "proc") == 0) ||
            (strcmp(mntent.fstyp, "rootfs") == 0) ||
            (strncmp(mntent.fstyp, "auto", 4) == 0) )
        {
            continue;
        }

        // compare device ID of mount point with that of target path
        if ((stat(mntent.path, &statbuf) == 0) && (target_dev == statbuf.st_dev))
        {
            const char * p = NULL;

            // NFS host:/path
            if ((mntent.fsname[0] != '/') &&
                ((p = strchr(mntent.fsname, ':')) != NULL) && (p[1] == '/'))
            {
                self->m_rpc_host = strdup(mntent.fsname);
                self->m_rpc_host[p - mntent.fsname] = 0;
                self->m_qcarg = strdup(p + 1);
                self->m_dev_fs_type = QUOTA_DEV_NFS;
            }
            // NFS /path@host -> swap to "host:/path"
            else if ((strncmp(mntent.fstyp, "nfs", 3) == 0) &&
                     (mntent.fsname[0] == '/') &&
                     ((p = strchr(mntent.fsname, '@')) != NULL) &&
                     (strchr(p + 1, '/') == NULL) )
            {
                self->m_qcarg = (char*) malloc(strlen(mntent.fsname) + 1 + 1);
                sprintf(self->m_qcarg, "%s:%.*s", p + 1, (int)(p - mntent.fsname), mntent.fsname);

                self->m_qcarg = strdup(mntent.fsname);
                self->m_qcarg[p - mntent.fsname] = 0;
                self->m_rpc_host = strdup(p + 1);
                self->m_dev_fs_type = QUOTA_DEV_NFS;
            }
            else  // local device
            {
                self->m_dev_fs_type = QUOTA_DEV_REGULAR;

                // XFS, VxFS and AFS quotas require separate access methods
#if defined (SGI_XFS)
                // (optional for VxFS: later versions use 'normal' quota interface)
                if (strcmp(mntent.fstyp, "xfs") == 0)
                    self->m_dev_fs_type = QUOTA_DEV_XFS;
#endif
#if defined (SOLARIS_VXFS)
                if (strcmp(mntent.fstyp, "vxfs") == 0)
                    self->m_dev_fs_type = QUOTA_DEV_VXFS;
#endif
#ifdef AFSQUOTA
                if ((strcmp(mntent.fstyp, "afs") == 0) && (strcmp(mntent.fsname, "AFS") == 0))
                    self->m_dev_fs_type = QUOTA_DEV_AFS;
#endif
#if defined(HAVE_JFS2)
                if (strcmp(mntent.fstyp, "jfs2") == 0)
                    self->m_dev_fs_type = QUOTA_DEV_JFS2;
#endif

#if defined(USE_IOCTL) || defined(QCARG_MNTPT)
                // use mount point
                self->m_qcarg = strdup(mntent.path);
#elif defined(HAVE_JFS2) || defined(AIX) || defined(OSF_QUOTA)
                // use path of any file in the file system
                self->m_qcarg = strdup(target_path);
#elif defined (Q_CTL_V2)
                // use path of "quotas" file directly under fs root path
                self->m_qcarg = (char *) malloc(strlen(mntent.path) + 7 + 1);
                strcpy(self->m_qcarg, mntent.path);
                strcat(self->m_qcarg, "/quotas");
#else
                // use device path
                // check for special case: Linux mount -o loop
                if (((p = strstr(mntent.fsopt, "loop=/dev/")) != NULL) &&
                    ((p == mntent.fsopt) || (*(p - 1) == ',')))
                {
                    const char * pe = strchr(p, ',');
                    if (pe != NULL)
                    {
                        self->m_qcarg = strdup(p);
                        self->m_qcarg[pe - p] = 0;
                    }
                    else
                    {
                        self->m_qcarg = strdup(p);
                    }
                }
                else
                {
                    self->m_qcarg = strdup(mntent.fsname);
                }
#endif
            }
            // getmntent loop done
            break;
        }
    }
    my_endmntent(&l_mntab);

    if (self->m_qcarg == NULL)
    {
        FsQuota_OsException(EINVAL, "Mount path not found or device unsupported", self->m_qcarg);
        self->m_dev_fs_type = QUOTA_DEV_INVALID;
        return -1;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Top-level definition of the module
// - the module only contains the two classes, but no methods directly

static struct PyModuleDef FsQuota_module =
{
    PyModuleDef_HEAD_INIT,
    .m_name = "FsQuota",
    .m_doc = PyDoc_STR("The FsQuota module provides the Quota and MntTab classes"),
    .m_size = -1,
    //.m_methods = FsQuota_Methods
};

PyMODINIT_FUNC
PyInit_FsQuota(void)
{
    if ((PyType_Ready(&QuotaTypeDef) < 0) ||
        (PyType_Ready(&MntTabTypeDef) < 0))
    {
        return NULL;
    }

    PyObject * module = PyModule_Create(&FsQuota_module);
    if (module == NULL)
    {
        return NULL;
    }

    // create exception class "FsQuota.error", derived from OSError
    FsQuotaError = PyErr_NewException("FsQuota.error", PyExc_OSError, NULL);
    Py_XINCREF(FsQuotaError);
    if (PyModule_AddObject(module, "error", FsQuotaError) < 0)
    {
        Py_XDECREF(FsQuotaError);
        Py_CLEAR(FsQuotaError);
        Py_DECREF(module);
        return NULL;
    }

    // create class "FsQuota.Quota"
    Py_INCREF(&QuotaTypeDef);
    if (PyModule_AddObject(module, "Quota", (PyObject *) &QuotaTypeDef) < 0)
    {
        Py_DECREF(&QuotaTypeDef);
        Py_XDECREF(FsQuotaError);
        Py_CLEAR(FsQuotaError);
        Py_DECREF(module);
        return NULL;
    }

    // create class "FsQuota.MntTab"
    Py_INCREF(&MntTabTypeDef);
    if (PyModule_AddObject(module, "MntTab", (PyObject *) &MntTabTypeDef) < 0)
    {
        Py_DECREF(&MntTabTypeDef);
        Py_DECREF(&QuotaTypeDef);
        Py_XDECREF(FsQuotaError);
        Py_CLEAR(FsQuotaError);
        Py_DECREF(module);
        return NULL;
    }

#if defined (NAMED_TUPLE_GC_BUG)
    if (PyStructSequence_InitType2(&FsQuota_QuotaQueryTypeBuf, &QuotaQuery_Desc) != 0)
#else
    FsQuota_QuotaQueryType = PyStructSequence_NewType(&QuotaQuery_Desc);
    if (FsQuota_QuotaQueryType == NULL)
#endif
    {
        Py_DECREF(&MntTabTypeDef);
        Py_XDECREF(FsQuotaError);
        Py_CLEAR(FsQuotaError);
        Py_DECREF(module);
        return NULL;
    }

#if defined (NAMED_TUPLE_GC_BUG)
    if (PyStructSequence_InitType2(&FsQuota_MntTabTypeBuf, &MntEntType_Desc) != 0)
#else
    FsQuota_MntTabType = PyStructSequence_NewType(&MntEntType_Desc);
    if (FsQuota_MntTabType == NULL)
#endif
    {
#if !defined (NAMED_TUPLE_GC_BUG)
        Py_DECREF(FsQuota_QuotaQueryType);
#endif
        Py_DECREF(&MntTabTypeDef);
        Py_XDECREF(FsQuotaError);
        Py_CLEAR(FsQuotaError);
        Py_DECREF(module);
        return NULL;
    }

    return module;
}
