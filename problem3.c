/*
 * CS3103 Problem 3: CPU scheduling.
 *
 * Reads a job file and runs five configurations on the same input:
 * FCFS, non-preemptive SJF, preemptive Priority, RR (quantum 3), RR (quantum 6).
 *
 * Usage: ./problem3 <orders_file>
 *
 * Input file: one job per line, blank lines and '#' comments allowed.
 *     job_id  arrival  burst  priority
 *     J001    0        8      3
 */
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ID_LEN 63          /* Maximum number of characters stored in a job ID. */
#define RR3 3              /* Required Round Robin time quantum. */
#define RR6 6              /* Second required Round Robin time quantum. */

/* ------------------------------------------------------------------------- */
/* Data structures                                                           */
/* ------------------------------------------------------------------------- */

/* One input job and its temporary scheduling state.
 * remaining_time is reset to burst_time before each algorithm runs, so one
 * loaded queue can be reused by all five configurations.
 * input_index keeps the original line order, which is the final tie-breaker. */
typedef struct {
    char job_id[ID_LEN + 1];
    int arrival_time, burst_time, priority, input_index, remaining_time;
} Job;

/* Dynamic array of jobs, kept in input-file order. */
typedef struct { Job *jobs; int count, capacity; } JobQueue;

/* One interval of the printed Gantt chart.
 * idle == 1 means the CPU was free; job_id is then empty. */
typedef struct {
    int start_time, end_time, idle;
    char job_id[ID_LEN + 1];
} GanttSegment;

/* Growable list of Gantt segments produced by one scheduling run. */
typedef struct { GanttSegment *segments; int count, capacity; } GanttChart;

/* Calculated results for one job after a schedule has completed. */
typedef struct {
    char job_id[ID_LEN + 1];
    int arrival_time, burst_time, completion_time, turnaround_time, waiting_time;
} JobMetrics;

/* All per-job results plus the three averages for one algorithm. */
typedef struct {
    JobMetrics *jobs;
    int job_count;
    double avg_tat, avg_wt, cpu_utilization;
} Metrics;

/* Fixed-size circular FIFO holding indices into a temporary Job array.
 * head is the position of the oldest element; size is the number stored. */
typedef struct { int *data; int size, head, capacity; } IntQueue;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

/* Reallocate memory and terminate the program if allocation fails.
 * Input: old pointer (may be NULL) and the new byte size.
 * Returns: the new pointer, never NULL. */
static void *xalloc(void *p, size_t n)
{
    p = realloc(p, n);                       /* grow or shrink the block */
    if (!p) { perror("memory"); exit(EXIT_FAILURE); }  /* Check for allocation failure */
    return p;
}

/* Release the job array and reset the queue to an empty state. */
static void free_jobs(JobQueue *q)
{
    free(q->jobs);          /* release the heap array */
    *q = (JobQueue){0};     /* all fields zero: jobs NULL, count 0, capacity 0 */
}

/* Prepare a chart for a new algorithm by clearing all counters. */
static void chart_init(GanttChart *c) { *c = (GanttChart){0}; }

/* Release chart storage and reset the chart. */
static void chart_free(GanttChart *c)
{
    free(c->segments);      /* release the segment array */
    *c = (GanttChart){0};   /* segments NULL, count 0, capacity 0 */
}

/* Append the interval [start, end) to the chart.
 * job == NULL records an IDLE interval.
 * Adjacent intervals of the same job (or adjacent IDLE intervals) are merged,
 * so one continuous run appears as a single Gantt block. */
static void add_segment(GanttChart *c, int start, int end, const Job *job)
{
    GanttSegment *last;  /* pointer to the previous or the new segment */

    /* Ignore zero-length or backwards intervals (can happen at a boundary). */
    if (end <= start) return;

    /* Try to merge with the previous segment instead of adding a new one. */
    if (c->count) {
        last = &c->segments[c->count - 1];
        if (last->end_time == start &&
            ((job == NULL && last->idle) ||
             (job && !last->idle && strcmp(last->job_id, job->job_id) == 0))) {
            last->end_time = end;
            return;
        }
    }

    /* Grow the segment array when it is full (doubling keeps it amortised O(1)). */
    if (c->count == c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : 16;
        c->segments = xalloc(c->segments,
                             (size_t)c->capacity * sizeof(*c->segments));
    }

    /* Store the new segment and advance the count. */
    last = &c->segments[c->count++];
    last->start_time = start;
    last->end_time = end;
    last->idle = job == NULL;
    strcpy(last->job_id, job ? job->job_id : "");
}

/* Copy the queue so an algorithm can modify remaining_time without
 * affecting the other runs. Every copy starts with a full burst time.
 * Returns: a newly allocated Job array, owned by the caller. */
static Job *working_copy(const JobQueue *q)
{
    Job *jobs = malloc((size_t)q->count * sizeof(*jobs));  /* fresh working array */
    int i;                                                 /* loop index */

    if (!jobs) { perror("memory"); exit(EXIT_FAILURE); }

    /* Shallow copy is enough: all fields except remaining_time are read-only. */
    memcpy(jobs, q->jobs, (size_t)q->count * sizeof(*jobs));

    /* Reset the per-run state. */
    for (i = 0; i < q->count; ++i) jobs[i].remaining_time = jobs[i].burst_time;
    return jobs;
}

/* Return the earliest arrival time strictly after "time" among unfinished
 * jobs, or INT_MAX when no further job will arrive.
 * Used to draw an IDLE interval and to detect that another arrival may
 * preempt the running job. */
static int next_arrival(const Job *jobs, int n, int time)
{
    int best = INT_MAX;  /* earliest qualifying arrival found so far */
    int i;               /* loop index over all jobs */

    /* Scan every job to find the earliest arrival time after "time". */
    for (i = 0; i < n; ++i) {
        /* Skip jobs that are already finished or have not arrived yet. */
        if (jobs[i].remaining_time <= 0 || jobs[i].arrival_time <= time) continue;

        /* Keep the earliest arrival time found so far. */
        if (jobs[i].arrival_time < best) best = jobs[i].arrival_time;
    }
    return best;
}

/* Select the best ready, unfinished job for FCFS, SJF, or priority.
 * mode 0 = FCFS    : smallest arrival time, then input order.
 * mode 1 = SJF     : smallest burst, then arrival, then input order.
 * mode 2 = priority: smallest priority number (1 = most urgent), then arrival,
 *                    then input order.
 * "time" is the current simulation time; jobs arriving later are not ready.
 * Returns: the job index, or -1 when no job is ready. */
static int pick(const Job *jobs, int n, int time, int mode)
{
    int best = -1;  /* index of the best ready job found so far */
    int i;          /* loop index over all jobs */

    /* Scan every job to find the best ready one according to the mode. */
    for (i = 0; i < n; ++i) {
        int better = 0;  /* becomes 1 when jobs[i] beats the current best */

        /* Skip jobs that are already finished or have not arrived yet. */
        if (jobs[i].remaining_time <= 0 || jobs[i].arrival_time > time) continue;

        if (best < 0) {
            /* First ready job found automatically becomes the best. */
            better = 1;
        } else if (mode == 0) {
            /* FCFS: earlier arrival wins; equal arrival falls back to file order. */
            better = jobs[i].arrival_time < jobs[best].arrival_time ||
                     (jobs[i].arrival_time == jobs[best].arrival_time &&
                      jobs[i].input_index < jobs[best].input_index);
        } else if (mode == 1) {
            /* SJF: shorter burst wins, then earlier arrival, then file order. */
            better = jobs[i].burst_time < jobs[best].burst_time ||
                     (jobs[i].burst_time == jobs[best].burst_time &&
                      (jobs[i].arrival_time < jobs[best].arrival_time ||
                       (jobs[i].arrival_time == jobs[best].arrival_time &&
                        jobs[i].input_index < jobs[best].input_index)));
        } else {
            /* Priority: lower number wins, then earlier arrival, then file order. */
            better = jobs[i].priority < jobs[best].priority ||
                     (jobs[i].priority == jobs[best].priority &&
                      (jobs[i].arrival_time < jobs[best].arrival_time ||
                       (jobs[i].arrival_time == jobs[best].arrival_time &&
                        jobs[i].input_index < jobs[best].input_index)));
        }
        /* If this job is better than the current best, remember it. */
        if (better) best = i;
    }
    return best;
}

/* Shared implementation of the two non-preemptive algorithms.
 * Once a job starts it runs to completion, so each iteration simply picks a
 * ready job, records it in the chart, and advances the clock by its burst. */
static void nonpreemptive(JobQueue *q, GanttChart *c, int mode)
{
    Job *jobs = working_copy(q);  /* private copy with full remaining times */
    int done = 0;                 /* number of jobs finished so far */
    int time = 0;                 /* current simulation time */

    chart_init(c);                /* prepare the chart for this run */

    /* Continue until every job has completed. */
    while (done < q->count) {
        int i = pick(jobs, q->count, time, mode);  /* job to run next, or -1 */

        /* No job is ready: jump forward to the next arrival as an IDLE gap. */
        if (i < 0) {
            int next = next_arrival(jobs, q->count, time);
            if (next == INT_MAX) break;   /* nothing left to run (defensive) */
            add_segment(c, time, next, NULL);
            time = next;
            continue;
        }

        /* Run the chosen job from "time" to "time + burst" in one piece. */
        add_segment(c, time, time + jobs[i].remaining_time, &jobs[i]);
        time += jobs[i].remaining_time;
        jobs[i].remaining_time = 0;   /* non-preemptive: it is now complete */
        ++done;
    }

    free(jobs);
}

/* ------------------------------------------------------------------------- */
/* Required scheduling functions                                             */
/* ------------------------------------------------------------------------- */

/* Schedule jobs with non-preemptive First-Come-First-Served.
 * Ties in arrival time are broken by input-file order.
 * Inputs: q = loaded queue, c = chart to fill. Side effect: fills *c. */
void schedule_FCFS(JobQueue *q, GanttChart *c) { nonpreemptive(q, c, 0); }

/* Schedule ready jobs with non-preemptive Shortest Job First.
 * Ties are broken by arrival time, then input-file order.
 * Inputs: q = loaded queue, c = chart to fill. Side effect: fills *c. */
void schedule_SJF(JobQueue *q, GanttChart *c) { nonpreemptive(q, c, 1); }

/* Schedule jobs using preemptive priority scheduling.
 * A job runs only until the next arrival; at that instant the highest-priority
 * ready job is selected again, which preempts the running one if it is more
 * urgent. Equal priorities keep the running job because "pick" would return the
 * same or an earlier-arrived job.
 * Inputs: q = loaded queue, c = chart to fill. Side effect: fills *c. */
void schedule_priority_preemptive(JobQueue *q, GanttChart *c)
{
    Job *jobs = working_copy(q);  /* private copy with full remaining times */
    int done = 0;                 /* number of jobs finished so far */
    int time = 0;                 /* current simulation time */

    chart_init(c);

    /* Continue until every job has completed. */
    while (done < q->count) {
        int i = pick(jobs, q->count, time, 2);  /* most urgent ready job, or -1 */
        int end;    /* end of the next CPU slice */
        int next;   /* next arrival time */

        /* Nothing ready: skip forward to the next arrival. */
        if (i < 0) {
            next = next_arrival(jobs, q->count, time);
            if (next == INT_MAX) break;
            add_segment(c, time, next, NULL);
            time = next;
            continue;
        }

        /* Run at most until this job finishes... */
        end = time + jobs[i].remaining_time;

        /* ...but stop early at the next arrival so preemption can be tested. */
        next = next_arrival(jobs, q->count, time);
        if (next < end) end = next;

        /* Record the slice and charge it against the job's remaining time. */
        add_segment(c, time, end, &jobs[i]);
        jobs[i].remaining_time -= end - time;
        time = end;

        /* Count the job as finished only when its remaining time reaches zero. */
        if (!jobs[i].remaining_time) ++done;
    }

    free(jobs);
}

/* ------------------------------------------------------------------------- */
/* Ready queue for Round Robin                                               */
/* ------------------------------------------------------------------------- */

/* Create a circular FIFO that can hold "capacity" job indices. */
static IntQueue *queue_new(int capacity)
{
    IntQueue *q = malloc(sizeof(*q));  /* queue object itself */
    if (!q) { perror("memory"); exit(EXIT_FAILURE); }

    /* Backing array: one int per job index. */
    q->data = malloc((size_t)capacity * sizeof(*q->data));
    if (!q->data) { perror("memory"); exit(EXIT_FAILURE); }

    q->size = q->head = 0;   /* empty queue starting at index 0 */
    q->capacity = capacity;
    return q;
}

/* Release a ready queue. */
static void queue_free(IntQueue *q) { free(q->data); free(q); }

/* Add a job index to the rear of the ready queue. */
static void push(IntQueue *q, int value)
{
    /* The queue can never hold more than one entry per job. */
    if (q->size == q->capacity) { fprintf(stderr, "queue overflow\n"); exit(EXIT_FAILURE); }

    /* The rear position is head + size, wrapped around the array. */
    q->data[(q->head + q->size++) % q->capacity] = value;
}

/* Remove and return the oldest job index from the ready queue. */
static int pop(IntQueue *q)
{
    int value = q->data[q->head];  /* oldest element */

    /* Advance the head with wraparound, then shrink the size. */
    q->head = (q->head + 1) % q->capacity;
    --q->size;
    return value;
}

/* Compare two job indices by arrival time, then by input-file order.
 * Signature matches qsort_r so the same rule can be reused elsewhere. */
static int arrival_order(const void *a, const void *b, void *arg)
{
    const Job *jobs = arg;              /* job array the indices refer to */
    int x = *(const int *)a;            /* first job index */
    int y = *(const int *)b;            /* second job index */

    /* Earlier arrival first. */
    if (jobs[x].arrival_time != jobs[y].arrival_time)
        return jobs[x].arrival_time - jobs[y].arrival_time;

    /* Same arrival: keep the original file order. */
    return jobs[x].input_index - jobs[y].input_index;
}

/* Schedule jobs with Round Robin using the given time quantum.
 * Jobs enter the ready queue in arrival order; a job that uses its quantum
 * without finishing is placed at the rear. An arrival during a slice cuts the
 * slice short so the Gantt chart shows exact arrival boundaries.
 * Inputs: q = loaded queue, quantum = time slice, c = chart to fill. */
void schedule_RR(JobQueue *q, int quantum, GanttChart *c)
{
    Job *jobs = working_copy(q);                          /* private working copy */
    IntQueue *ready = queue_new(q->count ? q->count : 1); /* ready queue */
    int *order = malloc((size_t)q->count * sizeof(*order)); /* indices sorted by arrival */
    int next = 0;   /* next position in order[] still to be admitted */
    int done = 0;   /* number of jobs finished */
    int time = 0;   /* current simulation time */
    int i;          /* loop index */

    chart_init(c);

    /* A non-positive quantum cannot be scheduled; bail out after cleanup. */
    if (quantum <= 0) { fprintf(stderr, "quantum must be positive\n"); goto finish; }

    /* order[] lists job indices sorted by (arrival, input order). */
    for (i = 0; i < q->count; ++i) order[i] = i;
    /* qsort_r is not portable; insertion sort keeps this small and portable. */
    for (i = 1; i < q->count; ++i) {
        int key = order[i], j = i;
        while (j && arrival_order(&key, &order[j - 1], jobs) < 0) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }

    /* Main scheduling loop: one iteration handles one job's turn. */
    while (done < q->count) {
        int used = 0;  /* quantum already consumed by the current job */
        int job;       /* index of the job currently running */

        /* Move every job that has arrived by now into the ready queue. */
        while (next < q->count && jobs[order[next]].arrival_time <= time)
            push(ready, order[next++]);

        /* Empty queue: the CPU is idle until the next job arrives. */
        if (!ready->size) {
            int idle = jobs[order[next]].arrival_time;  /* next arrival time */
            add_segment(c, time, idle, NULL);
            time = idle;
            continue;
        }

        /* Take the job at the front and give it up to one quantum. */
        job = pop(ready);

        /* Keep slicing until the quantum is used up or the job finishes. */
        while (used < quantum && jobs[job].remaining_time) {
            /* Default slice end: whatever part of the quantum is left. */
            int end = time + quantum - used;
            int slice;  /* actual length of this slice */

            /* Do not run past the job's own completion time. */
            if (end > time + jobs[job].remaining_time) end = time + jobs[job].remaining_time;

            /* Stop at the next arrival so it can enter the queue at the right time. */
            if (next < q->count && end > jobs[order[next]].arrival_time)
                end = jobs[order[next]].arrival_time;

            /* Record the slice and charge it to the job and to the quantum. */
            slice = end - time;
            add_segment(c, time, end, &jobs[job]);
            used += slice;
            time = end;
            jobs[job].remaining_time -= slice;

            /* New arrivals at this instant join before a preempted job is requeued. */
            while (next < q->count && jobs[order[next]].arrival_time <= time)
                push(ready, order[next++]);

            /* Job finished inside this slice. */
            if (!jobs[job].remaining_time) { ++done; break; }
        }

        /* Quantum used up but the job is unfinished: send it to the rear. */
        if (jobs[job].remaining_time) push(ready, job);
    }

finish:
    /* Release every temporary allocation, including the early-exit path. */
    free(order);
    queue_free(ready);
    free(jobs);
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

/* Load and validate jobs from the input file, preserving file order.
 * Skips blank lines, '#' comments, and a "job_id arrival burst priority" header.
 * Rejects malformed lines, duplicate IDs, and out-of-range values.
 * Returns: the number of jobs loaded, or -1 on error (queue left empty). */
int load_jobs(const char *path, JobQueue *q)
{
    FILE *fp;            /* input file opened for reading */
    char line[256];      /* buffer for one input line */
    int line_no = 0;     /* 1-based line number, used in error messages */

    /* Start from an empty queue so a failed load leaves no stale state. */
    *q = (JobQueue){0};

    /* Missing path or unreadable file: report and fail. */
    if (!path || !(fp = fopen(path, "r"))) { perror(path ? path : "input"); return -1; }

    /* Process one line of the file per iteration. */
    while (fgets(line, sizeof(line), fp)) {
        char id[128];    /* job ID as read (may exceed ID_LEN, checked later) */
        char extra;      /* catches an unexpected fifth field */
        char *p = line;  /* cursor used to skip leading whitespace */
        int a, b, pr;    /* arrival, burst, and priority of this job */
        int fields;      /* number of fields sscanf managed to parse */
        int i;           /* loop index for the duplicate-ID scan */

        ++line_no;       /* Increment the line number for error reporting. */

        /* Skip leading whitespace, then blank lines and comments. */
        while (isspace((unsigned char)*p)) ++p;
        if (!*p || *p == '#') continue;

        /* Parse exactly four fields; a fifth character means trailing junk. */
        fields = sscanf(p, "%127s %d %d %d %c", id, &a, &b, &pr, &extra);

        /* Tolerate the header line described in the specification. */
        if (fields == 1 && !strcasecmp(id, "job_id")) continue;

        /* Validate field count, ID length, and value ranges. */
        if (fields != 4 || strlen(id) > ID_LEN || a < 0 || b <= 0 || pr < 1 || pr > 5) {
            fprintf(stderr, "invalid input at line %d\n", line_no);
            fclose(fp); free_jobs(q); return -1;
        }

        /* Reject duplicate job IDs, which would make results ambiguous. */
        for (i = 0; i < q->count; ++i)
            if (!strcmp(q->jobs[i].job_id, id)) {
                fprintf(stderr, "duplicate job id at line %d\n", line_no);
                fclose(fp); free_jobs(q); return -1;
            }

        /* Grow the array when full (doubling keeps appends amortised O(1)). */
        if (q->count == q->capacity) {
            q->capacity = q->capacity ? q->capacity * 2 : 8;
            q->jobs = xalloc(q->jobs, (size_t)q->capacity * sizeof(*q->jobs));
        }

        /* Store the job; input_index records its position in the file. */
        q->jobs[q->count] = (Job){0};
        strcpy(q->jobs[q->count].job_id, id);
        q->jobs[q->count].arrival_time = a;
        q->jobs[q->count].burst_time = b;
        q->jobs[q->count].priority = pr;
        q->jobs[q->count].input_index = q->count;
        q->count++;
    }

    fclose(fp);     /* done reading the file */

    /* An empty input cannot be scheduled. */
    if (!q->count) { fprintf(stderr, "input contains no jobs\n"); return -1; }
    return q->count;
}

/* ------------------------------------------------------------------------- */
/* Metrics and output                                                        */
/* ------------------------------------------------------------------------- */

/* Compute CT, TAT, WT, their averages, and CPU utilisation from the chart.
 * CT = end of the job's last Gantt segment.
 * TAT = CT - arrival, WT = TAT - burst.
 * CPU utilisation = 100 * total burst / final completion time, so initial
 * idle time from t = 0 reduces the utilisation.
 * Inputs: c = completed chart, q = loaded queue.
 * Side effect: fills *m and allocates m->jobs (caller frees it). */
void compute_metrics(const GanttChart *c, const JobQueue *q, Metrics *m)
{
    int i;                                        /* loop index over jobs */
    int j;                                        /* loop index over segments */
    int final = c->count ? c->segments[c->count - 1].end_time : 0; /* run end */
    double tat = 0;                               /* running sum of TAT */
    double wt = 0;                                /* running sum of WT */
    double burst = 0;                             /* running sum of burst time */

    m->job_count = q->count;

    /* One metrics record per job, in input order. */
    m->jobs = malloc((size_t)q->count * sizeof(*m->jobs));
    if (!m->jobs) { perror("memory"); exit(EXIT_FAILURE); }

    for (i = 0; i < q->count; ++i) {
        JobMetrics *x = &m->jobs[i];  /* record being filled */

        /* Copy the fixed fields straight from the input job. */
        strcpy(x->job_id, q->jobs[i].job_id);
        x->arrival_time = q->jobs[i].arrival_time;
        x->burst_time = q->jobs[i].burst_time;

        /* Scan the chart backwards: the first match is the job's last piece. */
        x->completion_time = 0;
        for (j = c->count - 1; j >= 0; --j)
            if (!c->segments[j].idle && !strcmp(c->segments[j].job_id, x->job_id)) {
                x->completion_time = c->segments[j].end_time; break;
            }

        /* Derive the two standard scheduling metrics. */
        x->turnaround_time = x->completion_time - x->arrival_time;
        x->waiting_time = x->turnaround_time - x->burst_time;

        /* Accumulate for the averages. */
        tat += x->turnaround_time; wt += x->waiting_time; burst += x->burst_time;
    }

    /* Averages use floating-point division over the job count. */
    m->avg_tat = tat / q->count;
    m->avg_wt = wt / q->count;
    m->cpu_utilization = final ? 100.0 * burst / final : 0;
}

/* Print the Gantt chart and per-job metrics in the required format.
 * Job rows stay in input-file order; averages use two decimal places. */
void print_schedule(const char *name, const GanttChart *c, const Metrics *m)
{
    int width = 4;  /* minimum Gantt/table width; grows to the longest ID */
    int i;          /* loop index over segments or jobs */

    /* Choose a column width that fits the longest job ID. */
    for (i = 0; i < c->count; ++i)
        if (!c->segments[i].idle && (int)strlen(c->segments[i].job_id) > width)
            width = (int)strlen(c->segments[i].job_id);
    for (i = 0; i < m->job_count; ++i)
        if ((int)strlen(m->jobs[i].job_id) > width)
            width = (int)strlen(m->jobs[i].job_id);

    /* Header line naming the algorithm. */
    printf("=== %s ===\nGantt:", name);

    /* One "| job" cell per segment; IDLE is shown for idle segments. */
    for (i = 0; i < c->count; ++i)
        printf(" | %-*s", width, c->segments[i].idle ? "IDLE" : c->segments[i].job_id);
    printf(" |\nTime:  ");

    /* Time axis: each segment's start, then the final end time. */
    for (i = 0; i < c->count; ++i) printf("%-*d", width + 3, c->segments[i].start_time);
    printf("%d\n\n", c->count ? c->segments[c->count - 1].end_time : 0);

    /* Metric table header, then one row per job in input order. */
    printf("%-*s %3s %3s %3s %4s %3s\n", width, "Job", "AT", "BT", "CT", "TAT", "WT");
    for (i = 0; i < m->job_count; ++i)
        printf("%-*s %2d %3d %3d %3d %4d\n", width, m->jobs[i].job_id,
               m->jobs[i].arrival_time, m->jobs[i].burst_time,
               m->jobs[i].completion_time, m->jobs[i].turnaround_time,
               m->jobs[i].waiting_time);

    /* Summary line with the three required averages. */
    printf("Avg TAT: %.2f   Avg WT: %.2f   CPU Util: %.2f%%\n", m->avg_tat, m->avg_wt,
           m->cpu_utilization);
}

/* Run one algorithm, print its schedule, and release its temporary data.
 * Inputs: display name, scheduling function, and the loaded queue. */
static void run(const char *name, void (*schedule)(JobQueue *, GanttChart *), JobQueue *q)
{
    GanttChart c;   /* chart produced by this algorithm */
    Metrics m;      /* metrics derived from that chart */

    schedule(q, &c);              /* build the Gantt chart */
    compute_metrics(&c, q, &m);   /* turn the chart into CT/TAT/WT */
    print_schedule(name, &c, &m); /* show the results */

    /* Release temporary data so the next run starts clean. */
    free(m.jobs);
    chart_free(&c);
}

/* Run one Round Robin configuration with the given quantum. */
static void run_rr(const char *name, int quantum, JobQueue *q)
{
    GanttChart c;   /* chart produced by this RR run */
    Metrics m;      /* metrics derived from that chart */

    schedule_RR(q, quantum, &c);
    compute_metrics(&c, q, &m);
    print_schedule(name, &c, &m);

    /* Release temporary data so the next run starts clean. */
    free(m.jobs);
    chart_free(&c);
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

/* Load the input file once, then run the five required configurations on the
 * same data in the order required by the specification.
 * Exit status: EXIT_SUCCESS when every run completed, EXIT_FAILURE on a usage
 * or input error. */
int main(int argc, char **argv)
{
    JobQueue q;   /* jobs loaded once and reused by all five runs */

    /* Require exactly one argument: the path to the job file. */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <orders_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Read and validate the whole input before scheduling anything. */
    if (load_jobs(argv[1], &q) < 0) return EXIT_FAILURE;

    /* Each run starts from a fresh copy of the jobs (see working_copy), so
     * the algorithms cannot affect one another's results. */
    run("FCFS", schedule_FCFS, &q); printf("\n");   /* required configuration 1 */
    run("SJF", schedule_SJF, &q);   printf("\n");   /* required configuration 2 */

    run("Preemptive Priority", schedule_priority_preemptive, &q);   /* required configuration 3 */
    printf("\n");

    run_rr("Round Robin (q=3)", RR3, &q);   /* required configuration 4 */
    printf("\n");

    run_rr("Round Robin (q=6)", RR6, &q);   /* required configuration 5 */

    /* Release the shared job array before exiting. */
    free_jobs(&q);
    return EXIT_SUCCESS;
}
