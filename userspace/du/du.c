#include <forest.h>
#include <getopt.h>
#include <limits.h>

#define DEFAULT_BLOCK_SIZE 1024
#define SI_BLOCK_SIZE 1000
#define HUMAN_BASE 1024

enum display_mode {
	DISPLAY_BLOCKS,
	DISPLAY_HUMAN_1024,
	DISPLAY_HUMAN_1000,
	DISPLAY_KILO,
	DISPLAY_MEGA
};

struct du_options {
	enum display_mode mode;
	int summary;
	int all_files;
	int total;
	int max_depth;
	int follow_symlinks;
	int one_filesystem;
	dev_t root_device;
	int depth;
};

static int print_size(struct du_options *opts, off_t blocks, const char *name)
{
	switch (opts->mode) {
	case DISPLAY_HUMAN_1024: {
		double size = (double)blocks * DEFAULT_BLOCK_SIZE;
		const char *units[] = {"B", "K", "M", "G", "T", "P", "E"};
		int u = 0;
		while (size >= HUMAN_BASE && u < 6) {
			size /= HUMAN_BASE;
			u++;
		}
		if (u == 0)
			printf("%4.0f%s\t%s\n", size, units[u], name);
		else
			printf("%5.1f%s\t%s\n", size, units[u], name);
		break;
	}
	case DISPLAY_HUMAN_1000: {
		double size = (double)blocks * DEFAULT_BLOCK_SIZE;
		const char *units[] = {"B", "k", "M", "G", "T", "P", "E"};
		int u = 0;
		while (size >= SI_BLOCK_SIZE && u < 6) {
			size /= SI_BLOCK_SIZE;
			u++;
		}
		if (u == 0)
			printf("%4.0f%s\t%s\n", size, units[u], name);
		else
			printf("%5.1f%s\t%s\n", size, units[u], name);
		break;
	}
	case DISPLAY_KILO:
		printf("%ld\t%s\n", (long)((blocks * DEFAULT_BLOCK_SIZE + 512) / 1024), name);
		break;
	case DISPLAY_MEGA:
		printf("%ld\t%s\n", (long)((blocks * DEFAULT_BLOCK_SIZE + 524288) / 1048576), name);
		break;
	case DISPLAY_BLOCKS:
	default:
		printf("%ld\t%s\n", (long)blocks, name);
		break;
	}
	return 0;
}

static off_t du_dir(const char *path, struct du_options *opts);

static off_t du_entry(const char *path, struct du_options *opts)
{
	struct stat st;
	off_t total = 0;

	if (opts->follow_symlinks) {
		if (stat(path, &st) != 0) {
			fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
			return 0;
		}
	} else {
		if (lstat(path, &st) != 0) {
			fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
			return 0;
		}
	}

	if (S_ISLNK(st.st_mode) && !opts->follow_symlinks)
		return 0;

	if (S_ISDIR(st.st_mode)) {
		total = du_dir(path, opts);
	} else {
		total = (st.st_blocks / 2);
	}

	if (!opts->summary) {
		if (S_ISDIR(st.st_mode)) {
			print_size(opts, total, path);
		} else if (opts->all_files) {
			print_size(opts, total, path);
		}
	}

	return total;
}

static off_t du_dir(const char *path, struct du_options *opts)
{
	DIR *dir;
	struct dirent *ent;
	off_t total = 0;
	char fullpath[PATH_MAX];

	dir = opendir(path);
	if (!dir) {
		fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
		return 0;
	}

	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;

		int len = snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);
		if (len < 0 || (size_t)len >= sizeof(fullpath))
			continue;

		total += du_entry(fullpath, opts);
	}

	closedir(dir);
	return total;
}

static off_t du_path(const char *path, struct du_options *opts)
{
	struct stat st;

	if (opts->follow_symlinks) {
		if (stat(path, &st) != 0) {
			fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
			return 0;
		}
	} else {
		if (lstat(path, &st) != 0) {
			fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
			return 0;
		}
	}

	opts->root_device = st.st_dev;
	return du_entry(path, opts);
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-hHkmsacd:LxP] [--total] [--max-depth=N] [-L | -P] [file ...]\n", prog);
}

int main(int argc, char *argv[])
{
	struct du_options opts = {
		.mode = DISPLAY_BLOCKS,
		.summary = 0,
		.all_files = 0,
		.total = 0,
		.max_depth = -1,
		.follow_symlinks = 0,
		.one_filesystem = 0,
		.root_device = 0,
		.depth = 0
	};

	static struct option long_opts[] = {
		{"human-readable", no_argument, 0, 'h'},
		{"si",            no_argument, 0, 'H'},
		{"kilobytes",     no_argument, 0, 'k'},
		{"megabytes",     no_argument, 0, 'm'},
		{"summarize",     no_argument, 0, 's'},
		{"all",           no_argument, 0, 'a'},
		{"total",         no_argument, 0, 'c'},
		{"count-links",   no_argument, 0, 'l'},
		{"dereference-args", no_argument, 0, 'L'},
		{"no-dereference", no_argument, 0, 'P'},
		{"one-file-system", no_argument, 0, 'x'},
		{"max-depth",     required_argument, 0, 'd'},
		{0, 0, 0, 0}
	};

	int opt;
	int args_start = 1;

	while ((opt = getopt_long(argc, argv, "hHkmsacd:LxPl", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'h':
			opts.mode = DISPLAY_HUMAN_1024;
			break;
		case 'H':
			opts.mode = DISPLAY_HUMAN_1000;
			break;
		case 'k':
			opts.mode = DISPLAY_KILO;
			break;
		case 'm':
			opts.mode = DISPLAY_MEGA;
			break;
		case 's':
			opts.summary = 1;
			break;
		case 'a':
			opts.all_files = 1;
			break;
		case 'c':
			opts.total = 1;
			break;
		case 'l':
			/* count-links: not meaningful without hardlink tracking, ignore */
			break;
		case 'd': {
			char *end;
			long d = strtol(optarg, &end, 10);
			if (*end != '\0' || d < 0) {
				fprintf(stderr, "du: invalid maximum depth '%s'\n", optarg);
				return 1;
			}
			opts.max_depth = (int)d;
			break;
		}
		case 'L':
			opts.follow_symlinks = 1;
			break;
		case 'P':
			opts.follow_symlinks = 0;
			break;
		case 'x':
			opts.one_filesystem = 1;
			break;
		case '?':
			usage(argv[0]);
			return 1;
		default:
			break;
		}
	}

	args_start = optind;

	/* Handle --max-depth=N and --total from remaining args if getopt_long didn't catch them */
	for (int i = args_start; i < argc; i++) {
		if (strncmp(argv[i], "--max-depth=", 12) == 0) {
			char *end;
			long d = strtol(argv[i] + 12, &end, 10);
			if (*end != '\0' || d < 0) {
				fprintf(stderr, "du: invalid maximum depth '%s'\n", argv[i] + 12);
				return 1;
			}
			opts.max_depth = (int)d;
			args_start++;
		} else if (strcmp(argv[i], "--total") == 0) {
			opts.total = 1;
			args_start++;
		}
	}

	if (args_start >= argc) {
		args_start = 1;
		argv[1] = ".";
		if (argc < 2)
			argc = 2;
	}

	off_t grand_total = 0;
	int npaths = argc - args_start;

	for (int i = args_start; i < argc; i++) {
		off_t t = du_path(argv[i], &opts);
		if (opts.summary) {
			print_size(&opts, t, argv[i]);
		}
		grand_total += t;
	}

	if (opts.total && npaths > 1) {
		print_size(&opts, grand_total, "total");
	}

	return 0;
}
