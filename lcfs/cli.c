#include "includes.h"
#include "version/version.h"

#define PROG_NAME "lcfs"

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
    int (*func)(int argc, char *argv[]);
} cmd_group;

/* Run daemon to mount the file system */
static int
cmd_daemon(int argc, char *argv[]) {
    return lcfs_main(argc, argv);
}

/* Perform requested action */
static int
cmd_ioctl(int argc, char *argv[]) {
    return ioctl_main(argc, argv);
}

static struct
cmd_group lcfs_cmd_group[] = {
    {
        "daemon",
        "Start the lcfs daemon",
        // "--device=<device/file> --host-mount=<host-mountpath> --plugin-mount=<plugin-mountpath> [-f] [-d]",
        "<device/file> <host-mountpath> <plugin-mountpath> [-f] [-d]",
        "\tdevice     - device or file - image layers will be saved here\n"
        "\thost-mount - mount point on host\n"
        "\thost-mount - mount point propogated the plugin\n"
        "\t-f         - run foreground (optional)\n"
        "\t-d         - display debugging info (optional)\n",
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
        "Adjust page cache memory limit (default 25%, minimum 512MB)",
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
print_usage() {
    cmd_group *grp;
    int pad;

    printf("usage: lcfs [--help] [--version] <command> [<args>]\n\n");
    printf("Commands:\n");
    for (grp = lcfs_cmd_group; grp->cmd; grp++) {
        pad = 20 - strlen(grp->cmd);
        printf("  %s%*s%s\n", grp->cmd, pad, " ", grp->desc);
    }
    return 0;
}

/* Display command usage */
static int
print_cmd_usage(cmd_group *grp) {
    printf("usage: %s %s %s\n", PROG_NAME, grp->cmd, grp->usage);
    printf("%s", grp->help);
    return 0;
}

/* Invoke appropriate command requested */
int
run_cli(int argc, char *argv[]) {
    cmd_group *grp;

    if (argc < 2) {
        print_usage();
        return -1;
    }

    for (grp = lcfs_cmd_group; grp->cmd; grp++) {
        if (!strcasecmp(argv[1], "--help")) {
            return print_usage();
        }

        if (!strcasecmp(argv[1], "--version")) {
            return print_version();
        }

        if (!strcasecmp(argv[1], grp->cmd)) {
            if ((argc > 2) && (!strcasecmp(argv[2], "--help"))) {
                return print_cmd_usage(grp);
            }
            if (argc < (2 + grp->min)) {
                return print_cmd_usage(grp);
            }
            return grp->func(argc - 1, &argv[1]);
        }
    }
    printf("unknown command: %s\n", argv[1]);
    print_usage();
    return 0;
}

int
main(int argc, char *argv[]) {
    return run_cli(argc, argv);
}
