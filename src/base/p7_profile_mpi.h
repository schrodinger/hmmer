#ifndef P7_PROFILE_MPI_INCLUDED
#define P7_PROFILE_MPI_INCLUDED
#ifdef  HAVE_MPI

#include "p7_config.h"

#include <mpi.h>

#include "esl_alphabet.h"

#include "base/p7_bg.h"
#include "base/p7_profile.h"

extern int p7_profile_mpi_Send(P7_PROFILE *gm, int dest, int tag, MPI_Comm comm, char **buf, int *nalloc);
extern int p7_profile_mpi_PackSize(P7_PROFILE *gm, MPI_Comm comm, int *ret_n);
extern int p7_profile_mpi_Pack(P7_PROFILE *gm, char *buf, int n, int *pos, MPI_Comm comm);
extern int p7_profile_mpi_Unpack(char *buf, int n, int *pos, MPI_Comm comm,                          ESL_ALPHABET **byp_abc, P7_PROFILE **ret_gm);
extern int p7_profile_mpi_Recv(int source, int tag,          MPI_Comm comm, char **buf, int *nalloc, ESL_ALPHABET **byp_abc, P7_PROFILE **ret_gm);

#endif /*HAVE_MPI*/
#endif /*P7_HMM_MPI_INCLUDED*/

/*****************************************************************
 * @LICENSE@
 * 
 * SVN $Id$
 * SVN $URL$
 *****************************************************************/