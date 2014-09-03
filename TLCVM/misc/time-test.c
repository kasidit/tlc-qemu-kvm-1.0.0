#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

int main(int argc, char **argv)
{
  uint64_t elasp; 

  if (argc < 2)
    {
      printf("USAGE: %s loop-iterations\n", argv[0]);
      return 1;
    }

  int iterations = atoi(argv[1]);

  struct timeval start, end;

  gettimeofday(&start, NULL);

  for (int i = 0; i < iterations; i++)
    {
      sleep(1);
    }

  gettimeofday(&end, NULL);

  elasp = ((end.tv_sec * 1000000 + end.tv_usec)-(start.tv_sec * 1000000 + start.tv_usec));

  printf("usec = %"PRId64" us, msec = %"PRId64" ms, sec = %"PRId64" s\n", 
           elasp, (uint64_t)(elasp/1000), (uint64_t)(elasp/1000000));

  printf("%ld\n", ((end.tv_sec * 1000000 + end.tv_usec)
		  - (start.tv_sec * 1000000 + start.tv_usec)));

  return 0;
}

