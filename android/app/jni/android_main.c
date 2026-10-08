// Android entry point: SDL's Java side (GameActivity) calls SDL_main with the arguments from
// GameActivity.getArguments(). The game's console output goes to logcat (tag "PerfectDark").
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>
#include <SDL.h>

int pdhostMain(int argc, char **argv);

static int g_LogPipe[2];

static void *logThread(void *arg)
{
	char buf[1024];
	size_t len = 0;
	ssize_t n;
	while ((n = read(g_LogPipe[0], buf + len, sizeof(buf) - 1 - len)) > 0) {
		len += (size_t)n;
		buf[len] = '\0';
		char *start = buf, *nl;
		while ((nl = strchr(start, '\n'))) {
			*nl = '\0';
			__android_log_write(ANDROID_LOG_INFO, "PerfectDark", start);
			start = nl + 1;
		}
		len = strlen(start);
		memmove(buf, start, len + 1);
		if (len == sizeof(buf) - 1) {
			__android_log_write(ANDROID_LOG_INFO, "PerfectDark", buf);
			len = 0;
		}
	}
	return NULL;
}

static void redirectOutput(void)
{
	pthread_t thread;
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	if (pipe(g_LogPipe) == 0) {
		dup2(g_LogPipe[1], STDOUT_FILENO);
		dup2(g_LogPipe[1], STDERR_FILENO);
		pthread_create(&thread, NULL, logThread, NULL);
		pthread_detach(thread);
	}
}

int SDL_main(int argc, char *argv[])
{
	redirectOutput();
	return pdhostMain(argc, argv);
}
