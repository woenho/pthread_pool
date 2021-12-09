#if !defined(__AYNC_THREAD_POOL__)
#define __AYNC_THREAD_POOL__

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <locale.h>
#include <mcheck.h>
#include <limits.h>
#include <unistd.h>
#include <inttypes.h>

#include <pthread.h>

#include <sys/time.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/sockios.h>

#include <iostream>
#include <list>
#include <map>
#include <queue>
#include <fstream>

// 쓰레드풀의 종료 옵션
typedef enum {force=0,gracefully} ATP_END;
// 워크 쓰레드의 현재 상태
typedef enum { stat_suspend=0, stat_run, stat_exit = 9, stat_exited } ATP_STAT;
// 워크 쓰레드의 작업우선순위
typedef enum { atp_realtime=0, atp_normal } ATP_PRIORITY;

struct ATP_DATA_T; // ThreadFunction 선언시 사용할 수 있는 포인터만 선언..

// 워크쓰레드가 호출할 사용자함수 형식
typedef ATP_STAT(*ThreadFunction)(ATP_DATA_T*);

// 워크쓰레드가 사용자함수를 호출할 때 인자로 넘겨주는 데이타형식, 사용자 함수에서 몇 번째 워크쓰레드에서 동작하는지 알 수 있게 쓰레드 번호 추가 
typedef struct ATP_DATA_T { ThreadFunction func; ATP_PRIORITY priority; int threadNo, s_len; char s[]; } ATP_DATA, * PATP_DATA;

// ATP_DATA 를 사용할 때 이 함수로 메모리를 생성한다. 삭제는 자료의성격에 따라서 자동으로 진행된다
// stat_run으로 호출되는 함수가 상황에 따라 다른 함수를 호출 하고 싶으면 ATP_DATA::func 에 함수를 설정한다
inline PATP_DATA atp_alloc(size_t data_size) {
	size_t allocsize = data_size + sizeof(ATP_DATA) - sizeof(char); // sizeof(char) == ATP_DATA::s
	PATP_DATA atp = (PATP_DATA)malloc(allocsize);
	if (atp) { bzero(atp, allocsize); atp->s_len = data_size; }
	return atp;
};

// 워크쓰레드 정보 구조체
typedef struct _THREADINFO
{
	// thread base info
	int				nThreadNo;		// 쓰레드 고유일련번호
	pthread_t		threadID;		// 쓰레드 아이디
	pthread_attr_t	stAttr;			// 쓰레드 속성

	ATP_STAT		nThreadStat;	// 쓰레드의 현재 상태 (워크쓰레드가 생성된 후 최초 상태는 stat_startup이다)
	size_t			nExecuteCount;	// 쓰레드가 stat_run 모드로 실행한 건수
	int				nExitCode;		// 쓰레드 종료시 종료코드
	struct timespec waittime;		// 각 쓰레드 마다 스스로 깨어날 시간을 지정한다 (default 3초)

	// thread function
	ThreadFunction	atp_realtime_func;	// 실시간 수행 잡을 처리할 함수
	ThreadFunction	atp_normal_func;	// 낮은 순위의 잡을 처라할 함수(대부분 atp_realtime_func 와 같은 함수일 가능성이 크다)
	ThreadFunction	atp_idle_func;		// 쉬고 있을 때 수행 할일
	PATP_DATA		atp_idle_data;		// 쉬고있을 대 수행잡이 참고할 데이타 구조
	ThreadFunction	atp_exit_func;		// 메인쓰레드가 설정한 상태가 stat_exit 인 경우 실행할 함수. atp_setfunc() 로 설정
	PATP_DATA		atp_exit_data;		// 메인쓰레드가 설정한 상태가 상태가 stat_exit 인 경우 실행할 정보( 쓰레드 종료할 때 free()한다. atp_setfunc() 로 설정)

	// 외부연결이 필요한 경우 (예약)
	bool			keepsession;	// tcp 경우 세션유지가 필요한가?
	int				protocol;		// tcp or udp
	char			host[64];
	unsigned short	port;

	// log info (예약)
	time_t			logtime;
	struct tm		logtm;
	char			szThreadLog[1024];

} THREADINFO, *PTHREADINFO;

// AsyncThreadPool 을 이용하는 기본적인 함수들
int atp_create(int nThreadCount, ThreadFunction realtime, ThreadFunction normal=NULL, pthread_attr_t* stAttr=NULL);
int atp_destroy(ATP_END endcode, bool use_exit_func=false);
	
// atp_addQueue() 는 정상 작동하면 0 을 리턴한다. 큐를 추가하지 못했으면 -1 을 리턴한다
int atp_addQueue(PATP_DATA atp, ATP_PRIORITY priority=atp_realtime);

// 메인쓰레드가 일을 주지 않으면 각 워크 쓰레드는 지정된 시간이 경과하면 스스로 깨어난다
bool atp_setwaittime(struct timespec _w, int _n = -1);

// 각 워크쓰레드는 일정 시간이 자나면 스스로 깨어난다 스스로 깨어났을 때 ATP_STAT 에 따라 수행할 일이 있는 경우 사용
// _n 은 워크쓰레드 중 지정된 번호의 쓰레드에 적용됨을 나타낸다
// _n 을 -1 로 지정하면 전체 워크 쓰레드에 적용된다 (default)
// ATP_STAT 가 stat_run 인 경우는 atp_addQueue() 를 사용 하시오.
bool atp_setfunc(ATP_STAT _s, ThreadFunction _f, PATP_DATA _d, int _n = -1 );

// AsyncThreadPool 의 간단한 정보 조회
int atp_getThreadCount(); // 쓰레드풀의 갯수를 리턴한다
PTHREADINFO atp_getThreadInfo(); // 쓰레드풀 테이블 메모리 위치를 리턴한다
int atp_getRealtimeQueueCount(); // 실시간작업의뢰 큐 갯수를 리턴한다
int atp_getNormalQueueCount(); // 여유시간작업의뢰 큐 갯수를 리턴한다

// mutex 관련
int atp_worklock();		// 작업쓰레드간에 동기화를 위한 락이 필요한 경우
int atp_workunlock();	// 작업쓰레드간에 동기화를 위한 락이 필요한 경우
unsigned int atp_getWorkLockCount();	// 작업쓰레드간에 동기화를 위한 락 대기열 숫자 조회

// -------------------------------------------

#if defined(DEBUGTRACE)
	#define TRACE(...) \
	/* do while(0) 문은 블록이 없는 if문에서도 구문 없이 사용하기 위한 방법이다 */ \
	do { \
		time_t now = time(NULL); \
		struct	tm tm_s; \
		localtime_r(&now, &tm_s); \
		char buf[4096]; \
		int len = sprintf(buf,"%04d-%02d-%02d %02d:%02d:%02d " \
			, tm_s.tm_year + 1900 \
			, tm_s.tm_mon + 1 \
			, tm_s.tm_mday \
			, tm_s.tm_hour \
			, tm_s.tm_min \
			, tm_s.tm_sec \
			); \
		snprintf(buf+len,sizeof(buf)-len,__VA_ARGS__); \
		fwrite(buf,sizeof(char),strlen(buf),stdout); \
		fflush(stdout); \
	}while(0) 
#else
	#define TRACE(...) printf(__VA_ARGS__)
#endif

#endif	// end of #define (__AYNC_THREAD_POOL__)
