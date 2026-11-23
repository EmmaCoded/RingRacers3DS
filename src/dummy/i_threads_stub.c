// DR. ROBOTNIK'S RING RACERS — 3DS threading stubs
// HAVE_THREADS is defined globally, but the 3DS has no real implementation.
// All functions are no-ops; I_spawn_thread runs the function inline (synchronously).

#include "../i_threads.h"

void I_start_threads(void) {}
void I_stop_threads(void) {}

void I_spawn_thread(const char *name, I_thread_fn fn, void *userdata)
{
	(void)name;
	// No real threads: run synchronously so the work is not lost.
	fn(userdata);
}

int I_thread_is_stopped(void)
{
	return 1;
}

void I_lock_mutex(I_mutex *m)
{
	(void)m;
}

void I_unlock_mutex(I_mutex m)
{
	(void)m;
}

void I_hold_cond(I_cond *c, I_mutex m)
{
	(void)c;
	(void)m;
}

void I_wake_one_cond(I_cond *c)
{
	(void)c;
}

void I_wake_all_cond(I_cond *c)
{
	(void)c;
}
