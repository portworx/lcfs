#include <stdio.h>
#include "includes.h"
#include "lcfs.h"
#include "version/version.h"

#define PROG_NAME "lcfs"

typedef struct
cmd_group {
	char *cmd;
	char *desc;
	char *usage;
	char *help;
	int (*func)(int argc, char *argv[]);
} cmd_group;

int
cmd_daemon(int argc, char *argv[]) {
	printf("Running: %d %s\n", argc, argv[0]);
	return lcfs_main(argc, argv);
}

int
cmd_stats(int argc, char *argv[]) {
	fprintf(stderr, "not implemented\n");

	return 0;
}

static struct
cmd_group lcfs_cmd_group[] = {
	{
		"daemon",
		"Start the lcfs daemon",
		"--device=<device/file> --host-mount=<host-mountpath> --plugin-mount=<plugin-mountpath> [-f] [-d]",
		"\tdevice     - device or file wher lcfs will store the actuall data\n"
		"\thost-mount - mount point on host\n"
		"\thost-mount - mount point propogated the plugin\n"
		"\t-f         - run foreground (optional)\n"
		"\t-d         - display debugging info (optional)\n",
		cmd_daemon
	},
	{
		"stats", 
		"Display lcfs stats", 
		"", 
		"", 
		cmd_stats
	},
	{NULL},
};

int
print_version() {
	fprintf(stderr, "%s\n", Build);
	fprintf(stderr, "%s\n", Release);

	return 0;
}

int
print_usage() {
	printf("usage: lcfs [--help] [--version] <command> [<args>]\n\n");
	printf("Commands:\n");

	for (cmd_group *grp = lcfs_cmd_group; grp->cmd; grp++) {
		int pad = 20;
		pad -= strlen(grp->cmd);
		printf("  %s%*s%s\n", grp->cmd, pad, " ", grp->desc);
	}

	return 0;
}

int
run_cli(int argc, char *argv[]) {
	if (argc < 2) {
		print_usage();
		return -1;
	}

	for (cmd_group *grp = lcfs_cmd_group; grp->cmd; grp++) {
		if (!strcasecmp(argv[1], "--help")) {
			return print_usage();
		}

		if (!strcasecmp(argv[1], "--version")) {
			return print_version();
		}

		if (!strcasecmp(argv[1], grp->cmd)) {
			if ((argc > 2) && (!strcasecmp(argv[2], "--help"))) {
				printf("usage: %s %s %s\n", PROG_NAME, argv[1], grp->usage);
				printf("%s\n", grp->help);
				return 0;
			} else {
				return grp->func(argc-1, &argv[1]);
			}
		}
	}

	printf("Unknown command: %s\n", argv[1]);
	print_usage();

	return 0;
}
