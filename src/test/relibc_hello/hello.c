/*
 * A C program on relibc on Genode. No headers yet: relibc's are generated
 * by cbindgen (relibc's 'make headers'), declare what is used.
 */

typedef unsigned long size_t;
struct timespec { long tv_sec; long tv_nsec; };

int    printf(char const *, ...);
void  *malloc(size_t);
void   free(void *);
int    snprintf(char *, size_t, char const *, ...);
int    clock_gettime(int, struct timespec *);
int    nanosleep(struct timespec const *, struct timespec *);
char  *getenv(char const *);

typedef struct FILE FILE;
FILE  *fopen(char const *, char const *);
int    fclose(FILE *);
int    fprintf(FILE *, char const *, ...);
char  *fgets(char *, int, FILE *);
int    fseek(FILE *, long, int);
long   ftell(FILE *);
int   *__errno_location(void);
char  *strerror(int);

int main(int argc, char **argv)
{
	printf("Hello from relibc on Genode (%d args, argv[0] = %s)\n", argc, argv[0]);

	char *buf = malloc(64);
	snprintf(buf, 64, "malloc: %p, %d%%, %.3f", (void *)buf, 42, 3.14159);
	printf("%s\n", buf);
	free(buf);

	struct timespec a, b, ten_ms = { 0, 10*1000*1000 };
	clock_gettime(1, &a);
	nanosleep(&ten_ms, 0);
	clock_gettime(1, &b);
	printf("slept %ld us\n", (b.tv_sec - a.tv_sec)*1000000 + (b.tv_nsec - a.tv_nsec)/1000);

	char const *home = getenv("HOME");
	printf("HOME=%s\n", home ? home : "(unset)");

	/* files: Genode's VFS (a RAM file system at /tmp, see the run script) */
	FILE *f = fopen("/tmp/note.txt", "w+");
	if (!f) { printf("fopen failed: %s\n", strerror(*__errno_location())); return 1; }
	fprintf(f, "line one\nline two %d\n", 2);
	fseek(f, 0, 2 /* SEEK_END */);
	long size = ftell(f);
	fseek(f, 0, 0 /* SEEK_SET */);
	char line[64];
	while (fgets(line, sizeof(line), f)) printf("read: %s", line);
	fclose(f);
	printf("file size %ld\n", size);

	FILE *missing = fopen("/nope/x", "r");
	printf("fopen of a missing file: %s (%s)\n", missing ? "opened?!" : "failed",
	       strerror(*__errno_location()));
	return 0;
}
