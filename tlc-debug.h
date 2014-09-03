static  int tlcdebugflag = 4;

#define	DREG 		if(tlcdebugflag == 1)
#define DDETAIL 	if(tlcdebugflag == 2)
#define	DVERYDETAIL	if(tlcdebugflag == 3)
#define	DVIC		if(tlcdebugflag == 4)
//#define DSPC1		if(tlcdebugflag == 10)
//#define THIDHEADER 	int thid = ((uint64_t) pthread_self()%1000);	
