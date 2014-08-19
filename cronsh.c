#ifdef __linux__
	#define _GNU_SOURCE
#endif

#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

// gcc cronsh.c -o cronsh -O2 -Wall
// __linux__: add -lrt for clock_gettime()

#define CRONSH_LOGLEVEL_DEBUG		1
#define CRONSH_LOGLEVEL_NOTICE		2
#define CRONSH_LOGLEVEL_CRITICAL	3
#define CRONSH_LOGLEVEL_DEFAULT		CRONSH_LOGLEVEL_DEBUG

#define CRONSH_PARSE_OK			0
#define CRONSH_PARSE_MEMORY		1
#define CRONSH_PARSE_QUOTES		2

#define CRONSH_OPTION_NONE		0
#define CRONSH_OPTION_SILENT		(1 << 0)
#define CRONSH_OPTION_CRONDEFAULT	(1 << 1)
#define CRONSH_OPTION_CAPTURESTDOUT	(1 << 2)
#define CRONSH_OPTION_CAPTURESTDERR	(1 << 3)
#define CRONSH_OPTION_CAPTUREALL	(CRONSH_OPTION_CAPTURESTDOUT | CRONSH_OPTION_CAPTURESTDERR)
#define CRONSH_OPTION_SENDTOCRON	(1 << 4)
#define CRONSH_OPTION_SENDTOFILE	(1 << 5)
#define CRONSH_OPTION_SENDTOPIPE	(1 << 6)
#define CRONSH_OPTION_SENDTOALL		(CRONSH_OPTION_SENDTOCRON | CRONSH_OPTION_SENDTOFILE | CRONSH_OPTION_SENDTOPIPE)
#define CRONSH_OPTION_SENDFALLBACK	(1 << 7)

#define CRONSH_BUFFER_STEPSIZE		(64 * 1024)

typedef struct {
	char *data;
	size_t size;
	size_t used;
	size_t step;
} buffer_t;

typedef struct {
	unsigned int options;
	char **argv;
	
	char *tag;

	pid_t pid;
	pid_t ppid;

	int status;
	int signal;

	buffer_t *stdinbuffer;
	buffer_t stdoutbuffer;
	buffer_t stderrbuffer;
} command_t;

typedef struct {
	int loglevel;
	char *errorlog;
	FILE *errorfp;

	char *file;
	char *pipe;

	unsigned int options;

	char thisuser[256];
	char thishostname[256];
	
	pid_t pid;
} config_t;

config_t config;

void cronsh_init(void);
void cronsh_help(void);
int cronsh_pipe(const char *rawpipecommand, buffer_t *buffer);
void cronsh_log(int loglevel, const char *format, ...);

unsigned int cronsh_options(unsigned int prevoptions, const char *options);

command_t *cronsh_command_init(const char *rawcommand, buffer_t *stdinbuffer);
void cronsh_command_free(command_t *command);
void cronsh_command_options(command_t *command);
void cronsh_command_spawn(command_t *command);

int bufferInit(buffer_t *buffer, size_t nbytes);
int bufferFree(buffer_t *buffer);
int bufferReset(buffer_t *buffer);
int bufferAppendBuffer(buffer_t *dst, buffer_t *src);
int bufferAppendString(buffer_t *dst, const char *format, ...);
int bufferAppendBytes(buffer_t *dst, const char *bytes, size_t nbytes);
int bufferAppendYAML(buffer_t *dst, unsigned int level, const char *key, const char *format, ...);
int bufferAppendYAMLList(buffer_t *dst, unsigned int level, const char *key, char **list);


#ifdef __MACH__
#include <sys/time.h>
#define CLOCK_MONOTONIC 0
//clock_gettime is not implemented on OSX
int clock_gettime(int clk_id, struct timespec* t) {
    struct timeval now;
    int rv = gettimeofday(&now, NULL);
    if (rv) return rv;
    t->tv_sec  = now.tv_sec;
    t->tv_nsec = now.tv_usec * 1000;
    return 0;
}
#endif

float difftimespec(struct timespec *start, struct timespec *stop) {
	struct timespec t;

	if((stop->tv_nsec - start->tv_nsec) < 0) {
		t.tv_sec = stop->tv_sec - start->tv_sec - 1;
		t.tv_nsec = 1000000000 + stop->tv_nsec - start->tv_nsec;
	}
	else {
		t.tv_sec = stop->tv_sec - start->tv_sec;
		t.tv_nsec = stop->tv_nsec - start->tv_nsec;
	}

	return ((float)t.tv_sec + ((float)t.tv_nsec / 1000000000.0));
}


int main(int argc, char **argv) {
	int c;
	char *rawcommand = NULL;
	command_t *command;
	time_t utcstarttime;
	struct timespec starttime;
	struct timespec stoptime;
	buffer_t outbuffer;

	opterr = 0;

	while((c = getopt(argc, argv, ":c: h")) != -1) {
		switch(c) {
			case 'c':
				rawcommand = optarg;
				break;
			case 'h':
				cronsh_help();
				return 0;
			case '?':
				cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "unknown parameter: '%c'.", optopt);
				return 0;
			default:
				break;
		}
	}
	
	cronsh_init();

	if(rawcommand == NULL) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "no command given. Use -c to give a command to execute or check -h for help.");

		return 0;
	}

	utcstarttime = time(NULL);

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "rawcommand: %s", rawcommand);

	// parse command
	command = cronsh_command_init(rawcommand, NULL);
	if(command == NULL) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "failed parsing command.");

		return 0;
	}
	
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "tag: %s", (command->tag != NULL) ? command->tag : "[none]");

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "options: %d", command->options);
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   silent   = %s", (command->options & CRONSH_OPTION_SILENT) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   crondef  = %s", (command->options & CRONSH_OPTION_CRONDEFAULT) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   stdout   = %s", (command->options & CRONSH_OPTION_CAPTURESTDOUT) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   stderr   = %s", (command->options & CRONSH_OPTION_CAPTURESTDERR) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   cron     = %s", (command->options & CRONSH_OPTION_SENDTOCRON) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   log      = %s", (command->options & CRONSH_OPTION_SENDTOFILE) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   pipe     = %s", (command->options & CRONSH_OPTION_SENDTOPIPE) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   fallback = %s", (command->options & CRONSH_OPTION_SENDFALLBACK) ? "yes" : "no");


	// execute the actual command

	clock_gettime(CLOCK_MONOTONIC, &starttime);

	cronsh_command_spawn(command);
	
	clock_gettime(CLOCK_MONOTONIC, &stoptime);

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "status: %d, signal: %d", command->status, command->signal);
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "stdout: (%d) %s", command->stdoutbuffer.used, command->stdoutbuffer.data);
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "stderr: (%d) %s", command->stderrbuffer.used, command->stderrbuffer.data);

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "runtime: %dms", (int)(difftimespec(&starttime, &stoptime) * 1000));

	bufferInit(&outbuffer, CRONSH_BUFFER_STEPSIZE);
	
	bufferAppendString(&outbuffer, "---\n");
	bufferAppendYAML(&outbuffer, 0, "type", "cron");
	bufferAppendYAML(&outbuffer, 0, "hostname", "%s", config.thishostname);
	bufferAppendYAML(&outbuffer, 0, "user", "%s", config.thisuser);
	bufferAppendYAML(&outbuffer, 0, "rawcommand", "%s", rawcommand);

	bufferAppendYAMLList(&outbuffer, 0, "command", command->argv);

	bufferAppendYAML(&outbuffer, 0, "tag", "%s", (command->tag != NULL) ? command->tag : "");
	bufferAppendYAML(&outbuffer, 0, "starttime", "%ld", utcstarttime);
	bufferAppendYAML(&outbuffer, 0, "runtime", "%ld", (int)(difftimespec(&starttime, &stoptime) * 1000));
	bufferAppendYAML(&outbuffer, 0, "pid", "%u", command->pid);
	bufferAppendYAML(&outbuffer, 0, "ppid", "%u", command->ppid);
	bufferAppendYAML(&outbuffer, 0, "status", "%d", command->status);
	bufferAppendYAML(&outbuffer, 0, "signal", "%d", command->signal);

	if(!(command->options & CRONSH_OPTION_CAPTURESTDOUT))
		bufferReset(&command->stdoutbuffer);
	
	bufferAppendYAML(&outbuffer, 0, "stdout", "%s", command->stdoutbuffer.data);

	if(!(command->options & CRONSH_OPTION_CAPTURESTDERR))
		bufferReset(&command->stderrbuffer);
	
	bufferAppendYAML(&outbuffer, 0, "stderr", "%s", command->stderrbuffer.data);

	bufferAppendString(&outbuffer, "...\n");

	if(!(command->options & CRONSH_OPTION_SILENT)) {
		// write to pipe
		if(command->options & CRONSH_OPTION_SENDTOPIPE) {
			int rv = cronsh_pipe(config.pipe, &outbuffer);
			cronsh_log(CRONSH_LOGLEVEL_DEBUG, "sending to pipe (%d)", rv);

			if(rv == 0) {
				// if the fallback option was set, don't send it any further
				if(command->options & CRONSH_OPTION_SENDFALLBACK)
					command->options &= ~CRONSH_OPTION_SENDTOALL;
			}
		}

		// write to log
		if(command->options & CRONSH_OPTION_SENDTOFILE) {
			cronsh_log(CRONSH_LOGLEVEL_DEBUG, "sending to logfile");
			FILE *fp = fopen(config.file, "a");
			if(fp != NULL) {
				fprintf(fp, "%s", outbuffer.data);
				fclose(fp);

				// if the fallback option was set, don't send it any further
				if(command->options & CRONSH_OPTION_SENDFALLBACK)
					command->options &= ~CRONSH_OPTION_SENDTOALL;
			}
		}

		// write to cron
		if(command->options & CRONSH_OPTION_CRONDEFAULT) {
			if(command->stdoutbuffer.used == 0 && command->stderrbuffer.used == 0 && command->status == 0)
				command->options &= ~CRONSH_OPTION_SENDTOCRON;
		}
		
		if(command->options & CRONSH_OPTION_SENDTOCRON) {
			cronsh_log(CRONSH_LOGLEVEL_DEBUG, "sending to cron");
			fprintf(stdout, "%s", outbuffer.data);
		}
	}
	
	cronsh_command_free(command);

	bufferFree(&outbuffer);

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "done\n");

	return 0;
}

void cronsh_help(void) {
	fprintf(stderr, "NAME\n");
	fprintf(stderr, "\tcronsh - a shell for executing cron jobs\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "SYNOPSIS\n");
	fprintf(stderr, "\tcronsh -c command -h\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "DESCRIPTION\n");
	fprintf(stderr, "\tcronsh (or cronshell) is a shell for executing cron jobs. It collects stdout, stderr, the return code, and other\n");
	fprintf(stderr, "\tvalues from the command it runs. At the end of executing the command, all captured data is arranged in a YAML document.\n");
	fprintf(stderr, "\tThis document will be sent to cron, written to a log file (see CRONSH_MESSAGELOG), or piped to an other command (see\n");
	fprintf(stderr, "\tCRONSH_PIPE). Set CRONSH_OPTIONS for specifying the default behaviour.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tIn the crontab, point the SHELL environment variable to cronsh. cron will then execute cronsh by calling\n");
	fprintf(stderr, "\tit with the -c option and the command line as its value. The anatomy of the command line is:\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t   [executable] [arguments]? #[tag]? [options]?\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tEverything after (and including) the first # will not be part of the executed command and its arguments. Directly following\n");
	fprintf(stderr, "\tthe # up to the first blank or the end of the line is considered as a tag for this command. Associating a tag is optional.\n");
	fprintf(stderr, "\tThe options for the command are inherited from CRONSH_OPTIONS and can be modified by specifying addional options. This is\n");
	fprintf(stderr, "\toptional.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tThe anatomy of the YAML is:\n");

	fprintf(stderr, "\n");
	fprintf(stderr, "\t---\n");
	fprintf(stderr, "\ttype: cron\n");
	fprintf(stderr, "\thostname: Ingos-MacBook-Air.local                                   - CRONSH_HOSTNAME or gethostname().\n");
	fprintf(stderr, "\tuser: ioppermann                                                    - USER or LOGNAME.\n");
	fprintf(stderr, "\trawcommand: /usr/bin/printf \"hello world\" #tag sendtolog          - crontab command line.\n");
	fprintf(stderr, "\tcommand:                                                            - executed command.\n");
	fprintf(stderr, "\t  - /usr/bin/printf\n");
	fprintf(stderr, "\t  - hello world\n");
	fprintf(stderr, "\ttag: tag                                                            - tag as specified in rawcommand.\n");
	fprintf(stderr, "\tstarttime: 1396712280                                               - UNIX timestamp.\n");
	fprintf(stderr, "\truntime: 3                                                          - runtime in milliseconds.\n");
	fprintf(stderr, "\tpid: 4471                                                           - PID of the excuted command.\n");
	fprintf(stderr, "\tppid: 4470                                                          - PID of cronsh.\n");
	fprintf(stderr, "\tstatus: 0                                                           - exit status of executed command.\n");
	fprintf(stderr, "\tsignal: 0                                                           - signal that caused exiting.\n");
	fprintf(stderr, "\tstdout: hello world                                                 - captured stdout.\n");
	fprintf(stderr, "\tstderr:                                                             - captured stderr.\n");
	fprintf(stderr, "\t...\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "OPTIONS\n");
	fprintf(stderr, "\t-c command\n");
	fprintf(stderr, "\t    The command to execute.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-h\n");
	fprintf(stderr, "\t    Display the help screen.\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "ENVIRONMENT\n");
	fprintf(stderr, "\tThese environment variables are recognized by cronsh and can be set in the crontab.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_LOGLEVEL\n");
	fprintf(stderr, "\t    Set the logging verbosity for messages written to CRONSH_ERRORLOG. Valid verbosity levels are:\n");
	fprintf(stderr, "\t         debug     - very verbose logging, includes warn and critical.\n");
	fprintf(stderr, "\t         notice    - less verbose logging, includes critical.\n");
	fprintf(stderr, "\t         critical  - only logs events that prevent the proper execution of cronsh.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_ERRORLOG\n");
	fprintf(stderr, "\t    Path to the file where to write log messages to.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_FILE\n");
	fprintf(stderr, "\t    Path to the file where to write the YAML documents from every command to if the option 'sendtolog' is given.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_PIPE\n");
	fprintf(stderr, "\t    A command that will be executed after the cron job finished. The YAML document will be written to stdin of this command.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_OPTIONS\n");
	fprintf(stderr, "\t    Set the different options to define the behaviour of crons. valid options are:\n");
	fprintf(stderr, "\t         silent          - nothing will be send to cron, log, or pipe.\n");
	fprintf(stderr, "\t         crondefault     - mimic the default cron behaviour, i.e. send the YAML to cron only if there's output.\n");
	fprintf(stderr, "\t         capturestdout   - capture stdout.\n");
	fprintf(stderr, "\t         capturestderr   - capture stderr.\n");
	fprintf(stderr, "\t         captureall      - capture stdout and stderr.\n");
	fprintf(stderr, "\t         sendtocron      - send the YAML to cron.\n");
	fprintf(stderr, "\t         sendtolog       - send the YAML to a log file (see CRON_MESSAGELOG).\n");
	fprintf(stderr, "\t         sendtopipe      - send the YAML to the pipe (see CRONSH_PIPE).\n");
	fprintf(stderr, "\t         sendfallback    - send the YAML first to pipe, then to log, and then cron if the previous didn't work.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_HOSTNAME\n");
	fprintf(stderr, "\t    Override the hostname as given by gethostname().\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tUSER / LOGNAME\n");
	fprintf(stderr, "\n    The user who owns this crontab and this command will be run as. See the man page for crontab.\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "BUGS\n");
	fprintf(stderr, "\tNo known bugs (but probably there are some).\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "AUTHOR\n");
	fprintf(stderr, "\t(c) 2014+ Ingo Oppermann\n");

	return;
}

int cronsh_pipe(const char *rawpipecommand, buffer_t *buffer) {
	int rv;
	command_t *command;
	
	if(rawpipecommand == NULL)
		return -1;

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "sending to: %s", rawpipecommand);

	command = cronsh_command_init(rawpipecommand, buffer);
	if(command == NULL)
		return -1;

	cronsh_command_spawn(command);
	
	cronsh_command_free(command);
	
	rv = command->status;

	return rv;
}


void cronsh_command_spawn(command_t *command) {
/*
	- pipes for stdin, stdout, stderr
	- init the buffers
	- fork
	- dup2 for descriptors
	- execve
	- select on stdin, stdout, stderr to write to stdin of child, read from stdout, stderr from child, non-blocking
	- waitpid for child
	- capture the exit code
*/
	pid_t pid;
	int childstdinfd[2], childstdoutfd[2], childstderrfd[2];

	pipe(childstdinfd);
	pipe(childstdoutfd);
	pipe(childstderrfd);

	pid = fork();

	if(pid < 0) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "failed spawning child: %s", strerror(errno));

		command->status = -1;

		return;
	}

	if(pid == 0) {
		// redirect stdin
		dup2(childstdinfd[0], 0);
		close(childstdinfd[1]);

		// redirect stdout
		close(childstdoutfd[0]);
		dup2(childstdoutfd[1], 1);

		// redirect stderr
		close(childstderrfd[0]);
		dup2(childstderrfd[1], 2);

		execve(command->argv[0], command->argv, NULL);

		fprintf(stderr, "failed to execute '%s': %s (%d)", command->argv[0], strerror(errno), errno);

		_exit(-1);
	}
	
	command->pid = pid;

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "spawned child (%d)", pid);

	close(childstdinfd[0]);
	close(childstdoutfd[1]);
	close(childstderrfd[1]);

	struct timeval timeout;

	fd_set readfds;
	fd_set writefds;

	size_t stdinbytes = 0;
	if(command->stdinbuffer != NULL)
		stdinbytes = command->stdinbuffer->used;
	
	if(stdinbytes == 0)
		close(childstdinfd[1]);

	int rv, bytes, nfds;
	char buffer[64 * 1024];

	while(1) {
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);

		FD_SET(childstdoutfd[0], &readfds);
		FD_SET(childstderrfd[0], &readfds);
		
		nfds = 0;
		nfds = (childstdoutfd[0] > nfds) ? childstdoutfd[0] : nfds;
		nfds = (childstderrfd[0] > nfds) ? childstderrfd[0] : nfds;
		
		if(stdinbytes != 0) {
			FD_SET(childstdinfd[1], &writefds);
			
			nfds = (childstdinfd[1] > nfds) ? childstdinfd[1] : nfds;
		}
		
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		rv = select(nfds + 1, &readfds, &writefds, NULL, &timeout);
		if(rv == 0)
			continue;

		if(rv == -1) {
			if(errno == EAGAIN)
				continue;

			cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "select failed: %s", strerror(errno));

			break;
		}

		if(stdinbytes != 0) {
			if(FD_ISSET(childstdinfd[1], &writefds)) {
				bytes = write(childstdinfd[1], &command->stdinbuffer->data[command->stdinbuffer->used - stdinbytes], stdinbytes);
				if(bytes > 0)
					stdinbytes -= bytes;

				if(stdinbytes == 0)
					close(childstdinfd[1]);
			}
		}

		if(FD_ISSET(childstdoutfd[0], &readfds)) {
			bytes = read(childstdoutfd[0], buffer, sizeof(buffer));
			if(bytes > 0)
				bufferAppendBytes(&command->stdoutbuffer, buffer, bytes);
			else
				break;
		}

		if(FD_ISSET(childstderrfd[0], &readfds)) {
			bytes = read(childstderrfd[0], buffer, sizeof(buffer));
			if(bytes > 0)
				bufferAppendBytes(&command->stderrbuffer, buffer, bytes);
			else
				break;
		}
	}

	close(childstdoutfd[0]);
	close(childstderrfd[0]);

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "waitpid(%d)", pid);

	int status;

	waitpid(pid, &status, 0);

	if(WIFEXITED(status))
		command->status = WEXITSTATUS(status);
	else
		command->status = -1;

	if(WIFSIGNALED(status))
		command->signal = WTERMSIG(status);

	return;
}

void cronsh_init(void) {
	char *env;

	memset(&config, 0, sizeof(config_t));
	
	config.pid = getpid();


	/* DEBUG */

	env = getenv("CRONSH_LOGLEVEL");
	if(env != NULL) {
		if(!strcmp("debug", env))
			config.loglevel = CRONSH_LOGLEVEL_DEBUG;
		else if(!strcmp("notice", env))
			config.loglevel = CRONSH_LOGLEVEL_NOTICE;
		else if(!strcmp("critical", env))
			config.loglevel = CRONSH_LOGLEVEL_CRITICAL;
		else
			config.loglevel = CRONSH_LOGLEVEL_DEFAULT;
	}
	else
		config.loglevel = CRONSH_LOGLEVEL_DEFAULT;

	env = getenv("CRONSH_ERRORLOG");
	if(env != NULL) {
		config.errorlog = strdup(env);
		config.errorfp = fopen(config.errorlog, "a");
	}

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "init start");


	/* FILE */

	env = getenv("CRONSH_FILE");
	if(env != NULL) {
		config.file = strdup(env);
		cronsh_log(CRONSH_LOGLEVEL_DEBUG, "FILE: %s", config.file);
	}


	/* PIPE */

	env = getenv("CRONSH_PIPE");
	if(env != NULL) {
		config.pipe = strdup(env);
		cronsh_log(CRONSH_LOGLEVEL_DEBUG, "PIPE: %s", config.pipe);
	}
	
	
	/* OPTIONS */
	
	env = getenv("CRONSH_OPTIONS");
	if(env != NULL) {
		config.options = cronsh_options(CRONSH_OPTION_NONE, env);
	}
	else
		config.options = CRONSH_OPTION_NONE;
	
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "OPTIONS: %d", config.options);


	/* HOSTNAME */

	env = getenv("CRONSH_HOSTNAME");
	if(env != NULL) {
		strncpy(config.thishostname, env, sizeof(config.thishostname));
	}
	else if(gethostname(config.thishostname, sizeof(config.thishostname)) != 0) {
		strncpy(config.thishostname, "[unknown]", sizeof(config.thishostname));
	}

	config.thishostname[sizeof(config.thishostname) - 1] = '\0';

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "HOSTNAME: %s", config.thishostname);
	

	/* USER */
	
	env = getenv("USER");
	if(env != NULL) {
		strncpy(config.thisuser, env, sizeof(config.thisuser));
	}
	else {
		env = getenv("LOGNAME");
		if(env != NULL) {
			strncpy(config.thisuser, env, sizeof(config.thisuser));
		}
		else {
			strncpy(config.thisuser, "[unknown]", sizeof(config.thisuser));
		}
	}

	config.thisuser[sizeof(config.thisuser) - 1] = '\0';
	
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "USER: %s", config.thisuser);


	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "init done");

	return;
}

command_t *cronsh_command_init(const char *rawcommand, buffer_t *stdinbuffer) {
	int nargs, len, i, j, inquotes, inoptions;
	char last, current;
	char *tcommand;
	command_t *command;
	
	command = (command_t *)calloc(1, sizeof(command_t));
	if(command == NULL) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "Not enough memory for command structure!");

		return NULL;
	}
	
	command->ppid = config.pid;

	// collapse & separate individual commands
	// i.e. to put \0 between every individual command
/*
	behaviour
	:quotes: = " '
	:slash: = \
	:space: = \t \r \n 0x20
	:any: = everything else
	:null: = \0

	:slash: is for escaping
	specials chars: :quotes: :slash: :space: :any:
	rules:
	   !inquotes
	     :slash::quotes: -> :quotes: and no inquotes-toggle
	     :slash::slash: -> :slash:
	     :slash::space: -> :space: and no separation
	     :slash::any: -> :slash::any:
	     :slash::null: -> :slash::null:
	     :space:+ -> :space:
	     :any: -> :any:
	   inquotes
	     :slash::quotes: -> :quotes: and no inquote-toggle
	     :slash::slash: -> :slash:
	     :slash::space: -> :slash::space:
	     :slash::any: -> :slash::any:
	     :slash::null: -> :slash::null:
	     :space: -> :space:
	     :any: -> :any:

	cases:
	  "hello world" -> arg[0] = hello world
	  \"hello world" -> arg[0] = "hello | arg[1] = world -> error: mis-matching quotes
	  "hello world\" -> arg[0] = hello world" -> error: mis-matching quotes
	  "hello world\\" -> arg[0] = hello world\
	  "hello wo\rld" -> arg[0] = hello wo\rld
	  "hello\ world" -> arg[0] = hello\ world
	  hello wo\rld -> arg[0] = hello | arg[1] = wo\rld
	  hello\ world -> arg[0] = hello world
*/

	len = strlen(rawcommand);
	tcommand = (char *)calloc(len + 1, sizeof(char));
	if(tcommand == NULL) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "Not enough memory for temporary command!");
		
		free(command);

		return NULL;
	}

	nargs = 0;
	last = '\0';
	inquotes = 0;
	j = 0;
	for(i = 0; i < len; i++) {
		if(rawcommand[i] == '\0')
			break;

		current = rawcommand[i];

		// normalizing chars

		// :space: is separator of commands, ignored if in :quotes:

		// treat \t, \n, and \r the same as :space:
		if(current == '\t' || current == '\n' || current == '\r')
			current = ' ';

		// treat " and ' as :quotes:
		if(current == '"' || current == '\'')
			current = '"';

		if(inquotes == 0) {
			if(last == '\\') {
				if(current == '"' || current == '\\' || current == ' ') {
					current = '\0';
				}
				else {
					tcommand[j++] = last;
				}
			}
			else {
				if(current == '\\') {
					last = current;
					continue;
				}

				if(current == '"') {
					cronsh_log(CRONSH_LOGLEVEL_DEBUG, "inquotes: %d, last: %c, current: %c", inquotes, last, current);
					inquotes = 1;
					continue;
				}

				if(current == ' ') {
					if(last == ' ')
						continue;

					last = current;
					tcommand[j++] = '\0';
					nargs++;

					continue;
				}
			}
		}
		else {
			if(last == '\\') {
				if(current == '"' || current == '\\') {
					current = '\0';
				}
				else {
					tcommand[j++] = last;
				}
			}
			else {
				if(current == '\\') {
					last = current;
					continue;
				}

				if(current == '"') {
					cronsh_log(CRONSH_LOGLEVEL_DEBUG, "inquotes: %d, last: %c, current: %c", inquotes, last, current);
					inquotes = 0;
					continue;
				}
			}
		}

		// copy the current char
		tcommand[j++] = rawcommand[i];

		last = current;
	}

	nargs++;
	tcommand[j] = '\0';

	if(inquotes == 1) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "mis-matching quotes!");

		free(tcommand);
		free(command);

		return NULL;
	}

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "nargs: %d", nargs);

	// trimming & split to args

	command->argv = (char **)calloc(nargs + 1, sizeof(char *));
	if(command->argv == NULL) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "Not enough memory for arguments!");

		free(tcommand);
		free(command);

		return NULL;
	}

	nargs = 0;
	last = '\0';

	for(i = 0; i < len; i++) {
		if(last == '\0' && tcommand[i] != '\0')
			command->argv[nargs++] = strdup(&tcommand[i]);

		last = tcommand[i];
	}

	command->argv[nargs] = NULL;

	for(i = 0; command->argv[i] != NULL; i++) {
		cronsh_log(CRONSH_LOGLEVEL_DEBUG, "argv[%d]: %s", i, command->argv[i]);
	}
	
	
	// concat the options

	memset(tcommand, 0, len);
	
	inoptions = 0;
	for(i = 0; command->argv[i] != NULL; i++) {
		if(inoptions == 0) {
			if(command->argv[i][0] != '#')
				continue;

			if(strlen(command->argv[i]) > 1)
				command->tag = strdup(&command->argv[i][1]);

			command->argv[i] = NULL;
			inoptions = 1;

			continue;
		}
		
		strncat(tcommand, command->argv[i], strlen(command->argv[i]));
		strncat(tcommand, " ", 1);
	}
	
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "options: %s", tcommand);
	
	// set the individual options
	command->options = cronsh_options(config.options, tcommand);
	
	free(tcommand);

	command->stdinbuffer = stdinbuffer;
	bufferInit(&command->stdoutbuffer, CRONSH_BUFFER_STEPSIZE);
	bufferInit(&command->stderrbuffer, CRONSH_BUFFER_STEPSIZE);

	return command;
}

void cronsh_command_free(command_t *command) {
	if(command == NULL)
		return;
	
	bufferFree(&command->stdoutbuffer);
	bufferFree(&command->stderrbuffer);

	int i = 0;
	for(i = 0; command->argv[i] != NULL; i++)
		free(command->argv[i]);
	free(command->argv);
	
	return;
}

unsigned int cronsh_options(unsigned int inoptions, const char *options) {
	int negate;
	unsigned int outoptions = inoptions, toption;
	char *ref, *string, *token;

	if(options == NULL)
		return outoptions;
	
	ref = string = strdup(options);
	if(ref == NULL)
		return outoptions;
	
	/*
		silent, !silent
		// default cron behaviour. silent if no stdout and stderr
		crondefault, !crondefault
		// what to capture
		capturestdout, !capturestdout
		capturestderr, !capturestderr
		captureall, !captureall
		// where to send to
		sendtocron, !sendtocron
		sendtofile, !sendtofile
		sendtopipe, !sendtopipe
		sendtoall, !sendtoall
		sendfallback, !sendfallback
	*/

	while((token = strsep(&string, " ")) != NULL) {
		negate = 0;
		if(token[0] == '!') {
			token = &token[1];
			negate = 1;
		}

		if(strlen(token) == 0)
			continue;
		
		if(!strcmp(token, "silent")) toption = CRONSH_OPTION_SILENT;
		else if(!strcmp(token, "crondefault")) toption = CRONSH_OPTION_CRONDEFAULT;
		else if(!strcmp(token, "capturestdout")) toption = CRONSH_OPTION_CAPTURESTDOUT;
		else if(!strcmp(token, "capturestderr")) toption = CRONSH_OPTION_CAPTURESTDERR;
		else if(!strcmp(token, "captureall")) toption = CRONSH_OPTION_CAPTUREALL;
		else if(!strcmp(token, "sendtocron")) toption = CRONSH_OPTION_SENDTOCRON;
		else if(!strcmp(token, "sendtofile")) toption = CRONSH_OPTION_SENDTOFILE;
		else if(!strcmp(token, "sendtopipe")) toption = CRONSH_OPTION_SENDTOPIPE;
		else if(!strcmp(token, "sendtoall")) toption = CRONSH_OPTION_SENDTOALL;
		else if(!strcmp(token, "sendfallback")) toption = CRONSH_OPTION_SENDFALLBACK;
		else {
			toption = CRONSH_OPTION_NONE;
			cronsh_log(CRONSH_LOGLEVEL_NOTICE, "unknown option: %s", token);
		}
		
		if(negate == 1)
			outoptions &= ~toption;
		else
			outoptions |= toption;
	}
	
	free(ref);

	return outoptions;
}

void cronsh_log(int loglevel, const char *format, ...) {
	char message[1024 + 1], *l;
	va_list ap;

	if(loglevel < config.loglevel)
		return;

	if(config.errorfp == NULL) {
		config.errorfp = stderr;
	}
	
	time_t clock = time(NULL);
	struct tm timeptr;
	char datetime[32];

	gmtime_r(&clock, &timeptr);
	strftime(datetime, sizeof(datetime), "%F %T", &timeptr);

	va_start(ap, format);
	vsnprintf(message, sizeof(message), format, ap);
	va_end(ap);

	switch(loglevel) {
		case CRONSH_LOGLEVEL_DEBUG: l = "DEBG"; break;
		case CRONSH_LOGLEVEL_NOTICE: l = "NOTE"; break;
		case CRONSH_LOGLEVEL_CRITICAL: l = "CRIT"; break;
		default: l = "UNKN"; break;
	}

	fprintf(config.errorfp, "[%s] %s %u: %s\n", datetime, l, config.pid, message);
	
	fflush(config.errorfp);

	return;
}

/* buffer facility */

int bufferInit(buffer_t *buffer, size_t nbytes) {
	if(buffer == NULL)
		return 1;

	buffer->data = NULL;
	buffer->size = 0;
	buffer->used = 0;
	buffer->step = nbytes;

	buffer->data = (char *)calloc(buffer->step + 1, sizeof(char));
	if(buffer->data == NULL)
		return 1;

	buffer->size = buffer->step;

	return 0;
}

int bufferFree(buffer_t *buffer) {
	if(buffer == NULL)
		return 1;

	if(buffer->data != NULL) {
		free(buffer->data);
		buffer->data = NULL;
		buffer->size = 0;
		buffer->used = 0;
	}

	return 0;
}

int bufferReset(buffer_t *buffer) {
	if(buffer == NULL)
		return 1;

	buffer->used = 0;

	return 0;
}

int bufferAppendBytes(buffer_t *dst, const char *bytes, size_t nbytes) {
	size_t size;
	char *data;

	if(dst == NULL)
		return 1;

	if(bytes == NULL)
		return 0;

	if(nbytes == 0)
		return 0;

	// Check if we have to increase the buffer size
	if((dst->used + nbytes) > dst->size) {
		// Pre-allocating some memory. Round up to the next step bound
		size = ((dst->used + nbytes) / dst->step + 1) * dst->step;

		data = (char *)realloc(dst->data, size + 1);
		if(data == NULL)
			return 1;

		dst->data = data;
		dst->size = size;
	}

	// Copy the stuff into the buffer
	memcpy(&dst->data[dst->used], bytes, nbytes);

	dst->used += nbytes;

	dst->data[dst->used] = '\0';

	return 0;
}

int bufferAppendString(buffer_t *dst, const char *format, ...) {
	int rv;
	char *string;
	va_list ap;

	if(format == NULL)
		return 0;

	va_start(ap, format);
	vasprintf(&string, format, ap);
	va_end(ap);

	if(string == NULL)
		return 0;

	rv = bufferAppendBytes(dst, string, strlen(string));

	free(string);

	return rv;
}

int bufferAppendBuffer(buffer_t *dst, buffer_t *src) {
	if(src == NULL)
		return 0;

	return bufferAppendBytes(dst, src->data, src->used);
}

int bufferAppendYAML(buffer_t *dst, unsigned int level, const char *key, const char *format, ...) {
	int rv = 0;
	unsigned int n;
	char *string, *t, *p;
	va_list ap;

	if(key == NULL || format == NULL)
		return 0;

	va_start(ap, format);
	vasprintf(&string, format, ap);
	va_end(ap);

	if(string == NULL)
		return 0;

	for(n = 0; n < level; n++)
		rv += bufferAppendBytes(dst, "    ", 4);
	rv += bufferAppendBytes(dst, key, strlen(key));

	if(strcmp(key, "-"))
		rv += bufferAppendBytes(dst, ": ", 2);
	else
		rv += bufferAppendBytes(dst, " ", 1);

	t = string;
	n = 0;
	while(*t != '\0') {
		if(*t == '\r')
			*t = '\n';

		if(*t == '\n')
			n++;

		t++;
	}

	if(n != 0) {
		rv += bufferAppendBytes(dst, ">\n", 2);

		for(n = 0; n < (level + 1); n++)
			rv += bufferAppendBytes(dst, "    ", 4);

		size_t len = 0;
		t = p = string;
		while(*t != '\0') {
			len++;
			if(*t == '\n') {
				rv += bufferAppendBytes(dst, p, len);
				for(n = 0; n < (level + 1); n++)
					rv += bufferAppendBytes(dst, "    ", 4);

				len = 0;
				p = t + 1;
                        }

			t++;
		}
		rv += bufferAppendBytes(dst, p, len);
	}
	else {
		rv += bufferAppendBytes(dst, string, strlen(string));
	}

	rv += bufferAppendBytes(dst, "\n", 1);

	free(string);

	return rv;
}

int bufferAppendYAMLList(buffer_t *dst, unsigned int level, const char *key, char **list) {
	int rv = 0;
	unsigned int l;

	if(key == NULL)
		return 0;

	for(l = 0; l < level; l++)
		rv += bufferAppendBytes(dst, "    ", 4);
	rv += bufferAppendBytes(dst, key, strlen(key));

	rv += bufferAppendBytes(dst, ":\n", 2);

	for(l = 0; list[l] != NULL; l++) {
		rv += bufferAppendYAML(dst, level + 1, "-", "%s", list[l]);
	}

	return rv;
}

