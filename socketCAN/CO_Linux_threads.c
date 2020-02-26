/*
 * Helper functions for implementing CANopen threads in Linux
 *
 * @file        Linux_threads.c
 * @ingroup     CO_driver
 * @author      Janez Paternoster, Martin Wagner
 * @copyright   2004 - 2015 Janez Paternoster, 2017 - 2020 Neuberger Gebaeudeautomation GmbH
 *
 *
 * This file is part of CANopenNode, an opensource CANopen Stack.
 * Project home page is <https://github.com/CANopenNode/CANopenNode>.
 * For more information on CANopen see <http://www.can-cia.org/>.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/timerfd.h>
#include <fcntl.h>
#include <unistd.h>

#include "301/CO_driver.h"
#include "CANopen.h"

/* Helper function - get monotonic clock time in ms */
static uint64_t CO_LinuxThreads_clock_gettime_ms(void)
{
  struct timespec ts;

  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Mainline thread (threadMain) ***************************************************/
static struct
{
  uint64_t  start;                  /* time value CO_process() was called last time in ms */
} threadMain;

void threadMain_init(void (*callback)(void*), void *object)
{
  threadMain.start = CO_LinuxThreads_clock_gettime_ms();

  CO_CANopenInitCallback(object, callback);
}

void threadMain_close(void)
{
  CO_CANopenInitCallback(NULL, NULL);
}

void threadMain_process(CO_NMT_reset_cmd_t *reset)
{
  uint32_t finished;
  uint16_t diff;
  uint64_t now;

  now = CO_LinuxThreads_clock_gettime_ms();
  diff = (uint16_t)(now - threadMain.start);

  /* we use timerNext_us in CO_process() as indication if processing is
   * finished. We ignore any calculated values for maximum delay times. */
  do {
    finished = 1;
    *reset = CO_process(CO, (uint32_t)diff*1000, &finished);
    diff = 0;
  } while ((*reset == CO_RESET_NOT) && (finished == 0));

  /* prepare next call */
  threadMain.start = now;
}

/* Realtime thread (threadRT) *****************************************************/
static struct {
  uint32_t us_interval;         /* configured interval in us */
  int interval_fd;              /* timer fd */
} threadRT;

void CANrx_threadTmr_init(uint16_t interval)
{
  struct itimerspec itval;

  threadRT.us_interval = interval * 1000;
  /* set up non-blocking interval timer */
  threadRT.interval_fd = timerfd_create(CLOCK_MONOTONIC, 0);
  (void)fcntl(threadRT.interval_fd, F_SETFL, O_NONBLOCK);
  itval.it_interval.tv_sec = 0;
  itval.it_interval.tv_nsec = interval * 1000000;
  itval.it_value = itval.it_interval;
  (void)timerfd_settime(threadRT.interval_fd, 0, &itval, NULL);
}

void CANrx_threadTmr_close(void)
{
  (void)close(threadRT.interval_fd);
  threadRT.interval_fd = -1;
}

void CANrx_threadTmr_process(void)
{
  int32_t result;
  int32_t i;
  bool_t syncWas;
  unsigned long long missed;

  result = CO_CANrxWait(CO->CANmodule[0], threadRT.interval_fd, NULL);
  if (result < 0) {
    result = read(threadRT.interval_fd, &missed, sizeof(missed));
    if (result > 0) {
      /* at least one timer interval occured */
      CO_LOCK_OD();

      if(CO->CANmodule[0]->CANnormal) {

        for (i = 0; i <= missed; i++) {
          /* Process Sync */
          syncWas = CO_process_SYNC(CO, threadRT.us_interval, NULL);

          /* Read inputs */
          CO_process_RPDO(CO, syncWas);

          /* Write outputs */
          CO_process_TPDO(CO, syncWas, threadRT.us_interval, NULL);
        }
      }

      CO_UNLOCK_OD();
    }
  }
}
