/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2020 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU Lesser General Public License, as published by the Free Software
 * Foundation; either version 2.1 of the License, or (at your option) any
 * later version. Please see the file LICENSE-LGPL for details.
 *
 * Web Page: http://brltty.app/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

#include "prologue.h"

#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif /* HAVE_SYS_PRCTL_H */

#include "log.h"
#include "strfmt.h"
#include "pgmprivs.h"
#include "system_linux.h"
#include "file.h"

static int
amPrivilegedUser (void) {
  return !geteuid();
}

static int
setProcessOwnership (uid_t uid, gid_t gid) {
  gid_t oldRgid, oldEgid, oldSgid;

  if (getresgid(&oldRgid, &oldEgid, &oldSgid) != -1) {
    if (setresgid(gid, gid, gid) != -1) {
      if (setresuid(uid, uid, uid) != -1) {
        return 1;
      } else {
        logSystemError("setresuid");
      }

      setresgid(oldRgid, oldEgid, oldSgid);
    } else {
      logSystemError("setresgid");
    }
  } else {
    logSystemError("getresgid");
  }

  return 0;
}

typedef struct {
  const char *reason;
  int (*install) (void);
} KernelModuleEntry;

static const KernelModuleEntry kernelModuleTable[] = {
  { .reason = "for playing alert tunes via the built-in PC speaker",
    .install = installSpeakerModule,
  },

  { .reason = "for creating virtual devices",
    .install = installUinputModule,
  },
}; static const unsigned char kernelModuleCount = ARRAY_COUNT(kernelModuleTable);

static void
installKernelModules (void) {
  const KernelModuleEntry *kme = kernelModuleTable;
  const KernelModuleEntry *end = kme + kernelModuleCount;

  while (kme < end) {
    kme->install();
    kme += 1;
  }
}

#ifdef HAVE_GRP_H
#include <grp.h>

typedef struct {
  const char *message;
  const gid_t *groups;
  size_t count;
} GroupsLogData;

static size_t
groupsLogFormatter (char *buffer, size_t size, const void *data) {
  const GroupsLogData *gld = data;

  size_t length;
  STR_BEGIN(buffer, size);
  STR_PRINTF("%s:", gld->message);

  const gid_t *gid = gld->groups;
  const gid_t *end = gid + gld->count;

  while (gid < end) {
    STR_PRINTF(" %d", *gid);

    const struct group *grp = getgrgid(*gid);
    if (grp) STR_PRINTF("(%s)", grp->gr_name);

    gid += 1;
  }

  length = STR_LENGTH;
  STR_END;
  return length;
}

static void
logGroups (int level, const char *message, const gid_t *groups, size_t count) {
  GroupsLogData gld = {
    .message = message,
    .groups = groups,
    .count = count
  };

  logData(level, groupsLogFormatter, &gld);
}

static void
logGroup (int level, const char *message, gid_t group) {
  logGroups(level, message, &group, 1);
}

typedef struct {
  const char *reason;
  const char *name;
  const char *path;
  unsigned char needRead:1;
  unsigned char needWrite:1;
} RequiredGroupEntry;

static const RequiredGroupEntry requiredGroupTable[] = {
  { .reason = "for reading screen content",
    .name = "tty",
    .path = "/dev/vcs1",
  },

  { .reason = "for virtual console monitoring and control",
    .name = "tty",
    .path = "/dev/tty1",
  },

  { .reason = "for serial I/O",
    .name = "dialout",
    .path = "/dev/ttyS0",
  },

  { .reason = "for USB I/O via USBFS",
    .path = "/dev/bus/usb",
  },

  { .reason = "for playing sound via the ALSA framework",
    .name = "audio",
    .path = "/dev/snd/seq",
  },

  { .reason = "for playing sound via the Pulse Audio daemon",
    .name = "pulse-access",
  },

  { .reason = "for monitoring keyboard input",
    .name = "input",
    .path = "/dev/input/mice",
  },

  { .reason = "for creating virtual devices",
    .path = "/dev/uinput",
    .needRead = 1,
    .needWrite = 1,
  },
}; static const unsigned char requiredGroupCount = ARRAY_COUNT(requiredGroupTable);

static void
processRequiredGroups (GroupsProcessor *processGroups, void *data) {
  gid_t groups[requiredGroupCount * 2];
  size_t count = 0;

  {
    const RequiredGroupEntry *rge = requiredGroupTable;
    const RequiredGroupEntry *end = rge + requiredGroupCount;

    while (rge < end) {
      {
        const char *name = rge->name;

        if (name) {
          const struct group *grp;

          if ((grp = getgrnam(name))) {
            groups[count++] = grp->gr_gid;
          } else {
            logMessage(LOG_WARNING, "unknown group: %s", name);
          }
        }
      }

      {
        const char *path = rge->path;

        if (path) {
          struct stat status;

          if (stat(path, &status) != -1) {
            groups[count++] = status.st_gid;

            if (rge->needRead && !(status.st_mode & S_IRGRP)) {
              logMessage(LOG_WARNING, "path not group readable: %s", path);
            }

            if (rge->needWrite && !(status.st_mode & S_IWGRP)) {
              logMessage(LOG_WARNING, "path not group writable: %s", path);
            }
          } else {
            logMessage(LOG_WARNING, "path access error: %s: %s", path, strerror(errno));
          }
        }
      }

      rge += 1;
    }
  }

  removeDuplicateGroups(groups, &count);
  processGroups(groups, count, data);
}

static void
setSupplementaryGroups (const gid_t *groups, size_t count, void *data) {
  logGroups(LOG_DEBUG, "setting supplementary groups", groups, count);

  if (setgroups(count, groups) == -1) {
    logSystemError("setgroups");
  }
}

static void
joinRequiredGroups (void) {
  processRequiredGroups(setSupplementaryGroups, NULL);
}

typedef struct {
  const gid_t *groups;
  size_t count;
} CurrentGroupsData;

static void
logUnjoinedGroups (const gid_t *groups, size_t count, void *data) {
  const CurrentGroupsData *cgd = data;

  const gid_t *cur = cgd->groups;
  const gid_t *curEnd = cur + cgd->count;

  const gid_t *req = groups;
  const gid_t *reqEnd = req + count;

  while (req < reqEnd) {
    int relation = (cur < curEnd)? compareGroups(*cur, *req): 1;

    if (relation > 0) {
      logGroup(LOG_WARNING, "group not joined", *req++);
    } else {
      if (!relation) req += 1;
      cur += 1;
    }
  }
}

static void
logWantedGroups (const gid_t *groups, size_t count, void *data) {
  CurrentGroupsData cgd = {
    .groups = groups,
    .count = count
  };

  processRequiredGroups(logUnjoinedGroups, &cgd);
}

static void
logMissingGroups (void) {
  processSupplementaryGroups(logWantedGroups, NULL);
}

static void
closeGroupsDatabase (void) {
  endgrent();
}
#endif /* HAVE_GRP_H */

#ifdef HAVE_LIBCAP
#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif /* HAVE_SYS_CAPABILITY_H */
#endif /* HAVE_LIBCAP */

#ifdef CAP_IS_SUPPORTED
typedef struct {
  const char *label;
  cap_t caps;
} CapabilitiesLogData;

static size_t
capabilitiesLogFormatter (char *buffer, size_t size, const void *data) {
  const CapabilitiesLogData *cld = data;

  size_t length;
  STR_BEGIN(buffer, size);
  STR_PRINTF("capabilities: %s:", cld->label);

  int capsAllocated = 0;
  cap_t caps;

  if (!(caps = cld->caps)) {
    if (!(caps = cap_get_proc())) {
      logSystemError("cap_get_proc");
      goto done;
    }

    capsAllocated = 1;
  }

  {
    char *text;

    if ((text = cap_to_text(caps, NULL))) {
      STR_PRINTF(" %s", text);
      cap_free(text);
    } else {
      logSystemError("cap_to_text");
    }
  }

  if (capsAllocated) {
    cap_free(caps);
    caps = NULL;
  }

done:
  length = STR_LENGTH;
  STR_END;
  return length;
}

static void
logCapabilities (cap_t caps, const char *label) {
  CapabilitiesLogData cld = { .label=label, .caps=caps };
  logData(LOG_DEBUG, capabilitiesLogFormatter, &cld);
}

static void
logCurrentCapabilities (const char *label) {
  logCapabilities(NULL, label);
}

static int
setCapabilities (cap_t caps) {
  if (cap_set_proc(caps) != -1) return 1;
  logSystemError("cap_set_proc");
  return 0;
}

static int
hasCapability (cap_t caps, cap_flag_t set, cap_value_t capability) {
  cap_flag_value_t value;
  if (cap_get_flag(caps, capability, set, &value) != -1) return value == CAP_SET;
  logSystemError("cap_get_flag");
  return 0;
}

static int
addCapability (cap_t caps, cap_flag_t set, cap_value_t capability) {
  if (cap_set_flag(caps, set, 1, &capability, CAP_SET) != -1) return 1;
  logSystemError("cap_set_flag");
  return 0;
}

typedef struct {
  const char *reason;
  cap_value_t value;
} RequiredCapabilityEntry;

static const RequiredCapabilityEntry requiredCapabilityTable[] = {
  { .reason = "for injecting input characters typed on a braille device",
    .value = CAP_SYS_ADMIN,
  },

  { .reason = "for playing alert tunes via the built-in PC speaker",
    .value = CAP_SYS_TTY_CONFIG,
  },

  { .reason = "for creating needed but missing special device files",
    .value = CAP_MKNOD,
  },
}; static const unsigned char requiredCapabilityCount = ARRAY_COUNT(requiredCapabilityTable);

static void
setRequiredCapabilities (void) {
  cap_t newCaps, oldCaps;

  if (amPrivilegedUser()) {
    oldCaps = NULL;
  } else if (!(oldCaps = cap_get_proc())) {
    logSystemError("cap_get_proc");
    return;
  }

  if ((newCaps = cap_init())) {
    {
      const RequiredCapabilityEntry *rce = requiredCapabilityTable;
      const RequiredCapabilityEntry *end = rce + requiredCapabilityCount;

      while (rce < end) {
        cap_value_t capability = rce->value;

        if (!oldCaps || hasCapability(oldCaps, CAP_PERMITTED, capability)) {
          if (!addCapability(newCaps, CAP_PERMITTED, capability)) break;
          if (!addCapability(newCaps, CAP_EFFECTIVE, capability)) break;
        }

        rce += 1;
      }
    }

    setCapabilities(newCaps);
    cap_free(newCaps);
  } else {
    logSystemError("cap_init");
  }

#ifdef PR_CAP_AMBIENT_CLEAR_ALL
  if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) == -1) {
    logSystemError("prctl[PR_CAP_AMBIENT_CLEAR_ALL]");
  }
#else /* PR_CAP_AMBIENT_CLEAR_ALL */
  logMessage(LOG_WARNING, "can't clear ambient capabilities");
#endif /* PR_CAP_AMBIENT_CLEAR_ALL */

  if (oldCaps) cap_free(oldCaps);
}

static void
logMissingCapabilities (void) {
  cap_t caps;

  if ((caps = cap_get_proc())) {
    const RequiredCapabilityEntry *rce = requiredCapabilityTable;
    const RequiredCapabilityEntry *end = rce + requiredCapabilityCount;

    while (rce < end) {
      cap_value_t capability = rce->value;

      if (!hasCapability(caps, CAP_EFFECTIVE, capability)) {
        logMessage(LOG_WARNING,
          "required capability not granted: %s (%s)",
          cap_to_name(capability), rce->reason
        );
      }

      rce += 1;
    }

    cap_free(caps);
  } else {
    logSystemError("cap_get_proc");
  }
}

static int
requestCapability (cap_t caps, cap_value_t capability, int inheritable) {
  if (!hasCapability(caps, CAP_EFFECTIVE, capability)) {
    if (!hasCapability(caps, CAP_PERMITTED, capability)) {
      logMessage(LOG_WARNING, "capability not permitted: %s", cap_to_name(capability));
      return 0;
    }

    if (!addCapability(caps, CAP_EFFECTIVE, capability)) return 0;
    if (!inheritable) return setCapabilities(caps);
  } else if (!inheritable) {
    return 1;
  }

  if (!hasCapability(caps, CAP_INHERITABLE, capability)) {
    if (!addCapability(caps, CAP_INHERITABLE, capability)) {
      return 0;
    }
  }

  if (setCapabilities(caps)) {
#ifdef PR_CAP_AMBIENT_RAISE
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, capability, 0, 0) != -1) return 1;
    logSystemError("prctl[PR_CAP_AMBIENT_RAISE]");
#else /* PR_CAP_AMBIENT_RAISE */
    logMessage(LOG_WARNING, "can't raise ambient capabilities");
#endif /* PR_CAP_AMBIENT_RAISE */
  }

  return 0;
}

static int
needCapability (cap_value_t capability, int inheritable, const char *reason) {
  int haveCapability = 0;
  const char *outcome = NULL;
  cap_t caps;

  if ((caps = cap_get_proc())) {
    if (hasCapability(caps, CAP_EFFECTIVE, capability)) {
      haveCapability = 1;
      outcome = "already added";
    } else if (requestCapability(caps, capability, inheritable)) {
      haveCapability = 1;
      outcome = "added";
    } else {
      outcome = "not granted";
    }

    cap_free(caps);
  } else {
    logSystemError("cap_get_proc");
  }

  if (outcome) {
    logMessage(LOG_DEBUG,
      "temporary capability %s: %s (%s)",
      outcome, cap_to_name(capability), reason
    );
  }

  return haveCapability;
}

#else /* CAP_IS_SUPPORTED */
static void
logCurrentCapabilities (const char *label) {
}
#endif /* CAP_IS_SUPPORTED */

#ifdef HAVE_SCHED_H
#include <sched.h>

typedef struct {
  const char *name;
  const char *summary;
  int unshareFlag;
  unsigned char needSysadmin:1;
} PrivateNamespaceEntry;

static const PrivateNamespaceEntry privateNamespaceTable[] = {
  { .unshareFlag = CLONE_NEWUTS,
    .needSysadmin = 1,
    .name = "UTS",
    .summary = "host name and NIS domain name",
  },
}; static const unsigned char privateNamespaceCount = ARRAY_COUNT(privateNamespaceTable);

static void
unshareNamespaces (void) {
  int unshareFlags = 0;

  int sysadminRequested = 0;
  int sysadminGranted = 0;

  const PrivateNamespaceEntry *pne = privateNamespaceTable;
  const PrivateNamespaceEntry *end = pne + privateNamespaceCount;

  while (pne < end) {
    int canUnshare = 1;

    if (pne->needSysadmin) {
      if (!sysadminRequested) {
        sysadminRequested = 1;

#ifdef CAP_SYS_ADMIN
        if (needCapability(CAP_SYS_ADMIN, 0, "for unsharing privileged namespaces")) {
          sysadminGranted = 1;
        }
#endif /* CAP_SYS_ADMIN */
      }

      canUnshare = sysadminGranted;
    }

    if (canUnshare) {
      logMessage(LOG_DEBUG,
        "unsharing namespace: %s (%s)", pne->name, pne->summary
      );

      unshareFlags |= pne->unshareFlag;
    } else {
      logMessage(LOG_WARNING,
        "can't unshare privileged namespace: %s (%s)", pne->name, pne->summary
      );
    }

    pne += 1;
  }

  if (unshareFlags) {
    if (unshare(unshareFlags) == -1) {
      logSystemError("unshare");
    }
  }
}
#endif /* HAVE_SCHED_H */

typedef void PrivilegesAcquisitionFunction (void);
typedef void MissingPrivilegesLogger (void);
typedef void ReleaseResourcesFunction (void);

typedef struct {
  const char *reason;
  PrivilegesAcquisitionFunction *acquirePrivileges;
  MissingPrivilegesLogger *logMissingPrivileges;
  ReleaseResourcesFunction *releaseResources;

  #ifdef CAP_IS_SUPPORTED
  cap_value_t capability;
  unsigned char inheritable:1;
  #endif /* CAP_IS_SUPPORTED */
} PrivilegesAcquisitionEntry;

static const PrivilegesAcquisitionEntry privilegesAcquisitionTable[] = {
  { .reason = "for installing kernel modules",
    .acquirePrivileges = installKernelModules,

    #ifdef CAP_SYS_MODULE
    .capability = CAP_SYS_MODULE,
    .inheritable = 1,
    #endif /* CAP_SYS_MODULE, */
  },

#ifdef HAVE_GRP_H
  { .reason = "for joining the required groups",
    .acquirePrivileges = joinRequiredGroups,
    .logMissingPrivileges = logMissingGroups,
    .releaseResources = closeGroupsDatabase,

    #ifdef CAP_SETGID
    .capability = CAP_SETGID,
    #endif /* CAP_SETGID, */
  },
#endif /* HAVE_GRP_H */

// This one must be last because it relinquishes the temporary capabilities.
#ifdef CAP_IS_SUPPORTED
  { .reason = "for assigning required capabilities",
    .acquirePrivileges = setRequiredCapabilities,
    .logMissingPrivileges = logMissingCapabilities,
  }
#endif /* CAP_IS_SUPPORTED */
}; static const unsigned char privilegesAcquisitionCount = ARRAY_COUNT(privilegesAcquisitionTable);

static void
acquirePrivileges (void) {
  if (amPrivilegedUser()) {
    const PrivilegesAcquisitionEntry *pae = privilegesAcquisitionTable;
    const PrivilegesAcquisitionEntry *end = pae + privilegesAcquisitionCount;

    while (pae < end) {
      pae->acquirePrivileges();
      pae += 1;
    }
  }

#ifdef CAP_IS_SUPPORTED
  else {
    const PrivilegesAcquisitionEntry *pae = privilegesAcquisitionTable;
    const PrivilegesAcquisitionEntry *end = pae + privilegesAcquisitionCount;

    while (pae < end) {
      cap_value_t capability = pae->capability;

      if (!capability || needCapability(capability, pae->inheritable, pae->reason)) {
        pae->acquirePrivileges();
      }

      pae += 1;
    }
  }
#endif /* CAP_IS_SUPPORTED */

  {
    const PrivilegesAcquisitionEntry *pae = privilegesAcquisitionTable;
    const PrivilegesAcquisitionEntry *end = pae + privilegesAcquisitionCount;

    while (pae < end) {
      {
        MissingPrivilegesLogger *log = pae->logMissingPrivileges;
        if (log) log();
      }

      {
        ReleaseResourcesFunction *release = pae->releaseResources;
        if (release) release();
      }

      pae += 1;
    }
  }
}

static int
setEnvironmentVariable (const char *name, const char *value) {
  if (setenv(name, value, 1) != -1) {
    logMessage(LOG_DEBUG, "environment variable set: %s: %s", name, value);
    return 1;
  } else {
    logSystemError("setenv");
  }

  return 0;
}

static int
setSafePath (void) {
  int parameter = _CS_PATH;
  const char *variable = "PATH";
  size_t size = confstr(parameter, NULL, 0);

  if (size > 0) {
    char path[size];
    confstr(parameter, path, sizeof(path));
    return setEnvironmentVariable(variable, path);
  }

  return setEnvironmentVariable(variable, "/usr/sbin:/sbin:/usr/bin:/bin");
}

static int
setSafeShell (void) {
  return setEnvironmentVariable("SHELL", "/bin/sh");
}

#ifdef HAVE_PWD_H
#include <pwd.h>

static int
setHomeDirectory (const char *directory) {
  if (chdir(directory) != -1) {
    logMessage(LOG_DEBUG, "working directory changed: %s", directory);
    setEnvironmentVariable("HOME", directory);
    return 1;
  } else {
    logSystemError("chdir");
  }

  return 0;
}

static void
setUserProperties (const struct passwd *pwd) {
  const char *user = pwd->pw_name;

  {
    const char *directory = pwd->pw_dir;

    if (directory && *directory) {
      logMessage(LOG_DEBUG, "setting home directory: %s", user);
      setHomeDirectory(pwd->pw_dir);
    } else {
      logMessage(LOG_DEBUG, "home directory not defined: %s", user);
    }
  }
}

static int
canSwitchUser (uid_t uid) {
  {
    uid_t rUid, eUid, sUid;
    getresuid(&rUid, &eUid, &sUid);
    if ((uid == rUid) || (uid == eUid) || (uid == sUid)) return 1;
  }

#ifdef CAP_SETUID
  if (needCapability(CAP_SETUID, 0, "for switching to the unprivileged user")) {
    return 1;
  }
#endif /* CAP_SETUID */

  return 0;
}

static int
canSwitchGroup (gid_t gid) {
  {
    gid_t rGid, eGid, sGid;
    getresgid(&rGid, &eGid, &sGid);
    if ((gid == rGid) || (gid == eGid) || (gid == sGid)) return 1;
  }

#ifdef CAP_SETGID
  if (needCapability(CAP_SETGID, 0, "for switching to the writable group")) {
    return 1;
  }
#endif /* CAP_SETGID */

  return 0;
}

static int
switchToUser (const char *user) {
  const struct passwd *pwd;

  if ((pwd = getpwnam(user))) {
    uid_t uid = pwd->pw_uid;
    gid_t gid = pwd->pw_gid;

    if (uid) {
      if (amPrivilegedUser() || (canSwitchUser(uid) && canSwitchGroup(gid))) {
        if (setProcessOwnership(uid, gid)) {
          logMessage(LOG_NOTICE, "switched to user: %s", user);
          setUserProperties(pwd);
          return 1;
        }
      } else {
        logMessage(LOG_WARNING, "can't switch to another user");
      }
    } else {
      logMessage(LOG_WARNING, "not an unprivileged user: %s", user);
    }
  } else {
    logMessage(LOG_WARNING, "user not found: %s", user);
  }

  return 0;
}

static int
switchUser (const char *user) {
  if (*user) {
    if (!amPrivilegedUser()) {
      logMessage(LOG_WARNING, "not executing as a privileged user");
    } else if (getuid()) {
      logMessage(LOG_WARNING, "executing as a set-user-ID root program");
    } else if (switchToUser(user)) {
      return 1;
    }

    logMessage(LOG_WARNING, "ignoring the explicitly specified user: %s", user);
  }

  if (*(user = UNPRIVILEGED_USER)) {
    if (switchToUser(user)) return 1;
    logMessage(LOG_WARNING, "couldn't switch to the default unprivileged user: %s", user);
  }

  {
    uid_t uid = getuid();
    setProcessOwnership(uid, getgid());

    const struct passwd *pwd;
    const char *name;
    char number[0X10];

    if ((pwd = getpwuid(uid))) {
      name = pwd->pw_name;
    } else {
      snprintf(number, sizeof(number), "%d", uid);
      name = number;
    }

    logMessage(LOG_NOTICE, "continuing to execute as the invoking user: %s", name);
    if (pwd) setUserProperties(pwd);
  }

  return 0;
}

static const char *
getSocketsDirectory (void) {
  const char *path = BRLAPI_SOCKETPATH;
  if (!ensureDirectory(path)) path = NULL;
  return path;
}

typedef struct {
  const char *whichDirectory;
  const char *(*getPath) (void);
  const char *expectedName;
} StateDirectoryEntry;

static const StateDirectoryEntry stateDirectoryTable[] = {
  { .whichDirectory = "updatable",
    .getPath = getUpdatableDirectory,
    .expectedName = "brltty",
  },

  { .whichDirectory = "writable",
    .getPath = getWritableDirectory,
    .expectedName = "brltty",
  },

  { .whichDirectory = "sockets",
    .getPath = getSocketsDirectory,
    .expectedName = "BrlAPI",
  },
}; static const unsigned char stateDirectoryCount = ARRAY_COUNT(stateDirectoryTable);

static int
canCreateStateDirectory (void) {
#ifdef CAP_DAC_OVERRIDE
  if (needCapability(CAP_DAC_OVERRIDE, 0, "for creating missing state directories")) {
    return 1;
  }
#endif /* CAP_DAC_OVERRIDE */

  return 0;
}

static const char *
getStateDirectoryPath (const StateDirectoryEntry *sde) {
  {
    const char *path = sde->getPath();
    if (path) return path;
  }

  if (!canCreateStateDirectory()) return NULL;
  return sde->getPath();
}

static int
canChangePathOwnership (const char *path) {
#ifdef CAP_CHOWN
  if (needCapability(CAP_CHOWN, 0, "for claiming group ownership of the state directories")) {
    return 1;
  }
#endif /* CAP_CHOWN */

  return 0;
}

static int
canChangePathPermissions (const char *path) {
#ifdef CAP_FOWNER
  if (needCapability(CAP_FOWNER, 0, "for adding group permissions to the state directories")) {
    return 1;
  }
#endif /* CAP_FOWNER */

  return 0;
}

typedef struct {
  gid_t owningGroup;
} StateDirectoryData;

static int
claimStateDirectory (const PathProcessorParameters *parameters) {
  const StateDirectoryData *sdd = parameters->data;
  const char *path = parameters->path;
  gid_t group = sdd->owningGroup;
  struct stat status;

  if (stat(path, &status) != -1) {
    int ownershipClaimed = 0;

    if (status.st_gid == group) {
      ownershipClaimed = 1;
    } else if (!canChangePathOwnership(path)) {
      logMessage(LOG_WARNING, "can't claim ownership: %s", path);
    } else if (chown(path, -1, group) == -1) {
      logSystemError("chown");
    } else {
      logMessage(LOG_INFO, "ownership claimed: %s", path);
      ownershipClaimed = 1;
    }

    if (ownershipClaimed) {
      mode_t oldMode = status.st_mode;
      mode_t newMode = oldMode;

      newMode |= S_IRGRP | S_IWGRP;
      if (S_ISDIR(newMode)) newMode |= S_IXGRP | S_ISGID;

      if (newMode != oldMode) {
        if (!canChangePathPermissions(path)) {
          logMessage(LOG_WARNING, "can't add group permissions: %s", path);
        } else if (chmod(path, newMode) != -1) {
          logMessage(LOG_INFO, "group permissions added: %s", path);
        } else {
          logSystemError("chmod");
        }
      }
    }
  } else {
    logSystemError("stat");
  }

  return 1;
}

static void
claimStateDirectories (void) {
  StateDirectoryData sdd = {
    .owningGroup = getegid(),
  };

  const StateDirectoryEntry *sde = stateDirectoryTable;
  const StateDirectoryEntry *end = sde + stateDirectoryCount;

  while (sde < end) {
    const char *path = getStateDirectoryPath(sde);

    if (path && *path) {
      const char *name = locatePathName(path);

      if (strcasecmp(name, sde->expectedName) == 0) {
        processPathTree(path, claimStateDirectory, &sdd);
      } else {
        logMessage(LOG_WARNING,
          "not claiming %s directory: %s (expecting %s)",
          sde->whichDirectory, path, sde->expectedName
        );
      }
    }

    sde += 1;
  }
}
#endif /* HAVE_PWD_H */

void
establishProgramPrivileges (const char *user) {
  logCurrentCapabilities("at start");

  setSafePath();
  setSafeShell();

#ifdef PR_SET_KEEPCAPS
  if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) == -1) {
    logSystemError("prctl[PR_SET_KEEPCAPS]");
  }
#endif /* PR_SET_KEEPCAPS */

#ifdef HAVE_SCHED_H
  unshareNamespaces();
#endif /* HAVE_SCHED_H */

#ifdef HAVE_PWD_H
  {
    if (switchUser(user)) {
      umask(umask(0) & ~S_IRWXG);
      claimStateDirectories();
    }

    endpwent();
  }
#endif /* HAVE_PWD_H */

  acquirePrivileges();
  logCurrentCapabilities("after relinquish");
}