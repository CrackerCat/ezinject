#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>
#include <asm/unistd.h>

#define UNUSED(x) (void)x

int func1(int arg1, int arg2) {
	printf("arg1: %d, arg2: %d\n", arg1, arg2);
	return arg1 + arg2;
}

void func2(void) {
	puts("Func2 called!");
}

void onSignal(int sigNum){
	UNUSED(sigNum);
	printf("Error: got signal %d (%s)\n", sigNum, strsignal(sigNum));
	raise(SIGSTOP);
}

void print_maps(){
	pid_t pid = syscall(__NR_getpid);
	char *path;
	asprintf(&path, "/proc/%u/maps", pid);
	do {
		FILE *fh = fopen(path, "r");
		if(!fh){
			return;
		}
		
		char line[256];
		while(!feof(fh)){
			fgets(line, sizeof(line), fh);
			fputs(line, stdout);
		}
		fclose(fh);
	} while(0);
	free(path);
}

int main(int argc, char *argv[])
{
	UNUSED(argv);
	signal(SIGSEGV, onSignal);

	//signal(SIGTRAP, onSignal);

	print_maps();

	int interactive = argc > 1;
	printf("pid=%d\n&main=%p\n&return2=%p\n&func2=%p\n", getpid(), main, func1, func2);
	for(;;)
	{
		int val = func1(0, 1);
		printf("return1() = %d\n", val);
		if(!val)
			break;
		if(interactive)
			fgetc(stdin);
		else
			usleep(1000 * 1000);
	}
	return 0;
}
