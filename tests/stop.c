/* 
 * Very simple hello-world using C and MPI
 *
*/

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "mpi.h"

int
main(int argc, char* argv[]) {
  int myRank, numProcs;
  MPI_Status status;

  MPI_Init(&argc,&argv);

  MPI_Comm_rank(MPI_COMM_WORLD, &myRank);
  MPI_Comm_size(MPI_COMM_WORLD, &numProcs);

  raise(SIGSTOP);

  MPI_Finalize();

  return 0;
}

