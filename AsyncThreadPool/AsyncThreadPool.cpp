
#include "AsyncThreadPool.h"

using namespace std;

// 선언과 함께 초기화 { PTHREAD_MUTEX_INITIALIZER or pthread_mutex_init(&mutex,NULL); }
pthread_mutex_t mutexThread;	// 잡업큐 관리용 뮤텍스 
pthread_mutex_t	hMutex;			// 관리쓰레드와 워크쓰레드간의 시그널 동기화용 뮤텍스
pthread_mutex_t	mutexWork;		// 워크쓰레드들 사이에 동기화가 필요한 경우 사용하기 위한 뮤텍스
pthread_cond_t	hEvent;			// 쓰레드 깨움 이벤트시그널

// 상태 플러그
static int g_mainThread_run = 0; // 0으로 설정하면 관리 쓰레드 종료한다
static int g_mainThread_use_exit_func = 0; // 1로 설정하면 모든 워크 쓰레드에 atp_exit_func 를 호출하고 종료하게 한다
static int g_workThread_run = 0; // 0으로 설정하면 work 쓰레드 종료한다

// 워크 쓰레드관리 테이블
PTHREADINFO g_thread = NULL;
static int g_nThreadCount = 0;
pthread_t g_MainThreadID;

queue<PATP_DATA> g_work;

// ------------ function -----------------

void* mainthread(void* param)
{
	int i;
	bool bProcessed;
	while (g_mainThread_run) {
		if (g_work.size()) { // if (큐가 있으면)
			bProcessed = false;
			// 작업중일 때는 atp_addQueue() 추가 안한다
			if (!pthread_mutex_lock(&mutexThread)) {
				// 시그널 보내기 전에 반드시 락을 건다... 걍 하면 이미 쓰레드가 실행 중 일 수 있다
				if (!pthread_mutex_lock(&hMutex)) {
					for (i = 0; i < g_nThreadCount; i++) {
						// 잠자는 쓰레드가 있으면
						if (g_thread[i].nThreadStat == stat_suspend) {
							// 쓰레드 상태를 stat_run으로 변경하고 쓰레드를 깨우고 break
							// 없으면 다음 타임으로 자동 패스...
							g_thread[i].nThreadStat = stat_run;
							g_thread[i].atp_run_data = g_work.front();
							g_work.pop();
							pthread_cond_signal(&hEvent);
							bProcessed = true;
							TRACE("mainthread. run with workethread no(%d)\n", i);
							break;
						}
					}
					pthread_mutex_unlock(&hMutex);
				}
				// 뮤텍스 언락
				pthread_mutex_unlock(&mutexThread);
				// 처리했다면 (그러나 아직 큐에 자료가 남았을 수 있다) 바로 다음 작업 시키자
				// 처리하지 못했다면 노는 쓰레드가 없다. 좀 쉬었다가 다음 처리하자
				if (bProcessed)
					continue;
			}
		}
		// 큐에들어온 자료확인은 좀 천천히 하자 cpu 많이 먹네...
		usleep(100000);// 100,000 마이크로초 => 100밀리초 => // 마이크로초 (백만분의1초 10의 -6승)
	}

	int nCheck;
	int nTryCount;
	if (g_mainThread_use_exit_func) {
		// 종료할 때 워크쓰레드 사용자함수를 호출하고 종료하라고 요청을 받았다면
		TRACE("=== set exit signal to all workthread\n");
		nTryCount = 0;
		do {
			// 모든 뭐크쓰레드의 상태를 stat_exit 로 설정하는것을 10회 시도한다. 최대 5초간 시도
			if (++nTryCount > 10) {
				break;
			}
			else {
				usleep(500000);
			}
			// TRACE("Try to set thread exit! Try Count(%d)\n", nTryCount);


			nCheck = 0;
			// 시그널 보내기 전에 반드시 락을 건다... 걍 하면 이미 쓰레드가 실행 중 일 수 있다
			if (!pthread_mutex_lock(&hMutex)) {
				for (i = 0; i < g_nThreadCount; i++) {
					if (g_thread[i].nThreadStat < stat_run) {
						g_thread[i].nThreadStat = stat_exit;
						TRACE("=== thread no(%d), set to stat_exit signal\n", g_thread[i].nThreadNo);
						pthread_cond_signal(&hEvent);
					} else if (g_thread[i].nThreadStat >= stat_exit){
						nCheck++;
					}
				}
				pthread_mutex_unlock(&hMutex);
			}
		} while (nCheck < g_nThreadCount);
		// 실행유지플래그(g_workThread_run)를 끈다
		g_workThread_run = 0;
	}
	else {
		// 각 워크쓰레드 종료함수 호출 필요없는 경우
		// 실행유지플래그(g_workThread_run)를 끄고 일제히 깨운다
		TRACE("=== all thread broadcast exit !\n");
		g_workThread_run = 0;
		pthread_cond_broadcast(&hEvent);
	}

	int nExitCount = 0;
	char* szThreadStatus = (char*)malloc(g_nThreadCount);
	bzero(szThreadStatus, g_nThreadCount);

	nTryCount = 0;

	// 모든 워크쓰레드가 정상종료 되었는지 최대 10회 검증한다, 최대 5초 소요 후 강제 종료
	TRACE("=== check exited all workthread\n");
	do
	{
		if (++nTryCount > 10) {
			break;
		} else {
			usleep(500000);
		}
		// TRACE("Check to thread end! Try Count(%d)\n", nTryCount);

		nExitCount = 0;
		for (i = 0; i < g_nThreadCount; i++) {
			// 일하는 쓰레드 가 있으면 일이 종료될 때를 기다린다
			if (g_thread[i].nThreadStat == stat_exited) {
				if (!szThreadStatus[i]) {
					TRACE("=== thread no(%d), exited ok!\n", g_thread[i].nThreadNo);
					szThreadStatus[i] = '9';
				}
				nExitCount++;
			}
		}
	} while (nExitCount < g_nThreadCount);

	free(szThreadStatus);

	// 기다려도 종료하지 않는 쓰레드는 강제 종료 시킨다
	if (nExitCount < g_nThreadCount) {
		for (i = 0; i < g_nThreadCount; i++) {
			if (g_thread[i].nThreadStat != stat_exited) {
				TRACE("=== thread no(%d), still running. force kill...\n", g_thread[i].nThreadNo);
				pthread_cancel(g_thread[i].threadID);
			}
		}
		
	}

	if (g_thread) {
		// workthread memory free
		for (i = 0; i < g_nThreadCount; i++) {
			if (g_thread[i].atp_exit_data) free(g_thread[i].atp_exit_data);
			if (g_thread[i].atp_suspend_data) free(g_thread[i].atp_suspend_data);
		}
		// table memory free
		free(g_thread);
	}
	g_thread = NULL;

	// thread pool table space free
	g_nThreadCount = 0;

	// 관리쓰레드 명시적 종료
	pthread_exit(0);

}

void* workthread(void* param)
{
	PTHREADINFO me = (PTHREADINFO)param;
	int nStat;
	struct timespec waittime;
	struct timeval timenow;

	while (g_workThread_run) {

		pthread_mutex_lock(&hMutex);

		nStat = 0;

		// pthread_cond_wait() 은 락걸린 뮤텍스를 언락하고 웨이팅이 들어간다
		// 시그날이 들어오면 자기가 깨어날 때 뮤텍스를 락걸고 나온다
		// 깨어나면 락을 자기가 가지고 있으므로 먼저 락을 풀어주고 작업을 해야한다
		// 그래아지 다른 쓰레드가 시그널에서 깨어날 때 기다리지 않고 바로 락을 가지고 나온다.
		if (me->nThreadStat < stat_exit) {
			gettimeofday(&timenow, NULL);
			waittime.tv_sec = timenow.tv_sec + me->waittime.tv_sec;
			waittime.tv_nsec = timenow.tv_usec + me->waittime.tv_nsec;

			nStat = pthread_cond_timedwait(&hEvent, &hMutex, &waittime);
		}


		if (nStat==-1) {
			pthread_mutex_unlock(&hMutex);
			// wait 실패시 -1 설정, errno 로 오류 확인 필요
			// EBUSY : 16	/* Device or resource busy */
			if (errno == EBUSY)
			{
				TRACE("workthread no(%d), was timedwait() timeout, errno=%d\n", me->nThreadNo, errno);
			} else if (errno == EINVAL) {
				TRACE("workthread no(%d), was timedwait() error, errno=%d\n", me->nThreadNo, errno);
			} else {
				TRACE("workthread no(%d), was timedwait() error, errno=%d\n", me->nThreadNo, errno);
				usleep(10000);// 10,000 마이크로초 => 10밀리초 => // 마이크로초 (백만분의1초 10의 -6승)
			}

			me->nThreadStat = stat_suspend;
			continue;
		}

#if 0
		TRACE("workthread no(%d), getup or wakeup... status(%d)\n", me->nThreadNo, me->nThreadStat);
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
		
		if (me->nThreadStat == stat_run) {
			pthread_mutex_unlock(&hMutex);

			// 실행명령 전달받음
			me->nExecuteCount++;
			TRACE("workthread no(%d), I got a job. excuted: %lu\n", me->nThreadNo, me->nExecuteCount);

			ATP_STAT next = stat_suspend;

			if (me->atp_run_func)
				next = me->atp_run_func(me->atp_run_data);
			// me->atp_run_data 는 작업이 주어질때 마다 새로 할당 되므로 반드시 지워 준다
			if (me->atp_run_data)
				free(me->atp_run_data);
			me->atp_run_data = NULL;

			TRACE("workthread no(%d), I finished a job. excuted: %lu, next: %d\n", me->nThreadNo, me->nExecuteCount, next);

			me->nThreadStat = next;

			continue;

		} 
		
		if (me->nThreadStat == stat_suspend) {
			pthread_mutex_unlock(&hMutex);

			//TRACE("workthread no(%d), stat=%d\n", me->nThreadNo, me->nThreadStat);
			ATP_STAT next = stat_suspend;
			if (me->atp_suspend_func)
				next = me->atp_suspend_func(me->atp_suspend_data);
			//TRACE("workthread no(%d), I finished a stat_suspend. next =%d\n", me->nThreadNo, next);

			me->nThreadStat = next;

		} else {
			pthread_mutex_unlock(&hMutex);

			// 지정되지 않은 상태코드 발생.....
			// 사용자 함수에서 리턴값 제대로 설정하지 않으면 이리올 수 있다.
			printf("%s:%d 상태코드 확인바람 오류발생여지 있음...\n", __FILE__, __LINE__);
			me->nThreadStat = stat_suspend;
		}
	}

	// 워크쓰레드 명시적 종료
	me->nThreadStat = stat_exited;
	TRACE("workthread no(%d), terminated\n", me->nThreadNo);
	pthread_exit(0);
}

int atp_create(int nThreadCount, ThreadFunction _func, pthread_attr_t* stAttr)
{
	g_nThreadCount = nThreadCount;
	pthread_mutex_init(&mutexThread, NULL);
	pthread_mutex_init(&mutexWork, NULL);
	pthread_mutex_init(&hMutex, NULL);
	pthread_cond_init(&hEvent, NULL);

	g_thread = (PTHREADINFO)malloc(sizeof(THREADINFO) * nThreadCount);
	bzero(g_thread, sizeof(THREADINFO) * nThreadCount);

	// 쓰레드 생성전에 설정해야한다. 값이 0이면 쓰레드 만들자 마자 종료한다
	g_mainThread_run = 1;
	g_workThread_run = 1;
	g_mainThread_use_exit_func = 0;

	// 워크쓰레드 생성
	for (int i = 0; i < g_nThreadCount; i++) {
		g_thread[i].nThreadNo = i;
		g_thread[i].nThreadStat = stat_suspend;
		g_thread[i].atp_run_func = _func;
		g_thread[i].waittime.tv_sec = 3; // default 3 second
		g_thread[i].waittime.tv_nsec = 0; // default 0 nano second
		pthread_create(&g_thread[i].threadID, stAttr, workthread, &g_thread[i]);
		pthread_detach(g_thread[i].threadID);	// pthread_exit(0); 시 리소스 자동 해제
	}

	// 관리 쓰레드 생성
	pthread_create(&g_MainThreadID, NULL, mainthread, NULL); // 관리 쓰레드는 pthread_join() 으로 동기화 해서 종료할 것임

	TRACE("=== order: thread pool create, work thread count=%d\n", g_nThreadCount);

	return 0;
}

int atp_destroy(ATP_END endcode, bool use_exit_func)
{
	TRACE("=== %s() order: thread pool stop, atp_end code=%d\n", __func__, endcode);

	if (endcode == gracefully) {
		TRACE("=== %s() gracefully thread pool down check\n", __func__);
		while (g_work.size()) {
			TRACE("=== %s() queue check , queue size=%d\n", __func__, (int)g_work.size());
			usleep(100000);
		}
	} else {
		if (g_work.size()) {
			TRACE("=== %s() queue size=%d, but force down now\n", __func__, (int)g_work.size());
		}
	}

	// endcode == gracefully 인 경우는 모든 큐 비우고 이 루틴 탄다
	g_mainThread_use_exit_func = use_exit_func;
	g_mainThread_run = 0;

	// 모든 쓰레드 종료 동기화가 필요하면???
	pthread_join(g_MainThreadID, NULL); // 쓰레드 종료를 기다리고 종료되면 리소스를 해제한다

	pthread_cond_destroy(&hEvent);
	pthread_mutex_destroy(&hMutex);
	pthread_mutex_destroy(&mutexWork);
	pthread_mutex_destroy(&mutexThread);

	TRACE("=== %s() terminated now\n", __func__);

	return 0;
}

int atp_addQueue(PATP_DATA atp)
{
	if (!g_mainThread_run)
		return -1;

	// lock
	if (pthread_mutex_lock(&mutexThread))
		return -1;

	// add queue
	g_work.push(atp);

	// unlock
	pthread_mutex_unlock(&mutexThread);

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

int atp_getQueueCount()
{
	return g_work.size();
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
			if (g_thread[nStart].atp_suspend_data)
				free(g_thread[nStart].atp_suspend_data);
			g_thread[nStart].atp_suspend_func = _f;
			g_thread[nStart].atp_suspend_data = _d;
			break;
		case stat_exit:
			if (g_thread[nStart].atp_exit_data)
				free(g_thread[nStart].atp_exit_data);
			g_thread[nStart].atp_exit_func = _f;
			g_thread[nStart].atp_exit_data = _d;
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
	return pthread_mutex_lock(&mutexWork);
}

int atp_workunlock()
{
	return pthread_mutex_unlock(&mutexWork);
}



