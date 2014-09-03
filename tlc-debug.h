/*
 * Thread-based Live Migration (TLM) and Thread-based Live Checkpointing (TLC). 
 *
 * Copyright Kasidit Chanchio, Vasabilab. 2012
 *
 * Authors:
 *  Kasidit Chanchio   <kasiditchanchio@gmail.com>
 *
 *
 */

static  int tlcdebugflag = 4;

#define	DREG 		if(tlcdebugflag == 1)
#define DDETAIL 	if(tlcdebugflag == 2)
#define	DVERYDETAIL	if(tlcdebugflag == 3)
#define	DVIC		if(tlcdebugflag == 4)
//#define DSPC1		if(tlcdebugflag == 10)
//#define THIDHEADER 	int thid = ((uint64_t) pthread_self()%1000);	
