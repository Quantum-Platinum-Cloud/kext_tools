/*
 * Copyright (c) 2006-2012 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * FILE: update_boot.c
 * AUTH: Soren Spies (sspies)
 * DATE: 8 June 2006
 * DESC: implement 'kextcache -u' (copying to Apple_Boot partitions)
 *
 */

#include <bless.h>
#include <miscfs/devfs/devfs.h>     // UID_ROOT, GID_WHEEL
#include <fcntl.h>
#include <hfs/hfs_mount.h>          // hfs_mount_args
#include <libgen.h>
#include <mach/mach_error.h>
#include <mach/mach_port.h>         // mach_port_allocate()
#include <servers/bootstrap.h>
#include <sysexits.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <IOKit/kext/kextmanager_types.h>
#include <IOKit/kext/OSKextPrivate.h>
#include <IOKit/kext/kextmanager_types.h>
#include <IOKit/kext/kextmanager_mig.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOPartitionScheme.h>
#include <MediaKit/GPTTypes.h>
#include <bootfiles.h>
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <DiskArbitration/DiskArbitrationPrivate.h>

#include "bootcaches.h"
#include "bootroot_internal.h"
#include "fork_program.h"
#include "safecalls.h"
#include "kext_tools_util.h"


/******************************************************************************
* File-Globals
******************************************************************************/
static mach_port_t sBRUptLock = MACH_PORT_NULL;
static uuid_t      s_vol_uuid;      // XX not threadsafe (10561671)
static mach_port_t sKextdPort = MACH_PORT_NULL;


/******************************************************************************
* Types
******************************************************************************/
enum bootReversions {
    nothingSerious = 0,
    noLabel,                // 1
    copyingOFBooter,        // 2
    copyingEFIBooter,       // 3
    copiedBooters,          // 4
    activatingOFBooter,     // 5
    activatingEFIBooter,    // 6
    activatedBooters,       // 7
};

enum blessIndices {
    kSystemFolderIdx = 0,
    kEFIBooterIdx = 1
    // sBLSetBootFinderInfo() preserves other values
};

const char * bootReversionsStrings[] = {
    NULL,           // unused
    "Label deleted",
    "Unlinking and copying BootX booter",
    "Unlinking and copying EFI booter",
    "Booters copied",
    "Activating BootX",
    "Activating EFI booter",
    "Booters activated"
};


// for Apple_Boot update
struct updatingVol {
    struct bootCaches *caches;          // parsed bootcaches.plist data
    char srcRoot[PATH_MAX];             // src for boot caches as char[]
    CFDataRef bpdata;                   // data for com.apple.Boot.plist
    CFDictionaryRef csfdeprops;         // CSFDE property cache data (!encr)
    char flatTarget[PATH_MAX];          // indy <helper>/<target>, min-RPS
    OSKextLogSpec warnLogSpec;          // flags for file access warnings
    OSKextLogSpec errLogSpec;           // flags for file access errors
    CFArrayRef boots;                   // BSD Names of Apple_Boot partitions
    DASessionRef dasession;             // handle to diskarb

    // default to false for the common kextcache -u case
    Boolean expectUpToDate;             // expecting things to be right (-U)
    Boolean earlyBoot;                  // detect early boot check
    Boolean doRPS, doMisc, doBooters;   // what needs updating
    Boolean doSanitize, cleanOnceDir;   // how to cleanse each helper
    Boolean skipHelperBless;            // support non-default BR..ToDir()
    Boolean helpersOptional;            // -Installer doesn't want errors

    // updated as each Apple_Boot is updated
    int bootIdx;                        // which helper are we updating
    enum bootReversions changestate;    // track changes to roll back
    char bsdname[DEVMAXPATHSIZE];       // bsdname of Apple_Boot
    DADiskRef curBoot;                  // and matching diskarb ref
    char curMount[MNAMELEN];            // path to current boot mountpt
    int curbootfd;                      // Sec: handle to curMount
    char dstdir[PATH_MAX];              // full path to main dest.
    char efidst[PATH_MAX], ofdst[PATH_MAX];
    Boolean onAPM;                      // tweak support based on pmap
    Boolean detectedRecovery;           // any com.apple.recovery.boot?
};


/******************************************************************************
* Definitions
******************************************************************************/
#define COMPILE_TIME_ASSERT(pred) switch(0){case 0:case pred:;}

// for non-RPS content, including booters
#define OLDEXT ".old"
#define NEWEXT ".new"
#define SCALE_2xEXT "_2x"
#define CONTENTEXT ".contentDetails"
#define kBRRootUUIDFile ".root_uuid"
#define kBRBootOnceDir "/com.apple.boot.once"

// NOTE: These strings must be the same length, or code in ucopyRPS will break!
// There is a compile time assert in the function to this effect.
#define BOOTPLIST_NAME "com.apple.Boot.plist"
#define BOOTPLIST_APM_NAME "com.apple.boot.plist"


/******************************************************************************
* Helpers
******************************************************************************/

// diskarb
static int mountBoot(struct updatingVol *up);
static void unmountBoot(struct updatingVol *up);

// ucopy = unlink & copy
// no race for RPS, so install it first
static int ucopyRPS(struct updatingVol *s);  // nuke/copy to inactive
// the label files (for example) have no fallback, .new is harmless
// XX ucopy"Preboot/Firmware"
static int ucopyMisc(struct updatingVol *s);        // use/overwrite .new names
// booters have fallback paths, but originals might be broken
static int ucopyBooters(struct updatingVol *s);     // nuke/copy booters (inact)
// no label -> hint of indeterminate state (label key in plist?)
static int moveLabels(struct updatingVol *s);       // move aside
static int nukeBRLabels(struct updatingVol *s);     // byebye (all?)
// booters have worst critical:fragile ratio (point of departure)
static int activateBooters(struct updatingVol *s);  // bless new names
// and the RPS data needed for booting
static int activateRPS(struct updatingVol *s);      // leap-frog w/rename()
// finally, the label (indicating a working system via this helper partition)
// XX activate"FirmwarePaths/postboot"
static int activateMisc(struct updatingVol *s);     // rename .new / label
// and now that we're safe
static int nukeFallbacks(struct updatingVol *s);
static int eraseRPS(struct updatingVol *up, char *toErase);

// cleanup routines (RPS is the last step; activateMisc handles label)
static int revertState(struct updatingVol *up);

/* Chain of Trust
 * Our goal is to do anything the bootcaches.plist says, but only to that vol.
 * #1 we only pay attention to root-owned bootcaches.plist files
 * #2 we get an fd to the bootcaches.plist              [trust is here]
// * #3 we validate the bc.plist fd after getting an fd to the volume's root
 * #4 we use stored bsdname for libbless
 * #5 we validate cachefd after the call to bless       [trust -> bsdname]
 * #6 we get curbootfd after each apple_boot mount
 * #7 we validate cachefd after the call                [trust -> curfd]
 * #8 operations take an fd limiting their scope to the mount
 */

// XX should probably rename to all-caps
// seed errno since strlxxx routines do not set it. This will make
// downstream error messages more meaningful (since we're often logging the
// errno value and message).
#define pathcpy(dst, src) do { \
            Boolean useErrno = (errno == 0); \
            if (useErrno)       errno = ENAMETOOLONG; \
            if (strlcpy(dst, src, PATH_MAX) >= PATH_MAX)  goto finish; \
            if (useErrno)       errno = 0; \
    } while(0)
#define pathcat(dst, src) do { \
            Boolean useErrno = (errno == 0); \
            if (useErrno)       errno = ENAMETOOLONG; \
            if (strlcat(dst, src, PATH_MAX) >= PATH_MAX)  goto finish; \
            if (useErrno)       errno = 0; \
    } while(0)
#define makebootpath(path, rpath) do { \
                                    pathcpy(path, up->curMount); \
                                    if (up->flatTarget[0]) { \
                                        pathcat(path, up->flatTarget); \
                                        /* XX 10561671: basename unsafe */ \
                                        pathcat(path, "/"); \
                                        pathcat(path, basename(rpath)); \
                                    } else { \
                                        pathcat(path, rpath); \
                                    } \
                                } while(0)

// continue versions
#define PATHCPYcont(dst, src) do { \
            if (strlcpy(dst, src, PATH_MAX) >= PATH_MAX)  continue; \
    } while(0)
#define PATHCATcont(dst, src) do { \
            if (strlcat(dst, src, PATH_MAX) >= PATH_MAX)  continue; \
    } while(0)

// break versions
#define PATHCPYbreak(dst, src) do { \
            if (strlcpy(dst, src, PATH_MAX) >= PATH_MAX)  break; \
    } while(0)
#define PATHCATbreak(dst, src) do { \
            if (strlcat(dst, src, PATH_MAX) >= PATH_MAX)  break; \
    } while(0)

#define LOGERRxlate(up, ctx1, ctx2, val) do { \
        char *c2cpy = ctx2; \
        if (val == -1) { \
            OSKextLog(/* kext */ NULL, up->errLogSpec, \
                      "%s%s: %s", ctx1, c2cpy, strerror(errno)); \
            val = errno; \
        } else { \
            OSKextLog(/* kext */ NULL, up->errLogSpec, \
                      "%s%s: %s", ctx1, c2cpy, strerror(val)); \
        } \
    } while(0)


// XX there is overlap between errno values and sysexits
static int getExitValueFor(errval)
{
    int rval;

    switch (errval) {
        case ELAST + 1:
            rval = EX_SOFTWARE;
            break;
        case EPERM:
            rval = EX_NOPERM;
            break;
        case EAGAIN:
        case ENOLCK:
            rval = EX_OSERR;
            break;
        case -1:
            switch (errno) {
                case EIO:
                    rval = EX_IOERR;
                    break;
                default:
                    rval = EX_OSERR;
                    break;
            }
            break;
        default:
            rval = errval;
    }

    return rval;
}

// TM should no longer add to Apple_Boot partitions (8992773)
#define MOBILEBACKUPS_DIR "/.MobileBackups"
#define MDS_BULWARK "/.metadata_never_index"
#define MDS_DIR "/.Spotlight-V100"
#define FSEVENTS_BULWARK "/.fseventsd/no_log"
#define FSEVENTS_DIR "/.fseventsd"
static int
sanitizeBoot(struct updatingVol *up)
{
    int rval = 0;
    int fd = -1;
    struct statfs sfs;
    char bloatp[PATH_MAX], blockp[PATH_MAX];
    Boolean blockMissing = true;
    struct stat sb;
    
    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Removing unnecessary bloat.");

    // X if the size similar to a user's data volume, don't scrub
    if ((fstatfs(up->curbootfd, &sfs) == 0) &&
            (sfs.f_blocks * sfs.f_bsize > 1ULL<<32)) {
        return rval;
    }

    // make sure root is owned by root!
    if ((fstat(up->curbootfd, &sb) == 0) &&
            (sb.st_uid != UID_ROOT || sb.st_gid != GID_WHEEL))
        rval |= fchown(up->curbootfd, UID_ROOT, GID_WHEEL);

    // Time Machine
    makebootpath(bloatp, MOBILEBACKUPS_DIR);
    if (0 == (stat(bloatp, &sb))) {
        fd = sdeepunlink(up->curbootfd, bloatp);
        if (fd == -1) {
            rval |= errno;
        } else {
            close(fd);
        }
    } 

    // Spotlight
    makebootpath(blockp, MDS_BULWARK);
    if (-1 == stat(blockp, &sb) && errno == ENOENT) {
        fd = sopen(up->curbootfd, blockp, O_CREAT, kCacheFileMode);
        if (fd == -1) {
            rval |= errno;
        } else {
            close(fd);
        }
    }
    makebootpath(bloatp, MDS_DIR);
    if (0 == (stat(bloatp, &sb))) {
        rval |= sdeepunlink(up->curbootfd, bloatp);
    } 

    // FSEvents has its antithesis inside its directory :P
    // we'll assume if no_log is present, that there's no cruft
    makebootpath(bloatp, FSEVENTS_DIR);
    makebootpath(blockp, FSEVENTS_BULWARK);
    if (0 == (stat(bloatp, &sb))) {
        if (-1 == stat(blockp, &sb) && errno == ENOENT) {
            // no bulwark, so nuke the whole thing
            rval |= sdeepunlink(up->curbootfd, bloatp);
        } else {
            blockMissing = false;
        }
    } 

    if (blockMissing) {
        // then recreate the directory and the "stay away" file
        rval |= sdeepmkdir(up->curbootfd, bloatp, kCacheDirMode);
        fd = sopen(up->curbootfd, blockp, O_CREAT, kCacheFileMode);
        if (fd == -1) {
            rval |= errno;
        } else {
            close(fd);
        }
    }

    // no accumulated errors -> success

finish:
    if (rval)
        OSKextLog(NULL,up->warnLogSpec,"Warning: trouble cleaning Apple_Boot");

    return rval;
}


/******************************************************************************
 * checkForMissingFiles
 * Look for missing files in the relevant Apple_boot (helper) partition.  If
 * any of the files we care about are missing then we force an update of those 
 * files.
******************************************************************************/
static void
checkForMissingFiles(struct updatingVol *up)
{
    unsigned i;
    char srcpath[PATH_MAX], dstpath[PATH_MAX];
    struct stat sb;

    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Looking for missing files.");
   
    // forced updates not allowed with -U (early boot)
    if (up->expectUpToDate)  return;
    
    /* looking for missing .VolumeIcon.icns, SystemVersion.plist, 
     * PlatformSupport.plist, .disk_label, etc
     */
    if (!up->doMisc) {
        for (i = 0; i < up->caches->nmisc; i++) {
            pathcpy(srcpath, up->caches->root);
            pathcat(srcpath, up->caches->miscpaths[i].rpath);
            makebootpath(dstpath, up->caches->miscpaths[i].rpath);
        
            /* look to see if our source file exists (some may not) and if it does
             * and it's clone is NOT in Apple_Boot then force an update
             */
            if (stat(srcpath, &sb) == 0) { 
                // source file exists, now check on Apple_Boot
                if (stat(dstpath, &sb) != 0 && errno == ENOENT) {
                    // missing file, force an update
                    up->doMisc = true;
                    OSKextLog(nil,kOSKextLogFileAccessFlag|kOSKextLogBasicLevel,
                        "Helper partition missing misc files, forcing update");
                    break;
                }
            }         
        }
    }
    
    // now look for boot.efi
    if (!up->doBooters) {
        if (up->caches->efibooter.rpath[0]) {
            makebootpath(dstpath, up->caches->efibooter.rpath);
            if (stat(dstpath, &sb) != 0 && errno == ENOENT) {
                // missing file, force an update
                up->doBooters = true;
                OSKextLog(NULL, kOSKextLogFileAccessFlag|kOSKextLogBasicLevel,
                         "Helper partition missing EFI booter, forcing update");
                goto finish;
            }
        }
        // OF booter deserves love too :)
        if (up->caches->ofbooter.rpath[0]) {
            makebootpath(dstpath, up->caches->ofbooter.rpath);
            if (stat(dstpath, &sb) != 0 && errno == ENOENT) {
                // missing file, force an update
                up->doBooters = true;
                OSKextLog(NULL, kOSKextLogFileAccessFlag|kOSKextLogBasicLevel,
                         "Helper partition missing OF booter, forcing update");
                goto finish;
            }
        }
    }
    
finish:
    return;
}

/*******************************************************************************
* updateBootHelpers() updates per the passed-in struct updatingVol.
* Sec: must ensure each target is one of the source's Apple_Boot partitions
* Logically, callers provide up->boots,caches but initContext() also
* fills in up->dasession.  Callers must also releaseContext() afterwards.
*
* expectUpToDate -> just move the labels aside
******************************************************************************/
static int
updateBootHelpers(struct updatingVol *up, Boolean expectUpToDate)
{
    int errnum, result = 0;
    up->curbootfd = -1;
    struct stat sb;
    CFIndex bootcount, bootupdates = 0;
 
    // if the plist has gone stale, punt
    if ((result = fstat(up->caches->cachefd, &sb))) {
        OSKextLog(NULL, up->errLogSpec, "fstat(cachefd): %s", strerror(errno));
        goto finish;
    }

    bootcount = CFArrayGetCount(up->boots);
    for (up->bootIdx = 0; up->bootIdx < bootcount; up->bootIdx++) {
        char path[PATH_MAX];

        up->changestate = nothingSerious;           // init state
        if ((errnum = mountBoot(up))) {             // sets curMount
            result = errnum; goto bootfail;  
        }

        // if directed, nuke anything that doesn't belong
        if (up->doSanitize) {
            (void)sanitizeBoot(up);
        }
        if (up->cleanOnceDir && 
                strlcpy(path, up->curMount, PATH_MAX) < PATH_MAX &&
                strlcat(path, kBRBootOnceDir, PATH_MAX) < PATH_MAX &&
                0 == stat(path, &sb)) {
            (void)sdeepunlink(up->curbootfd, path);
        }

        // If files are missing, update up.do* to ensure we copy them
        // (implicitly forcing their update in subsequent helpers).
        checkForMissingFiles(up);

        if (up->doRPS && (result = ucopyRPS(up))) {
            goto bootfail;  // -> inactive
        }
        if (up->doMisc) {
            (void) ucopyMisc(up);  // -> .new files
        }
        
        // get the label out of the way (should be optional?)
        // expectUpToDate => early boot -> harder to generate label?
        if (expectUpToDate) {
            if ((result = moveLabels(up))) {
                goto bootfail;
            }
        } else {
            if ((result = nukeBRLabels(up))) {
                goto bootfail;
            }
        }
        
        if (up->doBooters && (result = ucopyBooters(up))) {                
            goto bootfail;      // .old still active
        }
        // If Recovery OS was available, we could swap these two and leave
        // the Recovery OS blessed until RPS and new booters were activated.
        if (up->doBooters && (result = activateBooters(up))) { // committed
            goto bootfail;
        }
        // 10.x.n+1 booters remain compatible 10.x.n kernels?? (power outage!)
        if (up->doRPS && (result = activateRPS(up))) {         // complete
            goto bootfail;
        }
        if ((result = activateMisc(up))) {
            goto bootfail;      // reverts label
        }

        up->changestate = nothingSerious;
        bootupdates++;      // loop success
        // -U -> updates are a warning
        OSKextLog(NULL, kOSKextLogFileAccessFlag |
                  (expectUpToDate?kOSKextLogWarningLevel:kOSKextLogBasicLevel),
                  "Successfully updated %s%s.", up->bsdname, up->flatTarget);
        
bootfail:
        // clean up this helper only, no hard failures in the loop
        if (up->changestate != nothingSerious && up->helpersOptional==false) {
            OSKextLog(NULL, up->errLogSpec,
                      "Error updating helper partition %s, state %d: %s.",
                      up->bsdname, up->changestate,
                      bootReversionsStrings[up->changestate]);
        }
        // unroll any changes we may have made
        (void)revertState(up);     // smart enough to do nothing
        if (result && up->flatTarget[0]) {
            (void)sdeepunlink(up->curbootfd, up->dstdir);
        }
        
        // clean up and unmount (flatTarget -> might not be a helper)
        // X could check for MNT_DONTBROWSE as a hint it's okay to unmount
        if (nukeFallbacks(up)) {
            OSKextLog(NULL, up->errLogSpec, "Warning: %s%s may be untidy.",
                      up->bsdname, up->flatTarget);
        }
        if (up->flatTarget[0] == '\0') {
            unmountBoot(up);       // best effort; no-op if not mounted
        }
    }

    if (bootupdates != bootcount && up->helpersOptional == false) {
        OSKextLog(NULL, up->errLogSpec, "Failed to update helper partition%s.",
                  bootcount - bootupdates == 1 ? "" : "s");
        // bullet-proofing: make sure there is a generic error
        if (result == 0) {
            // should always be a non-zero result at this point
            result = ELAST + 1;
        }
        goto finish;
    }

finish:
    return result;
}

/******************************************************************************
* entry points to update caches and copy files to helper partitions
* these culminate in BRUpdateBootFiles() and BRCopyBootFiles().
******************************************************************************/
// XX move to bootcaches.[ch]?
// sBRUptLock is accessible here and could be used to conditionalize
// the setting of _skiplocks.  But having kextd use this would be worth
// the move.
int
checkRebuildAllCaches(struct bootCaches *caches, int oodLogSpec)
{
    int opres, result = ELAST + 1;  // no pathc() [yet]
    struct stat sb;
 
    // if the caches data is no longer valid, abort immediately
    if ((opres = fstat(caches->cachefd, &sb))) { 
        result = opres; goto finish;
    }

    OSKextLog(NULL, kOSKextLogProgressLevel | kOSKextLogArchiveFlag,
              "Ensuring %s's caches are up to date.", caches->root);

    /* XX Sec (re-review?): can't let an external volume insert a cache
     * - mktmp/mkstmp used to create temp file at destination
     * - final rename must be on whatever volume provided the kexts
     * - if volume is /, then kexts owned by root can be trusted (4623559 fstat)
     * - otherwise, rename from wrong volume will fail
     */
     
    // We have to rely on the system's kextcache + IOKit.framework to
    // rebuild these caches.  If called on an older system via
    // libBootRoot against newer cache files, the launched kextcache
    // processes are unlikely to know how to update the caches.  Errors
    // should be returned.

    // avoid deadlock with the kextcache processes which might launch below
    // XX callers of this function should have taken the lock via initContext()
    setenv("_com_apple_kextd_skiplocks", "1", 1);


    // update the various mach kernel caches
    if (check_kext_boot_cache_file(caches,
                  caches->kext_boot_cache_file->rpath, caches->kernel)) {

        // rebuild the mkext under our lock / lack thereof
        // (-v forwarded via environment variable by kextcache & kextd)
        OSKextLog(nil, oodLogSpec, "rebuilding %s",
                caches->kext_boot_cache_file->rpath);
        if ((opres = rebuild_kext_boot_cache_file(caches, true /*wait*/,
                caches->kext_boot_cache_file->rpath, caches->kernel))) {
            OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogArchiveFlag,
                      "Error %d rebuilding %s.", result,
                      caches->kext_boot_cache_file->rpath);
                result = opres; goto finish;
        }
    } else {
        OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogArchiveFlag,
                  "Primary kext cache does not need update.");
    }



    // Check/rebuild the CSFDE property cache which goes into the Apple_Boot.
    // It's less critical for booting, but more critical for security.
    if (check_csfde(caches)) {
        OSKextLog(NULL,oodLogSpec,"rebuilding %s",caches->erpropcache->rpath);
        if ((opres = rebuild_csfde_cache(caches))) {
            OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogArchiveFlag,
                      "Error %d rebuilding %s.", result,
                      caches->erpropcache->rpath);
            result = opres; goto finish;
        }
    } else {
        OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogArchiveFlag,
                  "CSFDE property cache does not need update.");
    }

    // check on the (optional) localized resources used by EFI Login
    if (check_loccache(caches)) {
        OSKextLog(NULL,oodLogSpec,"rebuilding %s",caches->efiloccache->rpath);
        if ((result = rebuild_loccache(caches))) {
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogArchiveFlag,
                      "Warning: Error %d rebuilding %s", result == -1
                      ? errno : result, caches->efiloccache->rpath);
        }
        // efiloccache is not required when copying rpspaths
        // so we can ignore failures to rebuild the cache.
    }

    // success!
    result = 0;

finish:
    return result;
}


/*****************************************************************************
* initContext() sets up a struct updatingVol for use by other functions
* - volRoot must contain a supported bootcaches.plist
* - volRoot will be locked with kextd
* - if available, diskarb will be configured up->dasession
* - specifiying helperBSDName -> up->boots = [ helperBSDName ]
* releaseContext() should be called when the context is no longer needed.
*****************************************************************************/
#define BOOTCOUNT 1
static int
initContext(struct updatingVol *up, CFURLRef srcVol, CFStringRef helperBSDName,
            int expectUpToDate)
{
    int opres, result = ELAST + 1;      // circa 4/24/2012, all paths set
    const void         *values[BOOTCOUNT] = { helperBSDName };

    // start fresh (caller's job to have called releaseContext())
    bzero(up, sizeof(struct updatingVol));

    // callers want to rely on these log spec values even after failure
    up->warnLogSpec = kOSKextLogArchiveFlag | kOSKextLogWarningLevel;
    up->errLogSpec = kOSKextLogArchiveFlag | kOSKextLogErrorLevel;

    up->expectUpToDate = expectUpToDate;

    // takeVolumeForPath() wants a char* ... comes before up->caches = ...
    if (!CFURLGetFileSystemRepresentation(srcVol, /* resolveToBase */ true,
                             (UInt8 *)up->srcRoot,sizeof(up->srcRoot))){
        OSKextLogStringError(NULL);
        result = ENOMEM; goto finish;
    }

    // Technically, we don't need to lock to read bootcaches.plist,
    // but if there are multiple kextcache -u processes, it leaves
    // a longer window during which files can be updated.  Also, 
    // being locked means owners are enabled so it's okay for
    // read[SIC]BootCaches() to make the bootstamps directory.

    // For now, kextcache -U in early boot doesn't lock.
    // (part of why kextd delays auto-rebuild for 5 minutes)
    if (expectUpToDate && getppid() == 2 /* launchctl */) {
        up->earlyBoot = true;
    } else {
        if ((opres = takeVolumeForPath(up->srcRoot))) { // lock (logs)
            result = opres; goto finish;
        }
    }

    // initializing the context fails if there's no bootcaches.plist
    if (!(up->caches = readBootCaches(up->srcRoot))) {
        result = errno ? errno : ELAST + 1;
        goto finish;
    }

    // attempt to configure a disk arb session
    if ((up->dasession = DASessionCreate(nil))) {
        // mountBoot and unmountBoot will spin the runloop for this DA session
        DASessionScheduleWithRunLoop(up->dasession, CFRunLoopGetCurrent(),
                kCFRunLoopDefaultMode);
    } else {
        OSKextLog(NULL, up->warnLogSpec, "Warning: proceeding w/o DiskArb");
    }

    // if specified, this partition is the one to update
    if (helperBSDName) {
        up->boots = CFArrayCreate(nil,values,BOOTCOUNT,&kCFTypeArrayCallBacks);
    }

    result = 0;

finish:
    return result;
}

static void
releaseContext(struct updatingVol *up, int status)
{
    // unmountBoot() not always called
    if (up->curBoot)            CFRelease(up->curBoot);
    if (up->curbootfd != -1)    close(up->curbootfd);

    if (up->dasession) {
        DASessionUnscheduleFromRunLoop(up->dasession, CFRunLoopGetCurrent(),
                kCFRunLoopDefaultMode);
        CFRelease(up->dasession);
        up->dasession = NULL;
    }

    if (up->boots)          CFRelease(up->boots);
    if (up->csfdeprops)     CFRelease(up->csfdeprops);
    if (up->bpdata)         CFRelease(up->bpdata);
    if (up->caches)         destroyCaches(up->caches);

    // unlock
    putVolumeForPath(up->srcRoot, status);
}

static void
addDictOverride(const void *key, const void *value, void *ctx)
{
    CFMutableDictionaryRef tgtDict = (CFMutableDictionaryRef)ctx;

    // AddValue is "add if absent;" we implement override by removing
    if (CFDictionaryContainsKey(tgtDict, key))
        CFDictionaryRemoveValue(tgtDict, key);

    CFDictionaryAddValue(tgtDict, key, value);
}

static CFDataRef
createBootPrefData(struct updatingVol *up, uuid_string_t root_uuid,
                   CFDictionaryRef bootPrefOverrides)
{
    CFDataRef rval = NULL;
    char srcpath[PATH_MAX];
    int fd = -1;
    void *buf = NULL;
    CFDataRef data = NULL;
    CFMutableDictionaryRef pldict = NULL;
    CFStringRef UUIDStr = NULL;
    CFStringRef kernPathStr = NULL;

    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "creating com.apple.Boot.plist data with UUID %s.",
              root_uuid);

    // suck in any existing plist
    do {
        struct stat sb;
        PATHCPYcont(srcpath, up->caches->root);
        PATHCATcont(srcpath, up->caches->bootconfig->rpath);
        if (-1 == (fd=sopen(up->caches->cachefd,srcpath,O_RDONLY,0)))
            break;
        if (fstat(fd, &sb))                                 break;
        if (sb.st_size > UINT_MAX || sb.st_size > LONG_MAX) break;
        if (!(buf = malloc((size_t)sb.st_size)))            break;
        if (read(fd, buf, (size_t)sb.st_size) != sb.st_size)break;
        if (!(data = CFDataCreate(nil, buf, (long)sb.st_size)))
            break;

        // make mutable dictionary from file data
        pldict =(CFMutableDictionaryRef)CFPropertyListCreateFromXMLData(nil,
                    data, kCFPropertyListMutableContainers, NULL/*errstr*/);
    } while(0);

    errno = 0;

    // if we got a dictionary, just grab the file mode
    if (!pldict || CFGetTypeID(pldict)!=CFDictionaryGetTypeID()) {
        // otherwise, create a dictionary
        if (pldict)      CFRelease(pldict);     // e.g. if it was a non-dict
        pldict = CFDictionaryCreateMutable(nil, 1,
                                        &kCFTypeDictionaryKeyCallBacks,
                                        &kCFTypeDictionaryValueCallBacks);
        if (!pldict)    goto finish;
    }

    // make a CFStr out of the UUID and insert
    errno = 0;
    UUIDStr = CFStringCreateWithCString(nil,root_uuid,kCFStringEncodingASCII);
    if (!UUIDStr)   goto finish;
    CFDictionarySetValue(pldict, CFSTR(kRootUUIDKey), UUIDStr);
    if (!CFEqual(CFDictionaryGetValue(pldict,CFSTR(kRootUUIDKey)), UUIDStr))
        goto finish;

    // if necessary, tell the booter to load <flatTarget>/kernelcache
    if (up->flatTarget[0]) {
        char kpath[PATH_MAX];
        /* XX 10561671: basename() unsafe */
        pathcpy(kpath, up->flatTarget);
        pathcat(kpath, "/");
        pathcat(kpath, basename(up->caches->kext_boot_cache_file->rpath));
        kernPathStr = CFStringCreateWithFileSystemRepresentation(nil, kpath);
        if (!kernPathStr)   goto finish;
        CFDictionarySetValue(pldict, CFSTR(kKernelCacheKey), kernPathStr);
    }

    // add any additional override values
    if (bootPrefOverrides) {
        CFDictionaryApplyFunction(bootPrefOverrides,addDictOverride,pldict);
    }

    rval = CFPropertyListCreateXMLData(nil, pldict);

finish:
    if (kernPathStr)    CFRelease(kernPathStr);
    if (UUIDStr)        CFRelease(UUIDStr);
    if (pldict)         CFRelease(pldict);
    if (data)           CFRelease(data);

    if (buf)        free(buf);
    if (fd != -1)   close(fd);

    return rval;
}


/******************************************************************************
* checkUpdateCachesAndBoots() returns
* - success (EX_OK / 0) if nothing needs updating
* - success if updates were successfully made (and expectUTD = false)
* - EX_OSFILE if updates were unexpectedly needed and successfully made
******************************************************************************/
// keeping these active for reliability testing of live builds
#define BRDBG_OOD_HANG_BOOT_F "/var/db/.BRHangBootOnOODCaches"
#define BRDBG_HANG_MSG PRODUCT_NAME ": " BRDBG_OOD_HANG_BOOT_F \
                    "-> hanging on out of date caches"
#define BRDBG_CONS_MSG "[via /dev/console] " BRDBG_HANG_MSG "\n"
int
checkUpdateCachesAndBoots(CFURLRef volumeURL, updateOpts_t opts)
{
    int opres, result = ELAST + 1;          // try to always set on error
    OSKextLogSpec oodLogSpec = kOSKextLogGeneralFlag | kOSKextLogBasicLevel;
    Boolean expectUpToDate = (opts & kExpectUpToDate);
    Boolean doAny, cachesUpToDate = false;
    struct updatingVol up = { /*NULL...*/ };
    up.curbootfd = -1;

    // try to configure 'up'; treat missing data per opts
    if ((opres = initContext(&up, volumeURL, NULL, expectUpToDate))) {
        char *bcmsg = NULL;
        CFArrayRef helpers;
        switch (opres) {        // describe known problems
            case ENOENT: bcmsg = "no " kBootCachesPath; break;
            case EFTYPE: bcmsg = "unrecognized " kBootCachesPath; break;
            default:     break;
        }
        if ((opts & kForceUpdateHelpers) &&
                (helpers = BRCopyActiveBootPartitions(volumeURL))) {
            // helper partitions + -f => we require bootcaches.plist
            OSKextLog(NULL,up.errLogSpec,"%s: %s; aborting",up.srcRoot,bcmsg);
            CFRelease(helpers);
            result = opres; goto finish;
        } else if (bcmsg) {
            // politely pass on known limitations
            OSKextLog(NULL, oodLogSpec, "%s: %s; skipping",up.srcRoot,bcmsg);
            result = 0; goto finish;
        } else {
            // unknown error; fail
            OSKextLog(NULL, up.errLogSpec, "%s: error %d reading "
                      kBootCachesPath, up.srcRoot, opres);
            result = opres; goto finish;
        }
    }

    // -U logs what is out of date at a a more urgent level than -u
    if (expectUpToDate) {
        oodLogSpec = up.errLogSpec;
    }

    // do some real work updating caches *in* the source volume
    // checkRebuildAllCaches() logs its own errors
    if ((opres = checkRebuildAllCaches(up.caches, oodLogSpec))) {
        result = opres; goto finish;
    }

    // record partial success
    up.helpersOptional = (opts & kHelpersOptional);
    cachesUpToDate = true;
    
    // 9455881: If requested, only update the caches
    if (opts & kCachesOnly) {
        goto doneUpdatingHelpers;
    }

    if (!hasBootRootBoots(up.caches, &up.boots, NULL, &up.onAPM)) {
        OSKextLog(NULL, kOSKextLogBasicLevel | kOSKextLogFileAccessFlag,
              "%s: no supported helper partitions to update.", up.srcRoot);
        goto doneUpdatingHelpers;   // no boots -> nothing more to do
    }

    /* --- updating a Boot!=Root volume --- */

    // these are helper (not OS) partitions & should be clean
    up.doSanitize = true;

    // figure out what needs updating
    // needUpdates() also populates the timestamp values used by updateStamps()
    doAny = needUpdates(up.caches, &up.doRPS, &up.doBooters, &up.doMisc,
                        oodLogSpec);

#ifdef BRDBG_OOD_HANG_BOOT_F
    // check to see if out of date at early boot should cause a hang
    if (up.earlyBoot && doAny) {
        struct stat sb;
        int consfd = open(_PATH_CONSOLE, O_WRONLY|O_APPEND);
        while (stat(BRDBG_OOD_HANG_BOOT_F, &sb) == 0) {
            OSKextLog(NULL, up.errLogSpec, BRDBG_HANG_MSG);
            if (consfd > -1)
                write(consfd, BRDBG_CONS_MSG, sizeof(BRDBG_CONS_MSG)-1);
            sleep(30);
        }
    }
#endif  // BRDBG_OOD_HANG_BOOT_F

    // force ignores needUpdates() and does extra helper cleanup
    if (opts & kForceUpdateHelpers) {
        up.doRPS = up.doBooters = up.doMisc = true;
        up.cleanOnceDir = true;
    } else if (!doAny) {
        // LogLevelBasic is only emitted with -v and above
        OSKextLog(NULL, kOSKextLogBasicLevel | kOSKextLogFileAccessFlag,
                  "%s: helper partitions appear up to date.", up.srcRoot);
        goto doneUpdatingHelpers;
    }

    // fill in root_uuid, csfdeprops
    if (!(up.bpdata = createBootPrefData(&up, up.caches->fsys_uuid, NULL))) {
        result = ENOMEM; goto finish;       // cBPD() logs errors
    }
    if (up.caches->csfde_uuid) {
        opres = copyCSFDEInfo(up.caches->csfde_uuid, &up.csfdeprops, NULL);
        if (opres) {
            result = opres; goto finish;    // c..Info() logs errors
        }
    }

    // request actual helper updates
    if ((opres = updateBootHelpers(&up, expectUpToDate))) {
        result = opres; goto finish;        // uBH() logs errors
    }

    if ((opres = updateStamps(up.caches, kBCStampsApplyTimes))) {
        OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
                  "%s: could not update bootstamps.", up.srcRoot);
        result = opres; goto finish;
    }

doneUpdatingHelpers:
    // success
    result = 0;

    // kExpectUpToDate is used to differentiate "success: everything clean"
    // from "successfully updated:" the latter exits with EX_OSFILE.  During
    // early boot, this informs launchd to force a reboot off fresh caches.
    if (doAny && expectUpToDate) {
        result = EX_OSFILE;
    } 

finish:
    if (up.helpersOptional && cachesUpToDate) {
        // partial success okay
        result = 0;
    }

    // In early boot, kextcache -U pings kextd *after* successful non-update
    // instead of before.  XX improve this handoff.
    if (up.earlyBoot && expectUpToDate && result != EX_OSFILE) {
        if (takeVolumeForPath(up.srcRoot)) {
            OSKextLog(NULL, up.errLogSpec, "kextd lock/signal failed");
        }
    }

    // since updateBoots() -> exit(), convert common errors to sysexits(3)
    if (result && result != EX_OSFILE) {
        result = getExitValueFor(result);
    }

    // handles unlock / reporting to kextd
    releaseContext(&up, result);

    // all error paths should log if the functions they call don't

    return result;
}

#define kBRCheckLogSpec (kOSKextLogArchiveFlag | kOSKextLogProgressLevel)
OSStatus
BRUpdateBootFiles(CFURLRef volURL, Boolean force)
{
    if (!volURL)
        return EINVAL;

    return checkUpdateCachesAndBoots(volURL, force ? kForceUpdateHelpers : 0);
}


/* error handling style in this function is an experiment to find a
   1. correct (doesn't miss errors)
   2. robust (doesn't fall over on correctness if not followed)
   3. accurate (returns detailed error values)
   4. readable (can figure out what's going on)
   5. concise (can we achieve #1-3 w/o using both 'errnum' and 'result'?)
   style for handling errors.
*/
static OSStatus
addHostVolInfo(struct updatingVol *up, CFURLRef hostVol,
               CFDictionaryRef bootPrefOverrides, CFURLRef targetStr,
               BRBlessStyle blessSpec, CFStringRef pickerLabel)
{
    OSStatus result = EOVERFLOW;    // ! only set AFTER error detected !
    OSStatus errnum;                // temp var for collecting error values
    uuid_t vol_uuidbytes;
    uuid_string_t vol_uuidchars;
    CFStringRef csUUIDStr = NULL;
    char hostroot[PATH_MAX];

    // check/configure caller-specified target directory
    if (targetStr) {
        char targetdir[PATH_MAX], *slash;
        if (!CFURLGetFileSystemRepresentation(targetStr, true /*resolve*/,
                                              (UInt8*)targetdir, PATH_MAX)) {
            result = EINVAL; goto finish;
        }
        // target dir must not be '/'
        slash = targetdir;
        while (*slash == '/')       slash++;
        if (*slash == '\0') {
            result = EINVAL; goto finish;
        }
        // blessOnce -> special directory we clean up semi-regularly
        if (blessSpec == kBRBlessOnce) {
            pathcpy(up->flatTarget, kBRBootOnceDir);
        } else {
            up->flatTarget[0] = '\0';
        }
        if (targetdir[0] != '/')    // did caller provide a '/'?
            pathcat(up->flatTarget, "/");
        pathcat(up->flatTarget, targetdir);
    }
 
    // get UUIDs
    if (!CFURLGetFileSystemRepresentation(hostVol, true /*resolve base*/,
            (UInt8*)hostroot, PATH_MAX)) {
        result = ENOMEM; goto finish;
    }
    if ((errnum=copyVolumeInfo(hostroot,&vol_uuidbytes,&csUUIDStr,NULL,NULL))){
        result = errnum; goto finish;
    }
    uuid_unparse_upper(vol_uuidbytes, vol_uuidchars);

    // set up com.apple.Boot.plist, CSFDE property cache data
    up->bpdata = createBootPrefData(up, vol_uuidchars, bootPrefOverrides);
    if (!up->bpdata) {
        result = ENOMEM; goto finish;    // logs its own
    }
    if (csUUIDStr) {
        if ((errnum = copyCSFDEInfo(csUUIDStr, &up->csfdeprops, NULL))) {
            result = errnum; goto finish;
        }
    }

    // just overwrite caches->volname since that's all it should be used for
    if (pickerLabel) {
        if (!CFStringGetFileSystemRepresentation(pickerLabel, 
                                          up->caches->volname, PATH_MAX)) {
            result = EINVAL; goto finish;
        }
    }

    result = 0;
    
finish:    
    if (csUUIDStr)      CFRelease(csUUIDStr);

    return result;
}


/******************************************************************************
* copy boot files from source volume to destination partition
* - makes sure caches are up to date
* - ignore up to date bootstamps
* - *do* update bootstamps if srcVol == hostVol
* see bootroot.h for more details
******************************************************************************/
OSStatus
BRCopyBootFilesToDir(CFURLRef srcVol,
                     CFURLRef initialRoot,
                     CFDictionaryRef bootPrefOverrides,
                     CFStringRef targetBSDName,
                     CFURLRef targetDir,
                     BRBlessStyle blessSpec,
                     CFStringRef pickerLabel)
{
    OSStatus            result = ELAST + 1;     // generic = safest
    OSStatus            errnum;
    Boolean isBRDefault;
    struct updatingVol  up = { /* NULL, ... */ };
    up.curbootfd = -1;

    // defend libBootRoot entry point
    if (!srcVol || !initialRoot || !targetBSDName) {
        result = EINVAL; goto finish;
    }
    
    // attempt to detect updates that will later look valid to Boot!=Root
    // X: re targetBSDName, BRCopyActiveBootPartitions() doesn't know future
    // XX 9173158 tracks teaching B!=R to temporarily ignore custom configs
    isBRDefault = !targetDir && (blessSpec & kBRBlessFSDefault) &&
                  CFEqual(srcVol, initialRoot);

    // configure a single-helper context
    errnum = initContext(&up, srcVol, targetBSDName, false /*expectUTD*/);
    if (errnum) {
        result = errnum; goto finish;
    }

    // Make sure all caches are up to date on the source
    // (undefined if OOD & system's kext management can't rebuild)
    errnum = checkRebuildAllCaches(up.caches, kBRCheckLogSpec);
    if (errnum) {
        result = errnum; goto finish;
    }

    // if appropriate, gather timestamp data to apply on success
    if (isBRDefault) {
        (void)needUpdates(up.caches, NULL, NULL, NULL,
                          kOSKextLogGeneralFlag | kOSKextLogProgressLevel);
    }

    // configure additional options
    errnum = addHostVolInfo(&up, initialRoot, bootPrefOverrides,
                            targetDir, blessSpec, pickerLabel);
    if (errnum) {
        result = errnum; goto finish;
    }
    up.skipHelperBless = (blessSpec & kBRBlessFSDefault) == 0;

    // BRCopyBootFiles() always copies everything
    up.doRPS = up.doBooters = up.doMisc = true;

    // new content -> clean up any old temp stuff
    up.cleanOnceDir = true;

    // sanitize if this is a default Boot!=Root setup
    // (XX disables Spotlight, FSEvents; see 4 GB check in sanitizeBoot())
    up.doSanitize = isBRDefault;

    // get it updated!
    errnum = updateBootHelpers(&up, false /*ood fine*/);
    if (errnum) {
        result = errnum; goto finish;
    }

    // update stamps if this is likely [to later be] a valid B!=R setup
    // XXX this currently updates stamps for multi-PV when it should not. :P
    if (isBRDefault) {
        errnum = updateStamps(up.caches, kBCStampsApplyTimes);
        if (errnum) {
            result = errnum; goto finish;
        }
    }

    // success
    result = 0;

finish:
    releaseContext(&up, result);

    return result;
}

OSStatus
BRCopyBootFiles(CFURLRef srcVol,
                CFURLRef initialRoot,
                CFStringRef helperBSDName,
                CFDictionaryRef bootPrefOverrides)
{
    return BRCopyBootFilesToDir(srcVol, initialRoot, bootPrefOverrides, 
                                helperBSDName, NULL /*helperDir*/, 
                                kBRBlessFull, NULL /*pickerLabel*/);
}


/******************************************************************************
* FindRPSDir plays rock, paper scissors to identify the location of
* the latest complete copy of the files the booter needs.
******************************************************************************/
static int
FindRPSDir(struct updatingVol *up, char prev[PATH_MAX], char current[PATH_MAX],
            char next[PATH_MAX])
{
     char rpath[PATH_MAX], ppath[PATH_MAX], spath[PATH_MAX];   
/*
 * FindRPSDir looks for a "rock," "paper," or "scissors" directory
 * - handle all permutations: 3 dirs, any 2 dirs, any 1 dir
 */
// static EFI_STATUS
// FindRPSDir(EFI_FILE_HANDLE BootDir, EFI_FILE_HANDLE *newBoot)
// 
    int rval = ELAST + 1, status;
    struct stat r, p, s;
    Boolean haveR = false, haveP = false, haveS = false;
    char *prevp = NULL, *curp = NULL, *nextp = NULL;

    // set up full paths with intervening slash
    pathcpy(rpath, up->curMount);
    pathcat(rpath, "/");
    pathcpy(ppath, rpath);
    pathcpy(spath, rpath);

    pathcat(rpath, kBootDirR);
    pathcat(ppath, kBootDirP);
    pathcat(spath, kBootDirS);

    status = stat(rpath, &r);   // easier to let this fail
    haveR = (status == 0);
    status = stat(ppath, &p);
    haveP = (status == 0);
    status = stat(spath, &s);
    haveS = (status == 0);

    if (haveR && haveP && haveS) {    // NComb(3,3) = 1
        OSKextLog(NULL, up->warnLogSpec,
                  "Warning: all of R,P,S exist: picking 'R'; destroying 'P'.");
        curp = rpath;   nextp = ppath;  prevp = spath;
        if ((rval = eraseRPS(up, nextp)))
            goto finish;
    }   else if (haveR && haveP) {          // NComb(3,2) = 3
        // p wins
        curp = ppath;   nextp = spath;  prevp = rpath;
    } else if (haveR && haveS) {
        // r wins
        curp = rpath;   nextp = ppath;  prevp = spath;
    } else if (haveP && haveS) {
        // s wins
        curp = spath;   nextp = rpath;  prevp = ppath;
    } else if (haveR) {                     // NComb(3,1) = 3
        // r wins by default
        curp = rpath;   nextp = ppath;  prevp = spath;
    } else if (haveP) {
        // p wins by default
        curp = ppath;   nextp = spath;  prevp = rpath;
    } else if (haveS) {
        // s wins by default
        curp = spath;   nextp = rpath;  prevp = ppath;
    } else {                                          // NComb(3,0) = 0
        // we'll start with rock
        curp = rpath;   nextp = ppath;  prevp = spath;
    }

    if (strlcpy(prev, prevp, PATH_MAX) >= PATH_MAX)     goto finish;
    if (strlcpy(current, curp, PATH_MAX) >= PATH_MAX)   goto finish;
    if (strlcpy(next, nextp, PATH_MAX) >= PATH_MAX)     goto finish;

    rval = 0;

finish:
    if (rval) {
        /* can't use errno here since strlcpy and strlcat don't set it */
        OSKextLog(NULL, up->errLogSpec,
                  "%s - strlcpy or cat failed - >= PATH_MAX", __FUNCTION__);
    }

    return rval;
}

/******************************************************************************
* BREraseBootFiles() un-does BRCopyBootFiles()
******************************************************************************/
// helper does wraps BLSetFinderVolumeInfo with schdir()
static int
sBLSetBootFinderInfo(struct updatingVol *up, uint32_t newvinfo[8])
{
    int result, fd = -1;
    uint32_t    vinfo[8];

    result = schdir(up->curbootfd, up->curMount, &fd);
    if (result)         goto finish;
    result = BLGetVolumeFinderInfo(NULL, ".", vinfo);
    if (result)         goto finish;
    vinfo[kSystemFolderIdx] = newvinfo[kSystemFolderIdx];
    vinfo[kEFIBooterIdx] = newvinfo[kEFIBooterIdx];
    result = BLSetVolumeFinderInfo(NULL, ".", vinfo);

finish:
    if (fd != -1)  
        (void)restoredir(fd);
    return result;
}

// helper attempts to bless the Recovery OS if present
static int
blessRecovery(struct updatingVol *up)
{
    int result;
    char path[PATH_MAX];
    struct stat sb;
    uint32_t vinfo[8] = { 0, };

    // look up pathnames & file IDs
    result = ENAMETOOLONG;

    makebootpath(path, "/" kRecoveryBootDir);
    if (stat(path, &sb) == -1) {
        result = errno;
        goto finish;
    }
    vinfo[kSystemFolderIdx] = (uint32_t)sb.st_ino;

    // append boot.efi
    pathcat(path, "/");
    pathcat(path, basename(up->caches->efibooter.rpath));
    if (stat(path, &sb) == -1) {
        result = errno;
        goto finish;
    }
    vinfo[kEFIBooterIdx] = (uint32_t)sb.st_ino;

    if ((result = sBLSetBootFinderInfo(up, vinfo))) {
        OSKextLog(NULL, up->warnLogSpec,
                 "Warning: found recovery booter but couldn't bless it.");
    }

finish:
    return result;
}

#define RECERR(up, opres, warnmsg) do { \
            if (opres == -1 && errno == ENOENT) { \
                opres = 0; \
            } \
            if (opres) { \
                if (warnmsg) { \
                    OSKextLog(NULL, up->warnLogSpec, warnmsg); \
                } \
                if (firstErr == 0) { \
                    OSKextLog(NULL, up->warnLogSpec, "capturing err %d / %d", \
                              opres, errno); \
                    firstErr = opres; \
                    if (firstErr == -1)     firstErrno = errno; \
                } \
            } \
        } while(0)

OSStatus
BREraseBootFiles(CFURLRef srcVolRoot, CFStringRef helperBSDName)
{
    OSStatus result = ELAST + 1;
    int opres, firstErrno, firstErr = 0; 
    char path[PATH_MAX], prevRPS[PATH_MAX], nextRPS[PATH_MAX];
    struct stat sb;
    uint32_t zerowords[8] = { 0, };
    unsigned i;
    struct updatingVol  up = { /* NULL, ... */ }, *upp = &up;
    up.curbootfd = -1;
    
    // defend libBootRoot entry point
    if (!srcVolRoot || !helperBSDName) {
        result = EINVAL; goto finish;
    }

    opres = initContext(&up, srcVolRoot, helperBSDName, false /*expUTD*/);
    if (opres) {
        result = opres; goto finish;
    }

    if ((opres = mountBoot(&up))) {        // sets curMount
        result = opres; goto finish;
    }

    // generally best effort

    // bless recovery booter if present; else unbless volume
    if ((blessRecovery(&up))) {
        if ((opres = sBLSetBootFinderInfo(&up, zerowords))) {
            firstErr = opres;
            OSKextLog(NULL, up.warnLogSpec,
                      "Warning: couldn't unbless %s", up.curMount);
        }
    }

    // kill label
    opres = nukeBRLabels(&up);
    RECERR(upp, opres,"Warning: trouble nuking (inactive?) Boot!=Root label.");

    // unlink booters
    if (up.caches->ofbooter.rpath[0]) {
        pathcpy(path, up.curMount);
        pathcat(path, up.caches->ofbooter.rpath);
        opres = sunlink(up.curbootfd, path);
        RECERR(upp, opres, "couldn't unlink OF booter" /* remove w/9217695 */);
    }
    if (up.caches->efibooter.rpath[0]) {
        pathcpy(path, up.curMount);
        pathcat(path, up.caches->efibooter.rpath);
        opres = sunlink(up.curbootfd, path);
        RECERR(upp, opres, "couldn't unlink EFI booter" /* NULL w/9217695 */);
    }

    // find & nuke all RPS directories
    opres = FindRPSDir(&up, prevRPS, up.dstdir, nextRPS);
    if (opres == 0) {
        opres = eraseRPS(&up, prevRPS);
        RECERR(upp, opres, "Warning: trouble erasing R.");
        opres = eraseRPS(&up, up.dstdir);
        RECERR(upp, opres, "Warning: trouble erasing P.");
        opres = eraseRPS(&up, nextRPS);
        RECERR(upp, opres, "Warning: trouble erasing S.");
    } else {
        RECERR(upp, opres, "Warning: couldn't find RPS directories.");
    }
        
    for (i=0; i < up.caches->nmisc; i++) {
        char *rpath = up.caches->miscpaths[i].rpath;

        if (strlcpy(path, up.curMount, PATH_MAX) > PATH_MAX)   continue;
        if (strlcat(path, rpath, PATH_MAX) > PATH_MAX)         continue;
        opres = sdeepunlink(up.curbootfd, path);
        RECERR(upp, opres, "error unlinking miscpath" /* NULL w/9217695 */);
    }

    // clean up com.apple.boot.once if it exists
    pathcpy(path, up.curMount);
    pathcat(path, kBRBootOnceDir);
    if (0 == stat(path, &sb)) {
        opres = sdeepunlink(up.curbootfd, path);
        RECERR(upp, opres, "error unlinking" kBRBootOnceDir);
    }

    // no errors above, so firstErr == 0 -> success
    if (firstErr == -1) {
        firstErr = firstErrno;      // recorded by RECERR
    }
    result = firstErr;

finish:
    unmountBoot(&up);
    
    return result;
}


/******************************************************************************
* revertState() rolls back incomplete changes
******************************************************************************/
static int revertState(struct updatingVol *up)
{
    int rval = 0;       // optimism to accumulate errors with |=
    char path[PATH_MAX], oldpath[PATH_MAX];
    struct bootCaches *caches = up->caches;
    Boolean doMisc;
    struct stat sb;

    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Rolling back any incomplete updates.");
        
    switch (up->changestate) {
        // inactive booters are still good
        case activatedBooters:
            // we've blessed the new booters; so let's bless the old ones
            pathcat(up->ofdst, OLDEXT);
            pathcat(up->efidst, OLDEXT);
            // reactivates the old *if* present
            rval |= activateBooters(up);
        case activatingEFIBooter:
    case activatingOFBooter:        // unneeded since 'bless' is one op
        case copiedBooters:
    case copyingEFIBooter:
        if (caches->efibooter.rpath[0]) {
            makebootpath(path, caches->efibooter.rpath);
            pathcpy(oldpath, path);         // old ones are blessed; rename
            pathcat(oldpath, OLDEXT);
            // only unlink current booter if old one present
            if (stat(oldpath, &sb) == 0) {
                (void)sunlink(up->curbootfd, path);
                rval |= srename(up->curbootfd, oldpath, path);
            }
        }

    case copyingOFBooter:
        if (caches->ofbooter.rpath[0]) {
            makebootpath(path, caches->ofbooter.rpath);
            pathcpy(oldpath, path);
            pathcat(oldpath, OLDEXT);
            // only unlink current booter if old one present
            if (stat(oldpath, &sb) == 0) {
                (void)sunlink(up->curbootfd, path);
                rval |= srename(up->curbootfd, oldpath, path);
            }
        }

    // XX
    // case copyingMisc:
    // would clean up the .new turds

        case noLabel:
            // XX hacky (c.f. nukeFallbacks which nukes .disabled label)
            doMisc = up->doMisc;
            up->doMisc = false;
            rval |= activateMisc(up);  // writes new label if !doMisc
            up->doMisc = doMisc;

        case nothingSerious:
            // everything is good
            break;
    }

finish:
    if (rval) {
        OSKextLog(NULL, kOSKextLogErrorLevel,
                  "error rolling back incomplete updates.");
    }

    return rval;
};

/******************************************************************************
* mountBoot digs in for the root, and mounts up the Apple_Boots
* mountpoint -> up->curMount
******************************************************************************/
static int
_mountBootDA(struct updatingVol *up)
{
    int rval = ELAST + 1;
    CFStringRef mountargs[] = { CFSTR("perm"), CFSTR("nobrowse"), NULL };
    DADissenterRef dis = (void*)kCFNull;
    CFDictionaryRef ddesc = NULL;
    CFURLRef volURL;

    if (!(up->curBoot=DADiskCreateFromBSDName(nil,up->dasession,up->bsdname))){
        goto finish;
    }
    
    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Mounted %s.", up->bsdname);

    // DADiskMountWithArgument might call _daDone before it returns (e.g. if it
    // knows your request is impossible ...)
    // _daDone updates our 'dis[senter]'
    DADiskMountWithArguments(up->curBoot, NULL/*mnt*/,kDADiskMountOptionDefault,
                             _daDone, &dis, mountargs);

    // ... so we use kCFNull and check the value before CFRunLoopRun()
    if (dis == (void*)kCFNull) {
        CFRunLoopRun();         // stopped by _daDone (which updates 'dis')
    }
    if (dis) {
        rval = DADissenterGetStatus(dis);
        // only an error if it's not already mounted
        if (rval != kDAReturnBusy) {
            goto finish;
        }
    }

    // get and stash the mountpoint of the boot partition
    if (!(ddesc = DADiskCopyDescription(up->curBoot)))  goto finish;
    volURL = CFDictionaryGetValue(ddesc, kDADiskDescriptionVolumePathKey);
    if (!volURL || CFGetTypeID(volURL) != CFURLGetTypeID())  goto finish;
    if (!CFURLGetFileSystemRepresentation(volURL, true /*resolve base*/,
            (UInt8*)up->curMount, PATH_MAX))        goto finish;

    // success
    rval = 0;

finish:
    if (ddesc)      CFRelease(ddesc);
    if (dis && dis != (void*)kCFNull) { // for spurious CFRunLoopRun() return
        CFRelease(dis);
    }

    return rval;
}

/* _mountBootBuiltIn() will mount with mount(2) in /var/run.  Use
 * _findMountedhelper() first to see if it's already mounted. */
// Creating BRMNT_PARENT instead of using _PATH_VARRUN because the latter
// contains a trailing '/' and lacks the /private that mount(2) expects.
#define BRMNT_PARENT "/private/var/run"
#define BRMNT BRMNT_PARENT "/brmnt"
static int
_mountBootBuiltIn(struct updatingVol *up)
{
    int bsderr, rval = ELAST + 1;       // all paths should set rval
    int vrfd = -1;
    int fd = -1;
    struct stat sb;
    char devpath[DEVMAXPATHSIZE];
    struct hfs_mount_args hfsargs;

    // establish parent fd (assume /var/run safe)
    if (((vrfd = open(_PATH_VARRUN, O_RDONLY))) == -1) {
        rval = vrfd; goto finish;
    }

    // examine any existing filesystem object at intended mount point 
    // [Can't use sopen() since BRMNT already host a mount.]
    fd = open(BRMNT, O_RDONLY);

    // if it exists but isn't a directory, nuke it
    if (fd != -1 && fstat(fd, &sb)==0 && S_ISDIR(sb.st_mode)==false) {
        if ((bsderr = sunlink(vrfd, BRMNT))) {
            rval = bsderr; goto finish;
        }
        // it should be gone now: reset fd, etc
        close(fd);
        fd = open(BRMNT, O_RDONLY);
        if (fd != -1) {
            rval = EEXIST; goto finish;
        }
    }

    // If BRMNT exists, it is a directory; if not, create it.
    if (fd == -1 && errno == ENOENT) {
        if ((bsderr = smkdir(vrfd, BRMNT, kCacheDirMode))) {
            rval = bsderr; goto finish;
        }
    }

    // set up args & mount
    bzero(&hfsargs, sizeof(hfsargs));
    // _PATH_DEV contains a trailing '/'
    (void)snprintf(devpath, sizeof(devpath), _PATH_DEV "%s", up->bsdname);
    hfsargs.fspec = devpath;
    if ((bsderr = mount("hfs", BRMNT, MNT_DONTBROWSE, &hfsargs))) {
        rval = bsderr; goto finish;
    }

    // record result in context
    if (strlcpy(up->curMount, BRMNT, MNAMELEN) >= MNAMELEN) {
        rval = EOVERFLOW; goto finish;
    }

    // success
    rval = 0;

finish:
    if (fd != -1)       close(fd);
    if (vrfd != -1)     close(vrfd);

    return rval;
}

// loop copied from kextd_watchvol.c:reconsiderVolumes()
static int
_findMountedHelper(struct updatingVol *up)
{
    int rval = ELAST + 1;
    int nfsys, i;
    int bufsz;
    struct statfs *mounts = NULL;

    // get mount list
    if (-1 == (nfsys = getfsstat(NULL, 0, MNT_NOWAIT))) {
        rval = errno; goto finish;
    }
    bufsz = nfsys * sizeof(struct statfs);
    if (!(mounts = malloc(bufsz))) {
        rval = errno; goto finish;
    }
    if (-1 == getfsstat(mounts, bufsz, MNT_NOWAIT)) {
        rval = errno; goto finish;
    }

    // see whether the filesystem is already mounted somewhere
    for (i = 0; i < nfsys; i++) {
        struct statfs *sfs = &mounts[i];
        if (strlen(sfs->f_mntfromname) < sizeof(_PATH_DEV) ||
                0 != strcmp(sfs->f_fstypename, "hfs")) {
            continue;
        }
        if (0 == strcmp(sfs->f_mntfromname+strlen(_PATH_DEV), up->bsdname)){
            if (strlcpy(up->curMount, sfs->f_mntonname, MNAMELEN)>=MNAMELEN) {
                rval = EOVERFLOW; goto finish;
            }
            // we found it!
            rval = 0;
            goto finish;
        }
    }

    // default = not found (success in loop)
    rval = ENOENT;

finish:
    if (mounts)     free(mounts);

    return rval;
}

static int
mountBoot(struct updatingVol *up)
{
    int errnum, rval = ELAST + 1;
    CFStringRef str;
    struct statfs bsfs;
    uint32_t mntgoal;
    struct stat secsb;

    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Mounting helper partition...");

    // request the Apple_Boot mount
    str = (CFStringRef)CFArrayGetValueAtIndex(up->boots, up->bootIdx);
    if (!str) {
        goto finish;
    }
    if (!CFStringGetFileSystemRepresentation(str,up->bsdname,DEVMAXPATHSIZE)){
        goto finish;
    }
    if (up->dasession) {
        if ((errnum = _mountBootDA(up))) {
            rval = errnum; goto finish;
        }
    } else {
        if (_findMountedHelper(up) == ENOENT &&
                (errnum = _mountBootBuiltIn(up))) {
            rval = errnum; goto finish;
        }
    }

    // Sec: get a non-spoofable handle to the current boot (extend trust)
    if (-1 == (up->curbootfd = open(up->curMount, O_RDONLY, 0)))   goto finish;
    if (fstat(up->caches->cachefd, &secsb))  goto finish;    // rootvol extant?

    // Make sure the mount is read/write and has owners enabled.
    // Because helper partitions should always have owners enabled
    // and because we soft-unmount afterwards, we don't attempt to
    // restore this state.
    if (fstatfs(up->curbootfd, &bsfs))  goto finish;
    mntgoal = bsfs.f_flags;
    mntgoal &= ~(MNT_RDONLY|MNT_IGNORE_OWNERSHIP);
    if ((bsfs.f_flags != mntgoal) && updateMount(up->curMount, mntgoal)) {
        OSKextLog(NULL, up->warnLogSpec,
                  "Warning: couldn't update mount to read/write + owners");
    }

    // we only support 128+ MB Apple_Boot partitions
    if (bsfs.f_blocks * bsfs.f_bsize < (128 * 1<<20)) {
        OSKextLog(NULL, up->errLogSpec, "skipping Apple_Boot helper < 128 MB.");
        goto finish;
    }

    rval = 0;

finish:
    if (rval != 0 && (up->curBoot || up->curMount[0])) {
        (void)unmountBoot(up);      // undo anything significant
    }
    if (rval) {
        if (rval != ELAST + 1) {
            if (rval == -1)     rval = errno;
            OSKextLog(NULL, up->errLogSpec,
                "Failed to mount helper (%d/%#x): %s", rval,
                rval & ~(err_local|err_local_diskarbitration), strerror(rval));
        } else {
            OSKextLog(NULL, up->errLogSpec,"Failed to mount helper partition.");
        }
    }

    return rval;
}

/******************************************************************************
* unmountBoot 
* attempt to unmount; no worries on failure
******************************************************************************/
static void
unmountBoot(struct updatingVol *up)
{
    int errnum = 0;
    DADissenterRef dis = (void*)kCFNull;
    
    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Unmounting helper partition %s.", up->bsdname);

    // clean up curbootfd
    if (up->curbootfd != -1) {
        close(up->curbootfd);
        up->curbootfd = -1;
    }

    // clean up any DiskArb-mounted filesystem
    if (up->curBoot) {
        // _daDone populates 'dis'[senter]
        DADiskUnmount(up->curBoot,kDADiskMountOptionDefault,_daDone,&dis);
        if (dis == (void*)kCFNull) {    // DA.Unmount can call _daDone
            CFRunLoopRun();
        }

        // if that didn't work, just log
        if (dis) {
            OSKextLog(NULL, up->warnLogSpec,
                      "%s didn't unmount, leaving mounted", up->bsdname);
            if (dis != (void*)kCFNull) {
                CFRelease(dis);
            }
        }
        up->curMount[0] = '\0';     // only try to unmount once
        CFRelease(up->curBoot);
        up->curBoot = NULL;
    }

    // unmount anything mounted by _mountBuiltIn()
    if (up->dasession == NULL && up->curMount[0] != '\0') {
        if (unmount(up->curMount, 0)) {
            errnum = errno;
        }
        up->curMount[0] = '\0';     // only try to unmount once
    }

    if (errnum) {
        OSKextLog(NULL, up->errLogSpec,
            "Failed to unmount helper (%d/%#x): %s", errnum,
            errnum & ~(err_local|err_local_diskarbitration), strerror(errnum));
    }
}


/******************************************************************************
* ucopyRPS unlinks old/copies new RPS content w/o activating
* RPS files are considered important -- non-zero file sizes only!
* XX could validate the kernel with Mach-o header
* several intervening helpers including eraseRPS()
******************************************************************************/
static int
writeBootPrefs(struct updatingVol *up, char *dstpath)
{
    int         opres, rval = ELAST + 1;
    char        dstparent[PATH_MAX];
    ssize_t     len;
    int         fd = -1;

    // recursively create the parent directory       
    if (strlcpy(dstparent,dirname(dstpath),PATH_MAX) >= PATH_MAX) {
        rval = EOVERFLOW; goto finish;
    }
    opres = sdeepmkdir(up->curbootfd, dstparent, kCacheDirMode);
    if (opres) {
        rval = opres; goto finish;
    }

    // sopen adds O_EXCL to O_CREAT
    (void)sunlink(up->curbootfd, dstpath);
    fd = sopen(up->curbootfd, dstpath, O_WRONLY|O_CREAT, kCacheFileMode);
    if (fd == -1) {
        rval = errno; goto finish;
    }

    len = CFDataGetLength(up->bpdata);
    if (write(fd,CFDataGetBytePtr(up->bpdata),len) != len) {
        rval = errno; goto finish;
    }

    rval = 0;

finish:
    if (rval) {
        LOGERRxlate(up, dstpath, NULL, rval);
    }
    
    if (fd != -1)   close(fd);

    return rval;
}

// correctly erase (hopefully old :) items in the Apple_Boot
static int
eraseRPS(struct updatingVol *up, char *toErase)
{
    int rval = ELAST+1;
    char path[PATH_MAX];
    struct stat sb;

    // if nothing to erase, return cleanly
    if (stat(toErase, &sb) == -1 && errno == ENOENT) {
        rval = 0;
        goto finish;
    }

    if (up->caches->erpropcache->rpath) {
        // pathc*() seed errno
        pathcpy(path, toErase);
        pathcat(path, up->caches->erpropcache->rpath);
        // szerofile() won't complain if it is missing
        if (szerofile(up->curbootfd, path))
            goto finish;
    }

    rval = sdeepunlink(up->curbootfd, toErase);

finish:
    if (rval) {
        OSKextLog(NULL, up->errLogSpec | kOSKextLogFileAccessFlag,
                  "%s - %s. errno %d %s", 
                  __FUNCTION__, toErase, errno, strerror(errno));
    }

    return rval;
}

static int
_writeFDEPropsToHelper(struct updatingVol *up, char *dstpath)
{
    int errnum, rval = ELAST + 1;   // everyone sets it?
    CFDictionaryRef matching;       // IOServiceGetMatchingServices() releases
    io_service_t helper = IO_OBJECT_NULL;
    CFNumberRef unitNum = NULL;
    CFNumberRef partNum = NULL;
    int partnum;
    const void *keys[2], *vals[2];
    CFDictionaryRef props = NULL;
    io_service_t bearer = IO_OBJECT_NULL;
    CFStringRef partType = NULL;
    CFStringRef partBSD = NULL;
    char csbsd[DEVMAXPATHSIZE];

    // callers usually check first
    if (up->onAPM) {
        rval = EINVAL; goto finish;
    }

    // look up helper
    if (!(matching = IOBSDNameMatching(kIOMasterPortDefault, 0, up->bsdname))) {
        rval = ENOMEM; goto finish;
    }
    helper = IOServiceGetMatchingService(kIOMasterPortDefault, matching);
    matching = NULL;        // IOServiceGetMatchingService() released
    if (!helper) {
        rval = ENOENT; goto finish;
    }

    // extract properties, munge them to describe the data partition
    unitNum = (CFNumberRef)IORegistryEntryCreateCFProperty(helper,
                           CFSTR(kIOBSDUnitKey), nil, 0);
    if (!unitNum || CFGetTypeID(unitNum) != CFNumberGetTypeID()) {
        rval = ENODEV; goto finish;
    }
    partNum = (CFNumberRef)IORegistryEntryCreateCFProperty(helper,
                           CFSTR(kIOMediaPartitionIDKey), nil, 0);
    if (!partNum || CFGetTypeID(partNum) != CFNumberGetTypeID()) {
        rval = ENODEV; goto finish;
    }
    CFNumberGetValue(partNum, kCFNumberIntType, &partnum);
    CFRelease(partNum);
    partNum = NULL;
    // in GPT, data the partition comes before the Apple_Boot
    if (--partnum <= 0) {
        rval = ENODEV; goto finish;
    }
    partNum = CFNumberCreate(nil, kCFNumberIntType, &partnum);
    if (!partNum) {
        rval = ENOMEM; goto finish;
    }

    // create property and matching dictionaries
    keys[0] = CFSTR(kIOMediaPartitionIDKey);
    vals[0] = partNum;
    keys[1] = CFSTR(kIOBSDUnitKey);
    vals[1] = unitNum;
    if (!(props = CFDictionaryCreate(nil, keys, vals, 2,
                                  &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks))) {
        rval = ENOMEM; goto finish;
    }
    keys[0] = CFSTR(kIOProviderClassKey);
    vals[0] = CFSTR(kIOMediaClass);
    keys[1] = CFSTR(kIOPropertyMatchKey);
    vals[1] = props;
    if (!(matching = CFDictionaryCreate(nil, keys, vals, 2,
                                  &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks))) {
        rval = ENOMEM; goto finish;
    }

    // look up the data partition!
    bearer = IOServiceGetMatchingService(kIOMasterPortDefault, matching);
    matching = NULL;        // IOServiceGetMatchingService() released
    if (!bearer) {
        rval = ENOENT; goto finish;
    }

    // the data partition's type must be Apple_CoreStorage
    partType = (CFStringRef)IORegistryEntryCreateCFProperty(bearer,
                            CFSTR(kIOMediaContentKey), nil, 0);
    if (!partType || CFGetTypeID(partType) != CFStringGetTypeID()) {
        rval = ENODEV; goto finish;
    }
    if (!CFEqual(partType, CFSTR(APPLE_CORESTORAGE_UUID))) {
        rval = ENODEV; goto finish;
    }

    // extract BSD Name
    partBSD = (CFStringRef)IORegistryEntryCreateCFProperty(bearer,
                           CFSTR(kIOBSDNameKey), nil, 0);
    if (!partBSD || CFGetTypeID(partBSD) != CFStringGetTypeID()) {
        rval = ENODEV; goto finish;
    }
    if (!CFStringGetFileSystemRepresentation(partBSD, csbsd, DEVMAXPATHSIZE)){
        rval = EOVERFLOW; goto finish;
    }

    // and finally write the encryption context data to the Apple_Boot
    // encrypted with the wipe key for the Apple_CoreStorage.
    if ((errnum=writeCSFDEProps(up->curbootfd,up->csfdeprops,csbsd,dstpath))){
        rval = errnum; goto finish;
    }

    // success!
    rval = 0;

finish:
    if (partBSD)                    CFRelease(partBSD);
    if (partType)                   CFRelease(partType);
    if (bearer != IO_OBJECT_NULL)   IOObjectRelease(bearer);
    if (props)                      CFRelease(props);
    if (partNum)                    CFRelease(partNum);
    if (unitNum)                    CFRelease(unitNum);
    if (helper != IO_OBJECT_NULL)   IOObjectRelease(helper);

    return rval;
}


/* 
 * ucopyRPS - copy new RPS directory to "inactive" location
 * bails on any error because only a whole RPS dir makes sense
 */
static int
ucopyRPS(struct updatingVol *up)
{
    int bsderr, rval = ELAST + 1;   // generic safest
    char prevRPS[PATH_MAX], curRPS[PATH_MAX], discard[PATH_MAX];
    char *erdir;
    unsigned i;
    char srcpath[PATH_MAX], dstpath[PATH_MAX];
    COMPILE_TIME_ASSERT(sizeof(BOOTPLIST_NAME)==sizeof(BOOTPLIST_APM_NAME));
    
    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Copying files used by the booter.");

    bsderr = FindRPSDir(up, prevRPS, curRPS, discard);
    if (bsderr) {
        rval = bsderr; goto finish;
    }

    if (up->flatTarget[0]) {
        // copy desired target into dstdir
        pathcpy(up->dstdir, up->curMount);
        pathcat(up->dstdir, up->flatTarget);
        erdir = curRPS;
    } else {
        // we're going to copy into the currently-inactive directory
        pathcpy(up->dstdir, prevRPS);
        erdir = prevRPS;
    }

    // we expect to have removed it and eraseRPS() doesn't mind it missing
    if ((bsderr = eraseRPS(up, up->dstdir))) {
        rval = bsderr; goto finish;
    }

    // create the directory (RPS should not exist?)
    if ((bsderr = sdeepmkdir(up->curbootfd, up->dstdir, kCacheDirMode))) {
        LOGERRxlate(up, "mkdir() failed: ", up->dstdir, bsderr);
        rval = bsderr; goto finish;
    }

    // and loop
    for (i = 0; i < up->caches->nrps; i++) {
        cachedPath *curItem = &up->caches->rpspaths[i];

        pathcpy(srcpath, up->caches->root);
        pathcat(srcpath, curItem->rpath);
        pathcpy(dstpath, up->dstdir);
        // EfiLoginUI.a still digs down to its cache dirs
        if (up->flatTarget[0] && curItem != up->caches->efidefrsrcs
                             && curItem != up->caches->efiloccache) {
            /* XX 10561671: basename unsafe */
            pathcat(dstpath, "/");
            pathcat(dstpath, basename(curItem->rpath));
        } else {
            pathcat(dstpath, curItem->rpath);
        }

        // check for special files; first Boot.plist
        if (curItem == up->caches->bootconfig) {
            // PR-5115900 - call it com.apple.boot.plist on APM since Tiger
            // (since Tiger bless scribbles on com.apple.Boot.plist)
            if (up->onAPM) {
                char * plistNamePtr;
                // see assert above
                plistNamePtr = strstr(dstpath, BOOTPLIST_NAME);
                if (plistNamePtr) {
                    strncpy(plistNamePtr, BOOTPLIST_APM_NAME, strlen(BOOTPLIST_NAME));
                }
            }
            // write customized com.apple.Boot.plist data
            if ((bsderr = writeBootPrefs(up, dstpath))) {
                rval = bsderr; goto finish;
            }
        } else {
            // could deny zero-size cookies, busted Mach-O, etc here
            // scopyitem creates any intermediate directories
            bsderr=scopyitem(up->caches->cachefd,srcpath,up->curbootfd,dstpath);
            if (bsderr) {
                // erpropcache, efiloccache are optional
                if ((curItem == up->caches->erpropcache ||
                            curItem == up->caches->efiloccache)
                        && bsderr == -1 && errno == ENOENT) {
                    ; // no-op to allow real CSFDE data to be written
                } else {
                    OSKextLog(NULL, up->errLogSpec,
                              "Error copying %s to %s",
                              srcpath, dstpath);
                    rval = bsderr; goto finish;
                }
            }

            // having copied any existing file (for HFS conversions),
            // we now prefer the real data
            if (up->csfdeprops && curItem == up->caches->erpropcache &&
                    up->onAPM == false) {
                if ((bsderr = _writeFDEPropsToHelper(up, dstpath))) {
                    rval = bsderr; goto finish;
                }
            }
        }
    }

    // XX EFI is happier if there is a SystemVersion.plist it can find

    // 10561691 wasn't fixed until 10.8 so we implement "mostly flat"
    // for 10.7-era systems when flatTarget is set.
    // re-write correctly-encrypted context to secondary location
    if (up->flatTarget[0] && up->caches->erpropTSOnly == false &&
            up->onAPM == false && up->caches->erpropcache && up->csfdeprops) {
        pathcpy(dstpath, erdir);
        pathcat(dstpath, up->caches->erpropcache->rpath);
        if ((bsderr = _writeFDEPropsToHelper(up, dstpath))) {
            rval = bsderr; goto finish;
        }

        if (up->caches->efidefrsrcs) {
            pathcpy(srcpath, up->caches->root);
            pathcat(srcpath, up->caches->efidefrsrcs->rpath);
            pathcpy(dstpath, erdir);
            pathcat(dstpath, up->caches->efidefrsrcs->rpath);
            bsderr=scopyitem(up->caches->cachefd,srcpath,up->curbootfd,dstpath);
            if (bsderr) {
                rval = bsderr; goto finish;
            }
        }
    }

    // success
    rval = 0;

finish:
    if (rval) {
        LOGERRxlate(up,__FUNCTION__,": copying files for booter",rval);
    }

    return rval;
}

/******************************************************************************
* ucopyMisc writes misc files to .new (inactive) name
******************************************************************************/
static int
ucopyMisc(struct updatingVol *up)
{
    int bsderr, rval = -1;
    unsigned i, nprocessed = 0;
    char srcpath[PATH_MAX], dstpath[PATH_MAX];
    struct stat sb;

    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Copying files read before the booter runs.");
   
    for (i = 0; i < up->caches->nmisc; i++) {
        pathcpy(srcpath, up->caches->root);
        pathcat(srcpath, up->caches->miscpaths[i].rpath);
        makebootpath(dstpath, up->caches->miscpaths[i].rpath);
        pathcat(dstpath, ".new");

        if (stat(srcpath, &sb) == 0) { 
            // file exists and is accessible
            if ((bsderr = scopyitem(up->caches->cachefd, srcpath,
                                    up->curbootfd, dstpath))) {
                OSKextLog(NULL, up->errLogSpec, 
                          "Error copying %s to %s: %s",
                          srcpath, dstpath, strerror(bsderr));
                continue;
            }
        } else if (errno != ENOENT) {
            continue;
        }

        nprocessed++;
    }

    if (nprocessed == i) {
        rval = 0;
    } else {
        rval = errno;
    }

finish:
    if (rval) {
        LOGERRxlate(up, __FUNCTION__, "failure copying pre-booter files", rval);
    }

    return rval;
}

/******************************************************************************
* moveLabels() deactivates the but preserves it for later
* activateMisc() will move these back if needed
* no label -> hint of indeterminate state (label key in plist/other file??)
* XX put/switch in some sort of "(updating!)" label (see BL[ess] routines)
******************************************************************************/
static int moveLabels(struct updatingVol *up)
{
    int rval = -1;
    char path[PATH_MAX];
    struct stat sb;
    int fd = -1;
    
    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Moving aside old label.");

    // pathc*() seed errno
    makebootpath(path, up->caches->label->rpath);
    if (0 == (stat(path, &sb))) {
        char newpath[PATH_MAX];
        unsigned char nulltype[32] = {'\0', };

        // rename
        pathcpy(newpath, path);
        pathcat(newpath, NEWEXT);
        rval = srename(up->curbootfd, path, newpath);
        if (rval)       goto finish;

        // remove magic type/creator
        if (-1 == (fd=sopen(up->curbootfd, newpath, O_RDWR, 0)))  goto finish;
        if(fsetxattr(fd,XATTR_FINDERINFO_NAME,&nulltype,sizeof(nulltype),0,0)) {
            goto finish;
        }
    } 

    up->changestate = noLabel;
    rval = 0;

finish:
    if (fd != -1)   close(fd);

    if (rval) {
        OSKextLog(NULL, up->errLogSpec,
                  "%s - Error moving aside old label. errno %d %s.", 
                  __FUNCTION__, errno, strerror(errno));
   }

    return rval;
}

/******************************************************************************
* nukeBRLabels gets rid of the label and .contentDetails files
* no label -> hint of indeterminate state (label key in plist/other file?)
* someday: some sort of "(updating!)" label?
******************************************************************************/
static int
nukeBRLabels(struct updatingVol *up)
{
    int rval = EOVERFLOW;       // path*()
    int opres, firstErrno, firstErr = 0;
    char labelp[PATH_MAX], dstparent[PATH_MAX];
    struct stat sb;
    
    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Removing current disk label.");

    // .disk_label
    makebootpath(labelp, up->caches->label->rpath);
    if (0 == (stat(labelp, &sb))) {
        opres = sunlink(up->curbootfd, labelp);
        RECERR(up, opres, "error removing label" /*NULL w/9217695*/);
    } else {
        errno = 0;
    }

    // .disk_label_2x
    pathcpy(labelp, up->curMount);
    pathcat(labelp, up->caches->label->rpath);
    pathcat(labelp, SCALE_2xEXT);       // append extension
    if (0 == (stat(labelp, &sb))) {
        opres = sunlink(up->curbootfd, labelp);
        RECERR(up, opres, "error removing .contentDetails" /*NULL w/9217695*/);
    } else {
        errno = 0;
    }

    // .disk_label.contentsDetail
    pathcpy(labelp, up->curMount);
    pathcat(labelp, up->caches->label->rpath);
    pathcat(labelp, CONTENTEXT);        // append extension
    if (0 == (stat(labelp, &sb))) {
        opres = sunlink(up->curbootfd, labelp);
        RECERR(up, opres, "error removing " CONTENTEXT /*NULL w/9217695*/);
    } else {
        errno = 0;
    }

    // and possible .root_uuid
    makebootpath(labelp, up->caches->label->rpath);
    pathcpy(dstparent, dirname(labelp));
    pathcpy(labelp, dstparent);
    pathcat(labelp, "/" kBRRootUUIDFile);
    if (0 == (stat(labelp, &sb))) {
        opres = sunlink(up->curbootfd, labelp);
        RECERR(up, opres, "error removing " kBRRootUUIDFile /*NULL w/9217695*/);
    } else {
        errno = 0;
    }

    up->changestate = noLabel;

    if (firstErr == -1)     errno = firstErrno;
    rval = firstErr;

finish:
    if (rval)
        OSKextLog(NULL, kOSKextLogErrorLevel, "Error removing disk label.");

    return rval;
}

/******************************************************************************
* ucopyBooters unlink/copies down booters but doesn't bless them
******************************************************************************/
static int ucopyBooters(struct updatingVol *up)
{
    int rval = ELAST + 1;
    int bsderr;
    char srcpath[PATH_MAX], oldpath[PATH_MAX];
    int nbooters = 0;

    if (up->caches->ofbooter.rpath[0])      nbooters++;
    if (up->caches->efibooter.rpath[0])     nbooters++;
    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Copying new booter%s.", nbooters == 1 ? "" : "s");
    
    // copy BootX, boot.efi
    up->changestate = copyingOFBooter;
    if (up->caches->ofbooter.rpath[0]) {
        // pathc*() seed errno
        pathcpy(srcpath, up->caches->root);
        pathcat(srcpath, up->caches->ofbooter.rpath);   // <root>/S/L/CS/BootX
        makebootpath(up->ofdst, up->caches->ofbooter.rpath); // <boot>/../BootX
        pathcpy(oldpath, up->ofdst);
        pathcat(oldpath, OLDEXT);                  // <boot>/S/L/CS/BootX.old

        (void)sunlink(up->curbootfd, oldpath);
        bsderr = srename(up->curbootfd, up->ofdst, oldpath);
        if (bsderr && errno !=ENOENT) {
            OSKextLog(NULL, up->errLogSpec, 
                      "%s - Error rename old %s new %s",
                      __FUNCTION__, up->ofdst, oldpath);
            goto finish;
        }
        if ((bsderr = scopyitem(up->caches->cachefd, srcpath,
                                up->curbootfd, up->ofdst))) {
            if (up->helpersOptional == false) {
                OSKextLog(NULL, up->errLogSpec, "%s - Error copying %s to %s",
                          __FUNCTION__, srcpath, up->ofdst);
            }
            goto finish;
        }
    }

    up->changestate = copyingEFIBooter;
    if (up->caches->efibooter.rpath[0]) {
        // pathc*() seed errno
        pathcpy(srcpath, up->caches->root);
        pathcat(srcpath, up->caches->efibooter.rpath);   // ... boot.efi
        makebootpath(up->efidst, up->caches->efibooter.rpath);
        pathcpy(oldpath, up->efidst);
        pathcat(oldpath, OLDEXT);

        (void)sunlink(up->curbootfd, oldpath);
        bsderr = srename(up->curbootfd, up->efidst, oldpath);
        if (bsderr && errno != ENOENT) {
            OSKextLog(NULL, up->errLogSpec, 
                      "%s - Error rename old %s new %s",
                      __FUNCTION__, up->efidst, oldpath);
            goto finish;
        }
        if ((bsderr = scopyitem(up->caches->cachefd, srcpath,
                                up->curbootfd, up->efidst))) {
            if (up->helpersOptional == false) {
                OSKextLog(NULL, up->errLogSpec, "%s - Error copying %s to %s",
                          __FUNCTION__, srcpath, up->efidst);
            }
            goto finish;
        }
    }

    up->changestate = copiedBooters;
    rval = 0;

finish:
    // all goto paths log

    return rval;
}


// booters have worst critical:fragile ratio (basically point of no return)
/******************************************************************************
* bless recently-copied booters
* operatens entirely on up->??dst which allows revertState to use it ..?
******************************************************************************/
#define CLOSE(fd) do { (void)close(fd); fd = -1; } while(0)
static int
activateBooters(struct updatingVol *up)
{
    int errnum, rval = ELAST + 1;
    int fd = -1;
    uint32_t vinfo[8] = { 0, };
    struct stat sb;
    char parent[PATH_MAX];
    int nbooters = 0;

    if (up->caches->ofbooter.rpath[0])      nbooters++;   
    if (up->caches->efibooter.rpath[0])     nbooters++;    

    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Activating new booter%s.", nbooters == 1 ? "" : "s");

    // flush everything in this helper partition to disk
    if ((errnum = fcntl(up->curbootfd, F_FULLFSYNC))) {
        rval = errnum; goto finish;
    }
    
    // activate BootX, boot.efi
    up->changestate = activatingOFBooter;
    if (up->caches->ofbooter.rpath[0]) {
        unsigned char tbxichrp[32] = {'t','b','x','i','c','h','r','p','\0',};

        // apply type/creator (assuming same folder as previous, now active)
        if (-1==(fd=sopen(up->curbootfd, up->ofdst, O_RDONLY, 0))) {
            rval = errno; goto finish;
        }
        if(fsetxattr(fd,XATTR_FINDERINFO_NAME,&tbxichrp,sizeof(tbxichrp),0,0)){
            rval = errno; goto finish;
        }
        CLOSE(fd);

        // get fileID of booter's enclosing folder 
        pathcpy(parent, dirname(up->ofdst));
        if (-1 == (fd=sopen(up->curbootfd, parent, O_RDONLY, 0))
                    || fstat(fd, &sb)) {
            rval = errno; goto finish;
        }
        CLOSE(fd);
        if (sb.st_ino < (__darwin_ino64_t)2<<31) {
            vinfo[kSystemFolderIdx] = (uint32_t)sb.st_ino;
        } else {
            rval = EOVERFLOW; goto finish;
        }
    }

    up->changestate = activatingEFIBooter;
    if (up->caches->efibooter.rpath[0]) {
        // get file ID
        if (-1==(fd=sopen(up->curbootfd, up->efidst, O_RDONLY, 0))
                    || fstat(fd, &sb)) {
            rval = errno; goto finish;
        }
        CLOSE(fd);
        if (sb.st_ino < (__darwin_ino64_t)2<<31) {
            vinfo[kEFIBooterIdx] = (uint32_t)sb.st_ino;
        } else {
            rval = EOVERFLOW; goto finish;
        }

        // get folder ID of enclosing folder if not provided by ofbooter
        if (!vinfo[0]) {
            pathcpy(parent, dirname(up->efidst));
            if (-1 == (fd=sopen(up->curbootfd, parent, O_RDONLY, 0)) 
                    || fstat(fd, &sb)) {
                rval = errno; goto finish;
            }
            CLOSE(fd);
            if (sb.st_ino < (__darwin_ino64_t)2<<31) {
                vinfo[kSystemFolderIdx] = (uint32_t)sb.st_ino;
            } else {
                rval = EOVERFLOW; goto finish;
            }
        }
    }

    // Only FS-bless if requested.  We could skip gathering the data, but we
    // also made sure the OF booter got its tbxi/chrp, etc.
    if (!up->skipHelperBless) {
        if ((errnum = sBLSetBootFinderInfo(up, vinfo))) {
            rval = errnum; goto finish;    
        }
    }
    
    up->changestate = activatedBooters;

    // success
    rval = 0;

finish:
    if (fd != -1)   close(fd);

    if (rval)
        OSKextLog(NULL, kOSKextLogErrorLevel, "Error activating booter.");

    return rval;
}

/******************************************************************************
* leap-frog w/rename()
******************************************************************************/
static int activateRPS(struct updatingVol *up)
{
    int rval = ELAST + 1;
    char prevRPS[PATH_MAX], curRPS[PATH_MAX], nextRPS[PATH_MAX];
    
    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Activating files used by the booter.");

    // if using default RPS dirs, make fresh one current
    if (up->flatTarget[0] == '\0') {
        if (FindRPSDir(up, prevRPS, curRPS, nextRPS))   goto finish;

        // if current != the one we just populated
        if (strncmp(curRPS, up->dstdir, PATH_MAX) != 0) {
            // rename prev -> next ... done!?
            if (srename(up->curbootfd, prevRPS, nextRPS))   goto finish;
        }
    }

    // thwunk everything to disk (now that essential boot files are in place)
    if (fcntl(up->curbootfd, F_FULLFSYNC))              goto finish;

    rval = 0;

finish:
    if (rval) {
        OSKextLog(NULL, kOSKextLogErrorLevel,
              "Error activating files used by the booter.");
    }

    return rval;
}

// see makebootpath() at top of file
#define MAKEBOOTPATHcont(path, rpath) do { \
                                    PATHCPYcont(path, up->curMount); \
                                    if (up->flatTarget[0]) { \
                                        PATHCATcont(path, up->flatTarget); \
                                        /* XXX 10561671: basename unsafe */ \
                                        PATHCATcont(path, "/"); \
                                        PATHCATcont(path, basename(rpath)); \
                                    } else { \
                                        PATHCATcont(path, rpath); \
                                    } \
                                } while(0)
static int activateMisc(struct updatingVol *up)     // rename the .new
{
    int rval = ELAST + 1;
    char path[PATH_MAX], opath[PATH_MAX];
    unsigned i = 0, nprocessed = 0;
    int fd = -1;
    struct stat sb;
    unsigned char tbxjchrp[32] = { 't','b','x','j','c','h','r','p','\0', };
   
    if (up->doMisc) {
        OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
                  "Activating files used before the booter runs.");
        
        // do them all
        for (i = 0; i < up->caches->nmisc; i++) {
            MAKEBOOTPATHcont(path, up->caches->miscpaths[i].rpath);
            if (strlcpy(opath, path, PATH_MAX) >= PATH_MAX)     continue;
            if (strlcat(opath, NEWEXT, PATH_MAX) >= PATH_MAX)   continue;

            if (stat(opath, &sb) == 0) {
                if (srename(up->curbootfd, opath, path))        continue;
            }

            nprocessed++;
        }

    }

    makebootpath(path, up->caches->label->rpath);
        // move label back
        char newpath[PATH_MAX];

        pathcpy(newpath, path);     // just rename
        pathcat(newpath, NEWEXT);
        (void)srename(up->curbootfd, newpath, path);
    }

    // assign type/creator to the label
    if (0 == (stat(path, &sb))) {
        if (-1 == (fd = sopen(up->curbootfd, path, O_RDWR, 0)))   goto finish;

        if (fsetxattr(fd,XATTR_FINDERINFO_NAME,&tbxjchrp,sizeof(tbxjchrp),0,0))
            goto finish;
        close(fd); fd = -1;
    }

    rval = (i != nprocessed);

finish:
    if (fd != -1)   close(fd);

    if (rval) {
        OSKextLog(NULL, kOSKextLogErrorLevel,
                  "Error activating files used before the booter runs.");
    }

    return rval;
}

/******************************************************************************
* get rid of everything "extra"
******************************************************************************/
static int nukeFallbacks(struct updatingVol *up)
{
    int rval = 0;               // OR-ative return value
    int bsderr;
    char delpath[PATH_MAX];
    struct bootCaches *caches = up->caches;

    OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
              "Cleaning up fallbacks.");

    // using pathcpy b/c if that's failing, it's worth bailing
    // XX should probably only try to unlink if present

    // maybe mount failed (in which case there aren't any fallbacks)
    if (up->curMount[0] == '\0')    goto finish;

    // if needed, unlink .old booters
    if (up->doBooters) {
        if (caches->ofbooter.rpath[0]) {
            makebootpath(delpath, caches->ofbooter.rpath);
            pathcat(delpath, OLDEXT);
            if ((bsderr = sunlink(up->curbootfd, delpath)) && errno != ENOENT) {
                rval |= bsderr;
            }
        }
        if (caches->efibooter.rpath[0]) {
            makebootpath(delpath, caches->efibooter.rpath);
            pathcat(delpath, OLDEXT);
            if ((bsderr = sunlink(up->curbootfd, delpath)) && errno != ENOENT) {
                rval |= bsderr;
            }
        }
    }

    // if needed, erase prevRPS
    // which, conveniently, will be right regardless of whether we succeeded
    if (up->doRPS) {
        char ignore[PATH_MAX];

        if (0 == FindRPSDir(up, delpath, ignore, ignore)) {
            // eraseRPS ignores if missing
            rval |= eraseRPS(up, delpath);
        }
    }

finish:
    if (rval)
        OSKextLog(NULL, kOSKextLogErrorLevel, "Error cleaning up fallbacks.");

    return rval;
}

/*********************************************************************
// XXX not yet used / tested
*********************************************************************/
static int kill_kextd(void)
{
    int           result         = -1;
    kern_return_t kern_result    = kOSReturnError;
    mach_port_t   bootstrap_port = MACH_PORT_NULL;
    mach_port_t   kextd_port     = MACH_PORT_NULL;
    int           kextd_pid      = -1;

    kern_result = task_get_bootstrap_port(mach_task_self(), &bootstrap_port);
    if (kern_result != kOSReturnSuccess) {
        goto finish;
    }

    kern_result = bootstrap_look_up(bootstrap_port,
        (char *)KEXTD_SERVER_NAME, &kextd_port);
    if (kern_result != kOSReturnSuccess) {
        goto finish;
    }

    kern_result = pid_for_task(kextd_port, &kextd_pid);
    if (kern_result != kOSReturnSuccess) {
        goto finish;
    }

    result = kill(kextd_pid, SIGKILL);
    if (-1 == result) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
             "kill kextd failed - %s.", strerror(errno));
    }

finish:
    if (kern_result != kOSReturnSuccess) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
             "kill kextd failed - %s.", safe_mach_error_string(kern_result));
    }
    return result;
}

/******************************************************************************
// XXX not yet used / tested  
******************************************************************************/
int renameBootcachesPlist(
    char * hostVolume,
    char * oldPlistPath,
    char * newPlistPath)
{
    int    result            = -1;
    int    bootcachesPlistFd = -1;
    char * errorMessage      = NULL;
    char * errorPath         = NULL;
    char   oldname[PATH_MAX];
    char   newname[PATH_MAX];
    char * kextcacheArgs[] = {
        "/usr/sbin/kextcache",
        "-f",
        "-u",
        NULL, // replace with hostVolume
        NULL };

    errorMessage = "path concatenation error";
    errorPath = hostVolume;

    if (strlcpy(oldname, hostVolume, PATH_MAX) >= PATH_MAX) {
        goto finish;
    }
    if (strlcpy(newname, hostVolume, PATH_MAX) >= PATH_MAX) {
        goto finish;
    }

    errorPath = oldPlistPath;
    if (strlcpy(oldname, oldPlistPath, PATH_MAX) >= PATH_MAX) {
        goto finish;
    }

    errorPath = newPlistPath;
    if (strlcpy(newname, newPlistPath, PATH_MAX) >= PATH_MAX) {
        goto finish;
    }

    errorPath = oldname;
    bootcachesPlistFd = open(oldname, O_RDONLY);
    if (-1 == bootcachesPlistFd) {
        errorMessage = strerror(errno);
        goto finish;
    }
    
    if (-1 == srename(bootcachesPlistFd, oldname, newname)) {
        errorMessage = "rename failed.";
        goto finish;
    }
    
    errorMessage = "couldn't kill kextd";
    if (-1 == kill_kextd()) {
        goto finish;
    }

   /* Do we want to check fork_program's return value?
    */
    kextcacheArgs[3] = hostVolume;
    result = fork_program(kextcacheArgs[0], kextcacheArgs, true /* wait */);

finish:
    if (errorMessage) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
             "%s - %s.", errorPath, errorMessage);
    }
    if (bootcachesPlistFd >= 0) {
        close(bootcachesPlistFd);
    }
    return result;
}

/******************************************************************************
* takeVolumeForPath turns the path into a volume UUID and locks with kextd
******************************************************************************/
// upstat() stat()s "up" the path if a file doesn't exist
static int upstat(const char *path, struct stat *sb, struct statfs *sfs)
{
    int rval = ELAST+1;
    char buf[PATH_MAX], *tpath = buf;
    struct stat defaultsb;

    if (strlcpy(buf, path, PATH_MAX) > PATH_MAX)        goto finish;

    if (!sb)    sb = &defaultsb;
    while ((rval = stat(tpath, sb)) == -1 && errno == ENOENT) {
        // "." and "/" should always exist, but you never know
        if (tpath[0] == '.' && tpath[1] == '\0')  goto finish;
        if (tpath[0] == '/' && tpath[1] == '\0')  goto finish;
        tpath = dirname(tpath);     // Tiger's dirname() took const char*
    }

    // call statfs if the caller needed it
    if (sfs)
        rval = statfs(tpath, sfs);

finish:
    if (rval) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogWarningLevel | kOSKextLogFileAccessFlag,
                "Couldn't find volume for %s.", path);
    }

    return rval;
}


/******************************************************************************
* theoretically, takeVolumeForPaths() ensured all paths are on the given
* volume, then locked
******************************************************************************/
// int takeVolumeForPaths(char *volPath)

/******************************************************************************
* takeVolumeForPath() is all we ended up needing ...
* can return success if a lock isn't needed
* can return failure if sBRUptLock is already in use
******************************************************************************/
#define WAITFORLOCK 1
int
takeVolumeForPath(const char *path)
{
    int rval = ELAST + 1;
    kern_return_t macherr = KERN_SUCCESS;
    int lckres = 0;
    struct statfs sfs;
    const char *volPath = "<unknown>";  // llvm can't track lckres/macherr
    mach_port_t taskport = MACH_PORT_NULL;

    if (sBRUptLock) {
        return EALREADY;        // only support one lock at a time
    }

    if (geteuid() != 0) {
        // kextd shouldn't be watching anything you can touch
        // and ignores locking requests from non-root anyway
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                 "Warning: non-root can't lock the volume for %s", path);
        rval = 0;
        goto finish;
    }

    // look up kextd port if not cached
    // XX if there's a way to know kextd isn't already running, we could skip
    // unnecessarily bringing it up in the boot-time case (see 5108882).
    if (!sKextdPort) {
        macherr=bootstrap_look_up(bootstrap_port,KEXTD_SERVER_NAME,&sKextdPort);
        if (macherr)  goto finish;
    }

    // get the volume's UUID
    if ((rval = upstat(path, NULL, &sfs)))      goto finish;
    volPath = sfs.f_mntonname;
    if ((rval = copyVolumeInfo(volPath,&s_vol_uuid,NULL,NULL,NULL))) {
        goto finish;
    }
    
    // allocate a port to pass (in case we die -- kernel cleans up on exit())
    taskport = mach_task_self();
    if (taskport == MACH_PORT_NULL)  goto finish;
    macherr = mach_port_allocate(taskport,MACH_PORT_RIGHT_RECEIVE,&sBRUptLock);
    if (macherr)  goto finish;

    // try to take the lock; warn if it's busy and then wait for it
    // X kextcache -U, if it is going to lock at all, needs only WAITFORLOCK
    macherr = kextmanager_lock_volume(sKextdPort, sBRUptLock, s_vol_uuid,
                                      !WAITFORLOCK, &lckres);
    if (macherr)        goto finish;

    // 5519500: sleep until kextd is up and running (w/diskarb, etc)
    while (lckres == EAGAIN) {
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
            "kextd wasn't ready; waiting 10 seconds and trying again.");
        sleep(10);
        macherr = kextmanager_lock_volume(sKextdPort, sBRUptLock, s_vol_uuid,
                                          !WAITFORLOCK, &lckres);
        if (macherr)    goto finish;
    }

    // With kextd set up, we sleep until the volume is free.
    // WAITFORLOCK should cause the function not to return w/o the lock
    // but 8679674 suggested something was going awry so we'll retry.
    while (lckres == EBUSY) {
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
            "%s locked; waiting for lock.", volPath);
        macherr = kextmanager_lock_volume(sKextdPort, sBRUptLock, s_vol_uuid,
                                          WAITFORLOCK, &lckres);
        if (macherr)    goto finish;
        if (lckres == 0) {
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                "Lock acquired; proceeding.");
        }
    }

    
    // kextd might not be watching this volume (isn't currently competing)
    // so we set our success to the existance of the volume's root
    if (lckres == ENOENT) {
        struct stat sb;
        rval = stat(volPath, &sb);
        if (rval == 0) {
            OSKextLog(NULL, kOSKextLogProgressLevel | kOSKextLogIPCFlag,
                "Note: kextd not watching %s; proceeding w/o lock", volPath);
        }
    } else {
        rval = lckres;
    }

finish: 
    if (sBRUptLock != MACH_PORT_NULL && (lckres != 0 || macherr)) {
        mach_port_mod_refs(taskport, sBRUptLock, MACH_PORT_RIGHT_RECEIVE, -1);
        sBRUptLock = MACH_PORT_NULL;
    }

    /* XX needs unraveling XX */
    // if kextd isn't competing with us, then we didn't need the lock
    if (macherr == BOOTSTRAP_UNKNOWN_SERVICE ||
            macherr == MACH_SEND_INVALID_DEST) {
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
            "Warning: kextd unavailable; proceeding w/o lock for %s", volPath);
        rval = 0;
    } else if (macherr) {
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
            "Couldn't lock %s: %s (%x).", path,
            safe_mach_error_string(macherr), macherr);
        rval = macherr;
    } else {
        // dump rval
        if (rval == -1) {
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                "Couldn't lock %s.", path);
            rval = errno;
        } else if (rval) {
            // lckres == EAGAIN should get here
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                "Couldn't lock %s: %s", path, strerror(rval));
        }
    }

    return rval;
}

/******************************************************************************
* putVolumeForPath will unlock the relevant volume, passing 'status' to
* inform kextd whether we succeded, failed, or just need more time
******************************************************************************/
int putVolumeForPath(const char *path, int status)
{
    int rval = KERN_SUCCESS;

    // if not locked, don't sweat it
    if (sBRUptLock == MACH_PORT_NULL)
        goto finish;

    rval = kextmanager_unlock_volume(sKextdPort,sBRUptLock,s_vol_uuid,status);

    // tidy up; the server will clean up its stuff if we die prematurely
    mach_port_mod_refs(mach_task_self(),sBRUptLock,MACH_PORT_RIGHT_RECEIVE,-1);
    sBRUptLock = MACH_PORT_NULL;

finish:
    if (rval) {
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
            "Couldn't unlock volume for %s: %s (%d).",
            path, safe_mach_error_string(rval), rval);
    }

    return rval;
}
