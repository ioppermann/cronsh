#ifdef __linux__
	#define _GNU_SOURCE
#endif

#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
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

#define CRONSH_YAML_NONE		0
#define CRONSH_YAML_NUMBER		1
#define CRONSH_YAML_STRING		2

#define CRONSH_OPTION_NONE			0
#define CRONSH_OPTION_SILENT			(1 <<  0)
// capture options
#define CRONSH_OPTION_CAPTURE_STDOUT		(1 <<  1)
#define CRONSH_OPTION_CAPTURE_STDERR		(1 <<  2)
#define CRONSH_OPTION_CAPTURE_ALL		(CRONSH_OPTION_CAPTURE_STDOUT | CRONSH_OPTION_CAPTURE_STDERR)
// sendto options
#define CRONSH_OPTION_SENDTO_STDOUT		(1 <<  3)
#define CRONSH_OPTION_SENDTO_FILE		(1 <<  4)
#define CRONSH_OPTION_SENDTO_PIPE		(1 <<  5)
#define CRONSH_OPTION_SENDTO_ALL		(CRONSH_OPTION_SENDTO_STDOUT | CRONSH_OPTION_SENDTO_FILE | CRONSH_OPTION_SENDTO_PIPE)
#define CRONSH_OPTION_SENDTO_FALLBACK		(1 <<  6)
// sendif options
#define CRONSH_OPTION_SENDIF_STATUS		(1 <<  7)	// status != 0
#define CRONSH_OPTION_SENDIF_STATUS_OK		(1 <<  8)	// status == 0
#define CRONSH_OPTION_SENDIF_STATUS_ANY		(CRONSH_OPTION_SENDIF_STATUS_OK | CRONSH_OPTION_SENDIF_STATUS)
#define CRONSH_OPTION_SENDIF_SIGNAL		(1 <<  9)	// signal != 0
#define CRONSH_OPTION_SENDIF_SIGNAL_OK		(1 << 10)	// signal == 0
#define CRONSH_OPTION_SENDIF_SIGNAL_ANY		(CRONSH_OPTION_SENDIF_SIGNAL_OK | CRONSH_OPTION_SENDIF_SIGNAL)
#define CRONSH_OPTION_SENDIF_STDOUT 		(1 << 11)	// stdout != ''
#define CRONSH_OPTION_SENDIF_STDOUT_NONE	(1 << 12)	// stdout == ''
#define CRONSH_OPTION_SENDIF_STDOUT_ANY		(CRONSH_OPTION_SENDIF_STDOUT | CRONSH_OPTION_SENDIF_STDOUT_NONE)
#define CRONSH_OPTION_SENDIF_STDERR		(1 << 13)	// stderr != ''
#define CRONSH_OPTION_SENDIF_STDERR_NONE	(1 << 14)	// stderr == ''
#define CRONSH_OPTION_SENDIF_STDERR_ANY		(CRONSH_OPTION_SENDIF_STDERR | CRONSH_OPTION_SENDIF_STDERR_NONE)
#define CRONSH_OPTION_SENDIF_ANY		(CRONSH_OPTION_SENDIF_STATUS_ANY | CRONSH_OPTION_SENDIF_SIGNAL_ANY | CRONSH_OPTION_SENDIF_STDOUT_ANY | CRONSH_OPTION_SENDIF_STDERR_ANY)
// cron default options
#define CRONSH_OPTION_CRONDEFAULT		(CRONSH_OPTION_CAPTURE_ALL | CRONSH_OPTION_SENDTO_STDOUT | CRONSH_OPTION_SENDIF_STDOUT | CRONSH_OPTION_SENDIF_STDERR)

#define CRONSH_OPTION(a, o) ((((a) & CRONSH_OPTION_ ## o) == CRONSH_OPTION_ ## o))

#define CRONSH_BUFFER_STEPSIZE		(64 * 1024)

typedef struct {
	char *data;
	size_t size;
	size_t used;
	size_t step;
} buffer_t;

typedef struct {
	unsigned int options;
	char *argv[4];
	
	char *tag;

	pid_t pid;
	pid_t ppid;

	int status;
	int signal;

	struct rusage rusage;

	buffer_t *stdinbuffer;
	buffer_t stdoutbuffer;
	buffer_t stderrbuffer;
} command_t;

typedef struct {
	int loglevel;
	char *log;
	FILE *logfp;

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


/* buffer facility */

int bufferInit(buffer_t *buffer, size_t nbytes);
int bufferFree(buffer_t *buffer);
int bufferReset(buffer_t *buffer);
int bufferAppendBuffer(buffer_t *dst, buffer_t *src);
int bufferAppendString(buffer_t *dst, const char *format, ...);
int bufferAppendBytes(buffer_t *dst, const char *bytes, size_t nbytes);

int bufferStartYAML(buffer_t *dst);
int bufferEndYAML(buffer_t *dst);
int bufferAppendYAML(buffer_t *dst, unsigned int level, const char *key, const char *format, int type, ...);
int bufferAppendYAMLList(buffer_t *dst, unsigned int level, const char *key, int type, char **list);


// http://stackoverflow.com/a/9781275, not very accurate because it's not monotic
#ifdef __MACH__
#include <sys/time.h>
#define CLOCK_MONOTONIC 0
//clock_gettime is not implemented on OSX
int clock_gettime(int clk_id, struct timespec* t) {
    struct timeval now;
    int rv = gettimeofday(&now, NULL);	// better with mach_absolute_time() in <mach/mach_time.h>
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

	while((c = getopt(argc, argv, ":c: :V: :l: :f: :p: :o: :H: h")) != -1) {
		switch(c) {
			case 'c':
				rawcommand = optarg;
				break;
			case 'V':
				setenv("CRONSH_LOGLEVEL", optarg, 1);
				break;
			case 'l':
				setenv("CRONSH_LOG", optarg, 1);
				break;
			case 'f':
				setenv("CRONSH_FILE", optarg, 1);
				break;
			case 'p':
				setenv("CRONSH_PIPE", optarg, 1);
				break;
			case 'o':
				setenv("CRONSH_OPTIONS", optarg, 1);
				break;
			case 'H':
				setenv("CRONSH_HOSTNAME", optarg, 1);
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
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   silent                      = %s", CRONSH_OPTION(command->options, SILENT) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   crondefault                 = %s", CRONSH_OPTION(command->options, CRONDEFAULT) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   capture stdout              = %s", CRONSH_OPTION(command->options, CAPTURE_STDOUT) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   capture stderr              = %s", CRONSH_OPTION(command->options, CAPTURE_STDERR) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send to stdout              = %s", CRONSH_OPTION(command->options, SENDTO_STDOUT) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send to log                 = %s", CRONSH_OPTION(command->options, SENDTO_FILE) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send to pipe                = %s", CRONSH_OPTION(command->options, SENDTO_PIPE) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send to fallback            = %s", CRONSH_OPTION(command->options, SENDTO_FALLBACK) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if status is not 0     = %s", CRONSH_OPTION(command->options, SENDIF_STATUS) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if status is 0         = %s", CRONSH_OPTION(command->options, SENDIF_STATUS_OK) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if status is anything  = %s", CRONSH_OPTION(command->options, SENDIF_STATUS_ANY) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if signal is not 0     = %s", CRONSH_OPTION(command->options, SENDIF_SIGNAL) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if signal is 0         = %s", CRONSH_OPTION(command->options, SENDIF_SIGNAL_OK) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if signal is anything  = %s", CRONSH_OPTION(command->options, SENDIF_SIGNAL_ANY) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if stdout is not empty = %s", CRONSH_OPTION(command->options, SENDIF_STDOUT) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if stdout is empty     = %s", CRONSH_OPTION(command->options, SENDIF_STDOUT_NONE) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if stdout is anything  = %s", CRONSH_OPTION(command->options, SENDIF_STDOUT_ANY) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if stderr is not empty = %s", CRONSH_OPTION(command->options, SENDIF_STDERR) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if stderr is empty     = %s", CRONSH_OPTION(command->options, SENDIF_STDERR_NONE) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send if stderr is anything  = %s", CRONSH_OPTION(command->options, SENDIF_STDERR_ANY) ? "yes" : "no");
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "   send in any case            = %s", CRONSH_OPTION(command->options, SENDIF_ANY) ? "yes" : "no");


	// execute the actual command

	clock_gettime(CLOCK_MONOTONIC, &starttime);

	cronsh_command_spawn(command);
	
	clock_gettime(CLOCK_MONOTONIC, &stoptime);

	// finished executing the actual command


	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "status: %d", command->status);
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "signal: %d", command->signal);
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "stdout: (%d) %s", command->stdoutbuffer.used, command->stdoutbuffer.data);
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "stderr: (%d) %s", command->stderrbuffer.used, command->stderrbuffer.data);

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "runtime: %dms", (int)(difftimespec(&starttime, &stoptime) * 1000));

	bufferInit(&outbuffer, CRONSH_BUFFER_STEPSIZE);
	
	bufferStartYAML(&outbuffer);
	bufferAppendYAML(&outbuffer, 0, "hostname", "%s", CRONSH_YAML_STRING, config.thishostname);
	bufferAppendYAML(&outbuffer, 0, "user", "%s", CRONSH_YAML_STRING, config.thisuser);
	bufferAppendYAML(&outbuffer, 0, "rawcommand", "%s", CRONSH_YAML_STRING, rawcommand);

	bufferAppendYAMLList(&outbuffer, 0, "command", CRONSH_YAML_STRING, command->argv);

	bufferAppendYAML(&outbuffer, 0, "tag", "%s", CRONSH_YAML_STRING, (command->tag != NULL) ? command->tag : "");
	bufferAppendYAML(&outbuffer, 0, "starttime", "%ld", CRONSH_YAML_NUMBER, utcstarttime);
	bufferAppendYAML(&outbuffer, 0, "runtime", "%ld", CRONSH_YAML_NUMBER, (unsigned long)(difftimespec(&starttime, &stoptime) * 1000));
	bufferAppendYAML(&outbuffer, 0, "pid", "%u", CRONSH_YAML_NUMBER, command->pid);
	bufferAppendYAML(&outbuffer, 0, "ppid", "%u", CRONSH_YAML_NUMBER, command->ppid);
	bufferAppendYAML(&outbuffer, 0, "status", "%d", CRONSH_YAML_NUMBER, command->status);
	bufferAppendYAML(&outbuffer, 0, "signal", "%d", CRONSH_YAML_NUMBER, command->signal);

	if(!CRONSH_OPTION(command->options, CAPTURE_STDOUT)) {
		bufferReset(&command->stdoutbuffer);
	}
	
	bufferAppendYAML(&outbuffer, 0, "stdout", "%s", CRONSH_YAML_STRING, command->stdoutbuffer.data);

	if(!CRONSH_OPTION(command->options, CAPTURE_STDERR)) {
		bufferReset(&command->stderrbuffer);
	}
	
	bufferAppendYAML(&outbuffer, 0, "stderr", "%s", CRONSH_YAML_STRING, command->stderrbuffer.data);

	bufferAppendYAML(&outbuffer, 0, "rusage", "", CRONSH_YAML_NONE);

	bufferAppendYAML(&outbuffer, 1, "utime", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_utime.tv_sec * 1000 + command->rusage.ru_utime.tv_usec / 1000);	// user time used
	bufferAppendYAML(&outbuffer, 1, "stime", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_stime.tv_sec * 1000 + command->rusage.ru_stime.tv_usec / 1000);	// system time used
	bufferAppendYAML(&outbuffer, 1, "maxrss", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_maxrss);		// max resident set size
	bufferAppendYAML(&outbuffer, 1, "ixrss", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_ixrss);		// integral shared text memory size
	bufferAppendYAML(&outbuffer, 1, "idrss", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_idrss);		// integral unshared data size
	bufferAppendYAML(&outbuffer, 1, "isrss", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_isrss);		// integral unshared stack size
	bufferAppendYAML(&outbuffer, 1, "minflt", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_minflt);		// page reclaims
	bufferAppendYAML(&outbuffer, 1, "majflt", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_majflt);		// page faults
	bufferAppendYAML(&outbuffer, 1, "nswap", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_nswap);		// swaps
	bufferAppendYAML(&outbuffer, 1, "inblock", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_inblock);		// block input operations
	bufferAppendYAML(&outbuffer, 1, "oublock", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_oublock);		// block output operations
	bufferAppendYAML(&outbuffer, 1, "msgsnd", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_msgsnd);		// messages sent
	bufferAppendYAML(&outbuffer, 1, "msgrcv", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_msgrcv);		// messages received
	bufferAppendYAML(&outbuffer, 1, "nsignals", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_nsignals);	// signals received
	bufferAppendYAML(&outbuffer, 1, "nvcsw", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_nvcsw);		// voluntary context switches
	bufferAppendYAML(&outbuffer, 1, "nivcsw", "%ld", CRONSH_YAML_NUMBER, command->rusage.ru_nivcsw);		// involuntary context switches

	bufferEndYAML(&outbuffer);

	// check if we have to send anything
	int sendif = 0;

	if(CRONSH_OPTION(command->options, SENDIF_STATUS)) { if(command->status != 0) { sendif = 1; } }
	if(CRONSH_OPTION(command->options, SENDIF_STATUS_OK)) { if(command->status == 0) { sendif = 1; } }

	if(CRONSH_OPTION(command->options, SENDIF_SIGNAL)) { if(command->signal != 0) { sendif = 1; } }
	if(CRONSH_OPTION(command->options, SENDIF_SIGNAL_OK)) { if(command->signal == 0) { sendif = 1; } }

	if(CRONSH_OPTION(command->options, SENDIF_STDOUT)) { if(command->stdoutbuffer.used != 0) { sendif = 1; } }
	if(CRONSH_OPTION(command->options, SENDIF_STDOUT_NONE)) { if(command->stdoutbuffer.used == 0) { sendif = 1; } }

	if(CRONSH_OPTION(command->options, SENDIF_STDERR)) { if(command->stderrbuffer.used != 0) { sendif = 1; } }
	if(CRONSH_OPTION(command->options, SENDIF_STDERR_NONE)) { if(command->stderrbuffer.used == 0) { sendif = 1; } }

	// if we don't have to send anything, we're going into silent mode
	if(sendif == 0) {
		cronsh_log(CRONSH_LOGLEVEL_DEBUG, "we shall not send anything");
		command->options |= CRONSH_OPTION_SILENT;
	}

	if(!CRONSH_OPTION(command->options, SILENT)) {
		// write to pipe
		if(CRONSH_OPTION(command->options, SENDTO_PIPE)) {
			cronsh_log(CRONSH_LOGLEVEL_DEBUG, "sending to pipe");

			int rv = cronsh_pipe(config.pipe, &outbuffer);
			if(rv == 0) {
				// if the fallback option was set, don't send it any further
				if(CRONSH_OPTION(command->options, SENDTO_FALLBACK)) {
					command->options &= ~CRONSH_OPTION_SENDTO_ALL;
				}
			}
			else {
				cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "failed sending to pipe (%d)", rv);
			}
		}

		// write to file
		if(CRONSH_OPTION(command->options, SENDTO_FILE)) {
			cronsh_log(CRONSH_LOGLEVEL_DEBUG, "sending to file");

			FILE *fp = fopen(config.file, "a");
			if(fp != NULL) {
				fprintf(fp, "%s", outbuffer.data);
				fclose(fp);

				// if the fallback option was set, don't send it any further
				if(CRONSH_OPTION(command->options, SENDTO_FALLBACK)) {
					command->options &= ~CRONSH_OPTION_SENDTO_ALL;
				}
			}
			else {
				cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "failed sending to file (%s)", strerror(errno));
			}
		}

		// write to stdout
		if(CRONSH_OPTION(command->options, SENDTO_STDOUT)) {
			cronsh_log(CRONSH_LOGLEVEL_DEBUG, "sending to stdout");

			fprintf(stdout, "%s", outbuffer.data);
		}
	}
	
	cronsh_command_free(command);

	bufferFree(&outbuffer);

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "done");

	return 0;
}

int cronsh_pipe(const char *rawpipecommand, buffer_t *buffer) {
	int rv;
	command_t *command;
	
	if(rawpipecommand == NULL) {
		return -1;
	}

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "sending to: %s", rawpipecommand);

	command = cronsh_command_init(rawpipecommand, buffer);
	if(command == NULL) {
		return -1;
	}

	cronsh_command_spawn(command);

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "status: %d", command->status);
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "signal: %d", command->signal);
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "stdout: (%d) %s", command->stdoutbuffer.used, command->stdoutbuffer.data);
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "stderr: (%d) %s", command->stderrbuffer.used, command->stderrbuffer.data);
	
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

		execvp(command->argv[0], command->argv);

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
	if(command->stdinbuffer != NULL) {
		stdinbytes = command->stdinbuffer->used;
	}
	
	if(stdinbytes == 0) {
		close(childstdinfd[1]);
	}

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
		if(rv == 0) {
			continue;
		}

		if(rv == -1) {
			if(errno == EAGAIN) {
				continue;
			}

			cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "select failed: %s", strerror(errno));

			break;
		}

		if(stdinbytes != 0) {
			if(FD_ISSET(childstdinfd[1], &writefds)) {
				bytes = write(childstdinfd[1], &command->stdinbuffer->data[command->stdinbuffer->used - stdinbytes], stdinbytes);
				if(bytes > 0) {
					stdinbytes -= bytes;
				}

				if(stdinbytes == 0) {
					close(childstdinfd[1]);
				}
			}
		}

		if(FD_ISSET(childstdoutfd[0], &readfds)) {
			bytes = read(childstdoutfd[0], buffer, sizeof(buffer));
			if(bytes > 0) {
				bufferAppendBytes(&command->stdoutbuffer, buffer, bytes);
			}
			else {
				break;
			}
		}

		if(FD_ISSET(childstderrfd[0], &readfds)) {
			bytes = read(childstderrfd[0], buffer, sizeof(buffer));
			if(bytes > 0) {
				bufferAppendBytes(&command->stderrbuffer, buffer, bytes);
			}
			else {
				break;
			}
		}
	}

	close(childstdoutfd[0]);
	close(childstderrfd[0]);

	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "waitpid(%d)", pid);

	int status;

	wait4(pid, &status, 0, &command->rusage);

	if(WIFEXITED(status)) {
		command->status = WEXITSTATUS(status);
	}
	else {
		command->status = -1;
	}

	if(WIFSIGNALED(status)) {
		command->signal = WTERMSIG(status);
	}

	return;
}

void cronsh_init(void) {
	char *env;

	memset(&config, 0, sizeof(config_t));
	
	config.pid = getpid();


	/* DEBUG */

	env = getenv("CRONSH_LOGLEVEL");
	if(env != NULL) {
		if(!strcmp("debug", env)) {
			config.loglevel = CRONSH_LOGLEVEL_DEBUG;
		}
		else if(!strcmp("notice", env)) {
			config.loglevel = CRONSH_LOGLEVEL_NOTICE;
		}
		else if(!strcmp("critical", env)) {
			config.loglevel = CRONSH_LOGLEVEL_CRITICAL;
		}
		else {
			config.loglevel = CRONSH_LOGLEVEL_DEFAULT;
		}
	}
	else {
		config.loglevel = CRONSH_LOGLEVEL_DEFAULT;
	}

	env = getenv("CRONSH_LOG");
	if(env != NULL) {
		config.log = strdup(env);
		config.logfp = fopen(config.log, "a");
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
	else {
		config.options = CRONSH_OPTION_NONE;
	}
	
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "OPTIONS: %d", config.options);


	/* HOSTNAME */

	env = getenv("CRONSH_HOSTNAME");
	if(env != NULL) {
		strncpy(config.thishostname, env, sizeof(config.thishostname));
	}
	else if(gethostname(config.thishostname, sizeof(config.thishostname)) != 0) {
		strncpy(config.thishostname, "unknown", sizeof(config.thishostname));
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
			strncpy(config.thisuser, "unknown", sizeof(config.thisuser));
		}
	}

	config.thisuser[sizeof(config.thisuser) - 1] = '\0';
	
	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "USER: %s", config.thisuser);


	cronsh_log(CRONSH_LOGLEVEL_DEBUG, "init done");

	return;
}

command_t *cronsh_command_init(const char *rawcommand, buffer_t *stdinbuffer) {
	if(rawcommand == NULL) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "No command given.");

		return NULL;
	}
	
	command_t *command = (command_t *)calloc(1, sizeof(command_t));
	if(command == NULL) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "Not enough memory for command structure!");

		return NULL;
	}
	
	command->ppid = config.pid;

	/*
		behavior

		look for '#' and split the command there.
		if '#' is escaped with '\', don't split there.

		-> command, hash-options

		execute command with /bin/sh -c 'command'
		parse hash-options
	*/

	int len = strlen(rawcommand);

	char *tcommand = (char *)calloc(len + 1, sizeof(char));
	if(tcommand == NULL) {
		cronsh_log(CRONSH_LOGLEVEL_CRITICAL, "Not enough memory for temporary command string!");

		return NULL;
	}

	command->argv[0] = "/bin/sh";
	command->argv[1] = "-c";
	command->argv[2] = NULL;
	command->argv[3] = NULL;

	char *hashoptions = NULL;

	int i = 0, j = 0;
	for(i = 0; i < len; i++) {
		if(rawcommand[i] == '\\' && rawcommand[i + 1] == '#') {
			i++;

			tcommand[j++] = '#';

			continue;
		}
		else if(rawcommand[i] == '#') {
			if(hashoptions == NULL) {
				hashoptions = &tcommand[j];
			}
		}

		tcommand[j++] = rawcommand[i];
	}

	if(hashoptions != NULL) {
		char *options = NULL;

		if(hashoptions[0] == '#') {
			char *tag = &hashoptions[1];

			len = strlen(hashoptions);
			for(i = 0; i < len; i++) {
				if(isspace(hashoptions[i])) {
					hashoptions[i] = '\0';
					options = &hashoptions[i + 1];
					break;
				}
			}

			if(strlen(tag) != 0) {
				command->tag = strdup(tag);
			}
		}
		else {
			options = hashoptions;
		}

		cronsh_log(CRONSH_LOGLEVEL_DEBUG, "options: %s", (options != NULL) ? options : "");
	
		// set the individual options
		command->options = cronsh_options(config.options, options);

		hashoptions[0] = '\0';
	}
	else {
		command->options = config.options;
	}

	command->argv[2] = strdup(tcommand);

	free(tcommand);

	for(i = 0; command->argv[i] != NULL; i++) {
		cronsh_log(CRONSH_LOGLEVEL_DEBUG, "argv[%d]: %s", i, command->argv[i]);
	}

	command->stdinbuffer = stdinbuffer;
	bufferInit(&command->stdoutbuffer, CRONSH_BUFFER_STEPSIZE);
	bufferInit(&command->stderrbuffer, CRONSH_BUFFER_STEPSIZE);

	return command;
}

void cronsh_command_free(command_t *command) {
	if(command == NULL) {
		return;
	}
	
	bufferFree(&command->stdoutbuffer);
	bufferFree(&command->stderrbuffer);

	if(command->argv[2] != NULL) {
		free(command->argv[2]);
	}

	free(command);
	
	return;
}

unsigned int cronsh_options(unsigned int inoptions, const char *options) {
	int negate, exclusive;
	unsigned int outoptions = inoptions, toption;
	char *ref, *string, *token;

	if(options == NULL) {
		return outoptions;
	}
	
	ref = string = strdup(options);
	if(ref == NULL) {
		return outoptions;
	}
	
	/*
		silent, !silent
		// default cron behaviour. silent if no stdout and stderr
		crondefault, !crondefault
		// what to capture
		capture-stdout, !capture-stdout
		capture-stderr, !capture-stderr
		capture-all, !capture-all
		// where to send to
		sendto-stdout, !sendto-stdout
		sendto-file, !sendto-file
		sendto-pipe, !sendto-pipe
		sendto-all, !sendto-all
		sendto-fallback, !sendto-fallback
		// when to send
		sendif-status, !sendif-status
		sendif-status-ok, !sendif-status-ok
		sendif-status-any, !sendif-status-any
		sendif-signal, !sendif-signal
		sendif-signal-ok, !sendif-signal-ok
		sendif-signal-any, !sendif-signal-any
		sendif-stdout, !sendif-stdout
		sendif-stdout-none, !sendif-stdout-none
		sendif-stdout-any, !sendif-stdout-any
		sendif-stderr, !sendif-stderr
		sendif-stderr-none, !sendif-stderr-none
		sendif-stderr-any, !sendif-stderr-any
		sendif-any, !sendif-any
	*/

	while((token = strsep(&string, " ")) != NULL) {
		negate = 0;
		exclusive = 0;

		if(token[0] == '!' || token[0] == '*') {
			if(token[0] == '!') {
				negate = 1;
			}
			else if(token[0] == '*') {
				exclusive = 1;
			}

			token = &token[1];
		}

		if(strlen(token) == 0) {
			continue;
		}
		
		if(!strcmp(token, "silent")) { toption = CRONSH_OPTION_SILENT; }
		else if(!strcmp(token, "crondefault")) { toption = CRONSH_OPTION_CRONDEFAULT; }

		else if(!strcmp(token, "capture-stdout")) { toption = CRONSH_OPTION_CAPTURE_STDOUT; }
		else if(!strcmp(token, "capture-stderr")) { toption = CRONSH_OPTION_CAPTURE_STDERR; }
		else if(!strcmp(token, "capture-all")) { toption = CRONSH_OPTION_CAPTURE_ALL; }

		else if(!strcmp(token, "sendto-stdout")) { toption = CRONSH_OPTION_SENDTO_STDOUT; }
		else if(!strcmp(token, "sendto-file")) { toption = CRONSH_OPTION_SENDTO_FILE; }
		else if(!strcmp(token, "sendto-pipe")) { toption = CRONSH_OPTION_SENDTO_PIPE; }
		else if(!strcmp(token, "sendto-all")) { toption = CRONSH_OPTION_SENDTO_ALL; }
		else if(!strcmp(token, "sendto-fallback")) { toption = CRONSH_OPTION_SENDTO_FALLBACK; }

		else if(!strcmp(token, "sendif-status")) { toption = CRONSH_OPTION_SENDIF_STATUS; }
		else if(!strcmp(token, "sendif-status-ok")) { toption = CRONSH_OPTION_SENDIF_STATUS_OK; }
		else if(!strcmp(token, "sendif-status-any")) { toption = CRONSH_OPTION_SENDIF_STATUS_ANY; }

		else if(!strcmp(token, "sendif-signal")) { toption = CRONSH_OPTION_SENDIF_SIGNAL; }
		else if(!strcmp(token, "sendif-signal-ok")) { toption = CRONSH_OPTION_SENDIF_SIGNAL_OK; }
		else if(!strcmp(token, "sendif-signal-any")) { toption = CRONSH_OPTION_SENDIF_SIGNAL_ANY; }

		else if(!strcmp(token, "sendif-stdout")) { toption = CRONSH_OPTION_SENDIF_STDOUT; }
		else if(!strcmp(token, "sendif-stdout-none")) { toption = CRONSH_OPTION_SENDIF_STDOUT_NONE; }
		else if(!strcmp(token, "sendif-stdout-any")) { toption = CRONSH_OPTION_SENDIF_STDOUT_ANY; }

		else if(!strcmp(token, "sendif-stderr")) { toption = CRONSH_OPTION_SENDIF_STDERR; }
		else if(!strcmp(token, "sendif-stderr-none")) { toption = CRONSH_OPTION_SENDIF_STDERR_NONE; }
		else if(!strcmp(token, "sendif-stderr-any")) { toption = CRONSH_OPTION_SENDIF_STDERR_ANY; }
		
		else if(!strcmp(token, "sendif-any")) { toption = CRONSH_OPTION_SENDIF_ANY; }

		else {
			toption = CRONSH_OPTION_NONE;
			cronsh_log(CRONSH_LOGLEVEL_NOTICE, "unknown option: %s", token);
		}
		
		if(negate == 1) {
			outoptions &= ~toption;
		}
		else if(exclusive == 1) {
			outoptions = toption;
		}
		else {
			outoptions |= toption;
		}
	}
	
	free(ref);

	return outoptions;
}

void cronsh_log(int loglevel, const char *format, ...) {
	char message[1024 + 1], *l;
	va_list ap;

	if(loglevel < config.loglevel) {
		return;
	}

	if(config.logfp == NULL) {
		config.logfp = stderr;
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
		case CRONSH_LOGLEVEL_DEBUG: l = "DEBUG"; break;
		case CRONSH_LOGLEVEL_NOTICE: l = "NOTICE"; break;
		case CRONSH_LOGLEVEL_CRITICAL: l = "CRITICAL"; break;
		default: l = "UNKNOWN"; break;
	}

	fprintf(config.logfp, "[%s] %s %u: %s\n", datetime, l, config.pid, message);
	
	fflush(config.logfp);

	return;
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
	fprintf(stderr, "\tThis document will be sent to stdout, written to a file (see CRONSH_FILE), or piped to an other command (see\n");
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
	fprintf(stderr, "\thostname: Ingos-MacBook-Air.local                                   - CRONSH_HOSTNAME or gethostname().\n");
	fprintf(stderr, "\tuser: ioppermann                                                    - USER or LOGNAME.\n");
	fprintf(stderr, "\trawcommand: /usr/bin/printf 'hello world' #tag sendto-file          - crontab command line.\n");
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
	fprintf(stderr, "\trusage:                                                             - the values of the rusage struct.\n");
	fprintf(stderr, "\t...\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "OPTIONS\n");
	fprintf(stderr, "\t-c command\n");
	fprintf(stderr, "\t    The command to execute.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-V verbosity\n");
	fprintf(stderr, "\t    Sets the environment variable CRONSH_LOGLEVEL.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-l file\n");
	fprintf(stderr, "\t    Sets the environment variable CRONSH_LOG.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-p command\n");
	fprintf(stderr, "\t    Sets the environment variable CRONSH_PIPE.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-o options\n");
	fprintf(stderr, "\t    Sets the environment variable CRONSH_OPTIONS.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-H hostname\n");
	fprintf(stderr, "\t    Sets the environment variable CRONSH_HOSTNAME.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-h\n");
	fprintf(stderr, "\t    Display the help screen.\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "ENVIRONMENT\n");
	fprintf(stderr, "\tThese environment variables are recognized by cronsh and can be set in the crontab.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_LOGLEVEL\n");
	fprintf(stderr, "\t    Set the logging verbosity for messages written to CRONSH_LOG. Valid verbosity levels are:\n");
	fprintf(stderr, "\t         debug     - very verbose logging, includes warn and critical.\n");
	fprintf(stderr, "\t         notice    - less verbose logging, includes critical.\n");
	fprintf(stderr, "\t         critical  - only logs events that prevent the proper execution of cronsh.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_LOG\n");
	fprintf(stderr, "\t    Path to the file where to write log messages to.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_FILE\n");
	fprintf(stderr, "\t    Write the YAML document to this file if the option 'sendto-file' is given.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_PIPE\n");
	fprintf(stderr, "\t    Write the YAML document to STDIN of this command if the option 'sendto-pipe' is given.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_OPTIONS\n");
	fprintf(stderr, "\t    Set the different options to define the default behaviour of cronsh. The order of the\n");
	fprintf(stderr, "\t    options crucial. Valid options are:\n");
	fprintf(stderr, "\t         silent              - nothing will be send neither to cron, file, nor pipe.\n");
	fprintf(stderr, "\t         crondefault         - mimic the default cron behaviour, i.e. send the YAML to cron only if there's output.\n");
	fprintf(stderr, "\t         capture-stdout      - capture stdout.\n");
	fprintf(stderr, "\t         capture-stderr      - capture stderr.\n");
	fprintf(stderr, "\t         capture-all         - capture stdout and stderr.\n");
	fprintf(stderr, "\t         sendto-stdout       - send the YAML to stdout.\n");
	fprintf(stderr, "\t         sendto-file         - send the YAML to a file (see CRONSH_FILE).\n");
	fprintf(stderr, "\t         sendto-pipe         - send the YAML to the pipe (see CRONSH_PIPE).\n");
	fprintf(stderr, "\t         sendto-all          - send the YAML to cron, file, and pipe.\n");
	fprintf(stderr, "\t         sendto-fallback     - try to send the YAML first to pipe, then to file, and then cron if the previous didn't work.\n");
	fprintf(stderr, "\t         sendif-status       - send the YAML only if the return status is not 0.\n");
	fprintf(stderr, "\t         sendif-status-ok    - send the YAML only if the return status is 0.\n");
	fprintf(stderr, "\t         sendif-status-any   - send the YAML on any return status.\n");
	fprintf(stderr, "\t         sendif-signal       - send the YAML only if the signal status is not 0.\n");
	fprintf(stderr, "\t         sendif-signal-ok    - send the YAML only if the signal status is 0.\n");
	fprintf(stderr, "\t         sendif-signal-any   - send the YAML on any signal status.\n");
	fprintf(stderr, "\t         sendif-stdout       - send the YAML only if there was output to stdout.\n");
	fprintf(stderr, "\t         sendif-stdout-none  - send the YAML only if there was no output to stdout.\n");
	fprintf(stderr, "\t         sendif-stdout-any   - send the YAML on any stdout value.\n");
	fprintf(stderr, "\t         sendif-stderr       - send the YAML only if there was output to stderr.\n");
	fprintf(stderr, "\t         sendif-stderr-none  - send the YAML only if there was no output to stderr.\n");
	fprintf(stderr, "\t         sendif-stderr-any   - send the YAML on any stderr value.\n");
	fprintf(stderr, "\t         sendif-any          - send the YAML in any case.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tCRONSH_HOSTNAME\n");
	fprintf(stderr, "\t    Override the hostname as given by gethostname().\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tUSER / LOGNAME\n");
	fprintf(stderr, "\t    The user who owns this crontab and this command will be run as. See the man page for crontab.\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "BUGS\n");
	fprintf(stderr, "\tNo known bugs (but probably there are some).\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "AUTHOR\n");
	fprintf(stderr, "\t(c) 2014+ Ingo Oppermann\n");

	return;
}

/* buffer facility */

int bufferInit(buffer_t *buffer, size_t nbytes) {
	if(buffer == NULL) {
		return 1;
	}

	buffer->data = NULL;
	buffer->size = 0;
	buffer->used = 0;
	buffer->step = nbytes;

	buffer->data = (char *)calloc(buffer->step + 1, sizeof(char));
	if(buffer->data == NULL) {
		return 1;
	}

	buffer->size = buffer->step;

	return 0;
}

int bufferFree(buffer_t *buffer) {
	if(buffer == NULL) {
		return 1;
	}

	if(buffer->data != NULL) {
		free(buffer->data);
		buffer->data = NULL;
		buffer->size = 0;
		buffer->used = 0;
	}

	return 0;
}

int bufferReset(buffer_t *buffer) {
	if(buffer == NULL) {
		return 1;
	}

	buffer->used = 0;

	return 0;
}

int bufferAppendBytes(buffer_t *dst, const char *bytes, size_t nbytes) {
	size_t size;
	char *data;

	if(dst == NULL) {
		return 1;
	}

	if(bytes == NULL) {
		return 0;
	}

	if(nbytes == 0) {
		return 0;
	}

	// Check if we have to increase the buffer size
	if((dst->used + nbytes) > dst->size) {
		// Pre-allocating some memory. Round up to the next step bound
		size = ((dst->used + nbytes) / dst->step + 1) * dst->step;

		data = (char *)realloc(dst->data, size + 1);
		if(data == NULL) {
			return 1;
		}

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

	if(format == NULL) {
		return 0;
	}

	va_start(ap, format);
	vasprintf(&string, format, ap);
	va_end(ap);

	if(string == NULL) {
		return 0;
	}

	rv = bufferAppendBytes(dst, string, strlen(string));

	free(string);

	return rv;
}

int bufferAppendBuffer(buffer_t *dst, buffer_t *src) {
	if(src == NULL) {
		return 0;
	}

	return bufferAppendBytes(dst, src->data, src->used);
}

int bufferStartYAML(buffer_t *dst) {
	return bufferAppendString(dst, "---\n");
}

int bufferEndYAML(buffer_t *dst) {
	return bufferAppendString(dst, "...\n");
}

int bufferAppendYAML(buffer_t *dst, unsigned int level, const char *key, const char *format, int type, ...) {
	int rv = 0;
	unsigned int n;
	char *string, *t, *p;
	va_list ap;

	if(key == NULL || format == NULL) {
		return 0;
	}

	va_start(ap, type);
	vasprintf(&string, format, ap);
	va_end(ap);

	if(string == NULL) {
		return 0;
	}

	for(n = 0; n < level; n++) {
		rv += bufferAppendBytes(dst, "  ", 2);
	}
	rv += bufferAppendBytes(dst, key, strlen(key));

	if(strcmp(key, "-")) {
		rv += bufferAppendBytes(dst, ": ", 2);
	}
	else {
		rv += bufferAppendBytes(dst, " ", 1);
	}

	if(type == CRONSH_YAML_NUMBER) {
		rv += bufferAppendBytes(dst, string, strlen(string));
	}
	else if(type == CRONSH_YAML_STRING && strlen(string) != 0) {
		t = string;
		n = 0;
		int literal = 0;

		while(*t != '\0') {
			if(*t == '\r') {
				*t = '\n';
			}

			if(iscntrl(*t)) {
				literal = 1;
				break;
			}

			t++;
		}

		if(literal != 0) {
			rv += bufferAppendBytes(dst, "|-\n", 3);

			for(n = 0; n < (level + 1); n++) {
				rv += bufferAppendBytes(dst, "  ", 2);
			}

			size_t len = 0;
			t = p = string;
			while(*t != '\0') {
				len++;
				if(*t == '\n') {
					rv += bufferAppendBytes(dst, p, len);
					for(n = 0; n < (level + 1); n++) {
						rv += bufferAppendBytes(dst, "  ", 2);
					}

					len = 0;
					p = t + 1;
				}
				else if(iscntrl(*t)) {
					rv += bufferAppendBytes(dst, p, len - 1);
					rv += bufferAppendString(dst, "\\x%x", *t);

					len = 0;
					p = t + 1;
				}

				t++;
			}
			rv += bufferAppendBytes(dst, p, len);
		}
		else {
			rv += bufferAppendBytes(dst, "'", 1);
			size_t len = 0;
			t = p = string;
			while(*t != '\0') {
				len++;
				if(*t == '\'') {
					rv += bufferAppendBytes(dst, p, len);
					rv += bufferAppendBytes(dst, "'", 1);

					len = 0;
					p = t + 1;
	                        }

				t++;
			}
			rv += bufferAppendBytes(dst, p, len);
			rv += bufferAppendBytes(dst, "'", 1);
		}
	}

	rv += bufferAppendBytes(dst, "\n", 1);

	free(string);

	return rv;
}

int bufferAppendYAMLList(buffer_t *dst, unsigned int level, const char *key, int type, char **list) {
	int rv = 0;
	unsigned int l;

	if(key == NULL) {
		return 0;
	}

	for(l = 0; l < level; l++) {
		rv += bufferAppendBytes(dst, "  ", 2);
	}
	rv += bufferAppendBytes(dst, key, strlen(key));

	rv += bufferAppendBytes(dst, ":\n", 2);

	for(l = 0; list[l] != NULL; l++) {
		rv += bufferAppendYAML(dst, level + 1, "-", "%s", type, list[l]);
	}

	return rv;
}

