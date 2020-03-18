/*
 * Example on creating a monitor for a critical region and make sure
 * that not two processes can access it at the same time (traditional
 * mutual exclusion) using POSIX semaphores and shared memory.
 *
 * In this case, we are using semaphores rather than mutexes and place
 * the semaphore in shared memory together with the data it protects.
 *
 * As an added twist, we allow the children to crash randomly with the
 * semaphore taken and the parent should release the semaphore for the
 * child if it is not done.
 *
 * This can somewhat handle the situation that System V semaphores
 * tries to handle with SEM_UNDO, but require the parent process to do
 * the compensating action.
 *
 * This require information to be stored in a shared memory that the
 * parent process can check when it detects a crash, so we allocate a
 * piece of shared memory holding both the semaphore, the data being
 * protected, and data necessary for the coordination.
 */

#include <errno.h>
#include <fcntl.h> /* For O_* constants */
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h> /* For mode constants */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Structure for the shared memory.
 *
 * The semaphore and the owner are used to coordinate the usage of the
 * semaphore and the memory region. The data actually protected by the
 * semaphore is the counter.
 */
typedef struct Memory {
  sem_t sem_id;
  int owner;
  int counter;
} Memory;

static Memory *g_memory;

/*
 * The child process.
 *
 * It will then repeatedly take the semaphore, increase a counter,
 * sleep for a while, and then release the semaphore. To simulate a
 * failure, it will randomly send a signal to itself, causing it to
 * crash.
 */
void child(void) {
  pid_t pid = getpid();
  for (;;) {
    fprintf(stderr, "%5u: waiting for semaphore\n", pid);
    sem_wait(&g_memory->sem_id);
    fprintf(stderr, "%5u: entering region\n", pid);
    g_memory->owner = pid;

    /* Randomly commit suicide without releasing the semaphore. We are
       using a floating-point exception since that allow us to
       distinguish the simulated crash from a real crash. */
    if (rand() % 7 == 0) {
      fprintf(stderr, "%5u: process died!\n", pid);
      kill(pid, SIGFPE);
    }

    sleep(rand() % 5);
    g_memory->counter += 1;
    g_memory->owner = 0;
    fprintf(stderr, "%5u: counter: %d\n", pid, g_memory->counter);
    fprintf(stderr, "%5u: leaving region\n", pid);
    sem_post(&g_memory->sem_id);
    sleep(rand() % 5);
  }
}

/*
 * Configure will set up the shared memory and the semaphores.
 */
void configure(void) {
  int fd, res;
  void *addr;

  fd = shm_open("/monitor", O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd == -1) {
    fprintf(stderr, "%s: shm_open - %s\n", __func__, strerror(errno));
    return;
  }

  res = ftruncate(fd, sizeof(*g_memory));
  if (res == -1) {
    fprintf(stderr, "ftruncate");
    return;
  }

  addr =
      mmap(NULL, sizeof(*g_memory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    fprintf(stderr, "mmap");
    return;
  }

  g_memory = addr;

  if (sem_init(&g_memory->sem_id, 1, 1) < 0) {
    fprintf(stderr, "sem_init");
    return;
  }
}

void shutdown(void) {
  if (sem_destroy(&g_memory->sem_id) != 0) {
    fprintf(stderr, "%s: [sem_destroy] failed - %s\n", __func__,
            strerror(errno));
    return;
  }

  if (shm_unlink("/monitor") < 0) {
    fprintf(stderr, "%s: [shm_unlink] failed\n", __func__);
    return;
  }
}

int main() {
  int active = 0;
  pid_t my_pid = getpid();

  configure();

  /* Spawn the processes */
  while (active++ < 10) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      exit(EXIT_FAILURE);
    }

    if (pid == 0) {
      child();
      exit(EXIT_SUCCESS);
    } else {
      fprintf(stderr, "%5u: spawned child with pid %u\n", my_pid, pid);
    }
  }

  /* Wait for the processes to exit */
  while (--active > 0) {
    int status;
    fprintf(stderr, "%5u: waiting for child to exit\n", my_pid);
    pid_t child_pid = wait(&status);
    if (child_pid < 0) {
      perror("wait");
      break;
    } else {
      fprintf(stderr, "%5u: child with pid %d exited\n", my_pid, child_pid);
      if (WIFEXITED(status)) {
        fprintf(stderr, "%5u: pid %d exited normally with exits status %d\n",
                my_pid, child_pid, WEXITSTATUS(status));
      } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "%5u: pid %d exited with termination signal %d\n",
                my_pid, child_pid, WTERMSIG(status));
        if (WCOREDUMP(status))
          fprintf(stderr, "%5u: pid %d exited with core dump\n", my_pid,
                  child_pid);
      }

      /* Release the semaphore if the child died under suspicious circumstances
       */
      fprintf(stderr, "%5u: semaphore owned by %u\n", my_pid, g_memory->owner);
      if (g_memory->owner != 0)
        sem_post(&g_memory->sem_id);
    }
  }

  shutdown();

  return EXIT_SUCCESS;
}
