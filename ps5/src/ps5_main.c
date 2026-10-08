// PS5 entry point: picks the title's folders and starts the native host (native_host/), which
// runs the same pd.wasm as the browsers, so PS5 players share online matches with them.
//
//   /app0/assets/pd.ntsc-final.z64   the ROM (copied into the title's assets/ folder)
//   /app0/UserData/                  saves and settings, or /download0/perfectdark if the
//                                    title folder isn't writable
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int pdhostMain(int argc, char **argv);

static int writable(const char *dir)
{
	char probe[512];
	mkdir(dir, 0777);
	snprintf(probe, sizeof(probe), "%s/.writable", dir);
	FILE *f = fopen(probe, "w");
	if (!f) {
		return 0;
	}
	fclose(f);
	unlink(probe);
	return 1;
}

int main(void)
{
	static char save[256], tmp[300];

	snprintf(save, sizeof(save), "%s", "/app0/UserData");
	if (!writable(save)) {
		snprintf(save, sizeof(save), "%s", "/download0/perfectdark");
		mkdir(save, 0777);
	}
	snprintf(tmp, sizeof(tmp), "%s/tmp", save);
	mkdir(tmp, 0777);

	printf("[perfectdark] data /app0/assets, saves %s\n", save);

	char *args[] = {
		"pdhost", "--data", "/app0/assets", "--save", save, "--tmp", tmp, "--fullscreen", NULL,
	};
	int status = pdhostMain((int)(sizeof(args) / sizeof(*args)) - 1, args);

	// returning from main crashes a native title; the user closes it from the home screen
	printf("[perfectdark] game exited (%d)\n", status);
	fflush(stdout);
	for (;;) {
		sleep(1);
	}
}
