/* Student starter for Problem 1. Complete the TODOs before submission. */
/* Build on Linux: gcc -std=gnu11 -Wall -Wextra -pthread problem1.c -o problem1 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <string.h>

/* Edit these macros and recompile to change the configuration. */
#ifndef BUFFER_A_SIZE
#define BUFFER_A_SIZE 5
#endif
#ifndef BUFFER_B_SIZE
#define BUFFER_B_SIZE 5
#endif
#ifndef NUM_PRODUCERS
#define NUM_PRODUCERS 3
#endif
#ifndef NUM_WORKERS
#define NUM_WORKERS 3
#endif
#ifndef NUM_CONSUMERS
#define NUM_CONSUMERS 2
#endif
#ifndef ITEMS_PER_PRODUCER
#define ITEMS_PER_PRODUCER 4
#endif
#define TOTAL_ITEMS (NUM_PRODUCERS * ITEMS_PER_PRODUCER)

#ifndef PROD_DELAY_US
#define PROD_DELAY_US 0
#endif
#ifndef WORK_DELAY_US
#define WORK_DELAY_US 0
#endif
#ifndef CONS_DELAY_US
#define CONS_DELAY_US 0
#endif

typedef struct {
    int id;
    int producer_id;
    int value;
    int is_term; // 0=regular item, 1= termination item
} item_t;

typedef struct {
    int id;
    int producer_id;
    int worker_id;
    long result;
    int is_term; // 0=regular item, 1= termination item
} result_t;

typedef struct {
    item_t data[BUFFER_A_SIZE];
    int in;
    int out;
    sem_t mutex;
    sem_t empty;
    sem_t full;
} input_buffer_t;

typedef struct {
    result_t data[BUFFER_B_SIZE];
    int in;
    int out;
    sem_t mutex;
    sem_t empty;
    sem_t full;
} output_buffer_t;

typedef struct {
    input_buffer_t input;
    output_buffer_t output;
    /* DID: add shared result records and termination state as needed.
       You may extend these structures and add synchronization. */
    result_t results[TOTAL_ITEMS];
    int seen_count[TOTAL_ITEMS]; //for verification - make sure seen_count[id]=1
    int total_collected;
    int out_of_range_count; // counts out of range ids
    sem_t results_mutex;

    int producers_done;
    int workers_done;
    sem_t term_mutex;
} shared_t;

static void buffer_a_put(shared_t *sh, item_t item)
{
    sem_wait(&sh->input.empty);
    sem_wait(&sh->input.mutex);
    sh->input.data[sh->input.in] = item;
    sh->input.in = (sh->input.in + 1) % BUFFER_A_SIZE;
    sem_post(&sh->input.mutex);
    sem_post(&sh->input.full);
}

static item_t buffer_a_get(shared_t *sh)
{
    sem_wait(&sh->input.full);
    sem_wait(&sh->input.mutex);
    item_t tmp = sh->input.data[sh->input.out];
    sh->input.out = (sh->input.out + 1) % BUFFER_A_SIZE;
    sem_post(&sh->input.mutex);
    sem_post(&sh->input.empty);
    return tmp;
}
static void buffer_b_put(shared_t *sh, result_t res)
{
    sem_wait(&sh->output.empty);
    sem_wait(&sh->output.mutex);
    sh->output.data[sh->output.in] = res;
    sh->output.in = (sh->output.in + 1) % BUFFER_B_SIZE;
    sem_post(&sh->output.mutex);
    sem_post(&sh->output.full);

}
static result_t buffer_b_get(shared_t *sh)
{
    sem_wait(&sh->output.full);
    sem_wait(&sh->output.mutex);
    result_t tmp = sh->output.data[sh->output.out];
    sh->output.out = (sh->output.out + 1) % BUFFER_B_SIZE;
    sem_post(&sh->output.mutex);
    sem_post(&sh->output.empty);
    return tmp;
}

static void signal_producer_done(shared_t *sh)
{
    sem_wait(&sh->term_mutex);
    int is_last = (++sh->producers_done == NUM_PRODUCERS);
    sem_post(&sh->term_mutex);

    if (is_last) {
        item_t sentinel = { .id = -1, .producer_id = -1, .value = 0, .is_term = 1 };
        for (int i = 0; i < NUM_WORKERS; ++i) {
            buffer_a_put(sh, sentinel);
        }
    }
}

static void signal_worker_done(shared_t *sh)
{
    sem_wait(&sh->term_mutex);
    int is_last = (++sh->workers_done == NUM_WORKERS);
    sem_post(&sh->term_mutex);

    if (is_last) {
        result_t sentinel = {
            .id = -1, .producer_id = -1, .worker_id = -1, .result = 0, .is_term = 1
        };
        for (int i = 0; i < NUM_CONSUMERS; ++i) {
            buffer_b_put(sh, sentinel);
        }
    }
}

/* Generate this producer's input items and insert them into Buffer A. */
static void producer(shared_t *sh, int producer_id)
{
    /* DID: for j = 0 .. ITEMS_PER_PRODUCER-1, generate an item with
       id = producer_id * ITEMS_PER_PRODUCER + j, producer_id as supplied,
       and value = id + 1. Insert it into sh->input with synchronization.
       Generate the item outside the buffer's critical section.
       Use BUFFER_A_SIZE for circular-buffer wraparound.
       Complete this producer's part of your termination protocol. */
    for (int j=0; j<ITEMS_PER_PRODUCER; j++)
    {
        item_t item;
        item.is_term = 0;
        item.id = producer_id * ITEMS_PER_PRODUCER + j;
        item.producer_id = producer_id;
        item.value = item.id + 1;
        if (PROD_DELAY_US > 0) usleep(PROD_DELAY_US);
        buffer_a_put(sh, item);
    }
    signal_producer_done(sh);

}

/* Transfer input items through the computation stage into Buffer B. */
static void worker(shared_t *sh, int worker_id)
{
    /* DID: remove an item from sh->input and release its lock.
       Compute (long)item.value * item.value outside the critical section.
       Preserve id and producer_id, and record this worker_id.
       Insert the result into sh->output using BUFFER_B_SIZE for wraparound.
       Follow your termination protocol; an empty buffer is not completion. */
    while (1)
    {
        item_t item = buffer_a_get(sh);
        if (item.is_term)
        {
            break;
        }
        result_t res;
        res.id = item.id;
        res.producer_id = item.producer_id;
        res.worker_id = worker_id;
        res.result = (long)item.value*item.value;
        res.is_term = 0;
        if (WORK_DELAY_US > 0) usleep(WORK_DELAY_US);
        buffer_b_put(sh, res);
    }
    signal_worker_done(sh);
}

/* Collect Buffer B results into synchronized shared records. */
static void consumer(shared_t *sh, int consumer_id)
{

    /* DID: remove results from sh->output and save them in shared memory.
       Synchronize shared updates and retain enough information for the
       parent to detect missing, duplicate, or out-of-range item IDs.
       Exclude termination records and follow your termination protocol.
       The parent prints the final results in ascending item ID order. */
    (void)consumer_id;
    while (1)
    {
        result_t res = buffer_b_get(sh);
        if (res.is_term)
        {
            break;
        }
        if (CONS_DELAY_US > 0) usleep(CONS_DELAY_US);
        sem_wait(&sh->results_mutex);
        sh->total_collected++;
        if (res.id < 0 || res.id >= TOTAL_ITEMS)
        {
            sh->out_of_range_count++;
        }
        else
        {
            sh->results[res.id] = res;
            sh->seen_count[res.id]++;
        }
        sem_post(&sh->results_mutex);
    }
}

/* Initialize the shared buffer indices and process-shared semaphores. */
static int init_shared(shared_t *sh)
{
    sh->input.in = sh->input.out = 0;
    sh->output.in = sh->output.out = 0;

    for (int i = 0; i < TOTAL_ITEMS; ++i) {
        sh->seen_count[i] = 0;
    }
    sh->total_collected = 0;
    sh->out_of_range_count = 0;
    sh->producers_done = 0;
    sh->workers_done = 0;
    memset(sh->results, 0, sizeof(sh->results));

    if (sem_init(&sh->input.mutex, 1, 1) == -1 ||
        sem_init(&sh->input.empty, 1, BUFFER_A_SIZE) == -1 ||
        sem_init(&sh->input.full, 1, 0) == -1 ||
        sem_init(&sh->output.mutex, 1, 1) == -1 ||
        sem_init(&sh->output.empty, 1, BUFFER_B_SIZE) == -1 ||
        sem_init(&sh->output.full, 1, 0) == -1 ||
        sem_init(&sh->results_mutex, 1, 1) == -1 ||
        sem_init(&sh->term_mutex, 1, 1) == -1) {
        perror("sem_init");
        return -1;
        }
    /* DID: initialize any added shared records, synchronization, and
       termination state here, before any child process is created. */
    return 0;
}

/* Create shared resources, launch all roles, verify results, and clean up. */
int main(void)
{
    shared_t *sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    if (init_shared(sh) != 0) {
        munmap(sh, sizeof(*sh));
        return EXIT_FAILURE;
    }

    int created = 0;

    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
        if (pid == 0) { producer(sh, i); _exit(EXIT_SUCCESS); }
        ++created;
    }

    for (int i = 0; i < NUM_WORKERS; ++i) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
        if (pid == 0) { worker(sh, i); _exit(EXIT_SUCCESS); }
        ++created;
    }

    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
        if (pid == 0) { consumer(sh, i); _exit(EXIT_SUCCESS); }
        ++created;
    }

    /* All process groups must be started before waiting for children.
       DID(part of is_term mechanism): add any parent-side actions required by your termination
       protocol at the appropriate point; do not wait for a process that
       still needs a termination action from the parent. */
    /* DID: collect and check each child's exit status, rather than
       discarding it with wait(NULL). */
    int children_ok =1;
    for (int i = 0; i < created; ++i)
    {
        int status = 0;
        pid_t pid = wait(&status);
        if (pid < 0)
        {
            perror("wait");
            children_ok = 0;
        }
        else
        {
            if (!WIFEXITED(status) || WEXITSTATUS(status)!= EXIT_SUCCESS)
            {
                children_ok = 0;
                fprintf(stderr, "Child exited with status %d\n", status);
            }
        }
    }

    int verification_ok = children_ok;
    if (sh->out_of_range_count != 0)
    {
        fprintf(stderr, "Out-of-range error\n");
        verification_ok = 0;
    }
    if (sh->total_collected != TOTAL_ITEMS)
    {
        fprintf(stderr, "Total collected error\n");
        verification_ok = 0;
    }
    for (int id=0; id<TOTAL_ITEMS; ++id)
    {
        if (sh->seen_count[id] == 0)
        {
            fprintf(stderr, "Missing id\n");
            verification_ok = 0;
            continue;
        }
        if (sh->seen_count[id] > 1)
        {
            fprintf(stderr, "Duplicate id\n");
            verification_ok = 0;
        }
        result_t res = sh->results[id];
        long expected_value = (long)(id + 1);
        int expected_producer_id = id / ITEMS_PER_PRODUCER;
        long expected_result = (long)expected_value * expected_value;
        if (res.id != id)
        {
            fprintf(stderr, "id missmatch\n");
            verification_ok = 0;
        }
        if (res.producer_id != expected_producer_id)
        {
            fprintf(stderr, "Producer id missmatch\n");
            verification_ok = 0;
        }
        if (res.worker_id < 0 || res.worker_id >= NUM_WORKERS)
        {
            fprintf(stderr, "Worker id error\n");
            verification_ok = 0;
        }
        if (res.result != expected_result)
        {
            fprintf(stderr, "Result missmatch\n");
            verification_ok = 0;
        }
    }
        printf("%-4s %-12s %-10s %s\n", "id", "producer_id", "worker_id", "result");
        for (int id = 0; id < TOTAL_ITEMS; ++id) {
            if (sh->seen_count[id] > 0) {
                result_t res = sh->results[id];
                printf("%-4d %-12d %-10d %ld\n",
                       res.id, res.producer_id, res.worker_id, res.result);
            }
        }
    /* DID: before unmapping shared memory, verify the collected results:
       - every ID from 0 to NUM_PRODUCERS * ITEMS_PER_PRODUCER - 1 occurs once;
       - producer_id and result match the generated item;
       - worker_id is within 0 .. NUM_WORKERS-1;
       - the total is correct, with no out-of-range IDs.
       Print the results in ascending item ID order.
       Set verification_ok only after verification and child-status checks
       have succeeded. This placeholder is not a completed verifier. */

    /* DID: destroy any additional semaphores you initialize. */
    sem_destroy(&sh->input.mutex);
    sem_destroy(&sh->input.empty);
    sem_destroy(&sh->input.full);
    sem_destroy(&sh->output.mutex);
    sem_destroy(&sh->output.empty);
    sem_destroy(&sh->output.full);

    sem_destroy(&sh->results_mutex);
    sem_destroy(&sh->term_mutex);

    munmap(sh, sizeof(*sh));

    if (!verification_ok) {
        fprintf(stderr, "Starter incomplete or verification failed.\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}