#include "includes.h"
#include "version/version.h"

typedef struct cmd_group {

    /* Sub-command */
    char *cmd;

    /* Description */
    char *desc;

    /* Usage */
    char *usage;

    /* Help text */
    char *help;

    /* Minimum number of arguments */
    int min;

    /* Function to invoke */
    int (*func)(char *pgm, int argc, char *argv[]);
} cmd_group;

/* Run daemon to mount the file system */
static int
cmd_daemon(char *pgm, int argc, char *argv[]) {
    return lcfs_main(pgm, argc, argv);
}

/* Perform requested action */
static int
cmd_ioctl(char *pgm, int argc, char *argv[]) {
    return ioctl_main(pgm, argc, argv);
}

static struct
cmd_group lcfs_cmd_group[] = {
    {
        "daemon",
        "Start the lcfs daemon",
        "<device/file> <host-mountpath> <plugin-mountpath> "
#ifndef __MUSL__
            "[-p] "
#endif
            "[-f] [-d] [-m] [-r] [-t] [-v]",
        "\tdevice     - device or file - image layers will be saved here\n"
        "\thost-mount - mount point on host\n"
        "\thost-mount - mount point propogated to the plugin\n"
        "\t-f         - run foreground (optional)\n"
        "\t-c         - format file system (optional)\n"
        "\t-d         - display fuse debugging info (optional)\n"
        "\t-m         - enable memory stats (optional)\n"
        "\t-r         - enable request stats (optional)\n"
        "\t-t         - enable tracking count of file types (optional)\n"
#ifndef __MUSL__
        "\t-p         - enable profiling (optional)\n"
#endif
        "\t-s         - swap layers when committed\n"
        "\t-v         - enable verbose mode (optional)\n",
        3,
        cmd_daemon
    },
    {
        "stats",
        "Display lcfs stats",
        "<mnt> <id> [-c]",
        "\tmnt     - mount point\n"
        "\tid      - layer name or .\n"
        "\t[-c]    - clear stats (optional)\n",
        2,
        cmd_ioctl
    },
    {
        "commit",
        "Commit to disk",
        "<mnt>",
        "\tmnt     - mount point\n",
        1,
        cmd_ioctl
    },
    {
        "syncer",
        "Adjust syncer frequency (default 1 minute)",
        "<mnt> <seconds>",
        "\tmnt     - mount point\n"
        "\tseconds - time in seconds, 0 to disable (default 1 minute)\n",
        2,
        cmd_ioctl
    },
    {
        "pcache",
        "Adjust page cache memory limit (default 5%, minimum 512MB)",
        "<mnt> <limit>",
        "\tmnt     - mount point\n"
        "\tlimit   - memory limit in MB (default 512MB)\n",
        2,
        cmd_ioctl
    },
    {
        "flush",
        "Release pages not in use",
        "<mnt>",
        "\tmnt     - mount point\n",
        1,
        cmd_ioctl
    },
    {
        "grow",
        "Grow size of the file system",
        "<mnt>",
        "\tmnt     - mount point\n",
        1,
        cmd_ioctl
    },
#ifndef __MUSL__
    {
        "profile",
        "Enable/Disable profiling",
        "<mnt>",
        "\tmnt                  - mount point\n"
        "\t[enable|disable]     - enable/disable profiling\n",
        2,
        cmd_ioctl
    },
#endif
    {
        "verbose",
        "enable/disable verbose mode",
        "<mnt>",
        "\tmnt                  - mount point\n"
        "\t[enable|disable]     - enable/disable verbose mode\n",
        2,
        cmd_ioctl
    },
    {NULL},
};

/* Print version */
static int
print_version() {
    fprintf(stderr, "%s\n", Build);
    fprintf(stderr, "%s\n", Release);
    return 0;
}

/* Display usage */
static int
print_usage(char *pgm) {
    cmd_group *grp;
    int pad;

    fprintf(stderr, "usage: %s [--help] [--version] <command> [<args>]\n\n",
            pgm);
    fprintf(stderr, "Commands:\n");
    for (grp = lcfs_cmd_group; grp->cmd; grp++) {
        pad = 20 - strlen(grp->cmd);
        fprintf(stderr, "  %s%*s%s\n", grp->cmd, pad, " ", grp->desc);
    }
    return 0;
}

/* Display command usage */
static int
print_cmd_usage(char *pgm, cmd_group *grp) {
    fprintf(stderr, "usage: %s %s %s\n", pgm, grp->cmd, grp->usage);
    fprintf(stderr, "%s", grp->help);
    return 0;
}

/* Invoke appropriate command requested */
static int
run_cli(int argc, char *argv[]) {
    cmd_group *grp;

    if (argc < 2) {
        print_usage(argv[0]);
        return -1;
    }

    for (grp = lcfs_cmd_group; grp->cmd; grp++) {
        if (!strcasecmp(argv[1], "--help")) {
            return print_usage(argv[0]);
        }

        if (!strcasecmp(argv[1], "--version")) {
            return print_version();
        }

        if (!strcasecmp(argv[1], grp->cmd)) {
            if ((argc > 2) && (!strcasecmp(argv[2], "--help"))) {
                return print_cmd_usage(argv[0], grp);
            }
            if (argc < (2 + grp->min)) {
                return print_cmd_usage(argv[0], grp);
            }
            return grp->func(argv[0], argc - 1, &argv[1]);
        }
    }
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    print_usage(argv[0]);
    return 0;
}

int
main(int argc, char *argv[]) {
    if (getuid()) {
        fprintf(stderr, "Run as root\n");
        return EPERM;
    }
    return run_cli(argc, argv);
}
