#include <errno.h>
#include <libgen.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__linux__)
#include <limits.h>
#include <sys/stat.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
#include <sys/stat.h>
#include <sys/sysctl.h>
#endif

#include "cJSON.h"
#include "log.h"
#include "zip.h"

#define STREQ(a, b) (strcmp((a), (b)) == 0)
#define STRPREFIX(s, pre) (strncmp(pre, s, strlen(pre)) == 0)

#define NELEMS(x) ((int)(sizeof(x) / sizeof((x)[0])))

#define CLEANUP(x) if (x != NULL) { free(x); }

#define PREAMBLE_CONFIG_PATH ".bootstrap/PLZ_PREAMBLE_CONFIG"

#define ERR_INTERPRETERS "Failed to get interpreters from .pex preamble configuration"
#define ERR_INTERPRETER_ARGS "Failed to get interpreter arguments from .pex preamble configuration"

/*
 * log_init sets log.c's logging level based on the value of the PLZ_PEX_PREAMBLE_VERBOSITY
 * environment variable and configures it to include the current date and time in log messages.
 */
static void log_init() {
    char *verbosity = getenv("PLZ_PEX_PREAMBLE_VERBOSITY");
    int level = LOG_ERROR;

    if (verbosity != NULL) {
        if (STREQ(verbosity, "trace")) {
            level = LOG_TRACE;
        } else if (STREQ(verbosity, "debug")) {
            level = LOG_DEBUG;
        } else if (STREQ(verbosity, "info")) {
            level = LOG_INFO;
        } else if (STREQ(verbosity, "warn")) {
            level = LOG_WARN;
        } else if (STREQ(verbosity, "error")) {
            level = LOG_ERROR;
        } else if (STREQ(verbosity, "fatal")) {
            level = LOG_FATAL;
        } else {
            log_error("Unknown verbosity '%s'; defaulting to 'error'", verbosity);
        }
    }

    log_set_level(level);
    log_set_time_format("%Y-%m-%d %H:%M:%S");
}

#ifdef __linux__

/*
 * get_path_max returns the maximum length of a relative path name when the given path is the
 * current working directory (although the path need not actually be a directory, or even exist).
 */
static int get_path_max(char *path) {
    int path_max = pathconf(path, _PC_PATH_MAX);

    if (path_max <= 0) {
        // PATH_MAX may be defined in <limits.h>, but this is not a POSIX requirement. If it isn't
        // defined, fall back to 4096 (as recommended by Linux's realpath(3) man page).
#ifdef PATH_MAX
        path_max = PATH_MAX;
#else
        path_max = 4096;
#endif
    }

    return path_max;
}

/*
 * get_link_path resolves the symbolic link at the path lpath and stores the link's destination path
 * in rpath. It returns 0 on success and 1 on failure.
 */
static int get_link_path(char *lpath, char **rpath) {
    struct stat sb = {0};
    int slen = 0;
    int rlen = 0;
    int ret = 1;

    // Get the length of lpath's destination path so we can allocate a buffer of that length for
    // readlink to write to (plus one byte, so we can determine whether readlink has truncated the
    // path it writes, and also for the trailing null we'll append to the path afterwards). If lpath
    // is a magic symlink (in which case sb.st_size is 0), assume the destination path is PATH_MAX
    // bytes long - it's an overestimate, but at least the buffer will be large enough for readlink
    // to safely write to.
    if (lstat(lpath, &sb) == -1) {
        goto end;
    }
    slen = (sb.st_size == 0 ? get_path_max(lpath) : sb.st_size) + 1;

    if (((*rpath) = malloc(slen)) == NULL) {
        goto end;
    }

    if ((rlen = readlink(lpath, (*rpath), slen)) == -1) {
        goto end;
    }
    // If readlink filled the buffer, it truncated the destination path it wrote. In this case, the
    // value is untrustworthy, so we're better off not using it.
    if (slen == rlen) {
        goto end;
    }

    // Otherwise, add the trailing null to the destination path that readlink omitted.
    (*rpath)[rlen] = 0;

    ret = 0;

end:
    return ret;
}

#endif // __linux__

/*
 * get_origin stores the run-time path to the .pex file in origin. It returns 0 on success and 1 on
 * failure.
 */
static int get_origin(char **origin) {
    char *exe_path = NULL;
    char *exe_dir = NULL;
    int ret = 1;

#if defined(__linux__)
    if (get_link_path("/proc/self/exe", &exe_path) != 0) {
        goto end;
    }
#elif defined(__APPLE__)
    uint32_t len = 0;

    // Call _NSGetExecutablePath once to find out how long the executable path is, then again after
    // we've allocated that much memory to store it.
    _NSGetExecutablePath(NULL, &len);
    if ((exe_path = malloc(sizeof(char) * len)) == NULL) {
        goto end;
    }
    if (_NSGetExecutablePath(exe_path, &len) != 0) {
        goto end;
    }
#elif defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    size_t len = 0;

    // Call sysctl once to find out how long the executable path is, then again after we've
    // allocated that much memory to store it.
    if (sysctl(mib, NELEMS(mib), NULL, &len, NULL, 0) != 0) {
        goto end;
    }
    if ((exe_path = malloc(sizeof(char) * len)) == NULL) {
        goto end;
    }
    if (sysctl(mib, NELEMS(mib), exe_path, &len, NULL, 0) != 0) {
        goto end;
    }
#else
#error "Unsupported operating system"
#endif

    exe_dir = dirname(exe_path);
    if (((*origin) = strdup(exe_dir)) == NULL) {
        goto end;
    }

    ret = 0;

end:
#ifdef __APPLE__
    CLEANUP(exe_path);
#endif

    return ret;
}

/*
 * get_config reads and parses the .pex preamble configuration, a JSON-encoded file at
 * .bootstrap/PLZ_PREAMBLE_CONFIG in the zip archive, and returns it as a cJSON struct. It returns
 * NULL on failure.
 */
static const cJSON* get_config(char *pex_path) {
    struct zip_t *zip = NULL;
    int ziperr = 0;
    char *configbuf = NULL;
    size_t configsize = 0;
    const cJSON *config = NULL;

    zip = zip_openwitherror(pex_path, 0, 'r', &ziperr);
    if (ziperr < 0) {
        log_fatal("Failed to open .pex as zip file: %s", zip_strerror(ziperr));
        goto end;
    }

    if ((ziperr = zip_entry_open(zip, PREAMBLE_CONFIG_PATH)) < 0) {
        log_fatal("Failed to open .pex preamble configuration", zip_strerror(ziperr));
        goto end;
    }

    if ((ziperr = (int)zip_entry_read(zip, (void **)&configbuf, &configsize)) < 0) {
        log_fatal("Failed to read .pex preamble configuration", zip_strerror(ziperr));
        goto end;
    }

    zip_entry_close(zip);

    zip_close(zip);

    if ((config = cJSON_ParseWithLength(configbuf, configsize)) == NULL) {
        log_fatal("Failed to parse .pex preamble configuration: JSON syntax error near '%s'", cJSON_GetErrorPtr());
        goto end;
    }

end:
    CLEANUP(configbuf);

    return config;
}

/*
 * get_interpreter_args stores the list of interpreter command line arguments specified in the given
 * .pex preamble configuration in args, and the length of this list in len. It returns 0 on success
 * and 1 on failure.
 */
static int get_interpreter_args(const cJSON *config, int *len, char ***args) {
    const cJSON *json_args = NULL;
    const cJSON *json_arg = NULL;
    int i = 0;
    int ret = 0;

    *len = 0;

    json_args = cJSON_GetObjectItemCaseSensitive(config, "interpreter_args");
    if (cJSON_IsNull(json_args)) {
        log_debug("interpreter_args is null; no arguments added");
        goto end;
    } else if (!cJSON_IsArray(json_args)) {
        log_fatal("%s: interpreter_args must be an array or null", ERR_INTERPRETER_ARGS);
        ret = 1;
        goto end;
    }

    *len = cJSON_GetArraySize(json_args);
    if (*len == 0) {
        log_debug("interpreter_args is empty; no arguments added");
        goto end;
    }

    if ((*args = malloc(sizeof(char *) * (*len))) == NULL) {
        log_fatal("%s: memory allocation failure", ERR_INTERPRETER_ARGS);
        goto end;
    }

    cJSON_ArrayForEach(json_arg, json_args) {
        if (!cJSON_IsString(json_arg) || (json_arg->valuestring == NULL)) {
            log_fatal("%s: elements in interpreter_args must be strings", ERR_INTERPRETER_ARGS);
            ret = 1;
            goto end;
        }

        if (((*args)[i] = malloc(strlen(json_arg->valuestring) + 1)) == NULL) {
            log_fatal("%s: memory allocation failure", ERR_INTERPRETER_ARGS);
            goto end;
        }
        strcpy((*args)[i], json_arg->valuestring);
        log_debug("Added argument from interpreter_args: %s", (*args)[i]);

        i++;
    }

end:
    return ret;
}

/*
 * get_interpreters stores the list of interpreter paths specified in the given .pex preamble
 * configuration in interps, and the length of this list in len. It returns 0 on success and 1 on
 * failure.
 */
static int get_interpreters(const cJSON *config, int *len, char ***interps) {
    char *origin = NULL;
    const cJSON *json_interps = NULL;
    const cJSON *json_interp = NULL;
    int i = 0;
    int ret = 0;

    *len = 0;

    json_interps = cJSON_GetObjectItemCaseSensitive(config, "interpreters");
    if (!cJSON_IsArray(json_interps)) {
        log_fatal("%s: interpreters must be an array", ERR_INTERPRETERS);
        ret = 1;
        goto end;
    }

    if (cJSON_GetArraySize(json_interps) == 0) {
        log_fatal("%s: interpreters must not be empty", ERR_INTERPRETERS);
        ret = 1;
        goto end;
    }

    cJSON_ArrayForEach(json_interp, json_interps) {
        if (!cJSON_IsString(json_interp) || (json_interp->valuestring == NULL)) {
            log_fatal("%s: elements in interpreters must be strings", ERR_INTERPRETERS);
            ret = 1;
            goto end;
        }

        if ((*interps = realloc(*interps, sizeof(char *) * ++(*len))) == NULL) {
            log_fatal("%s: memory reallocation failure", ERR_INTERPRETERS);
            goto end;
        }

        if (STRPREFIX(json_interp->valuestring, "$ORIGIN/")) {
            if (origin == NULL) {
                if (get_origin(&origin) != 0) {
                    log_fatal("%s: origin resolution failure", ERR_INTERPRETERS);
                    ret = 1;
                    goto end;
                }
                log_debug("Resolved $ORIGIN to %s", origin);
            }

            // Replace "$ORIGIN" with the path to this program at the start of the interpreter path
            if (((*interps)[i] = malloc(strlen(origin) + strlen(json_interp->valuestring) - strlen("$ORIGIN") + 1)) == NULL) {
                log_fatal("%s: memory allocation failure", ERR_INTERPRETERS);
                goto end;
            }
            strncpy((*interps)[i], origin, strlen(origin));
            strcpy((*interps)[i] + strlen(origin), json_interp->valuestring + strlen("$ORIGIN"));
        } else {
            if (((*interps)[i] = malloc(strlen(json_interp->valuestring) + 1)) == NULL) {
                log_fatal("%s: memory allocation failure", ERR_INTERPRETERS);
                goto end;
            }
            strcpy((*interps)[i], json_interp->valuestring);
        }

        log_debug("Added interpreter to search path: %s", (*interps)[i]);

        i++;
    }

end:
    CLEANUP(origin);

    return ret;
}

int main(int argc, char **argv) {
    const cJSON *config = NULL;
    int config_args_len = 0;
    char **config_args = NULL;
    int interps_len = 0;
    char **interps = NULL;
    char **interp_argv = NULL;
    int i = 0;

    log_init();

    if (argc == 0) {
        log_fatal("Failed to execute .pex preamble: argv is empty");
        return 1;
    }

    if ((config = get_config(argv[0])) == NULL) {
        return 1;
    }

    if (get_interpreter_args(config, &config_args_len, &config_args) != 0) {
        return 1;
    }

    if (get_interpreters(config, &interps_len, &interps) != 0) {
        return 1;
    }

    // interp_argv is the array of arguments that will be passed to execvp():
    //
    // {
    //     interpreter,
    //     config_args[0],
    //     # ...
    //     config_args[n],
    //     argv[0],
    //     # ...
    //     argv[n],
    //     NULL
    // }
    if ((interp_argv = malloc(sizeof(char *) * (1 + config_args_len + argc + 1))) == NULL) {
        log_fatal("Failed to create process argument array: memory allocation failure");
        return 1;
    }

    // The first element is the Python interpreter path, which is substituted in during each
    // execvp() invocation. The remaining elements are static for each execvp() invocation:
    // - the interpreter arguments from the .pex preamble configuration (if any);
    for (i = 0; i < config_args_len; i++) {
        interp_argv[1 + i] = config_args[i];
    }
    // - the name of the currently-executing program (which is also the .pex file that the
    //   Python interpreter needs to execute) and the command line arguments given to this
    //   process (if any);
    for (i = 0; i < argc; i++) {
        interp_argv[1 + config_args_len + i] = argv[i];
    }
    // - NULL (the terminator for execvp()).
    interp_argv[1 + config_args_len + argc] = NULL;

    for (i = 0; i < interps_len; i++) {
        interp_argv[0] = interps[i];

        log_debug("Attempting to execute interpreter %s", interps[i]);

        execvp(interps[i], interp_argv);
        if (errno == ENOENT) {
            // ENOENT isn't necessarily an error case (there are legitimate reasons for any given
            // interpreter not to exist), hence logging this at info level rather than error.
            log_info("%s does not exist or could not be executed", interps[i]);
        } else {
            log_error("Failed to execute %s: %s", interps[i], strerror(errno));
        }
    }

    log_fatal("Failed to execute any interpreters in the interpreter search path");

    return 1;
}
