/*
 * Timer.cpp
 *
 *  Created on: Jan 2, 2012
 *      Author: William Boyd
 *				MIT, Course 22
 *              wboyd@mit.edu
 *  The timer class is for profiling code. It outputs running time in
 *  seconds but has resolution of microseconds on OSX and nanoseconds
 *  on Linux machines.
 */

#include "Timer.h"

/**
 * Timer class constructor
 */
Timer::Timer() {
	this->running = false;
	this->elapsed_time = 0;
}


/**
 * Default Timer destructor
 */
Timer::~Timer() { }


/**
 * Starts the Timer - similar to starting a stopwatch
 */
void Timer::start() {

	if (!this->running) {
		#ifdef __MACH__ 	/* For OSX */
			gettimeofday(&this->start_time, NULL);
		#else				/* For linux */
			clock_gettime(CLOCK_MONOTONIC, &this->start_time);
		#endif
		this->running = true;
	}
	return;
}


/**
 * Stops the timer - similar to stopping a stopwatch
 */
void Timer::stop() {
	if (this->running) {
		#ifdef __MACH__ /* For OSX */
			gettimeofday(&this->end_time, NULL);
		#else			/* For linux */
		  clock_gettime(CLOCK_MONOTONIC, &this->end_time);
		#endif
		this->running = false;
		this->elapsed_time += this->diff(this->start_time, this->end_time);
	}
	return;
}


/**
 * Resets the timer - similar to resetting a stopwatch.
 */
void Timer::reset() {
	this->elapsed_time = 0;
	this->running = false;
}


/**
 * Restarts the timer. The elapsed time will accumulate along with the
 * previous time(s) the timer was running. If the timer was already running
 * this function does nothing
 */
void Timer::restart() {
	if (!this->running) {
		this->elapsed_time += this->diff(this->start_time, this->end_time);
		this->start();
	}
}


/**
 * Returns the amount of time elapsed from start to stop of the timer. If the
 * timer is currently runnning, returns the time from the timer start to the present
 * time.
 * @return the elapsed time
 */
double Timer::getTime() {
	/* If the timer is not running */
	if (!this->running) {
		#ifdef __MACH__		/* For OSX */
			return this->elapsed_time * 1.0E-6;
		#else				/* For Linux */
			return this->elapsed_time * 1.0E-9;
		#endif
	}

	/* If the timer is currently running */
	else {
		timespec temp;
		#ifdef __MACH__ 	/* For OSX */
			gettimeofday(&temp, NULL);
		#else				/* For Linux */
		  clock_gettime(CLOCK_MONOTONIC, &temp);
		#endif

		this->elapsed_time += this->diff(this->start_time, temp);

		#ifdef __MACH__		/* For OSX */
			return this->elapsed_time * 1.0E-6;
		#else				/* For Linux */
			return this->elapsed_time * 1.0E-9;
		#endif
	}
}


/**
 * Helper function which computes the time between the values of
 * two timespec structs.
 * @param start timespec representing the start time
 * @param end timespec representing the end time
 */
double Timer::diff(timespec start, timespec end) {
	double sec, delta;
	#ifdef __MACH__
		double usec;
		delta = end.tv_usec - start.tv_usec;
	#else
		double nsec;
		delta = end.tv_nsec - start.tv_nsec;
	#endif

	if (delta < 0) {
		sec = end.tv_sec - start.tv_sec;
		#ifdef __MACH__
			usec = 1.0E6 + delta;
		#else
			nsec = 1.0E9 + delta;
		#endif

	} else {
		sec = end.tv_sec - start.tv_sec;
		#ifdef __MACH__
			usec = delta;
		#else
			nsec = delta;
		#endif
	}

	#ifdef __MACH__
		return (sec * 1.0E6 + usec);
	#else
		return(sec*1.0E9 + nsec);
	#endif
}
