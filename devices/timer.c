#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"


/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);


/* NEWCODE!!!*/
//List of threads in SLEEP state
static struct list asleep_list;
/* END OF NEWCODE */


/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
	
	/* NEWCODE : init the new asleep_list */
	list_init (&asleep_list);
	/* END OF NEWCODE */
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
void
timer_sleep (int64_t ticks) {
	enum intr_level old_level;

	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_ON);
	old_level = intr_disable ();
	
	int64_t start = timer_ticks ();

	//ASSERT (intr_get_level () == INTR_ON);
	/* NEWCODE */
	//Same as thread_yield(), but put thread in asleep_list.
	struct thread *curr = thread_current ();
	//ASSERT(curr != idle_thread());
	curr->alarm_ticks = start + ticks;
	list_push_back (&asleep_list, &curr->elem);
	printf("thread %d is going to sleep, instead of thread %d.\n", curr->tid, thread_current()->tid);
	//thread_current()->status = THREAD_BLOCKED;
	thread_block();
	intr_set_level (old_level);
	/* ENDOFNEWCODE */
	
	/*
	while (timer_elapsed (start) < ticks)
		thread_yield ();
	*/
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}



/* Alarm function. */
static void timer_alarm(void){
	//iterate through asleep_list, wake up any threads that are past "alarm time".
	intr_yield_on_return();
	
	struct thread* th;
	struct list_elem* i;
	i = list_begin(&asleep_list);
	while(i != list_end(&asleep_list)){
		th = list_entry(i,struct thread, elem);
		if(ticks >= th->alarm_ticks){	//past alarm time.
			i = list_remove(i);
			thread_unblock(th);
		}
		else{
			i = list_next(i);
		}
	}
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();
	/* NEWCODE FOR MLFQS SCHEDULER!!*/
	enum intr_level old_level;
	old_level = intr_disable ();
	if(thread_mlfqs){
		printf("current thread : %d\n", thread_current()->tid);
		//1.EVERY INTERRUPT -> increment recent_cpu.
		//mlfqs_increment();
		printf("incremented recent cpu\n");
		//2.Every second -> recalculate load_avg -> recent_cpu.
		if(ticks % TIMER_FREQ == 0){
			printf("calculating load avg : %d seconds..\n", (int)(ticks / TIMER_FREQ));
			mlfqs_load_avg();
			printf("calculating recent_cpu for all threads..\n");
			mlfqs_recalc();
		}
		//3.Every 4 ticks -> recalculate every thread's priority.
		if(ticks % 4 == 0){
			printf("calculating all thread priority..\n");
			mlfqs_recalc_threads();
			//mlfqs_priority(thread_current());
		}
	}
	intr_set_level (old_level);
	/* NEWCODE */
	//call alarm function.
	timer_alarm();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
