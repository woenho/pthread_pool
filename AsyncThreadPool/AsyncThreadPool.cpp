/*
* programming. woenho@daum.net
* on. 2021-12-20
* for. anybody
* using. free
* pay. comment to me
*/

#include "AsyncThreadPool.h"

using namespace std;

// 선언과 함께 초기화 { PTHREAD_MUTEX_INITIALIZER or pthread_mutex_init(&mutex,NULL); }
pthread_mutex_t	hMutex;			// 관리쓰레드와 워크쓰레드간의 시그널 동기화용 뮤텍스
pthread_mutex_t	mutexWork;		// 워크쓰레드들 사이에 동기화가 필요한 경우 사용하기 위한 뮤텍스
pthread_cond_t	hEvent;			// 쓰레드 깨움 이벤트시그널

// 상태 플러그
static int g_mainThread_run = 0; // 0으로 설정하면 관리 쓰레드 종료한다
static int g_workThread_run = 0; // 0으로 설정하면 work 쓰레드 종료한다
static int g_mainThread_use_exit_func; // 1로 설정하면 모든 워크 쓰레드에 atp_exit_func 를 호출하고 종료하게 한다
time_t g_endwaittime;	// 쓰레드풀 종료 시 작업하고있는 쓰레드를 기다릴 쵀대 시간을 지정한다

PTHREADINFO g_thread;		// 워크쓰레드관리 테이블 포인트
static int g_nThreadCount;	// 워크쓰레드 수
pthread_t g_MainThreadID;	// 관리쓰레드 아이덴티파이어

queue<PATP_DATA> g_queueRealtime;
queue<PATP_DATA> g_queueNormal;

PATP_DATA g_nextRealtime = NULL; // 워크쓰레드가 깨어나서 할 일의 데이타 (최우선순위)
PATP_DATA g_nextNormal = NULL; // 워크쓰레드가 깨어나서 할 일의 데이타 (g_nextRealtime 비었을 때 실행)

unsigned int g_mutexWorkCount;

time_t g_requestWorkDelay;	// 워크쓰레드에 작업의뢰를 한 후 실제 작업쓰레드가 작업을 시작까지 걸리는 시간 (자동계산)
struct timespec g_requestWorktime;	// 메인쓰레드가 작업의뢰시 설정하고 작업을 시작하는 워크 쓰레드가 이 시각과의 차이를 g_requestWorkDelay 에 반영한다(자동)

// ------------ function -----------------

// 실험결과 usleep은 정확한 마이크로시간 동안 슬립하지 않는다.
// 그래서 select를 이용함 함수를 만들었다. usleep과 별차이가 없넹... 흐미!
// select 가 나노초를 안쓴다 예전 마이크로초 ㅋㅋ
inline void SleepUSec(useconds_t a_nUSec)
{
	struct timeval stTimeVal;
	fd_set rset;

	stTimeVal.tv_sec = a_nUSec / 1e+6;
	stTimeVal.tv_usec = a_nUSec - (stTimeVal.tv_sec * 1e+6);
	FD_ZERO(&rset);

	int rc = select(1, &rset, 0, 0, &stTimeVal);
	if (rc > 0) {
		// 엥 입력 글자가 있다. 읽어서 모두 버려야 한다. 아니면 타임아웃 없다...
		char c;
		while (read(0, &c, 1));
	}
	else if (!rc) {
		// timeout
	} else {
		// error
	}

}


void* mainthread(void* param)
{
	int i;
	int nCheckExit;
	bool bRequested;
	struct timespec timenow;
	useconds_t waittime = 0;
	time_t seconds = 0;
	time_t nanoseconds = 0;

	g_requestWorkDelay = 0; // 최초 시작은 0으로
	g_mainThread_use_exit_func = 0;
	g_nextRealtime = NULL;
	g_nextNormal = NULL;
	g_mutexWorkCount = 0;

	while (g_mainThread_run) {
		// TRACE("mainthread..... run\n");
		waittime = 100000; // 1. 작업큐가 비었을 때, 2. 의뢰된 작업이 아직 처리시작 되지 못한 때 -> 작업큐 검사는 1초에 10회
		if (!g_queueRealtime.empty() || !g_queueNormal.empty()) { // if (큐가 있으면)
			bRequested = false;
			// 시그널 보내기 전에 반드시 락을 건다...
			if (!pthread_mutex_lock(&hMutex)) {

				if (g_queueRealtime.size()) {
					if (!g_nextRealtime) {
						g_nextRealtime = g_queueRealtime.front();
						g_queueRealtime.pop();
						clock_gettime(CLOCK_MONOTONIC, &g_requestWorktime); // 작업의뢰시각 기록, NTP영향없다
						pthread_cond_signal(&hEvent);
						bRequested = true;
						TRACE("mainthread. request a realtime job, avrage fetch delay:%.6f\n", g_requestWorkDelay / 1e+9);
					}
				} else {
					// 리얼타임 우선순위의 잡이 없다면
					if (!g_nextNormal && g_queueNormal.size()) {
						g_nextNormal = g_queueNormal.front();
						g_queueNormal.pop();
						clock_gettime(CLOCK_MONOTONIC, &g_requestWorktime); // 작업의뢰시각 기록, NTP영향없다
						pthread_cond_signal(&hEvent);
						bRequested = true;
						TRACE("mainthread. request a normal job, delay:%.6f\n", g_requestWorkDelay / 1e+9);
					}
				}
				
				if (bRequested) {
					// 처리했다면 워크쓰레드가 작업을 받아가는 평균 시간 만큼 기다리자
					// 처리하지 못했다면 노는 쓰레드가 없다. 좀 쉬었다가 다음 처리하자
					waittime = g_requestWorkDelay / 1e+3; // for micro secons
				} else if (g_nextRealtime|| g_nextNormal) {
					// 앞전에 시그널를 보냈는데 쉬는 쓰레드가 없었다면 이리 온다... 다시 시그널을 보내자
					pthread_cond_signal(&hEvent);
					clock_gettime(CLOCK_MONOTONIC, &timenow); // 작업의뢰지연시각구하기, NTP영향없다
					seconds = timenow.tv_sec - g_requestWorktime.tv_sec;
					nanoseconds = timenow.tv_nsec - g_requestWorktime.tv_nsec + (seconds * 1e+9);
					TRACE("mainthread. monitoring requested a job, elapsed:%.6f\n", nanoseconds / 1e+9); // 워크쓰레드가 작업을 받아가는 시간이 평균시간 보다 더 오래 걸리고 있는 상황
				}
				pthread_mutex_unlock(&hMutex);
			}
		}
		// 실험결과 usleep은 정확한 마이크로시간 동안 슬립하지 않는다
		// 큐에들어온 자료확인은 좀 천천히 하자 cpu 소모 많다
		// SleepUSec(waittime);
		usleep(waittime);
	}

	// 만일 아직 남은 잡이 있다면 모든 쓰레드가 깨어나 나머지 일을 해야한다
	pthread_cond_broadcast(&hEvent);

	nanoseconds = 0;

	if (g_mainThread_use_exit_func) {
		// 종료할 때 워크쓰레드 사용자함수를 호출 후 종료하라고 요청을 받았다면
		TRACE("=== set exit signal to all work threads\n");
		do {
			usleep(500000); // 0.5 second 
			nanoseconds += 5e+8;

			nCheckExit = 0;
			// 시그널 보내기 전에 반드시 락을 건다... 걍 하면 이미 쓰레드가 실행 중 일 수 있다
			if (!pthread_mutex_lock(&hMutex)) {
				if (!g_nextNormal && !g_nextRealtime) {
					for (i = 0; i < g_nThreadCount; i++) {
						if (g_thread[i].nThreadStat < stat_run) {
							g_thread[i].nThreadStat = stat_exit;
							TRACE("=== thread no(%d), set to stat_exit signal\n", g_thread[i].nThreadNo);
						}
						else if (g_thread[i].nThreadStat >= stat_exited) {
							// 종료한 작업 카운트
							nCheckExit++;
						}
					}
				}
				// 만일 아직 남은 잡이 있다면 모든 쓰레드가 깨어나 남은 잡을 가져가 실행 하고
				// 아니면 stat_exit 상태에 다른 작업을 모든 쓰레드가 수행해야 한다
				pthread_cond_broadcast(&hEvent);
				pthread_mutex_unlock(&hMutex);
			}
		} while (nCheckExit < g_nThreadCount && nanoseconds <= g_endwaittime);
		// 실행유지플래그(g_workThread_run)를 끈다
		g_workThread_run = 0;
	} else {
		// 각 워크쓰레드 종료함수 호출 필요없는 경우
		// 실행유지플래그(g_workThread_run)를 끄고 일제히 깨운다
		TRACE("=== all thread broadcast terminate signal !\n");
		g_workThread_run = 0;
		pthread_cond_broadcast(&hEvent);
	}

	char* szThreadStatus = (char*)malloc(g_nThreadCount);
	bzero(szThreadStatus, g_nThreadCount);

	// 모든 워크쓰레드가 정상종료 되었는지 검증한다, 최대 g_endwaittime 만큼 기다린다
	TRACE("=== check exited all work threads endwait seconds(%.6f)\n", g_endwaittime / 1e+9);

	do
	{
		usleep(100000);
		nCheckExit = 0;
		for (i = 0; i < g_nThreadCount; i++) {
			// 일하는 쓰레드 가 있으면 일이 종료될 때를 기다린다
			if (szThreadStatus[i] || g_thread[i].nThreadStat >= stat_exit) {
				if (!szThreadStatus[i]) {
					szThreadStatus[i] = '9';
				}
				nCheckExit++;
			}
			else {
				clock_gettime(CLOCK_MONOTONIC, &timenow);
				seconds = timenow.tv_sec - g_thread[i].beginWorktime.tv_sec;
				nanoseconds = timenow.tv_nsec - g_thread[i].beginWorktime.tv_nsec + (seconds * 1e+9);
				if (nanoseconds >= g_endwaittime) {
					if (!szThreadStatus[i]) {
						TRACE("=== thread no(%d), need force exit check %.6f / %.6f\n", g_thread[i].nThreadNo, nanoseconds / 1e+9, g_endwaittime / 1e+9);
						szThreadStatus[i] = '9';
						nCheckExit++;
					}
				} else {
					TRACE("=== thread no(%d), elapsed time %.6f seconds, waittime %.6f\n", g_thread[i].nThreadNo, nanoseconds / 1e+9, g_endwaittime / 1e+9);
				}
			}
		}
	} while (nCheckExit < g_nThreadCount);
	
	free(szThreadStatus);

	TRACE("=== force kill. if still running thread\n");
	// 기다려도 종료하지 않은 쓰레드는 강제 종료 시킨다
	for (i = 0; i < g_nThreadCount; i++) {
		if (g_thread[i].nThreadStat == stat_exited) {
			TRACE("=== thread no(%d), exited check ok!\n", g_thread[i].nThreadNo);
		} else {
			clock_gettime(CLOCK_MONOTONIC, &timenow);
			seconds = timenow.tv_sec - g_thread[i].beginWorktime.tv_sec;
			nanoseconds = timenow.tv_nsec - g_thread[i].beginWorktime.tv_nsec + (seconds * 1e+9);
			TRACE("=== thread no(%d), still running. force kill. run time(%.6f second)\n", g_thread[i].nThreadNo, nanoseconds / 1e+9);
			pthread_cancel(g_thread[i].threadID);
		}
	}

	// workthread memory free
	for (i = 0; i < g_nThreadCount; i++) {
		if (g_thread[i].atp_exit_data)
			free(g_thread[i].atp_exit_data);
		if (g_thread[i].atp_idle_data)
			free(g_thread[i].atp_idle_data);			
	}
	// table memory free
	free(g_thread);
	g_thread = NULL;

	// thread count reset
	g_nThreadCount = 0;

	// 관리쓰레드 명시적 종료
	pthread_exit(0);

}

void* workthread(void* param)
{
	PTHREADINFO me = (PTHREADINFO)param;
	int nStat;
	struct timespec waittime;
	struct timespec timenow;
	time_t seconds = 0;
	time_t nanoseconds = 0;

	while (g_workThread_run) {


		nStat = 0;

		// pthread_cond_wait() 은 락걸린 뮤텍스를 언락하고 웨이팅이 들어간다
		// 시그날이 들어오면 자기가 깨어날 때 뮤텍스를 락걸고 나온다
		// 깨어나면 락을 자기가 가지고 있으므로 락을 가지고 할일만 하고 락을 풀어 주어야 해야한다
		if (me->nThreadStat < stat_exit) {
			pthread_mutex_lock(&hMutex);
			clock_gettime(CLOCK_REALTIME, &timenow);	// NTP영향받아야 함
			waittime.tv_sec = timenow.tv_sec + me->waittime.tv_sec;
			waittime.tv_nsec = timenow.tv_nsec + me->waittime.tv_nsec;
			nStat = pthread_cond_timedwait(&hEvent, &hMutex, &waittime);
		}


		if (nStat==-1) {
			pthread_mutex_unlock(&hMutex);
			// wait 실패시 -1 설정, errno 로 오류 확인 필요
			// EBUSY : 16	/* Device or resource busy */
			if (errno == EBUSY)
			{
				TRACE("workthread no(%d), timedwait() EBUSY, errno=%d\n", me->nThreadNo, errno);
			} else if (errno == EINVAL) {
				TRACE("workthread no(%d), timedwait() EINVAL, errno=%d\n", me->nThreadNo, errno);
			} else {
				TRACE("workthread no(%d), timedwait() error, errno=%d\n", me->nThreadNo, errno);
				usleep(10000);// 10,000 마이크로초 => 10밀리초 => // 마이크로초 (백만분의1초 10의 -6승)
			}

			me->nThreadStat = stat_suspend;
			continue;
		}

#if 0
		TRACE("workthread no(%d), getup or wakeup... status(%d, %d)\n", me->nThreadNo, me->nThreadStat, errno);
#endif

		if (me->nThreadStat == stat_exit) {
			pthread_mutex_unlock(&hMutex);

			// 명시적으로 죽으라는 메시지를 받았다.
			TRACE("workthread no(%d), my signal is stat_exit\n", me->nThreadNo);

			if (me->atp_exit_func)
				me->nExitCode = me->atp_exit_func(me->atp_exit_data);
			else
				me->nExitCode = 0;

			me->nThreadStat = stat_exited;
			
			TRACE("workthread no(%d), I'm going to exiting\n", me->nThreadNo);
			
			break; /// exit from work loop

		}


		if (g_nextRealtime) {
			me->atp_realtime_data = g_nextRealtime;
			g_nextRealtime = NULL;		// 비워주어야 관리쓰레드가 다음 작업을 의뢰한다
			me->nThreadStat = stat_run;
			clock_gettime(CLOCK_MONOTONIC, &me->beginWorktime); // NTP영향 받으면 안됨
			seconds = me->beginWorktime.tv_sec - g_requestWorktime.tv_sec;
			nanoseconds = me->beginWorktime.tv_nsec - g_requestWorktime.tv_nsec + (seconds * 1e+9);
#if 0
			g_requestWorkDelay = nanooseconds; // 현재의 지연시간이 향후 지연시간이 될 가능성도 많다, 그러나 전체를 보면 평균이 나을것이다
#else
			g_requestWorkDelay = (nanoseconds + g_requestWorkDelay) / 2; // 기존 지연시간과의 평균값으로 자동 조정한다
#endif
			pthread_mutex_unlock(&hMutex); // 데이타포인트 작업 완료 후에 뮤텍스락을 푼다

			// 실행명령 전달받음
			TRACE("workthread no(%d), I got a realtime job. fetch delay:%.6f, excuted: %lu\n", me->nThreadNo, nanoseconds / 1e+9, me->nRealtimeCount);

			ATP_STAT next = stat_suspend;

			if (me->atp_realtime_data) {
				me->atp_realtime_data->priority = atp_realtime;
				me->atp_realtime_data->threadNo = me->nThreadNo;
			}
			if (me->atp_realtime_func)
				next = me->atp_realtime_func(me->atp_realtime_data);
			// 작업이 주어질때 마다 새로 할당 되므로 반드시 지워 준다
			free(me->atp_realtime_data);
			me->atp_realtime_data = NULL;

			me->nRealtimeCount++;
			clock_gettime(CLOCK_MONOTONIC, &me->endWorktime); // NTP영향 받으면 안됨
			seconds = me->endWorktime.tv_sec - me->beginWorktime.tv_sec;
			nanoseconds = me->endWorktime.tv_nsec - me->beginWorktime.tv_nsec + (seconds * 1e+9);
			me->sumRealtimeWorkingtime += nanoseconds / 1e+6; // milli seconds
			if (me->mostLongtimeRealtime < nanoseconds)
				me->mostLongtimeRealtime = nanoseconds;

			TRACE("workthread no(%d), I finished a realtime job. elapsed:%.6f excuted: %lu, next: %d\n", me->nThreadNo, nanoseconds / 1e+9, me->nRealtimeCount, next);

			me->nThreadStat = next;

		} else if (g_nextNormal) {
			me->atp_normal_data = g_nextNormal;
			g_nextNormal = NULL;		// 비워주어야 관리쓰레드가 다음 작업을 의뢰한다
			me->nThreadStat = stat_run;
			clock_gettime(CLOCK_MONOTONIC, &me->beginWorktime); // NTP영향 받으면 안됨
			seconds = me->beginWorktime.tv_sec - g_requestWorktime.tv_sec;
			nanoseconds = me->beginWorktime.tv_nsec - g_requestWorktime.tv_nsec + (seconds * 1e+9);
#if 0
			g_requestWorkDelay = nanoseconds; // 현재의 지연시간이 향후 지연시간이 될 가능성도 많다, 그러나 전체를 보면 평균이 나을것이다
#else
			g_requestWorkDelay = (nanoseconds + g_requestWorkDelay) / 2; // 기존 지연시간과의 평균값으로 자동 조정한다
#endif
			pthread_mutex_unlock(&hMutex); // 데이타포인트 작업 완료 후에 뮤텍스락을 푼다

			// 실행명령 전달받음
			TRACE("workthread no(%d), I got a normal job. fetch delay:%.6f, (real queue size = %lu, normal = %lu)\n"
				, me->nThreadNo, nanoseconds / 1e+9, g_queueRealtime.size(), g_queueNormal.size());

			ATP_STAT next = stat_suspend;
			if (me->atp_normal_data) {
				me->atp_normal_data->priority = atp_normal;
				me->atp_normal_data->threadNo = me->nThreadNo;
			}
			if (me->atp_normal_func)
				next = me->atp_normal_func(me->atp_normal_data);
			// 작업이 주어질때 마다 새로 할당 되므로 반드시 지워 준다
			free(me->atp_normal_data);
			me->atp_normal_data = NULL;

			me->nNormalCount++;
			clock_gettime(CLOCK_MONOTONIC, &me->endWorktime); // NTP영향 받으면 안됨
			seconds = me->endWorktime.tv_sec - me->beginWorktime.tv_sec;
			nanoseconds = me->endWorktime.tv_nsec - me->beginWorktime.tv_nsec + (seconds * 1e+9);
			me->sumNormalWorkingtime += nanoseconds / 1e+6; // milli seconds
			if (me->mostLongtimeNormal < nanoseconds)
				me->mostLongtimeNormal = nanoseconds;

			TRACE("workthread no(%d), I finished a normal job. elapsed:%.6f excuted: %lu, next: %d\n", me->nThreadNo, nanoseconds / 1e+9, me->nNormalCount, next);

			me->nThreadStat = next;
		} else {
			me->nThreadStat = stat_run;
			pthread_mutex_unlock(&hMutex);

			me->nIdleCount++;

			TRACE("workthread no(%d), I check idle process\n", me->nThreadNo);

			ATP_STAT next = stat_suspend;
			if (me->atp_idle_func) {
				TRACE("workthread no(%d), I start idle job\n", me->nThreadNo);
				next = me->atp_idle_func(me->atp_idle_data);
				TRACE("workthread no(%d), I finished a idle job. next =%d\n", me->nThreadNo, next);
			}
			me->nThreadStat = next;

		}
	}

	// 워크쓰레드 명시적 종료
	me->nThreadStat = stat_exited;
	TRACE("workthread no(%d), terminated\n", me->nThreadNo);
	pthread_exit(0);
}

int atp_create(int nThreadCount, ThreadFunction realtime, ThreadFunction normal, pthread_attr_t* stAttr)
{
	g_nThreadCount = nThreadCount;
	pthread_mutex_init(&mutexWork, NULL);
	pthread_mutex_init(&hMutex, NULL);
	pthread_cond_init(&hEvent, NULL);

	g_thread = (PTHREADINFO)malloc(sizeof(THREADINFO) * nThreadCount);
	bzero(g_thread, sizeof(THREADINFO) * nThreadCount);
	
	g_mainThread_run = 1;	// 쓰레드 생성전에 설정해야한다. 값이 0이면 쓰레드 만들자 마자 종료한다
	g_workThread_run = 1;	// 쓰레드 생성전에 설정해야한다. 값이 0이면 쓰레드 만들자 마자 종료한다

	// 워크쓰레드 생성
	for (int i = 0; i < g_nThreadCount; i++) {
		g_thread[i].nThreadNo = i;
		g_thread[i].nThreadStat = stat_suspend;
		g_thread[i].atp_realtime_func = realtime;
		g_thread[i].atp_normal_func = normal ? normal : realtime;	// 같은 함수를 호출할 가능성이 많다
		g_thread[i].waittime.tv_sec = 3; // default 3 second
		g_thread[i].waittime.tv_nsec = 0; // default 0 nano second
		g_thread[i].sd = -1; // invalid socket no is (-1)
		pthread_create(&g_thread[i].threadID, stAttr, workthread, &g_thread[i]);
		pthread_detach(g_thread[i].threadID);	// pthread_exit(0); 시 리소스 자동 해제
	}

	// 관리 쓰레드 생성
	pthread_create(&g_MainThreadID, NULL, mainthread, NULL); // 관리 쓰레드는 pthread_join() 으로 동기화 해서 종료할 것임

	TRACE("=== order: thread pool create, work thread count=%d\n", g_nThreadCount);

	return 0;
}

int atp_destroy(ATP_END endcode, bool use_exit_func, time_t endwaittime)
{
	TRACE("=== %s() order: thread pool stop, atp_end code=%d\n", __func__, endcode);

	g_endwaittime = endwaittime;

	if (endcode == gracefully) {
		TRACE("=== %s() gracefully thread pool down check\n", __func__);
		while (g_nextRealtime || g_nextNormal || !g_queueRealtime.empty() || !g_queueNormal.empty()) {
			TRACE("=== %s() queue check , queue size=%d,%d, request_job=%s,%s\n", __func__
				, (int)g_queueRealtime.size(), (int)g_queueNormal.size()
				, g_nextRealtime ? "true" : "none", g_nextNormal ? "true" : "none"
			);
			usleep(100000);
		}
	} else {
		if (g_queueRealtime.size() || g_queueNormal.size()) {
			TRACE("=== %s() queue size=%d,%d, but force down now\n", __func__, (int)g_queueRealtime.size(), (int)g_queueNormal.size());
		}
	}

	TRACE("=== %s() clear memory\n", __func__);
	// 작업의뢰 큐의 나머지 청소, 청소하지 않으면 다시 쓰레드 생성할 때 이전에 의뢰한 작업이 먼저 실행된다.
	while (g_queueRealtime.size()) { free(g_queueRealtime.front()); g_queueRealtime.pop(); }
	while (g_queueNormal.size()) { free(g_queueNormal.front()); g_queueNormal.pop(); }
	
	pthread_mutex_lock(&hMutex); // 데이타포인트 작업을 하려면 뮤텍스락을 걸어야 안전하다
	if (g_nextRealtime) {
		free(g_nextRealtime);
		g_nextRealtime = 0;
	}
	if (g_nextNormal) {
		free(g_nextNormal);
		g_nextNormal = 0;
	}
	pthread_mutex_unlock(&hMutex); // 데이타포인트 작업 완료 후에 뮤텍스락을 푼다

	// endcode == gracefully 인 경우는 모든 큐 비우고 이 루틴 탄다
	g_mainThread_use_exit_func = use_exit_func;
	g_mainThread_run = 0;

	// 모든 쓰레드 종료 동기화가 필요하면???
	pthread_join(g_MainThreadID, NULL); // 쓰레드 종료를 기다리고 종료되면 리소스를 해제한다

	pthread_cond_destroy(&hEvent);
	pthread_mutex_destroy(&hMutex);
	pthread_mutex_destroy(&mutexWork);
	
	TRACE("=== %s() terminated now\n", __func__);

	return 0;
}

int atp_addQueue(PATP_DATA atp, ATP_PRIORITY priority)
{
	if (!g_mainThread_run)
		return -1;

	// add queue
	// default 가 atp_realtime 이므로 명시적 normal이 어느곳에서든 있으면 atp_normal로 처리
	if(priority == atp_normal || atp->priority == atp_normal)
		g_queueNormal.push(atp);
	else
		g_queueRealtime.push(atp);

	return 0;
}

PTHREADINFO atp_getThreadInfo()
{
	return g_thread;
}

int atp_getThreadCount()
{
	return g_nThreadCount;
}

int atp_getRealtimeQueueCount()
{
	return g_queueRealtime.size();
}

int atp_getNormalQueueCount()
{
	return g_queueNormal.size();
}

bool atp_setwaittime(struct timespec _w, int _n)
{
	if (_n < -1 || _n >= g_nThreadCount)
		return false;

	int nStart = _n;
	int nEnd = _n + 1;

	if (_n == -1) {
		nStart = 0;
		nEnd = g_nThreadCount;
	}

	while (nStart < nEnd) {
		g_thread[nStart].waittime.tv_sec = _w.tv_sec;
		g_thread[nStart].waittime.tv_nsec = _w.tv_nsec;
		nStart++;
	}
	return true;
}

bool atp_setfunc(ATP_STAT _s, ThreadFunction _f, PATP_DATA _d, int _n)
{
	if (_n < -1 || _n >= g_nThreadCount)
		return false;

	int nStart = _n;
	int nEnd = _n + 1;

	if (_n == -1) {
		nStart = 0;
		nEnd = g_nThreadCount;
	}
	
	while (nStart < nEnd) {
		switch (_s) {
		case stat_suspend:
			if (g_thread[nStart].atp_idle_data)
				free(g_thread[nStart].atp_idle_data);
			g_thread[nStart].atp_idle_func = _f;
			g_thread[nStart].atp_idle_data = _d;
			if(_d)
				_d->threadNo = g_thread[nStart].nThreadNo;
			break;
		case stat_exit:
			if (g_thread[nStart].atp_exit_data)
				free(g_thread[nStart].atp_exit_data);
			g_thread[nStart].atp_exit_func = _f;
			g_thread[nStart].atp_exit_data = _d;
			if (_d)
				_d->threadNo = g_thread[nStart].nThreadNo;
			break;
		default:
			return false;
		}
		nStart++;
	}
	return true;
}

int atp_worklock()
{
	g_mutexWorkCount++;
	return pthread_mutex_lock(&mutexWork);
}

int atp_workunlock()
{
	g_mutexWorkCount--;
	return pthread_mutex_unlock(&mutexWork);
}

unsigned int atp_getWorkLockCount()
{
	return g_mutexWorkCount;
}


