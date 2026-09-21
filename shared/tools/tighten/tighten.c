/* tighten: cut the dead time out of a screen recording.
 *
 * A twenty minute recording of a turn-based game is mostly a still image. You
 * play, you wait for the other person, you play again. This finds the moving
 * parts and keeps only those.
 *
 * Nothing here decodes video. ffmpeg does that, and ffmpeg also does the
 * arithmetic: `tblend=all_mode=difference` gives |a-b| per pixel and
 * `signalstats` averages it, so what crosses into this program is one number
 * per sampled frame. The whole tool is therefore a scheduler for a handful of
 * ffmpeg invocations and the segment algebra between them.
 *
 *   tighten sheet IN.mp4 --from 0 --to 120 --step 2 -o sheet.jpg
 *   tighten scan  IN.mp4 -o cut.edl
 *   tighten cut   IN.mp4 --edl cut.edl -o OUT.mp4
 *
 * `scan` writes an EDL, which is a plain text list of in and out points. That
 * is the seam between "dead time removed" and "a showcase": open the EDL, keep
 * the six segments worth showing, put them in the order you want, render. `cut`
 * with no --edl scans and renders in one go.
 */

#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
/* mkdtemp sits behind the full Darwin level, not the POSIX one. */
#define _DARWIN_C_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The status bar clock ticks over once a minute and the wifi and battery glyphs
 * flicker, which reads as motion in an otherwise dead frame. Ignore the top of
 * the picture. On a 2796pt iPhone 5% clears the clock. */
#define DEFAULT_SKIP_TOP 0.05

/* The diff pass runs on a tiny grayscale copy. 72px wide still shows a card
 * move, and is small enough that a twenty minute source is decode bound. */
#define PROBE_WIDTH 72
#define PROBE_FPS   5.0

#define MAX_ARGV 64

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("tighten: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

static void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n);
	if (!q)
		die("out of memory");
	return q;
}

/* ---- a growable string ---------------------------------------------------
 *
 * Used for both halves of the job: collecting a child's stdout, and building a
 * filtergraph whose length is the number of segments.
 */

typedef struct {
	char  *s;
	size_t len, cap;
} Buf;

static void buf_grow(Buf *b, size_t extra)
{
	if (b->len + extra + 1 <= b->cap)
		return;
	size_t want = b->cap ? b->cap : 4096;
	while (want < b->len + extra + 1)
		want *= 2;
	b->s = xrealloc(b->s, want);
	b->cap = want;
}

static void buf_add(Buf *b, const char *s, size_t n)
{
	buf_grow(b, n);
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = '\0';
}

static void buf_addf(Buf *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof tmp)
		die("internal: format too long");
	buf_add(b, tmp, (size_t)n);
}

static void buf_free(Buf *b)
{
	free(b->s);
	b->s = NULL;
	b->len = b->cap = 0;
}

/* ---- running ffmpeg ------------------------------------------------------ */

static void argv_print(char *const *argv)
{
	for (int i = 0; argv[i]; i++)
		printf("%s%s", i ? " " : "", argv[i]);
	putchar('\n');
}

/* Run to completion with our stdio, for the passes whose progress should be
 * seen. Returns the exit status. */
static int run(char *const *argv)
{
	pid_t pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (pid == 0) {
		execvp(argv[0], argv);
		fprintf(stderr, "tighten: %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	int st = 0;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			die("waitpid: %s", strerror(errno));
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* Run and collect stdout. stderr is left alone, so `-v error` output still
 * reaches the terminal when something is wrong. */
static int run_capture(char *const *argv, Buf *out)
{
	int fd[2];
	if (pipe(fd) < 0)
		die("pipe: %s", strerror(errno));
	pid_t pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (pid == 0) {
		close(fd[0]);
		dup2(fd[1], STDOUT_FILENO);
		close(fd[1]);
		execvp(argv[0], argv);
		fprintf(stderr, "tighten: %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	close(fd[1]);
	char chunk[65536];
	ssize_t n;
	while ((n = read(fd[0], chunk, sizeof chunk)) > 0)
		buf_add(out, chunk, (size_t)n);
	close(fd[0]);
	int st = 0;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			die("waitpid: %s", strerror(errno));
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* ---- what ffprobe knows -------------------------------------------------- */

typedef struct {
	int    width, height;
	double fps;
	double duration;
	bool   has_audio;
} Meta;

static Meta probe(const char *src)
{
	Meta m = {0};
	Buf out = {0};
	char *argv[] = {
		"ffprobe", "-v", "error", "-select_streams", "v:0",
		"-show_entries", "stream=width,height,r_frame_rate:format=duration",
		"-of", "default=noprint_wrappers=1:nokey=0", (char *)src, NULL
	};
	if (run_capture(argv, &out) != 0)
		die("ffprobe failed on %s", src);

	for (char *line = strtok(out.s, "\n"); line; line = strtok(NULL, "\n")) {
		int num, den;
		if (sscanf(line, "width=%d", &m.width) == 1) continue;
		if (sscanf(line, "height=%d", &m.height) == 1) continue;
		if (sscanf(line, "duration=%lf", &m.duration) == 1) continue;
		if (sscanf(line, "r_frame_rate=%d/%d", &num, &den) == 2 && den)
			m.fps = (double)num / den;
	}
	buf_free(&out);
	if (m.width <= 0 || m.height <= 0)
		die("no video stream in %s", src);
	if (m.duration <= 0)
		die("%s has no duration; is it a complete file?", src);
	if (m.fps <= 0)
		m.fps = 30.0;

	Buf a = {0};
	char *aargv[] = {
		"ffprobe", "-v", "error", "-select_streams", "a:0",
		"-show_entries", "stream=index", "-of", "csv=p=0", (char *)src, NULL
	};
	run_capture(aargv, &a);
	m.has_audio = a.len > 0 && a.s[0] != '\n';
	buf_free(&a);
	return m;
}

/* Digital silence, every frame. Worth knowing: a phone screen recording of a
 * silent app carries a real AAC track of nothing at all, and shipping it is
 * just bytes and a speaker icon. */
static bool audio_is_silent(const char *src)
{
	Buf out = {0};
	char *argv[] = {
		"ffmpeg", "-v", "error", "-i", (char *)src, "-map", "a:0?",
		"-af", "astats=metadata=1:reset=0,"
		       "ametadata=print:key=lavfi.astats.Overall.Peak_level:file=-",
		"-f", "null", "-", NULL
	};
	if (run_capture(argv, &out) != 0) {
		buf_free(&out);
		return false;
	}
	bool any = false, all_silent = true;
	for (char *p = out.s; p && (p = strstr(p, "Peak_level=")); ) {
		p += strlen("Peak_level=");
		any = true;
		if (strncmp(p, "-inf", 4) != 0)
			all_silent = false;
	}
	buf_free(&out);
	return any && all_silent;
}

/* ---- the motion scan ----------------------------------------------------- */

typedef struct {
	double *t, *d;
	size_t  n, cap;
} Samples;

static void samples_add(Samples *s, double t, double d)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 4096;
		s->t = xrealloc(s->t, s->cap * sizeof *s->t);
		s->d = xrealloc(s->d, s->cap * sizeof *s->d);
	}
	s->t[s->n] = t;
	s->d[s->n] = d;
	s->n++;
}

typedef struct {
	double thr, gap, pad, minlen, skip_top, probe_fps;
} ScanOpts;

static Samples measure(const char *src, const Meta *m, const ScanOpts *o)
{
	/* Keep the probe frame's aspect after the crop, and even, which some
	 * scalers insist on. */
	double aspect = (double)m->height / m->width;
	int h = (int)(PROBE_WIDTH * aspect * (1.0 - o->skip_top) + 0.5) / 2 * 2;
	if (h < 2)
		h = 2;

	char vf[512];
	snprintf(vf, sizeof vf,
		 "fps=%g,crop=iw:ih*%.4f:0:ih*%.4f,scale=%d:%d,format=gray,"
		 "tblend=all_mode=difference,signalstats,"
		 "metadata=print:key=lavfi.signalstats.YAVG:file=-",
		 o->probe_fps, 1.0 - o->skip_top, o->skip_top, PROBE_WIDTH, h);

	char *argv[] = { "ffmpeg", "-v", "error", "-i", (char *)src, "-an",
			 "-vf", vf, "-f", "null", "-", NULL };
	Buf out = {0};
	if (run_capture(argv, &out) != 0)
		die("ffmpeg failed during the scan");

	/* The metadata filter prints two lines per frame:
	 *   frame:12   pts:13   pts_time:2.6
	 *   lavfi.signalstats.YAVG=0.0312
	 * so carry the time forward to the value that follows it. */
	Samples s = {0};
	double t = -1;
	for (char *line = strtok(out.s, "\n"); line; line = strtok(NULL, "\n")) {
		const char *p = strstr(line, "pts_time:");
		if (p) {
			t = atof(p + strlen("pts_time:"));
			continue;
		}
		p = strstr(line, "YAVG=");
		if (p && t >= 0) {
			samples_add(&s, t, atof(p + strlen("YAVG=")));
			t = -1;
		}
	}
	buf_free(&out);
	if (s.n == 0)
		die("the scan produced no samples; is this ffmpeg built with signalstats?");
	return s;
}

/* ---- segments ------------------------------------------------------------ */

typedef struct { double a, b; } Seg;

typedef struct {
	Seg   *v;
	size_t n, cap;
} Segs;

static void segs_add(Segs *s, double a, double b)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 64;
		s->v = xrealloc(s->v, s->cap * sizeof *s->v);
	}
	s->v[s->n].a = a;
	s->v[s->n].b = b;
	s->n++;
}

static double segs_total(const Segs *s)
{
	double t = 0;
	for (size_t i = 0; i < s->n; i++)
		t += s->v[i].b - s->v[i].a;
	return t;
}

/* Group the frames above the threshold into segments, bridge the short stills
 * between them, pad the edges, drop the crumbs.
 *
 * `gap` is what keeps one action in one piece. A card lands, nothing moves for
 * a beat, the hand re-sorts: bridging holds that together instead of cutting
 * through the middle of it. */
static Segs segment(const Samples *s, const ScanOpts *o, double duration)
{
	Segs raw = {0};
	bool open = false;
	double start = 0, prev = 0;

	for (size_t i = 0; i < s->n; i++) {
		if (s->d[i] <= o->thr)
			continue;
		if (!open) {
			start = prev = s->t[i];
			open = true;
		} else if (s->t[i] - prev > o->gap) {
			segs_add(&raw, start, prev);
			start = s->t[i];
		}
		prev = s->t[i];
	}
	if (open)
		segs_add(&raw, start, prev);

	Segs out = {0};
	for (size_t i = 0; i < raw.n; i++) {
		double a = raw.v[i].a - o->pad, b = raw.v[i].b + o->pad;
		if (a < 0)
			a = 0;
		if (b > duration)
			b = duration;
		if (out.n && a <= out.v[out.n - 1].b) {
			if (b > out.v[out.n - 1].b)
				out.v[out.n - 1].b = b;   /* padding made them touch */
		} else {
			segs_add(&out, a, b);
		}
	}
	free(raw.v);

	Segs kept = {0};
	for (size_t i = 0; i < out.n; i++)
		if (out.v[i].b - out.v[i].a >= o->minlen)
			segs_add(&kept, out.v[i].a, out.v[i].b);
	free(out.v);
	return kept;
}

/* Hold the last shot a moment longer. A cut that ends on the frame where the
 * last pixel stopped moving reads as truncated - the eye has not finished the
 * action yet - and a second of held frame is how you end a shot.
 *
 * The last segment in render order, which for an edited EDL is the last line of
 * the file and not the last one in the source. */
static void segs_tail(Segs *s, double seconds, double duration)
{
	if (s->n == 0 || seconds <= 0)
		return;
	Seg *last = &s->v[s->n - 1];
	last->b += seconds;
	if (last->b > duration)
		last->b = duration;
}

static void fmt_hms(double t, char *buf, size_t n)
{
	snprintf(buf, n, "%d:%05.2f", (int)(t / 60), t - 60 * (int)(t / 60));
}

static void segs_print(const Segs *s, double duration)
{
	char a[32], b[32];
	for (size_t i = 0; i < s->n; i++) {
		fmt_hms(s->v[i].a, a, sizeof a);
		fmt_hms(s->v[i].b, b, sizeof b);
		printf("  %3zu  %8s -> %-8s  %6.2fs\n", i, a, b, s->v[i].b - s->v[i].a);
	}
	double kept = segs_total(s);
	fmt_hms(kept, a, sizeof a);
	fmt_hms(duration, b, sizeof b);
	printf("\n  %zu segments, %s kept of %s (%.0f%%)\n",
	       s->n, a, b, duration > 0 ? kept / duration * 100 : 0);
}

/* ---- the EDL -------------------------------------------------------------
 *
 * Plain text, two numbers a line, because the point of it is that a person
 * opens it and rearranges the cut by hand.
 */

static void edl_write(const char *path, const char *src, const Segs *s,
		      const ScanOpts *o, double duration)
{
	FILE *f = fopen(path, "w");
	if (!f)
		die("%s: %s", path, strerror(errno));
	fprintf(f, "# tighten edl - in and out points in seconds, one segment a line.\n");
	fprintf(f, "# Edit freely: delete what you do not want, reorder, trim. The cut is\n");
	fprintf(f, "# rendered in the order written here, so moving a line moves the shot.\n");
	fprintf(f, "#\n");
	fprintf(f, "#   tighten cut %s --edl %s -o OUT.mp4\n", src, path);
	fprintf(f, "#\n");
	fprintf(f, "# source: %s\n", src);
	fprintf(f, "# scan:   thr=%.2f gap=%.2f pad=%.2f min=%.2f skip-top=%.3f\n",
		o->thr, o->gap, o->pad, o->minlen, o->skip_top);
	fprintf(f, "# kept:   %zu segments, %.1fs of %.1fs\n\n",
		s->n, segs_total(s), duration);
	for (size_t i = 0; i < s->n; i++)
		fprintf(f, "%8.2f %8.2f   # %6.2fs\n",
			s->v[i].a, s->v[i].b, s->v[i].b - s->v[i].a);
	fclose(f);
	printf("\n  wrote %s\n", path);
}

static Segs edl_read(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		die("%s: %s", path, strerror(errno));
	Segs s = {0};
	char line[512];
	int lineno = 0;
	while (fgets(line, sizeof line, f)) {
		lineno++;
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;
		double a, b;
		if (sscanf(p, "%lf %lf", &a, &b) != 2)
			die("%s:%d: expected two numbers: %s", path, lineno, p);
		if (b <= a)
			die("%s:%d: end %.2f is not after start %.2f", path, lineno, b, a);
		segs_add(&s, a, b);
	}
	fclose(f);
	if (s.n == 0)
		die("%s: no segments", path);
	return s;
}

/* ---- rendering ----------------------------------------------------------- */

typedef struct {
	int         height;     /* 0: keep the source size */
	double      fps;
	const char *codec;      /* "h264" or "hevc" */
	int         crf, quality;
	const char *preset;
	double      crop_top;   /* fraction cut off the top of the picture */
	double      tail;       /* seconds held after the last segment */
	bool        audio, dry_run;
} RenderOpts;

/* One ffmpeg pass per segment, then a stream copy join.
 *
 * The tempting filtergraph is a single `select` with an OR of between(), one
 * decode for the whole cut. It renders in the wrong order. `select` is a
 * predicate over one pass through the file, so it can only ever emit frames in
 * source order, and the EDL's whole point is that moving a line moves the shot.
 * So each segment is seeked to and encoded on its own, and the pieces are
 * joined with the concat demuxer. The join is a stream copy: every piece came
 * out of the same encoder at the same size, so there is nothing to re-encode.
 *
 * On a long source this is also the faster way round. A select pass decodes all
 * twenty minutes; seeking decodes only the five that survive.
 *
 * (The concat demuxer can nominally do the whole job itself, with inpoint and
 * outpoint per entry. Its seek is not frame accurate - it hands back everything
 * from the preceding keyframe - which on a screen recording is seconds of dead
 * time in front of every shot. That is exactly what this tool exists to remove.)
 *
 * setpts=N/FPS/TB rebuilds a constant frame rate. That is not cosmetic: a phone
 * records at a variable rate, and a plain copy of the original timestamps would
 * leave the gaps in the timeline as frozen frames. */
/* Pieces, graph and concat list all live in one temp directory, and nothing
 * outside this file ever names them, so leaving on any path can sweep up by
 * counting. */
static void cut_cleanup(const char *dir, size_t pieces)
{
	char p[512];
	for (size_t i = 0; i < pieces; i++) {
		snprintf(p, sizeof p, "%s/%04zu.mp4", dir, i);
		unlink(p);
	}
	snprintf(p, sizeof p, "%s/graph.txt", dir);
	unlink(p);
	snprintf(p, sizeof p, "%s/concat.txt", dir);
	unlink(p);
	rmdir(dir);
}

static void render(const char *src, const Segs *s, const char *out,
		   const RenderOpts *r)
{
	/* The graph is the same for every segment - nothing in it depends on
	 * where the segment sits in the source - so it is written once and
	 * every pass reads the same file. */
	Buf graph = {0};
	buf_addf(&graph, "[0:v]setpts=N/%g/TB", r->fps);
	if (r->crop_top > 0)
		/* Before any scale, so the scale sees the picture actually
		 * being shipped and the aspect ratio comes out right. The
		 * height is rounded down to even, which 4:2:0 has no way to
		 * express otherwise, and the window is anchored at the bottom
		 * so that the rounding comes off the edge being thrown away
		 * rather than off the picture. */
		buf_addf(&graph, ",crop=iw:trunc(ih*%.6f/2)*2:0:ih-oh",
			 1.0 - r->crop_top);
	if (r->height)
		buf_addf(&graph, ",scale=-2:%d:flags=lanczos", r->height);
	buf_add(&graph, "[v]", 3);
	if (r->audio)
		buf_add(&graph, ";[0:a]asetpts=N/SR/TB[a]", 24);

	char dir[] = "/tmp/tighten-cut-XXXXXX";
	if (!mkdtemp(dir))
		die("mkdtemp: %s", strerror(errno));

	char gpath[512], lpath[512];
	snprintf(gpath, sizeof gpath, "%s/graph.txt", dir);
	snprintf(lpath, sizeof lpath, "%s/concat.txt", dir);
	FILE *g = fopen(gpath, "w");
	if (!g)
		die("%s: %s", gpath, strerror(errno));
	fwrite(graph.s, 1, graph.len, g);
	fclose(g);

	FILE *list = fopen(lpath, "w");
	if (!list)
		die("%s: %s", lpath, strerror(errno));

	char fps_s[32], crf_s[32], q_s[32];
	snprintf(fps_s, sizeof fps_s, "%g", r->fps);
	snprintf(crf_s, sizeof crf_s, "%d", r->crf);
	snprintf(q_s, sizeof q_s, "%d", r->quality);

	for (size_t i = 0; i < s->n; i++) {
		char ss[32], t[32], piece[512];
		snprintf(ss, sizeof ss, "%.3f", s->v[i].a);
		snprintf(t, sizeof t, "%.3f", s->v[i].b - s->v[i].a);
		snprintf(piece, sizeof piece, "%s/%04zu.mp4", dir, i);

		char *argv[MAX_ARGV];
		int n = 0;
		argv[n++] = "ffmpeg";
		argv[n++] = "-v";     argv[n++] = "error";
		if (!r->audio)
			argv[n++] = "-an";
		/* -ss ahead of -i is the accurate seek: ffmpeg starts at the
		 * keyframe before it and throws away what it decodes up to the
		 * cut, so the piece begins on the frame asked for. */
		argv[n++] = "-ss";    argv[n++] = ss;
		argv[n++] = "-t";     argv[n++] = t;
		argv[n++] = "-i";     argv[n++] = (char *)src;
		argv[n++] = "-filter_complex_script"; argv[n++] = gpath;
		argv[n++] = "-map";   argv[n++] = "[v]";
		if (r->audio) {
			argv[n++] = "-map"; argv[n++] = "[a]";
			argv[n++] = "-c:a"; argv[n++] = "aac";
			argv[n++] = "-b:a"; argv[n++] = "128k";
		}
		if (strcmp(r->codec, "hevc") == 0) {
			argv[n++] = "-c:v";   argv[n++] = "hevc_videotoolbox";
			argv[n++] = "-q:v";   argv[n++] = q_s;
			argv[n++] = "-tag:v"; argv[n++] = "hvc1";
		} else {
			argv[n++] = "-c:v";     argv[n++] = "libx264";
			argv[n++] = "-preset";  argv[n++] = (char *)r->preset;
			argv[n++] = "-crf";     argv[n++] = crf_s;
			argv[n++] = "-pix_fmt"; argv[n++] = "yuv420p";
		}
		argv[n++] = "-r";        argv[n++] = fps_s;
		argv[n++] = piece;
		argv[n++] = "-y";
		argv[n] = NULL;
		if (n >= MAX_ARGV)
			die("internal: argv overflow");

		if (r->dry_run) {
			argv_print(argv);
		} else {
			printf("\r  rendering %zu/%zu ...", i + 1, s->n);
			fflush(stdout);
			if (run(argv) != 0) {
				fclose(list);
				cut_cleanup(dir, i + 1);
				die("ffmpeg failed on segment %zu (%.2f -> %.2f)",
				    i, s->v[i].a, s->v[i].b);
			}
		}
		/* The concat demuxer reads this list in order, and that order is
		 * the EDL's order. Absolute paths, so -safe 0 is the honest way
		 * to say the list was written by this program. */
		fprintf(list, "file '%s'\n", piece);
	}
	fclose(list);

	char *join[] = { "ffmpeg", "-v", "error", "-f", "concat", "-safe", "0",
			 "-i", lpath, "-c", "copy",
			 "-movflags", "+faststart", (char *)out, "-y", NULL };

	if (r->dry_run) {
		argv_print(join);
		printf("\n# filtergraph (%s):\n%s\n", gpath, graph.s);
		printf("# concat list (%s):\n", lpath);
		for (size_t i = 0; i < s->n; i++)
			printf("file '%s/%04zu.mp4'\n", dir, i);
		printf("\n# %s is left in place, so the commands above run as printed.\n",
		       dir);
		buf_free(&graph);
		return;
	}
	printf("\r  joining %zu segments ...   ", s->n);
	fflush(stdout);
	int rc = run(join);

	cut_cleanup(dir, s->n);
	buf_free(&graph);
	if (rc != 0)
		die("ffmpeg failed while joining the segments");

	Meta m = probe(out);
	struct stat st;
	char hms[32];
	fmt_hms(m.duration, hms, sizeof hms);
	stat(out, &st);
	printf("\r  %s: %s  %dx%d  %.1f MB\n",
	       out, hms, m.width, m.height, st.st_size / 1e6);
}

/* ---- the contact sheet ---------------------------------------------------
 *
 * Reading one of these is how you find out what is in a recording, and it is
 * the step that picks the shots. The step is fixed so that tile i, counted
 * across the rows from zero, is at `from + i * step` seconds. Every time you
 * point at a tile you are naming a timestamp.
 */
static void sheet(const char *src, double from, double to, double step,
		  int cols, int tile_w, const char *out)
{
	Meta m = probe(src);
	if (to <= 0)
		to = m.duration;
	if (to <= from)
		die("--to %.2f is not after --from %.2f", to, from);

	int h = (int)((double)tile_w * m.height / m.width + 0.5) / 2 * 2;
	if (h < 2)
		h = 2;

	char dir[] = "/tmp/tighten-sheet-XXXXXX";
	if (!mkdtemp(dir))
		die("mkdtemp: %s", strerror(errno));

	char from_s[32], span_s[32], vf[128], pat[512];
	snprintf(from_s, sizeof from_s, "%.3f", from);
	snprintf(span_s, sizeof span_s, "%.3f", to - from);
	snprintf(vf, sizeof vf, "fps=1/%g,scale=%d:%d", step, tile_w, h);
	snprintf(pat, sizeof pat, "%s/%%04d.jpg", dir);

	char *extract[] = { "ffmpeg", "-v", "error", "-ss", from_s, "-t", span_s,
			    "-i", (char *)src, "-an", "-vf", vf, "-q:v", "3",
			    pat, "-y", NULL };
	if (run(extract) != 0)
		die("ffmpeg failed while extracting the sheet frames");

	int count = 0;
	DIR *d = opendir(dir);
	if (!d)
		die("%s: %s", dir, strerror(errno));
	for (struct dirent *e; (e = readdir(d)); )
		if (e->d_name[0] != '.')
			count++;
	closedir(d);
	if (count == 0)
		die("no frames in that window");

	int rows = (count + cols - 1) / cols;
	char tile[128];
	snprintf(tile, sizeof tile, "tile=%dx%d:margin=4:padding=4:color=white", cols, rows);
	char *mont[] = { "ffmpeg", "-v", "error", "-i", pat, "-frames:v", "1",
			 "-vf", tile, "-q:v", "3", (char *)out, "-y", NULL };
	int rc = run(mont);

	for (int i = 1; i <= count; i++) {
		char p[512];
		snprintf(p, sizeof p, "%s/%04d.jpg", dir, i);
		unlink(p);
	}
	rmdir(dir);
	if (rc != 0)
		die("ffmpeg failed while tiling the sheet");

	printf("  %s: %d frames, %dx%d, %.1fs to %.1fs every %gs\n",
	       out, count, cols, rows, from, to, step);
	printf("  tile i (0-based, reading across) is at t = %.1f + i * %g\n", from, step);
}

/* ---- command line -------------------------------------------------------- */

static const char USAGE[] =
"tighten - cut the dead time out of a screen recording\n"
"\n"
"  tighten sheet IN [--from S] [--to S] [--step S] [--cols N] [--tile-width PX] -o OUT.jpg\n"
"      A contact sheet of a window, so you can see what is in the recording.\n"
"\n"
"  tighten scan  IN [scan options] [-o OUT.edl]\n"
"      Find the segments where something moves; print them, and write an EDL.\n"
"\n"
"  tighten cut   IN -o OUT.mp4 [--edl F] [scan options] [render options]\n"
"      Render those segments, or the ones an edited EDL names, end to end.\n"
"\n"
"scan options\n"
"  --thr F        motion threshold, mean absolute pixel difference 0-255 (0.30).\n"
"                 Lower catches subtler motion, and more noise\n"
"  --gap F        bridge stretches of stillness shorter than this, seconds (3.00)\n"
"  --pad F        stillness kept on each side of a segment, seconds (0.60)\n"
"  --min F        drop segments shorter than this, seconds (1.00)\n"
"  --skip-top F   ignore this fraction of the height at the top, where a phone\n"
"                 status bar clock ticks (0.05)\n"
"  --probe-fps F  sampling rate of the diff pass (5)\n"
"\n"
"render options\n"
"  --height N     scale the output to this height (default: the source size)\n"
"  --fps F        output frame rate (default: the source rate, capped at 60)\n"
"  --codec C      h264 (default, plays everywhere) or hevc (VideoToolbox, faster)\n"
"  --crf N        libx264 quality (22)\n"
"  --preset P     libx264 preset (medium)\n"
"  --quality N    VideoToolbox q:v (60)\n"
"  --crop-top F   crop this fraction off the top of the picture, losing the\n"
"                 phone status bar. Independent of --skip-top: one hides the\n"
"                 clock from the scan, this one takes it out of the shot (0)\n"
"  --tail F       hold the last shot this many seconds past its last moving\n"
"                 frame, clamped to the end of the source (0)\n"
"  --audio        keep the audio track. Dropped by default, and dropped anyway\n"
"                 when every frame of it is digital silence\n"
"  --dry-run      print the ffmpeg commands, the filtergraph and the concat\n"
"                 list, and render nothing\n";

static double need_num(int argc, char **argv, int *i, const char *flag)
{
	if (*i + 1 >= argc)
		die("%s needs a value", flag);
	return atof(argv[++*i]);
}

static const char *need_str(int argc, char **argv, int *i, const char *flag)
{
	if (*i + 1 >= argc)
		die("%s needs a value", flag);
	return argv[++*i];
}

int main(int argc, char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		fputs(USAGE, stdout);
		return argc < 2 ? 1 : 0;
	}
	const char *cmd = argv[1];
	bool is_sheet = !strcmp(cmd, "sheet");
	bool is_scan  = !strcmp(cmd, "scan");
	bool is_cut   = !strcmp(cmd, "cut");
	if (!is_sheet && !is_scan && !is_cut)
		die("unknown command %s (try sheet, scan or cut)", cmd);

	const char *src = NULL, *out = NULL, *edl = NULL;
	ScanOpts   so = { 0.30, 3.00, 0.60, 1.00, DEFAULT_SKIP_TOP, PROBE_FPS };
	RenderOpts ro = { 0, 0, "h264", 22, 60, "medium", 0.0, 0.0, false, false };
	double from = 0, to = 0, step = 10;
	int    cols = 8, tile_w = 160;

	for (int i = 2; i < argc; i++) {
		char *a = argv[i];
		if (a[0] != '-') {
			if (src)
				die("more than one input: %s and %s", src, a);
			src = a;
		}
		else if (!strcmp(a, "-o") || !strcmp(a, "--out")) out = need_str(argc, argv, &i, a);
		else if (!strcmp(a, "--edl"))        edl = need_str(argc, argv, &i, a);
		else if (!strcmp(a, "--thr"))        so.thr = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--gap"))        so.gap = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--pad"))        so.pad = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--min"))        so.minlen = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--skip-top"))   so.skip_top = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--probe-fps"))  so.probe_fps = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--height"))     ro.height = (int)need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--fps"))        ro.fps = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--codec"))      ro.codec = need_str(argc, argv, &i, a);
		else if (!strcmp(a, "--crf"))        ro.crf = (int)need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--preset"))     ro.preset = need_str(argc, argv, &i, a);
		else if (!strcmp(a, "--quality"))    ro.quality = (int)need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--crop-top"))   ro.crop_top = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--tail"))       ro.tail = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--audio"))      ro.audio = true;
		else if (!strcmp(a, "--dry-run"))    ro.dry_run = true;
		else if (!strcmp(a, "--from"))       from = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--to"))         to = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--step"))       step = need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--cols"))       cols = (int)need_num(argc, argv, &i, a);
		else if (!strcmp(a, "--tile-width")) tile_w = (int)need_num(argc, argv, &i, a);
		else die("unknown option %s", a);
	}

	if (!src)
		die("no input file");
	if (access(src, R_OK) != 0)
		die("%s: %s", src, strerror(errno));
	if (strcmp(ro.codec, "h264") && strcmp(ro.codec, "hevc"))
		die("--codec must be h264 or hevc, not %s", ro.codec);
	if (so.skip_top < 0 || so.skip_top >= 1)
		die("--skip-top must be in [0, 1)");
	if (ro.crop_top < 0 || ro.crop_top >= 1)
		die("--crop-top must be in [0, 1)");
	if (ro.tail < 0)
		die("--tail cannot be negative");
	if (step <= 0 || cols < 1 || tile_w < 2)
		die("--step, --cols and --tile-width must be positive");

	if (is_sheet) {
		if (!out)
			die("sheet needs -o OUT.jpg");
		sheet(src, from, to, step, cols, tile_w, out);
		return 0;
	}

	Meta m = probe(src);
	Segs segs;

	if (is_cut && edl) {
		segs = edl_read(edl);
		printf("  %s: %zu segments, %.1fs\n", edl, segs.n, segs_total(&segs));
	} else {
		char hms[32];
		fmt_hms(m.duration, hms, sizeof hms);
		printf("  scanning %s (%s, %dx%d) ...\n", src, hms, m.width, m.height);
		Samples s = measure(src, &m, &so);
		segs = segment(&s, &so, m.duration);
		free(s.t);
		free(s.d);
		if (segs.n == 0)
			die("nothing moved above the threshold; try a lower --thr");
		segs_print(&segs, m.duration);
	}

	if (is_scan) {
		if (out)
			edl_write(out, src, &segs, &so, m.duration);
		free(segs.v);
		return 0;
	}

	if (!out)
		die("cut needs -o OUT.mp4");
	if (ro.audio && !m.has_audio) {
		printf("  no audio track to keep\n");
		ro.audio = false;
	}
	if (ro.audio && audio_is_silent(src)) {
		printf("  the audio track is digital silence, dropping it\n");
		ro.audio = false;
	}
	if (ro.fps <= 0)
		ro.fps = m.fps < 60.0 ? m.fps : 60.0;
	if (ro.tail > 0) {
		double was = segs.v[segs.n - 1].b;
		segs_tail(&segs, ro.tail, m.duration);
		double got = segs.v[segs.n - 1].b - was;
		if (got < ro.tail)
			printf("  tail: only %.2fs of the requested %.2fs left "
			       "before the end of the source\n", got, ro.tail);
	}

	render(src, &segs, out, &ro);
	free(segs.v);
	return 0;
}
