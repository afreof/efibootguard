/*
 * EFI Boot Guard
 *
 * Copyright (c) Siemens AG, 2017
 *
 * Authors:
 *  Andreas Reichel <andreas.reichel.ext@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "env_api.h"

static char doc[] =
    "bg_setenv/bg_printenv - Environment tool for the EFI Boot Guard";

static struct argp_option options_setenv[] = {
    {"kernel", 'k', "KERNEL", 0, "Set kernel to load"},
    {"args", 'a', "KERNEL_ARGS", 0, "Set kernel arguments"},
    {"part", 'p', "ENV_PART", 0, "Set environment partition to update. "
				 "If no partition is specified, the one "
				 "with the smallest revision value "
				 "above zero is updated."},
    {"revision", 'r', "REVISION", 0, "Set revision value"},
    {"ustate", 's', "USTATE", 0, "Set update status for environment"},
    {"filepath", 'f', "ENVFILE_DIR", 0, "Output environment to file. Expects "
					"an output path where the file name "
					"is automatically appended."},
    {"watchdog", 'w', "WATCHDOG_TIMEOUT", 0, "Watchdog timeout in seconds"},
    {"confirm", 'c', 0, 0, "Confirm working environment"},
    {"update", 'u', 0, 0, "Automatically update oldest revision"},
    {"verbose", 'v', 0, 0, "Be verbose"},
    {0}};

static struct argp_option options_printenv[] = {
    {"verbose", 'v', 0, 0, "Be verbose"},
    {0}};

struct arguments {
	bool output_to_file;
	int which_part;
	BG_ENVDATA tmpdata;
};

/* The following byte is used to mark parts of a temporary
 * environment structure as to be ignored and not overwrite
 * values of the real environment. Can be anything
 * except used ASCII codes (should be > 0x7F).
 */
#define IGNORE_MARKER_BYTE 0xEA

/* auto update feature automatically updates partition with
 * oldest environment revision (lowest value)
 */
static bool auto_update = false;

static bool part_specified = false;

static bool verbosity = false;

static char *envfilepath = NULL;

static char *ustatemap[] = {"OK", "INSTALLED", "TESTING", "FAILED", "UNKNOWN"};

static uint8_t str2ustate(char *str)
{
	uint8_t i;

	if (!str) {
		return USTATE_UNKNOWN;
	}
	for (i = USTATE_MIN; i < USTATE_MAX; i++) {
		if (strncasecmp(str, ustatemap[i], strlen(ustatemap[i])) == 0) {
			return i;
		}
	}
	return USTATE_UNKNOWN;
}

static char *ustate2str(uint8_t ustate)
{
	if (ustate >= USTATE_MIN && ustate <= USTATE_MAX) {
		return ustatemap[ustate];
	}
}

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct arguments *arguments = state->input;
	int i;
	wchar_t buffer[ENV_STRING_LENGTH];
	char *tmp;

	switch (key) {
	case 'k':
		if (strlen(arg) > ENV_STRING_LENGTH) {
			fprintf(stderr, "Error, kernel filename is too long. "
					"Maximum of %d "
					"characters permitted.\n",
				ENV_STRING_LENGTH);
			return 1;
		}
		memcpy(arguments->tmpdata.kernelfile, str8to16(buffer, arg),
		       strlen(arg) * 2 + 2);
		break;
	case 'a':
		if (strlen(arg) > ENV_STRING_LENGTH) {
			fprintf(stderr,
				"Error, kernel arguments string is too long. "
				"Maximum of %d characters permitted.\n",
				ENV_STRING_LENGTH);
			return 1;
		}
		memcpy(arguments->tmpdata.kernelparams, str8to16(buffer, arg),
		       strlen(arg) * 2 + 2);
		break;
	case 'p':
		i = strtol(arg, &tmp, 10);
		if ((errno == ERANGE && (i == LONG_MAX || i == LONG_MIN)) ||
		    (errno != 0 && i == 0) || (tmp == arg)) {
			fprintf(stderr, "Invalid number specified for -p.\n");
			return 1;
		}
		if (i == 0 || i == 1) {
			printf("Updating config partition #%d\n", i);
			arguments->which_part = i;
			part_specified = true;
		} else {
			fprintf(stderr,
				"Selected partition out of range. Valid range: "
				"0..1.\n");
			return 1;
		}
		break;
	case 's':
		i = strtol(arg, &tmp, 10);
		if ((errno == ERANGE && (i == LONG_MAX || i == LONG_MIN)) ||
		    (errno != 0 && i == 0) || (tmp == arg)) {
			// maybe user specified an enum string
			i = str2ustate(arg);
			if (i == USTATE_UNKNOWN) {
				fprintf(stderr, "Invalid state specified.\n");
				return 1;
			}
		}
		if (i < 0 || i > 3) {
			fprintf(
			    stderr,
			    "Invalid ustate value specified. Possible values: "
			    "0 (%s), 1 (%s), 2 (%s), 3 (%s)\n",
			    ustatemap[0], ustatemap[1], ustatemap[2],
			    ustatemap[3]);
			return 1;
		} else {
			arguments->tmpdata.ustate = i;
			VERBOSE(stdout, "Ustate set to %u (%s).\n", i,
				ustate2str(i));
		}
		break;
	case 'r':
		i = atoi(arg);
		VERBOSE(stdout, "Revision is set to %d.\n", i);
		arguments->tmpdata.revision = i;
		break;
	case 'w':
		i = atoi(arg);
		if (i != 0) {
			VERBOSE(stdout,
				"Setting watchdog timeout to %d seconds.\n", i);
			arguments->tmpdata.watchdog_timeout_sec = i;
		} else {
			fprintf(stderr, "Watchdog timeout must be non-zero.\n");
			return 1;
		}
		break;
	case 'f':
		arguments->output_to_file = true;
		asprintf(&envfilepath, "%s/%s", arg, FAT_ENV_FILENAME);
		break;
	case 'c':
		VERBOSE(stdout,
			"Confirming environment to work. Removing boot-once "
			"and testing flag.\n");
		arguments->tmpdata.ustate = 0;
		break;
	case 'u':
		if (part_specified) {
			fprintf(stderr,
				"Error, both automatic and manual partition "
				"selection. Cannot use -p and -u "
				"simultaneously.\n");
			return 1;
		}
		auto_update = true;
		break;
	case 'v':
		/* Set verbosity in this program */
		verbosity = true;
		/* Set verbosity in the library */
		be_verbose(true);
		break;
	case ARGP_KEY_ARG:
		/* too many arguments - program terminates with call to
		 * argp_usage with non-zero return code */
		argp_usage(state);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static void update_environment(BG_ENVDATA *dest, BG_ENVDATA *src)
{
	if (!dest || !src) {
		return;
	}
	if ((uint8_t)*src->kernelfile != IGNORE_MARKER_BYTE) {
		memcpy((void *)dest->kernelfile, (void *)src->kernelfile,
		       sizeof(src->kernelfile));
	}
	if ((uint8_t)*src->kernelparams != IGNORE_MARKER_BYTE) {
		memcpy((void *)dest->kernelparams, (void *)src->kernelparams,
		       sizeof(src->kernelparams));
	}
	if ((uint8_t)src->ustate != IGNORE_MARKER_BYTE) {
		memcpy((void *)&dest->ustate, (void *)&src->ustate,
		       sizeof(src->ustate));
	}
	if ((uint8_t)src->revision != IGNORE_MARKER_BYTE) {
		memcpy((void *)&dest->revision, (void *)&src->revision,
		       sizeof(src->revision));
	}
	if ((uint8_t)src->watchdog_timeout_sec != IGNORE_MARKER_BYTE) {
		memcpy((void *)&dest->watchdog_timeout_sec,
		       (void *)&src->watchdog_timeout_sec,
		       sizeof(src->watchdog_timeout_sec));
	}
	dest->crc32 =
	    crc32(0, (Bytef *)dest, sizeof(BG_ENVDATA) - sizeof(dest->crc32));
}

static void dump_env(BG_ENVDATA *env)
{
	char buffer[ENV_STRING_LENGTH];
	printf("Values: \n");
	printf("revision: %u\n", env->revision);
	printf("kernel: %s\n", str16to8(buffer, env->kernelfile));
	printf("kernelargs: %s\n", str16to8(buffer, env->kernelparams));
	printf("watchdog timeout: %u seconds\n", env->watchdog_timeout_sec);
	printf("ustate: %u (%s)\n", (uint8_t)env->ustate,
	       ustate2str(env->ustate));
	printf("\n\n");
}

int main(int argc, char **argv)
{
	static struct argp argp_setenv = {options_setenv, parse_opt, NULL, doc};
	static struct argp argp_printenv = {options_printenv, parse_opt, NULL,
					    doc};
	static struct argp *argp;

	bool write_mode = (bool)strstr(argv[0], "bg_setenv");
	if (write_mode) {
		argp = &argp_setenv;
	} else {
		argp = &argp_printenv;
		verbosity = true;
	}

	struct arguments arguments;
	arguments.output_to_file = false;
	arguments.which_part = 0;
	memset((void *)&arguments.tmpdata, IGNORE_MARKER_BYTE,
	       sizeof(BG_ENVDATA));

	error_t e;
	if (e = argp_parse(argp, argc, argv, 0, 0, &arguments)) {
		return e;
	}
	int result = 0;
	if (!arguments.output_to_file) {
		if (!bgenv_init(BGENVTYPE_FAT)) {
			fprintf(stderr,
				"Error initializing FAT environment.\n");
			return 1;
		}
		for (int i = 0; i < ENV_NUM_CONFIG_PARTS; i++) {
			if (verbosity) {
				printf("\n----------------------------\n");
				printf(" Config Partition #%d ", i);
			}
			BGENV *env = bgenv_get_by_index(BGENVTYPE_FAT, i);
			if (env) {
				if (verbosity) {
					dump_env(env->data);
				}
			} else {
				fprintf(stderr, "Error, could not read "
						"environment for index %d\n",
					i);
				return 1;
			}
			bgenv_close(env);
		}
		if (write_mode) {
			BGENV *env_new;
			BGENV *env_current;
			if (auto_update) {
				/* search latest and oldest revision */
				env_current = bgenv_get_latest(BGENVTYPE_FAT);
				if (!env_current) {
					fprintf(stderr, "Failed to retrieve "
							"latest "
							"environment.\n");
					return 1;
				}
				arguments.tmpdata.revision =
				    env_current->data->revision + 1;

				env_new = bgenv_get_oldest(BGENVTYPE_FAT);
				if (!env_new) {
					fprintf(stderr, "Failed to retrieve "
							"oldest "
							"environment.\n");
					return 1;
				}
				if (verbosity) {
					printf("Updating environment with "
					       "revision %u\n",
					       env_new->data->revision);
				}
				/* if auto-updating, copy data from current
				 * revision to new
				 * revision, so that all data which is not
				 * overwritten is
				 * kept
				 */
				if (!env_current->data || !env_new->data) {
					fprintf(stderr, "Invalid environment "
							"data pointer.\n");
					bgenv_close(env_new);
					bgenv_close(env_current);
					return 1;
				}
				memcpy((char *)env_new->data,
				       (char *)env_current->data,
				       sizeof(BG_ENVDATA));
				if (!bgenv_close(env_current)) {
					fprintf(stderr,
						"Error closing environment.\n");
				}
			} else {
				if (part_specified) {
					env_new = bgenv_get_by_index(
					    BGENVTYPE_FAT,
					    arguments.which_part);
				} else {
					env_new =
					    bgenv_get_latest(BGENVTYPE_FAT);
				}
				if (!env_new) {
					fprintf(stderr, "Failed to retrieve "
							"environment by "
							"index.\n");
					return 1;
				}
			}
			update_environment(env_new->data, &arguments.tmpdata);
			if (verbosity) {
				printf("New environment data:\n");
				printf("---------------------\n");
				dump_env(env_new->data);
			}
			if (!bgenv_write(env_new)) {
				fprintf(stderr, "Error storing environment.\n");
				return 1;
			}
			if (!bgenv_close(env_new)) {
				fprintf(stderr, "Error closing environment.\n");
				return 1;
			}
		}
	} else {
		/* arguments.output_to_file can only be true in write mode */
		BG_ENVDATA data;
		memset(&data, 0, sizeof(BG_ENVDATA));
		update_environment(&data, &arguments.tmpdata);
		if (verbosity) {
			dump_env(&data);
		}
		FILE *of = fopen(envfilepath, "wb");
		if (of) {
			if (fwrite(&data, sizeof(BG_ENVDATA), 1, of) != 1) {
				fprintf(stderr,
					"Error writing to output file: %s\n",
					strerror(errno));
				result = errno;
			}
			if (fclose(of)) {
				fprintf(stderr, "Error closing output file.\n");
				result = 1;
			};
			printf("Output written to %s.\n", envfilepath);
		} else {
			fprintf(stderr, "Error opening output file %s (%s).\n",
				envfilepath, strerror(errno));
			result = 1;
		}
		free(envfilepath);
	}

	return result;
}
