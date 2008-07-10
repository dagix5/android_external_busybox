/* vi: set sw=4 ts=4: */
/*
 * simplified modprobe
 *
 * Copyright (c) 2008 Vladimir Dronnikov
 * Copyright (c) 2008 Bernhard Fischer (initial depmod code)
 *
 * Licensed under GPLv2, see file LICENSE in this tarball for details.
 */

#include "libbb.h"
#include "unarchive.h"

#include <sys/utsname.h> /* uname() */
#include <fnmatch.h>

extern int init_module(void *module, unsigned long len, const char *options);
extern int delete_module(const char *module, unsigned flags);
extern int query_module(const char *name, int which, void *buf, size_t bufsize, size_t *ret);


#define dbg1_error_msg(...) ((void)0)
#define dbg2_error_msg(...) ((void)0)
//#define dbg1_error_msg(...) bb_error_msg(__VA_ARGS__)
//#define dbg2_error_msg(...) bb_error_msg(__VA_ARGS__)

#define DEPFILE_BB CONFIG_DEFAULT_DEPMOD_FILE".bb"

enum {
	OPT_q = (1 << 0), /* be quiet */
	OPT_r = (1 << 1), /* module removal instead of loading */
};

typedef struct module_info {
	char *pathname;
	char *aliases;
	char *deps;
} module_info;

/*
 * GLOBALS
 */
struct globals {
	module_info *modinfo;
	char *module_load_options;
	int module_count;
	int module_found_idx;
	int stringbuf_idx;
	char stringbuf[32 * 1024]; /* some modules have lots of stuff */
	/* for example, drivers/media/video/saa7134/saa7134.ko */
};
#define G (*ptr_to_globals)
#define modinfo             (G.modinfo            )
#define module_count        (G.module_count       )
#define module_found_idx    (G.module_found_idx   )
#define module_load_options (G.module_load_options)
#define stringbuf_idx       (G.stringbuf_idx      )
#define stringbuf           (G.stringbuf          )
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
} while (0)


static void appendc(char c)
{
	if (stringbuf_idx < sizeof(stringbuf))
		stringbuf[stringbuf_idx++] = c;
}

static void bksp(void)
{
	if (stringbuf_idx)
		stringbuf_idx--;
}

static void append(const char *s)
{
	size_t len = strlen(s);
	if (stringbuf_idx + len < sizeof(stringbuf)) {
		memcpy(stringbuf + stringbuf_idx, s, len);
		stringbuf_idx += len;
	}
}

static void reset_stringbuf(void)
{
	stringbuf_idx = 0;
}

static char* copy_stringbuf(void)
{
	char *copy = xmalloc(stringbuf_idx);
	return memcpy(copy, stringbuf, stringbuf_idx);
}

static char* find_keyword(char *ptr, size_t len, const char *word)
{
	int wlen;

	if (!ptr) /* happens if read_module cannot read it */
		return NULL;

	wlen = strlen(word);
	len -= wlen - 1;
	while ((ssize_t)len > 0) {
		char *old = ptr;
		/* search for the first char in word */
		ptr = memchr(ptr, *word, len);
		if (ptr == NULL) /* no occurance left, done */
			break;
		if (strncmp(ptr, word, wlen) == 0)
			return ptr + wlen; /* found, return ptr past it */
		++ptr;
		len -= (ptr - old);
	}
	return NULL;
}

static void replace(char *s, char what, char with)
{
	while (*s) {
		if (what == *s)
			*s = with;
		++s;
	}
}

/* Take "word word", return malloced "word",NUL,"word",NUL,NUL */
static char* str_2_list(const char *str)
{
	int len = strlen(str) + 1;
	char *dst = xmalloc(len + 1);

	dst[len] = '\0';
	memcpy(dst, str, len);
//TODO: protect against 2+ spaces: "word  word"
	replace(dst, ' ', '\0');
	return dst;
}

#if ENABLE_FEATURE_MODPROBE_SMALL_ZIPPED
static char *xmalloc_open_zipped_read_close(const char *fname, size_t *sizep)
{
	size_t len;
	char *image;
	char *suffix;

	int fd = open_or_warn(fname, O_RDONLY);
	if (fd < 0)
		return NULL;

	suffix = strrchr(fname, '.');
	if (suffix) {
		if (strcmp(suffix, ".gz") == 0)
			fd = open_transformer(fd, unpack_gz_stream, "gunzip");
		else if (strcmp(suffix, ".bz2") == 0)
			fd = open_transformer(fd, unpack_bz2_stream, "bunzip2");
	}

	len = (sizep) ? *sizep : 64 * 1024 * 1024;
	image = xmalloc_read(fd, &len);
	if (!image)
		bb_perror_msg("read error from '%s'", fname);
	close(fd);

	if (sizep)
		*sizep = len;
	return image;
}
# define read_module xmalloc_open_zipped_read_close
#else
# define read_module xmalloc_open_read_close
#endif

/* We use error numbers in a loose translation... */
static const char *moderror(int err)
{
	switch (err) {
	case ENOEXEC:
		return "invalid module format";
	case ENOENT:
		return "unknown symbol in module or invalid parameter";
	case ESRCH:
		return "module has wrong symbol version";
	case EINVAL: /* "invalid parameter" */
		return "unknown symbol in module or invalid parameter"
		+ sizeof("unknown symbol in module or");
	default:
		return strerror(err);
	}
}

static int load_module(const char *fname, const char *options)
{
#if 1
	int r;
	size_t len = MAXINT(ssize_t);
	char *module_image;
	dbg1_error_msg("load_module('%s','%s')", fname, options);

	module_image = read_module(fname, &len);
	r = (!module_image || init_module(module_image, len, options ? options : "") != 0);
	free(module_image);
	dbg1_error_msg("load_module:%d", r);
	return r; /* 0 = success */
#else
	/* For testing */
	dbg1_error_msg("load_module('%s','%s')", fname, options);
	return 1;
#endif
}

static void parse_module(module_info *info, const char *pathname)
{
	char *module_image;
	char *ptr;
	size_t len;
	size_t pos;
	dbg1_error_msg("parse_module('%s')", pathname);

	/* Read (possibly compressed) module */
	len = 64 * 1024 * 1024; /* 64 Mb at most */
	module_image = read_module(pathname, &len);
//TODO: optimize redundant module body reads

	/* "alias1 symbol:sym1 alias2 symbol:sym2" */
	reset_stringbuf();
	pos = 0;
	while (1) {
		ptr = find_keyword(module_image + pos, len - pos, "alias=");
		if (!ptr) {
			ptr = find_keyword(module_image + pos, len - pos, "__ksymtab_");
			if (!ptr)
				break;
			/* DOCME: __ksymtab_gpl and __ksymtab_strings occur
			 * in many modules. What do they mean? */
			if (strcmp(ptr, "gpl") != 0 && strcmp(ptr, "strings") != 0) {
				dbg2_error_msg("alias:'symbol:%s'", ptr);
				append("symbol:");
			}
		} else {
			dbg2_error_msg("alias:'%s'", ptr);
		}
		append(ptr);
		appendc(' ');
		pos = (ptr - module_image);
	}
	bksp(); /* remove last ' ' */
	appendc('\0');
	info->aliases = copy_stringbuf();

	/* "dependency1 depandency2" */
	reset_stringbuf();
	ptr = find_keyword(module_image, len, "depends=");
	if (ptr && *ptr) {
		replace(ptr, ',', ' ');
		replace(ptr, '-', '_');
		dbg2_error_msg("dep:'%s'", ptr);
		append(ptr);
	}
	appendc('\0');
	info->deps = copy_stringbuf();

	free(module_image);
}

static int pathname_matches_modname(const char *pathname, const char *modname)
{
	const char *fname = bb_get_last_path_component_nostrip(pathname);
	const char *suffix = strrstr(fname, ".ko");
//TODO: can do without malloc?
	char *name = xstrndup(fname, suffix - fname);
	int r;
	replace(name, '-', '_');
	r = (strcmp(name, modname) == 0);
	free(name);
	return r;
}

static FAST_FUNC int fileAction(const char *pathname,
		struct stat *sb UNUSED_PARAM,
		void *modname_to_match,
		int depth UNUSED_PARAM)
{
	int cur;
	const char *fname;

	pathname += 2; /* skip "./" */
	fname = bb_get_last_path_component_nostrip(pathname);
	if (!strrstr(fname, ".ko")) {
		dbg1_error_msg("'%s' is not a module", pathname);
		return TRUE; /* not a module, continue search */
	}

	cur = module_count++;
	modinfo = xrealloc_vector(modinfo, 12, cur);
//TODO: use zeroing version of xrealloc_vector?
	modinfo[cur].pathname = xstrdup(pathname);
	modinfo[cur].aliases = NULL;
	modinfo[cur+1].pathname = NULL;

	if (!pathname_matches_modname(fname, modname_to_match)) {
		dbg1_error_msg("'%s' module name doesn't match", pathname);
		return TRUE; /* module name doesn't match, continue search */
	}

	dbg1_error_msg("'%s' module name matches", pathname);
	module_found_idx = cur;
	parse_module(&modinfo[cur], pathname);

	if (!(option_mask32 & OPT_r)) {
		if (load_module(pathname, module_load_options) == 0) {
			/* Load was successful, there is nothing else to do.
			 * This can happen ONLY for "top-level" module load,
			 * not a dep, because deps dont do dirscan. */
			exit(EXIT_SUCCESS);
		}
	}

	return TRUE;
}

static void load_dep_bb(void)
{
	char *line;
	FILE *fp = fopen(DEPFILE_BB, "r");

	if (!fp)
		return;

	while ((line = xmalloc_fgetline(fp)) != NULL) {
		char* space;
		int cur;

		if (!line[0]) {
			free(line);
			continue;
		}
		space = strchrnul(line, ' ');
		cur = module_count++;
		modinfo = xrealloc_vector(modinfo, 12, cur);
//TODO: use zeroing version of xrealloc_vector?
		modinfo[cur+1].pathname = NULL;
		modinfo[cur].pathname = line; /* we take ownership of malloced block here */
		if (*space)
			*space++ = '\0';
		modinfo[cur].aliases = space;
		modinfo[cur].deps = xmalloc_fgetline(fp) ? : xzalloc(1);
		if (modinfo[cur].deps[0]) {
			/* deps are not "", so next line must be empty */
			line = xmalloc_fgetline(fp);
			/* Refuse to work with damaged config file */
			if (line && line[0])
				bb_error_msg_and_die("error in %s at '%s'", DEPFILE_BB, line);
			free(line);
		}
	}
}

static module_info* find_alias(const char *alias)
{
	int i;
	dbg1_error_msg("find_alias('%s')", alias);

	/* First try to find by name (cheaper) */
	i = 0;
	while (modinfo[i].pathname) {
		if (pathname_matches_modname(modinfo[i].pathname, alias)) {
			dbg1_error_msg("found '%s' in module '%s'",
					alias, modinfo[i].pathname);
			if (!modinfo[i].aliases) {
				parse_module(&modinfo[i], modinfo[i].pathname);
			}
			return &modinfo[i];
		}
		i++;
	}

	/* Scan all module bodies, extract modinfo (it contains aliases) */
	i = 0;
	while (modinfo[i].pathname) {
		char *desc, *s;
		if (!modinfo[i].aliases) {
			parse_module(&modinfo[i], modinfo[i].pathname);
		}
		/* "alias1 symbol:sym1 alias2 symbol:sym2" */
		desc = str_2_list(modinfo[i].aliases);
		/* Does matching substring exist? */
		for (s = desc; *s; s += strlen(s) + 1) {
			/* Aliases in module bodies can be defined with
			 * shell patterns. Example:
			 * "pci:v000010DEd000000D9sv*sd*bc*sc*i*".
			 * Plain strcmp() won't catch that */
			if (fnmatch(s, alias, 0) == 0) {
				free(desc);
				dbg1_error_msg("found alias '%s' in module '%s'",
						alias, modinfo[i].pathname);
				return &modinfo[i];
			}
		}
		free(desc);
		i++;
	}
	dbg1_error_msg("find_alias '%s' returns NULL", alias);
	return NULL;
}

#if ENABLE_FEATURE_MODPROBE_SMALL_CHECK_ALREADY_LOADED
static int already_loaded(const char *name)
{
	int ret = 0;
	int len = strlen(name);
	char *line;
	FILE* modules;

	modules = xfopen("/proc/modules", "r");
	while ((line = xmalloc_fgets(modules)) != NULL) {
		if (strncmp(line, name, len) == 0 && line[len] == ' ') {
			free(line);
			ret = 1;
			break;
		}
		free(line);
	}
	fclose(modules);
	return ret;
}
#else
#define already_loaded(name) is_rmmod
#endif

/*
 Given modules definition and module name (or alias, or symbol)
 load/remove the module respecting dependencies
*/
#if !ENABLE_FEATURE_MODPROBE_SMALL_OPTIONS_ON_CMDLINE
#define process_module(a,b) process_module(a)
#define cmdline_options ""
#endif
static void process_module(char *name, const char *cmdline_options)
{
	char *s, *deps, *options;
	module_info *info;
	int is_rmmod = (option_mask32 & OPT_r) != 0;
	dbg1_error_msg("process_module('%s','%s')", name, cmdline_options);

	replace(name, '-', '_');

	dbg1_error_msg("already_loaded:%d is_rmmod:%d", already_loaded(name), is_rmmod);
	if (already_loaded(name) != is_rmmod) {
		dbg1_error_msg("nothing to do for '%s'", name);
		return;
	}

	options = NULL;
	if (!is_rmmod) {
		char *opt_filename = xasprintf("/etc/modules/%s", name);
		options = xmalloc_open_read_close(opt_filename, NULL);
		if (options)
			replace(options, '\n', ' ');
#if ENABLE_FEATURE_MODPROBE_SMALL_OPTIONS_ON_CMDLINE
		if (cmdline_options) {
			/* NB: cmdline_options always have one leading ' '
			 * (see main()), we remove it here */
			char *op = xasprintf(options ? "%s %s" : "%s %s" + 3,
						cmdline_options + 1, options);
			free(options);
			options = op;
		}
#endif
		free(opt_filename);
		module_load_options = options;
		dbg1_error_msg("process_module('%s'): options:'%s'", name, options);
	}

	if (!module_count) {
		/* Scan module directory. This is done only once.
		 * It will attempt module load, and will exit(EXIT_SUCCESS)
		 * on success. */
		module_found_idx = -1;
		recursive_action(".",
			ACTION_RECURSE, /* flags */
			fileAction, /* file action */
			NULL, /* dir action */
			name, /* user data */
			0); /* depth */
		dbg1_error_msg("dirscan complete");
		/* Module was not found, or load failed, or is_rmmod */
		if (module_found_idx >= 0) { /* module was found */
			info = &modinfo[module_found_idx];
		} else { /* search for alias, not a plain module name */
			info = find_alias(name);
		}
	} else {
		info = find_alias(name);
	}

	/* rmmod? unload it by name */
	if (is_rmmod) {
		if (delete_module(name, O_NONBLOCK|O_EXCL) != 0
		 && !(option_mask32 & OPT_q)
		) {
			bb_perror_msg("remove '%s'", name);
			goto ret;
		}
		/* N.B. we do not stop here -
		 * continue to unload modules on which the module depends:
		 * "-r --remove: option causes modprobe to remove a module.
		 * If the modules it depends on are also unused, modprobe
		 * will try to remove them, too." */
	}

	if (!info) { /* both dirscan and find_alias found nothing */
		goto ret;
	}

	/* Iterate thru dependencies, trying to (un)load them */
	deps = str_2_list(info->deps);
	for (s = deps; *s; s += strlen(s) + 1) {
		//if (strcmp(name, s) != 0) // N.B. do loops exist?
		dbg1_error_msg("recurse on dep '%s'", s);
		process_module(s, NULL);
		dbg1_error_msg("recurse on dep '%s' done", s);
	}
	free(deps);

	/* insmod -> load it */
	if (!is_rmmod) {
		errno = 0;
		if (load_module(info->pathname, options) != 0) {
			if (EEXIST != errno) {
				bb_error_msg("'%s': %s",
						info->pathname,
						moderror(errno));
			} else {
				dbg1_error_msg("'%s': %s",
						info->pathname,
						moderror(errno));
			}
		}
	}
 ret:
	free(options);
//TODO: return load attempt result from process_module.
//If dep didn't load ok, continuing makes little sense.
}
#undef cmdline_options


/* For reference, module-init-tools-0.9.15-pre2 options:

# insmod
Usage: insmod filename [args]

# rmmod --help
Usage: rmmod [-fhswvV] modulename ...
 -f (or --force) forces a module unload, and may crash your machine.
 -s (or --syslog) says use syslog, not stderr
 -v (or --verbose) enables more messages
 -w (or --wait) begins a module removal even if it is used
    and will stop new users from accessing the module (so it
    should eventually fall to zero).

# modprobe
Usage: modprobe [--verbose|--version|--config|--remove] filename [options]

# depmod --help
depmod 0.9.15-pre2 -- part of module-init-tools
depmod -[aA] [-n -e -v -q -V -r -u] [-b basedirectory] [forced_version]
depmod [-n -e -v -q -r -u] [-F kernelsyms] module1.o module2.o ...
If no arguments (except options) are given, "depmod -a" is assumed

depmod will output a dependancy list suitable for the modprobe utility.

Options:
        -a, --all               Probe all modules
        -n, --show              Write the dependency file on stdout only
        -b basedirectory
        --basedir basedirectory Use an image of a module tree.
        -F kernelsyms
        --filesyms kernelsyms   Use the file instead of the
                                current kernel symbols.
*/

int modprobe_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int modprobe_main(int argc UNUSED_PARAM, char **argv)
{
	struct utsname uts;
	char applet0 = applet_name[0];
	USE_FEATURE_MODPROBE_SMALL_OPTIONS_ON_CMDLINE(char *options;)

	/* depmod is a stub */
	if ('d' == applet0)
		return EXIT_SUCCESS;

	/* are we lsmod? -> just dump /proc/modules */
	if ('l' == applet0) {
		xprint_and_close_file(xfopen("/proc/modules", "r"));
		return EXIT_SUCCESS;
	}

	INIT_G();

	/* insmod, modprobe, rmmod require at least one argument */
	opt_complementary = "-1";
	/* only -q (quiet) and -r (rmmod),
	 * the rest are accepted and ignored (compat) */
	getopt32(argv, "qrfsvw");
	argv += optind;

	/* are we rmmod? -> simulate modprobe -r */
	if ('r' == applet0) {
		option_mask32 |= OPT_r;
	}

	/* goto modules directory */
	xchdir(CONFIG_DEFAULT_MODULES_DIR);
	uname(&uts); /* never fails */
	xchdir(uts.release);

#if ENABLE_FEATURE_MODPROBE_SMALL_OPTIONS_ON_CMDLINE
	/* If not rmmod, parse possible module options given on command line.
	 * insmod/modprobe takes one module name, the rest are parameters. */
	options = NULL;
	if ('r' != applet0) {
		char **arg = argv;
		while (*++arg) {
			/* Enclose options in quotes */
			char *s = options;
			options = xasprintf("%s \"%s\"", s ? s : "", *arg);
			free(s);
			*arg = NULL;
		}
	}
#else
	if ('r' != applet0)
		argv[1] = NULL;
#endif

	load_dep_bb();

	/* Load/remove modules.
	 * Only rmmod loops here, insmod/modprobe has only argv[0] */
	do {
		process_module(*argv++, options);
	} while (*argv);

	if (ENABLE_FEATURE_CLEAN_UP) {
		USE_FEATURE_MODPROBE_SMALL_OPTIONS_ON_CMDLINE(free(options);)
	}
	return EXIT_SUCCESS;
}