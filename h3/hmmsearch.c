/* main() for scoring a profile HMM against a sequence database.
 * 
 * SRE, Thu Dec 20 07:07:25 2007 [Janelia]
 * SVN $Id$
 */
#include "p7_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifdef HAVE_MPI
#include "mpi.h"
#endif 

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_getopts.h"
#include "esl_sqio.h"
#include "esl_histogram.h"
#include "esl_stopwatch.h"
#include "esl_gumbel.h"
#include "esl_exponential.h"

#include "hmmer.h"


static ESL_OPTIONS options[] = {
  /* name           type      default  env  range     toggles  reqs   incomp  help   docgroup*/
  { "-h",        eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  NULL, "show brief help on version and usage",              1 },
#ifdef HAVE_MPI
  { "--mpi",     eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  NULL, "run as an MPI parallel program",                    1 },
#endif
  { "--F1",      eslARG_REAL,  "0.02", NULL, NULL,      NULL,  NULL,  NULL, "MSP filter threshold: promote hits w/ P < F1",      2 },
  { "--F2",      eslARG_REAL,  "1e-3", NULL, NULL,      NULL,  NULL,  NULL, "Vit filter threshold: promote hits w/ P < F2",      2 },
  { "--F3",      eslARG_REAL,  "1e-5", NULL, NULL,      NULL,  NULL,  NULL, "Fwd filter threshold: promote hits w/ P < F3",      2 },
  { "--FX",      eslARG_REAL, "1e-12", NULL, NULL,      NULL,  NULL,  NULL, "Fwd/Vit choice threshold: hits w/ P < FX go to V",  2 },

  { "-o",        eslARG_OUTFILE, NULL, NULL, NULL,      NULL,  NULL,  NULL, "direct output to file <f>, not stdout",              3 },
  { "--textw",   eslARG_INT,    "120", NULL, NULL,      NULL,  NULL,  NULL, "sets maximum ASCII text output line length",         3 },
  { "--stall",   eslARG_NONE,   FALSE, NULL, NULL,      NULL,  NULL,  NULL, "arrest after start: for debugging MPI under gdb",    3 },  
 
 {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static char usage[]  = "[options] <query hmmfile> <target seqfile>";
static char banner[] = "search profile HMM(s) against a sequence database";



/* struct cfg_s: "Global" application configuration shared by all threads/MPI processes. */
struct cfg_s {
  /* Shared configuration in masters and workers                            */
  char            *hmmfile;     /* file to read HMM(s) from                 */
  char            *seqfile;     /* file to read sequence(s) from            */
  int              format;      /* format of seqfile                        */
  ESL_ALPHABET    *abc;         /* sequence alphabet                        */
  P7_BG           *bg;          /* null model                               */
  int              mode;	/* profile mode: e.g. p7_LOCAL              */
  P7_TOPHITS      *hitlist;	/* top-scoring sequence hits                */
  int              nseq;	/* number of sequences searched             */

  /* Shared configuration for MPI */
  int              do_mpi;
  int              my_rank;
  int              nproc;
  int              do_stall;

  /* Master only (i/o streams)                                          */
  P7_HMMFILE      *hfp;     /* open HMM file                            */  
  ESL_SQFILE      *sqfp;    /* open seqfile                             */
  FILE            *ofp;     /* output file for results (default stdout) */
};

static void process_commandline(int argc, char **argv, struct cfg_s *cfg, ESL_GETOPTS **ret_go);
static void init_shared_cfg(ESL_GETOPTS *go, struct cfg_s *cfg);
static int  init_master_cfg(ESL_GETOPTS *go, struct cfg_s *cfg, char *errbuf);
static void serial_master  (ESL_GETOPTS *go, struct cfg_s *cfg);
static int  output_per_sequence_hitlist(ESL_GETOPTS *go, struct cfg_s *cfg, P7_TOPHITS *hitlist);

int
main(int argc, char **argv)
{
  ESL_GETOPTS     *go	   = NULL;     /* command line processing                 */
  ESL_STOPWATCH   *w       = esl_stopwatch_Create();  /* timing                   */
  struct cfg_s     cfg;


  /* Initializations */
  process_commandline(argc, argv, &cfg, &go);    
  init_shared_cfg(go, &cfg);                        

  /* Stall here, if we need to wait for a debugger to attach to us (MPI) */
  while (cfg.do_stall); 

  /* Start timing */
  esl_stopwatch_Start(w);	                          
  
  /* Main body: hand off to serial version or MPI masters/workers, as appropriate */
#ifdef HAVE_MPI
  if (esl_opt_GetBoolean(go, "--mpi")) 
    {
      /* Initialize MPI, figure out who we are, and whether we're running
       * this show (proc 0) or working in it (procs >0).
       */
      cfg.do_mpi = TRUE;
      MPI_Init(&argc, &argv);
      MPI_Comm_rank(MPI_COMM_WORLD, &(cfg.my_rank));
      MPI_Comm_size(MPI_COMM_WORLD, &(cfg.nproc));
      if (cfg.my_rank == 0 && cfg.nproc < 2) p7_Fail("Need at least 2 MPI processes to run --mpi mode.");

#if 0
      if (cfg.my_rank > 0)   mpi_worker(go, &cfg);
      else                   mpi_master(go, &cfg);
#endif

      esl_stopwatch_Stop(w);
      esl_stopwatch_MPIReduce(w, 0, MPI_COMM_WORLD);
      MPI_Finalize();		/* both workers and masters reach this line */
    }
  else
#endif /*HAVE_MPI*/
    {		
      /* No MPI? Then we're just the serial master. */
      serial_master(go, &cfg);
      esl_stopwatch_Stop(w);
    }      

  /* Stop timing. */
  if (cfg.my_rank == 0) esl_stopwatch_Display(stdout, w, "# CPU time: ");

  /* Clean up and exit */
  if (cfg.my_rank == 0) {
    if (cfg.hfp  != NULL)               p7_hmmfile_Close(cfg.hfp);
    if (cfg.sqfp != NULL)               esl_sqfile_Close(cfg.sqfp);
    if (! esl_opt_IsDefault(go, "-o"))  fclose(cfg.ofp);
  }
  p7_bg_Destroy(cfg.bg);
  esl_alphabet_Destroy(cfg.abc);
  esl_getopts_Destroy(go);
  esl_stopwatch_Destroy(w);
  return eslOK;
}


/* process_commandline_and_help()
 * 
 * Processes the commandline, filling in fields in <cfg> and creating and returning
 * an <ESL_GETOPTS> options structure. The help page (hmmsearch -h) is formatted
 * here.
 */
static void
process_commandline(int argc, char **argv, struct cfg_s *cfg, ESL_GETOPTS **ret_go)
{
  ESL_GETOPTS *go = NULL;

  if ((go = esl_getopts_Create(options))     == NULL)     p7_Die("Internal failure creating options object");
  if (esl_opt_ProcessCmdline(go, argc, argv) != eslOK)  { printf("Failed to parse command line: %s", go->errbuf); goto ERROR; }
  if (esl_opt_VerifyConfig(go)               != eslOK)  { printf("Failed to parse command line: %s", go->errbuf); goto ERROR; }
 
  /* help format: */
  if (esl_opt_GetBoolean(go, "-h") == TRUE) 
    {
      p7_banner(stdout, argv[0], banner);
      esl_usage(stdout, argv[0], usage);
      puts("\nwhere options are:");
      esl_opt_DisplayHelp(stdout, go, 1, 2, 80); /* 1= group; 2 = indentation; 80=textwidth*/
      esl_opt_DisplayHelp(stdout, go, 2, 2, 80); /* 2= group; 2 = indentation; 80=textwidth*/
      esl_opt_DisplayHelp(stdout, go, 3, 2, 80); /* 3= group; 2 = indentation; 80=textwidth*/
      exit(0);
    }

  if (esl_opt_ArgNumber(go)                 != 2)      { puts("Incorrect number of command line arguments.");      goto ERROR; }
  if ((cfg->hmmfile = esl_opt_GetArg(go, 1)) == NULL)  { puts("Failed to get <seqfile> argument on command line"); goto ERROR; }
  if ((cfg->seqfile = esl_opt_GetArg(go, 2)) == NULL)  { puts("Failed to get <hmmfile> argument on command line"); goto ERROR; }

  *ret_go = go;
  return;
  
 ERROR:  /* all errors handled here are user errors, so be polite.  */
  esl_usage(stdout, argv[0], usage);
  puts("\nwhere most common options are:");
  esl_opt_DisplayHelp(stdout, go, 1, 2, 80); /* 1= group; 2 = indentation; 80=textwidth*/
  printf("\nTo see more help on available options, do %s -h\n\n", argv[0]);
  exit(1);  
}
 



static void
init_shared_cfg(ESL_GETOPTS *go, struct cfg_s *cfg)
{
  cfg->format   = eslSQFILE_UNKNOWN;    /* eventually, allow options to set this            */
  cfg->mode     = p7_LOCAL;

  /* in serial, master has the hitlists, of course; 
   * in MPI, workers assemble partial hitlists, send to master, 
   * master accumulates in its main hitlist. 
   */
  cfg->hitlist  = p7_tophits_Create();
  cfg->nseq     = 0;

  /* These will be initialized when we read first HMM and know our alphabet: */
  cfg->abc      = NULL;		        
  cfg->bg       = NULL;		        

  /* These will be initialized later if --mpi is set: */
  cfg->do_mpi   = FALSE;
  cfg->my_rank  = 0;
  cfg->nproc    = 0;
  cfg->do_stall = esl_opt_GetBoolean(go, "--stall");

  /* These are initialized later in the master only: */
  cfg->hfp      = NULL;
  cfg->sqfp     = NULL;
  cfg->ofp      = NULL;
}


/* init_master_cfg()
 *
 * Responsible for opening the i/o streams that only a master has his
 * hands on.
 *
 * Error handling relies on these pointers being initialized to NULL
 * by the caller; this was done in init_shared_cfg().
 *                   
 * Since now we may be within MPI, errors can no longer be handled by
 * dumping a message and exiting; we have worker processes to shut
 * down, and possibly other cleanup to do, so we must try to delay
 * resolution of any error messages until after we attempt to clean
 * up. Therefore errors return (code, errmsg) by the ESL_FAIL mech.
 */
static int
init_master_cfg(ESL_GETOPTS *go, struct cfg_s *cfg, char *errbuf)
{
  char *filename;
  int   status;

  status = p7_hmmfile_Open(cfg->hmmfile, NULL, &(cfg->hfp));
  if (status == eslENOTFOUND) ESL_FAIL(status, errbuf, "Failed to open hmm file %s for reading.\n",     cfg->hmmfile);
  else if (status != eslOK)   ESL_FAIL(status, errbuf, "Unexpected error %d in opening hmm file %s.\n", status, cfg->hmmfile);  
  
  status = esl_sqfile_Open(cfg->seqfile, cfg->format, p7_SEQDBENV, &(cfg->sqfp));
  if      (status == eslENOTFOUND) ESL_FAIL(status, errbuf, "Failed to open sequence file %s for reading\n",      cfg->seqfile);
  else if (status == eslEFORMAT)   ESL_FAIL(status, errbuf, "Sequence file %s is empty or misformatted\n",        cfg->seqfile);
  else if (status == eslEINVAL)    ESL_FAIL(status, errbuf, "Can't autodetect format of a stdin or .gz seqfile");
  else if (status != eslOK)        ESL_FAIL(status, errbuf, "Unexpected error %d opening sequence file %s\n", status, cfg->seqfile);

  filename = esl_opt_GetString(go, "-o");
  if (filename != NULL) {
    if ((cfg->ofp = fopen(filename, "w")) == NULL) 
      ESL_FAIL(eslESYS, errbuf, "Failed to open -o output file %s\n", filename);
  } else cfg->ofp = stdout;

  return eslOK;
}


static void
serial_master(ESL_GETOPTS *go, struct cfg_s *cfg)
{
  P7_HMM          *hmm     = NULL;     /* query HMM                               */
  P7_PROFILE      *gm      = NULL;     /* profile HMM                             */
  P7_OPROFILE     *om      = NULL;     /* optimized profiile                      */
  ESL_SQ          *sq      = NULL;      /* target sequence                        */
  P7_TRACE        *tr      = NULL;     /* trace of hmm aligned to sq              */
  P7_GMX          *gx      = NULL;     /* DP matrix                               */
  P7_OMX          *ox      = NULL;     /* optimized DP matrix                     */
  double           F1      = esl_opt_GetReal(go, "--F1"); /* MSPFilter threshold: must be < F1 to go on */
  double           F2      = esl_opt_GetReal(go, "--F2"); /* ViterbiFilter threshold */
  double           F3      = esl_opt_GetReal(go, "--F3"); /* ForwardFilter threshold */
  double           FF      = esl_opt_GetReal(go, "--FX"); /* Viterbi/Forward postprocessing threshold */
  float            usc, vfsc, ffsc;    /* filter scores                           */
  float            final_sc;	       /* final bit score                         */
  float            nullsc;             /* null model score                        */
  double           P;		       /* P-value of a hit */
  char             errbuf[eslERRBUFSIZE];
  int              status, hstatus, sstatus;

  tr = p7_trace_Create();
  gx = p7_gmx_Create(200, 400);	/* initial alloc is for M=200, L=400; will grow as needed */
  ox = p7_omx_Create(200);
  
  if ((status = init_master_cfg(go, cfg, errbuf)) != eslOK) esl_fatal(errbuf);
 
  while ((hstatus = p7_hmmfile_Read(cfg->hfp, &(cfg->abc), &hmm)) == eslOK) 
    {
      /* One time only initializations after abc becomes known: */
      if (cfg->bg == NULL) cfg->bg = p7_bg_Create(cfg->abc);
      if (sq      == NULL) sq      = esl_sq_CreateDigital(cfg->abc);
      
      gm = p7_profile_Create (hmm->M, cfg->abc);
      om = p7_oprofile_Create(hmm->M, cfg->abc);
      p7_ProfileConfig(hmm, cfg->bg, gm, 100, cfg->mode); /* 100 is a dummy length for now */
      p7_oprofile_Convert(gm, om);
      p7_omx_GrowTo(ox, om->M);

      while ( (sstatus = esl_sqio_Read(cfg->sqfp, sq)) == eslOK)
	{
	  cfg->nseq++;
	  p7_oprofile_ReconfigLength(om, sq->n);

	  /* Null model score for this sequence.  */
	  p7_bg_SetLength(cfg->bg, sq->n);
	  p7_bg_NullOne(cfg->bg, sq->dsq, sq->n, &nullsc);

	  /* First level filter: the MSP filter. */
	  p7_MSPFilter(sq->dsq, sq->n, om, ox, &usc);
	  usc = (usc - nullsc) / eslCONST_LOG2;
	  P = esl_gumbel_surv(usc,  hmm->evparam[p7_MU],  hmm->evparam[p7_LAMBDA]);
	  if (P >= F1) { esl_sq_Reuse(sq); continue; } 

	  /* Second level filter: ViterbiFilter() */
	  if (P >= F3) 
	    {
	      p7_ViterbiFilter(sq->dsq, sq->n, om, ox, &vfsc);  
	      vfsc = (vfsc-nullsc) / eslCONST_LOG2;
	      P  = esl_gumbel_surv(vfsc,  hmm->evparam[p7_MU],  hmm->evparam[p7_LAMBDA]);
	      if (P >= F2) { esl_sq_Reuse(sq); continue; } /* this also should be settable */
	    }
	  
	  /* Third filter: ForwardFilter() */
	  if (P >= F3) 
	    {
	      p7_ForwardFilter(sq->dsq, sq->n, om, ox, &ffsc);  
	      ffsc = (ffsc-nullsc) / eslCONST_LOG2;
	      P = esl_exp_surv   (ffsc, hmm->evparam[p7_TAU], hmm->evparam[p7_LAMBDA]);
	      if (P >= F3) { esl_sq_Reuse(sq); continue; } /* this also should be settable */
	    }

	  /* We're past the filters. Everything remaining is almost certainly a hit */
	  p7_gmx_GrowTo(gx, hmm->M, sq->n); /* realloc DP matrix as needed */
	  p7_ReconfigLength(gm, sq->n);
	  if (P >= FF) {
	    p7_GForward(sq->dsq, sq->n, gm, gx, &final_sc);         
	    final_sc = ( final_sc-nullsc) / eslCONST_LOG2;
	    P  = esl_exp_surv (final_sc,  hmm->evparam[p7_TAU], hmm->evparam[p7_LAMBDA]);
	  } else {
	    p7_GViterbi(sq->dsq, sq->n, gm, gx, &final_sc);         
	    final_sc = ( final_sc-nullsc) / eslCONST_LOG2;
	    P  = esl_gumbel_surv (final_sc,  hmm->evparam[p7_MU], hmm->evparam[p7_LAMBDA]);
	  }

	  /* Register the hit, for later sorting and output. */
	  p7_tophits_Add(cfg->hitlist,
			 sq->name, sq->acc, sq->desc,
			 -log(P), final_sc, P, final_sc, P, 
			 0, 0, 0,
			 0, 0, 0,
			 0, 0, NULL);
	  esl_sq_Reuse(sq);
	}
      if (sstatus != eslEOF) p7_Fail("Sequence file %s has a format problem: read failed at line %d:\n%s\n",
				     cfg->seqfile, cfg->sqfp->linenumber, cfg->sqfp->errbuf);     


      output_per_sequence_hitlist(go, cfg, cfg->hitlist);


      p7_profile_Destroy(gm);
      p7_oprofile_Destroy(om);
    }
  if      (hstatus == eslEOD)       p7_Fail("read failed, HMM file %s may be truncated?", cfg->hmmfile);
  else if (hstatus == eslEFORMAT)   p7_Fail("bad file format in HMM file %s",             cfg->hmmfile);
  else if (hstatus == eslEINCOMPAT) p7_Fail("HMM file %s contains different alphabets",   cfg->hmmfile);
  else if (hstatus != eslEOF)       p7_Fail("Unexpected error in reading HMMs from %s",   cfg->hmmfile);

  p7_gmx_Destroy(gx);
  p7_omx_Destroy(ox);
  p7_trace_Destroy(tr);
  esl_sq_Destroy(sq);
  return;
}



static int
output_per_sequence_hitlist(ESL_GETOPTS *go, struct cfg_s *cfg, P7_TOPHITS *hitlist)
{
  int h;
  int textw = esl_opt_GetInteger(go, "--textw");
  int namew = ESL_MAX(8, p7_tophits_GetMaxNameLength(hitlist));
  int descw = textw - namew - 24;         /* 7.1f score, 10.2g Eval, 3d ndom, 4 spaces betw 5 fields: 24 char  */
  double E;
  
  p7_tophits_Sort(hitlist);

  fprintf(cfg->ofp, "Scores for complete sequences (score includes all domains):\n");

  fprintf(cfg->ofp, "%-*s %-*s %7s %10s %3s\n", namew, "Sequence", descw, "Description", "Score", "E-value", " N ");
  fprintf(cfg->ofp, "%-*s %-*s %7s %10s %3s\n", namew, "--------", descw, "-----------", "-----", "-------", "---");

  for (h = 0; h < hitlist->N; h++)
    {
      E = (double) cfg->nseq * hitlist->hit[h]->pvalue;

      fprintf(cfg->ofp, "%-*s %-*.*s %7.1f %10.2g %3d\n",
	      namew,        hitlist->hit[h]->name,
	      descw, descw, hitlist->hit[h]->desc,
	      hitlist->hit[h]->score,
	      hitlist->hit[h]->pvalue * (double) cfg->nseq,
	      hitlist->hit[h]->ndom);
    }
  return eslOK;
}
