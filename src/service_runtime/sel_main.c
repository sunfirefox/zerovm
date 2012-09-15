/*
 * Copyright (c) 2011 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * ZeroVM main
 */

#include <assert.h>
#include <glib.h>
#include "src/gio/gio.h"
#include "src/fault_injection/fault_injection.h"
#include "src/perf_counter/nacl_perf_counter.h"
#include "src/service_runtime/nacl_all_modules.h"
#include "src/service_runtime/nacl_globals.h"
#include "src/service_runtime/nacl_signal.h"
#include "src/service_runtime/etag.h"
#include "src/manifest/manifest_parser.h" /* d'b. todo: move to initializer */
#include "src/manifest/manifest_setup.h" /* d'b. todo: move to initializer */
#include "src/service_runtime/sel_qualify.h"

/* initialize syslog to put ZeroVm log messages */
static void ZeroVMLogCtor()
{
  /*
   * are here to avoid redefinition LOG_* used in "nacl_log.h"
   * todo(d'b): remove this ugliness
   */
#define LOG_PID   0x01  /* log the pid with each message */
#define LOG_CONS  0x02  /* log on the console if errors in sending */
#define LOG_NDELAY  0x08  /* don't delay open */
#define LOG_USER  (1<<3)  /* random user-level messages */
  extern void openlog (const char *ident, int option, int facility);

  openlog(ZEROVMLOG_NAME, ZEROVMLOG_OPTIONS, ZEROVMLOG_PRIORITY);
}

/* close log. ### optional? */
static void ZeroVMLogDtor()
{
  extern void closelog (void); /* to avoid redefinition LOG_* */
  closelog();
}

/* parse given command line and initialize NaClApp object */
static void ParseCommandLine(struct NaClApp *nap, int argc, char **argv)
{
  int opt;
  int i;
  char *manifest_name = NULL;

  /* set defaults */
  nap->verbosity = NaClLogGetVerbosity();
  nap->skip_qualification = 0;
  nap->fuzzing_quit_after_load = 0;
  nap->handle_signals = 1;

  /* todo(d'b): revise switches and rename them */
  while((opt = getopt(argc, argv, "+FeQsSv:M:")) != -1)
  {
    switch(opt)
    {
      case 'M':
        manifest_name = optarg;
        break;
      case 's':
        nap->skip_validator = 1;
        NaClLog(LOG_WARNING, "validation disabled by -s\n");
        break;
      case 'F':
        nap->fuzzing_quit_after_load = 1;
        break;
      case 'e':
        EtagCtor();
        break;
      case 'S':
        /* d'b: disable signals handling */
        nap->handle_signals = 0;
        break;
      case 'v':
        i = atoi(optarg);
        i = nap->verbosity = i < 1 ? 0 : i;
        while(i--)
          NaClLogIncrVerbosity();
        break;
        /* case 'w':  with 'h' and 'r' above */
      case 'Q':
        nap->skip_qualification = 1;
        NaClLog(LOG_WARNING, "PLATFORM QUALIFICATION DISABLED BY -Q - "
                "Native Client's sandbox will be unreliable!\n");
        break;
      default:
        NaClLog(LOG_ERROR, "ERROR: unknown option: [%c]\n\n", opt);
        puts(HELP_SCREEN);
        exit(1);
        break;
    }
  }

  /* show zerovm command line */
  if(nap->verbosity)
  {
    NaClLog(LOG_INFO, "zerovm argument list:\n");
    for(i = 0; i < argc; ++i)
      NaClLog(LOG_INFO, "%s\n", argv[i]);
  }

  /* parse manifest file specified in cmdline */
  if(manifest_name == NULL)
  {
    puts(HELP_SCREEN);
    exit(1);
  }
  COND_ABORT(ManifestCtor(manifest_name), "Invalid manifest file");

  /* set available nap and manifest fields */
  assert(nap->system_manifest != NULL);
  nap->user_side_flag = 0; /* we are in the trusted code */
  nap->system_manifest->nexe = GetValueByKey("Nexe");
  syscallback = 0;
}

/* set zerovm state by given message */
/* todo(): move it to "tools.h", zvm_state should not be updated directly */
static void SetZVMState(struct NaClApp *nap, const char *msg)
{
  snprintf(nap->zvm_state, SIGNAL_STRLEN, "%s", msg);
  nap->zvm_state[SIGNAL_STRLEN] = '\0';
}

/*
 * set validation state according to zvm command line options
 * note: updates nap->validation_state
 */
static void ValidateNexe(struct NaClApp *nap)
{
  char *args[3] = {VALIDATOR_NAME, NULL, NULL};
  GError *error = NULL;
  int exit_status = 0;
  enum ValidationState {
    NotValidated,
    ValidationOK,
    ValidationFailed
  };

  assert(nap != NULL);
  assert(nap->system_manifest != NULL);

  /* skip validation? */
  nap->validation_state = NotValidated;
  if(nap->skip_validator != 0) return;

  /* prepare command line and run it */
  args[1] = nap->system_manifest->nexe;
  COND_ABORT(g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH |
      G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
      NULL, NULL, &exit_status, &error) == 0, "cannot start validator");

  /* check the result */
  nap->validation_state = exit_status == 0 ? ValidationOK : ValidationFailed;
  COND_ABORT(nap->validation_state != ValidationOK, "validation failed");
}

/* create/overwrite file and put integer in it */
static inline void EchoToFile(const char *path, int code)
{
  FILE *f = fopen(path, "w");

  COND_ABORT(f == NULL, "cannot create file");
  fprintf(f, "%d", code);
  fclose(f);
}

/* initialize extended user statistics */
static void ExternalAccounting(struct NaClApp *nap)
{
  struct stat st;
  char cfolder[BIG_ENOUGH_SPACE + 1];
  char counter[BIG_ENOUGH_SPACE + 1];
  int pid = (int32_t)getpid();
  int length;

  assert(nap != NULL);
  assert(nap->system_manifest != NULL);

  /* exit if the cgroups folder is missing */
  nap->system_manifest->extended_accounting = NULL;
  if(!(stat(CGROUPS_FOLDER, &st) == 0 && S_ISDIR(st.st_mode)))
    return;

  /* fail if folder of same pid exists and locked */
  length = snprintf(cfolder, BIG_ENOUGH_SPACE, "%s/%d", CGROUPS_FOLDER, pid);
  cfolder[BIG_ENOUGH_SPACE] = '\0';
  if(stat(cfolder, &st) == 0 && S_ISDIR(st.st_mode))
    COND_ABORT(rmdir(cfolder) != 0, "current pid in cgroups is already taken");

  /* create folder of own pid */
  COND_ABORT(mkdir(cfolder, 0700) != 0, "cannot create pid folder in cgroups");

  /* store accounting folder to the system manifest */
  nap->system_manifest->extended_accounting = malloc(length + 1);
  COND_ABORT(nap->system_manifest->extended_accounting == NULL,
      "cannot allocate memory to hold accounting folder name");
  strcpy(nap->system_manifest->extended_accounting, cfolder);

  /* create special file in it with own pid */
  snprintf(counter, BIG_ENOUGH_SPACE, "%s/%s", cfolder, CGROUPS_TASKS);
  EchoToFile(counter, pid);

  /* create user cpu accountant */
  snprintf(counter, BIG_ENOUGH_SPACE, "%s/%s", cfolder, CGROUPS_USER_CPU);
  EchoToFile(counter, 1);

  /* create memory accountant */
  snprintf(counter, BIG_ENOUGH_SPACE, "%s/%s", cfolder, CGROUPS_MEMORY);
  EchoToFile(counter, 1);

  /* create swap accountant */
  snprintf(counter, BIG_ENOUGH_SPACE, "%s/%s", cfolder, CGROUPS_SWAP);
  EchoToFile(counter, 1);
}

int main(int argc, char **argv)
{
  struct NaClApp state, *nap = &state;
  struct SystemManifest sys_mft;
  NaClErrorCode errcode = LOAD_INTERNAL;
  struct GioFile gout;
  struct GioMemoryFileSnapshot main_file;
  struct NaClPerfCounter time_all_main;

  /* d'b: initial settings */
  /* todo(d'b): move to inline function {{ */
  memset(nap, 0, sizeof *nap);
  nap->trusted_code = 1;
  nap->system_manifest = &sys_mft;
  memset(nap->system_manifest, 0, sizeof *nap->system_manifest);
  gnap = nap;
  SetZVMState(nap, "nexe didn't start");
  ZeroVMLogCtor();
  NaClSignalHandlerInit();

  /* @IGNORE_LINES_FOR_CODE_HYGIENE[1] */
  /*
   * Set malloc not to use mmap even for large allocations.  This is currently
   * necessary when we must use a specific area of RAM for the sandbox.
   *
   * During startup, before the sandbox is set up, the sel_ldr allocates a chunk
   * of memory to store the untrusted code.  Normally such an allocation would
   * go into the sel_ldr's heap area, but the allocation is typically large --
   * at least hundreds of KiB.  The default malloc configuration on Linux (at
   * least) switches to mmap for such allocations, and mmap will select
   * essentially any unoccupied section of the address space.  The result: the
   * nexe is allocated in the region we use for the sandbox, we protect the
   * address space, and then the memcpy into the sandbox (of course) fails.
   *
   * This is at best a temporary fix.  The proper fix is to reserve the
   * sandbox region early enough that this isn't a problem.  Possible methods
   * are discussed in this bug:
   *   http://code.google.com/p/nativeclient/issues/detail?id=232
   */

  NaClAllModulesInit();
  NaClPerfCounterCtor(&time_all_main, "SelMain");
  fflush((FILE *) NULL);
  COND_ABORT(!GioFileRefCtor(&gout, stdout),
             "Could not create general standard output channel");
  ParseCommandLine(nap, argc, argv);

  /* validate given nexe and run/fail/exit */
  ValidateNexe(nap);

  /* todo(d'b): remove it after validator will be removed from the project */
  NaClLogGetGio();

  /* the dyn_array constructor 1st call */
  COND_ABORT(NaClAppCtor(nap) == 0, "Error while constructing app state");
  errcode = LOAD_OK;

  /* We use the signal handler to verify a signal took place. */
  if(nap->skip_qualification == 0)
  {
    NaClErrorCode pq_error = NACL_FI_VAL("pq", NaClErrorCode, NaClRunSelQualificationTests());
    if(LOAD_OK != pq_error)
    {
      errcode = pq_error;
      nap->module_load_status = pq_error;
      NaClLog(LOG_ERROR, "Error while loading \"%s\": %s\n",
          NULL != nap->system_manifest->nexe ?
              nap->system_manifest->nexe : "(no file, to-be-supplied-via-RPC)",
          NaClErrorString(errcode));
    }
  }

  /* Remove the signal handler if we are not using it. */
  if(nap->handle_signals == 0)
  {
    NaClSignalHandlerFini();
    NaClSignalAssertNoHandlers(); /* Sanity check. */
  }

#define PERF_CNT(str)\
  NaClPerfCounterMark(&time_all_main, str);\
  NaClPerfCounterIntervalLast(&time_all_main);

  if(0 == GioMemoryFileSnapshotCtor(&main_file, nap->system_manifest->nexe))
  {
    NaClLog(LOG_ERROR, "%s", strerror(errno));
    NaClLog(LOG_FATAL, "Cannot open \"%s\".\n", nap->system_manifest->nexe);
  }
  PERF_CNT("SnapshotNaclFile");

  /* validate untrusted code (nexe) */
  if(LOAD_OK == errcode)
  {
    NaClLog(2, "Loading nacl file %s (non-RPC)\n", nap->system_manifest->nexe);
    errcode = NaClAppLoadFile((struct Gio *) &main_file, nap);
    if (LOAD_OK != errcode)
    {
      NaClLog(LOG_ERROR, "Error while loading \"%s\": %s\n",
              nap->system_manifest->nexe, NaClErrorString(errcode));
      NaClLog(LOG_ERROR, ("Using the wrong type of nexe (nacl-x86-32"
          " on an x86-64 or vice versa)\nor a corrupt nexe file may be"
          " responsible for this error.\n"));
    }

    PERF_CNT("AppLoadEnd");
    nap->module_load_status = errcode;
  }

  if(-1 == (*((struct Gio *) &main_file)->vtbl->Close)((struct Gio *) &main_file))
    NaClLog(LOG_ERROR, "Error while closing \"%s\".\n", nap->system_manifest->nexe);

  (*((struct Gio *) &main_file)->vtbl->Dtor)((struct Gio *) &main_file);
  if(nap->fuzzing_quit_after_load) NaClExit(0);

  /*
   * construct system and host manifests
   * todo(d'b): move it to "src/platform/platform_init.c" chain. problems to solve:
   * - channels construction needs initialized nacl descriptors (it is dyn_array)
   * - "memory chunk" needs initialized memory manager (user stack, text, data e.t.c)
   */
  SystemManifestCtor(nap); /* needs dyn_array initialized */

  /* error reporting done; can quit now if there was an error earlier */
  if(LOAD_OK != errcode)
    NaClLog(LOG_FATAL, "Not running app code since errcode is %s (%d)\n",
            NaClErrorString(errcode), errcode);

  PERF_CNT("CreateMainThread");

  /* Make sure all the file buffers are flushed before entering the nexe */
  fflush((FILE *) NULL);

  /*
   * "defence in depth" part
   * todo(): find a proper place for this call
   */
  LastDefenseLine();

  /* start external accounting */
  ExternalAccounting(nap);

  /* set user code trap() exit location */
  if(setjmp(user_exit) == 0)
  {
    /* pass control to the user code */
    if(!NaClCreateMainThread(nap))
      NaClLog(LOG_FATAL, "creating main thread failed\n");
  }
  PERF_CNT("WaitForMainThread");
  PERF_CNT("SelMainEnd");

  /* report to host. call destructors. exit */
  ZeroVMLogDtor();
  NaClExit(0);

  /* Unreachable, but having the return prevents a compiler error. */
  return -1;
}
