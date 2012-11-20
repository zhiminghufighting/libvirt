/*
 * Copyright (C) 2007-2012 Red Hat, Inc.
 * Copyright (C) 2010 Satoru SATOH <satoru.satoh@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Based on iptables.c
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#ifdef HAVE_PATHS_H
# include <paths.h>
#endif

#include "internal.h"
#include "datatypes.h"
#include "bitmap.h"
#include "dnsmasq.h"
#include "util.h"
#include "command.h"
#include "memory.h"
#include "virterror_internal.h"
#include "logging.h"
#include "virfile.h"

#define VIR_FROM_THIS VIR_FROM_NETWORK
#define networkReportError(code, ...)                                   \
    virReportErrorHelper(VIR_FROM_NETWORK, code, __FILE__,              \
                         __FUNCTION__, __LINE__, __VA_ARGS__)

#define DNSMASQ_HOSTSFILE_SUFFIX "hostsfile"
#define DNSMASQ_ADDNHOSTSFILE_SUFFIX "addnhosts"

static void
dhcphostFree(dnsmasqDhcpHost *host)
{
    VIR_FREE(host->host);
}

static void
addnhostFree(dnsmasqAddnHost *host)
{
    int i;

    for (i = 0; i < host->nhostnames; i++)
        VIR_FREE(host->hostnames[i]);
    VIR_FREE(host->hostnames);
    VIR_FREE(host->ip);
}

static void
addnhostsFree(dnsmasqAddnHostsfile *addnhostsfile)
{
    unsigned int i;

    if (addnhostsfile->hosts) {
        for (i = 0; i < addnhostsfile->nhosts; i++)
            addnhostFree(&addnhostsfile->hosts[i]);

        VIR_FREE(addnhostsfile->hosts);

        addnhostsfile->nhosts = 0;
    }

    VIR_FREE(addnhostsfile->path);

    VIR_FREE(addnhostsfile);
}

static int
addnhostsAdd(dnsmasqAddnHostsfile *addnhostsfile,
             virSocketAddr *ip,
             const char *name)
{
    char *ipstr = NULL;
    int idx = -1;
    int i;

    if (!(ipstr = virSocketAddrFormat(ip)))
        return -1;

    for (i = 0; i < addnhostsfile->nhosts; i++) {
        if (STREQ((const char *)addnhostsfile->hosts[i].ip, (const char *)ipstr)) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        if (VIR_REALLOC_N(addnhostsfile->hosts, addnhostsfile->nhosts + 1) < 0)
            goto alloc_error;

        idx = addnhostsfile->nhosts;
        if (VIR_ALLOC(addnhostsfile->hosts[idx].hostnames) < 0)
            goto alloc_error;

        if (virAsprintf(&addnhostsfile->hosts[idx].ip, "%s", ipstr) < 0)
            goto alloc_error;

        addnhostsfile->hosts[idx].nhostnames = 0;
        addnhostsfile->nhosts++;
    }

    if (VIR_REALLOC_N(addnhostsfile->hosts[idx].hostnames, addnhostsfile->hosts[idx].nhostnames + 1) < 0)
        goto alloc_error;

    if (virAsprintf(&addnhostsfile->hosts[idx].hostnames[addnhostsfile->hosts[idx].nhostnames], "%s", name) < 0)
        goto alloc_error;

    VIR_FREE(ipstr);

    addnhostsfile->hosts[idx].nhostnames++;

    return 0;

 alloc_error:
    virReportOOMError();
    VIR_FREE(ipstr);
    return -1;
}

static dnsmasqAddnHostsfile *
addnhostsNew(const char *name,
             const char *config_dir)
{
    dnsmasqAddnHostsfile *addnhostsfile;

    if (VIR_ALLOC(addnhostsfile) < 0) {
        virReportOOMError();
        return NULL;
    }

    addnhostsfile->hosts = NULL;
    addnhostsfile->nhosts = 0;

    if (virAsprintf(&addnhostsfile->path, "%s/%s.%s", config_dir, name,
                    DNSMASQ_ADDNHOSTSFILE_SUFFIX) < 0) {
        virReportOOMError();
        goto error;
    }

    return addnhostsfile;

 error:
    addnhostsFree(addnhostsfile);
    return NULL;
}

static int
addnhostsWrite(const char *path,
               dnsmasqAddnHost *hosts,
               unsigned int nhosts)
{
    char *tmp;
    FILE *f;
    bool istmp = true;
    unsigned int i, ii;
    int rc = 0;

    if (nhosts == 0)
        return rc;

    if (virAsprintf(&tmp, "%s.new", path) < 0)
        return -ENOMEM;

    if (!(f = fopen(tmp, "w"))) {
        istmp = false;
        if (!(f = fopen(path, "w"))) {
            rc = -errno;
            goto cleanup;
        }
    }

    for (i = 0; i < nhosts; i++) {
        if (fputs(hosts[i].ip, f) == EOF || fputc('\t', f) == EOF) {
            rc = -errno;
            VIR_FORCE_FCLOSE(f);

            if (istmp)
                unlink(tmp);

            goto cleanup;
        }

        for (ii = 0; ii < hosts[i].nhostnames; ii++) {
            if (fputs(hosts[i].hostnames[ii], f) == EOF || fputc('\t', f) == EOF) {
                rc = -errno;
                VIR_FORCE_FCLOSE(f);

                if (istmp)
                    unlink(tmp);

                goto cleanup;
            }
        }

        if (fputc('\n', f) == EOF) {
            rc = -errno;
            VIR_FORCE_FCLOSE(f);

            if (istmp)
                unlink(tmp);

            goto cleanup;
        }
    }

    if (VIR_FCLOSE(f) == EOF) {
        rc = -errno;
        goto cleanup;
    }

    if (istmp && rename(tmp, path) < 0) {
        rc = -errno;
        unlink(tmp);
        goto cleanup;
    }

 cleanup:
    VIR_FREE(tmp);

    return rc;
}

static int
addnhostsSave(dnsmasqAddnHostsfile *addnhostsfile)
{
    int err = addnhostsWrite(addnhostsfile->path, addnhostsfile->hosts,
                             addnhostsfile->nhosts);

    if (err < 0) {
        virReportSystemError(-err, _("cannot write config file '%s'"),
                             addnhostsfile->path);
        return -1;
    }

    return 0;
}

static int
genericFileDelete(char *path)
{
    if (!virFileExists(path))
        return 0;

    if (unlink(path) < 0) {
        virReportSystemError(errno, _("cannot remove config file '%s'"),
                             path);
        return -1;
    }

    return 0;
}

static void
hostsfileFree(dnsmasqHostsfile *hostsfile)
{
    unsigned int i;

    if (hostsfile->hosts) {
        for (i = 0; i < hostsfile->nhosts; i++)
            dhcphostFree(&hostsfile->hosts[i]);

        VIR_FREE(hostsfile->hosts);

        hostsfile->nhosts = 0;
    }

    VIR_FREE(hostsfile->path);

    VIR_FREE(hostsfile);
}

static int
hostsfileAdd(dnsmasqHostsfile *hostsfile,
             const char *mac,
             virSocketAddr *ip,
             const char *name)
{
    char *ipstr = NULL;
    if (VIR_REALLOC_N(hostsfile->hosts, hostsfile->nhosts + 1) < 0)
        goto alloc_error;

    if (!(ipstr = virSocketAddrFormat(ip)))
        return -1;

    if (name) {
        if (virAsprintf(&hostsfile->hosts[hostsfile->nhosts].host, "%s,%s,%s",
                        mac, ipstr, name) < 0) {
            goto alloc_error;
        }
    } else {
        if (virAsprintf(&hostsfile->hosts[hostsfile->nhosts].host, "%s,%s",
                        mac, ipstr) < 0) {
            goto alloc_error;
        }
    }
    VIR_FREE(ipstr);

    hostsfile->nhosts++;

    return 0;

 alloc_error:
    virReportOOMError();
    VIR_FREE(ipstr);
    return -1;
}

static dnsmasqHostsfile *
hostsfileNew(const char *name,
             const char *config_dir)
{
    dnsmasqHostsfile *hostsfile;

    if (VIR_ALLOC(hostsfile) < 0) {
        virReportOOMError();
        return NULL;
    }

    hostsfile->hosts = NULL;
    hostsfile->nhosts = 0;

    if (virAsprintf(&hostsfile->path, "%s/%s.%s", config_dir, name,
                    DNSMASQ_HOSTSFILE_SUFFIX) < 0) {
        virReportOOMError();
        goto error;
    }

    return hostsfile;

 error:
    hostsfileFree(hostsfile);
    return NULL;
}

static int
hostsfileWrite(const char *path,
               dnsmasqDhcpHost *hosts,
               unsigned int nhosts)
{
    char *tmp;
    FILE *f;
    bool istmp = true;
    unsigned int i;
    int rc = 0;

    if (nhosts == 0)
        return rc;

    if (virAsprintf(&tmp, "%s.new", path) < 0)
        return -ENOMEM;

    if (!(f = fopen(tmp, "w"))) {
        istmp = false;
        if (!(f = fopen(path, "w"))) {
            rc = -errno;
            goto cleanup;
        }
    }

    for (i = 0; i < nhosts; i++) {
        if (fputs(hosts[i].host, f) == EOF || fputc('\n', f) == EOF) {
            rc = -errno;
            VIR_FORCE_FCLOSE(f);

            if (istmp)
                unlink(tmp);

            goto cleanup;
        }
    }

    if (VIR_FCLOSE(f) == EOF) {
        rc = -errno;
        goto cleanup;
    }

    if (istmp && rename(tmp, path) < 0) {
        rc = -errno;
        unlink(tmp);
        goto cleanup;
    }

 cleanup:
    VIR_FREE(tmp);

    return rc;
}

static int
hostsfileSave(dnsmasqHostsfile *hostsfile)
{
    int err = hostsfileWrite(hostsfile->path, hostsfile->hosts,
                             hostsfile->nhosts);

    if (err < 0) {
        virReportSystemError(-err, _("cannot write config file '%s'"),
                             hostsfile->path);
        return -1;
    }

    return 0;
}

/**
 * dnsmasqContextNew:
 *
 * Create a new Dnsmasq context
 *
 * Returns a pointer to the new structure or NULL in case of error
 */
dnsmasqContext *
dnsmasqContextNew(const char *network_name,
                  const char *config_dir)
{
    dnsmasqContext *ctx;

    if (VIR_ALLOC(ctx) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (!(ctx->config_dir = strdup(config_dir))) {
        virReportOOMError();
        goto error;
    }

    if (!(ctx->hostsfile = hostsfileNew(network_name, config_dir)))
        goto error;
    if (!(ctx->addnhostsfile = addnhostsNew(network_name, config_dir)))
        goto error;

    return ctx;

 error:
    dnsmasqContextFree(ctx);
    return NULL;
}

/**
 * dnsmasqContextFree:
 * @ctx: pointer to the dnsmasq context
 *
 * Free the resources associated with a dnsmasq context
 */
void
dnsmasqContextFree(dnsmasqContext *ctx)
{
    if (!ctx)
        return;

    VIR_FREE(ctx->config_dir);

    if (ctx->hostsfile)
        hostsfileFree(ctx->hostsfile);
    if (ctx->addnhostsfile)
        addnhostsFree(ctx->addnhostsfile);

    VIR_FREE(ctx);
}

/**
 * dnsmasqAddDhcpHost:
 * @ctx: pointer to the dnsmasq context for each network
 * @mac: pointer to the string contains mac address of the host
 * @ip: pointer to the socket address contains ip of the host
 * @name: pointer to the string contains hostname of the host or NULL
 *
 * Add dhcp-host entry.
 */
int
dnsmasqAddDhcpHost(dnsmasqContext *ctx,
                   const char *mac,
                   virSocketAddr *ip,
                   const char *name)
{
    return hostsfileAdd(ctx->hostsfile, mac, ip, name);
}

/*
 * dnsmasqAddHost:
 * @ctx: pointer to the dnsmasq context for each network
 * @ip: pointer to the socket address contains ip of the host
 * @name: pointer to the string contains hostname of the host
 *
 * Add additional host entry.
 */

int
dnsmasqAddHost(dnsmasqContext *ctx,
               virSocketAddr *ip,
               const char *name)
{
    return addnhostsAdd(ctx->addnhostsfile, ip, name);
}

/**
 * dnsmasqSave:
 * @ctx: pointer to the dnsmasq context for each network
 *
 * Saves all the configurations associated with a context to disk.
 */
int
dnsmasqSave(const dnsmasqContext *ctx)
{
    int ret = 0;

    if (virFileMakePath(ctx->config_dir) < 0) {
        virReportSystemError(errno, _("cannot create config directory '%s'"),
                             ctx->config_dir);
        return -1;
    }

    if (ctx->hostsfile)
        ret = hostsfileSave(ctx->hostsfile);
    if (ret == 0) {
        if (ctx->addnhostsfile)
            ret = addnhostsSave(ctx->addnhostsfile);
    }

    return ret;
}


/**
 * dnsmasqDelete:
 * @ctx: pointer to the dnsmasq context for each network
 *
 * Delete all the configuration files associated with a context.
 */
int
dnsmasqDelete(const dnsmasqContext *ctx)
{
    int ret = 0;

    if (ctx->hostsfile)
        ret = genericFileDelete(ctx->hostsfile->path);
    if (ctx->addnhostsfile)
        ret = genericFileDelete(ctx->addnhostsfile->path);

    return ret;
}

/**
 * dnsmasqReload:
 * @pid: the pid of the target dnsmasq process
 *
 * Reloads all the configurations associated to a context
 */
int
dnsmasqReload(pid_t pid ATTRIBUTE_UNUSED)
{
#ifndef WIN32
    if (kill(pid, SIGHUP) != 0) {
        virReportSystemError(errno,
            _("Failed to make dnsmasq (PID: %d) reload config files."),
            pid);
        return -1;
    }
#endif /* WIN32 */

    return 0;
}

/*
 * dnsmasqCapabilities functions - provide useful information about the
 * version of dnsmasq on this machine.
 *
 */
struct _dnsmasqCaps {
    char *binaryPath;
    bool noRefresh;
    time_t mtime;
    virBitmapPtr flags;
    unsigned long version;
};

void
dnsmasqCapsFree(dnsmasqCapsPtr caps)
{
    if (!caps)
        return;
    virBitmapFree(caps->flags);
    VIR_FREE(caps->binaryPath);
}

static void
dnsmasqCapsSet(dnsmasqCapsPtr caps,
               dnsmasqCapsFlags flag)
{
    ignore_value(virBitmapSetBit(caps->flags, flag));
}


#define DNSMASQ_VERSION_STR "Dnsmasq version "

static int
dnsmasqCapsSetFromBuffer(dnsmasqCapsPtr caps, const char *buf)
{
    const char *p;

    caps->noRefresh = true;

    p = STRSKIP(buf, DNSMASQ_VERSION_STR);
    if (!p)
       goto fail;
    virSkipSpaces(&p);
    if (virParseVersionString(p, &caps->version, true) < 0)
        goto fail;

    if (strstr(buf, "--bind-dynamic"))
        dnsmasqCapsSet(caps, DNSMASQ_CAPS_BIND_DYNAMIC);

    VIR_INFO("dnsmasq version is %d.%d, --bind-dynamic is %s",
             (int)caps->version / 1000000, (int)(caps->version % 1000000) / 1000,
             dnsmasqCapsGet(caps, DNSMASQ_CAPS_BIND_DYNAMIC)
             ? "present" : "NOT present");
    return 0;

fail:
    p = strchrnul(buf, '\n');
    networkReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot parse %s version number in '%.*s'"),
                       caps->binaryPath, (int) (p - buf), buf);
    return -1;

}

static int
dnsmasqCapsSetFromFile(dnsmasqCapsPtr caps, const char *path)
{
    int ret = -1;
    char *buf = NULL;

    if (virFileReadAll(path, 1024 * 1024, &buf) < 0)
        goto cleanup;

    ret = dnsmasqCapsSetFromBuffer(caps, buf);

cleanup:
    VIR_FREE(buf);
    return ret;
}

static int
dnsmasqCapsRefreshInternal(dnsmasqCapsPtr caps, bool force)
{
    int ret = -1;
    struct stat sb;
    virCommandPtr cmd = NULL;
    char *help = NULL, *version = NULL, *complete = NULL;

    if (!caps || caps->noRefresh)
        return 0;

    if (stat(caps->binaryPath, &sb) < 0) {
        virReportSystemError(errno, _("Cannot check dnsmasq binary %s"),
                             caps->binaryPath);
        return -1;
    }
    if (!force && caps->mtime == sb.st_mtime) {
        return 0;
    }
    caps->mtime = sb.st_mtime;

    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so it's hard to feed back a useful error.
     */
    if (!virFileIsExecutable(caps->binaryPath)) {
        virReportSystemError(errno, _("dnsmasq binary %s is not executable"),
                             caps->binaryPath);
        goto cleanup;
    }

    cmd = virCommandNewArgList(caps->binaryPath, "--version", NULL);
    virCommandSetOutputBuffer(cmd, &version);
    virCommandSetErrorBuffer(cmd, &version);
    virCommandAddEnvPassCommon(cmd);
    virCommandClearCaps(cmd);
    if (virCommandRun(cmd, NULL) < 0) {
        virReportSystemError(errno, _("failed to run '%s --version': %s"),
                             caps->binaryPath, version);
        goto cleanup;
    }
    virCommandFree(cmd);

    cmd = virCommandNewArgList(caps->binaryPath, "--help", NULL);
    virCommandSetOutputBuffer(cmd, &help);
    virCommandSetErrorBuffer(cmd, &help);
    virCommandAddEnvPassCommon(cmd);
    virCommandClearCaps(cmd);
    if (virCommandRun(cmd, NULL) < 0) {
        virReportSystemError(errno, _("failed to run '%s --help': %s"),
                             caps->binaryPath, help);
        goto cleanup;
    }

    if (virAsprintf(&complete, "%s\n%s", version, help) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    ret = dnsmasqCapsSetFromBuffer(caps, complete);

cleanup:
    virCommandFree(cmd);
    VIR_FREE(help);
    VIR_FREE(version);
    VIR_FREE(complete);
    return ret;
}

static dnsmasqCapsPtr
dnsmasqCapsNewEmpty(const char *binaryPath)
{
    dnsmasqCapsPtr caps;

    if (VIR_ALLOC(caps) < 0)
        return NULL;
    if (!(caps->flags = virBitmapAlloc(DNSMASQ_CAPS_LAST)))
        goto error;
    if (!(caps->binaryPath = strdup(binaryPath ? binaryPath : DNSMASQ)))
        goto error;
    return caps;

error:
    virReportOOMError();
    dnsmasqCapsFree(caps);
    return NULL;
}

dnsmasqCapsPtr
dnsmasqCapsNewFromBuffer(const char *buf, const char *binaryPath)
{
    dnsmasqCapsPtr caps = dnsmasqCapsNewEmpty(binaryPath);

    if (!caps)
        return NULL;

    if (dnsmasqCapsSetFromBuffer(caps, buf) < 0) {
        dnsmasqCapsFree(caps);
        return NULL;
    }
    return caps;
}

dnsmasqCapsPtr
dnsmasqCapsNewFromFile(const char *dataPath, const char *binaryPath)
{
    dnsmasqCapsPtr caps = dnsmasqCapsNewEmpty(binaryPath);

    if (!caps)
        return NULL;

    if (dnsmasqCapsSetFromFile(caps, dataPath) < 0) {
        dnsmasqCapsFree(caps);
        return NULL;
    }
    return caps;
}

dnsmasqCapsPtr
dnsmasqCapsNewFromBinary(const char *binaryPath)
{
    dnsmasqCapsPtr caps = dnsmasqCapsNewEmpty(binaryPath);

    if (!caps)
        return NULL;

    if (dnsmasqCapsRefreshInternal(caps, true) < 0) {
        dnsmasqCapsFree(caps);
        return NULL;
    }
    return caps;
}

/** dnsmasqCapsRefresh:
 *
 *   Refresh an existing caps object if the binary has changed. If
 *   there isn't yet a caps object (if it's NULL), create a new one.
 *
 *   Returns 0 on success, -1 on failure
 */
int
dnsmasqCapsRefresh(dnsmasqCapsPtr *caps, const char *binaryPath)
{
    if (!*caps) {
        *caps = dnsmasqCapsNewFromBinary(binaryPath);
        return *caps ? 0 : -1;
    }
    return dnsmasqCapsRefreshInternal(*caps, false);
}

const char *
dnsmasqCapsGetBinaryPath(dnsmasqCapsPtr caps)
{
    return caps ? caps->binaryPath : DNSMASQ;
}

unsigned long
dnsmasqCapsGetVersion(dnsmasqCapsPtr caps)
{
    if (caps)
        return caps->version;
    else
        return 0;
}

bool
dnsmasqCapsGet(dnsmasqCapsPtr caps, dnsmasqCapsFlags flag)
{
    bool b;

    if (!caps || virBitmapGetBit(caps->flags, flag, &b) < 0)
        return false;
    else
        return b;
}
