// -*- mode:C++; tab-width:4; c-basic-offset:2; indent-tabs-mode:t -*- 
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */




#include "Timer.h"
#include "Cond.h"

#include "config.h"
#include "include/Context.h"

//#include "msg/Messenger.h"

#undef dout
#define dout(x)  if (x <= g_conf.debug) cout << "Timer: "

#define DBL 10

#include <signal.h>
#include <sys/time.h>
#include <math.h>

// single global instance
Timer      g_timer;

//Context *messenger_kicker = 0;
//Messenger *messenger = 0;



/**** thread solution *****/

void Timer::timer_entry()
{
  lock.Lock();
  
  while (!thread_stop) {
	
	// now
	utime_t now = g_clock.now();

	// any events due?
	utime_t next;
	Context *event = get_next_scheduled(next);
	
	list<Context*> pending;
	
	if (event && now >= next) {
	  // move to pending list
	  map< utime_t, multiset<Context*> >::iterator it = scheduled.begin();
	  while (it != scheduled.end()) {
		if (it->first > now) break;

		utime_t t = it->first;
		dout(DBL) << "queueing event(s) scheduled at " << t << endl;

		/*if (messenger) {
		  for (multiset<Context*>::iterator cit = it->second.begin();
			   cit != it->second.end();
			   cit++) {
			pending.push_back(*cit);
			event_times.erase(*cit);
			num_event--;
		  }
		}
		*/

		//pending[t] = it->second;
		map< utime_t, multiset<Context*> >::iterator previt = it;
		it++;
		scheduled.erase(previt);
	  }

	  if (!pending.empty()) {
		sleeping = false;
		lock.Unlock();
		{ // make sure we're not holding any locks while we do callbacks (or talk to the messenger)
		  if (1) {
			// make the callbacks myself.
			for (list<Context*>::iterator cit = pending.begin();
				 cit != pending.end();
				 cit++) 
			  (*cit)->finish(0);
			pending.clear();
		  } else {
			// give them to the messenger
			//messenger->queue_callbacks(pending);
		  }
		  assert(pending.empty());
		}
		lock.Lock();
	  }

	}

	else {
	  // sleep
	  if (event) {
		dout(DBL) << "sleeping until " << next << endl;
		timed_sleep = true;
		sleeping = true;
		timeout_cond.WaitUntil(lock, next);  // wait for waker or time
		utime_t now = g_clock.now();
		dout(DBL) << "kicked or timed out at " << now << endl;
	  } else {
		dout(DBL) << "sleeping" << endl;
		timed_sleep = false;
		sleeping = true;
		sleep_cond.Wait(lock);         // wait for waker
		utime_t now = g_clock.now();
		dout(DBL) << "kicked at " << now << endl;
	  }
	}
  }

  lock.Unlock();
}



/**
 * Timer bits
 */

/*
void Timer::set_messenger(Messenger *m)
{
  dout(10) << "set messenger " << m << endl;
  messenger = m;
}
void Timer::unset_messenger()
{
  dout(10) << "unset messenger" << endl;
  messenger = 0;
}
*/

void Timer::register_timer()
{
  if (timer_thread.is_started()) {
	if (sleeping) {
	  dout(DBL) << "register_timer kicking thread" << endl;
	  if (timed_sleep)
		timeout_cond.SignalAll();
	  else
		sleep_cond.SignalAll();
	} else {
	  dout(DBL) << "register_timer doing nothing; thread is alive but not sleeping" << endl;
	  // it's probably doing callbacks.
	}
  } else {
	dout(DBL) << "register_timer starting thread" << endl;
	timer_thread.create();
	//pthread_create(&thread_id, NULL, timer_thread_entrypoint, (void*)this);
  }
}

void Timer::cancel_timer()
{
  // clear my callback pointers
  if (timer_thread.is_started()) {
	dout(10) << "setting thread_stop flag" << endl;
	lock.Lock();
	thread_stop = true;
	if (timed_sleep)
	  timeout_cond.SignalAll();
	else
	  sleep_cond.SignalAll();
	lock.Unlock();
	
	dout(10) << "waiting for thread to finish" << endl;
	void *ptr;
	timer_thread.join(&ptr);//pthread_join(thread_id, &ptr);
	
	dout(10) << "thread finished, exit code " << ptr << endl;
  }
}


/*
 * schedule
 */


void Timer::add_event_after(float seconds,
							Context *callback) 
{
  utime_t when = g_clock.now();
  when.sec_ref() += (int)seconds;
  add_event_at(when, callback);
}

void Timer::add_event_at(utime_t when,
						 Context *callback) 
{
  // insert
  dout(DBL) << "add_event " << callback << " at " << when << endl;

  lock.Lock();
  scheduled[ when ].insert(callback);
  assert(event_times.count(callback) == 0);     // err.. there can be only one (for now!)
  event_times[callback] = when;
  
  num_event++;

  // make sure i wake up
  register_timer();

  lock.Unlock();
}

bool Timer::cancel_event(Context *callback) 
{
  lock.Lock();
  
  dout(DBL) << "cancel_event " << callback << endl;

  if (!event_times.count(callback)) {
	dout(DBL) << "cancel_event " << callback << " wasn't scheduled?" << endl;
	lock.Unlock();
	assert(0);
	return false;     // wasn't scheduled.
  }

  utime_t tp = event_times[callback];
  assert(scheduled.count(tp));

  multiset<Context*>::iterator p = scheduled[tp].find(callback);  // there may be more than one?
  assert(p != scheduled[tp].end());
  scheduled[tp].erase(p);

  event_times.erase(callback);
  
  lock.Unlock();
  return true;
}
