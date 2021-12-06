
#define DEBUGTRACE

#include "AsyncThreadPool.h"

ATP_STAT sub1(PATP_DATA data)
{
	TRACE(" %s func, data is (%s)\n", __func__, data->s ? data->s : "");
	sleep(1);

	return stat_suspend;
}

ATP_STAT sub2(PATP_DATA data)
{
	TRACE(" %s func, data is (%s)\n", __func__, data->s ? data->s : "");
	sleep(2);

	return stat_suspend;
}

ATP_STAT sub9(PATP_DATA data)
{
	TRACE(" %s func, data is (%s)\n", __func__, data->s ? data->s : "");
	sleep(200000);

	return stat_suspend;
}

ATP_STAT test(PATP_DATA param)
{
	if (param->func)
		return param->func(param);
	else
		TRACE(" %s func, data is (%s)\n", __FUNCTION__, param->s  ? param->s : "");

	return stat_suspend;
}

ATP_STAT exit_func(PATP_DATA param)
{
	TRACE(" %s func, OK exit func check (%s)\n", __FUNCTION__, param && param->s ? param->s : "");

	return stat_exited;
}



int main(int argc, char* argv[])
{
	TRACE("--- AsyncThreadPool library test\n");

	size_t data_size = 1024;
	PATP_DATA atpdata;
	int nIndx;

	atp_create(3, test);

	for (nIndx = 0; nIndx < 11; nIndx++) {

		// atpdata 는 쓰레딩 작업후에 자동으로 free 된다
		atpdata = atp_alloc(data_size);
		
		snprintf(atpdata->s, atpdata->s_len, "loop no (%d)", nIndx);

		if (nIndx < 10) {
			if (nIndx % 2)
				atpdata->func = sub1;
			else
				atpdata->func = sub2;
		} else {
			// 인덱스 10 부터는 서브호출 없음
		}

		if (!atp_addQueue(atpdata)) {
			TRACE("--- thread add Queue successed %d \n", nIndx);
		} else {
			free(atpdata);
			TRACE("--- thread add Queue failed %d \n", nIndx);
		}
	}

	// 강제 킬 목적으로 sub9 할당
	atpdata = atp_alloc(data_size);
	snprintf(atpdata->s, atpdata->s_len, "loop no (XXX)");
	atpdata->func = sub9;
	atp_addQueue(atpdata);


	sleep(4);

	PTHREADINFO pThread = atp_getThreadInfo();
	TRACE("--- request thread end...\n");
	TRACE("--- thread queue_size(%d)\n", atp_getQueueCount());
	TRACE("-------------------------------------------\n");
	for (nIndx = 0; nIndx < atp_getThreadCount(); nIndx++) {
		TRACE("--- thread no=%d, executed=%lu\n", pThread[nIndx].nThreadNo, pThread[nIndx].nExecuteCount);
	}
	TRACE("-------------------------------------------\n");


	ATP_END end = argc > 1 ? atoi(argv[1]) ? gracefully : force : gracefully;
	bool exit = argc > 2 ? atoi(argv[2]) ? true : false : false;

	TRACE("--- endcode(%s) exit_func(%s)\n", end == gracefully ? "gracefully" : "force", exit ? "true" : "fale");
	
	if (exit) {
#if 0
		atp_setfunc(stat_exit, exit_func, NULL, -1);
#else
		for (nIndx = 0; nIndx < atp_getThreadCount(); nIndx++) {
			atpdata = atp_alloc(32);
			sprintf(atpdata->s, "work no %d", nIndx);
			atp_setfunc(stat_exit, exit_func, atpdata, nIndx);
		}
#endif
	}

	atp_destroy(end, exit);



	return 0;
}