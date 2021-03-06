/**
 * @file    AOloopControl.c
 * @brief   Adaptive Optics Control loop engine
 * 
 * AO engine uses stream data structure
 *  
 * @author  O. Guyon
 * @date    10 Jun 2017
 *
 * 
 * @bug No known bugs.
 * 
 * @see http://oguyon.github.io/AdaptiveOpticsControl/src/AOloopControl/doc/AOloopControl.html
 * 
 * 
 * 
 * @defgroup AOloopControl_streams Image streams
 * @defgroup AOloopControl_AOLOOPCONTROL_CONF AOloopControl main data structure
 * 
 */



#define _GNU_SOURCE

// uncomment for test print statements to stdout
//#define _PRINT_TEST



/* =============================================================================================== */
/* =============================================================================================== */
/*                                        HEADER FILES                                             */
/* =============================================================================================== */
/* =============================================================================================== */

#include <stdint.h>
#include <unistd.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h> // needed for tid = syscall(SYS_gettid);



#ifdef __MACH__
#include <mach/mach_time.h>
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 0
int clock_gettime(int clk_id, struct mach_timespec *t) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    uint64_t time;
    time = mach_absolute_time();
    double nseconds = ((double)time * (double)timebase.numer)/((double)timebase.denom);
    double seconds = ((double)time * (double)timebase.numer)/((double)timebase.denom * 1e9);
    t->tv_sec = seconds;
    t->tv_nsec = nseconds;
    return 0;
}
#else
#include <time.h>
#endif

#include <math.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <err.h>
#include <fcntl.h>
#include <sched.h>
#include <ncurses.h>
#include <semaphore.h>


#include <gsl/gsl_matrix.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_eigen.h>
#include <gsl/gsl_blas.h>
#include <pthread.h>

#include <fitsio.h>

#include "CLIcore.h"
#include "00CORE/00CORE.h"
#include "COREMOD_memory/COREMOD_memory.h"
#include "COREMOD_iofits/COREMOD_iofits.h"
#include "COREMOD_tools/COREMOD_tools.h"
#include "COREMOD_arith/COREMOD_arith.h"
#include "linopt_imtools/linopt_imtools.h"
#include "AOloopControl/AOloopControl.h"
#include "image_filter/image_filter.h"
#include "info/info.h"
#include "ZernikePolyn/ZernikePolyn.h"
#include "linopt_imtools/linopt_imtools.h"
#include "image_gen/image_gen.h"
#include "statistic/statistic.h"
#include "fft/fft.h"


#include "AOloopControl_IOtools/AOloopControl_IOtools.h"



#ifdef HAVE_CUDA
#include "cudacomp/cudacomp.h"
#endif

# ifdef _OPENMP
# include <omp.h>
#define OMP_NELEMENT_LIMIT 1000000
# endif



/* =============================================================================================== */
/* =============================================================================================== */
/*                                      DEFINES, MACROS                                            */
/* =============================================================================================== */
/* =============================================================================================== */



#define MAX_MBLOCK 20






// data passed to each thread
typedef struct
{
    long nelem;
    float *arrayptr;
    float *result; // where to white status
} THDATA_IMTOTAL;






/* =============================================================================================== */
/* =============================================================================================== */
/*                                  GLOBAL DATA DECLARATION                                        */
/* =============================================================================================== */
/* =============================================================================================== */





/* =============================================================================================== */
/*                    LOGGING ACCESS TO FUNCTIONS                                                  */
/* =============================================================================================== */

// uncomment at compilation time to enable logging of function entry/exit
//#define AOLOOPCONTROL_LOGFUNC
static int AOLOOPCONTROL_logfunc_level = 0;
static int AOLOOPCONTROL_logfunc_level_max = 2; // log all levels equal or below this number
static char AOLOOPCONTROL_logfunc_fname[] = "AOloopControl.fcall.log";
static char flogcomment[200];


// GPU MultMat indexes
//
// 0: main loop CM multiplication
//
// 1: set DM modes:
//         int set_DM_modes(long loop)
//
// 2: compute modes loop
//         int AOloopControl_CompModes_loop(char *ID_CM_name, char *ID_WFSref_name, char *ID_WFSim_name, char *ID_WFSimtot_name, char *ID_coeff_name)
//
// 3: coefficients to DM shape [ NOTE: CRASHES IF NOT USING index 0 ]
//         int AOloopControl_GPUmodecoeffs2dm_filt_loop(char *modecoeffs_name, char *DMmodes_name, int semTrigg, char *out_name, int GPUindex, long loop, int offloadMode)
//
// 4: Predictive control (in modules linARfilterPred)







// TIMING
static struct timespec tnow;
static struct timespec tdiff;
static double tdiffv;




static int AOLCOMPUTE_TOTAL_ASYNC_THREADinit = 0;
static sem_t AOLCOMPUTE_TOTAL_ASYNC_sem_name;
static int AOLCOMPUTE_TOTAL_INIT = 0; // toggles to 1 AFTER total for first image is computed


static int AOLCOMPUTE_DARK_SUBTRACT_THREADinit = 0;
static int COMPUTE_DARK_SUBTRACT_NBTHREADS = 1;
static sem_t AOLCOMPUTE_DARK_SUBTRACT_sem_name[32];
static sem_t AOLCOMPUTE_DARK_SUBTRACT_RESULT_sem_name[32];

static int COMPUTE_GPU_SCALING = 0; // perform scaling inside GPU instead of CPU
static int initWFSref_GPU[100] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static int initcontrMcact_GPU[100] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static float GPU_alpha = 0.0;
static float GPU_beta = 0.0;


static int COMPUTE_PIXELSTREAMING = 0; // multiple pixel groups
static int PIXSTREAM_NBSLICES = 1; // number of image slices (= pixel groups)
static int PIXSTREAM_SLICE; // slice index 0 = all pixels



static long ti; // thread index

// static int MATRIX_COMPUTATION_MODE = 0;
// 0: compute sequentially modes and DM commands
// 1: use combined control matrix



static int wcol, wrow; // window size




/* =============================================================================================== */
/*                    aoconfID are global variables for convenience                                */
/* =============================================================================================== */


// Hardware connections
static long aoconfID_wfsim = -1;
static uint8_t WFSatype;
static long aoconfID_dmC = -1;
static long aoconfID_dmRM = -1;





static long aoconfID_wfsdark = -1;
static long aoconfID_imWFS0 = -1;
static long aoconfID_imWFS0tot = -1;
static long aoconfID_imWFS1 = -1;
static long aoconfID_imWFS2 = -1;
static long aoconfID_wfsref0 = -1;
static long aoconfID_wfsref = -1;

static long aoconfID_DMmodes = -1;
static long aoconfID_dmdisp = -1;  // to notify DMcomb that DM maps should be summed


// Control Modes
static long aoconfID_cmd_modes = -1;
static long aoconfID_meas_modes = -1; // measured
static long aoconfID_RMS_modes = -1;
static long aoconfID_AVE_modes = -1;



// mode gains, multf, limit are set in 3 tiers
// global gain
// block gain
// individual gains

// blocks
static long aoconfID_gainb = -1; // block modal gains
static long aoconfID_multfb = -1; // block modal gains
static long aoconfID_limitb = -1; // block modal gains

// individual modes
static long aoconfID_GAIN_modes = -1;
static long aoconfID_LIMIT_modes = -1;
static long aoconfID_MULTF_modes = -1;

static long aoconfID_cmd_modesRM = -1;

static long aoconfID_wfsmask = -1;
static long aoconfID_dmmask = -1;

static long aoconfID_respM = -1;
static long aoconfID_contrM = -1; // pixels -> modes
static long aoconfID_contrMc = -1; // combined control matrix: pixels -> DM actuators
static long aoconfID_meas_act = -1;
static long aoconfID_contrMcact[100] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

// pixel streaming
static long aoconfID_pixstream_wfspixindex; // index of WFS pixels

static long aoconfID_looptiming = -1; // control loop timing data. Pixel values correspond to time offset
// currently has 20 timing slots
// beginning of iteration is defined when entering "wait for image"
// md[0].atime.ts is absolute time at beginning of iteration
//
// pixel 0 is dt since last iteration
//
// pixel 1 is time from beginning of loop to status 01
// pixel 2 is time from beginning of loop to status 02
// ...
static long NBtimers = 21;

static long aoconfIDlogdata = -1;
static long aoconfIDlog0 = -1;
static long aoconfIDlog1 = -1;

static int *WFS_active_map; // used to map WFS pixels into active array
static int *DM_active_map; // used to map DM actuators into active array
static long aoconfID_meas_act_active;
static long aoconfID_imWFS2_active[100];








static int RMACQUISITION = 0;  // toggles to 1 when resp matrix is being acquired


static long wfsrefcnt0 = -1;
static long contrMcactcnt0[100] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};;



// variables used by functions
static char Average_cam_frames_dname[200];
static long Average_cam_frames_IDdark = -1;
static long Average_cam_frames_nelem = 1;


static int GPUcntMax = 100;
static int *GPUset0;
static int *GPUset1;





/* =============================================================================================== */
/*                                     MAIN DATA STRUCTURES                                        */
/* =============================================================================================== */

extern DATA data;

#define NB_AOloopcontrol 10 // max number of loops
static long LOOPNUMBER = 0; // current loop index
static int AOloopcontrol_meminit = 0;
static int AOlooploadconf_init = 0;

#define AOconfname "/tmp/AOconf.shm"
static AOLOOPCONTROL_CONF *AOconf; // configuration - this can be an array





/* =============================================================================================== */
/*                         BUFFERS (GLOBALS FOR CONVENIENCE & SPEED)                               */
/* =============================================================================================== */

// camera read
static float *arrayftmp;
static unsigned short *arrayutmp;
static int avcamarraysInit = 0;
static float normfloorcoeff = 1.0;
static float IMTOTAL = 0.0;















/* =============================================================================================== */
/*                                        DATA STREAMS SEMAPHORES                                  */
/* =============================================================================================== */
/*

NOTATIONS:
 [streamA]wn >--(function)--> [streamB]pm : function waits on semaphore #n of streamA and post #m of streamB
 pa    : post all



  [aol#_wfsim] : raw WFS input image, all semaphores posted when image is read
  [aol#_wfsim]w0 >--(Read_cam_frame)--> [aol#_imWFS0]pa [aol#_imWFS1]pa


  [aol#_imWFS0] : dark-subtracted WFS input image


  [aol#_imWFS1] : normalized dark-subtracted WFS input image


  [aol#_imWFS2] :

 */






























// CLI commands
//
// function CLI_checkarg used to check arguments
// CLI_checkarg ( CLI argument index , type code )
//
// type codes:
// 1: float
// 2: long
// 3: string, not existing image
// 4: existing image
// 5: string
//






/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 1. INITIALIZATION, configurations
 *  Allocate memory, import/export configurations */
/* =============================================================================================== */
/* =============================================================================================== */

/** @brief CLI function for AOloopControl_loadconfigure */
int_fast8_t AOloopControl_loadconfigure_cli() {
    if(CLI_checkarg(1,2)==0) {
        AOloopControl_loadconfigure(data.cmdargtoken[1].val.numl, 1, 10);
        return 0;
    }
    else return 1;
}




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 2. LOW LEVEL UTILITIES & TOOLS    
 *  Useful tools */
/* =============================================================================================== */
/* =============================================================================================== */

/* =============================================================================================== */
/** @name AOloopControl - 2.1. LOW LEVEL UTILITIES & TOOLS - LOAD DATA STREAMS */
/* =============================================================================================== */

/* =============================================================================================== */
/** @name AOloopControl - 2.2. LOW LEVEL UTILITIES & TOOLS - DATA STREAMS PROCESSING */
/* =============================================================================================== */


/** @brief CLI function for AOloopControl_stream3Dto2D */
/*int_fast8_t AOloopControl_stream3Dto2D_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,3)+CLI_checkarg(3,2)+CLI_checkarg(4,2)==0) {
        AOloopControl_stream3Dto2D(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.numl);
        return 0;
    }
    else return 1;
}*/


/* =============================================================================================== */
/** @name AOloopControl - 2.3. LOW LEVEL UTILITIES & TOOLS - MISC COMPUTATION ROUTINES */
/* =============================================================================================== */

/** @brief CLI function for AOloopControl_CrossProduct */
int_fast8_t AOloopControl_CrossProduct_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,3)==0) {
        AOloopControl_CrossProduct(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string);
        return 0;
    }
    else return 1;
}


/** @brief CLI function for AOloopControl_mkSimpleZpokeM */
int_fast8_t AOloopControl_mkSimpleZpokeM_cli()
{
    if(CLI_checkarg(1,2)+CLI_checkarg(2,2)+CLI_checkarg(3,3)==0)    {
        AOloopControl_mkSimpleZpokeM(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.string);
        return 0;
    }
    else        return 1;
}




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 3. WFS INPUT
 *  Read camera imates */
/* =============================================================================================== */
/* =============================================================================================== */

/** @brief CLI function for AOloopControl_camimage_extract2D_sharedmem_loop */
int_fast8_t AOloopControl_camimage_extract2D_sharedmem_loop_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,5)+CLI_checkarg(3,3)+CLI_checkarg(4,2)+CLI_checkarg(5,2)+CLI_checkarg(6,2)+CLI_checkarg(7,2)==0) {
        AOloopControl_camimage_extract2D_sharedmem_loop(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string , data.cmdargtoken[4].val.numl, data.cmdargtoken[5].val.numl, data.cmdargtoken[6].val.numl, data.cmdargtoken[7].val.numl);
        return 0;
    }
    else return 1;
}





/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 4. ACQUIRING CALIBRATION
 *  Measure system response */
/* =============================================================================================== */
/* =============================================================================================== */

/** @brief CLI function for AOloopControl_RespMatrix_Fast */
int_fast8_t AOloopControl_RespMatrix_Fast_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,4)+CLI_checkarg(4,2)+CLI_checkarg(5,1)+CLI_checkarg(6,1)+CLI_checkarg(7,1)+CLI_checkarg(8,3)==0) {
        AOloopControl_RespMatrix_Fast(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.numl, data.cmdargtoken[5].val.numf, data.cmdargtoken[6].val.numf, data.cmdargtoken[7].val.numf, data.cmdargtoken[8].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_Measure_WFSrespC */
int_fast8_t AOloopControl_Measure_WFSrespC_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,2)+CLI_checkarg(3,2)+CLI_checkarg(4,2)+CLI_checkarg(5,4)+CLI_checkarg(6,5)+CLI_checkarg(7,2)+CLI_checkarg(8,2)+CLI_checkarg(9,2)==0) {
        AOloopControl_Measure_WFSrespC(LOOPNUMBER, data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.numl, data.cmdargtoken[5].val.string, data.cmdargtoken[6].val.string, data.cmdargtoken[7].val.numl, data.cmdargtoken[8].val.numl, data.cmdargtoken[9].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_Measure_WFS_linResponse */
int_fast8_t AOloopControl_Measure_WFS_linResponse_cli() {
    if(CLI_checkarg(1,1)+CLI_checkarg(2,2)+CLI_checkarg(3,2)+CLI_checkarg(4,2)+CLI_checkarg(5,2)+CLI_checkarg(6,4)+CLI_checkarg(7,5)+CLI_checkarg(8,5)+CLI_checkarg(9,2)+CLI_checkarg(10,2)+CLI_checkarg(11,2)==0) {
        AOloopControl_Measure_WFS_linResponse(LOOPNUMBER, data.cmdargtoken[1].val.numf, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.numl, data.cmdargtoken[5].val.numl, data.cmdargtoken[6].val.string, data.cmdargtoken[7].val.string, data.cmdargtoken[8].val.string, data.cmdargtoken[9].val.numl, data.cmdargtoken[10].val.numl, data.cmdargtoken[11].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_Measure_zonalRM */
int_fast8_t AOloopControl_Measure_zonalRM_cli() {
    if(CLI_checkarg(1,1)+CLI_checkarg(2,2)+CLI_checkarg(3,2)+CLI_checkarg(4,2)+CLI_checkarg(5,2)+CLI_checkarg(6,3)+CLI_checkarg(7,3)+CLI_checkarg(8,3)+CLI_checkarg(9,3)+CLI_checkarg(10,2)+CLI_checkarg(11,2)+CLI_checkarg(12,2)+CLI_checkarg(13,2)==0) {
        AOloopControl_Measure_zonalRM(LOOPNUMBER, data.cmdargtoken[1].val.numf, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.numl, data.cmdargtoken[5].val.numl, data.cmdargtoken[6].val.string, data.cmdargtoken[7].val.string, data.cmdargtoken[8].val.string, data.cmdargtoken[9].val.string, data.cmdargtoken[10].val.numl, data.cmdargtoken[11].val.numl, data.cmdargtoken[12].val.numl, data.cmdargtoken[13].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_Measure_Resp_Matrix */
int_fast8_t AOloopControl_Measure_Resp_Matrix_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,1)+CLI_checkarg(3,2)+CLI_checkarg(4,2)+CLI_checkarg(5,2)==0) {
        Measure_Resp_Matrix(LOOPNUMBER, data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numf, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.numl, data.cmdargtoken[5].val.numl);
        return 0;
    }
    else return 1;
}




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 5. COMPUTING CALIBRATION
 *  Compute control matrix, modes */
/* =============================================================================================== */
/* =============================================================================================== */

/** @brief CLI function for AOloopControl_mkSlavedAct */
int_fast8_t AOloopControl_mkSlavedAct_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,1)+CLI_checkarg(3,3)==0) {
        AOloopControl_mkSlavedAct(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numf, data.cmdargtoken[3].val.string);
        return 0;
    }
    else	return 1;
}

/** @brief CLI function for AOloopControl_mkloDMmodes */
int_fast8_t AOloopControl_mkloDMmodes_cli() {
    if(CLI_checkarg(1,3)+CLI_checkarg(2,2)+CLI_checkarg(3,2)+CLI_checkarg(4,1)+CLI_checkarg(5,1)+CLI_checkarg(6,1)+CLI_checkarg(7,1)+CLI_checkarg(8,1)+CLI_checkarg(9,1)+CLI_checkarg(10,2)==0) {
        AOloopControl_mkloDMmodes(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.numf, data.cmdargtoken[5].val.numf, data.cmdargtoken[6].val.numf, data.cmdargtoken[7].val.numf, data.cmdargtoken[8].val.numf, data.cmdargtoken[9].val.numf, data.cmdargtoken[10].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_mkCM */
int_fast8_t AOloopControl_mkCM_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,3)+CLI_checkarg(3,1)==0) {
        AOloopControl_mkCM(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_mkModes */
int_fast8_t AOloopControl_mkModes_cli() {
    if(CLI_checkarg(1,3)+CLI_checkarg(2,2)+CLI_checkarg(3,2)+CLI_checkarg(4,1)+CLI_checkarg(5,1)+CLI_checkarg(6,1)+CLI_checkarg(7,1)+CLI_checkarg(8,1)+CLI_checkarg(9,1)+CLI_checkarg(10,2)+CLI_checkarg(11,2)+CLI_checkarg(12,1)==0) {
        AOloopControl_mkModes(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.numf, data.cmdargtoken[5].val.numf, data.cmdargtoken[6].val.numf, data.cmdargtoken[7].val.numf, data.cmdargtoken[8].val.numf, data.cmdargtoken[9].val.numf, data.cmdargtoken[10].val.numl, data.cmdargtoken[11].val.numl, data.cmdargtoken[12].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_mkModes_Simple */
int_fast8_t AOloopControl_mkModes_Simple_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,2)+CLI_checkarg(3,2)+CLI_checkarg(4,1)==0) {
        AOloopControl_mkModes_Simple(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_computeCM */
int_fast8_t AOloopControl_computeCM_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,4)+CLI_checkarg(3,3)+CLI_checkarg(4,1)+CLI_checkarg(5,2)+CLI_checkarg(6,1)==0) {
        compute_ControlMatrix(LOOPNUMBER, data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, "evecM", data.cmdargtoken[4].val.numf, data.cmdargtoken[5].val.numl, data.cmdargtoken[6].val.numf);
        save_fits("evecM","!evecM.fits");
        delete_image_ID("evecM");
    } else return 1;
}

/** @brief CLI function for AOloopControl_loadCM */
int_fast8_t AOloopControl_loadCM_cli() {
    if(CLI_checkarg(1,3)==0) {
        AOloopControl_loadCM(LOOPNUMBER, data.cmdargtoken[1].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_mkHadamardModes */
int_fast8_t AOloopControl_mkHadamardModes_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,3)==0) {
        AOloopControl_mkHadamardModes(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_Hadamard_decodeRM */
int_fast8_t AOloopControl_Hadamard_decodeRM_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,4)+CLI_checkarg(4,3)==0) {
        AOloopControl_Hadamard_decodeRM(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_mkCalib_map_mask */
int_fast8_t AOloopControl_mkCalib_map_mask_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,3)+CLI_checkarg(3,3)+CLI_checkarg(4,1)+CLI_checkarg(5,1)+CLI_checkarg(6,1)+CLI_checkarg(7,1)+CLI_checkarg(8,1)+CLI_checkarg(9,1)+CLI_checkarg(10,1)+CLI_checkarg(11,1)==0) {
        AOloopControl_mkCalib_map_mask(LOOPNUMBER, data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.numf, data.cmdargtoken[5].val.numf, data.cmdargtoken[6].val.numf, data.cmdargtoken[7].val.numf, data.cmdargtoken[8].val.numf, data.cmdargtoken[9].val.numf, data.cmdargtoken[10].val.numf, data.cmdargtoken[11].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_Process_zrespM */
int_fast8_t AOloopControl_Process_zrespM_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,3)+CLI_checkarg(4,3)+CLI_checkarg(5,3)==0) {
        AOloopControl_Process_zrespM(LOOPNUMBER, data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.string, data.cmdargtoken[5].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_ProcessZrespM */
int_fast8_t AOloopControl_ProcessZrespM_cli() {
    if(CLI_checkarg(1,3)+CLI_checkarg(2,3)+CLI_checkarg(3,3)+CLI_checkarg(4,3)+CLI_checkarg(5,1)+CLI_checkarg(6,2)==0) {
        AOloopControl_ProcessZrespM_medianfilt(LOOPNUMBER, data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.string, data.cmdargtoken[5].val.numf, data.cmdargtoken[6].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_compute_CombinedControlMatrix */
int_fast8_t AOloopControl_compute_CombinedControlMatrix_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,4)+CLI_checkarg(4,4)+CLI_checkarg(5,3)+CLI_checkarg(6,3)==0) {
        compute_CombinedControlMatrix(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.string, data.cmdargtoken[5].val.string, data.cmdargtoken[6].val.string);
        return 0;
    }
    else return 1;
}




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 6. REAL TIME COMPUTING ROUTINES
 *  calls CPU and GPU processing */
/* =============================================================================================== */
/* =============================================================================================== */

/** @brief CLI function for AOloopControl_WFSzpupdate_loop */
int_fast8_t AOloopControl_WFSzpupdate_loop_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,4)==0) {
        AOloopControl_WFSzpupdate_loop(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_WFSzeropoint_sum_update_loop */
int_fast8_t AOloopControl_WFSzeropoint_sum_update_loop_cli() {
    if(CLI_checkarg(1,3)+CLI_checkarg(2,2)+CLI_checkarg(3,4)+CLI_checkarg(4,4)==0) {
        AOloopControl_WFSzeropoint_sum_update_loop(LOOPNUMBER, data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_CompModes_loop */
int_fast8_t AOloopControl_CompModes_loop_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,4)+CLI_checkarg(4,4)+CLI_checkarg(5,3)==0) {
        AOloopControl_CompModes_loop(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.string, data.cmdargtoken[5].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_GPUmodecoeffs2dm_filt */
int_fast8_t AOloopControl_GPUmodecoeffs2dm_filt_loop_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,2)+CLI_checkarg(4,4)+CLI_checkarg(5,2)+CLI_checkarg(6,2)+CLI_checkarg(7,2)==0) {
        AOloopControl_GPUmodecoeffs2dm_filt_loop(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.string, data.cmdargtoken[5].val.numl, data.cmdargtoken[6].val.numl, data.cmdargtoken[7].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_computeWFSresidualimage */
int_fast8_t AOloopControl_computeWFSresidualimage_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,1)==0) {
        AOloopControl_computeWFSresidualimage(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_ComputeOpenLoopModes */
int_fast8_t AOloopControl_ComputeOpenLoopModes_cli() {
    if(CLI_checkarg(1,2)==0) {
        AOloopControl_ComputeOpenLoopModes(data.cmdargtoken[1].val.numl);
        return 0;
    } else return 1;
}

/** @brief CLI function for AOloopControl_AutoTuneGains */
int_fast8_t AOloopControl_AutoTuneGains_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,3)==0) {
        AOloopControl_AutoTuneGains(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_dm2dm_offload */
int_fast8_t AOloopControl_dm2dm_offload_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,1)+CLI_checkarg(4,1)+CLI_checkarg(5,1)==0) {
        AOloopControl_dm2dm_offload(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.numf, data.cmdargtoken[4].val.numf, data.cmdargtoken[5].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_sig2Modecoeff */
int_fast8_t AOloopControl_sig2Modecoeff_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,4)+CLI_checkarg(4,3)==0) {
        AOloopControl_sig2Modecoeff(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string,  data.cmdargtoken[4].val.string);
        return 0;
    } else return 1;
}





/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 7. PREDICTIVE CONTROL
 *  Predictive control using WFS telemetry */
/* =============================================================================================== */
/* =============================================================================================== */

/** @brief CLI function for AOloopControl_builPFloop_WatchInput */
int_fast8_t AOloopControl_builPFloop_WatchInput_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,2)==0) {
        AOloopControl_builPFloop_WatchInput(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numl);
        return 0;
    } else return 1;
}

/** @brief CLI function for AOloopControl_mapPredictiveFilter */
int_fast8_t AOloopControl_mapPredictiveFilter_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,2)+CLI_checkarg(3,1)==0) {
        AOloopControl_mapPredictiveFilter(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_testPredictiveFilter */
int_fast8_t AOloopControl_testPredictiveFilter_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,2)+CLI_checkarg(3,1)+CLI_checkarg(4,2)+CLI_checkarg(5,3)==0) {
        AOloopControl_testPredictiveFilter(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numf, data.cmdargtoken[4].val.numl, data.cmdargtoken[5].val.string, 1e-10);
        return 0;
    }
    else return 1;
}





/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 8.   LOOP CONTROL INTERFACE */
/* =============================================================================================== */
/* =============================================================================================== */


/** @brief CLI function for AOloopControl_setLoopNumber */
int_fast8_t AOloopControl_setLoopNumber_cli() {
    if(CLI_checkarg(1,2)==0) {
        AOloopControl_setLoopNumber(data.cmdargtoken[1].val.numl);
        return 0;
    }
    else return 1;
}



/* =============================================================================================== */
/** @name AOloopControl - 8.1. LOOP CONTROL INTERFACE - MAIN CONTROL : LOOP ON/OFF START/STOP/STEP/RESET */
/* =============================================================================================== */


/* =============================================================================================== */
/** @name AOloopControl - 8.2. LOOP CONTROL INTERFACE - DATA LOGGING                               */
/* =============================================================================================== */


/* =============================================================================================== */
/** @name AOloopControl - 8.3. LOOP CONTROL INTERFACE - PRIMARY DM WRITE                           */
/* =============================================================================================== */

/* =============================================================================================== */
/** @name AOloopControl - 8.4. LOOP CONTROL INTERFACE - INTEGRATOR AUTO TUNING                     */
/* =============================================================================================== */


/* =============================================================================================== */
/** @name AOloopControl - 8.5. LOOP CONTROL INTERFACE - PREDICTIVE FILTER ON/OFF                   */
/* =============================================================================================== */


/* =============================================================================================== */
/** @name AOloopControl - 8.6. LOOP CONTROL INTERFACE - TIMING PARAMETERS                          */
/* =============================================================================================== */



/* =============================================================================================== */
/** @name AOloopControl - 8.7. LOOP CONTROL INTERFACE - CONTROL LOOP PARAMETERS                    */
/* =============================================================================================== */



/** @brief CLI function for AOloopControl_set_modeblock_gain */
int_fast8_t AOloopControl_set_modeblock_gain_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,1)+CLI_checkarg(3,2)==0) {
        AOloopControl_set_modeblock_gain(LOOPNUMBER, data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numf, data.cmdargtoken[3].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_loopstep */
int_fast8_t AOloopControl_loopstep_cli() {
    if(CLI_checkarg(1,2)==0) {
        AOloopControl_loopstep(LOOPNUMBER, data.cmdargtoken[1].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_set_loopfrequ */
int_fast8_t AOloopControl_set_loopfrequ_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_set_loopfrequ(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_set_hardwlatency_frame */
int_fast8_t AOloopControl_set_hardwlatency_frame_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_set_hardwlatency_frame(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_set_complatency_frame */
int_fast8_t AOloopControl_set_complatency_frame_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_set_complatency_frame(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_set_wfsmextrlatency_frame */
int_fast8_t AOloopControl_set_wfsmextrlatency_frame_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_set_wfsmextrlatency_frame(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_set_AUTOTUNE_LIMITS_delta */
int_fast8_t AOloopControl_set_AUTOTUNE_LIMITS_delta_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_set_AUTOTUNE_LIMITS_delta(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_set_AUTOTUNE_LIMITS_perc */
int_fast8_t AOloopControl_set_AUTOTUNE_LIMITS_perc_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_set_AUTOTUNE_LIMITS_perc(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_set_AUTOTUNE_LIMITS_mcoeff */
int_fast8_t AOloopControl_set_AUTOTUNE_LIMITS_mcoeff_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_set_AUTOTUNE_LIMITS_mcoeff(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setgain */
int_fast8_t AOloopControl_setgain_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_setgain(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setARPFgain */
int_fast8_t AOloopControl_setARPFgain_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_setARPFgain(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setWFSnormfloor */
int_fast8_t AOloopControl_setWFSnormfloor_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_setWFSnormfloor(data.cmdargtoken[1].val.numf);
        return 0;
    } else return 1;
}

/** @brief CLI function for AOloopControl_setmaxlimit */
int_fast8_t AOloopControl_setmaxlimit_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_setmaxlimit(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setmult */
int_fast8_t AOloopControl_setmult_cli() {
    if(CLI_checkarg(1,1)==0) {
        AOloopControl_setmult(data.cmdargtoken[1].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setframesAve */
int_fast8_t AOloopControl_setframesAve_cli() {
    if(CLI_checkarg(1,2)==0) {
        AOloopControl_setframesAve(data.cmdargtoken[1].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setgainrange */
int_fast8_t AOloopControl_setgainrange_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,2)+CLI_checkarg(3,1)==0) {
        AOloopControl_setgainrange(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setlimitrange */
int_fast8_t AOloopControl_setlimitrange_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,2)+CLI_checkarg(3,1)==0) {
        AOloopControl_setlimitrange(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setmultfrange */
int_fast8_t AOloopControl_setmultfrange_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,2)+CLI_checkarg(3,1)==0) {
        AOloopControl_setmultfrange(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setgainblock */
int_fast8_t AOloopControl_setgainblock_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,1)==0) {
        AOloopControl_setgainblock(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setlimitblock */
int_fast8_t AOloopControl_setlimitblock_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,1)==0) {
        AOloopControl_setlimitblock(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_setmultfblock */
int_fast8_t AOloopControl_setmultfblock_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,1)==0) {
        AOloopControl_setmultfblock(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_scanGainBlock */
int_fast8_t AOloopControl_scanGainBlock_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,2)+CLI_checkarg(3,1)+CLI_checkarg(4,1)+CLI_checkarg(5,2)==0) {
        AOloopControl_scanGainBlock(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numf, data.cmdargtoken[4].val.numf, data.cmdargtoken[5].val.numl);
        return 0;
    }
    else    return 1;
}




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 9. STATUS / TESTING / PERF MEASUREMENT                                   */
/* =============================================================================================== */
/* =============================================================================================== */



/** @brief CLI function for AOcontrolLoop_TestDMSpeed */
int_fast8_t AOcontrolLoop_TestDMSpeed_cli()
{
    if(CLI_checkarg(1,4)+CLI_checkarg(2,2)+CLI_checkarg(3,2)+CLI_checkarg(4,1)==0) {
        AOcontrolLoop_TestDMSpeed( data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numl, data.cmdargtoken[4].val.numf);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOcontrolLoop_TestSystemLatency */
int_fast8_t AOcontrolLoop_TestSystemLatency_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,1)+CLI_checkarg(4,2)==0) {
        AOcontrolLoop_TestSystemLatency(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.numf, data.cmdargtoken[4].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_TestDMmodeResp */
int_fast8_t AOloopControl_TestDMmodeResp_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,2)+CLI_checkarg(3,1)+CLI_checkarg(4,1)+CLI_checkarg(5,1)+CLI_checkarg(6,1)+CLI_checkarg(7,1)+CLI_checkarg(8,2)+CLI_checkarg(9,4)+CLI_checkarg(10,4)+CLI_checkarg(11,4)+CLI_checkarg(12,3)==0) {
        AOloopControl_TestDMmodeResp(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numf, data.cmdargtoken[4].val.numf, data.cmdargtoken[5].val.numf, data.cmdargtoken[6].val.numf, data.cmdargtoken[7].val.numf, data.cmdargtoken[8].val.numl, data.cmdargtoken[9].val.string, data.cmdargtoken[10].val.string, data.cmdargtoken[11].val.string, data.cmdargtoken[12].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_TestDMmodes_Recovery */
int_fast8_t AOloopControl_TestDMmodes_Recovery_cli() {
    if(CLI_checkarg(1,4)+CLI_checkarg(2,1)+CLI_checkarg(3,4)+CLI_checkarg(4,4)+CLI_checkarg(5,4)+CLI_checkarg(6,4)+CLI_checkarg(7,1)+CLI_checkarg(8,2)+CLI_checkarg(9,3)+CLI_checkarg(10,3)+CLI_checkarg(11,3)+CLI_checkarg(12,3)==0) {
        AOloopControl_TestDMmodes_Recovery(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numf, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.string, data.cmdargtoken[5].val.string, data.cmdargtoken[6].val.string, data.cmdargtoken[7].val.numf, data.cmdargtoken[8].val.numl, data.cmdargtoken[9].val.string, data.cmdargtoken[10].val.string, data.cmdargtoken[11].val.string, data.cmdargtoken[12].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_blockstats */
int_fast8_t AOloopControl_blockstats_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,5)==0) {
        AOloopControl_blockstats(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.string);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_InjectMode */
int_fast8_t AOloopControl_InjectMode_cli() {
    if(CLI_checkarg(1,2)+CLI_checkarg(2,1)==0)    {
        AOloopControl_InjectMode(data.cmdargtoken[1].val.numl, data.cmdargtoken[2].val.numf);
        return 0;
    }
    else    return 1;
}

/** @brief CLI function for AOloopControl_loopMonitor */
int_fast8_t AOloopControl_loopMonitor_cli() {
    if(CLI_checkarg(1,1)+CLI_checkarg(2,2)==0) {
        AOloopControl_loopMonitor(LOOPNUMBER, data.cmdargtoken[1].val.numf, data.cmdargtoken[2].val.numl);
        return 0;
    } else {
        AOloopControl_loopMonitor(LOOPNUMBER, 1.0, 8);
        return 0;
    }
}

/** @brief CLI function for AOloopControl_statusStats */
int_fast8_t AOloopControl_statusStats_cli() {
    if(CLI_checkarg(1,2)==0) {
        AOloopControl_statusStats(data.cmdargtoken[1].val.numl);
        return 0;
    }
    else return 1;
}

/** @brief CLI function for AOloopControl_mkTestDynamicModeSeq */
int_fast8_t AOloopControl_mkTestDynamicModeSeq_cli()
{
    if(CLI_checkarg(1,3)+CLI_checkarg(2,2)+CLI_checkarg(3,2)==0) {
        AOloopControl_mkTestDynamicModeSeq(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numl, data.cmdargtoken[3].val.numl);
        return 0;
    }
    else  return 1;
}

/** @brief CLI function for AOloopControl_AnalyzeRM_sensitivity */
int_fast8_t AOloopControl_AnalyzeRM_sensitivity_cli()
{
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,4)+CLI_checkarg(4,4)+CLI_checkarg(4,4)+CLI_checkarg(5,4)+CLI_checkarg(6,1)+CLI_checkarg(7,1)+CLI_checkarg(8,3)==0)    {
        AOloopControl_AnalyzeRM_sensitivity(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.string, data.cmdargtoken[5].val.string, data.cmdargtoken[6].val.numf, data.cmdargtoken[7].val.numf, data.cmdargtoken[8].val.string);
        return 0;
    }
    else        return 1;
}



/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 10. FOCAL PLANE SPECKLE MODULATION / CONTROL                             */
/* =============================================================================================== */
/* =============================================================================================== */

/** @brief CLI function for AOloopControl_DMmodulateAB */
int_fast8_t AOloopControl_DMmodulateAB_cli()
{
    if(CLI_checkarg(1,4)+CLI_checkarg(2,4)+CLI_checkarg(3,4)+CLI_checkarg(4,4)+CLI_checkarg(5,4)+CLI_checkarg(6,1)+CLI_checkarg(7,2)==0)   {
        AOloopControl_DMmodulateAB(data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.string, data.cmdargtoken[3].val.string, data.cmdargtoken[4].val.string, data.cmdargtoken[5].val.string, data.cmdargtoken[6].val.numf, data.cmdargtoken[7].val.numl);
        return 0;
    }
    else        return 1;
}



/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 11. PROCESS LOG FILES                                                    */
/* =============================================================================================== */
/* =============================================================================================== */

/** @brief CLI function for AOloopControl_logprocess_modeval */
int_fast8_t AOloopControl_logprocess_modeval_cli() {
    if(CLI_checkarg(1,4)==0) {
        AOloopControl_logprocess_modeval(data.cmdargtoken[1].val.string);
        return 0;
    }
    else return 1;
}








































// 1: float
// 2: long
// 3: string, not existing image
// 4: existing image
// 5: string


























// OBSOLETE ??



int_fast8_t AOloopControl_setparam_cli()
{
    if(CLI_checkarg(1,3)+CLI_checkarg(2,1)==0)
    {
        AOloopControl_setparam(LOOPNUMBER, data.cmdargtoken[1].val.string, data.cmdargtoken[2].val.numf);
        return 0;
    }
    else
        return 1;
}











/* =============================================================================================== */
/* =============================================================================================== */
/*                                    FUNCTIONS SOURCE CODE                                        */
/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl functions */







/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 1. INITIALIZATION, configurations                                        */
/* =============================================================================================== */
/* =============================================================================================== */



int_fast8_t init_AOloopControl()
{
    FILE *fp;

#ifdef AOLOOPCONTROL_LOGFUNC
    CORE_logFunctionCall( 0, __FUNCTION__, __LINE__, "");
#endif


    if((fp=fopen("LOOPNUMBER","r"))!=NULL)
    {
        if(fscanf(fp,"%8ld", &LOOPNUMBER) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read LOOPNUMBER");

        printf("LOOP NUMBER = %ld\n", LOOPNUMBER);
        fclose(fp);
    }
    else
        LOOPNUMBER = 0;


    strcpy(data.module[data.NBmodule].name, __FILE__);
    strcpy(data.module[data.NBmodule].info, "AO loop control");
    data.NBmodule++;



    RegisterCLIcommand("aolloadconf",__FILE__, AOloopControl_loadconfigure_cli, "load AO loop configuration", "<loop #>", "AOlooploadconf 1", "int AOloopControl_loadconfigure(long loopnb, 1, 10)");




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 2. LOW LEVEL UTILITIES & TOOLS                                           */
/* =============================================================================================== */
/* =============================================================================================== */


/* =============================================================================================== */
/** @name AOloopControl - 2.1. LOW LEVEL UTILITIES & TOOLS - LOAD DATA STREAMS                     */
/* =============================================================================================== */

/* =============================================================================================== */
/** @name AOloopControl - 2.2. LOW LEVEL UTILITIES & TOOLS - DATA STREAMS PROCESSING               */
/* =============================================================================================== */

/*    RegisterCLIcommand("aveACshmim", __FILE__, AOloopControl_AveStream_cli, "average and AC shared mem image", "<input image> <coeff> <output image ave> <output AC> <output RMS>" , "aveACshmim imin 0.01 outave outAC outRMS", "int AOloopControl_AveStream(char *IDname, double alpha, char *IDname_out_ave, char *IDname_out_AC, char *IDname_out_RMS)");

    RegisterCLIcommand("aolstream3Dto2D", __FILE__, AOloopControl_stream3Dto2D_cli, "remaps 3D cube into 2D image", "<input 3D stream> <output 2D stream> <# cols> <sem trigger>" , "aolstream3Dto2D in3dim out2dim 4 1", "long AOloopControl_stream3Dto2D(const char *in_name, const char *out_name, int NBcols, int insem)");
*/


/* =============================================================================================== */
/** @name AOloopControl - 2.3. LOW LEVEL UTILITIES & TOOLS - MISC COMPUTATION ROUTINES             */
/* =============================================================================================== */

    RegisterCLIcommand("aolcrossp", __FILE__, AOloopControl_CrossProduct_cli, "compute cross product between two cubes. Apply mask if image xpmask exists", "<cube1> <cube2> <output image>", "aolcrossp imc0 imc1 crosspout", "AOloopControl_CrossProduct(char *ID1_name, char *ID1_name, char *IDout_name)");

    RegisterCLIcommand("aolmksimplezpM", __FILE__, AOloopControl_mkSimpleZpokeM_cli, "make simple poke sequence", "<dmsizex> <dmsizey> <output image>", "aolmksimplezpM 50 50 pokeM", "long AOloopControl_mkSimpleZpokeM( long dmxsize, long dmysize, char *IDout_name)");




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 3. WFS INPUT                                                             */
/* =============================================================================================== */
/* =============================================================================================== */

 

    RegisterCLIcommand("cropshim", __FILE__, AOloopControl_camimage_extract2D_sharedmem_loop_cli, "crop shared mem image", "<input image> <optional dark> <output image> <sizex> <sizey> <xstart> <ystart>" , "cropshim imin null imout 32 32 153 201", "int AOloopControl_camimage_extract2D_sharedmem_loop(char *in_name, const char *dark_name, char *out_name, long size_x, long size_y, long xstart, long ystart)");




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 4. ACQUIRING CALIBRATION                                                 */
/* =============================================================================================== */
/* =============================================================================================== */
  

    RegisterCLIcommand("aolacqresp", __FILE__, AOloopControl_Measure_Resp_Matrix_cli, "acquire AO response matrix and WFS reference", "<ave# [long]> <ampl [float]> <nbloop [long]> <frameDelay [long]> <NBiter [long]>", "aolacqresp 50 0.1 5 2", "int Measure_Resp_Matrix(long loop, long NbAve, float amp, long nbloop, long fDelay, long NBiter)");

    RegisterCLIcommand("aolmRMfast", __FILE__, AOloopControl_RespMatrix_Fast_cli, "acquire fast modal response matrix", "<modes> <dm RM stream> <WFS stream> <sem trigger> <hardware latency [s]> <loop frequ [Hz]> <ampl [um]> <outname>", "aolmRMfast DMmodes aol0_dmRM aol0_wfsim 4 0.00112 2000.0 0.03 rm000", "long AOloopControl_RespMatrix_Fast(char *DMmodes_name, char *dmRM_name, char *imWFS_name, long semtrig, float HardwareLag, float loopfrequ, float ampl, char *outname)");

    RegisterCLIcommand("aolmeasWFSrespC",__FILE__, AOloopControl_Measure_WFSrespC_cli, "measure WFS resp to DM patterns", "<delay frames [long]> <DMcommand delay us [long]> <nb frames per position [long]> <nb frames excluded [long]> <input DM patter cube [string]> <output response [string]> <normalize flag> <AOinitMode> <NBcycle>", "aolmeasWFSrespC 2 135 20 0 dmmodes wfsresp 1 0 5", "long AOloopControl_Measure_WFSrespC(long loop, long delayfr, long delayRM1us, long NBave, long NBexcl, char *IDpokeC_name, char *IDoutC_name, int normalize, int AOinitMode, long NBcycle);");

    RegisterCLIcommand("aolmeaslWFSrespC",__FILE__, AOloopControl_Measure_WFS_linResponse_cli, "measure linear WFS response to DM patterns", "<ampl [um]> <delay frames [long]> <DMcommand delay us [long]> <nb frames per position [long]> <nb frames excluded [long]> <input DM patter cube [string]> <output response [string]> <output reference [string]> <normalize flag> <AOinitMode> <NBcycle>", "aolmeasWFSrespC 0.05 2 135 20 0 dmmodes wfsresp wfsref 1 0 5", "long AOloopControl_Measure_WFS_linResponse(long loop, float ampl, long delayfr, long delayRM1us, long NBave, long NBexcl, char *IDpokeC_name, char *IDrespC_name, char *IDwfsref_name, int normalize, int AOinitMode, long NBcycle)");

    RegisterCLIcommand("aolmeaszrm",__FILE__, AOloopControl_Measure_zonalRM_cli, "measure zonal resp mat, WFS ref, DM and WFS response maps", "<ampl [float]> <delay frames [long]> <DMcommand delay us [long]> <nb frames per position [long]> <nb frames excluded [long]> <output image [string]> <output WFS ref [string]>  <output WFS response map [string]>  <output DM response map [string]> <mode> <normalize flag> <AOinitMode> <NBcycle>", "aolmeaszrm 0.05 2 135 20 zrm wfsref wfsmap dmmap 1 0 0 0", "long AOloopControl_Measure_zonalRM(long loop, double ampl, long delayfr, long delayRM1us, long NBave, long NBexcl, char *zrespm_name, char *WFSref_name, char *WFSmap_name, char *DMmap_name, long mode, int normalize, int AOinitMode, long NBcycle)");


/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 5. COMPUTING CALIBRATION                                                 */
/* =============================================================================================== */
/* =============================================================================================== */


    RegisterCLIcommand("aolmkH", __FILE__, AOloopControl_mkHadamardModes_cli, "make Hadamard poke sequence", "<DM pixel mask> <output fname [string]>", "aolmkH dm50mask h50pokec", "long AOloopControl_mkHadamardModes(char *DMmask_name, char outname)");

    RegisterCLIcommand("aolHaddec", __FILE__, AOloopControl_Hadamard_decodeRM_cli, "decode Hadamard matrix", "<input RM> <Hadamard matrix> <DMpix index frame> <output RM>", "aolHaddec imRMh Hmat pixiind imRM", "long AOloopControl_Hadamard_decodeRM(char *inname, char *Hmatname, char *indexname, char *outname)");

    RegisterCLIcommand("aolmkslact",__FILE__, AOloopControl_mkSlavedAct_cli, "create slaved actuators map based on proximity", "<maskRM> <distance> <outslact>", "aolmkslact DMmaskRM 2.5 DMslavedact", "long AOloopControl_mkSlavedAct(char *IDmaskRM_name, float pixrad, char *IDout_name)");

    RegisterCLIcommand("aolmklodmmodes",__FILE__, AOloopControl_mkloDMmodes_cli, "make low order DM modes", "<output modes> <sizex> <sizey> <max CPA> <delta CPA> <cx> <cy> <r0> <r1> <masking mode>", "aolmklodmmodes modes 50 50 5.0 0.8 1", "long AOloopControl_mkloDMmodes(char *ID_name, long msizex, long msizey, float CPAmax, float deltaCPA, double xc, double yx, double r0, double r1, int MaskMode)");

    RegisterCLIcommand("aolRM2CM",__FILE__, AOloopControl_mkCM_cli, "make control matrix from response matrix", "<RMimage> <CMimage> <SVDlim>", "aolRM2CM respM contrM 0.1", "long AOloopControl_mkCM(char *respm_name, char *cm_name, float SVDlim)");

    RegisterCLIcommand("aolmkmodes", __FILE__, AOloopControl_mkModes_cli, "make control modes", "<output modes> <sizex> <sizey> <max CPA> <delta CPA> <cx> <cy> <r0> <r1> <masking mode> <block> <SVDlim>", "aolmkmodes modes 50 50 5.0 0.8 1 2 0.01", "long AOloopControl_mkModes(char *ID_name, long msizex, long msizey, float CPAmax, float deltaCPA, double xc, double yx, double r0, double r1, int MaskMode, int BlockNB, float SVDlim)");

    RegisterCLIcommand("aolmkmodesM", __FILE__, AOloopControl_mkModes_Simple_cli, "make control modes in modal DM mode", "<input WFS modes> <NBmblock> <block> <SVDlim>", "aolmkmodesM wfsallmodes 5 2 0.01", "long AOloopControl_mkModes_Simple(char *IDin_name, long NBmblock, long Cmblock, float SVDlim)");

    RegisterCLIcommand("aolRMmkmasks",__FILE__, AOloopControl_mkCalib_map_mask_cli, "make sensitivity maps and masks from response matrix", "<zrespm fname [string]> <output WFS response map fname [string]>  <output DM response map fname [string]> <percentile low> <coefficient low> <percentile high> <coefficient high>", "aolRMmkmasks .. 0.2 1.0 0.5 0.3 0.05 1.0 0.65 0.3", "int AOloopControl_mkCalib_map_mask(long loop, char *zrespm_name, char *WFSmap_name, char *DMmap_name, float dmmask_perclow, float dmmask_coefflow, float dmmask_perchigh, float dmmask_coeffhigh, float wfsmask_perclow, float wfsmask_coefflow, float wfsmask_perchigh, float wfsmask_coeffhigh)");

    RegisterCLIcommand("aolproczrm",__FILE__, AOloopControl_Process_zrespM_cli, "process zonal resp mat, WFS ref -> DM and WFS response maps", "<input zrespm fname [string]> <input WFS ref fname [string]> <output zrespm [string]> <output WFS response map fname [string]>  <output DM response map fname [string]>", "aolproczrm zrespmat0 wfsref0 zrespm wfsmap dmmap", "int AOloopControl_Process_zrespM(long loop, char *IDzrespm0_name, char *IDwfsref_name, char *IDzrespm_name, char *WFSmap_name, char *DMmap_name)");

    RegisterCLIcommand("aolcleanzrm",__FILE__, AOloopControl_ProcessZrespM_cli, "clean zonal resp mat, WFS ref, DM and WFS response maps", "<zrespm fname [string]> <output WFS ref fname [string]>  <output WFS response map fname [string]>  <output DM response map fname [string]> <RM ampl [um]>", "aolcleanzrm zrm wfsref wfsmap dmmap 0.05", "int AOloopControl_ProcessZrespM(long loop, char *zrespm_name, char *WFSref0_name, char *WFSmap_name, char *DMmap_name, double ampl)");

    RegisterCLIcommand("aolcompcmatc",__FILE__, AOloopControl_compute_CombinedControlMatrix_cli, "compute combined control matrix", "<modal control matrix> <modes> <wfs mask> <dm mask> <combined cmat> <combined cmat, only active elements>", "aolcompcmatc cmat fmodes wfsmask dmmask cmatc cmatcact", "long compute_CombinedControlMatrix(char *IDcmat_name, char *IDmodes_name, char* IDwfsmask_name, char *IDdmmask_name, char *IDcmatc_name, char *IDcmatc_active_name)");

    RegisterCLIcommand("aolcmmake",__FILE__, AOloopControl_computeCM_cli, "make control matrix", "<NBmodes removed> <RespMatrix> <ContrMatrix> <beta> <nbremovedstep> <eigenvlim>", "aolcmmake 8 respm cmat", "int compute_ControlMatrix(long loop, long NB_MODE_REMOVED, char *ID_Rmatrix_name, char *ID_Cmatrix_name, char *ID_VTmatrix_name, double Beta, long NB_MODE_REMOVED_STEP, float eigenvlim)");



/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 6. REAL TIME COMPUTING ROUTINES                                          */
/* =============================================================================================== */
/* =============================================================================================== */
    

    RegisterCLIcommand("aolrun", __FILE__, AOloopControl_run, "run AO loop", "no arg", "aolrun", "int AOloopControl_run()");

    RegisterCLIcommand("aolzpwfsloop",__FILE__, AOloopControl_WFSzpupdate_loop_cli, "WFS zero point offset loop", "<dm offset [shared mem]> <zonal resp M [shared mem]> <nominal WFS reference>  <modified WFS reference>", "aolzpwfsloop dmZP zrespM wfszp", "int AOloopControl_WFSzpupdate_loop(char *IDzpdm_name, char *IDzrespM_name, char *IDwfszp_name)");

    RegisterCLIcommand("aolzpwfscloop", __FILE__, AOloopControl_WFSzeropoint_sum_update_loop_cli, "WFS zero point offset loop: combine multiple input channels", "<name prefix> <number of channels> <wfsref0> <wfsref>", "aolzpwfscloop wfs2zpoffset 4 wfsref0 wfsref", "int AOloopControl_WFSzeropoint_sum_update_loop(long loopnb, char *ID_WFSzp_name, int NBzp, char *IDwfsref0_name, char *IDwfsref_name)");

    RegisterCLIcommand("aocmlrun", __FILE__, AOloopControl_CompModes_loop_cli, "run AO compute modes loop", "<CM> <wfsref> <WFS image stream> <WFS image total stream> <output stream>", "aocmlrun CM wfsref wfsim wfsimtot aomodeval", "int AOloopControl_CompModes_loop(char *ID_CM_name, char *ID_WFSref_name, char *ID_WFSim_name, char *ID_WFSimtot, char *ID_coeff_name)");

    RegisterCLIcommand("aolmc2dmfilt", __FILE__, AOloopControl_GPUmodecoeffs2dm_filt_loop_cli, "convert mode coefficients to DM map", "<mode coeffs> <DMmodes> <sem trigg number> <out> <GPUindex> <loopnb> <offloadMode>", "aolmc2dmfilt aolmodeval DMmodesC 2 dmmapc 0.2 1 2 1", "int AOloopControl_GPUmodecoeffs2dm_filt_loop(char *modecoeffs_name, char *DMmodes_name, int semTrigg, char *out_name, int GPUindex, long loop, long offloadMode)");

    RegisterCLIcommand("aolsig2mcoeff", __FILE__, AOloopControl_sig2Modecoeff_cli, "convert signals to mode coeffs", "<signal data cube> <reference> <Modes data cube> <output image>", "aolsig2mcoeff wfsdata wfsref wfsmodes outim", "long AOloopControl_sig2Modecoeff(char *WFSim_name, char *IDwfsref_name, char *WFSmodes_name, char *outname)");


/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 7. PREDICTIVE CONTROL                                                    */
/* =============================================================================================== */
/* =============================================================================================== */



    RegisterCLIcommand("aolmappfilt", __FILE__, AOloopControl_mapPredictiveFilter_cli, "map/search predictive filter", "<input coeffs> <mode number> <delay [frames]>", "aolmkapfilt coeffim 23 2.4", "long AOloopControl_mapPredictiveFilter(char *IDmodecoeff_name, long modeout, double delayfr)");

    RegisterCLIcommand("aolmkpfilt", __FILE__, AOloopControl_testPredictiveFilter_cli, "test predictive filter", "<trace im> <mode number> <delay [frames]> <filter size> <out filter name>", "aolmkpfilt traceim 23 2.4 20 filt23","long AOloopControl_testPredictiveFilter(char *IDtrace_name, long mode, double delayfr, long filtsize, char *IDfilt_name, double SVDeps)");

    RegisterCLIcommand("aolmkpfilt", __FILE__, AOloopControl_testPredictiveFilter_cli, "test predictive filter", "<trace im> <mode number> <delay [frames]> <filter size> <out filter name>", "aolmkpfilt traceim 23 2.4 20 filt23","long AOloopControl_testPredictiveFilter(char *IDtrace_name, long mode, double delayfr, long filtsize, char *IDfilt_name, double SVDeps)");



/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 8.   LOOP CONTROL INTERFACE                                              */
/* =============================================================================================== */
/* =============================================================================================== */
    
    RegisterCLIcommand("aolnb", __FILE__, AOloopControl_setLoopNumber_cli, "set AO loop #", "<loop nb>", "AOloopnb 0", "int AOloopControl_setLoopNumber(long loop)");

/* =============================================================================================== */
/** @name AOloopControl - 8.1. LOOP CONTROL INTERFACE - MAIN CONTROL : LOOP ON/OFF START/STOP/STEP/RESET */
/* =============================================================================================== */


/* =============================================================================================== */
/** @name AOloopControl - 8.2. LOOP CONTROL INTERFACE - DATA LOGGING                               */
/* =============================================================================================== */

/* =============================================================================================== */
/** @name AOloopControl - 8.3. LOOP CONTROL INTERFACE - PRIMARY DM WRITE                           */
/* =============================================================================================== */

/* =============================================================================================== */
/** @name AOloopControl - 8.4. LOOP CONTROL INTERFACE - INTEGRATOR AUTO TUNING                     */
/* =============================================================================================== */
  

/* =============================================================================================== */
/** @name AOloopControl - 8.5. LOOP CONTROL INTERFACE - PREDICTIVE FILTER ON/OFF                   */
/* =============================================================================================== */

/* =============================================================================================== */
/** @name AOloopControl - 8.6. LOOP CONTROL INTERFACE - TIMING PARAMETERS                          */
/* =============================================================================================== */

/* =============================================================================================== */
/** @name AOloopControl - 8.7. LOOP CONTROL INTERFACE - CONTROL LOOP PARAMETERS                    */
/* =============================================================================================== */

    RegisterCLIcommand("aolsetgain", __FILE__, AOloopControl_setgain_cli, "set gain", "<gain value>", "aolsetgain 0.1", "int AOloopControl_setgain(float gain)");

    RegisterCLIcommand("aolsetARPFgain", __FILE__, AOloopControl_setARPFgain_cli, "set auto-regressive predictive filter gain", "<gain value>", "aolsetARPFgain 0.1", "int AOloopControl_setARPFgain(float gain)");










    RegisterCLIcommand("aolkill", __FILE__, AOloopControl_loopkill, "kill AO loop", "no arg", "aolkill", "int AOloopControl_setLoopNumber()");

    RegisterCLIcommand("aolon", __FILE__, AOloopControl_loopon, "turn loop on", "no arg", "aolon", "int AOloopControl_loopon()");

    RegisterCLIcommand("aoloff", __FILE__, AOloopControl_loopoff, "turn loop off", "no arg", "aoloff", "int AOloopControl_loopoff()");

    RegisterCLIcommand("aolstep",__FILE__, AOloopControl_loopstep_cli, "turn loop on for N steps", "<nbstep>", "aolstep", "int AOloopControl_loopstep(long loop, long NBstep)");

    RegisterCLIcommand("aolreset", __FILE__, AOloopControl_loopreset, "reset loop, and turn it off", "no arg", "aolreset", "int AOloopControl_loopreset()");

    RegisterCLIcommand("aolsetmbgain",__FILE__, AOloopControl_set_modeblock_gain_cli, "set modal block gain", "<loop #> <gain> <compute sum flag>", "aolsetmbgain 2 0.2 1", "int AOloopControl_set_modeblock_gain(long loop, long blocknb, float gain, int add)");

    RegisterCLIcommand("aolDMprimWon", __FILE__, AOloopControl_DMprimaryWrite_on, "turn DM primary write on", "no arg", "aolDMprimWon", "int AOloopControl_DMprimaryWrite_on()");

    RegisterCLIcommand("aolDMprimWoff", __FILE__, AOloopControl_DMprimaryWrite_off, "turn DM primary write off", "no arg", "aolDMprimWoff", "int AOloopControl_DMprimaryWrite_off()");

    RegisterCLIcommand("aolAUTOTUNELIMon", __FILE__, AOloopControl_AUTOTUNE_LIMITS_on, "turn auto-tuning modal limits on", "no arg", "aolAUTOTUNELIMon", "int AOloopControl_AUTOTUNE_LIMITS_on()");

    RegisterCLIcommand("aolAUTOTUNELIMoff", __FILE__, AOloopControl_AUTOTUNE_LIMITS_off, "turn auto-tuning modal limits off", "no arg", "aolAUTOTUNELIMoff", "int AOloopControl_AUTOTUNE_LIMITS_off()");

    RegisterCLIcommand("aolsetATlimd", __FILE__, AOloopControl_set_AUTOTUNE_LIMITS_delta_cli, "set auto-tuning modal limits delta", "<delta value [um]>", "aolsetATlimd 0.0001", "int AOloopControl_set_AUTOTUNE_LIMITS_delta(float AUTOTUNE_LIMITS_delta)");

    RegisterCLIcommand("aolsetATlimp", __FILE__, AOloopControl_set_AUTOTUNE_LIMITS_perc_cli, "set auto-tuning modal limits percentile", "<percentile value [percent]>", "aolsetATlimp 1.0", "int AOloopControl_set_AUTOTUNE_LIMITS_perc(float AUTOTUNE_LIMITS_perc)");

    RegisterCLIcommand("aolsetATlimm", __FILE__, AOloopControl_set_AUTOTUNE_LIMITS_mcoeff_cli, "set auto-tuning modal limits multiplicative coeff", "<multiplicative coeff [float]>", "aolsetATlimm 1.5", "int AOloopControl_set_AUTOTUNE_LIMITS_mcoeff(float AUTOTUNE_LIMITS_mcoeff)");

    RegisterCLIcommand("aolAUTOTUNEGAINon", __FILE__, AOloopControl_AUTOTUNE_GAINS_on, "turn auto-tuning modal gains on", "no arg", "aolAUTOTUNEGAINon", "int AOloopControl_AUTOTUNE_GAINS_on()");

    RegisterCLIcommand("aolAUTOTUNEGAINoff", __FILE__, AOloopControl_AUTOTUNE_GAINS_off, "turn auto-tuning modal gains off", "no arg", "aolAUTOTUNEGAINoff", "int AOloopControl_AUTOTUNE_GAINS_off()");

    RegisterCLIcommand("aolARPFon", __FILE__, AOloopControl_ARPFon, "turn auto-regressive predictive filter on", "no arg", "aolARPFon", "int AOloopControl_ARPFon()");

    RegisterCLIcommand("aolARPFoff", __FILE__, AOloopControl_ARPFoff, "turn auto-regressive predictive filter off", "no arg", "aolARPFoff", "int AOloopControl_ARPFoff()");



/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 9. STATUS / TESTING / PERF MEASUREMENT                                   */
/* =============================================================================================== */
/* =============================================================================================== */

    RegisterCLIcommand("aoldmtestsp", __FILE__, AOcontrolLoop_TestDMSpeed_cli, "test DM speed by sending circular tip-tilt", "<dmname> <delay us [long]> <NB pts> <ampl>", "aoldmtestsp dmdisp2 100 20 0.1", "long AOcontrolLoop_TestDMSpeed(char *dmname, long delayus, long NBpts, float ampl)");

    RegisterCLIcommand("aoltestlat", __FILE__, AOcontrolLoop_TestSystemLatency_cli, "test system latency", "<dm stream> <wfs stream> <ampl [um]> <NBiter>", "aoltestlat dmC wfsim 0.1 5000", "long AOcontrolLoop_TestSystemLatency(char *dmname, char *wfsname, float OPDamp, long NBiter)");

    RegisterCLIcommand("aoltestmresp", __FILE__, AOloopControl_TestDMmodeResp_cli, "Measure system response for a single mode", "<DM modes [3D im]> <mode #> <ampl [um]> <fmin [Hz]> <fmax [Hz]> <fstep> <meas. time [sec]> <time step [us]> <DM mask> <DM in [2D stream]> <DM out [2D stream]>  <output [2D im]>", "aoltestmresp DMmodesC 5 0.05 10.0 100.0 1.2 1.0 1000 dmmask dmdisp3 dmC out", "long AOloopControl_TestDMmodeResp(char *DMmodes_name, long index, float ampl, float fmin, float fmax, float fmultstep, float avetime, long dtus, char *DMmask_name, char *DMstream_in_name, char *DMstream_out_name, char *IDout_name)");

    RegisterCLIcommand("aoltestdmrec", __FILE__, AOloopControl_TestDMmodes_Recovery_cli, "Test system DM modes recovery", "<DM modes [3D im]> <ampl [um]> <DM mask [2D im]> <DM in [2D stream]> <DM out [2D stream]> <meas out [2D stream]> <lag time [us]>  <NB averages [long]>  <out ave [2D im]> <out rms [2D im]> <out meas ave [2D im]> <out meas rms [2D im]>", "aoltestdmrec DMmodesC 0.05 DMmask dmsisp2 dmoutr 2000  20 outave outrms outmave outmrms", "long AOloopControl_TestDMmodes_Recovery(char *DMmodes_name, float ampl, char *DMmask_name, char *DMstream_in_name, char *DMstream_out_name, char *DMstream_meas_name, long tlagus, long NBave, char *IDout_name, char *IDoutrms_name, char *IDoutmeas_name, char *IDoutmeasrms_name)");



/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 10. FOCAL PLANE SPECKLE MODULATION / CONTROL                             */
/* =============================================================================================== */
/* =============================================================================================== */




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 11. PROCESS LOG FILES                                                    */
/* =============================================================================================== */
/* =============================================================================================== */




























    RegisterCLIcommand("aolsetloopfrequ",
                       __FILE__,
                       AOloopControl_set_loopfrequ_cli,
                       "set loop frequency",
                       "<loop frequ [Hz]>",
                       "aolsetloopfrequ 2000",
                       "int AOloopControl_set_loopfrequ(float loopfrequ)");



    RegisterCLIcommand("aolsethlat", __FILE__, AOloopControl_set_hardwlatency_frame_cli, "set hardware latency", "<hardware latency [frame]>", "aolsethlat 2.7", "int AOloopControl_set_hardwlatency_frame(float hardwlatency_frame)");

    RegisterCLIcommand("aolsetclat",__FILE__, AOloopControl_set_complatency_frame_cli,"set computation latency", "<computation latency [frame]>", "aolsetclat 0.6", "int AOloopControl_set_complatency_frame(float complatency_frame)");

    RegisterCLIcommand("aolsetwlat", __FILE__, AOloopControl_set_wfsmextrlatency_frame_cli, "set WFS mode extraction latency", "<latency [frame]>", "aolsetwlat 0.8", "int AOloopControl_set_wfsmextrlatency_frame(float wfsmextrlatency_frame)");

    RegisterCLIcommand("aolsetwfsnormf", __FILE__, AOloopControl_setWFSnormfloor_cli, "set WFS normalization floor", "<floor value (total flux)>", "aolsetwfsnormf 10000.0", "int AOloopControl_setWFSnormfloor(float WFSnormfloor)");

    RegisterCLIcommand("aolsetmaxlim", __FILE__, AOloopControl_setmaxlimit_cli, "set max limit for AO mode correction", "<limit value>", "aolsetmaxlim 0.01", "int AOloopControl_setmaxlimit(float maxlimit)");

    RegisterCLIcommand("aolsetmult", __FILE__, AOloopControl_setmult_cli, "set mult coeff for AO mode correction", "<mult value>", "aolsetmult 0.98", "int AOloopControl_setmult(float multcoeff)");

    RegisterCLIcommand("aolsetnbfr",__FILE__, AOloopControl_setframesAve_cli, "set number of frames to be averaged", "<nb frames>", "aolsetnbfr 10", "int AOloopControl_setframesAve(long nbframes)");













    RegisterCLIcommand("aollogprocmodeval",__FILE__, AOloopControl_logprocess_modeval_cli, "process log image modeval", "<modeval image>", "aollogprocmodeval imc", "int AOloopControl_logprocess_modeval(const char *IDname);");










    RegisterCLIcommand("aolloadcm", __FILE__, AOloopControl_loadCM_cli, "load new control matrix from file", "<fname>", "aolloadcm cm32.fits", "long AOloopControl_loadCM(long loop, const char *CMfname)");

    RegisterCLIcommand("aolmon", __FILE__, AOloopControl_loopMonitor_cli, "monitor loop", "<frequ> <Nbcols>", "aolmon 10.0 3", "int AOloopControl_loopMonitor(long loop, double frequ)");

    RegisterCLIcommand("aolsetgainr", __FILE__, AOloopControl_setgainrange_cli, "set modal gains from m0 to m1 included", "<modemin [long]> <modemax [long]> <gainval>", "aolsetgainr 20 30 0.2", "int AOloopControl_setgainrange(long m0, long m1, float gainval)");

    RegisterCLIcommand("aolsetlimitr",__FILE__, AOloopControl_setlimitrange_cli, "set modal limits", "<modemin [long]> <modemax [long]> <limval>", "aolsetlimitr 20 30 0.02", "int AOloopControl_setlimitrange(long m0, long m1, float gainval)");

    RegisterCLIcommand("aolsetmultfr", __FILE__, AOloopControl_setmultfrange_cli, "set modal multf", "<modemin [long]> <modemax [long]> <multfval>", "aolsetmultfr 10 30 0.98", "int AOloopControl_setmultfrange(long m0, long m1, float multfval)");

    RegisterCLIcommand("aolsetgainb", __FILE__, AOloopControl_setgainblock_cli, "set modal gains by block", "<block [long]> <gainval>", "aolsetgainb 2 0.2", "int AOloopControl_setgainblock(long m0, long m1, float gainval)");

    RegisterCLIcommand("aolsetlimitb",__FILE__, AOloopControl_setlimitblock_cli, "set modal limits by block", "<block [long]> <limval>", "aolsetlimitb 2 0.02", "int AOloopControl_setlimitblock(long mb, float limitval)");

    RegisterCLIcommand("aolsetmultfb", __FILE__, AOloopControl_setmultfblock_cli, "set modal multf by block", "<block [long]> <multfval>", "aolsetmultfb 2 0.98", "int AOloopControl_setmultfblock(long mb, float multfval)");

    RegisterCLIcommand("aolresetrms", __FILE__, AOloopControl_resetRMSperf, "reset RMS performance monitor", "no arg", "aolresetrms", "int AOloopControl_resetRMSperf()");

    RegisterCLIcommand("aolscangainb", __FILE__, AOloopControl_scanGainBlock_cli, "scan gain for block", "<blockNB> <NBAOsteps> <gainstart> <gainend> <NBgainpts>", "aolscangainb", "int AOloopControl_scanGainBlock(long NBblock, long NBstep, float gainStart, float gainEnd, long NBgain)");

    RegisterCLIcommand("aolstatusstats", __FILE__, AOloopControl_statusStats_cli, "measures distribution of status values", "<update flag [int]>", "aolstatusstats 0", "int AOloopControl_statusStats(int updateconf)");

    RegisterCLIcommand("aolblockstats", __FILE__, AOloopControl_blockstats_cli, "measures mode stats per block", "<loopnb> <outim>", "aolblockstats 2 outstats", "long AOloopControl_blockstats(long loop, const char *IDout_name)");

    RegisterCLIcommand("aolmkwfsres", __FILE__, AOloopControl_computeWFSresidualimage_cli, "compute WFS residual real time", "<loopnb> <averaging coeff>", "aolmkwfsres 2 0.001", "long AOloopControl_computeWFSresidualimage(long loop, float alpha)");

    RegisterCLIcommand("aolPFwatchin",__FILE__, AOloopControl_builPFloop_WatchInput_cli, "watch telemetry for predictive filter input", "<loop #> <PFblock #>", "aolPFwatchin 0 2", "long AOloopControl_builPFloop_WatchInput(long loop, long PFblock)");

    RegisterCLIcommand("aolcompolm", __FILE__, AOloopControl_ComputeOpenLoopModes_cli, "compute open loop mode values", "<loop #>", "aolcompolm 2", "long AOloopControl_ComputeOpenLoopModes(long loop)");

    RegisterCLIcommand("aolautotunegains", __FILE__, AOloopControl_AutoTuneGains_cli, "compute optimal gains", "<loop #> <gain stream>", "aolautotunegains 0 autogain", "long AOloopControl_AutoTuneGains(long loop, const char *IDout_name)");

    RegisterCLIcommand("aoldm2dmoffload", __FILE__, AOloopControl_dm2dm_offload_cli, "slow offload from dm to dm", "<streamin> <streamout> <timestep[sec]> <offloadcoeff> <multcoeff>", "aoldm2dmoffload dmin dmout 0.5 -0.01 0.999", "long AOloopControl_dm2dm_offload(const char *streamin, const char *streamout, float twait, float offcoeff, float multcoeff)");

    RegisterCLIcommand("aolmktestmseq", __FILE__, AOloopControl_mkTestDynamicModeSeq_cli, "make modal periodic test sequence", "<outname> <number of slices> <number of modes>", "aolmktestmseq outmc 100 50", "long AOloopControl_mkTestDynamicModeSeq(const char *IDname_out, long NBpt, long NBmodes)");

    RegisterCLIcommand("aolautotune",  __FILE__, AOloopControl_AutoTune, "auto tuning of loop parameters", "no arg", "aolautotune", "int_fast8_t AOloopControl_AutoTune()");

    RegisterCLIcommand("aolset", __FILE__, AOloopControl_setparam_cli, "set parameter", "<parameter> <value>" , "aolset", "int AOloopControl_setparam(long loop, const char *key, double value)");

    RegisterCLIcommand("aoldmmodAB", __FILE__, AOloopControl_DMmodulateAB_cli, "module DM with linear combination of probes A and B", "<probeA> <probeB> <dmstream> <WFS resp mat> <WFS ref stream> <delay [sec]> <NB probes>", "aoldmmodAB probeA probeB wfsrespmat wfsref 0.1 6","int AOloopControl_DMmodulateAB(const char *IDprobeA_name, const char *IDprobeB_name, const char *IDdmstream_name, const char *IDrespmat_name, const char *IDwfsrefstream_name, double delay, long NBprobes)");

    RegisterCLIcommand("aolzrmsens", __FILE__, AOloopControl_AnalyzeRM_sensitivity_cli, "Measure zonal RM sensitivity", "<DMmodes> <DMmask> <WFSref> <WFSresp> <WFSmask> <amplitude[nm]> <lambda[nm]> <outname>", "aolzrmsens DMmodes dmmask wfsref0 zrespmat wfsmask 0.1 outfile.txt", "long AOloopControl_AnalyzeRM_sensitivity(const char *IDdmmodes_name, const char *IDdmmask_name, const char *IDwfsref_name, const char *IDwfsresp_name, const char *IDwfsmask_name, float amplimitnm, float lambdanm, const char *foutname)");







/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 12. OBSOLETE ?                                                           */ 
/* =============================================================================================== */
/* =============================================================================================== */



    RegisterCLIcommand("aolinjectmode",__FILE__, AOloopControl_InjectMode_cli, "inject single mode error into RM channel", "<index> <ampl>", "aolinjectmode 20 0.1", "int AOloopControl_InjectMode()");





    // add atexit functions here
    // atexit((void*) myfunc);

    return 0;
}



















/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 1. INITIALIZATION, configurations                                        */
/* =============================================================================================== */
/* =============================================================================================== */

/**
 * ## Purpose
 * 
 * load / setup configuration
 *
 * ## Arguments
 * 
 * @param[in]
 * loop		INT
 * 			Loop number
 * 
 * @param[in]
 * mode		INT 
 * - 1 loads from ./conf/ directory to shared memory
 * - 0 simply connects to shared memory
 * 
 * @param[in]
 * level	INT
 * - 2 zonal only
 * - 10+ load all
 * 
 * 
 * 
 * @ingroup AOloopControl_streams
 */
static int_fast8_t AOloopControl_loadconfigure(long loop, int mode, int level)
{
    FILE *fp;
    char content[201];
    char name[201];
    char fname[201];
    uint32_t *sizearray;
    int kw;
    long k;
    int r;
    int sizeOK;
    char command[501];
    int CreateSMim;
    long ii;
    long tmpl;
    char testdirname[201];

    int initwfsref;

    FILE *fplog; // human-readable log of load sequence


#ifdef AOLOOPCONTROL_LOGFUNC
	AOLOOPCONTROL_logfunc_level = 0;
    CORE_logFunctionCall( AOLOOPCONTROL_logfunc_level, AOLOOPCONTROL_logfunc_level_max, 0, __FUNCTION__, __LINE__, "");
#endif


	// Create logfile for this function
	//
    if((fplog=fopen("logdir/loadconf.log", "w"))==NULL)
    {
        printf("ERROR: cannot create logdir/loadconf.log\n");
        exit(0);
    }
    loadcreateshm_log = 1;
    loadcreateshm_fplog = fplog;

	/** --- */
	/** # Details */
	
	/** ## 1. Initial setup from configuration files */


	/** - 1.1. Initialize memory */
	fprintf(fplog, "\n\n============== 1.1. Initialize memory ===================\n\n");
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(0);



	
	//
    /** ### 1.2. Set names of key streams */
    //
    // Here we define names of key streams used by loop

	fprintf(fplog, "\n\n============== 1.2. Set names of key streams ===================\n\n");

	/** - dmC stream  : DM control */
    if(sprintf(name, "aol%ld_dmC", loop)<1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    printf("DM control file name : %s\n", name);
    strcpy(AOconf[loop].dmCname, name);

	/** - dmdisp stream : total DM displacement */
	// used to notify dm combine that a new displacement should be computed
    if(sprintf(name, "aol%ld_dmdisp", loop) < 1) 
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    printf("DM displacement file name : %s\n", name);
    strcpy(AOconf[loop].dmdispname, name);

	/** - dmRM stream : response matrix */
    if(sprintf(name, "aol%ld_dmRM", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    printf("DM RM file name : %s\n", name);
    strcpy(AOconf[loop].dmRMname, name);

	/** - wfsim : WFS image */
    if(sprintf(name, "aol%ld_wfsim", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    printf("WFS file name: %s\n", name);
    strcpy(AOconf[loop].WFSname, name);



    // Modal control

	/** - DMmodes : control modes */
    if(sprintf(name, "aol%ld_DMmodes", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    printf("DMmodes file name: %s\n", name);
    strcpy(AOconf[loop].DMmodesname, name);

	/** - respM : response matrix */
    if(sprintf(name, "aol%ld_respM", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    printf("respM file name: %s\n", name);
    strcpy(AOconf[loop].respMname, name);

	/** - contrM : control matrix */
    if(sprintf(name, "aol%ld_contrM", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    printf("contrM file name: %s\n", name);
    strcpy(AOconf[loop].contrMname, name);






    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*3);


    /** ### 1.3. Read loop name
     * 
     * - ./conf/conf_LOOPNAME.txt -> AOconf[loop].name 
     */
	fprintf(fplog, "\n\n============== 1.3. Read loop name ===================\n\n");

    if((fp=fopen("./conf/conf_LOOPNAME.txt","r"))==NULL)
    {
        printf("ERROR: file ./conf/conf_LOOPNAME.txt missing\n");
        exit(0);
    }
    if(fscanf(fp, "%200s", content) != 1)
    {
        printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");
		exit(0);
	}

    printf("loop name : %s\n", content);
    fflush(stdout);
    fprintf(fplog, "AOconf[%ld].name = %s\n", loop, AOconf[loop].name);
    fclose(fp);
    strcpy(AOconf[loop].name, content);


    /** ### 1.4. Define WFS image normalization mode 
     * 
     * - conf/param_WFSnorm.txt -> AOconf[loop].WFSnormalize
     */ 
    fprintf(fplog, "\n\n============== 1.4. Define WFS image normalization mode ===================\n\n");
    
    if((fp=fopen("./conf/param_WFSnorm.txt", "r"))==NULL)
    {
        printf("WARNING: file ./conf/param_WFSnorm.txt missing\n");
        fprintf(fplog, "WARNING: file ./conf/param_WFSnorm.txt missing. Assuming WFSnormalize = 1\n");
        AOconf[loop].WFSnormalize = 1;
    }
    else
    {
        if(fscanf(fp, "%200s", content) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("WFSnormalize : %d\n", atoi(content));
        fclose(fp);
        fflush(stdout);
        AOconf[loop].WFSnormalize = atoi(content);
        fprintf(fplog, "AOconf[%ld].WFSnormalize = %d\n", loop, AOconf[loop].WFSnormalize);
    }



    /** ### 1.5. Read Timing info
     * 
     * - ./conf/param_loopfrequ.txt    -> AOconf[loop].loopfrequ
     * - ./conf/param_hardwlatency.txt -> AOconf[loop].hardwlatency
     * - AOconf[loop].hardwlatency_frame = AOconf[loop].hardwlatency * AOconf[loop].loopfrequ
     * - ./conf/param_complatency.txt  -> AOconf[loop].complatency
     * - AOconf[loop].complatency_frame = AOconf[loop].complatency * AOconf[loop].loopfrequ;
     * - ./conf/param_wfsmextrlatency.txt -> AOconf[loop].wfsmextrlatency
     */
     fprintf(fplog, "\n\n============== 1.5. Read Timing info ===================\n\n");
     
    if((fp=fopen("./conf/param_loopfrequ.txt", "r"))==NULL)
    {
        printf("WARNING: file ./conf/param_loopfrequ.txt missing\n");
    }
    else
    {
        if(fscanf(fp, "%50f", &AOconf[loop].loopfrequ) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("loopfrequ : %f\n", AOconf[loop].loopfrequ);
        fclose(fp);
        fflush(stdout);
        fprintf(fplog, "AOconf[%ld].loopfrequ = %f\n", loop, AOconf[loop].loopfrequ);
    }



    if((fp=fopen("./conf/param_hardwlatency.txt", "r"))==NULL)
    {
        printf("WARNING: file ./conf/param_hardwlatency.txt missing\n");
    }
    else
    {
        if(fscanf(fp, "%50f", &AOconf[loop].hardwlatency) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("hardwlatency : %f\n", AOconf[loop].hardwlatency);
        fclose(fp);
        fflush(stdout);
        fprintf(fplog, "AOconf[%ld].hardwlatency = %f\n", loop, AOconf[loop].hardwlatency);
    }

    AOconf[loop].hardwlatency_frame = AOconf[loop].hardwlatency * AOconf[loop].loopfrequ;


    if((fp=fopen("./conf/param_complatency.txt", "r"))==NULL)
    {
        printf("WARNING: file ./conf/param_complatency.txt missing\n");
    }
    else
    {
        if(fscanf(fp, "%50f", &AOconf[loop].complatency) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("complatency : %f\n", AOconf[loop].complatency);
        fclose(fp);
        fflush(stdout);
        fprintf(fplog, "AOconf[%ld].complatency = %f\n", loop, AOconf[loop].complatency);
    }
    AOconf[loop].complatency_frame = AOconf[loop].complatency * AOconf[loop].loopfrequ;


    if((fp=fopen("./conf/param_wfsmextrlatency.txt", "r"))==NULL)
    {
        printf("WARNING: file ./conf/param_wfsmextrlatency.txt missing\n");
    }
    else
    {
        if(fscanf(fp, "%50f", &AOconf[loop].wfsmextrlatency) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf(" : %f\n", AOconf[loop].wfsmextrlatency);
        fclose(fp);
        fflush(stdout);
        fprintf(fplog, "AOconf[%ld].wfsmextrlatency = %f\n", loop, AOconf[loop].wfsmextrlatency);
    }
    AOconf[loop].wfsmextrlatency_frame = AOconf[loop].wfsmextrlatency * AOconf[loop].loopfrequ;




    /** ### 1.6. Define GPU use
     * 
     * - ./conf/param_GPU0.txt           > AOconf[loop].GPU0 (0 if missing)
     * - ./conf/param_GPUall.txt        -> AOconf[loop].GPUall
     * - ./conf/param_DMprimWriteON.txt -> AOconf[loop].DMprimaryWrite_ON
     * 
     */ 
	fprintf(fplog, "\n\n============== 1.6. Define GPU use ===================\n\n");
	
    if((fp=fopen("./conf/param_GPU0.txt","r"))==NULL)
    {
        printf("WARNING: file ./conf/param_GPU0.txt missing\n");
        printf("Using CPU only\n");
        fprintf(fplog, "WARNING: file ./conf/param_GPU0.txt missing. Using CPU only\n");
        AOconf[loop].GPU0 = 0;
    }
    else
    {
        if(fscanf(fp, "%200s", content) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("GPU0 : %d\n", atoi(content));
        fclose(fp);
        fflush(stdout);
        AOconf[loop].GPU0 = atoi(content);
        fprintf(fplog, "AOconf[%ld].GPU0 = %d\n", loop, AOconf[loop].GPU0);
    }

    if((fp=fopen("./conf/param_GPU1.txt","r"))==NULL)
    {
        printf("WARNING: file ./conf/param_GPU1.txt missing\n");
        printf("Using CPU only\n");
        fprintf(fplog, "WARNING: file ./conf/param_GPU1.txt missing. Using CPU only\n");
        AOconf[loop].GPU1 = 0;
    }
    else
    {
        if(fscanf(fp, "%200s", content) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("GPU1 : %d\n", atoi(content));
        fclose(fp);
        fflush(stdout);
        AOconf[loop].GPU1 = atoi(content);
        fprintf(fplog, "AOconf[%ld].GPU1 = %d\n", loop, AOconf[loop].GPU1);
    }





    // Skip CPU image scaling and go straight to GPUs ?

    if((fp=fopen("./conf/param_GPUall.txt","r"))==NULL)
    {
        printf("WARNING: file ./conf/param_GPUall.txt missing\n");
        printf("Using CPU for image scaling\n");
        fprintf(fplog, "WARNING: file ./conf/param_GPUall.txt missing. Using CPU for image scaling\n");
        AOconf[loop].GPUall = 0;
    }
    else
    {
        if(fscanf(fp, "%200s", content) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("GPUall : %d\n", atoi(content));
        fclose(fp);
        fflush(stdout);
        AOconf[loop].GPUall = atoi(content);
        fprintf(fplog, "AOconf[%ld].GPUall = %d\n", loop, AOconf[loop].GPUall);
    }

    // Direct DM write ?
    if((fp=fopen("./conf/param_DMprimWriteON.txt", "r"))==NULL)
    {
        printf("WARNING: file ./conf/param_DMprimWriteON.txt missing\n");
        printf("Setting DMprimaryWrite_ON = 1\n");
        fprintf(fplog, "WARNING: file ./conf/param_DMprimWriteON.txt missing. Setting to 1\n");
        AOconf[loop].DMprimaryWrite_ON = 1;
    }
    else
    {
        if(fscanf(fp, "%200s", content) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("GPU : %d\n", atoi(content));
        fclose(fp);
        fflush(stdout);
        AOconf[loop].DMprimaryWrite_ON = atoi(content);
        fprintf(fplog, "AOconf[%ld].DMprimaryWrite_ON = %d\n", loop, AOconf[loop].DMprimaryWrite_ON);
    }
    
    

	/** ### 1.7. WFS image total flux computation mode
	 * 
	 * - ./conf/param_COMPUTE_TOTAL_ASYNC.txt -> AOconf[loop].AOLCOMPUTE_TOTAL_ASYNC
	 * 
	 */
	 fprintf(fplog, "\n\n============== 1.7. WFS image total flux computation mode ===================\n\n");

    // TOTAL image done in separate thread ?
    AOconf[loop].AOLCOMPUTE_TOTAL_ASYNC = 0;
    if((fp=fopen("./conf/param_COMPUTE_TOTAL_ASYNC.txt","r"))==NULL)
    {
        printf("WARNING: file ./conf/param_COMPUTE_TOTAL_ASYNC.txt missing\n");
        printf("Using default: %d\n", AOconf[loop].AOLCOMPUTE_TOTAL_ASYNC);
        fprintf(fplog, "WARNING: file ./conf/param_COMPUTE_TOTAL_ASYNC.txt missing. Using default: %d\n", AOconf[loop].AOLCOMPUTE_TOTAL_ASYNC);
    }
    else
    {
        if(fscanf(fp, "%200s", content) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("AOLCOMPUTE_TOTAL_ASYNC : %d\n", atoi(content));
        fclose(fp);
        fflush(stdout);
        AOconf[loop].AOLCOMPUTE_TOTAL_ASYNC = atoi(content);
        fprintf(fplog, "AOconf[%ld].AOLCOMPUTE_TOTAL_ASYNC = %d\n", loop, AOconf[loop].AOLCOMPUTE_TOTAL_ASYNC);
    }


    /** ### 1.8. Read CMatrix mult mode
     * 
     * - ./conf/param_CMMMODE.txt -> CMMODE
     * 		- 0 : WFS signal -> Mode coeffs -> DM act values  (2 sequential matrix multiplications)
     * 		- 1 : WFS signal -> DM act values  (1 combined matrix multiplication)
     */ 

 	fprintf(fplog, "\n\n============== 1.8. Read CMatrix mult mode ===================\n\n");

    if((fp=fopen("./conf/param_CMMODE.txt", "r"))==NULL)
    {
        printf("WARNING: file ./conf/param_CMMODE.txt missing\n");
        printf("Using combined matrix\n");
        AOconf[loop].CMMODE = 1;  // by default, use combined matrix
        fprintf(fplog, "WARNING: file ./conf/param_CMMODE.txt missing. Using combined matrix\n");
    }
    else
    {
        if(fscanf(fp, "%200s", content) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("Matrix mult mode : %d\n", atoi(content));
        fclose(fp);
        fflush(stdout);
        AOconf[loop].CMMODE = atoi(content);
        fprintf(fplog, "CMMODE = %d\n", AOconf[loop].CMMODE);
    }




	/** ### 1.9. Read loop frequ
	 * 
	 * - ./conf/param_loopfrequ.txt -> AOconf[loop].loopfrequ
	 * 
	 * @warning check redundancy with earlier read
	 */
	fprintf(fplog, "\n\n============== 1.9. Read loop frequ ===================\n\n");

    if((fp=fopen("./conf/param_loopfrequ.txt","r"))==NULL)
    {
        printf("WARNING: file ./conf/param_loopfrequ.txt missing\n");
        printf("Using default loop speed\n");
        fprintf(fplog, "WARNING: file ./conf/param_loopfrequ.txt missing. Using default loop speed\n");
        AOconf[loop].loopfrequ = 2000.0;
    }
    else
    {
        if(fscanf(fp, "%200s", content) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        printf("loopfrequ : %f\n", atof(content));
        fclose(fp);
        fflush(stdout);
        AOconf[loop].loopfrequ = atof(content);
        fprintf(fplog, "AOconf[%ld].loopfrequ = %f\n", loop, AOconf[loop].loopfrequ);
    }




	/** ### 1.10. Setup loop timing array 
	 */
	fprintf(fplog, "\n\n============== 1.10. Setup loop timing array ===================\n\n");

    if(sprintf(name, "aol%ld_looptiming", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    aoconfID_looptiming = AOloopControl_2Dloadcreate_shmim(name, " ", NBtimers, 1);





	/** ## 2. Read/load shared memory arrays
	 * 
	 */ 
	fprintf(fplog, "\n\n============== 2. Read/load shared memory arrays ===================\n\n");

    /**
     * ### 2.1. CONNECT to existing streams
     * 
     * Note: these streams MUST exist
     * 
     *  - AOconf[loop].dmdispname  : this image is read to notify when new dm displacement is ready
     *  - AOconf[loop].WFSname     : connect to WFS camera. This is where the size of the WFS is read 
     */
     
     fprintf(fplog, "\n\n============== 2.1. CONNECT to existing streams  ===================\n\n");
     
    aoconfID_dmdisp = read_sharedmem_image(AOconf[loop].dmdispname);
    if(aoconfID_dmdisp==-1)
        fprintf(fplog, "ERROR : cannot read shared memory stream %s\n", AOconf[loop].dmdispname);
    else
        fprintf(fplog, "stream %s loaded as ID = %ld\n", AOconf[loop].dmdispname, aoconfID_dmdisp);

 
    aoconfID_wfsim = read_sharedmem_image(AOconf[loop].WFSname);
    if(aoconfID_wfsim == -1)
        fprintf(fplog, "ERROR : cannot read shared memory stream %s\n", AOconf[loop].WFSname);
    else
        fprintf(fplog, "stream %s loaded as ID = %ld\n", AOconf[loop].WFSname, aoconfID_wfsim);

    AOconf[loop].sizexWFS = data.image[aoconfID_wfsim].md[0].size[0];
    AOconf[loop].sizeyWFS = data.image[aoconfID_wfsim].md[0].size[1];
    AOconf[loop].sizeWFS = AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS;

    fprintf(fplog, "WFS stream size = %ld x %ld\n", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);





    /**
     * 
     * ### 2.2. Read file to stream or connect to existing stream
     * 
     *  The AOloopControl_xDloadcreate_shmim functions are used, and follows these rules:
     * 
     * If file already loaded, use it (we assume it's already been properly loaded) \n
     * If not, attempt to read it from shared memory \n
     * If not available in shared memory, create it in shared memory \n
     * if "fname" exists, attempt to load it into the shared memory image
     *
     * Stream names are fixed: 
     * - aol_wfsdark
     * - aol_imWFS0
     * - aol_imWFS0tot
     * - aol_imWFS1
     * - aol_imWFS2
     * - aol_wfsref0
     * - aol_wfsref
     */
     
     
	fprintf(fplog, "\n\n============== 2.2. Read file to stream or connect to existing stream  ===================\n\n");

    if(sprintf(name, "aol%ld_wfsdark", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    sprintf(fname, "./conf/aol%ld_wfsdark.fits", loop);
    aoconfID_wfsdark = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);



    if(sprintf(name, "aol%ld_imWFS0", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    aoconfID_imWFS0 = AOloopControl_2Dloadcreate_shmim(name, " ", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    COREMOD_MEMORY_image_set_createsem(name, 10);

    if(sprintf(name, "aol%ld_imWFS0tot", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    aoconfID_imWFS0tot = AOloopControl_2Dloadcreate_shmim(name, " ", 1, 1);
    COREMOD_MEMORY_image_set_createsem(name, 10);

    if(sprintf(name, "aol%ld_imWFS1", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    aoconfID_imWFS1 = AOloopControl_2Dloadcreate_shmim(name, " ", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);

    if(sprintf(name, "aol%ld_imWFS2", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    aoconfID_imWFS2 = AOloopControl_2Dloadcreate_shmim(name, " ", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);





    initwfsref = AOconf[loop].init_wfsref0;

    if(sprintf(name, "aol%ld_wfsref0", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if(sprintf(fname, "./conf/aol%ld_wfsref0.fits", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    aoconfID_wfsref0 = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    AOconf[loop].init_wfsref0 = 1;

    if(sprintf(name, "aol%ld_wfsref", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if(sprintf(fname, "./conf/aol%ld_wfsref.fits", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    aoconfID_wfsref = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);

    if(initwfsref==0)
    {
        char name1[200];

        if(sprintf(name1, "aol%ld_wfsref0", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        copy_image_ID(name1, name, 1);
    }




    /** ### Connect to DM
     * 
     * - AOconf[loop].dmCname : DM control channel
     * 
     *  Here the DM size is read -> Oconf[loop].sizexDM, AOconf[loop].sizeyDM
     */


    aoconfID_dmC = image_ID(AOconf[loop].dmCname);
    if(aoconfID_dmC==-1)
    {
        printf("connect to %s\n", AOconf[loop].dmCname);
        aoconfID_dmC = read_sharedmem_image(AOconf[loop].dmCname);
        if(aoconfID_dmC==-1)
        {
            printf("ERROR: cannot connect to shared memory %s\n", AOconf[loop].dmCname);
            exit(0);
        }
    }
    AOconf[loop].sizexDM = data.image[aoconfID_dmC].md[0].size[0];
    AOconf[loop].sizeyDM = data.image[aoconfID_dmC].md[0].size[1];
    AOconf[loop].sizeDM = AOconf[loop].sizexDM*AOconf[loop].sizeyDM;

    fprintf(fplog, "Connected to DM %s, size = %ld x %ld\n", AOconf[loop].dmCname, AOconf[loop].sizexDM, AOconf[loop].sizeyDM);


	/**
	 * - AOconf[loop].dmRMname : DM response matrix channel
	 * 
	 */
    aoconfID_dmRM = image_ID(AOconf[loop].dmRMname);
    if(aoconfID_dmRM==-1)
    {
        printf("connect to %s\n", AOconf[loop].dmRMname);
        aoconfID_dmRM = read_sharedmem_image(AOconf[loop].dmRMname);
        if(aoconfID_dmRM==-1)
        {
            printf("ERROR: cannot connect to shared memory %s\n", AOconf[loop].dmRMname);
            exit(0);
        }
    }
    fprintf(fplog, "stream %s loaded as ID = %ld\n", AOconf[loop].dmRMname, aoconfID_dmRM);



	/// Connect to DM modes shared mem
	aoconfID_DMmodes = image_ID(AOconf[loop].DMmodesname);
	if(aoconfID_DMmodes==-1)
    {
        printf("connect to %s\n", AOconf[loop].DMmodesname);
        aoconfID_DMmodes = read_sharedmem_image(AOconf[loop].DMmodesname);
        if(aoconfID_DMmodes==-1)
        {
            printf("ERROR: cannot connect to shared memory %s\n", AOconf[loop].DMmodesname);
            exit(0);
        }
    }
    fprintf(fplog, "stream %s loaded as ID = %ld\n", AOconf[loop].dmRMname, aoconfID_DMmodes);
	AOconf[loop].NBDMmodes = data.image[aoconfID_DMmodes].md[0].size[2];
	printf("NBmodes = %ld\n", AOconf[loop].NBDMmodes);


	/** 
	 * ## 3. Load DM modes (if level >= 10)
	 * 
	 * 
	 * */

	fprintf(fplog, "\n\n============== 3. Load DM modes (if level >= 10)  ===================\n\n");


	
    if(level>=10) // Load DM modes (will exit if not successful)
    {				
		/** 
		 * Load AOconf[loop].DMmodesname \n
		 * if already exists in local memory, trust it and adopt it \n
		 * if not, load from ./conf/aol%ld_DMmodes.fits \n
		 * 
		 */
		
        aoconfID_DMmodes = image_ID(AOconf[loop].DMmodesname); 
		


        if(aoconfID_DMmodes == -1) // If not, check file
        {
            long ID1tmp, ID2tmp;
            int vOK;

			

            if(sprintf(fname, "./conf/aol%ld_DMmodes.fits", loop) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            printf("Checking file \"%s\"\n", fname);

            // GET SIZE FROM FILE
            ID1tmp = load_fits(fname, "tmp3Dim", 1);
            if(ID1tmp==-1)
            {
                printf("WARNING: no file \"%s\" -> loading zonal modes\n", fname);

                if(sprintf(fname, "./conf/aol%ld_DMmodes_zonal.fits", loop) <1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                ID1tmp = load_fits(fname, "tmp3Dim", 1);
                if(ID1tmp==-1)
                {
                    printf("ERROR: cannot read zonal modes \"%s\"\n", fname);
                    exit(0);
                }
            }


            // check size
            if(data.image[ID1tmp].md[0].naxis != 3)
            {
                printf("ERROR: File \"%s\" is not a 3D image (cube)\n", fname);
                exit(0);
            }
            if(data.image[ID1tmp].md[0].size[0] != AOconf[loop].sizexDM)
            {
                printf("ERROR: File \"%s\" has wrong x size: should be %ld, is %ld\n", fname, AOconf[loop].sizexDM, (long) data.image[ID1tmp].md[0].size[0]);
                exit(0);
            }
            if(data.image[ID1tmp].md[0].size[1] != AOconf[loop].sizeyDM)
            {
                printf("ERROR: File \"%s\" has wrong y size: should be %ld, is %ld\n", fname, AOconf[loop].sizeyDM, (long) data.image[ID1tmp].md[0].size[1]);
                exit(0);
            }
            AOconf[loop].NBDMmodes = data.image[ID1tmp].md[0].size[2];

            printf("NUMBER OF MODES = %ld\n", AOconf[loop].NBDMmodes);

            // try to read it from shared memory
            ID2tmp = read_sharedmem_image(AOconf[loop].DMmodesname);
            vOK = 0;
            if(ID2tmp != -1) // if shared memory exists, check its size
            {
                vOK = 1;
                if(data.image[ID2tmp].md[0].naxis != 3)
                {
                    printf("ERROR: Shared memory File %s is not a 3D image (cube)\n", AOconf[loop].DMmodesname);
                    vOK = 0;
                }
                if(data.image[ID2tmp].md[0].size[0] != AOconf[loop].sizexDM)
                {
                    printf("ERROR: Shared memory File %s has wrong x size: should be %ld, is %ld\n", AOconf[loop].DMmodesname, AOconf[loop].sizexDM, (long) data.image[ID2tmp].md[0].size[0]);
                    vOK = 0;
                }
                if(data.image[ID2tmp].md[0].size[1] != AOconf[loop].sizeyDM)
                {
                    printf("ERROR: Shared memory File %s has wrong y size: should be %ld, is %ld\n", AOconf[loop].DMmodesname, AOconf[loop].sizeyDM, (long) data.image[ID2tmp].md[0].size[1]);
                    vOK = 0;
                }
                if(data.image[ID2tmp].md[0].size[2] != AOconf[loop].NBDMmodes)
                {
                    printf("ERROR: Shared memory File %s has wrong y size: should be %ld, is %ld\n", AOconf[loop].DMmodesname, AOconf[loop].NBDMmodes, (long) data.image[ID2tmp].md[0].size[2]);
                    vOK = 0;
                }

                if(vOK==1) // if size is OK, adopt it
                    aoconfID_DMmodes = ID2tmp;
                else // if not, erase shared memory
                {
                    printf("SHARED MEM IMAGE HAS WRONG SIZE -> erasing it\n");
                    delete_image_ID(AOconf[loop].DMmodesname);
                }
            }


            if(vOK==0) // create shared memory
            {

                sizearray[0] = AOconf[loop].sizexDM;
                sizearray[1] = AOconf[loop].sizeyDM;
                sizearray[2] = AOconf[loop].NBDMmodes;
                printf("Creating %s   [%ld x %ld x %ld]\n", AOconf[loop].DMmodesname, (long) sizearray[0], (long) sizearray[1], (long) sizearray[2]);
                fflush(stdout);
                aoconfID_DMmodes = create_image_ID(AOconf[loop].DMmodesname, 3, sizearray, _DATATYPE_FLOAT, 1, 0);
            }

            // put modes into shared memory

            switch (data.image[ID1tmp].md[0].atype) {
            case _DATATYPE_FLOAT :
                memcpy(data.image[aoconfID_DMmodes].array.F, data.image[ID1tmp].array.F, sizeof(float)*AOconf[loop].sizexDM*AOconf[loop].sizeyDM*AOconf[loop].NBDMmodes);
                break;
            case _DATATYPE_DOUBLE :
                for(ii=0; ii<AOconf[loop].sizexDM*AOconf[loop].sizeyDM*AOconf[loop].NBDMmodes; ii++)
                    data.image[aoconfID_DMmodes].array.F[ii] = data.image[ID1tmp].array.D[ii];
                break;
            default :
                printf("ERROR: TYPE NOT RECOGNIZED FOR MODES\n");
                exit(0);
                break;
            }

            delete_image_ID("tmp3Dim");
        }

        fprintf(fplog, "stream %s loaded as ID = %ld, size %ld %ld %ld\n", AOconf[loop].DMmodesname, aoconfID_DMmodes, AOconf[loop].sizexDM, AOconf[loop].sizeyDM, AOconf[loop].NBDMmodes);
    }





    // TO BE CHECKED

    // AOconf[loop].NBMblocks = AOconf[loop].DMmodesNBblock;
    // printf("NBMblocks : %ld\n", AOconf[loop].NBMblocks);
    // fflush(stdout);


    AOconf[loop].AveStats_NBpt = 100;
    for(k=0; k<AOconf[loop].DMmodesNBblock; k++)
    {
        AOconf[loop].block_OLrms[k] = 0.0;
        AOconf[loop].block_Crms[k] = 0.0;
        AOconf[loop].block_WFSrms[k] = 0.0;
        AOconf[loop].block_limFrac[k] = 0.0;

        AOconf[loop].blockave_OLrms[k] = 0.0;
        AOconf[loop].blockave_Crms[k] = 0.0;
        AOconf[loop].blockave_WFSrms[k] = 0.0;
        AOconf[loop].blockave_limFrac[k] = 0.0;
    }




    printf("%ld modes\n", AOconf[loop].NBDMmodes);





    // load ref WFS image
    // sprintf(name, "aol%ld_wfsref", loop);
    // aoconfID_wfsref = AOloopControl_2Dloadcreate_shmim(name, "./conf/wfsref.fits", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);


    if(level>=10)
    {
        long ID;

        // Load/create modal command vector memory
        if(sprintf(name, "aol%ld_DMmode_cmd", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        ID = image_ID(name);
        printf("STEP 000----------- (%ld) --\n", AOconf[loop].NBDMmodes);
        fflush(stdout);
        list_image_ID();
        aoconfID_cmd_modes = AOloopControl_2Dloadcreate_shmim(name, "", AOconf[loop].NBDMmodes, 1);
        printf("STEP 001------------\n");
        fflush(stdout);


        if(ID==-1)
            for(k=0; k<AOconf[loop].NBDMmodes; k++)
                data.image[aoconfID_cmd_modes].array.F[k] = 0.0;

        if(sprintf(name, "aol%ld_DMmode_meas", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        ID = image_ID(name);
        aoconfID_meas_modes = AOloopControl_2Dloadcreate_shmim(name, "", AOconf[loop].NBDMmodes, 1);
        if(ID==-1)
            for(k=0; k<AOconf[loop].NBDMmodes; k++)
                data.image[aoconfID_meas_modes].array.F[k] = 0.0;

        if(sprintf(name, "aol%ld_DMmode_AVE", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        ID = image_ID(name);
        aoconfID_AVE_modes = AOloopControl_2Dloadcreate_shmim(name, "", AOconf[loop].NBDMmodes, 1);
        if(ID==-1)
            for(k=0; k<AOconf[loop].NBDMmodes; k++)
                data.image[aoconfID_AVE_modes].array.F[k] = 0.0;

        if(sprintf(name, "aol%ld_DMmode_RMS", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        ID = image_ID(name);
        aoconfID_RMS_modes = AOloopControl_2Dloadcreate_shmim(name, "", AOconf[loop].NBDMmodes, 1);
        if(ID==-1)
            for(k=0; k<AOconf[loop].NBDMmodes; k++)
                data.image[aoconfID_RMS_modes].array.F[k] = 0.0;

        if(sprintf(name, "aol%ld_DMmode_GAIN", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        ID = image_ID(name);
        aoconfID_GAIN_modes = AOloopControl_2Dloadcreate_shmim(name, "", AOconf[loop].NBDMmodes, 1);
        if(ID==-1)
            for(k=0; k<AOconf[loop].NBDMmodes; k++)
                data.image[aoconfID_GAIN_modes].array.F[k] = 1.0;

        if(sprintf(name, "aol%ld_DMmode_LIMIT", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        ID = image_ID(name);
        aoconfID_LIMIT_modes = AOloopControl_2Dloadcreate_shmim(name, "", AOconf[loop].NBDMmodes, 1);
        if(ID==-1)
            for(k=0; k<AOconf[loop].NBDMmodes; k++)
                data.image[aoconfID_LIMIT_modes].array.F[k] = 1.0;

        if(sprintf(name, "aol%ld_DMmode_MULTF", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        ID = image_ID(name);
        aoconfID_MULTF_modes = AOloopControl_2Dloadcreate_shmim(name, "", AOconf[loop].NBDMmodes, 1);
        if(ID==-1)
            for(k=0; k<AOconf[loop].NBDMmodes; k++)
                data.image[aoconfID_MULTF_modes].array.F[k] = 1.0;




        if(sprintf(name, "aol%ld_wfsmask", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        sprintf(fname, "conf/%s.fits", name);
        aoconfID_wfsmask = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
        AOconf[loop].activeWFScnt = 0;
        if(aoconfID_wfsmask==-1)
            for(ii=0; ii<AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS; ii++)
                data.image[aoconfID_wfsmask].array.F[ii] = 1.0;
        for(ii=0; ii<AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS; ii++)
            if(data.image[aoconfID_wfsmask].array.F[ii]>0.5)
                AOconf[loop].activeWFScnt++;

        if(sprintf(name, "aol%ld_dmmask", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        if(sprintf(fname, "conf/%s.fits", name) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        aoconfID_dmmask = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].sizexDM, AOconf[loop].sizeyDM);
        if(aoconfID_dmmask==-1)
            for(ii=0; ii<AOconf[loop].sizexDM*AOconf[loop].sizeyDM; ii++)
                data.image[aoconfID_dmmask].array.F[ii] = 1.0;
        AOconf[loop].activeDMcnt = 0;
        for(ii=0; ii<AOconf[loop].sizexDM*AOconf[loop].sizeyDM; ii++)
            if(data.image[aoconfID_dmmask].array.F[ii]>0.5)
                AOconf[loop].activeDMcnt++;
        printf(" AOconf[loop].activeWFScnt = %ld\n", AOconf[loop].activeWFScnt );
        printf(" AOconf[loop].activeDMcnt = %ld\n", AOconf[loop].activeDMcnt );


        AOconf[loop].init_RM = 0;
        if(sprintf(fname, "conf/aol%ld_respM.fits", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_respM = AOloopControl_3Dloadcreate_shmim(AOconf[loop].respMname, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].NBDMmodes);
        AOconf[loop].init_RM = 1;


        AOconf[loop].init_CM = 0;
        if(sprintf(fname, "conf/aol%ld_contrM.fits", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_contrM = AOloopControl_3Dloadcreate_shmim(AOconf[loop].contrMname, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].NBDMmodes);
        AOconf[loop].init_CM = 1;

        if((fp=fopen("conf/param_NBmodeblocks.txt", "r"))==NULL)
        {
            printf("Cannot open conf/param_NBmodeblocks.txt.... assuming 1 block\n");
            AOconf[loop].DMmodesNBblock = 1;
        }
        else
        {
            if(fscanf(fp, "%50ld", &tmpl) == 1)
                AOconf[loop].DMmodesNBblock = tmpl;
            else
            {
                printf("Cannot read conf/param_NBmodeblocks.txt.... assuming 1 block\n");
                AOconf[loop].DMmodesNBblock = 1;
            }
            fclose(fp);
        }



        if(sprintf(name, "aol%ld_contrMc", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        if(sprintf(fname, "conf/aol%ld_contrMc.fits", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        aoconfID_contrMc = AOloopControl_3Dloadcreate_shmim(name, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].sizeDM);

        if(sprintf(name, "aol%ld_contrMcact", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        if(sprintf(fname, "conf/aol%ld_contrMcact_00.fits", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        aoconfID_contrMcact[0] = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].activeWFScnt, AOconf[loop].activeDMcnt);




        if(sprintf(name, "aol%ld_gainb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        if(sprintf(fname, "conf/aol%ld_gainb.fits", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_gainb = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].DMmodesNBblock, 1);

        if(sprintf(name, "aol%ld_multfb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        if(sprintf(fname, "conf/aol%ld_multfb.fits", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        aoconfID_multfb = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].DMmodesNBblock, 1);

        if(sprintf(name, "aol%ld_limitb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        if(sprintf(fname, "conf/aol%ld_limitb.fits", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        aoconfID_limitb = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].DMmodesNBblock, 1);


#ifdef _PRINT_TEST
        printf("TEST - INITIALIZE contrMc, contrMcact\n");
        fflush(stdout);
#endif


        uint_fast16_t kk;
        for(kk=0; kk<AOconf[loop].DMmodesNBblock; kk++)
        {
            long ID;

#ifdef _PRINT_TEST
            printf("TEST - BLOCK %3ld gain = %f\n", kk, data.image[aoconfID_gainb].array.F[kk]);
            fflush(stdout);
#endif

            if(sprintf(name, "aol%ld_DMmodes%02ld", loop, kk) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            if(sprintf(fname, "conf/%s.fits", name) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            printf("FILE = %s\n", fname);
            printf("====== LOADING %s to %s\n", fname, name);
            fflush(stdout);
            if((ID=AOloopControl_3Dloadcreate_shmim(name, fname, AOconf[loop].sizexDM, AOconf[loop].sizeyDM, 0))!=-1)
                AOconf[loop].NBmodes_block[kk] = data.image[ID].md[0].size[2];


            if(sprintf(name, "aol%ld_respM%02ld", loop, kk) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            if(sprintf(fname, "conf/%s.fits", name) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            printf("====== LOADING %s to %s\n", fname, name);
            fflush(stdout);
            AOloopControl_3Dloadcreate_shmim(name, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].NBmodes_block[kk]);


            if(sprintf(name, "aol%ld_contrM%02ld", loop, kk) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            if(sprintf(fname, "conf/%s.fits", name) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            printf("====== LOADING %s to %s\n", fname, name);
            fflush(stdout);
            AOloopControl_3Dloadcreate_shmim(name, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].NBmodes_block[kk]);


            if(sprintf(name, "aol%ld_contrMc%02ld", loop, kk) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            if(sprintf(fname, "conf/%s.fits", name) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            ID = AOloopControl_3Dloadcreate_shmim(name, fname, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].sizexDM*AOconf[loop].sizeyDM);
            if(kk==0)
                for(ii=0; ii<AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS*AOconf[loop].sizexDM*AOconf[loop].sizeyDM; ii++)
                    data.image[aoconfID_contrMc].array.F[ii] = 0.0;
            for(ii=0; ii<AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS*AOconf[loop].sizexDM*AOconf[loop].sizeyDM; ii++)
                data.image[aoconfID_contrMc].array.F[ii] += data.image[aoconfID_gainb].array.F[kk]*data.image[ID].array.F[ii];


            if(sprintf(name, "aol%ld_contrMcact%02ld_00", loop, kk) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            if(sprintf(fname, "conf/%s.fits", name) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            //   sprintf(fname, "conf/aol%ld_contrMcact%02ld_00", loop, kk);
            printf("====== LOADING %s to %s  size %ld %ld\n", fname, name,  AOconf[loop].activeWFScnt, AOconf[loop].activeDMcnt);
            ID = AOloopControl_2Dloadcreate_shmim(name, fname, AOconf[loop].activeWFScnt, AOconf[loop].activeDMcnt);

            if(kk==0)
                for(ii=0; ii<AOconf[loop].activeWFScnt*AOconf[loop].activeDMcnt; ii++)
                    data.image[aoconfID_contrMcact[0]].array.F[ii] = 0.0;

            for(ii=0; ii<AOconf[loop].activeWFScnt*AOconf[loop].activeDMcnt; ii++)
                data.image[aoconfID_contrMcact[0]].array.F[ii] += data.image[aoconfID_gainb].array.F[kk]*data.image[ID].array.F[ii];

        }
    }
    free(sizearray);




    if(AOconf[loop].DMmodesNBblock==1)
        AOconf[loop].indexmaxMB[0] = AOconf[loop].NBDMmodes;
    else
    {
        AOconf[loop].indexmaxMB[0] = AOconf[loop].NBmodes_block[0];
        for(k=1; k<AOconf[loop].DMmodesNBblock; k++)
            AOconf[loop].indexmaxMB[k] = AOconf[loop].indexmaxMB[k-1] + AOconf[loop].NBmodes_block[k];
    }

    if(sprintf(fname, "./conf/param_blockoffset_%02ld.txt", (long) 0) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    fp = fopen(fname, "w");
    fprintf(fp, "   0\n");
    fprintf(fp, "%4ld\n", AOconf[loop].NBmodes_block[0]);
    fclose(fp);
    for(k=1; k<AOconf[loop].DMmodesNBblock; k++)
    {
        if(sprintf(fname, "./conf/param_blockoffset_%02ld.txt", k) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        fp = fopen(fname, "w");
        fprintf(fp, "%4ld\n", AOconf[loop].indexmaxMB[k-1]);
        fprintf(fp, "%4ld\n", AOconf[loop].NBmodes_block[k]);
        fclose(fp);
    }



    list_image_ID();
    printf(" AOconf[loop].activeWFScnt = %ld\n", AOconf[loop].activeWFScnt );
    printf(" AOconf[loop].activeDMcnt = %ld\n", AOconf[loop].activeDMcnt );
    printf("   init_WFSref0    %d\n", AOconf[loop].init_wfsref0);
    printf("   init_RM        %d\n", AOconf[loop].init_RM);
    printf("   init_CM        %d\n", AOconf[loop].init_CM);


    AOconf[loop].init = 1;

    loadcreateshm_log = 0;
    fclose(fplog);

#ifdef AOLOOPCONTROL_LOGFUNC
	AOLOOPCONTROL_logfunc_level = 0;
    CORE_logFunctionCall( AOLOOPCONTROL_logfunc_level, AOLOOPCONTROL_logfunc_level_max, 1, __FUNCTION__, __LINE__, "");
#endif

    return(0);
}
















/*** mode = 0 or 1. if mode == 1, simply connect */

static int_fast8_t AOloopControl_InitializeMemory(int mode)
{
    int SM_fd;
    struct stat file_stat;
    int create = 0;
    long loop;
    int tmpi;


#ifdef AOLOOPCONTROL_LOGFUNC
	AOLOOPCONTROL_logfunc_level = 0;
    CORE_logFunctionCall( AOLOOPCONTROL_logfunc_level, AOLOOPCONTROL_logfunc_level_max, 0, __FUNCTION__, __LINE__, "");
#endif





    loop = LOOPNUMBER;

    SM_fd = open(AOconfname, O_RDWR);
    if(SM_fd==-1)
    {
        printf("Cannot import file \"%s\" -> creating file\n", AOconfname);
        create = 1;
    }
    else
    {
        fstat(SM_fd, &file_stat);
        printf("File %s size: %zd\n", AOconfname, file_stat.st_size);
        if(file_stat.st_size!=sizeof(AOLOOPCONTROL_CONF)*NB_AOloopcontrol)
        {
            printf("File \"%s\" size is wrong -> recreating file\n", AOconfname);
            create = 1;
            close(SM_fd);
        }
    }

    if(create==1)
    {
        int result;

        SM_fd = open(AOconfname, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);

        if (SM_fd == -1) {
            perror("Error opening file for writing");
            exit(0);
        }

        result = lseek(SM_fd, sizeof(AOLOOPCONTROL_CONF)*NB_AOloopcontrol-1, SEEK_SET);
        if (result == -1) {
            close(SM_fd);
            perror("Error calling lseek() to 'stretch' the file");
            exit(0);
        }

        result = write(SM_fd, "", 1);
        if (result != 1) {
            close(SM_fd);
            perror("Error writing last byte of the file");
            exit(0);
        }
    }




    AOconf = (AOLOOPCONTROL_CONF*) mmap(0, sizeof(AOLOOPCONTROL_CONF)*NB_AOloopcontrol, PROT_READ | PROT_WRITE, MAP_SHARED, SM_fd, 0);
    if (AOconf == MAP_FAILED) {
        close(SM_fd);
        perror("Error mmapping the file");
        exit(0);
    }




    if((mode==0)||(create==1))
    {
        char cntname[200];

        AOconf[loop].on = 0;
        AOconf[loop].DMprimaryWrite_ON = 0;
        AOconf[loop].AUTOTUNE_LIMITS_ON = 0;
        AOconf[loop].AUTOTUNE_GAINS_ON = 0;
        AOconf[loop].ARPFon = 0;
        AOconf[loop].cnt = 0;
        AOconf[loop].cntmax = 0;
        AOconf[loop].init_CMc = 0;

        if(sprintf(cntname, "aol%ld_logdata", loop) < 1) // contains loop count (cnt0) and loop gain
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        if((aoconfIDlogdata = image_ID(cntname))==-1)
        {
            uint32_t *sizearray;
            sizearray = (uint32_t*) malloc(sizeof(uint32_t)*2);
            sizearray[0] = 1;
            sizearray[1] = 1;
            aoconfIDlogdata = create_image_ID(cntname, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
            free(sizearray);
        }
    }


    if(create==1)
    {
        for(loop=0; loop<NB_AOloopcontrol; loop++)
        {
            AOconf[loop].init = 0;
            AOconf[loop].on = 0;
            AOconf[loop].DMprimaryWrite_ON = 0;
            AOconf[loop].ARPFon = 0;
            AOconf[loop].cnt = 0;
            AOconf[loop].cntmax = 0;
            AOconf[loop].maxlimit = 0.3;
            AOconf[loop].mult = 1.00;
            AOconf[loop].gain = 0.0;
            AOconf[loop].AUTOTUNE_LIMITS_perc = 1.0; // percentile threshold
            AOconf[loop].AUTOTUNE_LIMITS_mcoeff = 1.0; // multiplicative coeff
            AOconf[loop].AUTOTUNE_LIMITS_delta = 1.0e-3;
            AOconf[loop].ARPFgain = 0.0;
            AOconf[loop].WFSnormfloor = 0.0;
            AOconf[loop].framesAve = 1;
            AOconf[loop].DMmodesNBblock = 1;
            AOconf[loop].GPUusesem = 1;

            AOconf[loop].loopfrequ = 2000.0;
            AOconf[loop].hardwlatency = 0.0011;
            AOconf[loop].hardwlatency_frame = 2.2;
            AOconf[loop].complatency = 0.0001;
            AOconf[loop].complatency_frame = 0.2;
            AOconf[loop].wfsmextrlatency = 0.0003;
            AOconf[loop].wfsmextrlatency_frame = 0.6;
        }
    }
    else
    {
        for(loop=0; loop<NB_AOloopcontrol; loop++)
            if(AOconf[loop].init == 1)
            {
                printf("LIST OF ACTIVE LOOPS:\n");
                printf("----- Loop %ld   (%s) ----------\n", loop, AOconf[loop].name);
                printf("  WFS:  %s  [%ld]  %ld x %ld\n", AOconf[loop].WFSname, aoconfID_wfsim, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
                printf("   DM:  %s  [%ld]  %ld x %ld\n", AOconf[loop].dmCname, aoconfID_dmC, AOconf[loop].sizexDM, AOconf[loop].sizeyDM);
                printf("DM RM:  %s  [%ld]  %ld x %ld\n", AOconf[loop].dmRMname, aoconfID_dmC, AOconf[loop].sizexDM, AOconf[loop].sizeyDM);
            }
    }

    if(AOloopcontrol_meminit==0)
    {

        printf("INITIALIZING GPUset ARRAYS\n");
        fflush(stdout);

        GPUset0 = (int*) malloc(sizeof(int)*GPUcntMax);

        uint_fast16_t k;

        for(k=0; k<GPUcntMax; k++)
        {
            FILE *fp;
            char fname[200];

            if(sprintf(fname, "./conf/param_GPUset0dev%d.txt", (int) k) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            fp = fopen(fname, "r");
            if(fp!=NULL)
            {
                if(fscanf(fp, "%50d" , &tmpi) != 1)
                    printERROR(__FILE__, __func__, __LINE__, "Cannot read parameter from file");

                fclose(fp);
                GPUset0[k] = tmpi;
            }
            else
                GPUset0[k] = k;
        }


        GPUset1 = (int*) malloc(sizeof(int)*GPUcntMax);
        for(k=0; k<GPUcntMax; k++)
        {
            FILE *fp;
            char fname[200];

            if(sprintf(fname, "./conf/param_GPUset1dev%d.txt", (int) k) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            fp = fopen(fname, "r");
            if(fp!=NULL)
            {
                if(fscanf(fp, "%50d" , &tmpi) != 1)
                    printERROR(__FILE__, __func__, __LINE__, "Cannot read parameter from file");

                fclose(fp);
                GPUset1[k] = tmpi;
            }
            else
                GPUset1[k] = k;
        }
    }

    AOloopcontrol_meminit = 1;


#ifdef AOLOOPCONTROL_LOGFUNC
	AOLOOPCONTROL_logfunc_level = 0;
    CORE_logFunctionCall( AOLOOPCONTROL_logfunc_level, AOLOOPCONTROL_logfunc_level_max, 1, __FUNCTION__, __LINE__, "");
#endif

    return 0;
}





/* =============================================================================================== */
/** @name AOloopControl - 2.3. LOW LEVEL UTILITIES & TOOLS - MISC COMPUTATION ROUTINES             */
/* =============================================================================================== */




// measures cross product between 2 cubes
static long AOloopControl_CrossProduct(const char *ID1_name, const char *ID2_name, const char *IDout_name)
{
    long ID1, ID2, IDout;
    long xysize1, xysize2;
    long zsize1, zsize2;
    long z1, z2;
    long ii;
    long IDmask;




    ID1 = image_ID(ID1_name);
    ID2 = image_ID(ID2_name);

    xysize1 = data.image[ID1].md[0].size[0]*data.image[ID1].md[0].size[1];
    xysize2 = data.image[ID2].md[0].size[0]*data.image[ID2].md[0].size[1];
    zsize1 = data.image[ID1].md[0].size[2];
    zsize2 = data.image[ID2].md[0].size[2];

    if(xysize1!=xysize2)
    {
        printf("ERROR: cubes %s and %s have different xysize: %ld %ld\n", ID1_name, ID2_name, xysize1, xysize2);
        exit(0);
    }

    IDmask = image_ID("xpmask");


    IDout = create_2Dimage_ID(IDout_name, zsize1, zsize2);
    for(ii=0; ii<zsize1*zsize2; ii++)
        data.image[IDout].array.F[ii] = 0.0;

    if(IDmask==-1)
    {
        printf("No mask\n");
        fflush(stdout);


        for(z1=0; z1<zsize1; z1++)
            for(z2=0; z2<zsize2; z2++)
            {
                for(ii=0; ii<xysize1; ii++)
                {
                    data.image[IDout].array.F[z2*zsize1+z1] += data.image[ID1].array.F[z1*xysize1+ii] * data.image[ID2].array.F[z2*xysize2+ii];
                }
            }
    }
    else
    {
        printf("Applying mask\n");
        fflush(stdout);

        for(z1=0; z1<zsize1; z1++)
            for(z2=0; z2<zsize2; z2++)
            {
                for(ii=0; ii<xysize1; ii++)
                {
                    data.image[IDout].array.F[z2*zsize1+z1] += data.image[IDmask].array.F[ii]*data.image[IDmask].array.F[ii]*data.image[ID1].array.F[z1*xysize1+ii] * data.image[ID2].array.F[z2*xysize2+ii];
                }
            }
    }


    return(IDout);
}




static void *compute_function_imtotal( void *ptr )
{
    long ii;
    long nelem;
    int semval;



    nelem = data.image[aoconfID_imWFS0].md[0].size[0]*data.image[aoconfID_imWFS0].md[0].size[1];

    while(1)
    {
        sem_wait(&AOLCOMPUTE_TOTAL_ASYNC_sem_name);
        IMTOTAL = 0.0;
        if(aoconfID_wfsmask!=-1)
        {
            for(ii=0; ii<nelem; ii++)
                IMTOTAL += data.image[aoconfID_imWFS0].array.F[ii]*data.image[aoconfID_wfsmask].array.F[ii];
        }
        else
        {
            for(ii=0; ii<nelem; ii++)
                IMTOTAL += data.image[aoconfID_imWFS0].array.F[ii];
        }
        data.image[aoconfID_imWFS0tot].array.F[0] = IMTOTAL;
        COREMOD_MEMORY_image_set_sempost_byID(aoconfID_imWFS0tot, -1);
    }

}




static void *compute_function_dark_subtract( void *ptr )
{
    long ii, iistart, iiend;
    long nelem;
    long *index;
    int sval;
    long threadindex;
    int semval;


    nelem = data.image[aoconfID_imWFS0].md[0].size[0]*data.image[aoconfID_imWFS0].md[0].size[1];
    index = (long*) ptr;
    threadindex = *index;

    iistart = (long) ((threadindex)*nelem/COMPUTE_DARK_SUBTRACT_NBTHREADS);
    iiend = (long) ((threadindex+1)*nelem/COMPUTE_DARK_SUBTRACT_NBTHREADS);

    while(1)
    {
        sem_wait(&AOLCOMPUTE_DARK_SUBTRACT_sem_name[threadindex]);

        switch ( WFSatype ) {
        case _DATATYPE_UINT16 :
            for(ii=iistart; ii<iiend; ii++)
                data.image[aoconfID_imWFS0].array.F[ii] = ((float) arrayutmp[ii]) - data.image[Average_cam_frames_IDdark].array.F[ii];
            break;
        case _DATATYPE_FLOAT :
            for(ii=iistart; ii<iiend; ii++)
                data.image[aoconfID_imWFS0].array.F[ii] = ((float) arrayftmp[ii]) - data.image[Average_cam_frames_IDdark].array.F[ii];
            break;
        default :
            printf("ERROR: WFS data type not recognized\n");
            exit(0);
            break;
        }

        sem_getvalue(&AOLCOMPUTE_DARK_SUBTRACT_RESULT_sem_name[threadindex], &semval);
        if(semval<SEMAPHORE_MAXVAL)
            sem_post(&AOLCOMPUTE_DARK_SUBTRACT_RESULT_sem_name[threadindex]);
    }

}



// create simple poke matrix
long AOloopControl_mkSimpleZpokeM( long dmxsize, long dmysize, char *IDout_name)
{
    long IDout;
    uint_fast16_t dmxysize;
    uint_fast16_t ii, jj, kk;


    dmxysize = dmxsize * dmysize;

    IDout = create_3Dimage_ID(IDout_name, dmxsize, dmysize, dmxysize);

    for(kk=0; kk<dmxysize; kk++)
        data.image[IDout].array.F[kk*dmxysize + kk] = 1.0;

    return(IDout);
}










/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 3. WFS INPUT                                                             */
/* =============================================================================================== */
/* =============================================================================================== */



//
// every time im_name changes (counter increments), crop it to out_name in shared memory
//
int_fast8_t AOloopControl_camimage_extract2D_sharedmem_loop(const char *in_name, const char *dark_name, const char *out_name, long size_x, long size_y, long xstart, long ystart)
{
    long iiin,jjin, iiout, jjout;
    long IDin, IDout, IDdark;
    uint8_t atype;
    uint8_t atypeout;
    uint32_t *sizeout;
    long long cnt0;
    long IDmask;
    long sizeoutxy;
    long ii;


    sizeout = (uint32_t*) malloc(sizeof(uint32_t)*2);
    sizeout[0] = size_x;
    sizeout[1] = size_y;
    sizeoutxy = size_x*size_y;

    IDin = image_ID(in_name);
    atype = data.image[IDin].md[0].atype;



    // Check if there is a mask
    IDmask = image_ID("csmask");
    if(IDmask!=-1)
        if((data.image[IDmask].md[0].size[0]!=size_x)||(data.image[IDmask].md[0].size[1]!=size_y))
        {
            printf("ERROR: csmask has wrong size\n");
            exit(0);
        }

    // Check dark
    IDdark = image_ID(dark_name);

    if(IDdark!=-1)
    {
        if((data.image[IDdark].md[0].size[0]!=data.image[IDin].md[0].size[0])||(data.image[IDdark].md[0].size[1]!=data.image[IDin].md[0].size[1]))
        {
            printf("ERROR: csmask has wrong size\n");
            exit(0);
        }
        if(data.image[IDdark].md[0].atype != _DATATYPE_FLOAT)
        {
            printf("ERROR: csmask has wrong type\n");
            exit(0);
        }
        atypeout = _DATATYPE_FLOAT;
    }
    else
        atypeout = atype;


    // Create shared memory output image
    IDout = create_image_ID(out_name, 2, sizeout, atypeout, 1, 0);

    cnt0 = -1;

    switch (atype) {
    case _DATATYPE_UINT16 :
        while(1)
        {
            usleep(10); // OK FOR NOW (NOT USED BY FAST WFS)
            if(data.image[IDin].md[0].cnt0!=cnt0)
            {
                data.image[IDout].md[0].write = 1;
                cnt0 = data.image[IDin].md[0].cnt0;
                if(atypeout == _DATATYPE_UINT16)
                {
                    for(iiout=0; iiout<size_x; iiout++)
                        for(jjout=0; jjout<size_y; jjout++)
                        {
                            iiin = xstart + iiout;
                            jjin = ystart + jjout;
                            data.image[IDout].array.UI16[jjout*size_x+iiout] = data.image[IDin].array.UI16[jjin*data.image[IDin].md[0].size[0]+iiin];
                        }
                    if(IDmask!=-1)
                        for(ii=0; ii<sizeoutxy; ii++)
                            data.image[IDout].array.UI16[ii] *= (int) data.image[IDmask].array.F[ii];
                }
                else // FLOAT
                {
                    if(IDdark==-1)
                    {
                        for(iiout=0; iiout<size_x; iiout++)
                            for(jjout=0; jjout<size_y; jjout++)
                            {
                                iiin = xstart + iiout;
                                jjin = ystart + jjout;
                                data.image[IDout].array.F[jjout*size_x+iiout] = data.image[IDin].array.UI16[jjin*data.image[IDin].md[0].size[0]+iiin];
                            }
                    }
                    else
                    {
                        for(iiout=0; iiout<size_x; iiout++)
                            for(jjout=0; jjout<size_y; jjout++)
                            {
                                iiin = xstart + iiout;
                                jjin = ystart + jjout;
                                data.image[IDout].array.F[jjout*size_x+iiout] = 1.0*data.image[IDin].array.UI16[jjin*data.image[IDin].md[0].size[0]+iiin] - data.image[IDdark].array.F[jjin*data.image[IDdark].md[0].size[0]+iiin];
                            }
                    }

                    if(IDmask!=-1)
                        for(ii=0; ii<sizeoutxy; ii++)
                            data.image[IDout].array.F[ii] *= data.image[IDmask].array.F[ii];
                }
                data.image[IDout].md[0].cnt0 = cnt0;
                data.image[IDout].md[0].write = 0;
            }
        }
        break;
    case _DATATYPE_FLOAT :
        while(1)
        {
            usleep(50); // OK FOR NOW (NOT USED BY FAST WFS)
            if(data.image[IDin].md[0].cnt0!=cnt0)
            {
                data.image[IDout].md[0].write = 1;
                cnt0 = data.image[IDin].md[0].cnt0;
                if(IDdark==-1)
                {
                    for(iiout=0; iiout<size_x; iiout++)
                        for(jjout=0; jjout<size_y; jjout++)
                        {
                            iiin = xstart + iiout;
                            jjin = ystart + jjout;
                            data.image[IDout].array.F[jjout*size_x+iiout] = data.image[IDin].array.F[jjin*data.image[IDin].md[0].size[0]+iiin];
                        }
                }
                else
                {
                    for(iiout=0; iiout<size_x; iiout++)
                        for(jjout=0; jjout<size_y; jjout++)
                        {
                            iiin = xstart + iiout;
                            jjin = ystart + jjout;
                            data.image[IDout].array.F[jjout*size_x+iiout] = data.image[IDin].array.F[jjin*data.image[IDin].md[0].size[0]+iiin] - data.image[IDdark].array.F[jjin*data.image[IDdark].md[0].size[0]+iiin];
                        }
                }

                if(IDmask!=-1)
                    for(ii=0; ii<sizeoutxy; ii++)
                        data.image[IDout].array.F[ii] *= data.image[IDmask].array.F[ii];

                data.image[IDout].md[0].cnt0 = cnt0;
                data.image[IDout].md[0].write = 0;
            }
        }
        break;
    default :
        printf("ERROR: DATA TYPE NOT SUPPORTED\n");
        exit(0);
        break;
    }
    free(sizeout);

    return(0);
}





/** @brief Read image from WFS camera
 *
 * supports ring buffer
 * puts image from camera buffer aoconfID_wfsim into aoconfID_imWFS1 (supplied by user)
 *
 * RM = 1 if response matrix
 *
 * if normalize == 1, image is normalized by dividing by (total + AOconf[loop].WFSnormfloor)*AOconf[loop].WFSsize
 * if PixelStreamMode = 1, read on semaphore 1, return slice index
 *
 */

int_fast8_t Read_cam_frame(long loop, int RM, int normalize, int PixelStreamMode, int InitSem)
{
    long imcnt;
    long ii;
    double totalinv;
    char name[200];
    int slice;
    char *ptrv;
    long double tmplv1;
    double tmpf;
    long IDdark;
    char dname[200];
    long nelem;
    pthread_t thread_computetotal_id;
    pthread_t thread_dark_subtract[20];
    float resulttotal;
    int sval0, sval;
    void *status = 0;
    long i;
    int semval;
    int s;

    int semindex = 0;




    if(RM==0)
        semindex = 0;
    else
        semindex = 1;


    WFSatype = data.image[aoconfID_wfsim].md[0].atype;

    if(avcamarraysInit==0)
    {
        arrayftmp = (float*) malloc(sizeof(float)*AOconf[loop].sizeWFS);
        arrayutmp = (unsigned short*) malloc(sizeof(unsigned short)*AOconf[loop].sizeWFS);

        if(sprintf(Average_cam_frames_dname, "aol%ld_wfsdark", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        Average_cam_frames_IDdark = image_ID(Average_cam_frames_dname);
        Average_cam_frames_nelem = AOconf[loop].sizeWFS;

        // set semaphore to 0
        sem_getvalue(data.image[aoconfID_wfsim].semptr[semindex], &semval);
        printf("INITIALIZING SEMAPHORE %d   %s   (%d)\n", semindex, data.image[aoconfID_wfsim].md[0].name, semval);
        for(i=0; i<semval; i++)
            sem_trywait(data.image[aoconfID_wfsim].semptr[semindex]);

        //PIXSTREAM_SLICE = data.image[aoconfID_wfsim].md[0].cnt1;    // set semaphore 1 to 0

        avcamarraysInit = 1;
    }

    if(InitSem==1)
    {
        sem_getvalue(data.image[aoconfID_wfsim].semptr[semindex], &semval);
        printf("INITIALIZING SEMAPHORE %d   %s   (%d)\n", semindex, data.image[aoconfID_wfsim].md[0].name, semval);
        for(i=0; i<semval; i++)
            sem_trywait(data.image[aoconfID_wfsim].semptr[semindex]);
    }

#ifdef _PRINT_TEST
    printf("TEST - SEMAPHORE INITIALIZED\n");
    fflush(stdout);
#endif

    if(RM==0)
    {
        AOconf[loop].status = 20;  // 020: WAIT FOR IMAGE
        clock_gettime(CLOCK_REALTIME, &tnow);
        tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
        tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
        data.image[aoconfID_looptiming].array.F[20] = tdiffv;
    }
    else
        data.status1 = 2;

    //   usleep(20);

#ifdef _PRINT_TEST
    printf("TEST - WAITING FOR IMAGE %s\n", data.image[aoconfID_wfsim].md[0].name);
    fflush(stdout);
#endif

    if(data.image[aoconfID_wfsim].md[0].sem==0)
    {
        if(RM==0)
            while(AOconf[loop].WFScnt==data.image[aoconfID_wfsim].md[0].cnt0) // test if new frame exists
                usleep(5);
        else
            while(AOconf[loop].WFScntRM==data.image[aoconfID_wfsim].md[0].cnt0) // test if new frame exists
                usleep(5);
    }
    else
    {
#ifdef _PRINT_TEST
        printf("TEST - waiting on semindex = %d\n", semindex);
        fflush(stdout);
#endif

        sem_wait(data.image[aoconfID_wfsim].semptr[semindex]);

        sem_getvalue(data.image[aoconfID_wfsim].semptr[semindex], &semval);
        for(i=0; i<semval; i++)
            sem_trywait(data.image[aoconfID_wfsim].semptr[semindex]);


#ifdef _PRINT_TEST
        printf("TEST - semaphore posted\n");
        fflush(stdout);
#endif
    }

    if(RM==0)
        AOconf[loop].status = 0;  // LOAD IMAGE

    AOconf[loop].statusM = 0;


    slice = 0;
    if(data.image[aoconfID_wfsim].md[0].naxis==3) // ring buffer
    {
        slice = data.image[aoconfID_wfsim].md[0].cnt1;
        if(slice==-1)
            slice = data.image[aoconfID_wfsim].md[0].size[2];
    }

    switch (WFSatype) {
    case _DATATYPE_FLOAT :
        ptrv = (char*) data.image[aoconfID_wfsim].array.F;
        ptrv += sizeof(float)*slice* AOconf[loop].sizeWFS;
        memcpy(arrayftmp, ptrv,  sizeof(float)*AOconf[loop].sizeWFS);
        break;
    case _DATATYPE_UINT16 :
        ptrv = (char*) data.image[aoconfID_wfsim].array.UI16;
        ptrv += sizeof(unsigned short)*slice* AOconf[loop].sizeWFS;
        memcpy (arrayutmp, ptrv, sizeof(unsigned short)*AOconf[loop].sizeWFS);
        break;
    default :
        printf("ERROR: DATA TYPE NOT SUPPORTED\n");
        exit(0);
        break;
    }
    if(RM==0)
        AOconf[loop].WFScnt = data.image[aoconfID_wfsim].md[0].cnt0;
    else
        AOconf[loop].WFScntRM = data.image[aoconfID_wfsim].md[0].cnt0;


    //   if(COMPUTE_PIXELSTREAMING==1) // multiple pixel groups
    PIXSTREAM_SLICE = data.image[aoconfID_wfsim].md[0].cnt1;


    // THIS IS THE STARTING POINT FOR THE LOOP
    if(RM==0)
    {
        AOconf[loop].status = 1;  // 3->001: DARK SUBTRACT
        clock_gettime(CLOCK_REALTIME, &tnow);
        tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
        tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
        data.image[aoconfID_looptiming].array.F[0] = tdiffv;

        data.image[aoconfID_looptiming].md[0].atime.ts = tnow;
    }

#ifdef _PRINT_TEST
    printf("TEST - DARK SUBTRACT\n");
    fflush(stdout);
#endif

    // Dark subtract and compute total

    if((loop==0)||(RM == 1)) // single thread, in CPU
    {
        switch ( WFSatype ) {
        case _DATATYPE_UINT16 :
# ifdef _OPENMP
            #pragma omp parallel num_threads(8) if (Average_cam_frames_nelem>OMP_NELEMENT_LIMIT)
        {
# endif

# ifdef _OPENMP
            #pragma omp for
# endif
            for(ii=0; ii<Average_cam_frames_nelem; ii++)
                data.image[aoconfID_imWFS0].array.F[ii] = ((float) arrayutmp[ii]) - data.image[Average_cam_frames_IDdark].array.F[ii];
# ifdef _OPENMP
        }
# endif
        break;
        case _DATATYPE_FLOAT :
# ifdef _OPENMP
            #pragma omp parallel num_threads(8) if (Average_cam_frames_nelem>OMP_NELEMENT_LIMIT)
        {
# endif

# ifdef _OPENMP
            #pragma omp for
# endif
            for(ii=0; ii<Average_cam_frames_nelem; ii++)
                data.image[aoconfID_imWFS0].array.F[ii] = arrayftmp[ii] - data.image[Average_cam_frames_IDdark].array.F[ii];
# ifdef _OPENMP
        }
# endif
        break;
        default :
            printf("ERROR: WFS data type not recognized\n");
            exit(0);
            break;
        }

        for(s=0; s<data.image[aoconfID_imWFS0].md[0].sem; s++)
        {
            sem_getvalue(data.image[aoconfID_imWFS0].semptr[s], &semval);
            if(semval<SEMAPHORE_MAXVAL)
                sem_post(data.image[aoconfID_imWFS0].semptr[s]);
        }
    }
    else
    {
#ifdef _PRINT_TEST
        printf("TEST - DARK SUBTRACT - START  (init = %d, %d threads)\n", AOLCOMPUTE_DARK_SUBTRACT_THREADinit, COMPUTE_DARK_SUBTRACT_NBTHREADS);
        fflush(stdout);
#endif

        if(AOLCOMPUTE_DARK_SUBTRACT_THREADinit==0)
        {
#ifdef _PRINT_TEST
            printf("TEST - DARK SUBTRACT - CREATE %d THREADS\n", COMPUTE_DARK_SUBTRACT_NBTHREADS);
            fflush(stdout);
#endif

            ti = 0;

            while(ti<COMPUTE_DARK_SUBTRACT_NBTHREADS)
            {
                pthread_create( &thread_dark_subtract[ti], NULL, compute_function_dark_subtract, (void*) &ti);
                sem_init(&AOLCOMPUTE_DARK_SUBTRACT_sem_name[ti], 0, 0);
                sem_init(&AOLCOMPUTE_DARK_SUBTRACT_RESULT_sem_name[ti], 0, 0);
                usleep(100);
                ti++;
            }
            AOLCOMPUTE_DARK_SUBTRACT_THREADinit = 1;
        }


        for(ti=0; ti<COMPUTE_DARK_SUBTRACT_NBTHREADS; ti++)
        {
            sem_getvalue(&AOLCOMPUTE_DARK_SUBTRACT_sem_name[ti], &sval0);
            if(sval0<SEMAPHORE_MAXVAL)
                sem_post(&AOLCOMPUTE_DARK_SUBTRACT_sem_name[ti]);

            sem_getvalue(&AOLCOMPUTE_DARK_SUBTRACT_sem_name[ti], &sval);

#ifdef _PRINT_TEST
            printf("TEST - DARK SUBTRACT - WAITING ON THREAD %ld\n", ti);
            fflush(stdout);
#endif
            sem_wait(&AOLCOMPUTE_DARK_SUBTRACT_RESULT_sem_name[ti]);
        }

        for(s=0; s<data.image[aoconfID_imWFS0].md[0].sem; s++)
        {
            sem_getvalue(data.image[aoconfID_imWFS0].semptr[s], &semval);
            if(semval<SEMAPHORE_MAXVAL)
                sem_post(data.image[aoconfID_imWFS0].semptr[s]);
        }
#ifdef _PRINT_TEST
        printf("TEST - DARK SUBTRACT - END\n");
        fflush(stdout);
#endif
    }

    //  if(IDdark!=-1)
    // {
    //    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
    //       data.image[aoconfID_imWFS0].array.F[ii] -= data.image[IDdark].array.F[ii];
    //}
    AOconf[loop].statusM = 2;
    if(RM==0)
    {
        AOconf[loop].status = 2; // 4 -> 002 : COMPUTE TOTAL OF IMAGE
        clock_gettime(CLOCK_REALTIME, &tnow);
        tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
        tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
        data.image[aoconfID_looptiming].array.F[2] = tdiffv;
    }

#ifdef _PRINT_TEST
    printf("TEST - NORMALIZE\n");
    fflush(stdout);
#endif


    // Normalize
    if(normalize==1)
    {
        if((AOconf[loop].AOLCOMPUTE_TOTAL_ASYNC==0)||(AOLCOMPUTE_TOTAL_INIT==0)||(RM == 1)) // do it in main thread
        {
            nelem = data.image[aoconfID_imWFS0].md[0].size[0]*data.image[aoconfID_imWFS0].md[0].size[1];
            IMTOTAL = 0.0;
            if(aoconfID_wfsmask!=-1)
            {
                for(ii=0; ii<nelem; ii++)
                    IMTOTAL += data.image[aoconfID_imWFS0].array.F[ii]*data.image[aoconfID_wfsmask].array.F[ii];
            }
            else
            {
                for(ii=0; ii<nelem; ii++)
                    IMTOTAL += data.image[aoconfID_imWFS0].array.F[ii];
            }

            //            AOconf[loop].WFStotalflux = arith_image_total(data.image[aoconfID_imWFS0].name);
            AOconf[loop].WFStotalflux = IMTOTAL;

            AOLCOMPUTE_TOTAL_INIT = 1;
            //            IMTOTAL = AOconf[loop].WFStotalflux;
            if(aoconfID_imWFS0tot!=-1)
            {
                data.image[aoconfID_imWFS0tot].array.F[0] = IMTOTAL;
                COREMOD_MEMORY_image_set_sempost_byID(aoconfID_imWFS0tot, -1);
                //                sem_getvalue(data.image[aoconfID_imWFS0tot].semptr[0], &semval);
                //               if(semval<SEMAPHORE_MAXVAL)
                //                  sem_post(data.image[aoconfID_imWFS0tot].semptr[0]);
            }
        }
        else  // do it in other threads
        {
            AOconf[loop].WFStotalflux = IMTOTAL; // from last loop
            if(AOLCOMPUTE_TOTAL_ASYNC_THREADinit==0)
            {
                pthread_create( &thread_computetotal_id, NULL, compute_function_imtotal, NULL);
                AOLCOMPUTE_TOTAL_ASYNC_THREADinit = 1;
                sem_init(&AOLCOMPUTE_TOTAL_ASYNC_sem_name, 0, 0);
            }
            sem_getvalue(&AOLCOMPUTE_TOTAL_ASYNC_sem_name, &semval);
            if(semval<SEMAPHORE_MAXVAL)
                sem_post(&AOLCOMPUTE_TOTAL_ASYNC_sem_name);
        }
    }


    if(RM==0)
    {
        AOconf[loop].status = 3;  // 5 -> 003: NORMALIZE WFS IMAGE
        clock_gettime(CLOCK_REALTIME, &tnow);
        tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
        tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
        data.image[aoconfID_looptiming].array.F[3] = tdiffv;
    }

    data.image[aoconfID_imWFS0].md[0].cnt0 ++;

    nelem = AOconf[loop].sizeWFS;

    if(normalize==1)
    {
        totalinv=1.0/(AOconf[loop].WFStotalflux + AOconf[loop].WFSnormfloor*AOconf[loop].sizeWFS);
        normfloorcoeff = AOconf[loop].WFStotalflux / (AOconf[loop].WFStotalflux + AOconf[loop].WFSnormfloor*AOconf[loop].sizeWFS);
    }
    else
    {
        totalinv = 1.0;
        normfloorcoeff = 1.0;
    }

    GPU_alpha = totalinv;

    GPU_beta = -normfloorcoeff;





    if( ((COMPUTE_GPU_SCALING==0)&&(RM==0)) || (RM==1))  // normalize WFS image by totalinv
    {
#ifdef _PRINT_TEST
        printf("TEST - Normalize [%d]: totalinv = %f\n", AOconf[loop].WFSnormalize, totalinv);
        fflush(stdout);
#endif

        data.image[aoconfID_imWFS1].md[0].write = 1;
# ifdef _OPENMP
        #pragma omp parallel num_threads(8) if (nelem>OMP_NELEMENT_LIMIT)
        {
# endif

# ifdef _OPENMP
            #pragma omp for
# endif
            for(ii=0; ii<nelem; ii++)
                data.image[aoconfID_imWFS1].array.F[ii] = data.image[aoconfID_imWFS0].array.F[ii]*totalinv;
# ifdef _OPENMP
        }
# endif
        COREMOD_MEMORY_image_set_sempost_byID(aoconfID_imWFS1, -1);
        data.image[aoconfID_imWFS1].md[0].cnt0 ++;
        data.image[aoconfID_imWFS1].md[0].write = 0;
    }

#ifdef _PRINT_TEST
    printf("TEST - READ CAM DONE\n");
    fflush(stdout);
#endif


    return(0);
}




/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 4. ACQUIRING CALIBRATION                                                 */
/* =============================================================================================== */
/* =============================================================================================== */



long AOloopControl_Measure_WFSrespC(long loop, long delayfr, long delayRM1us, long NBave, long NBexcl, const char *IDpokeC_name, const char *IDoutC_name, int normalize, int AOinitMode, long NBcycle)
{
    char fname[200];
    char name[200];
    char command[200];
    uint32_t *sizearray;
    long IDoutC;

    long NBiter = 10000; // runs until USR1 signal received
    long iter;
    int r;
    long IDpokeC;
    long NBpoke;
    long framesize;
    char *ptr0; // source
    float *arrayf;
    int RT_priority = 80; //any number from 0-99
    struct sched_param schedpar;
    int ret;
    long imcnt;
    long ii;

    long imcntmax;
    long *array_iter;
    int *array_poke;
    int *array_accum;
    long *array_kk;
    long *array_kk1;
    long *array_PokeIndex;
    long *array_PokeIndex1;
    FILE *fp;




    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    sched_setscheduler(0, SCHED_FIFO, &schedpar);
#endif


    if(NBcycle < 1)
        NBiter = LONG_MAX; // runs until USR1 signal received
    else
        NBiter = NBcycle;



    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*3);


    printf("INITIALIZE MEMORY (mode %d, meminit = %d)....\n", AOinitMode, AOloopcontrol_meminit);
    fflush(stdout);

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(AOinitMode);

    AOloopControl_loadconfigure(LOOPNUMBER, 1, 2);






    printf("Importing DM response matrix channel shared memory ...\n");
    fflush(stdout);
    aoconfID_dmRM = read_sharedmem_image(AOconf[loop].dmRMname);

    printf("Importing WFS camera image shared memory ... \n");
    fflush(stdout);
    aoconfID_wfsim = read_sharedmem_image(AOconf[loop].WFSname);


    IDpokeC = image_ID(IDpokeC_name);
    NBpoke = data.image[IDpokeC].md[0].size[2];
    sizearray[0] = AOconf[loop].sizexWFS;
    sizearray[1] = AOconf[loop].sizeyWFS;
    sizearray[2] = NBpoke;

    IDoutC = create_3Dimage_ID(IDoutC_name, sizearray[0], sizearray[1], sizearray[2]);


    arrayf = (float*) malloc(sizeof(float)*AOconf[loop].sizeDM);
    for(ii=0; ii<AOconf[loop].sizeDM; ii++)
        arrayf[ii] = 0.0;






    if(sprintf(name, "aol%ld_imWFS1RM", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    sizearray[0] = AOconf[loop].sizexWFS;
    sizearray[1] = AOconf[loop].sizeyWFS;
    printf("WFS size = %ld %ld\n", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    fflush(stdout);
    aoconfID_imWFS1 = create_image_ID(name, 2, sizearray, _DATATYPE_FLOAT, 1, 0);



    uint_fast16_t PokeIndex;
    for(PokeIndex = 0; PokeIndex < NBpoke; PokeIndex++)
        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            data.image[IDoutC].array.F[PokeIndex*AOconf[loop].sizeWFS+ii] = 0.0;


    iter = 0;


    ptr0 = (char*) data.image[IDpokeC].array.F;
    framesize = sizeof(float)*AOconf[loop].sizexDM*AOconf[loop].sizeyDM;

    printf("STARTING response measurement...\n");
    fflush(stdout);


    imcntmax = (4+delayfr+(NBave+NBexcl)*NBpoke)*NBiter + 4;
    array_iter = (long*) malloc(sizeof(long)*imcntmax);
    array_poke = (int*) malloc(sizeof(int)*imcntmax);
    array_accum = (int*) malloc(sizeof(int)*imcntmax);
    array_kk = (long*) malloc(sizeof(long)*imcntmax);
    array_kk1 = (long*) malloc(sizeof(long)*imcntmax);
    array_PokeIndex = (long*) malloc(sizeof(long)*imcntmax);
    array_PokeIndex1 = (long*) malloc(sizeof(long)*imcntmax);

    for(imcnt=0; imcnt<imcntmax; imcnt++)
    {
        array_poke[imcnt] = 0;
        array_accum[imcnt] = 0;
    }

    imcnt = 0;

    while((iter<NBiter)&&(data.signal_USR1==0)&&(data.signal_USR2==0))
    {
        printf("iteration # %8ld / %8ld   ( %6ld / %6ld )  \n", iter, NBiter, imcnt, imcntmax);
        fflush(stdout);


        // initialize with first poke
        long kk = 0;
        long kk1 = 0;
        uint_fast16_t PokeIndex = 0;
        uint_fast16_t PokeIndex1 = 0;


        usleep(delayRM1us);
        data.image[aoconfID_dmRM].md[0].write = 1;
        memcpy (data.image[aoconfID_dmRM].array.F, ptr0 + PokeIndex1*framesize, sizeof(float)*AOconf[loop].sizeDM);
        COREMOD_MEMORY_image_set_sempost_byID(aoconfID_dmRM, -1);
        data.image[aoconfID_dmRM].md[0].cnt1 = PokeIndex1;
        data.image[aoconfID_dmRM].md[0].cnt0++;
        data.image[aoconfID_dmRM].md[0].write = 0;
        AOconf[loop].DMupdatecnt ++;
        array_poke[imcnt] = 1;


        // WAIT FOR LOOP DELAY, PRIMING
        array_iter[imcnt] = iter;
        array_kk[imcnt] = kk;
        array_kk1[imcnt] = kk1;
        array_PokeIndex[imcnt] = PokeIndex;
        array_PokeIndex1[imcnt] = PokeIndex1;
        imcnt ++;

        Read_cam_frame(loop, 1, normalize, 0, 0);


        COREMOD_MEMORY_image_set_sempost_byID(aoconfID_dmRM, -1);
        data.image[aoconfID_dmRM].md[0].cnt0++;




        // read delayfr frames
        for(kk=0; kk<delayfr; kk++)
        {
            array_iter[imcnt] = iter;
            array_kk[imcnt] = kk;
            array_kk1[imcnt] = kk1;
            array_PokeIndex[imcnt] = PokeIndex;
            array_PokeIndex1[imcnt] = PokeIndex1;
            imcnt ++;

            Read_cam_frame(loop, 1, normalize, 0, 0);

            kk1++;
            if(kk1==NBave)
            {
                kk1 = -NBexcl;
                PokeIndex1++;

                if(PokeIndex1>NBpoke-1)
                    PokeIndex1 -= NBpoke;

                // POKE
                usleep(delayRM1us);
                data.image[aoconfID_dmRM].md[0].write = 1;
                memcpy (data.image[aoconfID_dmRM].array.F, ptr0 + PokeIndex1*framesize, sizeof(float)*AOconf[loop].sizeDM);
                COREMOD_MEMORY_image_set_sempost_byID(aoconfID_dmRM, -1);
                data.image[aoconfID_dmRM].md[0].cnt1 = PokeIndex1;
                data.image[aoconfID_dmRM].md[0].cnt0++;
                data.image[aoconfID_dmRM].md[0].write = 0;
                AOconf[loop].DMupdatecnt ++;
                array_poke[imcnt] = 1;
            }
        }





        while ((PokeIndex < NBpoke)&&(data.signal_USR1==0))
        {
            // INTEGRATION

            for(kk=0; kk<NBave+NBexcl; kk++)
            {
                array_iter[imcnt] = iter;
                array_kk[imcnt] = kk;
                array_kk1[imcnt] = kk1;
                array_PokeIndex[imcnt] = PokeIndex;
                array_PokeIndex1[imcnt] = PokeIndex1;
                imcnt ++;

                Read_cam_frame(loop, 1, normalize, 0, 0);


                if(kk<NBave)
                {
                    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                        data.image[IDoutC].array.F[PokeIndex*AOconf[loop].sizeWFS+ii] += data.image[aoconfID_imWFS1].array.F[ii];
                    array_accum[imcnt] = 1;
                }
                kk1++;
                if(kk1==NBave)
                {
                    kk1 = -NBexcl;
                    PokeIndex1++;

                    if(PokeIndex1>NBpoke-1)
                        PokeIndex1 -= NBpoke;


                    usleep(delayRM1us);
                    data.image[aoconfID_dmRM].md[0].write = 1;
                    memcpy (data.image[aoconfID_dmRM].array.F, ptr0 + PokeIndex1*framesize, sizeof(float)*AOconf[loop].sizeDM);
                    COREMOD_MEMORY_image_set_sempost_byID(aoconfID_dmRM, -1);
                    data.image[aoconfID_dmRM].md[0].cnt1 = PokeIndex1;
                    data.image[aoconfID_dmRM].md[0].cnt0++;
                    data.image[aoconfID_dmRM].md[0].write = 0;
                    AOconf[loop].DMupdatecnt ++;
                    array_poke[imcnt] = 1;
                }
            }

            PokeIndex++;
        }


        for(ii=0; ii<AOconf[loop].sizeDM; ii++)
            arrayf[ii] = 0.0;

        // zero DM channel

        usleep(delayRM1us);
        data.image[aoconfID_dmRM].md[0].write = 1;
        memcpy (data.image[aoconfID_dmRM].array.F, arrayf, sizeof(float)*AOconf[loop].sizeDM);
        COREMOD_MEMORY_image_set_sempost_byID(aoconfID_dmRM, -1);
        data.image[aoconfID_dmRM].md[0].cnt1 = 0;
        data.image[aoconfID_dmRM].md[0].cnt0++;
        data.image[aoconfID_dmRM].md[0].write = 0;
        AOconf[loop].DMupdatecnt ++;
        array_poke[imcnt] = 1;

        iter++;

    } // end of iteration loop

    free(arrayf);

    free(sizearray);

    for(PokeIndex = 0; PokeIndex < NBpoke; PokeIndex++)
        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            data.image[IDoutC].array.F[PokeIndex*AOconf[loop].sizeWFS+ii] /= NBave*iter;


    // print poke log
    fp = fopen("RMpokelog.txt", "w");
    for(imcnt=0; imcnt<imcntmax; imcnt++)
    {
        fprintf(fp, "%6ld %3ld    %1d %1d     %6ld  %6ld  %6ld  %6ld     %3ld %3ld %3ld\n", imcnt, array_iter[imcnt], array_poke[imcnt], array_accum[imcnt], array_kk[imcnt], array_kk1[imcnt], array_PokeIndex[imcnt], array_PokeIndex1[imcnt], NBpoke, NBexcl, NBave);
    }
    fclose(fp);


    free(array_iter);
    free(array_accum);
    free(array_poke);
    free(array_kk);
    free(array_kk1);
    free(array_PokeIndex);
    free(array_PokeIndex1);


    return(IDoutC);
}




//
// Measure the WFS linear response to a set of DM patterns
//
long AOloopControl_Measure_WFS_linResponse(long loop, float ampl, long delayfr, long delayRM1us, long NBave, long NBexcl, const char *IDpokeC_name, const char *IDrespC_name, const char *IDwfsref_name, int normalize, int AOinitMode, long NBcycle)
{
    long IDrespC;
    long IDpokeC;
    long dmxsize, dmysize, dmxysize;
    long wfsxsize, wfsysize, wfsxysize;
    long NBpoke, NBpoke2;
    long IDpokeC2;
    long IDwfsresp2;
    long poke, act, pix;
    long IDwfsref;



    IDpokeC = image_ID(IDpokeC_name);
    dmxsize = data.image[IDpokeC].md[0].size[0];
    dmysize = data.image[IDpokeC].md[0].size[1];
    dmxysize = dmxsize*dmysize;
    NBpoke = data.image[IDpokeC].md[0].size[2];

    NBpoke2 = 2*NBpoke + 4; // add zero frame before and after


    IDpokeC2 = create_3Dimage_ID("dmpokeC2", dmxsize, dmysize, NBpoke2);

    for(act=0; act<dmxysize; act++)
    {
        data.image[IDpokeC2].array.F[act] = 0.0;
        data.image[IDpokeC2].array.F[dmxysize + act] = 0.0;
        data.image[IDpokeC2].array.F[dmxysize*(2*NBpoke+2) + act] = 0.0;
        data.image[IDpokeC2].array.F[dmxysize*(2*NBpoke+3) + act] = 0.0;

    }

    for(poke=0; poke<NBpoke; poke++)
    {
        for(act=0; act<dmxysize; act++)
            data.image[IDpokeC2].array.F[dmxysize*(2*poke+2) + act] = ampl*data.image[IDpokeC].array.F[dmxysize*poke+act];
        for(act=0; act<dmxysize; act++)
            data.image[IDpokeC2].array.F[dmxysize*(2*poke+2) + dmxysize + act] = -ampl*data.image[IDpokeC].array.F[dmxysize*poke+act];
    }
    //	save_fits("dmpokeC2", "!tmp/test_dmpokeC2.fits");

    printf("NBpoke = %ld\n", NBpoke);
    fflush(stdout);

    AOloopControl_Measure_WFSrespC(loop, delayfr, delayRM1us, NBave, NBexcl, "dmpokeC2", "wfsresp2", normalize, AOinitMode, NBcycle);

    printf("STEP done\n");
    fflush(stdout);

    //	save_fits("wfsresp2", "!tmp/test_wfsresp2.fits");


    // process data cube
    IDwfsresp2 = image_ID("wfsresp2");
    wfsxsize = data.image[IDwfsresp2].md[0].size[0];
    wfsysize = data.image[IDwfsresp2].md[0].size[1];
    wfsxysize = wfsxsize*wfsysize;
    IDrespC = create_3Dimage_ID(IDrespC_name, wfsxsize, wfsysize, NBpoke);

    IDwfsref = create_2Dimage_ID(IDwfsref_name, wfsxsize, wfsysize);

    for(poke=0; poke<NBpoke; poke++)
    {
        for(pix=0; pix<wfsxysize; pix++)
            data.image[IDrespC].array.F[wfsxysize*poke + pix] = (data.image[IDwfsresp2].array.F[wfsxysize*(2*poke+2) + pix] - data.image[IDwfsresp2].array.F[wfsxysize*(2*poke+2) + wfsxysize + pix])/2.0/ampl;
        for(pix=0; pix<wfsxysize; pix++)
            data.image[IDwfsref].array.F[pix] += (data.image[IDwfsresp2].array.F[wfsxysize*(2*poke+2) + pix] + data.image[IDwfsresp2].array.F[wfsxysize*(2*poke+2) + wfsxysize + pix])/(2*NBpoke);
    }


    return(IDrespC);
}




/** Measures zonal response matrix
 * -> collapses it to DM response map and WFS response map
 * (both maps show amplitude of actuator effect on WFS)
 *
 * mode :
 *  0: compute WFSmap and DMmap
 *  1: compute WFSmap, DMmap, WFSmask and DMmask  -> images wfsmask and dmmask
 * NOTE can take custom poke matrix (loaded in image name RMpokeCube)
 *
 * ASYNC = 1  -> record ALL frames and assemble the RM off-line
 *
 * AOinitMode = 0:  create AO shared mem struct
 * AOinitMode = 1:  connect only to AO shared mem struct
 *  */

long AOloopControl_Measure_zonalRM(long loop, double ampl, long delayfr, long delayRM1us, long NBave, long NBexcl, const char *zrespm_name, const char *WFSref0_name, const char *WFSmap_name, const char *DMmap_name, long mode, int normalize, int AOinitMode, long NBcycle)
{
    long ID_WFSmap, ID_WFSref0, ID_WFSref2, ID_DMmap, IDmapcube, IDzrespm, IDzrespmn, ID_WFSref0n,  ID_WFSref2n;
    long act, j, ii, kk;
    double value;
    float *arrayf;
    char fname[200];
    char name[200];
    char command[200];
    long IDpos, IDneg;
    float tot, v1, rms;
    uint32_t *sizearray;

    long NBiter = LONG_MAX; // runs until USR1 signal received
    long iter;
    float *arraypix;
    long i;
    long istart, iend, icnt;
    long cntn;
    double tmpv;

    long ID_WFSmask, ID_DMmask;
    float lim;
    double total;
    int r;
    long IDzrespfp, IDzrespfm;
    long IDpokeC;
    long NBpoke;

    int RT_priority = 80; //any number from 0-99
    struct sched_param schedpar;
    int ret;
    long act2;

    long *actarray;
    long poke, poke1, poke2;




    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    sched_setscheduler(0, SCHED_FIFO, &schedpar);
#endif


    if(NBcycle < 1)
        NBiter = LONG_MAX; // runs until USR1 signal received
    else
        NBiter = NBcycle;



    arraypix = (float*) malloc(sizeof(float)*NBiter);
    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*3);





    printf("INITIALIZE MEMORY (mode %d)....\n", AOinitMode);
    fflush(stdout);



    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(AOinitMode);
    fflush(stdout);



    //  sprintf(fname, "./conf/AOloop.conf");

    printf("LOAD/CONFIGURE loop ...\n");
    fflush(stdout);


    AOloopControl_loadconfigure(LOOPNUMBER, 1, 2);



    printf("Importing DM response matrix channel shared memory ...\n");
    fflush(stdout);
    aoconfID_dmRM = read_sharedmem_image(AOconf[loop].dmRMname);



    printf("Importing WFS camera image shared memory ... \n");
    fflush(stdout);
    aoconfID_wfsim = read_sharedmem_image(AOconf[loop].WFSname);



    if(sprintf(name, "aol%ld_imWFS1RM", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    sizearray[0] = AOconf[loop].sizexWFS;
    sizearray[1] = AOconf[loop].sizeyWFS;
    printf("WFS size = %ld %ld\n", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    fflush(stdout);
    aoconfID_imWFS1 = create_image_ID(name, 2, sizearray, _DATATYPE_FLOAT, 1, 0);


    arrayf = (float*) malloc(sizeof(float)*AOconf[loop].sizeDM);

    sizearray[0] = AOconf[loop].sizexDM;
    sizearray[1] = AOconf[loop].sizeyDM;
    ID_DMmap = create_image_ID(DMmap_name, 2, sizearray, _DATATYPE_FLOAT, 1, 5);


    IDpos = create_2Dimage_ID("wfsposim", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    IDneg = create_2Dimage_ID("wfsnegim", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);



    IDpokeC = image_ID("RMpokeCube");
    if(IDpokeC==-1)
    {
        IDpokeC = create_3Dimage_ID("RMpokeCube", AOconf[loop].sizexDM, AOconf[loop].sizeyDM, AOconf[loop].sizexDM*AOconf[loop].sizeyDM);
        for(act=0; act<AOconf[loop].sizexDM*AOconf[loop].sizeyDM; act++)
        {
            for(ii=0; ii<AOconf[loop].sizexDM*AOconf[loop].sizeyDM; ii++)
                data.image[IDpokeC].array.F[act*AOconf[loop].sizexDM*AOconf[loop].sizeyDM+ii] = 0.0;
            data.image[IDpokeC].array.F[act*AOconf[loop].sizexDM*AOconf[loop].sizeyDM+act] = 1.0;
        }
        //        save_fits("RMpokeCube", "!./conf/RMpokeCube.fits");
        save_fits("RMpokeCube", "!./conf/zRMpokeCube.fits");

        NBpoke = data.image[IDpokeC].md[0].size[2];
    }
    else
    {
        //NBpoke = AOconf[loop].sizeDM;
        NBpoke = data.image[IDpokeC].md[0].size[2];
    }

    //    save_fits("RMpokeCube", "!./conf/test1_RMpokeCube.fits");

    if(sprintf(command, "echo \"%ld\" > RM_NBpoke.txt\n", NBpoke) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    if(system(command) != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    if(sprintf(command, "echo \"%ld\" > test_RM_NBpoke.txt\n", NBpoke) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
    if(system(command) != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    //    sleep(10);

    sizearray[0] = AOconf[loop].sizexWFS;
    sizearray[1] = AOconf[loop].sizeyWFS;
    sizearray[2] = NBpoke; //AOconf[loop].sizeDM;

    actarray = (long*) malloc(sizeof(long)*NBpoke);

    ID_WFSmap = create_image_ID(WFSmap_name, 2, sizearray, _DATATYPE_FLOAT, 1, 5);
    ID_WFSref0 = create_image_ID("tmpwfsref0", 2, sizearray, _DATATYPE_FLOAT, 1, 5);
    ID_WFSref2 = create_image_ID("tmpwfsref2", 2, sizearray, _DATATYPE_FLOAT, 1, 5);
    ID_WFSref0n = create_image_ID(WFSref0_name, 2, sizearray, _DATATYPE_FLOAT, 1, 5);
    ID_WFSref2n = create_image_ID("tmpwfsimrms", 2, sizearray, _DATATYPE_FLOAT, 1, 5);
    IDzrespm = create_image_ID("zrespm", 3, sizearray, _DATATYPE_FLOAT, 0, 5); // Zonal response matrix
    IDzrespmn = create_image_ID(zrespm_name, 3, sizearray, _DATATYPE_FLOAT, 0, 5); // Zonal response matrix normalized

    IDzrespfp = create_image_ID("zrespfp", 3, sizearray, _DATATYPE_FLOAT, 0, 5); // positive poke image
    IDzrespfm = create_image_ID("zrespfm", 3, sizearray, _DATATYPE_FLOAT, 0, 5); // negative poke image

    if(mode>0)
    {
        sizearray[0] = AOconf[loop].sizexWFS;
        sizearray[1] = AOconf[loop].sizeyWFS;
        ID_WFSmask = create_image_ID("wfsmask", 2, sizearray, _DATATYPE_FLOAT, 1, 5);

        sizearray[0] = AOconf[loop].sizexDM;
        sizearray[1] = AOconf[loop].sizeyDM;
        ID_DMmask = create_image_ID("dmmask", 2, sizearray, _DATATYPE_FLOAT, 1, 5);
    }


    cntn = 0;
    iter = 0;


    printf("Clearing directory files\n");
    fflush(stdout);

    //    for(iter=0; iter<NBiter; iter++)
    if(system("mkdir -p zresptmp") != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    if(system("rm ./zresptmp/LO*.fits") != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    if(sprintf(command, "echo %ld > ./zresptmp/%s_nbiter.txt", iter, zrespm_name) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if(system(command) != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");


    printf("STARTING RM...\n");
    fflush(stdout);

    while((iter<NBiter)&&(data.signal_USR1==0))
    {
        printf("iteration # %8ld    \n", iter);
        fflush(stdout);


        // permut actarray
        for(poke=0; poke<NBpoke; poke++)
            actarray[poke] = poke;

        for(poke=0; poke<NBpoke; poke++)
        {
            poke1 = (long) (ran1()*NBpoke);
            if(poke1>=NBpoke)
                poke1 = NBpoke-1;
            if(poke!=poke1)
            {
                poke2 = actarray[poke1];
                actarray[poke1] = actarray[poke];
                actarray[poke] = poke2;
            }
        }



        for(poke=0; poke<NBpoke; poke++)
            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                data.image[IDzrespm].array.F[poke*AOconf[loop].sizeWFS+ii] = 0.0;


        act = 0;

        long kk1 = 0;
        int PokeSign = 1;
        long act1 = 0;



        // initialize with first positive poke
        for(j=0; j<AOconf[loop].sizeDM; j++)
            arrayf[j] = ampl*data.image[IDpokeC].array.F[actarray[act1]*AOconf[loop].sizeDM+j];


        usleep(delayRM1us);
        data.image[aoconfID_dmRM].md[0].write = 1;
        memcpy (data.image[aoconfID_dmRM].array.F, arrayf, sizeof(float)*AOconf[loop].sizeDM);
        data.image[aoconfID_dmRM].md[0].cnt0++;
        data.image[aoconfID_dmRM].md[0].write = 0;
        AOconf[loop].DMupdatecnt ++;



        // WAIT FOR LOOP DELAY, PRIMING
        Read_cam_frame(loop, 1, normalize, 0, 1);

        // read delayfr frames
        for(kk=0; kk<delayfr; kk++)
        {
            Read_cam_frame(loop, 1, normalize, 0, 0);
            kk1++;
            if(kk1==NBave)
            {
                kk1 = -NBexcl;
                if(PokeSign==1)
                    PokeSign = -1;
                else
                {
                    act1++;
                    PokeSign = 1;
                }

                if(act1>NBpoke-1)
                    act1 = NBpoke-1;
                // POKE
                for(j=0; j<AOconf[loop].sizeDM; j++)
                    arrayf[j] = ampl*PokeSign*data.image[IDpokeC].array.F[actarray[act1]*AOconf[loop].sizeDM+j];

                usleep(delayRM1us);
                data.image[aoconfID_dmRM].md[0].write = 1;
                memcpy (data.image[aoconfID_dmRM].array.F, arrayf, sizeof(float)*AOconf[loop].sizeDM);
                data.image[aoconfID_dmRM].md[0].cnt0++;
                data.image[aoconfID_dmRM].md[0].write = 0;
                AOconf[loop].DMupdatecnt ++;
            }
        }





        while ((act < NBpoke)&&(data.signal_USR1==0))
        {
            //	printf("act = %6ld   NBpoke = %6ld\n", act, NBpoke);
            //	fflush(stdout);
            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            {
                data.image[IDpos].array.F[ii] = 0.0;
                data.image[IDneg].array.F[ii] = 0.0;
            }

            // POSITIVE INTEGRATION
            //  printf("POSITIVE INTEGRATION\n");
            //  fflush(stdout);
            for(kk=0; kk<NBave+NBexcl; kk++)
            {
                Read_cam_frame(loop, 1, normalize, 0, 0);
                if(kk<NBave)
                    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                        data.image[IDpos].array.F[ii] += data.image[aoconfID_imWFS1].array.F[ii];
                kk1++;
                if(kk1==NBave)
                {
                    kk1 = -NBexcl;
                    if(PokeSign==1)
                        PokeSign = -1;
                    else
                    {
                        act1++;
                        PokeSign = 1;
                    }
                    if(act1>NBpoke-1)
                        act1 = NBpoke-1;
                    // POKE
                    for(j=0; j<AOconf[loop].sizeDM; j++)
                        arrayf[j] = ampl*PokeSign*data.image[IDpokeC].array.F[actarray[act1]*AOconf[loop].sizeDM+j];

                    usleep(delayRM1us);
                    data.image[aoconfID_dmRM].md[0].write = 1;
                    memcpy (data.image[aoconfID_dmRM].array.F, arrayf, sizeof(float)*AOconf[loop].sizeDM);
                    data.image[aoconfID_dmRM].md[0].cnt0++;
                    data.image[aoconfID_dmRM].md[0].write = 0;
                    AOconf[loop].DMupdatecnt ++;
                }
            }


            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            {
                data.image[IDzrespm].array.F[actarray[act]*AOconf[loop].sizeWFS+ii] += data.image[IDpos].array.F[ii];
                data.image[IDzrespfp].array.F[actarray[act]*AOconf[loop].sizeWFS+ii] = data.image[IDpos].array.F[ii];
                data.image[ID_WFSref0].array.F[ii] += data.image[IDpos].array.F[ii];
                data.image[ID_WFSref2].array.F[ii] += data.image[IDpos].array.F[ii]*data.image[IDpos].array.F[ii];
            }

            // NEGATIVE INTEGRATION
            //   printf("NEGATIVE INTEGRATION\n");
            //   fflush(stdout);
            for(kk=0; kk<NBave+NBexcl; kk++)
            {
                Read_cam_frame(loop, 1, normalize, 0, 0);
                if(kk<NBave)
                    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                        data.image[IDneg].array.F[ii] += data.image[aoconfID_imWFS1].array.F[ii];
                kk1++;
                if(kk1==NBave)
                {
                    kk1 = -NBexcl;
                    if(PokeSign==1)
                        PokeSign = -1;
                    else
                    {
                        act1++;
                        PokeSign = 1;
                    }
                    if(act1>NBpoke-1)
                        act1 = NBpoke-1;
                    // POKE
                    for(j=0; j<AOconf[loop].sizeDM; j++)
                        arrayf[j] = ampl*PokeSign*data.image[IDpokeC].array.F[actarray[act1]*AOconf[loop].sizeDM+j];

                    usleep(delayRM1us);
                    data.image[aoconfID_dmRM].md[0].write = 1;
                    memcpy (data.image[aoconfID_dmRM].array.F, arrayf, sizeof(float)*AOconf[loop].sizeDM);
                    data.image[aoconfID_dmRM].md[0].cnt0++;
                    data.image[aoconfID_dmRM].md[0].write = 0;
                    AOconf[loop].DMupdatecnt ++;
                }
            }

            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            {
                data.image[IDzrespm].array.F[actarray[act]*AOconf[loop].sizeWFS+ii] -= data.image[IDneg].array.F[ii];
                data.image[IDzrespfm].array.F[actarray[act]*AOconf[loop].sizeWFS+ii] = data.image[IDneg].array.F[ii];
                data.image[ID_WFSref0].array.F[ii] += data.image[IDneg].array.F[ii];
                data.image[ID_WFSref2].array.F[ii] += data.image[IDneg].array.F[ii] * data.image[IDneg].array.F[ii];
            }

            act++;
        }
        cntn = 2*NBave; // Number of images


        for(j=0; j<AOconf[loop].sizeDM; j++)
            arrayf[j] = 0.0;

        usleep(delayRM1us);
        data.image[aoconfID_dmRM].md[0].write = 1;
        memcpy (data.image[aoconfID_dmRM].array.F, arrayf, sizeof(float)*AOconf[loop].sizeDM);
        data.image[aoconfID_dmRM].md[0].cnt0++;
        data.image[aoconfID_dmRM].md[0].write = 0;
        AOconf[loop].DMupdatecnt ++;


        if(data.signal_USR1==0) // keep looping
        {
            for(act=0; act<NBpoke; act++)
                for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                    data.image[IDzrespmn].array.F[act*AOconf[loop].sizeWFS+ii] = data.image[IDzrespm].array.F[actarray[act]*AOconf[loop].sizeWFS+ii]/ampl/cntn;
            if(sprintf(fname, "!./zresptmp/%s_%03ld.fits", zrespm_name, iter) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
            save_fits(zrespm_name, fname);

            for(act=0; act<NBpoke; act++)
                for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                {
                    data.image[IDzrespfp].array.F[act*AOconf[loop].sizeWFS+ii] /= NBave;
                    data.image[IDzrespfm].array.F[act*AOconf[loop].sizeWFS+ii] /= NBave;
                }

            if(sprintf(fname, "!./zresptmp/%s_pos_%03ld.fits", zrespm_name, iter) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits("zrespfp", fname);

            if(sprintf(fname, "!./zresptmp/%s_neg_%03ld.fits", zrespm_name, iter) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits("zrespfm", fname);

            total = 0.0;
            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            {
                data.image[ID_WFSref2n].array.F[ii] = sqrt((data.image[ID_WFSref2].array.F[ii] - data.image[ID_WFSref0].array.F[ii]*data.image[ID_WFSref0].array.F[ii])/NBave/cntn);
                data.image[ID_WFSref0n].array.F[ii] = data.image[ID_WFSref0].array.F[ii]/NBave/cntn;
                total += data.image[ID_WFSref0n].array.F[ii];
            }



            if(normalize==1)
            {
                for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                {
                    data.image[ID_WFSref0n].array.F[ii] /= total;
                    data.image[ID_WFSref2n].array.F[ii] /= total;
                }
            }
            else
            {
                for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                {
                    data.image[ID_WFSref0n].array.F[ii] /= NBave;
                    data.image[ID_WFSref2n].array.F[ii] /= NBave;
                }
            }

            if(sprintf(fname, "!./zresptmp/%s_%03ld.fits", WFSref0_name, iter) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits(WFSref0_name, fname);

            if(sprintf(fname, "!./zresptmp/wfsimRMS.fits") < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits("wfsimrms", fname);


            if(mode!=3)
            {
                for(poke=0; poke<NBpoke; poke++)
                {
                    rms = 0.0;
                    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                    {
                        tmpv = data.image[IDzrespmn].array.F[poke*AOconf[loop].sizeWFS+ii];
                        rms += tmpv*tmpv;
                    }
                    data.image[ID_DMmap].array.F[act] = rms;
                }

                if(sprintf(fname, "!./zresptmp/%s_%03ld.fits", DMmap_name, iter) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                save_fits(DMmap_name, fname);


                for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                {
                    rms = 0.0;
                    for(poke=0; poke<NBpoke; poke++)
                    {
                        tmpv = data.image[IDzrespmn].array.F[poke*AOconf[loop].sizeWFS+ii];
                        rms += tmpv*tmpv;
                    }
                    data.image[ID_WFSmap].array.F[ii] = rms;
                }

                if(sprintf(fname, "!./zresptmp/%s_%03ld.fits", zrespm_name, iter) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                save_fits(WFSmap_name, fname);

                if(mode>0) // compute WFSmask and DMmask
                {
                    // WFSmask : select pixels >40% of 85-percentile
                    lim = 0.4*img_percentile(WFSmap_name, 0.7);
                    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                    {
                        if(data.image[ID_WFSmap].array.F[ii]<lim)
                            data.image[ID_WFSmask].array.F[ii] = 0.0;
                        else
                            data.image[ID_WFSmask].array.F[ii] = 1.0;
                    }

                    // DMmask: select pixels >10% of 50-percentile
                    lim = 0.1*img_percentile(DMmap_name, 0.5);
                    for(act=0; act<AOconf[loop].sizeDM; act++)
                    {
                        if(data.image[ID_DMmap].array.F[act]<lim)
                            data.image[ID_DMmask].array.F[act] = 0.0;
                        else
                            data.image[ID_DMmask].array.F[act] = 1.0;
                    }
                }
            }
            iter++;
            if(sprintf(command, "echo %ld > ./zresptmp/%s_nbiter.txt", iter, zrespm_name) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(system(command) != 0)
                printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");
        }
    } // end of iteration loop

    free(arrayf);
    free(sizearray);
    free(arraypix);

    free(actarray);

    delete_image_ID("tmpwfsref0");


    return(ID_WFSmap);
}



/** measures response matrix AND reference */
// scan delay up to fDelay

int_fast8_t Measure_Resp_Matrix(long loop, long NbAve, float amp, long nbloop, long fDelay, long NBiter)
{
    long NBloops;
    long kloop;
    long delayus = 10000; // delay in us
    long ii, i, imax;
    int Verbose = 0;
    long k1, k, k2;
    char fname[200];
    char name0[200];
    char name[200];

    long kk;
    long RespMatNBframes;
    long IDrmc;
    long kc;

    long IDeigenmodes;

    long double RMsig;
    long double RMsigold;
    long kc0;
    FILE *fp;
    long NBexcl = 2; // number of frames excluded between DM mode changes
    long kc0min, kc0max;
    long IDrmtest;
    int vOK;

    long iter;
    long IDrmi;
    float beta = 0.0;
    float gain = 0.0001;
    long IDrmcumul;
    long IDrefi;
    long IDrefcumul;

    uint32_t *sizearray;

    long IDrespM;
    long IDwfsref0;

    long IDoptsignal; // optical signal for each mode, cumulative
    long IDoptsignaln; // optical signal for each mode, normalize
    long IDmcoeff; // multiplicative gain to amplify low-oder modes
    long IDoptcnt;
    double rmsval;
    char signame[200];

    double normcoeff, normcoeffcnt;


    int AdjustAmplitude = 0;
    char command[2000];

    float valave;
    long IDrmc1;



    RMACQUISITION = 1;


    printf("ACQUIRE RESPONSE MATRIX - loop = %ld, NbAve = %ld, amp = %f, nbloop = %ld, fDelay = %ld, NBiter = %ld\n", loop, NbAve, amp, nbloop, fDelay, NBiter);

    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*3);



    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(0);
    //   sprintf(fname, "./conf/AOloop.conf");
    AOloopControl_loadconfigure(LOOPNUMBER, 1, 10);


    // create output
    IDwfsref0 = create_2Dimage_ID("refwfsacq", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    IDrespM = create_3Dimage_ID("respmacq", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].NBDMmodes);




    IDoptsignal = create_2Dimage_ID("optsig", AOconf[loop].NBDMmodes, 1);
    IDoptsignaln = create_2Dimage_ID("optsign", AOconf[loop].NBDMmodes, 1);
    IDmcoeff = create_2Dimage_ID("mcoeff", AOconf[loop].NBDMmodes, 1);
    IDoptcnt = create_2Dimage_ID("optsigcnt", AOconf[loop].NBDMmodes, 1);

    for(k=0; k<AOconf[loop].NBDMmodes; k++)
    {
        data.image[IDoptcnt].array.F[k] = 0.0;
        data.image[IDoptsignal].array.F[k] = 0.0;
        data.image[IDoptsignaln].array.F[k] = 0.0;
        data.image[IDmcoeff].array.F[k] = 1.0;
    }


    RespMatNBframes = 2*AOconf[loop].NBDMmodes*NbAve;  // *nbloop
    printf("%ld frames total\n", RespMatNBframes);
    fflush(stdout);

    IDrmc = create_3Dimage_ID("RMcube", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, RespMatNBframes); // this is the main cube





    IDrmi = create_3Dimage_ID("RMiter", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].NBDMmodes);    // Response matrix for 1 iteration
    IDrmcumul = create_3Dimage_ID("RMcumul", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].NBDMmodes);  // Cumulative Response matrix

    IDrefi = create_2Dimage_ID("REFiter", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    IDrefcumul = create_2Dimage_ID("REFcumul", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);



    /// local arrays for image acquision
    //	aoconfID_wfsim = create_2Dimage_ID("RMwfs", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    aoconfID_imWFS0 = create_2Dimage_ID("RMwfs0", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    aoconfID_imWFS1 = create_2Dimage_ID("RMwfs1", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    aoconfID_imWFS2 = create_2Dimage_ID("RMwfs2", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);


    aoconfID_cmd_modesRM = create_2Dimage_ID("RMmodesloc", AOconf[loop].NBDMmodes, 1);


    for(iter=0; iter<NBiter; iter++)
    {
        if (file_exists("stopRM.txt"))
        {
            if(system("rm stopRM.txt") != 0)
                printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

            iter = NBiter;
        }
        else
        {
            NBloops = nbloop;


            // initialize reference to zero
            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                data.image[IDrefi].array.F[ii] = 0.0;

            for(ii=0; ii<AOconf[loop].sizeWFS*RespMatNBframes; ii++)
                data.image[IDrmc].array.F[ii] = 0.0;


            //            printf("\n");
            //            printf("Testing (in measure_resp_matrix function) :,  NBloops = %ld, NBmode = %ld\n",  NBloops, AOconf[loop].NBDMmodes);
            //            fflush(stdout);
            //            sleep(1);

            for(k2 = 0; k2 < AOconf[loop].NBDMmodes; k2++)
                data.image[aoconfID_cmd_modesRM].array.F[k2] = 0.0;



            // set DM to last mode, neg
            k1 = AOconf[loop].NBDMmodes-1;
            data.image[aoconfID_cmd_modesRM].array.F[k1] = -amp*data.image[IDmcoeff].array.F[k1];
            set_DM_modesRM(loop);


            usleep(delayus);

            for (kloop = 0; kloop < NBloops; kloop++)
            {
                kc = 0;
                if(Verbose)
                {
                    printf("\n Loop %ld / %ld (%f)\n", kloop, NBloops, amp);
                    fflush(stdout);
                }


                for(k1 = 0; k1 < AOconf[loop].NBDMmodes; k1++)
                {
                    for(k2 = 0; k2 < AOconf[loop].NBDMmodes; k2++)
                        data.image[aoconfID_cmd_modesRM].array.F[k2] = 0.0;

                    // positive
                    data.image[aoconfID_cmd_modesRM].array.F[k1] = amp*data.image[IDmcoeff].array.F[k1];
                    set_DM_modesRM(loop);




                    for(kk=0; kk<NbAve; kk++)
                    {
                        Read_cam_frame(loop, 1, 1, 0, 0);


                        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                        {
                            data.image[IDrefi].array.F[ii] += data.image[aoconfID_imWFS1].array.F[ii];
                            data.image[IDrmc].array.F[kc*AOconf[loop].sizeWFS+ii] += data.image[aoconfID_imWFS1].array.F[ii];
                        }
                        kc++;
                    }


                    // negative
                    data.image[aoconfID_cmd_modesRM].array.F[k1] = 0.0-amp*data.image[IDmcoeff].array.F[k1];
                    set_DM_modesRM(loop);



                    for(kk=0; kk<NbAve; kk++)
                    {
                        Read_cam_frame(loop, 1, 1, 0, 0);

                        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                        {
                            data.image[IDrefi].array.F[ii] += data.image[aoconfID_imWFS1].array.F[ii];
                            data.image[IDrmc].array.F[kc*AOconf[loop].sizeWFS+ii] += data.image[aoconfID_imWFS1].array.F[ii];
                        }
                        kc++;
                    }
                }
            }

            for(ii=0; ii<AOconf[loop].sizeWFS*RespMatNBframes; ii++)
                data.image[IDrmc].array.F[ii] /= NBloops;


            // set DM to zero
            for(k2 = 0; k2 < AOconf[loop].NBDMmodes; k2++)
                data.image[aoconfID_cmd_modesRM].array.F[k2] = 0.0;
            set_DM_modesRM(loop);

            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                data.image[IDrefi].array.F[ii] /= RespMatNBframes*NBloops;



            // SAVE RMCUBE
            //    save_fits("RMcube", "!RMcube.fits");

            // remove average
            if(1)
            {
                IDrmc1 = create_3Dimage_ID("RMcube1", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, RespMatNBframes); // this is the main cube, average removed

                for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                {
                    valave = 0.0;
                    for(kc=0; kc<RespMatNBframes; kc++)
                        valave += data.image[IDrmc].array.F[kc*AOconf[loop].sizeWFS+ii];
                    valave /= RespMatNBframes;
                    for(kc=0; kc<RespMatNBframes; kc++)
                        data.image[IDrmc1].array.F[kc*AOconf[loop].sizeWFS+ii] = data.image[IDrmc].array.F[kc*AOconf[loop].sizeWFS+ii] - valave;
                }
                save_fits("RMcube1", "!RMcube1.fits");
            }




            IDrmtest = create_3Dimage_ID("rmtest", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].NBDMmodes);


            kc0 = fDelay;

            // initialize RM to zero
            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                for(k=0; k<AOconf[loop].NBDMmodes; k++)
                    data.image[IDrmtest].array.F[k*AOconf[loop].sizeWFS+ii] = 0.0;

            // initialize reference to zero
            kc = kc0;

            for(k1 = 0; k1 < AOconf[loop].NBDMmodes; k1++)
            {
                // positive
                kc += NBexcl;
                if(kc > data.image[IDrmc].md[0].size[2]-1)
                    kc -= data.image[IDrmc].md[0].size[2];
                for(kk=NBexcl; kk<NbAve-NBexcl; kk++)
                {
                    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                    {
                        data.image[IDrmtest].array.F[k1*AOconf[loop].sizeWFS+ii] += data.image[IDrmc].array.F[kc*AOconf[loop].sizeWFS+ii];
                        //     data.image[IDrmc].array.F[kc*AOconf[loop].sizeWFS+ii] += 1.0;
                    }
                    kc++;
                    if(kc > data.image[IDrmc].md[0].size[2]-1)
                        kc -= data.image[IDrmc].md[0].size[2];
                }
                kc+=NBexcl;
                if(kc > data.image[IDrmc].md[0].size[2]-1)
                    kc -= data.image[IDrmc].md[0].size[2];

                // negative
                kc+=NBexcl;
                if(kc > data.image[IDrmc].md[0].size[2]-1)
                    kc -= data.image[IDrmc].md[0].size[2];
                for(kk=NBexcl; kk<NbAve-NBexcl; kk++)
                {
                    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                    {
                        data.image[IDrmtest].array.F[k1*AOconf[loop].sizeWFS+ii] -= data.image[IDrmc].array.F[kc*AOconf[loop].sizeWFS+ii];
                        //  data.image[IDrmc].array.F[kc*AOconf[loop].sizeWFS+ii] -= 1.0;
                    }
                    kc++;
                    if(kc > data.image[IDrmc].md[0].size[2]-1)
                        kc -= data.image[IDrmc].md[0].size[2];
                }
                kc+=NBexcl;
                if(kc > data.image[IDrmc].md[0].size[2]-1)
                    kc -= data.image[IDrmc].md[0].size[2];
            }

            //  save_fits("RMcube", "!RMcube2.fits");
            //  exit(0);
            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                for(k1=0; k1<AOconf[loop].NBDMmodes; k1++)
                    data.image[IDrmi].array.F[k1*AOconf[loop].sizeWFS+ii] = data.image[IDrmtest].array.F[k1*AOconf[loop].sizeWFS+ii];


            //        save_fl_fits("rmtest", "!rmtest.fits");
            delete_image_ID("rmtest");




            printf("%ld %ld  %ld  %ld\n", IDrefcumul, IDrmcumul, IDwfsref0, IDrespM);


            beta = (1.0-gain)*beta + gain;
            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            {
                data.image[IDrefcumul].array.F[ii] = (1.0-gain)*data.image[IDrefcumul].array.F[ii] + gain*data.image[IDrefi].array.F[ii];

                data.image[IDwfsref0].array.F[ii] = data.image[IDrefcumul].array.F[ii]/beta;



                for(k1=0; k1<AOconf[loop].NBDMmodes; k1++)
                {
                    data.image[IDrmcumul].array.F[k1*AOconf[loop].sizeWFS+ii] = (1.0-gain)*data.image[IDrmcumul].array.F[k1*AOconf[loop].sizeWFS+ii] + gain*data.image[IDrmi].array.F[k1*AOconf[loop].sizeWFS+ii];
                    data.image[IDrespM].array.F[k1*AOconf[loop].sizeWFS+ii] = data.image[IDrmcumul].array.F[k1*AOconf[loop].sizeWFS+ii]/beta;
                }
            }

            for(k1=0; k1<AOconf[loop].NBDMmodes; k1++)
            {
                rmsval = 0.0;
                for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                    rmsval += data.image[IDrespM].array.F[k1*AOconf[loop].sizeWFS+ii]*data.image[IDrespM].array.F[k1*AOconf[loop].sizeWFS+ii];

                data.image[IDoptsignal].array.F[k1] += rmsval;
                data.image[IDoptcnt].array.F[k1] += 1.0;

                data.image[IDoptsignaln].array.F[k1] = data.image[IDoptsignal].array.F[k1]/data.image[IDoptcnt].array.F[k1];
            }
            save_fits("optsignaln","!./tmp/RM_optsign.fits");

            if(sprintf(signame, "./tmp/RM_optsign_%06ld.txt", iter) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            normcoeff = 0.0;
            normcoeffcnt = 0.0;
            for(k1=AOconf[loop].NBDMmodes/2; k1<AOconf[loop].NBDMmodes; k1++)
            {
                normcoeff += data.image[IDoptsignaln].array.F[k1];
                normcoeffcnt += 1.0;
            }
            normcoeff /= normcoeffcnt;



            if(AdjustAmplitude==1)
                for(k1=0; k1<AOconf[loop].NBDMmodes; k1++)
                {
                    data.image[IDmcoeff].array.F[k1] = 0.8*data.image[IDmcoeff].array.F[k1] + 0.2/(data.image[IDoptsignaln].array.F[k1]/normcoeff);
                    if(data.image[IDmcoeff].array.F[k1]>5.0)
                        data.image[IDmcoeff].array.F[k1] = 5.0;
                }

            fp = fopen(signame, "w");
            for(k1=0; k1<AOconf[loop].NBDMmodes; k1++)
                fprintf(fp, "%ld  %g  %g  %g\n", k1, data.image[IDoptsignaln].array.F[k1], data.image[IDoptcnt].array.F[k1], data.image[IDmcoeff].array.F[k1]*amp);
            fclose(fp);
            if(system("cp ./tmp/RM_outsign%06ld.txt ./tmp/RM_outsign.txt") != 0)
                printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

            save_fits("refwfsacq", "!./tmp/refwfs.fits");
            save_fits("respmacq", "!./tmp/respM.fits");
        }
    }


    fp = fopen("./tmp/rmparams.txt", "w");
    fprintf(fp, "%5ld       NbAve: number of WFS frames per averaging\n", NbAve);
    fprintf(fp, "%f	        amp: nominal DM amplitude (RMS)\n", amp);
    fprintf(fp, "%ld        iter: number of iterations\n", iter);
    fprintf(fp, "%ld        nbloop: number of loops per iteration\n", nbloop);
    fprintf(fp, "%ld        fDelay: delay number of frames\n", fDelay);
    fclose(fp);



    printf("Done\n");
    free(sizearray);


    return(0);
}



//
// Measure fast Modal response matrix
//
// HardwareLag [s]
//
// ampl [um]
//

long AOloopControl_RespMatrix_Fast(const char *DMmodes_name, const char *dmRM_name, const char *imWFS_name, long semtrig, float HardwareLag, float loopfrequ, float ampl, const char *outname)
{
    long IDout;
    long IDmodes;
    long IDmodes1; // muplitiples by ampl, + and -
    long IDdmRM;
    long IDwfs;
    long IDbuff;
    long ii, kk;

    long HardwareLag_int;
    float HardwareLag_frac;
    float WFSperiod;

    long NBmodes;
    long dmxsize, dmysize, dmxysize, wfsxsize, wfsysize, wfsxysize;
    long twait;

    int RT_priority = 80; //any number from 0-99
    struct sched_param schedpar;

    char *ptr0;
    long dmframesize;
    long wfsframesize;
    char *ptrs0;
    char *ptrs1;




    WFSperiod = 1.0/loopfrequ;
    HardwareLag_int = (long) (HardwareLag/WFSperiod);
    HardwareLag_frac = HardwareLag - WFSperiod*HardwareLag_int; // [s]

    twait = (long) (1.0e6 * ( (0.5*WFSperiod) - HardwareLag_frac ) );



    IDmodes = image_ID(DMmodes_name);
    dmxsize = data.image[IDmodes].md[0].size[0];
    dmysize = data.image[IDmodes].md[0].size[1];
    NBmodes = data.image[IDmodes].md[0].size[2];
    dmxysize = dmxsize*dmysize;




    IDmodes1 = image_ID("_tmpmodes");
    if(IDmodes1 == -1)
        IDmodes1 = create_3Dimage_ID("_tmpmodes", dmxsize, dmysize, 2*NBmodes);

    for(kk=0; kk<NBmodes; kk++)
    {
        for(ii=0; ii<dmxysize; ii++)
        {
            data.image[IDmodes1].array.F[2*kk*dmxysize+ii] =  ampl * data.image[IDmodes].array.F[kk*dmxysize+ii];
            data.image[IDmodes1].array.F[(2*kk+1)*dmxysize+ii] =  -ampl * data.image[IDmodes].array.F[kk*dmxysize+ii];
        }
    }



    IDdmRM = image_ID(dmRM_name);

    IDwfs = image_ID(imWFS_name);
    wfsxsize = data.image[IDwfs].md[0].size[0];
    wfsysize = data.image[IDwfs].md[0].size[1];
    wfsxysize = wfsxsize*wfsysize;

    IDbuff = image_ID("RMbuff");
    if(IDbuff == -1)
        IDbuff = create_3Dimage_ID("RMbuff", wfsxsize, wfsysize, 2*NBmodes + HardwareLag_int + 1);

    dmframesize = sizeof(float)*dmxysize;
    wfsframesize = sizeof(float)*wfsxysize;

    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    // r = seteuid(euid_called); //This goes up to maximum privileges
    sched_setscheduler(0, SCHED_FIFO, &schedpar); //other option is SCHED_RR, might be faster
    // r = seteuid(euid_real);//Go back to normal privileges
#endif

    ptr0 = (char*) data.image[IDmodes1].array.F;
    ptrs0 = (char*) data.image[IDbuff].array.F;

    // flush semaphore
    while(sem_trywait(data.image[IDwfs].semptr[semtrig])==0) {}


    for(kk=0; kk<NBmodes; kk++)
    {
        char *ptr1;

        sem_wait(data.image[IDwfs].semptr[semtrig]);
        ptrs1 = ptrs0 + wfsxysize*(2*kk);
        memcpy(ptrs1, data.image[IDwfs].array.F, wfsframesize);
        usleep(twait);

        // apply positive mode poke
        ptr1 = ptr0 + (2*kk)*dmframesize;
        data.image[IDdmRM].md[0].write = 1;
        memcpy(data.image[IDdmRM].array.F, ptr1, dmframesize);
        COREMOD_MEMORY_image_set_sempost_byID(IDdmRM, -1);
        data.image[IDdmRM].md[0].cnt0++;
        data.image[IDdmRM].md[0].write = 0;



        sem_wait(data.image[IDwfs].semptr[semtrig]);
        ptrs1 = ptrs0 + wfsxysize*(2*kk+1);
        memcpy(ptrs1, data.image[IDwfs].array.F, wfsframesize);
        usleep(twait);

        // apply negative mode poke
        ptr1 = ptr0 + (2*kk+1)*dmframesize;
        data.image[IDdmRM].md[0].write = 1;
        memcpy(data.image[IDdmRM].array.F, ptr1, dmframesize);
        COREMOD_MEMORY_image_set_sempost_byID(IDdmRM, -1);
        data.image[IDdmRM].md[0].cnt0++;
        data.image[IDdmRM].md[0].write = 0;
    }

    for(kk=0; kk<HardwareLag_int + 1; kk++)
    {
        sem_wait(data.image[IDwfs].semptr[semtrig]);
        ptrs1 = ptrs0 + wfsxysize*(2*NBmodes+kk);
        memcpy(ptrs1, data.image[IDwfs].array.F, wfsframesize);
        usleep(twait);

        // apply zero poke
        data.image[IDdmRM].md[0].write = 1;
        memset(data.image[IDdmRM].array.F, 0, dmframesize);
        COREMOD_MEMORY_image_set_sempost_byID(IDdmRM, -1);
        data.image[IDdmRM].md[0].cnt0++;
        data.image[IDdmRM].md[0].write = 0;
    }


    IDout = create_3Dimage_ID(outname, wfsxsize, wfsysize, NBmodes);
    for(kk=0; kk<NBmodes; kk++)
    {
        long buffindex;

        buffindex = 2*kk + HardwareLag_int;
        for(ii=0; ii<wfsxysize; ii++)
        {
            data.image[IDout].array.F[kk*wfsxysize + ii] = ( data.image[IDbuff].array.F[(buffindex)*wfsxysize + ii] - data.image[IDbuff].array.F[(buffindex+1)*wfsxysize + ii] ) / ampl;
        }

    }

    return(IDout);
}










/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 5. COMPUTING CALIBRATION                                                 */
/* =============================================================================================== */
/* =============================================================================================== */



// output:
// Hadamard modes (outname)
// Hadamard matrix ("Hmat.fits")
// pixel indexes ("Hpixindex.fits", float, to be converted to long)
long AOloopControl_mkHadamardModes(const char *DMmask_name, const char *outname)
{
    long IDout;
    long xsize, ysize, xysize;
    //    long IDdisk;
    long cnt;

    long Hsize;
    int n2max;

    long *indexarray;
    long index;
    long IDtest;
    int *Hmat;
    long k, ii, jj, n, n2, i, j;
    long IDindex;
    uint32_t *sizearray;

    long IDmask;



    IDmask = image_ID(DMmask_name);
    xsize = data.image[IDmask].md[0].size[0];
    ysize = data.image[IDmask].md[0].size[1];
    xysize = xsize*ysize;

    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*2);
    sizearray[0] = xsize;
    sizearray[1] = ysize;
    IDindex = create_image_ID("Hpixindex", 2, sizearray, _DATATYPE_FLOAT, 0, 0);
    free(sizearray);

    cnt = 0;
    for(ii=0; ii<xysize; ii++)
        if(data.image[IDmask].array.F[ii]>0.5)
            cnt++;

    Hsize = 1;
    n2max = 0;
    while(Hsize<cnt)
    {
        Hsize *= 2;
        n2max++;
    }
    n2max++;

    printf("Hsize n2max = %ld  %d\n", Hsize, n2max);
    fflush(stdout);

    for(ii=0; ii<xysize; ii++)
        data.image[IDindex].array.F[ii] = -10.0;

    index = 0;

    indexarray = (long*) malloc(sizeof(long)*Hsize);
    for(k=0; k<Hsize; k++)
        indexarray[k] = -1;
    for(ii=0; ii<xysize; ii++)
        if((data.image[IDmask].array.F[ii]>0.5)&&(index<Hsize))
        {

            indexarray[index] = ii;
            // printf("(%ld %ld)  ", index, ii);

            data.image[IDindex].array.F[ii] = 1.0*index;

            index++;
        }
    save_fits("Hpixindex", "!Hpixindex.fits.gz");

    Hmat = (int*) malloc(sizeof(int)*Hsize*Hsize);



    // n = 0

    ii = 0;
    jj = 0;
    Hmat[jj*Hsize+ii] = 1;
    n2=1;
    for(n=1; n<n2max; n++)
    {
        for(ii=0; ii<n2; ii++)
            for(jj=0; jj<n2; jj++)
            {
                Hmat[ jj*Hsize + (ii+n2)] = Hmat[ jj*Hsize + ii];
                Hmat[ (jj+n2)*Hsize + (ii+n2)] = -Hmat[ jj*Hsize + ii];
                Hmat[ (jj+n2)*Hsize + ii] = Hmat[ jj*Hsize + ii];
            }
        n2 *= 2;
    }

    printf("n2 = %ld\n", n2);
    fflush(stdout);

    IDtest = create_2Dimage_ID("Htest", Hsize, Hsize);

    for(ii=0; ii<Hsize; ii++)
        for(jj=0; jj<Hsize; jj++)
            data.image[IDtest].array.F[jj*Hsize+ii] = Hmat[jj*Hsize+ii];

    save_fits("Htest", "!Hmat.fits.gz");


    IDout = create_3Dimage_ID(outname, xsize, ysize, Hsize);
    for(k=0; k<Hsize; k++)
    {
        for(index=0; index<Hsize; index++)
        {
            ii = indexarray[index];
            data.image[IDout].array.F[k*xysize+ii] = Hmat[k*Hsize+index];
        }
    }

    free(Hmat);

    free(indexarray);


    return(IDout);
}




long AOloopControl_Hadamard_decodeRM(const char *inname, const char *Hmatname, const char *indexname, const char *outname)
{
    long IDin, IDhad, IDout, IDindex;
    long NBact, NBframes, sizexwfs, sizeywfs, sizewfs;
    long kk, kk1, ii;
    uint32_t zsizeout;



    IDin = image_ID(inname);
    sizexwfs = data.image[IDin].md[0].size[0];
    sizeywfs = data.image[IDin].md[0].size[1];
    sizewfs = sizexwfs*sizeywfs;
    NBframes = data.image[IDin].md[0].size[2];

    IDindex = image_ID(indexname);



    IDhad = image_ID(Hmatname);
    if((data.image[IDhad].md[0].size[0]!=NBframes)||(data.image[IDhad].md[0].size[1]!=NBframes))
    {
        printf("ERROR: size of Hadamard matrix [%ld x %ld] does not match available number of frames [%ld]\n", (long) data.image[IDhad].md[0].size[0], (long) data.image[IDhad].md[0].size[1], NBframes);
        exit(0);
    }

    zsizeout = data.image[IDindex].md[0].size[0]*data.image[IDindex].md[0].size[1];
    IDout = create_3Dimage_ID(outname, sizexwfs, sizeywfs, zsizeout);

    long kk0;
# ifdef _OPENMP
    #pragma omp parallel for private(kk0,kk1,ii)
# endif
    for(kk=0; kk<zsizeout; kk++) // output frame
    {
        kk0 = (long) (data.image[IDindex].array.F[kk]+0.1);
        if(kk0 > -1)
        {   printf("\r  frame %5ld / %5ld     ", kk0, NBframes);
            fflush(stdout);
            for(kk1=0; kk1<NBframes; kk1++)
            {
                for(ii=0; ii<sizewfs; ii++)
                    data.image[IDout].array.F[kk*sizewfs+ii] += data.image[IDin].array.F[kk1*sizewfs+ii]*data.image[IDhad].array.F[kk0*NBframes+kk1];
            }
        }
    }

    for(kk=0; kk<zsizeout; kk++)
    {
        for(ii=0; ii<sizewfs; ii++)
            data.image[IDout].array.F[kk*sizewfs+ii] /= NBframes;

    }

    printf("\n\n");


    return(IDout);
}




// make low order DM modes
long AOloopControl_mkloDMmodes(const char *ID_name, long msizex, long msizey, float CPAmax, float deltaCPA, double xc, double yc, double r0, double r1, int MaskMode)
{
    long IDmask;
    long ID, ID0, IDtm, IDem, IDtmpg, IDslaved;
    long ii, jj;
    double x, y, r, PA, xc1, yc1, totm, offset, rms, sigma;

    long NBZ, m;
    long zindex[10];
    double zcpa[10];  /// CPA for each Zernike (somewhat arbitrary... used to sort modes in CPA)
    long IDfreq, IDmfcpa;
    long k;

    double coeff;
    long kelim = 20;
    long conviter;




    zindex[0] = 1; // tip
    zcpa[0] = 0.0;

    zindex[1] = 2; // tilt
    zcpa[1] = 0.0;

    zindex[2] = 4; // focus
    zcpa[2] = 0.25;

    zindex[3] = 3; // astig
    zcpa[3] = 0.4;

    zindex[4] = 5; // astig
    zcpa[4] = 0.4;

    zindex[5] = 7; // coma
    zcpa[5] = 0.6;

    zindex[6] = 8; // coma
    zcpa[6] = 0.6;

    zindex[7] = 6; // trefoil
    zcpa[7] = 1.0;

    zindex[8] = 9; // trefoil
    zcpa[8] = 1.0;

    zindex[9] = 12;
    zcpa[9] = 1.5;


    printf("msizexy = %ld %ld\n", msizex, msizey);
    list_image_ID();
    IDmask = image_ID("dmmask");
    if(IDmask==-1)
    {
        double val0, val1;
        double a0=0.88;
        double b0=40.0;
        double a1=1.2;
        double b1=12.0;

        IDmask = create_2Dimage_ID("dmmask", msizex, msizey);
        for(ii=0; ii<msizex; ii++)
            for(jj=0; jj<msizey; jj++)
            {
                x = 1.0*ii-xc;
                y = 1.0*jj-yc;
                r = sqrt(x*x+y*y)/r1;
                val1 = 1.0-exp(-pow(a1*r,b1));
                r = sqrt(x*x+y*y)/r0;
                val0 = exp(-pow(a0*r,b0));
                data.image[IDmask].array.F[jj*msizex+ii] = val0*val1;
            }
        save_fits("dmmask", "!dmmask.fits");
        xc1 = xc;
        yc1 = yc;
    }
    else /// extract xc and yc from mask
    {
        xc1 = 0.0;
        yc1 = 0.0;
        totm = 0.0;
        for(ii=0; ii<msizex; ii++)
            for(jj=0; jj<msizey; jj++)
            {
                xc1 += 1.0*ii*data.image[IDmask].array.F[jj*msizex+ii];
                yc1 += 1.0*jj*data.image[IDmask].array.F[jj*msizex+ii];
                totm += data.image[IDmask].array.F[jj*msizex+ii];
            }
        // printf("xc1 yc1    %f  %f     %f\n", xc1, yc1, totm);
        xc1 /= totm;
        yc1 /= totm;
    }




    totm = arith_image_total("dmmask");
    if((msizex != data.image[IDmask].md[0].size[0])||(msizey != data.image[IDmask].md[0].size[1]))
    {
        printf("ERROR: file dmmask size (%ld %ld) does not match expected size (%ld %ld)\n", (long) data.image[IDmask].md[0].size[0], (long) data.image[IDmask].md[0].size[1], (long) msizex, (long) msizey);
        exit(0);
    }


    NBZ = 0;

    for(m=0; m<10; m++)
    {
        if(zcpa[m]<CPAmax)
            NBZ++;
    }



    linopt_imtools_makeCPAmodes("CPAmodes", msizex, CPAmax, deltaCPA, 0.5*msizex, 1.2, 0);
    ID0 = image_ID("CPAmodes");
    IDfreq = image_ID("cpamodesfreq");



    printf("  %ld %ld %ld\n", msizex, msizey, (long) data.image[ID0].md[0].size[2]-1 );
    ID = create_3Dimage_ID(ID_name, msizex, msizey, data.image[ID0].md[0].size[2]-1+NBZ);





    IDmfcpa = create_2Dimage_ID("modesfreqcpa", data.image[ID0].md[0].size[2]-1+NBZ, 1);

    /*** Create TTF first */
    zernike_init();
    printf("r1 = %f    %f %f\n", r1, xc1, yc1);
    for(k=0; k<NBZ; k++)
    {
        data.image[IDmfcpa].array.F[k] = zcpa[k];
        for(ii=0; ii<msizex; ii++)
            for(jj=0; jj<msizey; jj++)
            {
                x = 1.0*ii-xc1;
                y = 1.0*jj-yc1;
                r = sqrt(x*x+y*y)/r1;
                PA = atan2(y,x);
                data.image[ID].array.F[k*msizex*msizey+jj*msizex+ii] = Zernike_value(zindex[k], r, PA);
            }
    }




    for(k=0; k<data.image[ID0].md[0].size[2]-1; k++)
    {
        data.image[IDmfcpa].array.F[k+NBZ] = data.image[IDfreq].array.F[k+1];
        for(ii=0; ii<msizex*msizey; ii++)
            data.image[ID].array.F[(k+NBZ)*msizex*msizey+ii] = data.image[ID0].array.F[(k+1)*msizex*msizey+ii];
    }


    for(k=0; k<data.image[ID0].md[0].size[2]-1+NBZ; k++)
    {
        /// Remove excluded modes
        long IDeModes = image_ID("emodes");
        if(IDeModes!=-1)
        {
            IDtm = create_2Dimage_ID("tmpmode", msizex, msizey);

            for(ii=0; ii<msizex*msizey; ii++)
                data.image[IDtm].array.F[ii] = data.image[ID].array.F[k*msizex*msizey+ii];
            linopt_imtools_image_fitModes("tmpmode", "emodes", "dmmask", 1.0e-2, "lcoeff", 0);
            linopt_imtools_image_construct("emodes", "lcoeff", "em00");
            delete_image_ID("lcoeff");
            IDem = image_ID("em00");

            coeff = 1.0-exp(-pow(1.0*k/kelim,6.0));
            if(k>2.0*kelim)
                coeff = 1.0;
            for(ii=0; ii<msizex*msizey; ii++)
                data.image[ID].array.F[k*msizex*msizey+ii] = data.image[IDtm].array.F[ii] - coeff*data.image[IDem].array.F[ii];

            delete_image_ID("em00");
            delete_image_ID("tmpmode");
        }


        double totvm = 0.0;
        for(ii=0; ii<msizex*msizey; ii++)
        {
            //	  data.image[ID].array.F[k*msize*msize+ii] = data.image[ID0].array.F[(k+1)*msize*msize+ii];
            totvm += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[IDmask].array.F[ii];
        }
        offset = totvm/totm;

        for(ii=0; ii<msizex*msizey; ii++)
        {
            data.image[ID].array.F[k*msizex*msizey+ii] -= offset;
            data.image[ID].array.F[k*msizex*msizey+ii] *= data.image[IDmask].array.F[ii];
        }

        offset = 0.0;
        for(ii=0; ii<msizex*msizey; ii++)
            offset += data.image[ID].array.F[k*msizex*msizey+ii];

        rms = 0.0;
        for(ii=0; ii<msizex*msizey; ii++)
        {
            data.image[ID].array.F[k*msizex*msizey+ii] -= offset/msizex/msizey;
            rms += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[ID].array.F[k*msizex*msizey+ii];
        }
        rms = sqrt(rms/totm);
        printf("Mode %ld   RMS = %lf  (%f)\n", k, rms, totm);
        for(ii=0; ii<msizex*msizey; ii++)
            data.image[ID].array.F[k*msizex*msizey+ii] /= rms;
    }


    for(k=0; k<data.image[ID0].md[0].size[2]-1+NBZ; k++)
    {
        rms = 0.0;
        for(ii=0; ii<msizex*msizey; ii++)
        {
            data.image[ID].array.F[k*msizex*msizey+ii] -= offset/msizex/msizey;
            rms += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[ID].array.F[k*msizex*msizey+ii];
        }
        rms = sqrt(rms/totm);
        printf("Mode %ld   RMS = %lf\n", k, rms);
    }



    if(MaskMode==1)
    {
        long kernsize = 5;
        long NBciter = 200;
        long citer;

        if(2*kernsize>msizex)
            kernsize = msizex/2;
        for(citer=0; citer<NBciter; citer++)
        {
            long IDg;

            printf("Convolution [%3ld/%3ld]\n", citer, NBciter);
            gauss_filter(ID_name, "modeg", 4.0*pow(1.0*(NBciter-citer)/NBciter,0.5), kernsize);
            IDg = image_ID("modeg");
            for(k=0; k<data.image[ID].md[0].size[2]; k++)
            {
                for(ii=0; ii<msizex*msizey; ii++)
                    if(data.image[IDmask].array.F[ii]<0.98)
                        data.image[ID].array.F[k*msizex*msizey+ii] = data.image[IDg].array.F[k*msizex*msizey+ii];
            }
            delete_image_ID("modeg");
        }
    }




    /// SLAVED ACTUATORS
    IDslaved = image_ID("dmslaved");
    ID = image_ID(ID_name);
    if((IDslaved != -1)&&(IDmask!=-1))
    {
        long IDtmp = create_2Dimage_ID("_tmpinterpol", msizex, msizey);
        long IDtmp1 = create_2Dimage_ID("_tmpcoeff1", msizex, msizey);
        long IDtmp2 = create_2Dimage_ID("_tmpcoeff2", msizex, msizey);
        for(m=0; m<data.image[ID].md[0].size[2]; m++)
        {
            // write input DM mode
            for(ii=0; ii<msizex*msizey; ii++)
            {
                data.image[IDtmp].array.F[ii] = data.image[ID].array.F[m*msizex*msizey+ii];
                data.image[IDtmp1].array.F[ii] = data.image[IDmask].array.F[ii] * (1.0-data.image[IDslaved].array.F[ii]);
                data.image[IDtmp2].array.F[ii] = data.image[IDtmp1].array.F[ii];
            }

            long pixcnt = 1;
            float vxp, vxm, vyp, vym, cxp, cxm, cyp, cym;
            float ctot;

            while(pixcnt>0)
            {
                pixcnt = 0;
                for(ii=1; ii<msizex-1; ii++)
                    for(jj=1; jj<msizey-1; jj++)
                    {
                        if((data.image[IDtmp1].array.F[jj*msizex+ii]<0.5) && (data.image[IDslaved].array.F[jj*msizex+ii]>0.5))
                        {
                            pixcnt ++;
                            vxp = data.image[IDtmp].array.F[jj*msizex+(ii+1)];
                            cxp = data.image[IDtmp1].array.F[jj*msizex+(ii+1)];

                            vxm = data.image[IDtmp].array.F[jj*msizex+(ii-1)];
                            cxm = data.image[IDtmp1].array.F[jj*msizex+(ii-1)];

                            vyp = data.image[IDtmp].array.F[(jj+1)*msizex+ii];
                            cyp = data.image[IDtmp1].array.F[(jj+1)*msizex+ii];

                            vym = data.image[IDtmp].array.F[(jj-1)*msizex+ii];
                            cym = data.image[IDtmp1].array.F[(jj-1)*msizex+ii];

                            ctot = (cxp+cxm+cyp+cym);

                            if(ctot>0.5)
                            {
                                data.image[IDtmp].array.F[jj*msizex+ii] = (vxp*cxp+vxm*cxm+vyp*cyp+vym*cym)/ctot;
                                data.image[IDtmp2].array.F[jj*msizex+ii] = 1.0;
                            }
                        }
                    }
                for(ii=0; ii<msizex*msizey; ii++)
                    data.image[IDtmp1].array.F[ii] = data.image[IDtmp2].array.F[ii];
            }
            for(ii=0; ii<msizex*msizey; ii++)
                data.image[ID].array.F[m*msizex*msizey+ii] = data.image[IDtmp].array.F[ii];






            /*

                    IDtmp = create_2Dimage_ID("_tmpinterpol", msizex, msizey);
                    for(m=0; m<data.image[ID].md[0].size[2]; m++)
                    {
                        for(ii=0; ii<msizex*msizey; ii++)
                            data.image[IDtmp].array.F[ii] = data.image[ID].array.F[m*msizex*msizey+ii];

                        for(conviter=0; conviter<NBconviter; conviter++)
                        {
                            sigma = 0.5*NBconviter/(1.0+conviter);
                            gauss_filter("_tmpinterpol", "_tmpinterpolg", 1.0, 2);
                            IDtmpg = image_ID("_tmpinterpolg");
                            for(ii=0; ii<msizex*msizey; ii++)
                            {
                                if((data.image[IDmask].array.F[ii]>0.5)&&(data.image[IDslaved].array.F[ii]<0.5))
                                    data.image[IDtmp].array.F[ii] = data.image[ID].array.F[m*msizex*msizey+ii];
                                else
                                    data.image[IDtmp].array.F[ii] = data.image[IDtmpg].array.F[ii];
                            }
                            delete_image_ID("_tmpinterpolg");
                        }
                        for(ii=0; ii<msizex*msizey; ii++)
                            if(data.image[IDmask].array.F[ii]>0.5)
                                data.image[ID].array.F[m*msizex*msizey+ii] = data.image[IDtmp].array.F[ii];
                    */
        }
        delete_image_ID("_tmpinterpol");
        delete_image_ID("_tmpcoeff1");
        delete_image_ID("_tmpcoeff2");
    }



    return(ID);
}





// dmmask_perclow = 0.2
// dmmask_coefflow = 1.0
// dmmask_perchigh = 0.7
// dmmask_coeffhigh = 0.3

int_fast8_t AOloopControl_mkCalib_map_mask(long loop, const char *zrespm_name, const char *WFSmap_name, const char *DMmap_name, float dmmask_perclow, float dmmask_coefflow, float dmmask_perchigh, float dmmask_coeffhigh, float wfsmask_perclow, float wfsmask_coefflow, float wfsmask_perchigh, float wfsmask_coeffhigh)
{
    long IDWFSmap, IDDMmap;
    long IDWFSmask, IDDMmask;
    long IDzrm;
    long ii;
    float lim, rms;
    double tmpv;
    long sizexWFS, sizeyWFS, sizeWFS;
    long sizexDM, sizeyDM;
    long IDdm;
    char name[200];
    long NBpoke, poke;
    long IDDMmap1;
    float lim0;
    long IDtmp;


    IDzrm = image_ID(zrespm_name);
    sizexWFS = data.image[IDzrm].md[0].size[0];
    sizeyWFS = data.image[IDzrm].md[0].size[1];
    NBpoke = data.image[IDzrm].md[0].size[2];

    if(sprintf(name, "aol%ld_dmC", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDdm = read_sharedmem_image(name);
    sizexDM = data.image[IDdm].md[0].size[0];
    sizeyDM = data.image[IDdm].md[0].size[1];

    sizeWFS = sizexWFS*sizeyWFS;

    IDWFSmap = create_2Dimage_ID(WFSmap_name, sizexWFS, sizeyWFS);
    IDDMmap = create_2Dimage_ID(DMmap_name, sizexDM, sizeyDM);
    IDWFSmask = create_2Dimage_ID("wfsmask", sizexWFS, sizeyWFS);
    IDDMmask = create_2Dimage_ID("dmmask", sizexDM, sizeyDM);




    printf("Preparing DM map ... ");
    fflush(stdout);
    for(poke=0; poke<NBpoke; poke++)
    {
        rms = 0.0;
        for(ii=0; ii<sizeWFS; ii++)
        {
            tmpv = data.image[IDzrm].array.F[poke*sizeWFS+ii];
            rms += tmpv*tmpv;
        }
        data.image[IDDMmap].array.F[poke] = rms;
    }
    printf("done\n");
    fflush(stdout);



    printf("Preparing WFS map ... ");
    fflush(stdout);
    for(ii=0; ii<sizeWFS; ii++)
    {
        rms = 0.0;
        for(poke=0; poke<NBpoke; poke++)
        {
            tmpv = data.image[IDzrm].array.F[poke*sizeWFS+ii];
            rms += tmpv*tmpv;
        }
        data.image[IDWFSmap].array.F[ii] = rms;
    }
    printf("done\n");
    fflush(stdout);




    printf("Preparing DM mask ... ");
    fflush(stdout);

    // pre-filtering
    // gauss_filter(DMmap_name, "dmmapg", 5.0, 8);
    // IDDMmap1 = image_ID("dmmapg");

    // (map/map1)*pow(map,0.25)

    // DMmask: select pixels
    lim0 = dmmask_coefflow*img_percentile(DMmap_name, dmmask_perclow);
    IDtmp = create_2Dimage_ID("_tmpdmmap", sizexDM, sizeyDM);
    for(ii=0; ii<sizexDM*sizeyDM; ii++)
        data.image[IDtmp].array.F[ii] = data.image[IDDMmap].array.F[ii] - lim0;
    lim = dmmask_coeffhigh*img_percentile("_tmpdmmap", dmmask_perchigh);

    for(poke=0; poke<NBpoke; poke++)
    {
        if(data.image[IDtmp].array.F[poke]<lim)
            data.image[IDDMmask].array.F[poke] = 0.0;
        else
            data.image[IDDMmask].array.F[poke] = 1.0;
    }
    delete_image_ID("_tmpdmmap");
    printf("done\n");
    fflush(stdout);



    // WFSmask : select pixels
    printf("Preparing WFS mask ... ");
    fflush(stdout);

    lim0 = wfsmask_coefflow*img_percentile(WFSmap_name, wfsmask_perclow);
    IDtmp = create_2Dimage_ID("_tmpwfsmap", sizexWFS, sizeyWFS);
    for(ii=0; ii<sizexWFS*sizeyWFS; ii++)
        data.image[IDtmp].array.F[ii] = data.image[IDWFSmap].array.F[ii] - lim0;
    lim = wfsmask_coeffhigh*img_percentile("_tmpwfsmap", wfsmask_perchigh);

    for(ii=0; ii<sizeWFS; ii++)
    {
        if(data.image[IDWFSmap].array.F[ii]<lim)
            data.image[IDWFSmask].array.F[ii] = 0.0;
        else
            data.image[IDWFSmask].array.F[ii] = 1.0;
    }
    delete_image_ID("_tmpwfsmap");
    printf("done\n");
    fflush(stdout);

    return(0);
}





//
// if images "Hmat" AND "pixindexim" are provided, decode the image
// TEST: if "RMpokeC" exists, decode it as well
//
int_fast8_t AOloopControl_Process_zrespM(long loop, const char *IDzrespm0_name, const char *IDwfsref_name, const char *IDzrespm_name, const char *WFSmap_name, const char *DMmap_name)
{
    long NBpoke;
    long IDzrm;

    long sizexWFS, sizeyWFS, sizexDM, sizeyDM;
    long sizeWFS, sizeDM;
    long poke, ii;
    char name[200];

    double rms, tmpv;
    long IDDMmap, IDWFSmap, IDdm;



    // DECODE MAPS (IF REQUIRED)
    IDzrm = image_ID(IDzrespm0_name);
    if((image_ID("RMmat")!=-1) && (image_ID("pixindexim")!=-1))  // start decoding
    {
        save_fits(IDzrespm0_name, "!zrespm_Hadamard.fits");

        AOloopControl_Hadamard_decodeRM(IDzrespm0_name, "RMmat", "pixindexim", IDzrespm_name);
        IDzrm = image_ID(IDzrespm_name);

        if(image_ID("RMpokeC")!=-1)
        {
            AOloopControl_Hadamard_decodeRM("RMpokeC", "RMmat", "pixindexim", "RMpokeC1");
            //save_fits("RMpokeC1", "!tmp/test_RMpokeC1.fits");
        }
    }
    else // NO DECODING
    {
        copy_image_ID(IDzrespm0_name, IDzrespm_name, 0);
    }




    // create sensitivity maps

    sizexWFS = data.image[IDzrm].md[0].size[0];
    sizeyWFS = data.image[IDzrm].md[0].size[1];
    NBpoke = data.image[IDzrm].md[0].size[2];

    if(sprintf(name, "aol%ld_dmC", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDdm = read_sharedmem_image(name);
    sizexDM = data.image[IDdm].md[0].size[0];
    sizeyDM = data.image[IDdm].md[0].size[1];

    sizeWFS = sizexWFS*sizeyWFS;

    IDWFSmap = create_2Dimage_ID(WFSmap_name, sizexWFS, sizeyWFS);
    IDDMmap = create_2Dimage_ID(DMmap_name, sizexDM, sizeyDM);


    printf("Preparing DM map ... ");
    fflush(stdout);
    for(poke=0; poke<NBpoke; poke++)
    {
        rms = 0.0;
        for(ii=0; ii<sizeWFS; ii++)
        {
            tmpv = data.image[IDzrm].array.F[poke*sizeWFS+ii];
            rms += tmpv*tmpv;
        }
        data.image[IDDMmap].array.F[poke] = rms;
    }
    printf("done\n");
    fflush(stdout);



    printf("Preparing WFS map ... ");
    fflush(stdout);
    for(ii=0; ii<sizeWFS; ii++)
    {
        rms = 0.0;
        for(poke=0; poke<NBpoke; poke++)
        {
            tmpv = data.image[IDzrm].array.F[poke*sizeWFS+ii];
            rms += tmpv*tmpv;
        }
        data.image[IDWFSmap].array.F[ii] = rms;
    }
    printf("done\n");
    fflush(stdout);



    /*
    IDWFSmask = image_ID("wfsmask");


    // normalize wfsref with wfsmask
    tot = 0.0;
    for(ii=0; ii<sizeWFS; ii++)
    	tot += data.image[IDWFSref].array.F[ii]*data.image[IDWFSmask].array.F[ii];

    totm = 0.0;
    for(ii=0; ii<sizeWFS; ii++)
    	totm += data.image[IDWFSmask].array.F[ii];

    for(ii=0; ii<sizeWFS; ii++)
    	data.image[IDWFSref].array.F[ii] /= tot;

    // make zrespm flux-neutral over wfsmask
    fp = fopen("zrespmat_flux.log", "w");
    for(poke=0;poke<NBpoke;poke++)
    {
    	tot = 0.0;
    	for(ii=0; ii<sizeWFS; ii++)
    		tot += data.image[IDzrm].array.F[poke*sizeWFS+ii]*data.image[IDWFSmask].array.F[ii];

    	for(ii=0; ii<sizeWFS; ii++)
    		data.image[IDzrm].array.F[poke*sizeWFS+ii] -= tot*data.image[IDWFSmask].array.F[ii]/totm;

    	tot1 = 0.0;
    	for(ii=0; ii<sizeWFS; ii++)
    		tot1 += data.image[IDzrm].array.F[poke*sizeWFS+ii]*data.image[IDWFSmask].array.F[ii];
    	fprintf(fp, "%6ld %06ld %20f %20f\n", poke, NBpoke, tot, tot1);
    }
    fclose(fp);

    */

}





// CANDIDATE FOR RETIREMENT
//
// median-averages multiple response matrices to create a better one
//
// if images "Hmat" AND "pixindexim" are provided, decode the image
// TEST: if "RMpokeC" exists, decode it as well
//
int_fast8_t AOloopControl_ProcessZrespM_medianfilt(long loop, const char *zrespm_name, const char *WFSref0_name, const char *WFSmap_name, const char *DMmap_name, double rmampl, int normalize)
{
    long NBmat; // number of matrices to average
    FILE *fp;
    int r;
    char name[200];
    char fname[200];
    char zrname[200];
    long kmat;
    long sizexWFS, sizeyWFS, sizeWFS;
    long *IDzresp_array;
    long ii;
    double fluxpos, fluxneg;
    float *pixvalarray;
    long k, kmin, kmax, kband;
    long IDzrm;
    float ave;
    long *IDWFSrefc_array;
    long IDWFSref;
    long IDWFSmap, IDDMmap;
    long IDWFSmask, IDDMmask;
    float lim, rms;
    double tmpv;
    long NBmatlim = 3;
    long ID1;
    long NBpoke, poke;
    double tot, totm;


    if(sprintf(fname, "./zresptmp/%s_nbiter.txt", zrespm_name) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if((fp = fopen(fname, "r"))==NULL)
    {
        printf("ERROR: cannot open file \"%s\"\n", fname);
        exit(0);
    }
    else
    {
        if(fscanf(fp, "%50ld", &NBmat) != 1)
            printERROR(__FILE__,__func__,__LINE__, "Cannot read parameter for file");

        fclose(fp);
    }


    if(NBmat<NBmatlim)
    {
        printf("ERROR: insufficient number of input matrixes:\n");
        printf(" NBmat = %ld, should be at least %ld\n", (long) NBmat, (long) NBmatlim);
        exit(0);
    }
    else
        printf("Processing %ld matrices\n", NBmat);

    if(sprintf(name, "aol%ld_dmC", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");


    IDzresp_array = (long*) malloc(sizeof(long)*NBmat);
    IDWFSrefc_array = (long*) malloc(sizeof(long)*NBmat);

    // STEP 1: build individually cleaned RM
    for(kmat=0; kmat<NBmat; kmat++)
    {
        if(sprintf(fname, "./zresptmp/%s_pos_%03ld.fits", zrespm_name, kmat) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        long IDzrespfp = load_fits(fname, "zrespfp", 2);

        if(sprintf(fname, "./zresptmp/%s_neg_%03ld.fits", zrespm_name, kmat) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        long IDzrespfm = load_fits(fname, "zrespfm", 2);

        sizexWFS = data.image[IDzrespfp].md[0].size[0];
        sizeyWFS = data.image[IDzrespfp].md[0].size[1];
        NBpoke = data.image[IDzrespfp].md[0].size[2];
        sizeWFS = sizexWFS*sizeyWFS;

        if(sprintf(name, "wfsrefc%03ld", kmat) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        IDWFSrefc_array[kmat] = create_3Dimage_ID(name, sizexWFS, sizeyWFS, NBpoke);

        if(sprintf(zrname, "zrespm%03ld", kmat) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        IDzresp_array[kmat] = create_3Dimage_ID(zrname, sizexWFS, sizeyWFS, NBpoke);



# ifdef _OPENMP
        #pragma omp parallel for private(fluxpos,fluxneg,ii)
# endif
        for(poke=0; poke<NBpoke; poke++)
        {
            fluxpos = 0.0;
            fluxneg = 0.0;
            for(ii=0; ii<sizeWFS; ii++)
            {
                if(isnan(data.image[IDzrespfp].array.F[poke*sizeWFS+ii])!=0)
                {
                    printf("%ld element %ld is NAN -> replacing by 0\n", IDzrespfp, poke*sizeWFS+ii);
                    data.image[IDzrespfp].array.F[poke*sizeWFS+ii] = 0.0;
                }
                fluxpos += data.image[IDzrespfp].array.F[poke*sizeWFS+ii];
            }

            for(ii=0; ii<sizeWFS; ii++)
            {
                if(isnan(data.image[IDzrespfm].array.F[poke*sizeWFS+ii])!=0)
                {
                    printf("%ld element %ld is NAN -> replacing by 0\n", IDzrespfm, poke*sizeWFS+ii);
                    data.image[IDzrespfm].array.F[poke*sizeWFS+ii] = 0.0;
                }
                fluxneg += data.image[IDzrespfm].array.F[poke*sizeWFS+ii];
            }

            for(ii=0; ii<sizeWFS; ii++)
            {
                if(normalize==1)
                {
                    data.image[IDzrespfp].array.F[poke*sizeWFS+ii] /= fluxpos;
                    data.image[IDzrespfm].array.F[poke*sizeWFS+ii] /= fluxneg;
                }
                data.image[IDzresp_array[kmat]].array.F[poke*sizeWFS+ii] = 0.5*(data.image[IDzrespfp].array.F[poke*sizeWFS+ii]-data.image[IDzrespfm].array.F[poke*sizeWFS+ii]);
                data.image[IDWFSrefc_array[kmat]].array.F[poke*sizeWFS+ii] = 0.5*(data.image[IDzrespfp].array.F[poke*sizeWFS+ii]+data.image[IDzrespfm].array.F[poke*sizeWFS+ii]);

                if(isnan(data.image[IDzresp_array[kmat]].array.F[poke*sizeWFS+ii])!=0)
                {
                    printf("%ld element %ld is NAN -> replacing by 0\n", IDzresp_array[kmat], poke*sizeWFS+ii);
                    data.image[IDzresp_array[kmat]].array.F[poke*sizeWFS+ii] = 0.0;
                }
                if(isnan(data.image[IDWFSrefc_array[kmat]].array.F[poke*sizeWFS+ii])!=0)
                {
                    printf("%ld element %ld is NAN -> replacing by 0\n", IDWFSrefc_array[kmat], poke*sizeWFS+ii);
                    data.image[IDWFSrefc_array[kmat]].array.F[poke*sizeWFS+ii] = 0.0;
                }
            }
        }

        delete_image_ID("zrespfp");
        delete_image_ID("zrespfm");
    }

    // STEP 2: average / median each pixel
    IDzrm = create_3Dimage_ID(zrespm_name, sizexWFS, sizeyWFS, NBpoke);
    IDWFSref = create_2Dimage_ID(WFSref0_name, sizexWFS, sizeyWFS);




    kband = (long) (0.2*NBmat);

    kmin = kband;
    kmax = NBmat-kband;

# ifdef _OPENMP
    #pragma omp parallel for private(ii,kmat,ave,k,pixvalarray)
# endif
    for(poke=0; poke<NBpoke; poke++)
    {
        printf("\r act %ld / %ld        ", poke, NBpoke);
        fflush(stdout);

        if((pixvalarray = (float*) malloc(sizeof(float)*NBmat))==NULL)
        {
            printf("ERROR: cannot allocate pixvalarray, size = %ld\n", (long) NBmat);
            exit(0);
        }

        for(ii=0; ii<sizeWFS; ii++)
        {
            for(kmat=0; kmat<NBmat; kmat++)
                pixvalarray[kmat] = data.image[IDzresp_array[kmat]].array.F[poke*sizeWFS+ii] ;
            quick_sort_float(pixvalarray, kmat);
            ave = 0.0;
            for(k=kmin; k<kmax; k++)
                ave += pixvalarray[k];
            ave /= (kmax-kmin);
            data.image[IDzrm].array.F[poke*sizeWFS+ii] = ave/rmampl;
        }
        free(pixvalarray);
    }

    printf("\n");



    kband = (long) (0.2*NBmat*NBpoke);
    kmin = kband;
    kmax = NBmat*NBpoke-kband;


# ifdef _OPENMP
    #pragma omp parallel for private(poke,kmat,pixvalarray,ave,k)
# endif
    for(ii=0; ii<sizeWFS; ii++)
    {
        printf("\r wfs pix %ld / %ld        ", ii, sizeWFS);
        fflush(stdout);
        if((pixvalarray = (float*) malloc(sizeof(float)*NBmat*NBpoke))==NULL)
        {
            printf("ERROR: cannot allocate pixvalarray, size = %ld x %ld\n", (long) NBmat, (long) NBpoke);
            exit(0);
        }

        for(poke=0; poke<NBpoke; poke++)
            for(kmat=0; kmat<NBmat; kmat++)
                pixvalarray[kmat*NBpoke+poke] = data.image[IDWFSrefc_array[kmat]].array.F[poke*sizeWFS+ii] ;


        quick_sort_float(pixvalarray, NBpoke*NBmat);

        ave = 0.0;
        for(k=kmin; k<kmax; k++)
            ave += pixvalarray[k];
        ave /= (kmax-kmin);
        data.image[IDWFSref].array.F[ii] = ave;

        //printf("free pixvalarray : %ld x %ld\n", NBmat, NBpoke);
        //fflush(stdout);
        free(pixvalarray);
        //printf("done\n");
        //fflush(stdout);
    }
    free(IDzresp_array);
    free(IDWFSrefc_array);





    // DECODE MAPS (IF REQUIRED)

    if((image_ID("Hmat")!=-1) && (image_ID("pixindexim")!=-1))
    {
        chname_image_ID(zrespm_name, "tmprm");
        save_fits("tmprm", "!zrespm_Hadamard.fits");

        AOloopControl_Hadamard_decodeRM("tmprm", "Hmat", "pixindexim", zrespm_name);
        delete_image_ID("tmprm");

        IDzrm = image_ID(zrespm_name);

        if(image_ID("RMpokeC") != -1)
        {
            AOloopControl_Hadamard_decodeRM("RMpokeC", "Hmat", "pixindexim", "RMpokeC1");
            save_fits("RMpokeC1", "!test_RMpokeC1.fits");
        }
    }

    NBpoke = data.image[IDzrm].md[0].size[2];


    AOloopControl_mkCalib_map_mask(loop, zrespm_name, WFSmap_name, DMmap_name, 0.2, 1.0, 0.7, 0.3, 0.05, 1.0, 0.65, 0.3);

    //	list_image_ID();
    //printf("========== STEP 000 ============\n");
    //	fflush(stdout);


    IDWFSmask = image_ID("wfsmask");
    //	printf("ID   %ld %ld\n", IDWFSmask, IDWFSref);

    // normalize wfsref with wfsmask
    tot = 0.0;
    for(ii=0; ii<sizeWFS; ii++)
        tot += data.image[IDWFSref].array.F[ii]*data.image[IDWFSmask].array.F[ii];

    totm = 0.0;
    for(ii=0; ii<sizeWFS; ii++)
        totm += data.image[IDWFSmask].array.F[ii];

    for(ii=0; ii<sizeWFS; ii++)
        data.image[IDWFSref].array.F[ii] /= tot;



    // make zrespm flux-neutral over wfsmask
    fp = fopen("zrespmat_flux.log", "w");
    for(poke=0; poke<NBpoke; poke++)
    {
        tot = 0.0;
        for(ii=0; ii<sizeWFS; ii++)
            tot += data.image[IDzrm].array.F[poke*sizeWFS+ii]*data.image[IDWFSmask].array.F[ii];

        for(ii=0; ii<sizeWFS; ii++)
            data.image[IDzrm].array.F[poke*sizeWFS+ii] -= tot*data.image[IDWFSmask].array.F[ii]/totm;

        double tot1 = 0.0;
        for(ii=0; ii<sizeWFS; ii++)
            tot1 += data.image[IDzrm].array.F[poke*sizeWFS+ii]*data.image[IDWFSmask].array.F[ii];
        fprintf(fp, "%6ld %06ld %20f %20f\n", poke, NBpoke, tot, tot1);
    }
    fclose(fp);


    return(0);
}





// make control matrix
//
long AOloopControl_mkCM(const char *respm_name, const char *cm_name, float SVDlim)
{


    // COMPUTE OVERALL CONTROL MATRIX

    printf("COMPUTE OVERALL CONTROL MATRIX\n");
#ifdef HAVE_MAGMA
    CUDACOMP_magma_compute_SVDpseudoInverse(respm_name, cm_name, SVDlim, 100000, "VTmat", 0);
#else
    linopt_compute_SVDpseudoInverse(respm_name, cm_name, SVDlim, 10000, "VTmat");
#endif

    //save_fits("VTmat", "!./mkmodestmp/VTmat.fits");
    delete_image_ID("VTmat");


    return(image_ID(cm_name));
}




//
// make slave actuators from maskRM
//
long AOloopControl_mkSlavedAct(const char *IDmaskRM_name, float pixrad, const char *IDout_name)
{
    long IDout;
    long IDmaskRM;
    long ii, jj;
    long ii1, jj1;
    long xsize, ysize;
    long pixradl;
    long ii1min, ii1max, jj1min, jj1max;
    float dx, dy, r;


    IDmaskRM = image_ID(IDmaskRM_name);
    xsize = data.image[IDmaskRM].md[0].size[0];
    ysize = data.image[IDmaskRM].md[0].size[1];

    pixradl = (long) pixrad + 1;

    IDout = create_2Dimage_ID(IDout_name, xsize, ysize);
    for(ii=0; ii<xsize*ysize; ii++)
        data.image[IDout].array.F[ii] = xsize+ysize;

    for(ii=0; ii<xsize; ii++)
        for(jj=0; jj<ysize; jj++)
        {
            if(data.image[IDmaskRM].array.F[jj*xsize+ii] < 0.5)
            {
                ii1min = ii-pixradl;
                if(ii1min<0)
                    ii1min=0;
                ii1max = ii+pixradl;
                if(ii1max>(xsize-1))
                    ii1max = xsize-1;

                jj1min = jj-pixradl;
                if(jj1min<0)
                    jj1min=0;
                jj1max = jj+pixradl;
                if(jj1max>(ysize-1))
                    jj1max = ysize-1;

                for(ii1=ii1min; ii1<ii1max+1; ii1++)
                    for(jj1=jj1min; jj1<jj1max+1; jj1++)
                        if(data.image[IDmaskRM].array.F[jj1*xsize+ii1]>0.5)
                        {
                            dx = 1.0*(ii-ii1);
                            dy = 1.0*(jj-jj1);
                            r = sqrt(dx*dx+dy*dy);
                            if(r<pixrad)
                                if(r < data.image[IDout].array.F[jj*xsize+ii])
                                    data.image[IDout].array.F[jj*xsize+ii] = r;
                        }
            }
        }

    for(ii=0; ii<xsize; ii++)
        for(jj=0; jj<ysize; jj++)
            if(data.image[IDout].array.F[jj*xsize+ii] > (xsize+ysize)/2 )
                data.image[IDout].array.F[jj*xsize+ii] = 0.0;


    return(IDout);
}



static long AOloopControl_DMedgeDetect(const char *IDmaskRM_name, const char *IDout_name)
{
    long IDout;
    long IDmaskRM;
    long ii, jj;
    float val1;
    long xsize, ysize;


    IDmaskRM = image_ID(IDmaskRM_name);
    xsize = data.image[IDmaskRM].md[0].size[0];
    ysize = data.image[IDmaskRM].md[0].size[1];

    IDout = create_2Dimage_ID(IDout_name, xsize, ysize);

    for(ii=1; ii<xsize-1; ii++)
        for(jj=1; jj<ysize-1; jj++)
        {
            val1 = 0.0;
            if(data.image[IDmaskRM].array.F[jj*xsize+ii] > 0.5)
            {
                if(data.image[IDmaskRM].array.F[jj*xsize+ii+1]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[jj*xsize+ii-1]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj+1)*xsize+ii]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj-1)*xsize+ii]<0.5)
                    val1 += 1.0;

                if(data.image[IDmaskRM].array.F[(jj+1)*xsize+ii+1]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj+1)*xsize+ii-1]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj-1)*xsize+ii+1]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj-1)*xsize+ii-1]<0.5)
                    val1 += 1.0;

                if(data.image[IDmaskRM].array.F[jj*xsize+ii+2]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[jj*xsize+ii-2]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj+2)*xsize+ii]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj-2)*xsize+ii]<0.5)
                    val1 += 1.0;



                if(data.image[IDmaskRM].array.F[(jj+1)*xsize+ii+2]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj+1)*xsize+ii-2]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj-1)*xsize+ii+2]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj-1)*xsize+ii-2]<0.5)
                    val1 += 1.0;

                if(data.image[IDmaskRM].array.F[(jj+2)*xsize+ii-1]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj-2)*xsize+ii-1]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj+2)*xsize+ii+1]<0.5)
                    val1 += 1.0;
                if(data.image[IDmaskRM].array.F[(jj-2)*xsize+ii+1]<0.5)
                    val1 += 1.0;

            }
            if(val1>4.9)
                val1 = 1.0;
            else
                val1 = 0.0;
            data.image[IDout].array.F[jj*xsize+ii] = val1;
        }

    return(IDout);
}



static long AOloopControl_DMextrapolateModes(const char *IDin_name, const char *IDmask_name, const char *IDcpa_name, const char *IDout_name)
{
    long IDin, IDmask, IDcpa, IDout;
    long xsize, ysize, zsize, xysize;
    long IDpixdist;
    long ii, jj, ii1, jj1, dii, djj, dii2, djj2;
    float r, dist;
    float coeff;
    long index;
    long kk;


    IDin = image_ID(IDin_name);
    xsize = data.image[IDin].md[0].size[0];
    ysize = data.image[IDin].md[0].size[1];
    if(data.image[IDin].md[0].naxis == 3)
    {
        zsize = data.image[IDin].md[0].size[2];
        IDout = create_3Dimage_ID(IDout_name, xsize, ysize, zsize);
    }
    else
    {
        zsize = 1;
        IDout = create_2Dimage_ID(IDout_name, xsize, ysize);
    }
    xysize = xsize*ysize;

    IDmask = image_ID(IDmask_name);
    IDcpa = image_ID(IDcpa_name);


    // measure pixel distance to mask
    IDpixdist = create_2Dimage_ID("pixmaskdist", xsize, ysize);
    for(ii=0; ii<xsize; ii++)
        for(jj=0; jj<ysize; jj++)
        {
            dist = 1.0*xsize+1.0*ysize;
            for(ii1=0; ii1<xsize; ii1++)
                for(jj1=0; jj1<ysize; jj1++)
                {
                    if(data.image[IDmask].array.F[jj1*xsize+ii1] > 0.5)
                    {
                        dii = ii1-ii;
                        djj = jj1-jj;
                        dii2 = dii*dii;
                        djj2 = djj*djj;
                        r = sqrt(dii2+djj2);
                        if(r<dist)
                            dist = r;
                    }
                }
            data.image[IDpixdist].array.F[jj*xsize+ii] = dist;
        }
    //save_fits("pixmaskdist", "!_tmp_pixmaskdist.fits");
    //save_fits(IDcpa_name, "!_tmp_cpa.fits");
    for(kk=0; kk<zsize; kk++)
    {
        for(ii=0; ii<xsize; ii++)
            for(jj=0; jj<ysize; jj++)
            {
                index = jj*xsize+ii;
                coeff =  data.image[IDpixdist].array.F[index] / ((1.0*xsize/(data.image[IDcpa].array.F[kk]+0.1))*0.8);

                coeff = (exp(-coeff*coeff)-exp(-1.0))  / (1.0 - exp(-1.0));
                if(coeff<0.0)
                    coeff = 0.0;
                data.image[IDout].array.F[kk*xysize+index] = coeff*data.image[IDin].array.F[kk*xysize+index]*coeff;
            }
    }
    delete_image_ID("pixmaskdist");


    return(IDout);
}



long AOloopControl_DMslaveExt(const char *IDin_name, const char *IDmask_name, const char *IDsl_name, const char *IDout_name, float r0)
{
    long IDin, IDmask, IDsl, IDout;
    long ii, jj, kk, ii1, jj1;
    long xsize, ysize, zsize, xysize;
    long index;
    float rfactor = 2.0;
    float val1, val1cnt;
    float pixrad;
    long pixradl;
    long ii1min, ii1max, jj1min, jj1max;
    float dx, dy, r, r1;
    float coeff;
    float valr;


    IDin = image_ID(IDin_name);
    xsize = data.image[IDin].md[0].size[0];
    ysize = data.image[IDin].md[0].size[1];
    if(data.image[IDin].md[0].naxis == 3)
    {
        zsize = data.image[IDin].md[0].size[2];
        IDout = create_3Dimage_ID(IDout_name, xsize, ysize, zsize);
    }
    else
    {
        zsize = 1;
        IDout = create_2Dimage_ID(IDout_name, xsize, ysize);
    }
    xysize = xsize*ysize;

    IDmask = image_ID(IDmask_name);
    IDsl = image_ID(IDsl_name);




    for(ii=0; ii<xsize; ii++)
        for(jj=0; jj<ysize; jj++)
        {
            index = jj*xsize+ii;
            if(data.image[IDmask].array.F[index]>0.5)
            {
                for(kk=0; kk<zsize; kk++)
                    data.image[IDout].array.F[kk*xysize+index] = data.image[IDin].array.F[kk*xysize+index];
            }
            else if (data.image[IDsl].array.F[index]>0.5)
            {
                for(kk=0; kk<zsize; kk++)
                {
                    val1 = 0.0;
                    val1cnt = 0.0;
                    pixrad = (rfactor*data.image[IDsl].array.F[index]+1.0);
                    pixradl = (long) pixrad + 1;

                    ii1min = ii-pixradl;
                    if(ii1min<0)
                        ii1min=0;
                    ii1max = ii+pixradl;
                    if(ii1max>(xsize-1))
                        ii1max = xsize-1;

                    jj1min = jj-pixradl;
                    if(jj1min<0)
                        jj1min=0;
                    jj1max = jj+pixradl;
                    if(jj1max>(ysize-1))
                        jj1max = ysize-1;


                    valr = 0.0;
                    for(ii1=ii1min; ii1<ii1max+1; ii1++)
                        for(jj1=jj1min; jj1<jj1max+1; jj1++)
                        {
                            dx = 1.0*(ii-ii1);
                            dy = 1.0*(jj-jj1);
                            r = sqrt(dx*dx+dy*dy);
                            if((r<pixrad)&&(data.image[IDmask].array.F[jj1*xsize+ii1]>0.5))
                            {
                                r1 = r/pixrad;
                                coeff = exp(-10.0*r1*r1);
                                valr += r*coeff;
                                val1 += data.image[IDin].array.F[kk*xysize+jj1*xsize+ii1]*coeff;
                                val1cnt += coeff;
                            }
                        }
                    valr /= val1cnt;
                    if(val1cnt>0.0001)
                        data.image[IDout].array.F[kk*xysize+index] = (val1/val1cnt)*exp(-(valr/r0)*(valr/r0));
                }
            }
            else
                for(kk=0; kk<zsize; kk++)
                    data.image[IDout].array.F[kk*xysize+index] = 0.0;
        }


    return(IDout);
}




/*** \brief creates AO control modes
 *
 *
 * creates image "modesfreqcpa" which contains CPA value for each mode
 *
 *
 * if Mmask exists, measure xc, yc from it, otherwise use values given to function
 *
 * INPUT (optional): "dmmaskRM" DM actuators directly controlled
 * INPUT (optional): "dmslaved" force these actuators to be slaved to neighbors
 * OUTPUT :          "dmmask" is the union of "dmmaskRM" and "dmslaved"
 *
 * MaskMode = 0  : tapered masking
 * MaskMode = 1  : STRICT masking
 *
 * if BlockNB < 0 : do all blocks
 * if BlockNB >= 0 : only update single block (untested)
 *
 * SVDlim = 0.05 works well
 *
 * OPTIONAL : if file zrespM exists, WFS modes will be computed
 *
 */

long AOloopControl_mkModes(const char *ID_name, long msizex, long msizey, float CPAmax, float deltaCPA, double xc, double yc, double r0, double r1, int MaskMode, int BlockNB, float SVDlim)
{
    FILE *fp;
    long ID = -1;
    long ii, jj;

    long IDmaskRM; // DM mask
    long IDmask; // DM mask

    double totm;

    double x, y, r, xc1, yc1;
    double rms;

    long IDz;

    long zindex[10];
    double zcpa[10];  /// CPA for each Zernike (somewhat arbitrary... used to sort modes in CPA)

    long mblock, m;
    long NBmblock;

    long MBLOCK_NBmode[MAX_MBLOCK]; // number of blocks
    long MBLOCK_ID[MAX_MBLOCK];
    long MBLOCK_IDwfs[MAX_MBLOCK];
    float MBLOCK_CPA[MAX_MBLOCK];


    char *ptr0;
    char *ptr1;

    char imname[200];
    char imname1[200];


    char fname[200];
    char fname1[200];


    float value, value0, value1, value1cnt, valuen;
    long msizexy;
    long m0, mblock0;

    long iter;

    long IDzrespM;
    long wfsxsize, wfsysize, wfssize;
    long wfselem, act;

    long IDout, ID1, ID2, ID2b;
    long zsize1, zsize2;
    long z1, z2;
    long xysize1, xysize2;



    float SVDlim00;// DM filtering step 0
    float SVDlim01; // DM filtering step 1


    float rmslim1 = 0.1;
    long IDm;


    int *mok;
    long NBmm = 2000; // max number of modes per block
    long cnt;


    int reuse;
    long IDSVDmodein, IDSVDmode1, IDSVDcoeff, IDSVDmask;
    long m1;
    long IDnewmodeC;

    long IDtmp, IDtmpg;
    long conviter;
    float sigma;

    int MODAL; // 1 if "pixels" of DM are already modes

    long IDRMMmodes = -1;
    long IDRMMresp  = -1;
    long ID_imfit = -1;
    long IDRMM_coeff = -1;
    long IDcoeffmat = -1;

    long linfitsize;
    int linfitreuse;
    double res, res1, v0;


    double resn, vn;
    double LOcoeff;
    FILE *fpLOcoeff;
    long IDwfstmp;

    long pixcnt;
    float vxp, vxm, vyp, vym, cxp, cxm, cyp, cym, ctot;
    long IDtmp1, IDtmp2;


    int COMPUTE_DM_MODES = 1; // compute DM modes (initial step) fmode2b_xxx and fmodes2ball


    long ii1, jj1;
    float dx, dy, dist, dist0, val1cnt;
    long IDprox, IDprox1;
    float gain;

    FILE *fpcoeff;
    char fnameSVDcoeff[400];


    // extra block
    long extrablockIndex;



    // SET LIMITS
    SVDlim00 = SVDlim; // DM filtering step 0
    SVDlim01 = SVDlim; // DM filtering step 1




    MODAL = 0;
    if(msizey==1)
        MODAL = 1;


    zindex[0] = 1; // tip
    zcpa[0] = 0.0;

    zindex[1] = 2; // tilt
    zcpa[1] = 0.0;

    zindex[2] = 4; // focus
    zcpa[2] = 0.25;

    zindex[3] = 3; // astig
    zcpa[3] = 0.4;

    zindex[4] = 5; // astig
    zcpa[4] = 0.4;

    zindex[5] = 7; // coma
    zcpa[5] = 0.6;

    zindex[6] = 8; // coma
    zcpa[6] = 0.6;

    zindex[7] = 6; // trefoil
    zcpa[7] = 1.0;

    zindex[8] = 9; // trefoil
    zcpa[8] = 1.0;

    zindex[9] = 12;
    zcpa[9] = 1.5;



    if(system("mkdir -p mkmodestmp") < 1)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    msizexy = msizex*msizey;

    /// STEP 1: CREATE STARTING POINT : ZERNIKES + FOURIER MODES

    /// if Mmask exists, use it, otherwise create it
    if(MODAL == 0)
    {
        IDmaskRM = image_ID("dmmaskRM");
        if(IDmaskRM==-1)
        {
            double val0, val1;
            double a0=0.88;
            double b0=40.0;
            double a1=1.2;
            double b1=12.0;

            IDmaskRM = create_2Dimage_ID("dmmaskRM", msizex, msizey);
            for(ii=0; ii<msizex; ii++)
                for(jj=0; jj<msizey; jj++)
                {
                    x = 1.0*ii-xc;
                    y = 1.0*jj-yc;
                    r = sqrt(x*x+y*y)/r1;
                    val1 = 1.0-exp(-pow(a1*r,b1));
                    r = sqrt(x*x+y*y)/r0;
                    val0 = exp(-pow(a0*r,b0));
                    data.image[IDmaskRM].array.F[jj*msizex+ii] = val0*val1;
                }
            save_fits("dmmaskRM", "!dmmaskRM.fits");
            xc1 = xc;
            yc1 = yc;
        }
        else /// extract xc and yc from mask
        {
            xc1 = 0.0;
            yc1 = 0.0;
            totm = 0.0;
            for(ii=0; ii<msizex; ii++)
                for(jj=0; jj<msizey; jj++)
                {
                    xc1 += 1.0*ii*data.image[IDmaskRM].array.F[jj*msizex+ii];
                    yc1 += 1.0*jj*data.image[IDmaskRM].array.F[jj*msizex+ii];
                    totm += data.image[IDmaskRM].array.F[jj*msizex+ii];
                }
            xc1 /= totm;
            yc1 /= totm;
        }

        totm = arith_image_total("dmmaskRM");
        if((msizex != data.image[IDmaskRM].md[0].size[0])||(msizey != data.image[IDmaskRM].md[0].size[1]))
        {
            printf("ERROR: file dmmaskRM size (%ld %ld) does not match expected size (%ld %ld)\n", (long) data.image[IDmaskRM].md[0].size[0], (long) data.image[IDmaskRM].md[0].size[1], (long) msizex, (long) msizey);
            exit(0);
        }
    }
    else
        totm = 1.0;




    COMPUTE_DM_MODES = 0;
    ID2b = image_ID("fmodes2ball");

    if(ID2b == -1)
        COMPUTE_DM_MODES = 1;



    if(COMPUTE_DM_MODES==1) // DM modes fmodes2b
    {
        long ID0 = -1;
        long NBZ = 0;
        long IDmfcpa;
        float CPAblocklim[MAX_MBLOCK]; // defines CPA limits for blocks

        long IDmask;
        long IDslaved;
        long IDmaskRMedge;



        if(MODAL==0)
        {
            long IDmaskRMin;

            // AOloopControl_mkloDMmodes(ID_name, msizex, msizey, CPAmax, deltaCPA, xc, yc, r0, r1, MaskMode);
            //NBZ = 5; /// 3: tip, tilt, focus
            NBZ = 0;
            for(m=0; m<10; m++)
            {
                if(zcpa[m]<CPAmax)
                    NBZ++;
            }


            // here we create simple Fourier modes
            linopt_imtools_makeCPAmodes("CPAmodes", msizex, CPAmax, deltaCPA, 0.5*msizex, 1.2, 0);
            ID0 = image_ID("CPAmodes");

            long IDfreq = image_ID("cpamodesfreq");



            printf("  %ld %ld %ld\n", msizex, msizey, (long) (data.image[ID0].md[0].size[2]-1) );
            ID = create_3Dimage_ID(ID_name, msizex, msizey, data.image[ID0].md[0].size[2]-1+NBZ);

            IDmfcpa = create_2Dimage_ID("modesfreqcpa", data.image[ID0].md[0].size[2]-1+NBZ, 1);


            zernike_init();

            double PA;
            uint_fast32_t k;
            for(k=0; k<NBZ; k++)
            {
                data.image[IDmfcpa].array.F[k] = zcpa[k];
                for(ii=0; ii<msizex; ii++)
                    for(jj=0; jj<msizey; jj++)
                    {
                        x = 1.0*ii-xc1;
                        y = 1.0*jj-yc1;
                        r = sqrt(x*x+y*y)/r1;
                        PA = atan2(y,x);
                        data.image[ID].array.F[k*msizex*msizey+jj*msizex+ii] = Zernike_value(zindex[k], r, PA);
                    }
            }

            for(k=0; k<data.image[ID0].md[0].size[2]-1; k++)
            {
                data.image[IDmfcpa].array.F[k+NBZ] = data.image[IDfreq].array.F[k+1];
                for(ii=0; ii<msizex*msizey; ii++)
                    data.image[ID].array.F[(k+NBZ)*msizex*msizey+ii] = data.image[ID0].array.F[(k+1)*msizex*msizey+ii];
            }


            fp = fopen("rmscomp.dat", "w");



            for(k=0; k<data.image[ID0].md[0].size[2]-1+NBZ; k++)
            {
                // set RMS = 1 over mask
                rms = 0.0;
                for(ii=0; ii<msizex*msizey; ii++)
                {
                    //data.image[ID].array.F[k*msizex*msizey+ii] -= offset/totm;
                    rms += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[ID].array.F[k*msizex*msizey+ii]*data.image[IDmaskRM].array.F[ii];
                }
                rms = sqrt(rms/totm);
                printf("Mode %ld   RMS = %lf\n", k, rms);

                fprintf(fp, "%5ld  %g ", k, rms);

                /// Remove excluded modes if they exist
                /*          IDeModes = image_ID("emodes");
                          if(IDeModes!=-1)
                          {
                              IDtm = create_2Dimage_ID("tmpmode", msizex, msizey);

                              for(ii=0; ii<msizex*msizey; ii++)
                                  data.image[IDtm].array.F[ii] = data.image[ID].array.F[k*msizex*msizey+ii];
                              linopt_imtools_image_fitModes("tmpmode", "emodes", "dmmaskRM", 1.0e-3, "lcoeff", 0);
                              linopt_imtools_image_construct("emodes", "lcoeff", "em00");
                              delete_image_ID("lcoeff");
                              IDem = image_ID("em00");

                //					coeff = 1.0-exp(-pow(1.0*k/kelim,6.0));

                			if(k>kelim)
                				coeff = 1.0;
                			else
                				coeff = 0.0;


                              for(ii=0; ii<msizex*msizey; ii++)
                                  data.image[ID].array.F[k*msizex*msizey+ii] = data.image[IDtm].array.F[ii] - coeff*data.image[IDem].array.F[ii];

                              delete_image_ID("em00");
                              delete_image_ID("tmpmode");
                          }*/


                // Compute total of image over mask -> totvm
                double totvm = 0.0;
                for(ii=0; ii<msizex*msizey; ii++)
                    totvm += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[IDmaskRM].array.F[ii];

                // compute DC offset in mode
                double offset = totvm/totm;

                // remove DM offset
                for(ii=0; ii<msizex*msizey; ii++)
                    data.image[ID].array.F[k*msizex*msizey+ii] -= offset;

                offset = 0.0;
                for(ii=0; ii<msizex*msizey; ii++)
                    offset += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[IDmaskRM].array.F[ii];

                // set RMS = 1 over mask
                rms = 0.0;
                for(ii=0; ii<msizex*msizey; ii++)
                {
                    data.image[ID].array.F[k*msizex*msizey+ii] -= offset/totm;
                    rms += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[ID].array.F[k*msizex*msizey+ii]*data.image[IDmaskRM].array.F[ii];
                }
                rms = sqrt(rms/totm);
                printf("\r Mode %ld   RMS = %lf   ", k, rms);
                fprintf(fp, " %g\n", rms);

                for(ii=0; ii<msizex*msizey; ii++)
                    data.image[ID].array.F[k*msizex*msizey+ii] /= rms;
            }
            fclose(fp);
			printf("\n");


            if(MaskMode==1)
            {
                long kernsize = 5;
                if(2*kernsize>msizex)
                    kernsize = msizex/2;
                long citer;
                long NBciter = 200;
                for(citer=0; citer<NBciter; citer++)
                {
                    printf("Convolution [%3ld/%3ld]\n", citer, NBciter);
                    gauss_filter(ID_name, "modeg", 4.0*pow(1.0*(NBciter-citer)/NBciter,0.5), kernsize);
                    long IDg = image_ID("modeg");
                    uint_fast32_t  k;
                    for(k=0; k<data.image[ID].md[0].size[2]; k++)
                    {
                        for(ii=0; ii<msizex*msizey; ii++)
                            if(data.image[IDmaskRM].array.F[ii]<0.98)
                                data.image[ID].array.F[k*msizex*msizey+ii] = data.image[IDg].array.F[k*msizex*msizey+ii];
                    }
                    delete_image_ID("modeg");
                }
            }







            // MAKE MASKS FOR EDGE EXTRAPOLATION


            IDslaved = image_ID("dmslaved");
            // load or create DM mask : union of dmslaved and dmmaskRM
            //IDmask = load_fits("dmmask.fits", "dmmask", 1);
            printf("Create DM mask\n");
            fflush(stdout);


            //IDmask = -1;
            //if(IDmask == -1)
            //{
            IDmask = create_2Dimage_ID("dmmask", msizex, msizey);
            for(ii=0; ii<msizex*msizey; ii++)
            {
                data.image[IDmask].array.F[ii] = 1.0 - (1.0-data.image[IDmaskRM].array.F[ii])*(1.0-data.image[IDslaved].array.F[ii]);
            //    data.image[IDmask].array.F[ii] = 1.0 - (1.0-data.image[IDslaved].array.F[ii]);
                if(data.image[IDmask].array.F[ii]>1.0)
                    data.image[IDmask].array.F[ii] = 1.0;
            }
            save_fits("dmmask", "!dmmask.fits");
            //}

            // EDGE PIXELS IN IDmaskRM
            IDmaskRMedge = AOloopControl_DMedgeDetect(data.image[IDmaskRM].md[0].name, "dmmaskRMedge");
            save_fits("dmmaskRMedge", "!dmmaskRMedge.fits");

            // IDmaskRM pixels excluding edge
            IDmaskRMin = create_2Dimage_ID("dmmaskRMin", msizex, msizey);
            for(ii=0; ii<msizex*msizey; ii++)
                data.image[IDmaskRMin].array.F[ii] = data.image[IDmaskRM].array.F[ii] * (1.0 - data.image[IDmaskRMedge].array.F[ii]);
            save_fits("dmmaskRMin", "!dmmaskRMin.fits");


            save_fits(ID_name, "!./mkmodestmp/_test_fmodes0all00.fits");


            IDtmp = AOloopControl_DMextrapolateModes(ID_name, "dmmaskRMin", "modesfreqcpa", "fmodes0test");
            save_fits("fmodes0test", "!fmodes0test.fits");


            for(m=0; m<data.image[ID].md[0].size[2]; m++)
            {
                for(ii=0; ii<msizex*msizey; ii++)
                    data.image[ID].array.F[m*msizex*msizey+ii] = data.image[IDtmp].array.F[m*msizex*msizey+ii] * data.image[IDmask].array.F[ii];
            }
        }
        else
        {
            ID = create_3Dimage_ID(ID_name, msizex, msizey, msizex);
            IDmfcpa = create_2Dimage_ID("modesfreqcpa", msizex, 1);

            for(m=0; m<data.image[ID].md[0].size[2]; m++)
            {
                if(m<10)
                    data.image[IDmfcpa].array.F[m] = zcpa[m];
                else
                    data.image[IDmfcpa].array.F[m] = zcpa[9];

                for(ii=0; ii<msizex*msizey; ii++)
                    data.image[ID].array.F[m*msizex*msizey+ii] = 0.0;
                data.image[ID].array.F[m*msizex*msizey+m] = 1.0;
            }
        }



        printf("SAVING MODES : %s...\n", ID_name);
        save_fits(ID_name, "!./mkmodestmp/fmodes0all_00.fits");






        // remove modes
        uint_fast32_t k;
        for(k=0; k<data.image[ID0].md[0].size[2]-1 + NBZ; k++)
        {
            /// Remove excluded modes if they exist
            long IDeModes = image_ID("emodes");
            if(IDeModes!=-1)
            {
                long kelim = 5;
                long IDtm = create_2Dimage_ID("tmpmode", msizex, msizey);

                for(ii=0; ii<msizex*msizey; ii++)
                    data.image[IDtm].array.F[ii] = data.image[ID].array.F[k*msizex*msizey+ii];
                linopt_imtools_image_fitModes("tmpmode", "emodes", "dmmask", 1.0e-3, "lcoeff", 0);
                linopt_imtools_image_construct("emodes", "lcoeff", "em00");
                delete_image_ID("lcoeff");
                long IDem = image_ID("em00");

                double coeff = 1.0-exp(-pow(1.0*k/kelim,6.0));

                if(k>kelim)
                    coeff = 1.0;
                else
                    coeff = 0.0;


                for(ii=0; ii<msizex*msizey; ii++)
                    data.image[ID].array.F[k*msizex*msizey+ii] = data.image[IDtm].array.F[ii] - coeff*data.image[IDem].array.F[ii];

                delete_image_ID("em00");
                delete_image_ID("tmpmode");
            }

            // Compute total of image over mask -> totvm
            double ave = 0.0;
            double totvm = 0.0;
            totm = 0.0;
            for(ii=0; ii<msizex*msizey; ii++)
            {
                totvm += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[IDmask].array.F[ii];
                totm += data.image[IDmask].array.F[ii];
            }

            // compute DC offset in mode
            double offset = totvm/totm;

            // remove DM offset
            for(ii=0; ii<msizex*msizey; ii++)
                data.image[ID].array.F[k*msizex*msizey+ii] -= offset;

            offset = 0.0;
            for(ii=0; ii<msizex*msizey; ii++)
                offset += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[IDmask].array.F[ii];

            // set RMS = 1 over mask
            rms = 0.0;
            for(ii=0; ii<msizex*msizey; ii++)
            {
                data.image[ID].array.F[k*msizex*msizey+ii] -= offset/totm;
                rms += data.image[ID].array.F[k*msizex*msizey+ii]*data.image[ID].array.F[k*msizex*msizey+ii]*data.image[IDmask].array.F[ii];
            }
            rms = sqrt(rms/totm);
            printf("Mode %ld   RMS = %lf\n", k, rms);

            for(ii=0; ii<msizex*msizey; ii++)
                data.image[ID].array.F[k*msizex*msizey+ii] /= rms;
        }

        save_fits(ID_name, "!./mkmodestmp/fmodes0all.fits");









        long IDmodes0all = image_ID(ID_name);
        printf("DONE SAVING\n");

        // time : 0:04



        /// COMPUTE WFS RESPONSE TO MODES -> fmodesWFS00all.fits
        msizexy = msizex*msizey;
        ID = image_ID(ID_name);
        IDzrespM = image_ID("zrespM");
        save_fits("zrespM", "!_test_zrespM.fits");
        save_fits(ID_name, "!_test_name.fits");
        if(data.image[IDzrespM].md[0].size[2]!=msizexy)
        {
            printf("ERROR: zrespM has wrong z size : %ld, should be %ld\n", (long) data.image[IDzrespM].md[0].size[2], (long) msizexy);
            exit(0);
        }

        wfsxsize = data.image[IDzrespM].md[0].size[0];
        wfsysize = data.image[IDzrespM].md[0].size[1];
        wfssize = wfsxsize*wfsysize;
        IDm = create_3Dimage_ID("fmodesWFS00all", wfsxsize, wfsysize, data.image[ID].md[0].size[2]);

        printf("size: %ld %ld %ld\n", (long) data.image[ID].md[0].size[2], msizexy, wfssize);
        printf("\n");

        long act1, act2;
# ifdef _OPENMP
        #pragma omp parallel for private(m,m1,act,act1,act2,wfselem)
# endif

        for(m=0; m<data.image[ID].md[0].size[2]; m++)
        {
            m1 = m*wfssize;

            printf("\r %5ld / %5ld   ", m, (long) data.image[ID].md[0].size[2]);
            fflush(stdout);
            for(act=0; act<msizexy; act++)
            {
                act1 = m*msizexy+act;
                act2 = act*wfssize;
                for(wfselem=0; wfselem<wfssize; wfselem++)
                {
                    data.image[IDm].array.F[m1+wfselem] += data.image[ID].array.F[act1] * data.image[IDzrespM].array.F[act2+wfselem];
                }
            }
        }



        // if modal response matrix exists, use it
        IDRMMmodes = image_ID("RMMmodes"); // modal resp matrix modes
        IDRMMresp = image_ID("RMMresp"); // modal resp matrix

        fpLOcoeff = fopen("./mkmodestmp/LOcoeff.txt", "w");
        if(fpLOcoeff == NULL)
        {
            printf("ERROR: cannot create file \"LOcoeff.txt\"\n");
            exit(0);
        }
        save_fits("fmodesWFS00all", "!./mkmodestmp/fmodesWFS00all.HO.fits");

        if((IDRMMmodes!=-1)&&(IDRMMresp!=-1))
        {
            linfitsize = data.image[IDRMMmodes].md[0].size[2];
            IDRMM_coeff = create_2Dimage_ID("linfitcoeff", linfitsize, 1);

            ID_imfit = create_2Dimage_ID("imfitim", msizex, msizey);

            IDcoeffmat = create_2Dimage_ID("imfitmat", linfitsize, data.image[ID].md[0].size[2]);

            linfitreuse = 0;

            IDwfstmp = create_2Dimage_ID("wfsimtmp", wfsxsize, wfsysize);

            for(m=0; m<data.image[IDmodes0all].md[0].size[2]; m++)
            {
                for(ii=0; ii<msizexy; ii++)
                    data.image[ID_imfit].array.F[ii] = data.image[IDmodes0all].array.F[m*msizexy+ii];

                linopt_imtools_image_fitModes("imfitim", "RMMmodes", "dmmaskRM", 1.0e-2, "linfitcoeff", linfitreuse);
                linfitreuse = 1;

                for(jj=0; jj<linfitsize; jj++)
                    data.image[IDcoeffmat].array.F[m*linfitsize+jj] = data.image[IDRMM_coeff].array.F[jj];

                // construct linear fit result (DM)
                IDtmp = create_2Dimage_ID("testrc", msizex, msizey);
                for(jj=0; jj<linfitsize; jj++)
                    for(ii=0; ii<msizex*msizey; ii++)
                        data.image[IDtmp].array.F[ii] += data.image[IDRMM_coeff].array.F[jj]*data.image[IDRMMmodes].array.F[jj*msizex*msizey+ii];

                res = 0.0;
                resn = 0.0;
                for(ii=0; ii<msizex*msizey; ii++)
                {
                    v0 = data.image[IDtmp].array.F[ii]-data.image[ID_imfit].array.F[ii];
                    vn = data.image[ID_imfit].array.F[ii];
                    res += v0*v0;
                    resn += vn*vn;
                }
                res /= resn;

                res1 = 0.0;
                for(jj=0; jj<linfitsize; jj++)
                    res1 += data.image[IDRMM_coeff].array.F[jj]*data.image[IDRMM_coeff].array.F[jj];

                delete_image_ID("testrc");


                LOcoeff = 1.0/(1.0+pow(10.0*res, 4.0));

                if(res1>1.0)
                    LOcoeff *= 1.0/(1.0+pow((res1-1.0)*0.1, 2.0));


                fprintf(fpLOcoeff, "%5ld   %20g  %20g   ->  %f\n", m, res, res1, LOcoeff);
                // printf("%5ld   %20g  %20g   ->  %f\n", m, res, res1, LOcoeff);

                if(LOcoeff>0.01)
                {
                    // construct linear fit (WFS space)
                    for(wfselem=0; wfselem<wfssize; wfselem++)
                        data.image[IDwfstmp].array.F[wfselem] = 0.0;
                    for(jj=0; jj<linfitsize; jj++)
                        for(wfselem=0; wfselem<wfssize; wfselem++)
                            data.image[IDwfstmp].array.F[wfselem] += data.image[IDRMM_coeff].array.F[jj] * data.image[IDRMMresp].array.F[jj*wfssize+wfselem];

                    for(wfselem=0; wfselem<wfssize; wfselem++)
                        data.image[IDm].array.F[m*wfssize+wfselem] = LOcoeff*data.image[IDwfstmp].array.F[wfselem] + (1.0-LOcoeff)*data.image[IDm].array.F[m*wfssize+wfselem];
                }
            }

            delete_image_ID("linfitcoeff");
            delete_image_ID("imfitim");
            delete_image_ID("wfsimtmp");
            save_fits("imfitmat", "!imfitmat.fits");
            delete_image_ID("imfitmat");
        }
        fclose(fpLOcoeff);

        printf("\n");
        save_fits("fmodesWFS00all", "!./mkmodestmp/fmodesWFS00all.fits");


        //    exit(0);




        // time : 0:42





        /// STEP 2: SEPARATE DM MODES INTO BLOCKS AND MASK
        msizexy = msizex*msizey;

        CPAblocklim[0] = 0.1; // tip and tilt
        CPAblocklim[1] = 0.3; // focus
        CPAblocklim[2] = 1.6; // other Zernikes
        CPAblocklim[3] = 3.0;
        CPAblocklim[4] = 5.0;
        CPAblocklim[5] = 7.0;
        CPAblocklim[6] = 9.0;
        CPAblocklim[7] = 11.0;
        CPAblocklim[8] = 13.0;
        CPAblocklim[9] = 15.0;
        CPAblocklim[10] = 17.0;
        CPAblocklim[11] = 19.0;
        CPAblocklim[12] = 21.0;
        CPAblocklim[13] = 100.0;

        for(mblock=0; mblock<MAX_MBLOCK; mblock++)
            MBLOCK_NBmode[mblock] = 0;



        NBmblock = 0;
        for(m=0; m<data.image[ID].md[0].size[2]; m++)
        {
            float cpa = data.image[IDmfcpa].array.F[m];
            mblock = 0;
            while (cpa > CPAblocklim[mblock])
            {
                //    printf("[%ld  %f %f -> +]\n", mblock, cpa, CPAblocklim[mblock]);
                mblock++;
            }

            MBLOCK_NBmode[mblock]++;

            if(mblock>NBmblock)
                NBmblock = mblock;

            //    printf("%ld %f  -> %ld\n", m, cpa, mblock);
        }

        NBmblock++;


        long IDextrablock = image_ID("extrablockM");
        if(IDextrablock != -1)
        {
            extrablockIndex = 4;

            fp = fopen("./conf/param_extrablockIndex.txt", "r");
            if(fp != NULL)
            {
                if(fscanf(fp, "%50ld", &extrablockIndex) != 1)
                    printERROR(__FILE__, __func__, __LINE__, "cannot read parameter from file");
                fclose(fp);
            }
        }



        for(mblock=0; mblock<NBmblock; mblock++)
        {
            long mblock1;

            if(IDextrablock != -1)
            {
                mblock1 = mblock;
                if(mblock>extrablockIndex-1)
                    mblock1 = mblock+1;
            }
            else
                mblock1 = mblock;

            if(sprintf(imname, "fmodes0_%02ld", mblock1) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            MBLOCK_ID[mblock1] = create_3Dimage_ID(imname, msizex, msizey, MBLOCK_NBmode[mblock]);
            MBLOCK_ID[mblock1] = image_ID(imname);
        }



        for(mblock=0; mblock<MAX_MBLOCK; mblock++)
            MBLOCK_NBmode[mblock] = 0;


		ID = image_ID("fmodes");
        for(m=0; m<data.image[ID].md[0].size[2]; m++)
        {
            long mblock1;

            float cpa = data.image[IDmfcpa].array.F[m];

            mblock = 0;
            while (cpa > CPAblocklim[mblock])
                mblock++;

            if(IDextrablock!= -1)
            {
                mblock1 = mblock;
                if(mblock>extrablockIndex-1)
                    mblock1 = mblock+1;
            }
            else
                mblock1 = mblock;


            for(ii=0; ii<msizex*msizey; ii++)
                data.image[MBLOCK_ID[mblock1]].array.F[MBLOCK_NBmode[mblock1]*msizex*msizey+ii] = data.image[ID].array.F[m*msizex*msizey+ii]*data.image[IDmaskRM].array.F[ii];

            MBLOCK_NBmode[mblock1]++;
        }


        if(IDextrablock != -1)
        {
            mblock = extrablockIndex;

            if(sprintf(imname, "fmodes0_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            MBLOCK_NBmode[mblock] = data.image[IDextrablock].md[0].size[2];
            MBLOCK_ID[mblock] = create_3Dimage_ID(imname, msizex, msizey, MBLOCK_NBmode[mblock]);

            for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                for(ii=0; ii<msizex*msizey; ii++)
                    data.image[MBLOCK_ID[mblock]].array.F[m*msizex*msizey+ii] = data.image[IDextrablock].array.F[m*msizex*msizey+ii]*data.image[IDmaskRM].array.F[ii];

            NBmblock++;
        }





        // time : 00:42

        /// STEP 3: REMOVE NULL SPACE WITHIN EACH BLOCK - USE SVDlim00 FOR CUTOFF -> fmodes1all.fits  (DM space)
        printf("STEP 3: REMOVE NULL SPACE WITHIN EACH BLOCK - USE SVDlim00 FOR CUTOFF -> fmodes1all.fits  (DM space)\n");
        fflush(stdout);

        for(mblock=0; mblock<NBmblock; mblock++)
        {
            printf("\nMODE BLOCK %ld\n", mblock);
            fflush(stdout);

            if(sprintf(imname, "fmodes0_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

			//TEST
			//sprintf(fname, "!./mkmodestmp/fmodes0_%02ld.fits", mblock);
			//save_fits(imname, fname);

            printf("SVD decomp ... (%ld) .... ", (long) data.image[image_ID(imname)].md[0].size[2]);
            fflush(stdout);
            linopt_compute_SVDdecomp(imname, "svdmodes", "svdcoeff");
            printf("DONE\n");
            fflush(stdout);
            cnt = 0;
            IDSVDcoeff = image_ID("svdcoeff");
            float svdcoeff0 = data.image[IDSVDcoeff].array.F[0];
            for(m=0; m<data.image[IDSVDcoeff].md[0].size[0]; m++)
            {
				//printf("( %ld -> %g )\n", m, data.image[IDSVDcoeff].array.F[m]);
                if(data.image[IDSVDcoeff].array.F[m] > SVDlim00*svdcoeff0)
                    cnt++;
            }
            printf("BLOCK %ld/%ld: keeping %ld / %ld modes  ( %f %f ) [%ld  %ld %ld]\n", mblock, NBmblock, cnt, m, SVDlim00, svdcoeff0, (long) data.image[IDSVDcoeff].md[0].size[0], msizex, msizey);
            fflush(stdout);

            if(sprintf(imname1, "fmodes1_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            IDm = create_3Dimage_ID(imname1, msizex, msizey, cnt);
            long IDSVDmodes = image_ID("svdmodes");
            for(ii=0; ii<cnt*msizex*msizey; ii++)
                data.image[IDm].array.F[ii] = data.image[IDSVDmodes].array.F[ii];

            MBLOCK_NBmode[mblock] = cnt;
            MBLOCK_ID[mblock] = IDm;

            if(sprintf(fname1, "!./mkmodestmp/fmodes1_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits(imname1, fname1);

            delete_image_ID("svdmodes");
            delete_image_ID("svdcoeff");
        }


        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
            cnt += MBLOCK_NBmode[mblock];
        IDm = create_3Dimage_ID("fmodes1all", msizex, msizey, cnt);
        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
        {
            for(m=0; m<MBLOCK_NBmode[mblock]; m++)
            {
                for(ii=0; ii<msizexy; ii++)
                    data.image[IDm].array.F[cnt*msizexy+ii] = data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii];
                // printf("Writing cnt %ld    %ld of %ld  [%ld -> %ld]\n", cnt, m, mblock, MBLOCK_ID[mblock], IDm);
                cnt++;
            }
        }
        save_fits("fmodes1all", "!./mkmodestmp/fmodes1all.fits");


	


        /// STEP 4: REMOVE MODES THAT ARE CONTAINED IN PREVIOUS BLOCKS, AND ENFORCE DM-SPACE ORTHOGONALITY BETWEEN BLOCKS -> fmodes2all.fits  (DM space)
        /// fmodes1all -> fmodes2all
        printf("STEP 4: REMOVE MODES THAT ARE CONTAINED IN PREVIOUS BLOCKS, AND ENFORCE DM-SPACE ORTHOGONALITY BETWEEN BLOCKS -> fmodes2all.fits  (DM space)\n");
        fflush(stdout);

        IDSVDmask = create_2Dimage_ID("SVDmask", msizex, msizey);
        for(ii=0; ii<msizexy; ii++)
            data.image[IDSVDmask].array.F[ii] = data.image[IDmaskRM].array.F[ii];
        IDSVDmodein = create_2Dimage_ID("SVDmodein", msizex, msizey);

        mok = (int*) malloc(sizeof(int)*NBmm);
        for(m=0; m<NBmm; m++)
            mok[m] = 1;

        for(mblock=0; mblock<NBmblock; mblock++)
        {
            for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                mok[m] = 1;
            for(mblock0=0; mblock0<mblock; mblock0++)
            {
                reuse = 0;
                for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                {
                    //  printf("STEP 4: REMOVING BLOCK %ld from   block %ld mode %ld/%ld      ", mblock0, mblock, m, MBLOCK_NBmode[mblock]);
                    //  fflush(stdout);

                    for(ii=0; ii<msizexy; ii++)
                        data.image[IDSVDmodein].array.F[ii] = data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii];

                    if(sprintf(imname, "fmodes1_%02ld", mblock0) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    linopt_imtools_image_fitModes("SVDmodein", imname, "SVDmask", 1.0e-2, "modecoeff", reuse);


                    reuse = 1;
                    linopt_imtools_image_construct(imname, "modecoeff", "SVDmode1");
                    IDSVDmode1 = image_ID("SVDmode1");
                    delete_image_ID("modecoeff");
                    value1 = 0.0;
                    for(ii=0; ii<msizexy; ii++)
                    {
                        data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii] -= data.image[IDSVDmode1].array.F[ii];;
                        value1 += data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii]*data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii];
                    }
                    delete_image_ID("SVDmode1");

                    rms = sqrt(value1/totm);
                    float rmslim0 = 0.01;
                    if(rms>rmslim0)
                    {
                        //       for(ii=0; ii<msizexy; ii++)
                        //         data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii] /= rms;
                    }
                    else
                        mok[m] = 0;

                    //                    printf("->  %12g (%g %g)\n", rms, value1, totm);
                    //					fflush(stdout);
                }
            }
            cnt = 0;
            for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                cnt += mok[m];
            printf("====== BLOCK %ld : keeping %ld / %ld modes\n", mblock, cnt, MBLOCK_NBmode[mblock]);
            fflush(stdout);
            if(cnt>0)
            {
                if(sprintf(imname, "fmodes2_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                printf("saving result %s \n", imname);
                fflush(stdout);
                IDm = create_3Dimage_ID(imname, msizex, msizey, cnt);
                m1 = 0;
                for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                {
                    if(mok[m]==1)
                    {
                        for(ii=0; ii<msizexy; ii++)
                            data.image[IDm].array.F[m1*msizex*msizey+ii] = data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii];
                        printf("BLOCK %ld   [%ld]  m1 = %ld / %ld\n", mblock, IDm, m1, cnt);
                        fflush(stdout);
                        m1++;
                    }
                }
                MBLOCK_ID[mblock] = IDm;

                char fname2[200];
                if(sprintf(fname2, "!./mkmodestmp/fmodes2_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                save_fits(imname, fname2);
            }
            MBLOCK_NBmode[mblock] = cnt;
        }

        delete_image_ID("SVDmask");
        delete_image_ID("SVDmodein");

        free(mok);


        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
            cnt += MBLOCK_NBmode[mblock];
        IDm = create_3Dimage_ID("fmodes2all", msizex, msizey, cnt);


        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
        {

            for(m=0; m<MBLOCK_NBmode[mblock]; m++)
            {
                for(ii=0; ii<msizexy; ii++)
                    data.image[IDm].array.F[cnt*msizexy+ii] = data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii];
                cnt++;
            }
        }
        save_fits("fmodes2all", "!./mkmodestmp/fmodes2all.fits");







        /// STEP 5: REMOVE NULL SPACE WITHIN EACH BLOCK - USE SVDlim01 FOR CUTOFF -> fmodes2ball.fits  (DM space)
        for(mblock=0; mblock<NBmblock; mblock++)
        {
            printf("MODE BLOCK %ld\n", mblock);
            fflush(stdout);

            if(sprintf(imname, "fmodes2_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            printf("SVD decomp ...");
            fflush(stdout);
            linopt_compute_SVDdecomp(imname, "svdmodes", "svdcoeff");
            printf("DONE\n");
            fflush(stdout);
            cnt = 0;
            IDSVDcoeff = image_ID("svdcoeff");
            float svdcoeff0 = data.image[IDSVDcoeff].array.F[0];

            if(sprintf(fnameSVDcoeff, "./mkmodestmp/SVDcoeff01_%02ld.txt", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            fpcoeff = fopen(fnameSVDcoeff, "w");
            for(m=0; m<data.image[IDSVDcoeff].md[0].size[0]; m++)
            {
                fprintf(fpcoeff, "%5ld   %12g   %12g  %5ld     %10.8f  %10.8f\n", m, data.image[IDSVDcoeff].array.F[m], data.image[IDSVDcoeff].array.F[0], cnt, data.image[IDSVDcoeff].array.F[m]/data.image[IDSVDcoeff].array.F[0], SVDlim01);

                if(data.image[IDSVDcoeff].array.F[m]>SVDlim01*svdcoeff0)
                    cnt++;
            }
            fclose(fpcoeff);

            printf("BLOCK %ld/%ld: keeping %ld / %ld modes\n", mblock, NBmblock, cnt, m);
            fflush(stdout);

            if(sprintf(imname1, "fmodes2b_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            IDm = create_3Dimage_ID(imname1, msizex, msizey, cnt);
            long IDSVDmodes = image_ID("svdmodes");
            for(ii=0; ii<cnt*msizex*msizey; ii++)
                data.image[IDm].array.F[ii] = data.image[IDSVDmodes].array.F[ii];

            for(m=0; m<cnt; m++)
            {
                value1 = 0.0;
                value1cnt = 0.0;
                for(ii=0; ii<msizexy; ii++)
                {
                    value1 += data.image[IDm].array.F[m*msizexy+ii]*data.image[IDmaskRM].array.F[ii];
                    value1cnt += data.image[IDmaskRM].array.F[ii];
                }
                for(ii=0; ii<msizexy; ii++)
                    data.image[IDm].array.F[m*msizexy+ii] -= value1/value1cnt;

                value1 = 0.0;
                for(ii=0; ii<msizexy; ii++)
                    value1 += data.image[IDm].array.F[m*msizexy+ii]*data.image[IDm].array.F[m*msizexy+ii]*data.image[IDmaskRM].array.F[ii];
                rms = sqrt(value1/value1cnt);
                for(ii=0; ii<msizexy; ii++)
                    data.image[IDm].array.F[m*msizexy+ii] /= rms;
            }

            // Extrapolate outside maskRM
            IDtmp = AOloopControl_DMslaveExt(data.image[IDm].md[0].name, data.image[IDmaskRM].md[0].name, "dmslaved", "fmodesext", 100.0);
            for(m=0; m<cnt; m++)
                for(ii=0; ii<msizexy; ii++)
                    data.image[IDm].array.F[m*msizexy+ii] = data.image[IDtmp].array.F[m*msizexy+ii];
            delete_image_ID("fmodesext");


            MBLOCK_NBmode[mblock] = cnt;
            MBLOCK_ID[mblock] = IDm;

            if(sprintf(fname1, "!./mkmodestmp/fmodes2b_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits(imname1, fname1);

            delete_image_ID("svdmodes");
            delete_image_ID("svdcoeff");
        }


        fp = fopen("./mkmodestmp/NBblocks.txt", "w");
        fprintf(fp, "%ld\n", NBmblock);
        fclose(fp);

        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
            cnt += MBLOCK_NBmode[mblock];
        IDm = create_3Dimage_ID("fmodes2ball", msizex, msizey, cnt);
        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
        {
            for(m=0; m<MBLOCK_NBmode[mblock]; m++)
            {
                for(ii=0; ii<msizexy; ii++)
                    data.image[IDm].array.F[cnt*msizexy+ii] = data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii];
                // printf("Writing cnt %ld    %ld of %ld  [%ld -> %ld]\n", cnt, m, mblock, MBLOCK_ID[mblock], IDm);
                cnt++;
            }
        }
        save_fits("fmodes2ball", "!./mkmodestmp/fmodes2ball.fits");
    }
    else
    {
        fp = fopen("./mkmodestmp/NBblocks.txt", "r");
        if(fscanf(fp, "%50ld", &NBmblock) != 1)
            printERROR(__FILE__, __func__, __LINE__, "Cannot read parameter from file");

        fclose(fp);
        for(mblock=0; mblock<NBmblock; mblock++)
        {
            if(sprintf(fname, "./mkmodestmp/fmodes2b_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(sprintf(imname, "fmodes2b_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            ID = load_fits(fname, imname, 1);
            MBLOCK_NBmode[mblock] = data.image[ID].md[0].size[2];
            MBLOCK_ID[mblock] = ID;
        }
    }







    // 1:25


    // ==================================================



    // WFS modes
    IDzrespM = image_ID("zrespM");
    if(IDzrespM!=-1) // compute WFS response to DM modes
    {
        /// STEP 6: COMPUTE WFS RESPONSE TO MODES
        /// fmodes2ball -> fmodesWFS0all.fits

        char imnameDM[200];
        char imnameDM1[200];

        if(BlockNB<0)
        {   // check size
            if(data.image[IDzrespM].md[0].size[2]!=msizexy)
            {
                printf("ERROR: zrespM has wrong z size : %ld, should be %ld\n", (long) data.image[IDzrespM].md[0].size[2], (long) msizexy);
                exit(0);
            }

            wfsxsize = data.image[IDzrespM].md[0].size[0];
            wfsysize = data.image[IDzrespM].md[0].size[1];
            wfssize = wfsxsize*wfsysize;


            /// Load ... or create WFS mask
            long IDwfsmask = image_ID("wfsmask");
            if((wfsxsize!=data.image[IDwfsmask].md[0].size[0])||(wfsysize!=data.image[IDwfsmask].md[0].size[1]))
            {
                printf("ERROR: File wfsmask has wrong size\n");
                exit(0);
            }
            if(IDwfsmask==-1)
            {
                IDwfsmask = create_2Dimage_ID("wfsmask", wfsxsize, wfsysize);
                for(ii=0; ii<wfssize; ii++)
                    data.image[IDwfsmask].array.F[ii] = 1.0;
            }



            for(mblock=0; mblock<NBmblock; mblock++)
            {
                printf("BLOCK %ld has %ld modes\n", mblock, MBLOCK_NBmode[mblock]);
                fflush(stdout);


                if(sprintf(imname, "fmodesWFS0_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                if(MBLOCK_NBmode[mblock]>0)
                {
                    long IDwfsMresp = create_3Dimage_ID(imname, wfsxsize, wfsysize, MBLOCK_NBmode[mblock]);

# ifdef _OPENMP
                    #pragma omp parallel for private(m,act,wfselem)
# endif
                    for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                    {
                        for(act=0; act<msizexy; act++)
                        {
                            for(wfselem=0; wfselem<wfssize; wfselem++)
                            {
                                data.image[IDwfsMresp].array.F[m*wfssize+wfselem] += data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+act] * data.image[IDzrespM].array.F[act*wfssize+wfselem];
                            }
                        }
                    }

                    if((IDRMMmodes!=-1)&&(IDRMMresp!=-1))
                    {
                        char fnameLOcoeff[200];
                        if(sprintf(fnameLOcoeff, "./mkmodestmp/LOcoeff_%02ld.txt", mblock) < 1)
                            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                        fpLOcoeff = fopen(fnameLOcoeff, "w");
                        if(fpLOcoeff == NULL)
                        {
                            printf("ERROR: cannot create file \"LOcoeff1.txt\"\n");
                            exit(0);
                        }



                        linfitsize = data.image[IDRMMmodes].md[0].size[2];
                        IDRMM_coeff = create_2Dimage_ID("linfitcoeff", linfitsize, 1);

                        ID_imfit = create_2Dimage_ID("imfitim", msizex, msizey);

                        IDcoeffmat = create_2Dimage_ID("imfitmat", linfitsize, data.image[ID].md[0].size[2]);

                        linfitreuse = 0;

                        IDwfstmp = create_2Dimage_ID("wfsimtmp", wfsxsize, wfsysize);

                        for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                        {
                            for(ii=0; ii<msizexy; ii++)
                                data.image[ID_imfit].array.F[ii] = data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii];

                            linopt_imtools_image_fitModes("imfitim", "RMMmodes", "dmmaskRM", 1.0e-2, "linfitcoeff", linfitreuse);
                            linfitreuse = 1;

                            for(jj=0; jj<linfitsize; jj++)
                                data.image[IDcoeffmat].array.F[m*linfitsize+jj] = data.image[IDRMM_coeff].array.F[jj];

                            // prevent large coefficients (noise propagation)


                            // construct linear fit result (DM)
                            IDtmp = create_2Dimage_ID("testrc", msizex, msizey);
                            for(jj=0; jj<linfitsize; jj++)
                                for(ii=0; ii<msizex*msizey; ii++)
                                    data.image[IDtmp].array.F[ii] += data.image[IDRMM_coeff].array.F[jj]*data.image[IDRMMmodes].array.F[jj*msizex*msizey+ii];

                            res = 0.0;
                            resn = 0.0;
                            for(ii=0; ii<msizex*msizey; ii++)
                            {
                                v0 = data.image[IDtmp].array.F[ii]-data.image[ID_imfit].array.F[ii];
                                vn = data.image[ID_imfit].array.F[ii];
                                res += v0*v0;
                                resn += vn*vn;
                            }
                            res /= resn;

                            res1 = 0.0;  // norm squared of linear vector
                            for(jj=0; jj<linfitsize; jj++)
                                res1 += data.image[IDRMM_coeff].array.F[jj]*data.image[IDRMM_coeff].array.F[jj];

                            delete_image_ID("testrc");

                            LOcoeff = 1.0;

                            LOcoeff = 1.0/(1.0+pow(10.0*res, 4.0));

                            if(res1>1.0)
                                LOcoeff *= 1.0/(1.0+pow((res1-1.0)*0.1, 2.0));


                            fprintf(fpLOcoeff, "%5ld   %20g  %20g   ->  %f\n", m, res, res1, LOcoeff);
                            // printf("%5ld   %20g  %20g   ->  %f\n", m, res, res1, LOcoeff);

                            if(LOcoeff>0.01)
                            {
                                // construct linear fit (WFS space)
                                for(wfselem=0; wfselem<wfssize; wfselem++)
                                    data.image[IDwfstmp].array.F[wfselem] = 0.0;
                                for(jj=0; jj<linfitsize; jj++)
                                    for(wfselem=0; wfselem<wfssize; wfselem++)
                                        data.image[IDwfstmp].array.F[wfselem] += data.image[IDRMM_coeff].array.F[jj] * data.image[IDRMMresp].array.F[jj*wfssize+wfselem];

                                for(wfselem=0; wfselem<wfssize; wfselem++)
                                    data.image[IDwfsMresp].array.F[m*wfssize+wfselem] = LOcoeff*data.image[IDwfstmp].array.F[wfselem] + (1.0-LOcoeff)*data.image[IDwfsMresp].array.F[m*wfssize+wfselem];
                            }
                        }

                        delete_image_ID("linfitcoeff");
                        delete_image_ID("imfitim");

                        save_fits("imfitmat", "!imfitmat.fits");
                        delete_image_ID("imfitmat");

                        fclose(fpLOcoeff);
                    }


                    for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                        for(wfselem=0; wfselem<wfssize; wfselem++)
                            data.image[IDwfsMresp].array.F[m*wfssize+wfselem] *= data.image[IDwfsmask].array.F[wfselem];


                    if(sprintf(fname, "!./mkmodestmp/fmodesWFS0_%02ld.fits", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    save_fits(imname, fname);
                }
            }

            cnt = 0;
            for(mblock=0; mblock<NBmblock; mblock++)
                cnt += MBLOCK_NBmode[mblock];
            IDm = create_3Dimage_ID("fmodesWFS0all", wfsxsize, wfsysize, cnt);
            cnt = 0;


            for(mblock=0; mblock<NBmblock; mblock++)
            {
                if(sprintf(imname, "fmodesWFS0_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                long IDmwfs = image_ID(imname);
                for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                {
                    for(ii=0; ii<wfssize; ii++)
                        data.image[IDm].array.F[cnt*wfssize+ii] = data.image[IDmwfs].array.F[m*wfssize+ii];
                    cnt++;
                }
            }
            save_fits("fmodesWFS0all", "!./mkmodestmp/fmodesWFS0all.fits");





            // time : 02:00


            /// STEP 7: REMOVE WFS MODES THAT ARE CONTAINED IN PREVIOUS BLOCKS, AND ENFORCE WFS-SPACE ORTHOGONALITY BETWEEN BLOCKS
            /// Input: fmodesWFS0all (corresponding to fmodes2ball)
            /// Output -> fmodesWFS1all / fmodes3all

            IDSVDmask = create_2Dimage_ID("SVDmask", wfsxsize, wfsysize);
            for(ii=0; ii<wfssize; ii++)
                data.image[IDSVDmask].array.F[ii] = 1.0;
            IDSVDmodein = create_2Dimage_ID("SVDmodein", wfsxsize, wfsysize);

            mok = (int*) malloc(sizeof(int)*NBmm);
            for(m=0; m<NBmm; m++)
                mok[m] = 1;



            for(mblock=0; mblock<NBmblock; mblock++)
            {
                float *rmsarray;
                rmsarray = (float*) malloc(sizeof(float)*MBLOCK_NBmode[mblock]);
                for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                {
                    if(sprintf(imname, "fmodesWFS0_%02ld", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    long IDmwfs = image_ID(imname);
                    value1 = 0.0;
                    for(ii=0; ii<wfssize; ii++)
                        value1 += data.image[IDmwfs].array.F[m*wfssize+ii]*data.image[IDmwfs].array.F[m*wfssize+ii];
                    rmsarray[m] = sqrt(value1/wfssize);
                }

                for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                    mok[m] = 1;



                // REMOVE WFS MODES FROM PREVIOUS BLOCKS

                for(mblock0=0; mblock0<mblock; mblock0++)
                {

                    reuse = 0;
                    for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                    {
                        if(sprintf(imname, "fmodesWFS0_%02ld", mblock) < 1)
                            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                        long IDmwfs = image_ID(imname);

                        if(sprintf(imnameDM, "fmodes2b_%02ld", mblock) < 1)
                            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                        IDm = image_ID(imnameDM);


                        for(ii=0; ii<wfsxsize*wfsysize; ii++)
                            data.image[IDSVDmodein].array.F[ii] = data.image[IDmwfs].array.F[m*wfssize+ii];

                        if(sprintf(imname, "fmodesWFS0_%02ld", mblock0) < 1)
                            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                        if(sprintf(imnameDM, "fmodes2b_%02ld", mblock0) < 1)
                            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                        linopt_imtools_image_fitModes("SVDmodein", imname, "SVDmask", 1.0e-2, "modecoeff", reuse);
                        IDSVDcoeff = image_ID("modecoeff");
                        reuse = 1;
                        linopt_imtools_image_construct(imname, "modecoeff", "SVDmode1");
                        linopt_imtools_image_construct(imnameDM, "modecoeff", "SVDmode1DM");
                        IDSVDmode1 = image_ID("SVDmode1");

                        long IDSVDmode1DM = image_ID("SVDmode1DM");

                        delete_image_ID("modecoeff");

                        value1 = 0.0;
                        for(ii=0; ii<wfssize; ii++)
                        {
                            data.image[IDmwfs].array.F[m*wfssize+ii] -= data.image[IDSVDmode1].array.F[ii];
                            value1 += data.image[IDmwfs].array.F[m*wfssize+ii]*data.image[IDmwfs].array.F[m*wfssize+ii];
                        }
                        for(ii=0; ii<msizexy; ii++)
                            data.image[IDm].array.F[m*msizexy+ii] -= data.image[IDSVDmode1DM].array.F[ii];


                        delete_image_ID("SVDmode1");
                        delete_image_ID("SVDmode1DM");

                        rms = sqrt(value1/wfssize);

                        if(rms<rmsarray[m]*rmslim1)
                        {
                            mok[m] = 0;
                        }
                        printf("RMS RATIO  %3ld :   %12g\n", m, rms/rmsarray[m]);
                    }
                }

                cnt = 0;
                for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                    cnt += mok[m];
                printf("====== WFS BLOCK %ld : keeping %ld / %ld modes\n", mblock, cnt, MBLOCK_NBmode[mblock]);

                if(cnt>0)
                {
                    if(sprintf(imname, "fmodesWFS1_%02ld", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    if(sprintf(imnameDM, "fmodes3_%02ld", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");


                    long IDmwfs1 = create_3Dimage_ID(imname, wfsxsize, wfsysize, cnt);
                    long IDmdm1 = create_3Dimage_ID(imnameDM, msizex, msizey, cnt);
                    m1 = 0;

                    if(sprintf(imname, "fmodesWFS0_%02ld", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    long IDmwfs = image_ID(imname);

                    if(sprintf(imnameDM, "fmodes2b_%02ld", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    long IDmdm = image_ID(imnameDM);
                    if(IDmdm==-1)
                    {
                        printf("ERROR: image %s does not exist\n", imnameDM);
                        exit(0);
                    }
                    for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                    {
                        if(mok[m]==1)
                        {
                            printf("writing %ld / %ld  ->  %ld / %ld        \n", m, (long) data.image[IDmwfs].md[0].size[2], m1, (long) data.image[IDm].md[0].size[2]);

                            printf("max index IDmwfs1 %ld  = %ld / %ld    [ %ld %ld %ld ]\n", (long) m1, (long) (m1*wfssize+wfssize-1), (long) (data.image[IDmwfs1].md[0].size[0]*data.image[IDmwfs1].md[0].size[1]*data.image[IDmwfs1].md[0].size[2]), (long) data.image[IDmwfs1].md[0].size[0], (long) data.image[IDmwfs1].md[0].size[1], (long) data.image[IDmwfs1].md[0].size[2]);
                            printf("max index IDmwfs  %ld  = %ld / %ld    [ %ld %ld %ld ]\n", (long) m, (long) (m*wfssize+wfssize-1), (long) (data.image[IDmwfs].md[0].size[0]*data.image[IDmwfs].md[0].size[1]*data.image[IDmwfs].md[0].size[2]), (long) data.image[IDmwfs].md[0].size[0], (long) data.image[IDmwfs].md[0].size[1], (long) data.image[IDmwfs].md[0].size[2]);

                            printf("max index IDmdm1  %ld  = %ld / %ld    [ %ld %ld %ld ]\n", (long) m1, (long) (m1*msizexy+msizexy-1), (long) (data.image[IDmdm1].md[0].size[0]*data.image[IDmdm1].md[0].size[1]*data.image[IDmdm1].md[0].size[2]), (long) data.image[IDmdm1].md[0].size[0], (long) data.image[IDmdm1].md[0].size[1], (long) data.image[IDmdm1].md[0].size[2]);
                            printf("max index IDmdm   %ld  = %ld / %ld    [ %ld %ld %ld ]\n", (long) m, (long) (m*msizexy+msizexy-1), (long) (data.image[IDmdm].md[0].size[0]*data.image[IDmdm].md[0].size[1]*data.image[IDmdm].md[0].size[2]), (long) data.image[IDmdm].md[0].size[0], (long) data.image[IDmdm].md[0].size[1], (long) data.image[IDmdm].md[0].size[2]);


                            fflush(stdout);//TEST
                            for(ii=0; ii<wfssize; ii++)
                                data.image[IDmwfs1].array.F[m1*wfssize+ii] = data.image[IDmwfs].array.F[m*wfssize+ii];
                            for(ii=0; ii<msizexy; ii++)
                                data.image[IDmdm1].array.F[m1*msizexy+ii] = data.image[IDmdm].array.F[m*msizexy+ii];
                            value1 = 0.0;
                            m1++;
                        }
                        else
                        {
                            printf("Skipping %ld / %ld\n", m, (long) data.image[IDmwfs].md[0].size[2]);
                            fflush(stdout);
                        }
                    }
                    printf("STEP 0000\n");
                    fflush(stdout);//TEST

                    if(sprintf(imname1, "fmodesWFS1_%02ld", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    if(sprintf(fname1, "!./mkmodestmp/fmodesWFS1_%02ld.fits", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    printf("   saving   %s -> %s\n", imname1, fname1);
                    fflush(stdout);//TEST

                    save_fits(imname1, fname1);

                    printf("STEP 0001\n");
                    fflush(stdout);//TEST

                    if(sprintf(imname1, "fmodes3_%02ld", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    if(sprintf(fname1, "!./mkmodestmp/fmodes3_%02ld.fits", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    save_fits(imname1, fname1);
                    MBLOCK_ID[mblock] = IDmdm1;
                    printf("STEP 0002\n");
                    fflush(stdout);//TEST
                }
                else
                {
                    printf("ERROR: keeping no mode in block !!!\n");
                    exit(0);
                }
                printf("STEP 0010\n");
                fflush(stdout);//TEST

                MBLOCK_NBmode[mblock] = cnt;
                free(rmsarray);
            }
            delete_image_ID("SVDmask");
            delete_image_ID("SVDmodein");

            printf("STEP 0020\n");
            fflush(stdout);//TEST

            free(mok);


            // time : 04:34


            list_image_ID();
            cnt = 0;
            for(mblock=0; mblock<NBmblock; mblock++)
                cnt += MBLOCK_NBmode[mblock];
            IDm = create_3Dimage_ID("fmodesWFS1all", wfsxsize, wfsysize, cnt);
            long IDmdm1 = create_3Dimage_ID("fmodes3all", msizex, msizey, cnt);

            cnt = 0;
            for(mblock=0; mblock<NBmblock; mblock++)
            {
                if(MBLOCK_NBmode[mblock]>0)
                {
                    if(sprintf(imname, "fmodesWFS1_%02ld", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    long IDmwfs = image_ID(imname);

                    if(sprintf(imnameDM, "fmodes3_%02ld", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    long IDmdm = image_ID(imnameDM);

                    if(IDmwfs==-1)
                    {
                        printf("ERROR: image %s does not exit\n", imname);
                        exit(0);
                    }
                    for(m=0; m<MBLOCK_NBmode[mblock]; m++)
                    {
                        // printf("writing %ld / %ld  ->  %ld / %ld\n", m, data.image[IDmwfs].md[0].size[2], cnt, data.image[IDm].md[0].size[2]);
                        // fflush(stdout);
                        for(ii=0; ii<wfssize; ii++)
                            data.image[IDm].array.F[cnt*wfssize+ii] = data.image[IDmwfs].array.F[m*wfssize+ii];
                        for(ii=0; ii<msizexy; ii++)
                            data.image[IDmdm1].array.F[cnt*msizexy+ii] = data.image[IDmdm].array.F[m*msizexy+ii];
                        cnt++;
                    }
                }
            }
            save_fits("fmodesWFS1all", "!./mkmodestmp/fmodesWFS1all.fits");
            save_fits("fmodes3all", "!./mkmodestmp/fmodes3all.fits");


        }


        // time : 04:36

        if(BlockNB<0)
        {
            char command[1000];
            if(sprintf(command, "echo \"%ld\" > ./conf_staged/param_NBmodeblocks.txt", NBmblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(system(command) != 0)
                printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");
        }
        else
        {
            if((fp = fopen("./conf/param_NBmodeblocks.txt", "r"))==NULL)
            {
                printf("ERROR: cannot read file ./conf_staged/param_NBmodeblocks.txt\n");
                exit(0);
            }
            if(fscanf(fp, "%50ld", &NBmblock) != 1)
                printERROR(__FILE__, __func__, __LINE__, "Cannot read parameter from file");
            fclose(fp);
        }

        printf("%ld blocks\n", NBmblock);



        /// STEP 8: SVD WFS SPACE IN EACH BLOCK
        /// fmodesWFS1all, fmodes3 -> fmodesall

        // fmodesWFS1_##, fmodes3_## -> fmodes_##

        for(mblock=0; mblock<NBmblock; mblock++)
        {
            if(BlockNB>-1) // LOAD & VERIFY SIZE
            {
                if(sprintf(imname1, "fmodesWFS1_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                if(sprintf(fname1, "./mkmodestmp/fmodesWFS1_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                ID = load_fits(fname1, imname1, 1);
                wfsxsize = data.image[ID].md[0].size[0];
                wfsysize = data.image[ID].md[0].size[1];
                wfssize = wfsxsize*wfsysize;

                if(sprintf(imname1, "fmodes3_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                if(sprintf(fname1, "./mkmodestmp/fmodes3_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                ID = load_fits(fname1, imname1, 1);
                if((data.image[ID].md[0].size[0] != msizex) && (msizey != data.image[ID].md[0].size[0]))
                {
                    printf("ERROR: file dmmaskRM size (%ld %ld) does not match expected size (%ld %ld)\n", (long) data.image[IDmask].md[0].size[0], (long) data.image[IDmask].md[0].size[1], (long) msizex, (long) msizey);
                    exit(0);
                }
                msizexy = data.image[ID].md[0].size[0]*data.image[ID].md[0].size[1];
            }


            if((BlockNB<0)||(BlockNB==mblock))
            {
                char command[1000];
                if(sprintf(command, "echo \"%f\" > ./conf_staged/block%02ld_SVDlim.txt", SVDlim, mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                if(system(command) != 0)
                    printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");


                //if(MBLOCK_NBmode[mblock]>-1)
                //{

                if(sprintf(imname, "fmodesWFS1_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                long IDmwfs = image_ID(imname);
                if(IDmwfs==-1)
                {
                    printf("ERROR: image %s does not exit\n", imname);
                    exit(0);
                }

                if(sprintf(imnameDM, "fmodes3_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                long IDmdm = image_ID(imnameDM);
                if(IDmdm==-1)
                {
                    printf("ERROR: image %s does not exit\n", imnameDM);
                    exit(0);
                }

                if(sprintf(imnameDM1, "fmodes_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");


                linopt_compute_SVDdecomp(imname, "SVDout", "modecoeff"); // SVD
                IDSVDcoeff = image_ID("modecoeff");

                cnt = 0;

                if(sprintf(fnameSVDcoeff, "./mkmodestmp/SVDcoeff_%02ld.txt", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                fpcoeff = fopen(fnameSVDcoeff, "w");
                uint_fast16_t kk;
                for(kk=0; kk<data.image[IDSVDcoeff].md[0].size[0]; kk++)
                {
                    fprintf(fpcoeff, "%5ld   %12g   %12g  %5ld     %10.8f  %10.8f\n", kk, data.image[IDSVDcoeff].array.F[kk], data.image[IDSVDcoeff].array.F[0], cnt, data.image[IDSVDcoeff].array.F[kk]/data.image[IDSVDcoeff].array.F[0], SVDlim);
                    printf("==== %ld %12g %12g  %3ld\n", kk, data.image[IDSVDcoeff].array.F[kk], data.image[IDSVDcoeff].array.F[0], cnt);
                    if(data.image[IDSVDcoeff].array.F[kk]>SVDlim*data.image[IDSVDcoeff].array.F[0])
                        cnt++;
                }
                fclose(fpcoeff);


                long IDmdm1 = create_3Dimage_ID(imnameDM1, msizex, msizey, cnt);

                char imnameWFS1[200];
                if(sprintf(imnameWFS1, "fmodesWFS_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                long IDmwfs1 = create_3Dimage_ID(imnameWFS1, wfsxsize, wfsysize, cnt);
                long ID_VTmatrix = image_ID("SVD_VTm");


                for(kk=0; kk<cnt; kk++) /// eigen mode index
                {
                    long kk1;
                    for(kk1=0; kk1<data.image[IDSVDcoeff].md[0].size[0]; kk1++)
                    {
                        for(ii=0; ii<msizexy; ii++)
                            data.image[IDmdm1].array.F[kk*msizexy + ii] += data.image[ID_VTmatrix].array.F[kk1*data.image[IDSVDcoeff].md[0].size[0]+kk]*data.image[IDmdm].array.F[kk1*msizexy + ii];

                        for(ii=0; ii<wfssize; ii++)
                            data.image[IDmwfs1].array.F[kk*wfssize + ii] += data.image[ID_VTmatrix].array.F[kk1*data.image[IDSVDcoeff].md[0].size[0]+kk]*data.image[IDmwfs].array.F[kk1*wfssize + ii];
                    }

                    value1 = 0.0;
                    value1cnt = 0.0;
                    for(ii=0; ii<msizexy; ii++)
                    {
                        value1 += data.image[IDmdm1].array.F[kk*msizexy+ii]*data.image[IDmaskRM].array.F[ii];
                        value1cnt += data.image[IDmaskRM].array.F[ii];
                    }
                    for(ii=0; ii<msizexy; ii++)
                        data.image[IDmdm1].array.F[kk*msizexy+ii] -= value1/value1cnt;

                    value1 = 0.0;
                    for(ii=0; ii<msizexy; ii++)
                        value1 += data.image[IDmdm1].array.F[kk*msizexy+ii]*data.image[IDmdm1].array.F[kk*msizexy+ii]*data.image[IDmaskRM].array.F[ii];
                    rms = sqrt(value1/value1cnt);

                    for(ii=0; ii<msizexy; ii++)
                        data.image[IDmdm1].array.F[kk*msizexy+ii] /= rms;

                    for(ii=0; ii<wfssize; ii++)
                        data.image[IDmwfs1].array.F[kk*wfssize+ii] /= rms;


                    /*     value1 = 0.0;
                         for(ii=0; ii<msizexy; ii++)
                             value1 += data.image[IDmdm1].array.F[kk*msizexy + ii]*data.image[IDmdm1].array.F[kk*msizexy + ii];
                         rms = sqrt(value1/totm);
                         */


                    // for(ii=0; ii<msizexy; ii++)
                    //     data.image[IDmdm1].array.F[kk*msizexy + ii] /= rms;
                }
                delete_image_ID("SVDout");
                delete_image_ID("modecoeff");

                if(sprintf(fname, "!./mkmodestmp/fmodes_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                save_fits(imnameDM1, fname);

                if(sprintf(fname, "!./mkmodestmp/fmodesWFS_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                save_fits(imnameWFS1, fname);
                MBLOCK_ID[mblock] = IDmdm1;
                MBLOCK_IDwfs[mblock] = IDmwfs1;
                MBLOCK_NBmode[mblock] = cnt;
                //}
            }
            else
            {
                if(sprintf(fname, "./mkmodestmp/fmodes_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                if(sprintf(imnameDM1, "fmodes_%02ld", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                long IDmdm1 = load_fits(fname, imnameDM1, 1);
                MBLOCK_ID[mblock] = IDmdm1;
                //MBLOCK_IDwfs[mblock] = IDmwfs1;
                MBLOCK_NBmode[mblock] = data.image[IDmdm1].md[0].size[2];
            }
        }

        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
            cnt += MBLOCK_NBmode[mblock];
        IDm = create_3Dimage_ID("fmodesall", msizex, msizey, cnt);
        long IDwfs = create_3Dimage_ID("fmodesWFSall", wfsxsize, wfsysize, cnt);
        cnt = 0;
        long cnt1 = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
        {
            if(MBLOCK_NBmode[mblock]>0)
                cnt1++;

            for(m=0; m<MBLOCK_NBmode[mblock]; m++)
            {
                for(ii=0; ii<msizexy; ii++)
                    data.image[IDm].array.F[cnt*msizexy+ii] = data.image[MBLOCK_ID[mblock]].array.F[m*msizexy+ii];
                for(ii=0; ii<wfssize; ii++)
                    data.image[IDwfs].array.F[cnt*wfssize+ii] = data.image[MBLOCK_IDwfs[mblock]].array.F[m*wfssize+ii];


                cnt++;
            }
        }

        save_fits("fmodesall", "!./mkmodestmp/fmodesall.fits");
        save_fits("fmodesWFSall", "!./mkmodestmp/fmodesWFSall.fits");

        NBmblock = cnt1;







        //exit(0);//TEST










        /// WFS MODES, MODAL CONTROL MATRICES
        for(mblock=0; mblock<NBmblock; mblock++)
        {
            printf(".... BLOCK %ld has %ld modes\n", mblock, MBLOCK_NBmode[mblock]);
            fflush(stdout);

            if(sprintf(imname, "fmodesWFS_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            char imnameCM[200]; // modal control matrix
            if(sprintf(imnameCM, "cmat_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            char imnameCMc[200]; // zonal ("combined") control matrix
            if(sprintf(imnameCMc, "cmatc_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            char imnameCMcact[200]; // zonal control matrix masked
            if(sprintf(imnameCMcact, "cmatcact_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if((BlockNB<0)||(BlockNB==mblock))
            {
                if(MBLOCK_NBmode[mblock]>0)
                {



                    printf("-- COMPUTE MODAL CONTROL MATRICES\n");
                    fflush(stdout);

                    // COMPUTE MODAL CONTROL MATRICES
                    printf("COMPUTE CONTROL MATRIX\n");
                    float SVDlim1 = 0.01; // WFS filtering (ONLY USED FOR FULL SINGLE STEP INVERSION)
#ifdef HAVE_MAGMA
                    CUDACOMP_magma_compute_SVDpseudoInverse(imname, imnameCM, SVDlim1, 10000, "VTmat", 0);
#else
                    linopt_compute_SVDpseudoInverse(imname, imnameCM, SVDlim1, 10000, "VTmat");
#endif

                    delete_image_ID("VTmat");

                    if(sprintf(fname, "!./mkmodestmp/cmat_%02ld.fits", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    save_fits(imnameCM, fname);

                    printf("-- COMPUTE ZONAL CONTROL MATRIX FROM MODAL CONTROL MATRIX\n");
                    fflush(stdout);

                    // COMPUTE ZONAL CONTROL MATRIX FROM MODAL CONTROL MATRIX
                    sprintf(imname, "fmodes_%02ld", mblock);
                    compute_CombinedControlMatrix(imnameCM, imname, "wfsmask", "dmmask", imnameCMc, imnameCMcact);


                    if(sprintf(fname, "!./mkmodestmp/cmatc_%02ld.fits", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    save_fits(imnameCMc, fname);

                    if(sprintf(fname, "!./mkmodestmp/cmatcact_%02ld.fits", mblock) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    if(sprintf(imname1, "%s_00", imnameCMcact) < 1)
                        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                    save_fits(imname1, fname);

                    list_image_ID();
                }

            }
            else
            {
                printf("LOADING WFS MODES, MODAL CONTROL MATRICES: block %ld\n", mblock);
                fflush(stdout);

                //	list_image_ID();

                if(sprintf(fname, "./mkmodestmp/fmodesWFS_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                load_fits(fname, imname, 1);

                if(sprintf(fname, "./mkmodestmp/cmat_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                load_fits(fname, imnameCM, 1);

                if(sprintf(fname, "./mkmodestmp/cmatc_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                load_fits(fname, imnameCMc, 1);

                if(sprintf(fname, "./mkmodestmp/cmatcact_%02ld.fits", mblock) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                load_fits(fname, imnameCMcact, 1);
            }
        }

        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
            cnt += MBLOCK_NBmode[mblock];
        IDm = create_3Dimage_ID("fmodesWFSall", wfsxsize, wfsysize, cnt);
        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
        {
            char command[1000];
            if(sprintf(command, "echo \"%ld\" > ./conf_staged/block%02ld_NBmodes.txt", MBLOCK_NBmode[mblock], mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(system(command) != 0)
                printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

            if(sprintf(imname, "fmodesWFS_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            long IDmwfs = image_ID(imname);
            for(m=0; m<MBLOCK_NBmode[mblock]; m++)
            {
                for(ii=0; ii<wfssize; ii++)
                    data.image[IDm].array.F[cnt*wfssize+ii] = data.image[IDmwfs].array.F[m*wfssize+ii];
                cnt++;
            }
        }
        save_fits("fmodesWFSall", "!./mkmodestmp/fmodesWFSall.fits");


		fp = fopen("./mkmodestmp/NBmodes.txt", "w");
        fprintf(fp, "%ld\n", cnt);
        fclose(fp);		
		
		
        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
            cnt += MBLOCK_NBmode[mblock];
        long IDcmatall = create_3Dimage_ID("cmatall", wfsxsize, wfsysize, cnt);
        cnt = 0;
        for(mblock=0; mblock<NBmblock; mblock++)
        {
            if(sprintf(imname, "cmat_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            long IDcmat = image_ID(imname);
            for(m=0; m<MBLOCK_NBmode[mblock]; m++)
            {
                for(ii=0; ii<wfssize; ii++)
                    data.image[IDcmatall].array.F[cnt*wfssize+ii] = data.image[IDcmat].array.F[m*wfssize+ii];
                cnt++;
            }
        }
        save_fits("cmatall", "!./mkmodestmp/cmatall.fits");




        // COMPUTE OVERALL CONTROL MATRIX
        /*    int COMPUTE_FULL_CMAT = 0;
            if(COMPUTE_FULL_CMAT == 1)
            {
                printf("COMPUTE OVERALL CONTROL MATRIX\n");
                float SVDlim1 = 0.01; // WFS filtering (ONLY USED FOR FULL SINGLE STEP INVERSION)
                #ifdef HAVE_MAGMA
                    CUDACOMP_magma_compute_SVDpseudoInverse("fmodesWFSall", "cmat", SVDlim1, 100000, "VTmat", 0);
                #else
                    linopt_compute_SVDpseudoInverse("fmodesWFSall", "cmat", SVDlim1, 10000, "VTmat");
        		#endif

                delete_image_ID("VTmat");
                save_fits("cmat", "!./mkmodestmp/cmat.fits");

        	}

        		char command[1000];
                if(sprintf(command, "echo \"%ld\" > ./conf_staged/param_NBmodes.txt", cnt) < 1)
        			printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                if(system(command) != 0)
        			printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

            */
    }
    // time : 07:43


    return(ID);
}




/*** \brief Creates control matrices per block, using native modes
 */
long AOloopControl_mkModes_Simple(const char *IDin_name, long NBmblock, long Cmblock, float SVDlim)
{
    long IDin; // input WFS responses
    FILE *fp;
    long mblock;
    long *MBLOCK_NBmode;
    long *MBLOCK_blockstart;
    long *MBLOCK_blockend;
    char fname[500];

    char imname[500];
    char imname1[500];
    long ID;
    long wfsxsize, wfsysize;
    long wfssize;
    long ii, kk;
    char imnameCM[500];
    char imnameCMc[500];
    char imnameCMcact[500];
    long IDwfsmask;
    long IDdmmask;
    long IDmodes;
    long NBmodes;
    long cnt;
    long IDm;
    long m;
    long IDcmatall;
    char command[500];


    printf("Function AOloopControl_mkModes_Simple - Cmblock = %ld / %ld\n", Cmblock, NBmblock);
    fflush(stdout);

    if(system("mkdir -p mkmodestmp") != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");



    MBLOCK_NBmode = (long*) malloc(sizeof(long)*NBmblock);
    MBLOCK_blockstart = (long*) malloc(sizeof(long)*NBmblock);
    MBLOCK_blockend = (long*) malloc(sizeof(long)*NBmblock);


    IDin = image_ID(IDin_name);
    wfsxsize = data.image[IDin].md[0].size[0];
    wfsysize = data.image[IDin].md[0].size[1];
    wfssize = wfsxsize*wfsysize;
    NBmodes = data.image[IDin].md[0].size[2];

    // read block ends
    if(NBmblock==1)
    {
        MBLOCK_blockend[0] = NBmodes;

        if(sprintf(fname, "./conf_staged/param_block00end.txt") < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        fp = fopen(fname, "w");
        fprintf(fp, "%03ld\n", NBmodes);
        fclose(fp);
    }
    else
    {
        for(mblock=0; mblock<NBmblock; mblock++)
        {
            if(sprintf(fname, "./conf_staged/param_block%02ldend.txt", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            fp = fopen(fname, "r");
            if(fp==NULL)
            {
                printf("ERROR: File \"%s\" not found\n", fname);
                exit(0);
            }
            if(fscanf(fp, "%50ld", &MBLOCK_blockend[mblock]) != 1)
                printERROR(__FILE__, __func__, __LINE__, "Cannot read parameter from file");
            fclose(fp);

            printf("Block end %ld = %ld\n", mblock, MBLOCK_blockend[mblock]);
            fflush(stdout);
        }
    }

    MBLOCK_NBmode[0] = MBLOCK_blockend[0];
    MBLOCK_blockstart[0] = 0;
    for(mblock=1; mblock<NBmblock; mblock++)
    {
        MBLOCK_NBmode[mblock] = MBLOCK_blockend[mblock] - MBLOCK_blockend[mblock-1];
        MBLOCK_blockstart[mblock] =  MBLOCK_blockstart[mblock-1] + MBLOCK_NBmode[mblock-1];
    }




    IDmodes = create_3Dimage_ID("fmodesall", NBmodes, 1, NBmodes);
    for(kk=0; kk<NBmodes*NBmodes; kk++)
        data.image[IDmodes].array.F[kk] = 0.0;
    for(kk=0; kk<NBmodes; kk++)
        data.image[IDmodes].array.F[kk*NBmodes+kk] = 1.0;
    save_fits("fmodesall", "!./mkmodestmp/fmodesall.fits");

    for(mblock=0; mblock<NBmblock; mblock++)
    {
        printf("mblock %02ld  : %ld modes\n", mblock, MBLOCK_NBmode[mblock]);


        if( (Cmblock == mblock) || (Cmblock == -1) )
        {
            printf("Reconstructing block %ld\n", mblock);

            if(sprintf(command, "echo \"%f\" > ./conf_staged/block%02ld_SVDlim.txt", SVDlim, mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(system(command) != 0)
                printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");


            IDdmmask = create_2Dimage_ID("dmmask", NBmodes, 1);
            for(kk=0; kk<NBmodes; kk++)
                data.image[IDdmmask].array.F[kk] = 1.0;

            if(sprintf(imname, "fmodesWFS_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            ID = create_3Dimage_ID(imname, wfsxsize, wfsysize, MBLOCK_NBmode[mblock]);
            for(kk=0; kk<MBLOCK_NBmode[mblock]; kk++)
            {
                for(ii=0; ii<wfssize; ii++)
                    data.image[ID].array.F[kk*wfssize+ii] = data.image[IDin].array.F[(kk+MBLOCK_blockstart[mblock])*wfssize+ii];
            }

            if(sprintf(fname, "!./mkmodestmp/fmodesWFS_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits(imname, fname);


            if(sprintf(imnameCM, "cmat_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(sprintf(imnameCMc, "cmatc_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(sprintf(imnameCMcact, "cmatcact_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            // COMPUTE MODAL CONTROL MATRICES
            printf("COMPUTE CONTROL MATRIX\n");
#ifdef HAVE_MAGMA
            CUDACOMP_magma_compute_SVDpseudoInverse(imname, imnameCM, SVDlim, 10000, "VTmat", 0);
#else
            linopt_compute_SVDpseudoInverse(imname, imnameCM, SVDlim, 10000, "VTmat");
#endif

            delete_image_ID("VTmat");

            if(sprintf(fname, "!./mkmodestmp/cmat_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits(imnameCM, fname);

            printf("-- COMPUTE ZONAL CONTROL MATRIX FROM MODAL CONTROL MATRIX\n");
            fflush(stdout);

            // COMPUTE ZONAL CONTROL MATRIX FROM MODAL CONTROL MATRIX
            if(sprintf(imname, "fmodes_%02ld", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            IDmodes = create_3Dimage_ID(imname, NBmodes, 1, MBLOCK_NBmode[mblock]);
            list_image_ID();
            for(kk=0; kk<MBLOCK_NBmode[mblock]; kk++)
            {
                for(ii=0; ii<NBmodes; ii++)
                    data.image[IDmodes].array.F[kk*NBmodes+ii] = 0.0;
                data.image[IDmodes].array.F[kk*NBmodes+(kk+MBLOCK_blockstart[mblock])] = 1.0;
            }

            if(sprintf(fname, "!./mkmodestmp/fmodes_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits(imname, fname);


            compute_CombinedControlMatrix(imnameCM, imname, "wfsmask", "dmmask", imnameCMc, imnameCMcact);
            delete_image_ID("dmmask");

            if(sprintf(fname, "!./mkmodestmp/cmatc_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits(imnameCMc, fname);

            if(sprintf(fname, "!./mkmodestmp/cmatcact_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(sprintf(imname1, "%s_00", imnameCMcact) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            save_fits(imname1, fname);
        }

        else
        {
            printf("LOADING WFS MODES, MODAL CONTROL MATRICES: block %ld\n", mblock);
            fflush(stdout);

            if(sprintf(fname, "./mkmodestmp/fmodesWFS_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            load_fits(fname, imname, 1);

            if(sprintf(fname, "./mkmodestmp/cmat_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            load_fits(fname, imnameCM, 1);

            if(sprintf(fname, "./mkmodestmp/cmatc_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            load_fits(fname, imnameCMc, 1);

            if(sprintf(fname, "./mkmodestmp/cmatcact_%02ld.fits", mblock) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            load_fits(fname, imnameCMcact, 1);
        }
    }


    cnt = 0;
    for(mblock=0; mblock<NBmblock; mblock++)
        cnt += MBLOCK_NBmode[mblock];
    IDm = create_3Dimage_ID("fmodesWFSall", wfsxsize, wfsysize, cnt);
    cnt = 0;
    for(mblock=0; mblock<NBmblock; mblock++)
    {
        if(sprintf(command, "echo \"%ld\" > ./conf_staged/block%02ld_NBmodes.txt", MBLOCK_NBmode[mblock], mblock) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        if(system(command) != 0)
            printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

        if(sprintf(imname, "fmodesWFS_%02ld", mblock) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        long IDmwfs = image_ID(imname);
        for(m=0; m<MBLOCK_NBmode[mblock]; m++)
        {
            for(ii=0; ii<wfssize; ii++)
                data.image[IDm].array.F[cnt*wfssize+ii] = data.image[IDmwfs].array.F[m*wfssize+ii];
            cnt++;
        }
    }
    save_fits("fmodesWFSall", "!./mkmodestmp/fmodesWFSall.fits");


    cnt = 0;
    for(mblock=0; mblock<NBmblock; mblock++)
        cnt += MBLOCK_NBmode[mblock];
    IDcmatall = create_3Dimage_ID("cmatall", wfsxsize, wfsysize, cnt);
    cnt = 0;
    for(mblock=0; mblock<NBmblock; mblock++)
    {
        if(sprintf(imname, "cmat_%02ld", mblock) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        long IDcmat = image_ID(imname);
        for(m=0; m<MBLOCK_NBmode[mblock]; m++)
        {
            for(ii=0; ii<wfssize; ii++)
                data.image[IDcmatall].array.F[cnt*wfssize+ii] = data.image[IDcmat].array.F[m*wfssize+ii];
            cnt++;
        }
    }
    save_fits("cmatall", "!./mkmodestmp/cmatall.fits");





    free(MBLOCK_NBmode);
    free(MBLOCK_blockstart);
    free(MBLOCK_blockend);


    return(IDin);
}



/** \brief Computes control matrix using SVD
 *
 *        Conventions:
 * 				m: number of actuators (= NB_MODES);
 * 				n: number of sensors  (= # of pixels)
 *	works even for m != n
 *
 *
 *
 */

int_fast8_t compute_ControlMatrix(long loop, long NB_MODE_REMOVED, const char *ID_Rmatrix_name, const char *ID_Cmatrix_name, const char *ID_VTmatrix_name, double Beta, long NB_MODE_REMOVED_STEP, float eigenvlim)
{
    FILE *fp;
    long ii1, jj1, k, ii;
    gsl_matrix *matrix_D; /* this is the response matrix */
    gsl_matrix *matrix_Ds; /* this is the pseudo inverse of D */
    gsl_matrix *matrix_Dtra;
    gsl_matrix *matrix_DtraD;
    gsl_matrix *matrix_DtraDinv;
    gsl_matrix *matrix_DtraD_evec;
    gsl_matrix *matrix1;
    gsl_matrix *matrix2;
    gsl_vector *matrix_DtraD_eval;
    gsl_eigen_symmv_workspace *w;

    gsl_matrix *matrix_save;

    long m;
    long n;
    long ID_Rmatrix, ID_Cmatrix, ID_VTmatrix;
    uint32_t *arraysizetmp;

    long IDmodes;

    long IDeigenmodesResp;
    long kk, kk1;
    long ID_RMmask;

    double *CPAcoeff; /// gain applied to modes to enhance low orders in SVD

    char fname[200];
    long NB_MR;  /// number of modes removed

    long NB_MODE_REMOVED1;
    float eigenvmin=0.0;
    long NBMODES_REMOVED_EIGENVLIM = 0;


    long MB_MR_start;
    long MB_MR_end;
    long MB_MR_step;

    int ret;
    char command[200];


    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);


    arraysizetmp = (uint32_t*) malloc(sizeof(uint32_t)*3);


    ID_Rmatrix = image_ID(ID_Rmatrix_name);


    n = data.image[ID_Rmatrix].md[0].size[0]*data.image[ID_Rmatrix].md[0].size[1]; //AOconf[loop].NBDMmodes;
    m = data.image[ID_Rmatrix].md[0].size[2]; //AOconf[loop].sizeWFS;


    ID_RMmask = image_ID("RMmask");
    if(ID_RMmask!=-1) // apply mask to response matrix
    {
        for(kk=0; kk<m; kk++)
        {
            for(ii=0; ii<n; ii++)
                data.image[ID_Rmatrix].array.F[kk*n+ii] *= data.image[ID_RMmask].array.F[ii];
        }
    }



    /** in this procedure, m=number of actuators/modes, n=number of WFS elements */
    //  long m = smao[0].NBmode;
    // long n = smao[0].NBwfselem;

    printf("m = %ld actuators (modes), n = %ld sensors\n", m, n);
    fflush(stdout);

    NB_MODE_REMOVED1 = m-1;

    matrix_DtraD_eval = gsl_vector_alloc (m);
    matrix_D = gsl_matrix_alloc (n,m);
    matrix_Ds = gsl_matrix_alloc (m,n);
    matrix_Dtra = gsl_matrix_alloc (m,n);
    matrix_DtraD = gsl_matrix_alloc (m,m);
    matrix_DtraDinv = gsl_matrix_alloc (m,m);
    matrix_DtraD_evec = gsl_matrix_alloc (m,m);


    CPAcoeff = (double*) malloc(sizeof(double)*m);

    if(Beta>0.000001)
    {
        long ID = load_fits("modesfreqcpa.fits", "modesfreqcpa", 1);
        if(ID==-1)
        {
            for(k=0; k<m; k++)
                CPAcoeff[k] = 1.0;
        }
        else
        {
            for(k=0; k<m; k++)
            {
                CPAcoeff[k] =  exp(-data.image[ID].array.F[k]*Beta);
                printf("%5ld %5.3f %g\n", k, data.image[ID].array.F[k], CPAcoeff[k]);
            }
        }
    }
    else
    {
        for(k=0; k<m; k++)
            CPAcoeff[k] = 1.0;
    }


    /* write matrix_D */
    for(k=0; k<m; k++)
    {
        for(ii=0; ii<n; ii++)
            gsl_matrix_set (matrix_D, ii, k, data.image[ID_Rmatrix].array.F[k*n+ii]*CPAcoeff[k]);
    }
    /* compute DtraD */
    gsl_blas_dgemm (CblasTrans, CblasNoTrans, 1.0, matrix_D, matrix_D, 0.0, matrix_DtraD);


    /* compute the inverse of DtraD */

    /* first, compute the eigenvalues and eigenvectors */
    w =   gsl_eigen_symmv_alloc (m);
    matrix_save = gsl_matrix_alloc (m,m);
    gsl_matrix_memcpy(matrix_save, matrix_DtraD);
    gsl_eigen_symmv (matrix_save, matrix_DtraD_eval, matrix_DtraD_evec, w);
    gsl_matrix_free(matrix_save);
    gsl_eigen_symmv_free(w);
    gsl_eigen_symmv_sort (matrix_DtraD_eval, matrix_DtraD_evec, GSL_EIGEN_SORT_ABS_DESC);

    printf("Eigenvalues\n");
    fflush(stdout);

    // Write eigenvalues
    if((fp=fopen("eigenv.dat","w"))==NULL)
    {
        printf("ERROR: cannot create file \"eigenv.dat\"\n");
        exit(0);
    }
    for(k=0; k<m; k++)
        fprintf(fp,"%ld %g\n", k, gsl_vector_get(matrix_DtraD_eval,k));
    fclose(fp);

    eigenvmin = eigenvlim*gsl_vector_get(matrix_DtraD_eval,0);

    NBMODES_REMOVED_EIGENVLIM = 0;
    for(k=0; k<m; k++)
    {
        printf("Mode %ld eigenvalue = %g\n", k, gsl_vector_get(matrix_DtraD_eval,k));
        if(gsl_vector_get(matrix_DtraD_eval,k) < eigenvmin)
            NBMODES_REMOVED_EIGENVLIM++;
    }




    /** Write rotation matrix to go from DM modes to eigenmodes */
    arraysizetmp[0] = m;
    arraysizetmp[1] = m;
    ID_VTmatrix = create_image_ID(ID_VTmatrix_name, 2, arraysizetmp, _DATATYPE_FLOAT, 0, 0);
    for(ii=0; ii<m; ii++) // modes
        for(k=0; k<m; k++) // modes
            data.image[ID_VTmatrix].array.F[k*m+ii] = (float) gsl_matrix_get( matrix_DtraD_evec, k, ii);


    /// Compute eigenmodes responses
    IDeigenmodesResp = create_3Dimage_ID("eigenmodesrespM", data.image[ID_Rmatrix].md[0].size[0], data.image[ID_Rmatrix].md[0].size[1], data.image[ID_Rmatrix].md[0].size[2]);
    printf("Computing eigenmode responses .... \n");
    for(kk=0; kk<m; kk++) /// eigen mode index
    {
        printf("\r eigenmode %4ld / %4ld   ", kk, m);
        fflush(stdout);
        for(kk1=0; kk1<m; kk1++)
        {
            for(ii=0; ii<n; ii++)
                data.image[IDeigenmodesResp].array.F[kk*n + ii] += data.image[ID_VTmatrix].array.F[kk1*m+kk]*data.image[ID_Rmatrix].array.F[kk1*n + ii];
        }
    }
    if(sprintf(fname, "!eigenmodesrespM_%4.2f.fits", Beta) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    save_fits("eigenmodesrespM", fname);
    printf("\n");


    /// if modesM exists, compute eigenmodes using rotation matrix
    IDmodes = image_ID("modesM");
    if(IDmodes!=-1)
    {
        uint32_t xsize_modes, ysize_modes, zsize_modes;
        xsize_modes = data.image[IDmodes].md[0].size[0];
        ysize_modes = data.image[IDmodes].md[0].size[1];
        zsize_modes = data.image[IDmodes].md[0].size[2];
        if(zsize_modes != m)
            printf("ERROR: zsize (%ld) of modesM does not match expected size (%ld)\n", (long) zsize_modes, (long) m);
        else
        {
            long IDeigenmodes = create_3Dimage_ID("eigenmodesM", xsize_modes, ysize_modes, m);
            printf("Computing eigenmodes .... \n");
            for(kk=0; kk<m; kk++) /// eigen mode index
            {
                printf("\r eigenmode %4ld / %4ld   ", kk, m);
                fflush(stdout);
                for(kk1=0; kk1<m; kk1++)
                {
                    for(ii=0; ii<xsize_modes*ysize_modes; ii++)
                        data.image[IDeigenmodes].array.F[kk*xsize_modes*ysize_modes + ii] += data.image[ID_VTmatrix].array.F[kk1*m+kk]*data.image[IDmodes].array.F[kk1*xsize_modes*ysize_modes + ii];
                }
            }
            printf("\n");
        }

        if(sprintf(fname, "!eigenmodesM_%4.2f.fits", Beta) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        save_fits("eigenmodesM", fname);
    }




    /// second, build the "inverse" of the diagonal matrix of eigenvalues (matrix1)
    matrix1 = gsl_matrix_alloc (m, m);
    matrix2 = gsl_matrix_alloc (m, m);
    arraysizetmp[0] = AOconf[loop].sizexWFS;
    arraysizetmp[1] = AOconf[loop].sizeyWFS;
    arraysizetmp[2] = m;
    ID_Cmatrix = create_image_ID(ID_Cmatrix_name, 3, arraysizetmp, _DATATYPE_FLOAT, 0, 0);

    printf("COMPUTING CMAT .... \n");


    if(NB_MODE_REMOVED_STEP==0)
    {
        MB_MR_start = NBMODES_REMOVED_EIGENVLIM;
        MB_MR_end = NBMODES_REMOVED_EIGENVLIM+1;
        MB_MR_step = 10;
    }
    else
    {
        MB_MR_start = 0;
        MB_MR_end = NB_MODE_REMOVED1;
        MB_MR_step = NB_MODE_REMOVED_STEP;
    }

    for(NB_MR=MB_MR_start; NB_MR<MB_MR_end; NB_MR+=MB_MR_step)
    {
        printf("\r Number of modes removed : %5ld / %5ld  (step %ld)  ", NB_MR, NB_MODE_REMOVED1, NB_MODE_REMOVED_STEP);
        fflush(stdout);
        for(ii1=0; ii1<m; ii1++)
            for(jj1=0; jj1<m; jj1++)
            {
                if(ii1==jj1)
                {
                    if((m-ii1-1)<NB_MR)
                        gsl_matrix_set(matrix1, ii1, jj1, 0.0);
                    else
                        gsl_matrix_set(matrix1, ii1, jj1, 1.0/gsl_vector_get(matrix_DtraD_eval,ii1));
                }
                else
                    gsl_matrix_set(matrix1, ii1, jj1, 0.0);
            }


        /* third, compute the "inverse" of DtraD */
        gsl_blas_dgemm (CblasNoTrans, CblasNoTrans, 1.0, matrix_DtraD_evec, matrix1, 0.0, matrix2);
        gsl_blas_dgemm (CblasNoTrans, CblasTrans, 1.0, matrix2, matrix_DtraD_evec, 0.0, matrix_DtraDinv);
        gsl_blas_dgemm (CblasNoTrans, CblasTrans, 1.0, matrix_DtraDinv, matrix_D, 0.0, matrix_Ds);

        /* write result */
        printf("write result to ID %ld   [%ld %ld]\n", ID_Cmatrix, n, m);
        fflush(stdout);

        for(ii=0; ii<n; ii++) // sensors
            for(k=0; k<m; k++) // actuator modes
                data.image[ID_Cmatrix].array.F[k*n+ii] = (float) gsl_matrix_get(matrix_Ds, k, ii)*CPAcoeff[k];


        if(NB_MODE_REMOVED_STEP==0)
        {
            save_fits(ID_Cmatrix_name, "!cmat.fits");

            if(sprintf(command, "echo \"%ld\" > ./cmat.NB_MODES_RM.txt", NBMODES_REMOVED_EIGENVLIM) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(system(command) != 0)
                printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

            if(sprintf(command, "echo \"%ld\" > ./cmat.NB_MODES.txt",  m) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            if(system(command) != 0)
                printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");
        }
        else
        {
            if(sprintf(fname, "!cmat_%4.2f_%02ld.fits", Beta, NB_MR) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            printf("  SAVING -> %s\n", fname);
            fflush(stdout);
            save_fits(ID_Cmatrix_name, fname);
        }
    }

    printf("\n\n");

    gsl_matrix_free(matrix1);
    gsl_matrix_free(matrix2);

    gsl_vector_free(matrix_DtraD_eval);
    gsl_matrix_free(matrix_D);
    gsl_matrix_free(matrix_Ds);
    gsl_matrix_free(matrix_Dtra);
    gsl_matrix_free(matrix_DtraD);
    gsl_matrix_free(matrix_DtraDinv);
    gsl_matrix_free(matrix_DtraD_evec);

    free(arraysizetmp);

    free(CPAcoeff);



    return(ID_Cmatrix);
}





//
// computes combined control matrix
//
//

long compute_CombinedControlMatrix(const char *IDcmat_name, const char *IDmodes_name, const char* IDwfsmask_name, const char *IDdmmask_name, const char *IDcmatc_name, const char *IDcmatc_active_name)
{
    // long ID;
    struct timespec t1;
    struct timespec t2;
    struct timespec tdiff;
    double tdiffv;

    float *matrix_cmp;
    long wfselem, act, mode;
    //    long n_sizeDM, n_NBDMmodes, n_sizeWFS;
    float *matrix_Mc, *matrix_DMmodes;
    long act_active, wfselem_active;

    long IDwfsmask, IDdmmask;
    long sizexWFS, sizeyWFS, sizeWFS, sizeWFS_active[100];
    long ii, ii1;
    long sizexDM, sizeyDM;
    long sizeDM_active;
    uint32_t *sizearray;
    long IDcmat;
    long IDcmatc;
    long IDmodes;
    long NBDMmodes;
    long sizeDM;
    long IDcmatc_active[100];
    char name[200];
    char imname[200];
    int slice;


    printf("COMPUTING COMBINED CONTROL MATRIX .... \n");
    fflush(stdout);

    clock_gettime(CLOCK_REALTIME, &t1);


    // initialize size of arrays
    IDwfsmask = image_ID(IDwfsmask_name);
    sizexWFS = data.image[IDwfsmask].md[0].size[0];
    sizeyWFS = data.image[IDwfsmask].md[0].size[1];
    sizeWFS = sizexWFS*sizeyWFS;

    printf("IDwfsmask = %ld\n", IDwfsmask);
    fflush(stdout);

    IDdmmask = image_ID(IDdmmask_name);
    sizexDM = data.image[IDdmmask].md[0].size[0];
    sizeyDM = data.image[IDdmmask].md[0].size[1];
    sizeDM = sizexDM*sizeyDM;

    printf("IDdmmask = %ld\n", IDdmmask);
    fflush(stdout);

    IDmodes = image_ID(IDmodes_name);

    printf("IDmodes = %ld\n", IDmodes);
    fflush(stdout);

    NBDMmodes = data.image[IDmodes].md[0].size[2];



    // allocate array for combined matrix
    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*3);
    sizearray[0] = sizexWFS;
    sizearray[1] = sizeyWFS;
    sizearray[2] = sizeDM;

    printf("Creating 3D image : %ld %ld %ld\n", sizexWFS, sizeyWFS, sizeDM);
    fflush(stdout);
    IDcmatc = create_image_ID(IDcmatc_name, 3, sizearray, _DATATYPE_FLOAT, 0, 0);
    free(sizearray);


    printf("PREPARE MATRIX MULT\n");
    fflush(stdout);



    // init matrix_Mc
    matrix_Mc = (float*) malloc(sizeof(float)*sizeWFS*sizeDM);
    memcpy(matrix_Mc, data.image[IDcmatc].array.F, sizeof(float)*sizeWFS*sizeDM);

    // copy modal control matrix to matrix_cmp
    IDcmat = image_ID(IDcmat_name);
    matrix_cmp = (float*) malloc(sizeof(float)*sizeWFS*NBDMmodes);
    memcpy(matrix_cmp, data.image[IDcmat].array.F, sizeof(float)*sizeWFS*NBDMmodes);

    // copy modes matrix to matrix_DMmodes
    matrix_DMmodes = (float*) malloc(sizeof(float)*NBDMmodes*sizeDM);
    memcpy(matrix_DMmodes, data.image[IDmodes].array.F, sizeof(float)*NBDMmodes*sizeDM);

    printf("START MATRIX MULT\n");
    fflush(stdout);



    // computing combine matrix (full size)
    //# ifdef _OPENMP
    //   #pragma omp parallel shared(matrix_Mc, matrix_cmp, matrix_DMmodes ,chunk) private( mode, act, wfselem)
    //  {
    //        #pragma omp for schedule (static)
    //# endif
    for(mode=0; mode<NBDMmodes; mode++)
    {
        for(act=0; act<sizeDM; act++)
        {
            for(wfselem=0; wfselem<sizeWFS; wfselem++)
                matrix_Mc[act*sizeWFS+wfselem] += matrix_cmp[mode*sizeWFS+wfselem]*matrix_DMmodes[mode*sizeDM+act];
        }
    }
    //# ifdef _OPENMP
    //    }
    //# endif
    memcpy(data.image[IDcmatc].array.F, matrix_Mc, sizeof(float)*sizeWFS*sizeDM);
    free(matrix_cmp);

    printf("REDUCE MATRIX SIZE\n");
    fflush(stdout);



    WFS_active_map = (int*) malloc(sizeof(int)*sizeWFS*PIXSTREAM_NBSLICES);
    for(slice=0; slice<PIXSTREAM_NBSLICES; slice++)
    {
        ii1 = 0;
        for(ii=0; ii<sizeWFS; ii++)
            if(data.image[IDwfsmask].array.F[ii]>0.1)
            {
                if(slice==0)
                {
                    WFS_active_map[ii1] = ii;
                    ii1++;
                }
                else
                {
                    if(data.image[aoconfID_pixstream_wfspixindex].array.UI16[ii]==slice+1)
                    {
                        WFS_active_map[slice*sizeWFS+ii1] = ii;
                        ii1++;
                    }
                }
            }
        sizeWFS_active[slice] = ii1;

        if(sprintf(imname, "aol%ld_imWFS2active_%02d", LOOPNUMBER, slice) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        /* CAN CRASH
                sizearray = (long*) malloc(sizeof(long)*2);
                sizearray[0] =  sizeWFS_active[slice];
                sizearray[1] =  1;
                aoconfID_imWFS2_active[slice] = create_image_ID(imname, 2, sizearray, FLOAT, 1, 0);
        		free(sizearray);
        */
    }



    DM_active_map = (int*) malloc(sizeof(int)*sizeDM);
    ii1 = 0;
    for(ii=0; ii<sizeDM; ii++)
        if(data.image[IDdmmask].array.F[ii]>0.1)
        {
            DM_active_map[ii1] = ii;
            ii1++;
        }
    sizeDM_active = ii1;
    //   aoconfID_meas_act_active = create_2Dimage_ID("meas_act_active", sizeDM_active, 1);

    /* CAN CRASH
        sizearray = (long*) malloc(sizeof(long)*2);
        sizearray[0] = sizeDM_active;
        sizearray[1] = 1;
        sprintf(name, "aol%ld_meas_act_active", LOOPNUMBER);
        aoconfID_meas_act_active = create_image_ID(name, 2, sizearray, FLOAT, 1, 0);
       free(sizearray);
    */





    // reduce matrix size to active elements
    for(slice=0; slice<PIXSTREAM_NBSLICES; slice++)
    {
        if(sprintf(imname, "%s_%02d", IDcmatc_active_name, slice) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        IDcmatc_active[slice] = create_2Dimage_ID(imname, sizeWFS_active[slice], sizeDM_active);
        for(act_active=0; act_active<sizeDM_active; act_active++)
        {
            for(wfselem_active=0; wfselem_active<sizeWFS_active[slice]; wfselem_active++)
            {
                act = DM_active_map[act_active];
                wfselem = WFS_active_map[slice*sizeWFS+wfselem_active];
                data.image[IDcmatc_active[slice]].array.F[act_active*sizeWFS_active[slice]+wfselem_active] = matrix_Mc[act*sizeWFS+wfselem];
            }
        }
        printf("PIXEL SLICE %d     Keeping only active pixels / actuators : %ld x %ld   ->   %ld x %ld\n", slice, sizeWFS, sizeDM, sizeWFS_active[slice], sizeDM_active);


    }

    free(matrix_Mc);
    free(matrix_DMmodes);



    clock_gettime(CLOCK_REALTIME, &t2);
    tdiff = info_time_diff(t1, t2);
    tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
    printf("\n");
    printf("TIME TO COMPUTE MATRIX = %f sec\n", tdiffv);

    return(0);
}






long AOloopControl_loadCM(long loop, const char *CMfname)
{
    long ID = -1;



    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(0);

    if( (ID = load_fits(CMfname, "tmpcontrM", 1)) != -1 )
    {

        // check size is OK
        int vOK = 1;


        if(data.image[ID].md[0].naxis!=3)
        {
            printf("Control matrix has wrong dimension\n");
            vOK = 0;
        }
        if(data.image[ID].md[0].atype!=_DATATYPE_FLOAT)
        {
            printf("Control matrix has wrong type\n");
            vOK = 0;
        }
        if(vOK==1)
        {
            if(data.image[ID].md[0].size[0]!=AOconf[loop].sizexWFS)
            {
                printf("Control matrix has wrong x size : is %ld, should be %ld\n", (long) data.image[ID].md[0].size[0], (long) AOconf[loop].sizexWFS);
                vOK = 0;
            }
            if(data.image[ID].md[0].size[1]!=AOconf[loop].sizeyWFS)
            {
                printf("Control matrix has wrong y size\n");
                vOK = 0;
            }
            if(data.image[ID].md[0].size[2]!=AOconf[loop].NBDMmodes)
            {
                printf("Control matrix has wrong z size\n");
                vOK = 0;
            }
        }


        if(vOK==1)
        {
            AOconf[loop].init_CM = 1;
            char name[200];
            if(sprintf(name, "ContrM_%ld", loop) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            ID = image_ID(name);
            if(ID==-1)
                ID = read_sharedmem_image(name);
            long ID0 = image_ID("tmpcontrM");
            data.image[ID].md[0].write  = 1;
            long ii;
            for(ii=0; ii<AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS*AOconf[loop].NBDMmodes; ii++)
                data.image[ID].array.F[ii] = data.image[ID0].array.F[ii];
            data.image[ID].md[0].write  = 0;
            data.image[ID].md[0].cnt0++;
        }
        delete_image_ID("tmpcontrM");
    }

    return(ID);
}






/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 6. REAL TIME COMPUTING ROUTINES                                          */
/* =============================================================================================== */
/* =============================================================================================== */





// zero point offset loop
//
// args:
//  DM offset channel (shared memory)
//  zonal resp matrix (shared memory)
//  nominal wfs reference without offset (shared memory)
//  wfs reference to be updated (shared memory)
//
// computation triggered on semaphore wait on semaphore #1 of DM offset
//
// will run until SIGUSR1 received
//
int_fast8_t AOloopControl_WFSzpupdate_loop(const char *IDzpdm_name, const char *IDzrespM_name, const char *IDwfszp_name)
{
    long IDzpdm, IDzrespM, IDwfszp;
    uint32_t dmxsize, dmysize, dmxysize;
    long wfsxsize, wfsysize, wfsxysize;
    long IDtmp;
    long elem, act;
    long zpcnt = 0;
    long zpcnt0;
    int semval;
    struct timespec t1;
    struct timespec t2;



    IDzpdm = image_ID(IDzpdm_name);

    if(data.image[IDzpdm].md[0].sem<2) // if semaphore #1 does not exist, create it
        COREMOD_MEMORY_image_set_createsem(IDzpdm_name, 2);


    IDzrespM = image_ID(IDzrespM_name);
    IDwfszp = image_ID(IDwfszp_name);


    // array sizes extracted from IDzpdm and IDwfsref

    dmxsize = data.image[IDzpdm].md[0].size[0];
    dmysize = data.image[IDzpdm].md[0].size[1];
    dmxysize = dmxsize*dmysize;
    wfsxsize = data.image[IDwfszp].md[0].size[0];
    wfsysize = data.image[IDwfszp].md[0].size[1];
    wfsxysize = wfsxsize*wfsysize;

    // VERIFY SIZES

    // verify zrespM
    if(data.image[IDzrespM].md[0].size[0]!=wfsxsize)
    {
        printf("ERROR: zrespM xsize %ld does not match wfsxsize %ld\n", (long) data.image[IDzrespM].md[0].size[0], (long) wfsxsize);
        exit(0);
    }
    if(data.image[IDzrespM].md[0].size[1]!=wfsysize)
    {
        printf("ERROR: zrespM ysize %ld does not match wfsysize %ld\n", (long) data.image[IDzrespM].md[0].size[1], (long) wfsysize);
        exit(0);
    }
    if(data.image[IDzrespM].md[0].size[2]!=dmxysize)
    {
        printf("ERROR: zrespM zsize %ld does not match wfsxysize %ld\n", (long) data.image[IDzrespM].md[0].size[1], (long) wfsxysize);
        exit(0);
    }



    IDtmp = create_2Dimage_ID("wfsrefoffset", wfsxsize, wfsysize);


    zpcnt0 = 0;

    if(data.image[IDzpdm].md[0].sem > 1) // drive semaphore #1 to zero
        while(sem_trywait(data.image[IDzpdm].semptr[1])==0) {}
    else
    {
        printf("ERROR: semaphore #1 missing from image %s\n", IDzpdm_name);
        exit(0);
    }

    while(data.signal_USR1==0)
    {
        memset(data.image[IDtmp].array.F, '\0', sizeof(float)*wfsxysize);

        while(zpcnt0 == data.image[IDzpdm].md[0].cnt0)
            usleep(10);

        zpcnt0 = data.image[IDzpdm].md[0].cnt0;

        // TO BE DONE
        //  sem_wait(data.image[IDzpdm].semptr[1]);


        printf("WFS zero point offset update  # %8ld       (%s -> %s)  ", zpcnt, data.image[IDzpdm].name, data.image[IDwfszp].name);
        fflush(stdout);


        clock_gettime(CLOCK_REALTIME, &t1);

# ifdef _OPENMP
        #pragma omp parallel for private(elem)
# endif
        for(act=0; act<dmxysize; act++)
            for(elem=0; elem<wfsxysize; elem++)
                data.image[IDtmp].array.F[elem] += data.image[IDzpdm].array.F[act]*data.image[IDzrespM].array.F[act*wfsxysize+elem];



        clock_gettime(CLOCK_REALTIME, &t2);
        tdiff = info_time_diff(t1, t2);
        tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;

        printf(" [ %10.3f ms]\n", 1e3*tdiffv);
        fflush(stdout);


        // copy results to IDwfszpo
        data.image[IDwfszp].md[0].write = 1;
        memcpy(data.image[IDwfszp].array.F, data.image[IDtmp].array.F, sizeof(float)*wfsxysize);
        COREMOD_MEMORY_image_set_sempost_byID(IDwfszp, -1);
        data.image[IDwfszp].md[0].cnt0 ++;
        data.image[IDwfszp].md[0].write = 0;

        //        sem_getvalue(data.image[IDwfszp].semptr[0], &semval);
        //        if(semval<SEMAPHORE_MAXVAL)
        //            COREMOD_MEMORY_image_set_sempost(IDwfszp_name, 0);
        zpcnt++;
    }

    return 0;
}




//
// Create zero point WFS channels
// watch semaphore 1 on output (IDwfsref_name) -> sum all channels to update WFS zero point
// runs in separate process from RT computation
//
//
//
int_fast8_t AOloopControl_WFSzeropoint_sum_update_loop(long loopnb, const char *ID_WFSzp_name, int NBzp, const char *IDwfsref0_name, const char *IDwfsref_name)
{
    long wfsxsize, wfsysize, wfsxysize;
    long IDwfsref, IDwfsref0;
    long *IDwfszparray;
    long cntsumold;
    int RT_priority = 95; //any number from 0-99
    struct sched_param schedpar;
    long nsecwait = 10000; // 10 us
    struct timespec semwaitts;
    long ch;
    long IDtmp;
    long ii;
    char name[200];
    int semval;



    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    if(seteuid(euid_called) != 0) //This goes up to maximum privileges
        printERROR(__FILE__, __func__, __LINE__, "seteuid() returns non-zero value");

    sched_setscheduler(0, SCHED_FIFO, &schedpar); //other option is SCHED_RR, might be faster

    if(seteuid(euid_real) != 0) //Go back to normal privileges
        printERROR(__FILE__, __func__, __LINE__, "seteuid() returns non-zero value");
#endif

    IDwfsref = image_ID(IDwfsref_name);
    wfsxsize = data.image[IDwfsref].md[0].size[0];
    wfsysize = data.image[IDwfsref].md[0].size[1];
    wfsxysize = wfsxsize*wfsysize;
    IDtmp = create_2Dimage_ID("wfsrefoffset", wfsxsize, wfsysize);
    IDwfsref0 = image_ID(IDwfsref0_name);

    if(data.image[IDwfsref].md[0].sem > 1) // drive semaphore #1 to zero
        while(sem_trywait(data.image[IDwfsref].semptr[1])==0) {}
    else
    {
        printf("ERROR: semaphore #1 missing from image %s\n", IDwfsref_name);
        exit(0);
    }

    IDwfszparray = (long*) malloc(sizeof(long)*NBzp);
    // create / read the zero point WFS channels
    for(ch=0; ch<NBzp; ch++)
    {
        if(sprintf(name, "%s%ld", ID_WFSzp_name, ch) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        AOloopControl_2Dloadcreate_shmim(name, "", wfsxsize, wfsysize);
        COREMOD_MEMORY_image_set_createsem(name, 10);
        IDwfszparray[ch] = image_ID(name);
    }


    cntsumold = 0;
    for(;;)
    {
        if (clock_gettime(CLOCK_REALTIME, &semwaitts) == -1) {
            perror("clock_gettime");
            exit(EXIT_FAILURE);
        }
        semwaitts.tv_nsec += nsecwait;
        if(semwaitts.tv_nsec >= 1000000000)
            semwaitts.tv_sec = semwaitts.tv_sec + 1;

        sem_timedwait(data.image[IDwfsref].semptr[1], &semwaitts);

        long cntsum = 0;
        for(ch=0; ch<NBzp; ch++)
            cntsum += data.image[IDwfszparray[ch]].md[0].cnt0;



        if(cntsum != cntsumold)
        {
            memcpy(data.image[IDtmp].array.F, data.image[IDwfsref0].array.F, sizeof(float)*wfsxysize);
            for(ch=0; ch<NBzp; ch++)
                for(ii=0; ii<wfsxysize; ii++)
                    data.image[IDtmp].array.F[ii] += data.image[IDwfszparray[ch]].array.F[ii];

            // copy results to IDwfsref
            data.image[IDwfsref].md[0].write = 1;
            memcpy(data.image[IDwfsref].array.F, data.image[IDtmp].array.F, sizeof(float)*wfsxysize);
            data.image[IDwfsref].md[0].cnt0 ++;
            data.image[IDwfsref].md[0].write = 0;

            sem_getvalue(data.image[IDwfsref].semptr[0], &semval);
            if(semval<SEMAPHORE_MAXVAL)
                COREMOD_MEMORY_image_set_sempost(IDwfsref_name, 0);

            cntsumold = cntsum;
        }
    }

    free(IDwfszparray);


    return(0);
}


///
/// main routine
///
int_fast8_t AOloopControl_run()
{
    FILE *fp;
    char fname[200];
    long loop;
    int vOK;
    long ii;
    long ID;
    long j, m;
    struct tm *uttime;
    time_t t;
    struct timespec *thetime = (struct timespec *)malloc(sizeof(struct timespec));
    char logfname[1000];
    char command[1000];
    int r;
    int RT_priority = 90; //any number from 0-99
    struct sched_param schedpar;
    double a;
    long cnttest;
    float tmpf1;


    struct timespec t1;
    struct timespec t2;
    struct timespec tdiff;
    int semval;


    /*    float tmpv, tmpv1, tmpv2;
        float range1 = 0.1; // limit single iteration motion
        float rangec = 0.3; // limit cumulative motion
      */

    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    // r = seteuid(euid_called); //This goes up to maximum privileges
    sched_setscheduler(0, SCHED_FIFO, &schedpar); //other option is SCHED_RR, might be faster
    // r = seteuid(euid_real);//Go back to normal privileges
#endif



    loop = LOOPNUMBER;



    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(0);



    printf("SETTING UP...\n");
    AOloopControl_loadconfigure(LOOPNUMBER, 1, 10);

	

    COMPUTE_GPU_SCALING = AOconf[loop].GPUall;



    // pixel streaming ?
    COMPUTE_PIXELSTREAMING = 1;

    if(COMPUTE_GPU_SCALING == 0)
        COMPUTE_PIXELSTREAMING = 0;


    printf("============ pixel streaming ? =============\n");
    fflush(stdout);

    if(COMPUTE_PIXELSTREAMING == 1)
        aoconfID_pixstream_wfspixindex = load_fits("pixstream_wfspixindex.fits", "pixstream", 1);

    if(aoconfID_pixstream_wfspixindex == -1)
        COMPUTE_PIXELSTREAMING = 0;
    else
    {
        printf("Testing data type\n");
        fflush(stdout);
        if(data.image[aoconfID_pixstream_wfspixindex].md[0].atype != _DATATYPE_UINT16)
            COMPUTE_PIXELSTREAMING = 0;
    }

    if(COMPUTE_PIXELSTREAMING == 1)
    {
        long xsize = data.image[aoconfID_pixstream_wfspixindex].md[0].size[0];
        long ysize = data.image[aoconfID_pixstream_wfspixindex].md[0].size[1];
        PIXSTREAM_NBSLICES = 0;
        for(ii=0; ii<xsize*ysize; ii++)
            if(data.image[aoconfID_pixstream_wfspixindex].array.UI16[ii] > PIXSTREAM_NBSLICES)
                PIXSTREAM_NBSLICES = data.image[aoconfID_pixstream_wfspixindex].array.UI16[ii];
        PIXSTREAM_NBSLICES++;
        printf("PIXEL STREAMING:   %d image slices\n", PIXSTREAM_NBSLICES);
    }



    printf("============ FORCE pixel streaming = 0\n");
    fflush(stdout);
    COMPUTE_PIXELSTREAMING = 0; // TEST


    printf("GPU0 = %d\n", AOconf[loop].GPU0);
    if(AOconf[loop].GPU0>1)
    {
        uint8_t k;
        for(k=0; k<AOconf[loop].GPU0; k++)
            printf("stream %2d      GPUset0 = %2d\n", (int) k, GPUset0[k]);
    }

    printf("GPU1 = %d\n", AOconf[loop].GPU1);
    if(AOconf[loop].GPU1>1)
    {
        uint8_t k;
        for(k=0; k<AOconf[loop].GPU1; k++)
            printf("stream %2d      GPUset1 = %2d\n", (int) k, GPUset1[k]);
    }








    vOK = 1;
    if(AOconf[loop].init_wfsref0==0)
    {
        printf("ERROR: CANNOT RUN LOOP WITHOUT WFS REFERENCE\n");
        vOK = 0;
    }
    if(AOconf[loop].init_CM==0)
    {
        printf("ERROR: CANNOT RUN LOOP WITHOUT CONTROL MATRIX\n");
        vOK = 0;
    }

    AOconf[loop].initmapping = 0;
    AOconf[loop].init_CMc = 0;
    clock_gettime(CLOCK_REALTIME, &t1);


    if(vOK==1)
    {
        AOconf[loop].kill = 0;
        AOconf[loop].on = 0;
        AOconf[loop].DMprimaryWrite_ON = 0;
        AOconf[loop].ARPFon = 0;
        printf("entering loop ...\n");
        fflush(stdout);

        int timerinit = 0;

        while( AOconf[loop].kill == 0)
        {
            if(timerinit==1)
            {
                clock_gettime(CLOCK_REALTIME, &t1);
                printf("timer init\n");
            }
            clock_gettime(CLOCK_REALTIME, &t2);

            tdiff = info_time_diff(t1, t2);
            double tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;

            printf(" WAITING     %20.3lf sec         \r", tdiffv);
            fflush(stdout);
            usleep(1000);


            timerinit = 0;
            while(AOconf[loop].on == 1)
            {
                if(timerinit==0)
                {
#ifdef _PRINT_TEST
                    printf("TEST - Read first image\n");
                    fflush(stdout);
#endif

                    //      Read_cam_frame(loop, 0, AOconf[loop].WFSnormalize, 0, 1);
                    clock_gettime(CLOCK_REALTIME, &t1);
                    timerinit = 1;

#ifdef _PRINT_TEST
                    printf("\n");
                    printf("LOOP CLOSED  ");
                    fflush(stdout);
#endif
                }

#ifdef _PRINT_TEST
                printf("TEST - Start AO compute\n");
                fflush(stdout);
#endif

                AOcompute(loop, AOconf[loop].WFSnormalize);

#ifdef _PRINT_TEST
                printf("TEST - exiting AOcompute\n");
                fflush(stdout);
#endif


                AOconf[loop].status = 12; // 12
                clock_gettime(CLOCK_REALTIME, &tnow);
                tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
                tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
                data.image[aoconfID_looptiming].array.F[12] = tdiffv;

#ifdef _PRINT_TEST
                printf("TEST -  CMMODE = %d\n", AOconf[loop].CMMODE);
                fflush(stdout);
#endif

                if(AOconf[loop].CMMODE==0)  // 2-step : WFS -> mode coeffs -> DM act
                {
#ifdef _PRINT_TEST
                    printf("TEST -  DMprimaryWrite_ON = %d\n", AOconf[loop].DMprimaryWrite_ON);
                    fflush(stdout);
#endif

                    if(AOconf[loop].DMprimaryWrite_ON==1) // if Writing to DM
                    {
#ifdef _PRINT_TEST
                        printf("TEST -  gain = %f\n", AOconf[loop].gain);
                        fflush(stdout);
#endif

                        if(fabs(AOconf[loop].gain)>1.0e-6)
                            set_DM_modes(loop);
                    }

                }
                else // 1 step: WFS -> DM act
                {
                    if(AOconf[loop].DMprimaryWrite_ON==1) // if Writing to DM
                    {
                        data.image[aoconfID_dmC].md[0].write = 1;

                        for(ii=0; ii<AOconf[loop].sizeDM; ii++)//TEST
                        {
                            if(isnan(data.image[aoconfID_meas_act].array.F[ii])!=0)
                            {
                                printf("image aol2_meas_act  element %ld is NAN -> replacing by 0\n", ii);
                                data.image[aoconfID_meas_act].array.F[ii] = 0.0;
                            }
                        }




                        AOconf[loop].status = 13; // enforce limits
                        clock_gettime(CLOCK_REALTIME, &tnow);
                        tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
                        tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
                        data.image[aoconfID_looptiming].array.F[13] = tdiffv;


                        for(ii=0; ii<AOconf[loop].sizeDM; ii++)
                        {
                            data.image[aoconfID_dmC].array.F[ii] -= AOconf[loop].gain * data.image[aoconfID_meas_act].array.F[ii];

                            data.image[aoconfID_dmC].array.F[ii] *= AOconf[loop].mult;

                            if(data.image[aoconfID_dmC].array.F[ii] > AOconf[loop].maxlimit)
                                data.image[aoconfID_dmC].array.F[ii] = AOconf[loop].maxlimit;
                            if(data.image[aoconfID_dmC].array.F[ii] < -AOconf[loop].maxlimit)
                                data.image[aoconfID_dmC].array.F[ii] = -AOconf[loop].maxlimit;
                        }


                        AOconf[loop].status = 14; // write to DM
                        clock_gettime(CLOCK_REALTIME, &tnow);
                        tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
                        tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
                        data.image[aoconfID_looptiming].array.F[14] = tdiffv;



                        int semnb;
                        for(semnb=0; semnb<data.image[aoconfID_dmC].md[0].sem; semnb++)
                        {
                            sem_getvalue(data.image[aoconfID_dmC].semptr[semnb], &semval);
                            if(semval<SEMAPHORE_MAXVAL)
                                sem_post(data.image[aoconfID_dmC].semptr[semnb]);
                        }
                        data.image[aoconfID_dmC].md[0].cnt0++;
                        data.image[aoconfID_dmC].md[0].write = 0;
                        // inform dmdisp that new command is ready in one of the channels
                        if(aoconfID_dmdisp!=-1)
                            if(data.image[aoconfID_dmdisp].md[0].sem > 1)
                            {
                                sem_getvalue(data.image[aoconfID_dmdisp].semptr[0], &semval);
                                if(semval<SEMAPHORE_MAXVAL)
                                    sem_post(data.image[aoconfID_dmdisp].semptr[1]);
                            }
                        AOconf[loop].DMupdatecnt ++;
                    }
                }

                AOconf[loop].status = 18; // 18
                clock_gettime(CLOCK_REALTIME, &tnow);
                tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
                tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
                data.image[aoconfID_looptiming].array.F[18] = tdiffv;

                AOconf[loop].cnt++;







                data.image[aoconfIDlogdata].md[0].cnt0 = AOconf[loop].cnt;
                data.image[aoconfIDlogdata].array.F[0] = AOconf[loop].gain;


                if(AOconf[loop].cnt == AOconf[loop].cntmax)
                    AOconf[loop].on = 0;
            }

        }
    }

    free(thetime);

    return(0);
}







int_fast8_t ControlMatrixMultiply( float *cm_array, float *imarray, long m, long n, float *outvect)
{
    long i;

    cblas_sgemv (CblasRowMajor, CblasNoTrans, m, n, 1.0, cm_array, n, imarray, 1, 0.0, outvect, 1);

    return(0);
}





int_fast8_t set_DM_modes(long loop)
{
    double a;
    long cnttest;
    int semval;


    if(AOconf[loop].GPU1 == 0)
    {
        float *arrayf;
        long i, j, k;

        arrayf = (float*) malloc(sizeof(float)*AOconf[loop].sizeDM);

        for(j=0; j<AOconf[loop].sizeDM; j++)
            arrayf[j] = 0.0;

        for(i=0; i<AOconf[loop].sizeDM; i++)
            for(k=0; k < AOconf[loop].NBDMmodes; k++)
                arrayf[i] += data.image[aoconfID_cmd_modes].array.F[k] * data.image[aoconfID_DMmodes].array.F[k*AOconf[loop].sizeDM+i];

        data.image[aoconfID_dmC].md[0].write = 1;
        memcpy (data.image[aoconfID_dmC].array.F, arrayf, sizeof(float)*AOconf[loop].sizeDM);
        if(data.image[aoconfID_dmC].md[0].sem > 0)
        {
            sem_getvalue(data.image[aoconfID_dmC].semptr[0], &semval);
            if(semval<SEMAPHORE_MAXVAL)
                sem_post(data.image[aoconfID_dmC].semptr[0]);
        }
        data.image[aoconfID_dmC].md[0].cnt0++;
        data.image[aoconfID_dmC].md[0].write = 0;

        free(arrayf);
    }
    else
    {
#ifdef HAVE_CUDA


        GPU_loop_MultMat_setup(1, data.image[aoconfID_DMmodes].name, data.image[aoconfID_cmd_modes].name, data.image[aoconfID_dmC].name, AOconf[loop].GPU1, GPUset1, 1, AOconf[loop].GPUusesem, 1, loop);
        AOconf[loop].status = 12;
        clock_gettime(CLOCK_REALTIME, &tnow);
        tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
        tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
        data.image[aoconfID_looptiming].array.F[12] = tdiffv;

        GPU_loop_MultMat_execute(1, &AOconf[loop].status, &AOconf[loop].GPUstatus[0], 1.0, 0.0, 1);
#endif
    }

    if(aoconfID_dmdisp!=-1)
        if(data.image[aoconfID_dmdisp].md[0].sem > 1)
        {
            sem_getvalue(data.image[aoconfID_dmdisp].semptr[1], &semval);
            if(semval<SEMAPHORE_MAXVAL)
                sem_post(data.image[aoconfID_dmdisp].semptr[1]);
        }

    AOconf[loop].DMupdatecnt ++;

    return(0);
}





int_fast8_t set_DM_modesRM(long loop)
{
    long k;
    long i, j;
    float *arrayf;


    arrayf = (float*) malloc(sizeof(float)*AOconf[loop].sizeDM);

    for(j=0; j<AOconf[loop].sizeDM; j++)
        arrayf[j] = 0.0;

    for(k=0; k < AOconf[loop].NBDMmodes; k++)
    {
        for(i=0; i<AOconf[loop].sizeDM; i++)
            arrayf[i] += data.image[aoconfID_cmd_modesRM].array.F[k] * data.image[aoconfID_DMmodes].array.F[k*AOconf[loop].sizeDM+i];
    }


    data.image[aoconfID_dmRM].md[0].write = 1;
    memcpy (data.image[aoconfID_dmRM].array.F, arrayf, sizeof(float)*AOconf[loop].sizeDM);
    data.image[aoconfID_dmRM].md[0].cnt0++;
    data.image[aoconfID_dmRM].md[0].write = 0;

    free(arrayf);
    AOconf[loop].DMupdatecnt ++;

    return(0);
}





int_fast8_t AOcompute(long loop, int normalize)
{
    long k1, k2;
    long ii;
    long i;
    long m, n;
    long index;
    //  long long wcnt;
    // long long wcntmax;
    double a;

    float *matrix_cmp;
    long wfselem, act, mode;

    struct timespec t1;
    struct timespec t2;

    float *matrix_Mc, *matrix_DMmodes;
    long n_sizeDM, n_NBDMmodes, n_sizeWFS;

    long IDmask;
    long act_active, wfselem_active;
    float *matrix_Mc_active;
    long IDcmatca_shm;
    int r;
    float imtot;

    int slice;
    int semnb;
    int semval;



    // waiting for dark-subtracted image
    AOconf[loop].status = 19;  //  19: WAITING FOR IMAGE
    clock_gettime(CLOCK_REALTIME, &tnow);
    tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
    tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
    data.image[aoconfID_looptiming].array.F[19] = tdiffv;//TBM



    // md[0].atime.ts is absolute time at beginning of iteration
    //
    // pixel 0 is dt since last iteration
    //
    // pixel 1 is time from beginning of loop to status 01
    // pixel 2 is time from beginning of loop to status 02


    Read_cam_frame(loop, 0, normalize, 0, 0);

    slice = PIXSTREAM_SLICE;
    if(COMPUTE_PIXELSTREAMING==0) // no pixel streaming
        PIXSTREAM_SLICE = 0;
    //    else
    //        PIXSTREAM_SLICE = 1 + slice;

    //    printf("slice = %d  ->  %d\n", slice, PIXSTREAM_SLICE);
    //    fflush(stdout);

    AOconf[loop].status = 4;  // 4: REMOVING REF
    clock_gettime(CLOCK_REALTIME, &tnow);
    tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
    tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
    data.image[aoconfID_looptiming].array.F[4] = tdiffv;


    if(COMPUTE_GPU_SCALING==0)
    {
        data.image[aoconfID_imWFS2].md[0].write = 1;
        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            data.image[aoconfID_imWFS2].array.F[ii] = data.image[aoconfID_imWFS1].array.F[ii] - normfloorcoeff*data.image[aoconfID_wfsref].array.F[ii];
        COREMOD_MEMORY_image_set_sempost_byID(aoconfID_imWFS2, -1);
        data.image[aoconfID_imWFS2].md[0].cnt0 ++;
        data.image[aoconfID_imWFS2].md[0].write = 0;
    }


    AOconf[loop].status = 5; // 5 MULTIPLYING BY CONTROL MATRIX -> MODE VALUES
    clock_gettime(CLOCK_REALTIME, &tnow);
    tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
    tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
    data.image[aoconfID_looptiming].array.F[5] = tdiffv;


    if(AOconf[loop].initmapping == 0) // compute combined control matrix or matrices
    {
        printf("COMPUTING MAPPING ARRAYS .... \n");
        fflush(stdout);

        clock_gettime(CLOCK_REALTIME, &t1);

        //
        // There is one mapping array per WFS slice
        // WFS slice 0 = all active pixels
        //
        WFS_active_map = (int*) malloc(sizeof(int)*AOconf[loop].sizeWFS*PIXSTREAM_NBSLICES);
        if(aoconfID_wfsmask != -1)
        {
            for(slice=0; slice<PIXSTREAM_NBSLICES; slice++)
            {
                long ii1 = 0;
                for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                    if(data.image[aoconfID_wfsmask].array.F[ii]>0.1)
                    {
                        if(slice==0)
                        {
                            WFS_active_map[slice*AOconf[loop].sizeWFS+ii1] = ii;
                            ii1++;
                        }
                        else if (data.image[aoconfID_pixstream_wfspixindex].array.UI16[ii]==slice+1)
                        {
                            WFS_active_map[slice*AOconf[loop].sizeWFS+ii1] = ii;
                            ii1++;
                        }
                    }
                AOconf[loop].sizeWFS_active[slice] = ii1;

                char imname[200];
                if(sprintf(imname, "aol%ld_imWFS2active_%02d", LOOPNUMBER, slice) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                uint32_t *sizearray;
                sizearray = (uint32_t*) malloc(sizeof(uint32_t)*2);
                sizearray[0] =  AOconf[loop].sizeWFS_active[slice];
                sizearray[1] =  1;
                aoconfID_imWFS2_active[slice] = create_image_ID(imname, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
                free(sizearray);
                //aoconfID_imWFS2_active[slice] = create_2Dimage_ID(imname, AOconf[loop].sizeWFS_active[slice], 1);
            }
        }
        else
        {
            printf("ERROR: aoconfID_wfsmask = -1\n");
            fflush(stdout);
            exit(0);
        }



        // create DM active map
        DM_active_map = (int*) malloc(sizeof(int)*AOconf[loop].sizeDM);
        if(aoconfID_dmmask != -1)
        {
            long ii1 = 0;
            for(ii=0; ii<AOconf[loop].sizeDM; ii++)
                if(data.image[aoconfID_dmmask].array.F[ii]>0.5)
                {
                    DM_active_map[ii1] = ii;
                    ii1++;
                }
            AOconf[loop].sizeDM_active = ii1;
        }



        uint32_t *sizearray;
        sizearray = (uint32_t*) malloc(sizeof(uint32_t)*2);
        sizearray[0] = AOconf[loop].sizeDM_active;
        sizearray[1] = 1;

        char imname[200];
        if(sprintf(imname, "aol%ld_meas_act_active", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_meas_act_active = create_image_ID(imname, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
        free(sizearray);



        if(aoconfID_meas_act==-1)
        {
            sizearray = (uint32_t*) malloc(sizeof(uint32_t)*2);
            sizearray[0] = AOconf[loop].sizexDM;
            sizearray[1] = AOconf[loop].sizeyDM;

            if(sprintf(imname, "aol%ld_meas_act", LOOPNUMBER) < 1)
                printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

            aoconfID_meas_act = create_image_ID(imname, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
            COREMOD_MEMORY_image_set_createsem(imname, 10);
            free(sizearray);
        }

        clock_gettime(CLOCK_REALTIME, &t2);
        tdiff = info_time_diff(t1, t2);
        tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
        printf("\n");
        printf("TIME TO COMPUTE MAPPING ARRAYS = %f sec\n", tdiffv);
        AOconf[loop].initmapping = 1;
    }




    if(AOconf[loop].GPU0 == 0)   // run in CPU
    {
        if(AOconf[loop].CMMODE==0)  // goes explicitely through modes, slow but useful for tuning
        {
#ifdef _PRINT_TEST
            printf("TEST - CM mult: GPU=0, CMMODE=0 - %s x %s -> %s\n", data.image[aoconfID_contrM].md[0].name, data.image[aoconfID_imWFS2].md[0].name, data.image[aoconfID_meas_modes].md[0].name);
            fflush(stdout);
#endif

            data.image[aoconfID_meas_modes].md[0].write = 1;
            ControlMatrixMultiply( data.image[aoconfID_contrM].array.F, data.image[aoconfID_imWFS2].array.F, AOconf[loop].NBDMmodes, AOconf[loop].sizeWFS, data.image[aoconfID_meas_modes].array.F);
            COREMOD_MEMORY_image_set_sempost_byID(aoconfID_meas_modes, -1);
            data.image[aoconfID_meas_modes].md[0].cnt0 ++;
            data.image[aoconfID_meas_modes].md[0].write = 0;
        }
        else // (*)
        {
#ifdef _PRINT_TEST
            printf("TEST - CM mult: GPU=0, CMMODE=1 - using matrix %s\n", data.image[aoconfID_contrMc].md[0].name);
            fflush(stdout);
#endif

            data.image[aoconfID_meas_modes].md[0].write = 1;
            ControlMatrixMultiply( data.image[aoconfID_contrMc].array.F, data.image[aoconfID_imWFS2].array.F, AOconf[loop].sizeDM, AOconf[loop].sizeWFS, data.image[aoconfID_meas_act].array.F);
            data.image[aoconfID_meas_modes].md[0].cnt0 ++;
            COREMOD_MEMORY_image_set_sempost_byID(aoconfID_meas_modes, -1);
            data.image[aoconfID_meas_modes].md[0].cnt0 ++;
            data.image[aoconfID_meas_modes].md[0].write = 0;
        }
    }
    else
    {
#ifdef HAVE_CUDA
        if(AOconf[loop].CMMODE==0)  // goes explicitely through modes, slow but useful for tuning
        {
#ifdef _PRINT_TEST
            printf("TEST - CM mult: GPU=1, CMMODE=0 - using matrix %s    GPU alpha beta = %f %f\n", data.image[aoconfID_contrM].md[0].name, GPU_alpha, GPU_beta);
            fflush(stdout);
#endif

            //initWFSref_GPU[PIXSTREAM_SLICE] = 1; // default: do not re-compute reference output

            if(COMPUTE_GPU_SCALING==1)
            {
                // TBD : TEST IF contrM or wfsref have changed



                if(initWFSref_GPU[PIXSTREAM_SLICE]==0) // initialize WFS reference
                {
#ifdef _PRINT_TEST
                    printf("\nINITIALIZE WFS REFERENCE: COPY NEW REF (WFSREF) TO imWFS0\n"); //TEST
                    fflush(stdout);
#endif

                    data.image[aoconfID_imWFS0].md[0].write = 1;
                    for(wfselem=0; wfselem<AOconf[loop].sizeWFS; wfselem++)
                        data.image[aoconfID_imWFS0].array.F[wfselem] = data.image[aoconfID_wfsref].array.F[wfselem];
                    COREMOD_MEMORY_image_set_sempost_byID(aoconfID_imWFS0, -1);
                    data.image[aoconfID_imWFS0].md[0].cnt0++;
                    data.image[aoconfID_imWFS0].md[0].write = 0;
                    fflush(stdout);
                }
            }


            if(COMPUTE_GPU_SCALING==1)
                GPU_loop_MultMat_setup(0, data.image[aoconfID_contrM].name, data.image[aoconfID_imWFS0].name, data.image[aoconfID_meas_modes].name, AOconf[loop].GPU0, GPUset0, 0, AOconf[loop].GPUusesem, initWFSref_GPU[PIXSTREAM_SLICE], loop);
            else
                GPU_loop_MultMat_setup(0, data.image[aoconfID_contrM].name, data.image[aoconfID_imWFS2].name, data.image[aoconfID_meas_modes].name, AOconf[loop].GPU0, GPUset0, 0, AOconf[loop].GPUusesem, 1, loop);

            initWFSref_GPU[PIXSTREAM_SLICE] = 1;

            AOconf[loop].status = 6; // 6 execute

            clock_gettime(CLOCK_REALTIME, &tnow);
            tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
            tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
            data.image[aoconfID_looptiming].array.F[6] = tdiffv;

            if(COMPUTE_GPU_SCALING==1)
                GPU_loop_MultMat_execute(0, &AOconf[loop].status, &AOconf[loop].GPUstatus[0], GPU_alpha, GPU_beta, 1);
            else
                GPU_loop_MultMat_execute(0, &AOconf[loop].status, &AOconf[loop].GPUstatus[0], 1.0, 0.0, 1);
        }
        else // direct pixel -> actuators linear transformation
        {
#ifdef _PRINT_TEST
            printf("TEST - CM mult: GPU=1, CMMODE=1\n");
            fflush(stdout);
#endif

            if(1==0)
            {
                GPU_loop_MultMat_setup(0, data.image[aoconfID_contrMc].name, data.image[aoconfID_imWFS2].name, data.image[aoconfID_meas_act].name, AOconf[loop].GPU0, GPUset0, 0, AOconf[loop].GPUusesem, 1, loop);
                AOconf[loop].status = 6; // 6 execute
                clock_gettime(CLOCK_REALTIME, &tnow);
                tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
                tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
                data.image[aoconfID_looptiming].array.F[6] = tdiffv;

                GPU_loop_MultMat_execute(0, &AOconf[loop].status, &AOconf[loop].GPUstatus[0], 1.0, 0.0, 1);
            }
            else // only use active pixels and actuators (**)
            {
                // re-map input vector into imWFS2_active

                if(COMPUTE_GPU_SCALING==1) // (**)
                {
#ifdef _PRINT_TEST
                    printf("TEST - CM mult: GPU=1, CMMODE=1, COMPUTE_GPU_SCALING=1\n");
                    fflush(stdout);
#endif

                    data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].md[0].write = 1;
                    for(wfselem_active=0; wfselem_active<AOconf[loop].sizeWFS_active[PIXSTREAM_SLICE]; wfselem_active++)
                        data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].array.F[wfselem_active] = data.image[aoconfID_imWFS0].array.F[WFS_active_map[PIXSTREAM_SLICE*AOconf[loop].sizeWFS+wfselem_active]];
                    COREMOD_MEMORY_image_set_sempost_byID(aoconfID_imWFS2_active[PIXSTREAM_SLICE], -1);
                    data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].md[0].cnt0++;
                    data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].md[0].write = 0;
                }
                else
                {
                    data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].md[0].write = 1;
                    for(wfselem_active=0; wfselem_active<AOconf[loop].sizeWFS_active[PIXSTREAM_SLICE]; wfselem_active++)
                        data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].array.F[wfselem_active] = data.image[aoconfID_imWFS2].array.F[WFS_active_map[PIXSTREAM_SLICE*AOconf[loop].sizeWFS+wfselem_active]];
                    COREMOD_MEMORY_image_set_sempost_byID(aoconfID_imWFS2_active[PIXSTREAM_SLICE], -1);
                    data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].md[0].cnt0++;
                    data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].md[0].write = 0;
                }

                // look for updated control matrix or reference
                if(COMPUTE_GPU_SCALING==1) // (**)
                {
                    if(data.image[aoconfID_contrMcact[PIXSTREAM_SLICE]].md[0].cnt0 != contrMcactcnt0[PIXSTREAM_SLICE])
                    {
                        printf("NEW CONTROL MATRIX DETECTED (%s) -> RECOMPUTE REFERENCE x MATRIX\n", data.image[aoconfID_contrMcact[PIXSTREAM_SLICE]].md[0].name);
                        fflush(stdout);

                        initWFSref_GPU[PIXSTREAM_SLICE] = 0;
                        contrMcactcnt0[PIXSTREAM_SLICE] = data.image[aoconfID_contrMcact[PIXSTREAM_SLICE]].md[0].cnt0;
                    }

                    if(data.image[aoconfID_wfsref].md[0].cnt0 != wfsrefcnt0)  // (*)
                    {
                        printf("NEW REFERENCE WFS DETECTED (%s) [ %ld %ld ]\n", data.image[aoconfID_wfsref].md[0].name, data.image[aoconfID_wfsref].md[0].cnt0, wfsrefcnt0);
                        fflush(stdout);

                        initWFSref_GPU[PIXSTREAM_SLICE] = 0;
                        wfsrefcnt0 = data.image[aoconfID_wfsref].md[0].cnt0;
                    }
                    if(initWFSref_GPU[PIXSTREAM_SLICE]==0) // initialize WFS reference
                    {
                        printf("\nINITIALIZE WFS REFERENCE: COPY NEW REF (WFSREF) TO imWFS2_active\n"); //TEST
                        fflush(stdout);
                        data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].md[0].write = 1;
                        for(wfselem_active=0; wfselem_active<AOconf[loop].sizeWFS_active[PIXSTREAM_SLICE]; wfselem_active++)
                            data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].array.F[wfselem_active] = data.image[aoconfID_wfsref].array.F[WFS_active_map[PIXSTREAM_SLICE*AOconf[loop].sizeWFS+wfselem_active]];
                        COREMOD_MEMORY_image_set_sempost_byID(aoconfID_imWFS2_active[PIXSTREAM_SLICE], -1);
                        data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].md[0].cnt0++;
                        data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].md[0].write = 0;
                        fflush(stdout);
                    }
                }

                if(initcontrMcact_GPU[PIXSTREAM_SLICE]==0)
                    initWFSref_GPU[PIXSTREAM_SLICE] = 0;


                GPU_loop_MultMat_setup(0, data.image[aoconfID_contrMcact[PIXSTREAM_SLICE]].name, data.image[aoconfID_imWFS2_active[PIXSTREAM_SLICE]].name, data.image[aoconfID_meas_act_active].name, AOconf[loop].GPU0, GPUset0, 0, AOconf[loop].GPUusesem, initWFSref_GPU[PIXSTREAM_SLICE], loop);


                initWFSref_GPU[PIXSTREAM_SLICE] = 1;
                initcontrMcact_GPU[PIXSTREAM_SLICE] = 1;
                AOconf[loop].status = 6; // 6 execute
                clock_gettime(CLOCK_REALTIME, &tnow);
                tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
                tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
                data.image[aoconfID_looptiming].array.F[6] = tdiffv;


                if(COMPUTE_GPU_SCALING==1)
                    GPU_loop_MultMat_execute(0, &AOconf[loop].status, &AOconf[loop].GPUstatus[0], GPU_alpha, GPU_beta, 1);
                else
                    GPU_loop_MultMat_execute(0, &AOconf[loop].status, &AOconf[loop].GPUstatus[0], 1.0, 0.0, 1);

                // re-map output vector
                data.image[aoconfID_meas_act].md[0].write = 1;
                for(act_active=0; act_active<AOconf[loop].sizeDM_active; act_active++)
                    data.image[aoconfID_meas_act].array.F[DM_active_map[act_active]] = data.image[aoconfID_meas_act_active].array.F[act_active];

                for(semnb=0; semnb<data.image[aoconfID_meas_act].md[0].sem; semnb++)
                {
                    sem_getvalue(data.image[aoconfID_meas_act].semptr[semnb], &semval);
                    if(semval<SEMAPHORE_MAXVAL)
                        sem_post(data.image[aoconfID_meas_act].semptr[semnb]);
                }
                data.image[aoconfID_meas_act].md[0].cnt0++;
                data.image[aoconfID_meas_act].md[0].write = 0;
            }
        }
#endif
    }

    AOconf[loop].status = 11; // 11 MULTIPLYING BY GAINS
    clock_gettime(CLOCK_REALTIME, &tnow);
    tdiff = info_time_diff(data.image[aoconfID_looptiming].md[0].atime.ts, tnow);
    tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
    data.image[aoconfID_looptiming].array.F[11] = tdiffv;

    if(AOconf[loop].CMMODE==0)
    {
        AOconf[loop].RMSmodes = 0;
        long k;
        for(k=0; k<AOconf[loop].NBDMmodes; k++)
            AOconf[loop].RMSmodes += data.image[aoconfID_meas_modes].array.F[k]*data.image[aoconfID_meas_modes].array.F[k];

        AOconf[loop].RMSmodesCumul += AOconf[loop].RMSmodes;
        AOconf[loop].RMSmodesCumulcnt ++;


        for(k=0; k<AOconf[loop].NBDMmodes; k++)
        {
            data.image[aoconfID_RMS_modes].array.F[k] = 0.99*data.image[aoconfID_RMS_modes].array.F[k] + 0.01*data.image[aoconfID_meas_modes].array.F[k]*data.image[aoconfID_meas_modes].array.F[k];
            data.image[aoconfID_AVE_modes].array.F[k] = 0.99*data.image[aoconfID_AVE_modes].array.F[k] + 0.01*data.image[aoconfID_meas_modes].array.F[k];

            data.image[aoconfID_cmd_modes].array.F[k] -= AOconf[loop].gain * data.image[aoconfID_GAIN_modes].array.F[k] * data.image[aoconfID_meas_modes].array.F[k];

            if(data.image[aoconfID_cmd_modes].array.F[k] < -AOconf[loop].maxlimit * data.image[aoconfID_LIMIT_modes].array.F[k])
                data.image[aoconfID_cmd_modes].array.F[k] = -AOconf[loop].maxlimit * data.image[aoconfID_LIMIT_modes].array.F[k];

            if(data.image[aoconfID_cmd_modes].array.F[k] > AOconf[loop].maxlimit * data.image[aoconfID_LIMIT_modes].array.F[k])
                data.image[aoconfID_cmd_modes].array.F[k] = AOconf[loop].maxlimit * data.image[aoconfID_LIMIT_modes].array.F[k];

            data.image[aoconfID_cmd_modes].array.F[k] *= AOconf[loop].mult * data.image[aoconfID_MULTF_modes].array.F[k];

            // update total gain
            //     data.image[aoconfID_GAIN_modes].array.F[k+AOconf[loop].NBDMmodes] = AOconf[loop].gain * data.image[aoconfID_GAIN_modes].array.F[k];
        }


        data.image[aoconfID_cmd_modes].md[0].cnt0 ++;
    }

    return(0);
}



int_fast8_t AOloopControl_CompModes_loop(const char *ID_CM_name, const char *ID_WFSref_name, const char *ID_WFSim_name, const char *ID_WFSimtot_name, const char *ID_coeff_name)
{
#ifdef HAVE_CUDA

    int *GPUsetM;
    long ID_CM;
    long ID_WFSref;
    long ID_coeff;
    long GPUcnt;
    int k;
    int_fast8_t GPUstatus[100];
    int_fast8_t status;
    long NBmodes;
    uint32_t *sizearray;

    long ID_WFSim;
    long ID_WFSim_n;
    long wfsxsize, wfsysize;
    int m;
    long IDcoeff0;

    long ID_WFSimtot;
    double totfluxave;
    long ID_coefft;

    double alpha = 0.1;


    GPUcnt = 2;

    GPUsetM = (int*) malloc(sizeof(int)*GPUcnt);
    for(k=0; k<GPUcnt; k++)
        GPUsetM[k] = k+5;


    ID_CM = image_ID(ID_CM_name);
    wfsxsize = data.image[ID_CM].md[0].size[0];
    wfsysize = data.image[ID_CM].md[0].size[1];
    NBmodes = data.image[ID_CM].md[0].size[2];

    ID_WFSref = image_ID(ID_WFSref_name);


    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*2);
    sizearray[0] = NBmodes;
    sizearray[1] = 1;

    ID_coeff = create_image_ID(ID_coeff_name, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(ID_coeff_name, 10);
    data.image[ID_coeff].md[0].cnt0 = 0;

    ID_coefft = create_image_ID("coefftmp", 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem("coefftmp", 10);


    IDcoeff0 = create_image_ID("coeff0", 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    ID_WFSim_n = create_2Dimage_ID("wfsim_n", wfsxsize, wfsysize);
    COREMOD_MEMORY_image_set_createsem("wfsim_n", 10);




    ID_WFSim = image_ID(ID_WFSim_name);
    ID_WFSimtot = image_ID(ID_WFSimtot_name);


    GPU_loop_MultMat_setup(2, ID_CM_name, "wfsim_n", "coefftmp", GPUcnt, GPUsetM, 0, 1, 1, 0);

    totfluxave = 1.0;
    int initWFSref;
    while(1==1)
    {
        if(initWFSref==0)
        {
            printf("Computing reference\n");
            fflush(stdout);
            memcpy(data.image[ID_WFSim_n].array.F, data.image[ID_WFSref].array.F, sizeof(float)*wfsxsize*wfsysize);
            GPU_loop_MultMat_execute(2, &status, &GPUstatus[0], 1.0, 0.0, 0);
            for(m=0; m<NBmodes; m++)
            {
                data.image[IDcoeff0].array.F[m] = data.image[ID_coefft].array.F[m];
            }
            printf("\n");
            initWFSref = 1;
            printf("reference computed\n");
            fflush(stdout);
        }

        memcpy(data.image[ID_WFSim_n].array.F, data.image[ID_WFSim].array.F, sizeof(float)*wfsxsize*wfsysize);
        COREMOD_MEMORY_image_set_semwait(ID_WFSim_name, 0);

        GPU_loop_MultMat_execute(2, &status, &GPUstatus[0], 1.0, 0.0, 0);
        totfluxave = (1.0-alpha)*totfluxave + alpha*data.image[ID_WFSimtot].array.F[0];

        data.image[ID_coeff].md[0].write = 1;
        for(m=0; m<NBmodes; m++)
            data.image[ID_coeff].array.F[m] = data.image[ID_coefft].array.F[m]/totfluxave - data.image[IDcoeff0].array.F[m];
        data.image[ID_coeff].md[0].cnt0 ++;
        data.image[ID_coeff].md[0].write = 0;
    }


    delete_image_ID("coeff0");
    free(sizearray);

    free(GPUsetM);



#endif

    return(0);
}


//
// compute DM map from mode values
// this is a separate process -> using index = 0
//
// if offloadMode = 1, apply correction to aol#_dmC
//
int_fast8_t AOloopControl_GPUmodecoeffs2dm_filt_loop(const char *modecoeffs_name, const char *DMmodes_name, int semTrigg, const char *out_name, int GPUindex, long loop, int offloadMode)
{
#ifdef HAVE_CUDA
    long IDmodecoeffs;
    int GPUcnt, k;
    int *GPUsetM;
    int_fast8_t GPUstatus[100];
    int_fast8_t status;
    float alpha = 1.0;
    float beta = 0.0;
    int initWFSref = 0;
    int orientation = 1;
    int use_sem = 1;
    long IDout;
    int write_timing = 0;
    long NBmodes, m;

    float x, x2, x4, x8;
    float gamma;

    uint32_t *sizearray;
    char imnamecorr[200];
    long IDmodesC;

    char imnamedmC[200];
    long IDc;
    long dmxsize, dmysize;
    long ii;


    int RT_priority = 80; //any number from 0-99
    struct sched_param schedpar;



    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    sched_setscheduler(0, SCHED_FIFO, &schedpar);
#endif


    // read AO loop gain, mult
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);


    GPUcnt = 1;
    GPUsetM = (int*) malloc(sizeof(int)*GPUcnt);
    for(k=0; k<GPUcnt; k++)
        GPUsetM[k] = k+GPUindex;

    IDout = image_ID(out_name);
    IDmodecoeffs = image_ID(modecoeffs_name);

    NBmodes = data.image[IDmodecoeffs].md[0].size[0];


    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*2);

    if(sprintf(imnamecorr, "aol%ld_mode_limcorr", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    sizearray[0] = NBmodes;
    sizearray[1] = 1;
    IDmodesC = create_image_ID(imnamecorr, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imnamecorr, 10);
    free(sizearray);




    GPU_loop_MultMat_setup(0, DMmodes_name, imnamecorr, out_name, GPUcnt, GPUsetM, orientation, use_sem, initWFSref, 0);


    for(k=0; k<GPUcnt; k++)
        printf(" ====================     USING GPU %d\n", GPUsetM[k]);


    if(offloadMode==1)
    {
        if(sprintf(imnamedmC, "aol%ld_dmC", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        IDc = image_ID(imnamedmC);
        dmxsize = data.image[IDc].md[0].size[0];
        dmysize = data.image[IDc].md[0].size[1];


        printf("offloadMode = %d  %ld %ld\n", offloadMode, dmxsize, dmysize);
        fflush(stdout);
    }

    while(1==1)
    {
        COREMOD_MEMORY_image_set_semwait(modecoeffs_name, semTrigg);
        AOconf[loop].statusM = 10;

        for(m=0; m<NBmodes; m++)
            data.image[IDmodesC].array.F[m] = data.image[IDmodecoeffs].array.F[m];


        GPU_loop_MultMat_execute(0, &status, &GPUstatus[0], alpha, beta, write_timing);

        if(offloadMode==1) // offload back to dmC
        {
            data.image[IDc].md[0].write = 1;
            for(ii=0; ii<dmxsize*dmysize; ii++)
                data.image[IDc].array.F[ii] = data.image[IDout].array.F[ii];

            COREMOD_MEMORY_image_set_sempost_byID(IDc, -1);
            data.image[IDc].md[0].write = 0;
            data.image[IDc].md[0].cnt0++;
        }
        AOconf[loop].statusM = 20;
    }



    free(GPUsetM);

#endif

    return(0);
}





//
// assumes the WFS mode basis is already orthogonall
// removes reference from each frame
//
long AOloopControl_sig2Modecoeff(const char *WFSim_name, const char *IDwfsref_name, const char *WFSmodes_name, const char *outname)
{
    long IDout;
    long IDwfs, IDmodes, IDwfsref;
    long wfsxsize, wfsysize, wfssize, NBmodes, NBframes;
    double totim, totref;
    float coeff;
    long ii, m, kk;
    FILE *fp;
    double *mcoeff_ave;
    double *mcoeff_rms;


    IDwfs = image_ID(WFSim_name);
    wfsxsize = data.image[IDwfs].md[0].size[0];
    wfsysize = data.image[IDwfs].md[0].size[1];
    NBframes = data.image[IDwfs].md[0].size[2];
    wfssize = wfsxsize*wfsysize;




    IDwfsref = image_ID(IDwfsref_name);

    IDmodes = image_ID(WFSmodes_name);
    NBmodes = data.image[IDmodes].md[0].size[2];

    mcoeff_ave = (double*) malloc(sizeof(double)*NBmodes);
    mcoeff_rms = (double*) malloc(sizeof(double)*NBmodes);



    IDout = create_2Dimage_ID(outname, NBframes, NBmodes);

    totref = 0.0;

    for(ii=0; ii<wfssize; ii++)
        totref += data.image[IDwfsref].array.F[ii];
    for(ii=0; ii<wfssize; ii++)
        data.image[IDwfsref].array.F[ii] /= totref;

    for(kk=0; kk<NBframes; kk++)
    {
        totim = 0.0;
        for(ii=0; ii<wfssize; ii++)
            totim += data.image[IDwfs].array.F[kk*wfssize+ii];
        for(ii=0; ii<wfssize; ii++)
        {
            data.image[IDwfs].array.F[kk*wfssize+ii] /= totim;
            data.image[IDwfs].array.F[kk*wfssize+ii] -= data.image[IDwfsref].array.F[ii];
        }


        for(m=0; m<NBmodes; m++)
        {
            coeff = 0.0;
            for(ii=0; ii<wfssize; ii++)
                coeff += data.image[IDmodes].array.F[m*wfssize+ii] * data.image[IDwfs].array.F[kk*wfssize+ii];
            data.image[IDout].array.F[m*NBframes+kk] = coeff;
            mcoeff_ave[m] += coeff;
            mcoeff_rms[m] += coeff*coeff;
        }
    }


    fp  = fopen("mode_stats.txt", "w");
    for(m=0; m<NBmodes; m++)
    {
        mcoeff_rms[m] = sqrt( mcoeff_rms[m]/NBframes );
        mcoeff_ave[m] /= NBframes;
        fprintf(fp, "%4ld  %12g %12g\n", m, mcoeff_ave[m], mcoeff_rms[m]);
    }
    fclose(fp);

    free(mcoeff_ave);
    free(mcoeff_rms);

    return(IDout);
}




/**
 * ## Purpose
 * 
 * Computes average of residual in WFS
 * 
 * ## Arguments
 * 
 * @param[in]
 * loop		INT
 * 			loop number
 * 
 * @param[in]
 * alpha	FLOAT
 * 			averaging coefficient
 * 
 * 
 * ## Output files
 * 
 * - aol_wfsres_ave
 * - aol_wfsres_ave
 * - aol_wfsresm
 * - aol_wfsresm_ave
 * - aol_wfsres_rms
 * 
 * 
 * 
 */

long AOloopControl_computeWFSresidualimage(long loop, float alpha)
{
    long IDimWFS0, IDwfsref, IDwfsmask, IDtot, IDout, IDoutave, IDoutm, IDoutmave, IDoutrms;
    char imname[200];
    uint32_t *sizearray;
    long wfsxsize, wfsysize, wfsxysize;
    long cnt;
    long ii;


    if(sprintf(imname, "aol%ld_imWFS0", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDimWFS0 = read_sharedmem_image(imname);

    if(sprintf(imname, "aol%ld_wfsref", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDwfsref = read_sharedmem_image(imname);

    if(sprintf(imname, "aol%ld_wfsmask", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDwfsmask = read_sharedmem_image(imname);

    if(sprintf(imname, "aol%ld_imWFS0tot", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDtot = read_sharedmem_image(imname);

    wfsxsize = data.image[IDimWFS0].md[0].size[0];
    wfsysize = data.image[IDimWFS0].md[0].size[1];
    wfsxysize = wfsxsize*wfsysize;

    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*2);
    sizearray[0] = wfsxsize;
    sizearray[1] = wfsysize;

    if(sprintf(imname, "aol%ld_wfsres", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDout = create_image_ID(imname, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);

    if(sprintf(imname, "aol%ld_wfsres_ave", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDoutave = create_image_ID(imname, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);
    for(ii=0; ii<wfsxysize; ii++)
        data.image[IDoutave].array.F[ii] = 0.0;

    if(sprintf(imname, "aol%ld_wfsresm", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDoutm = create_image_ID(imname, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);

    if(sprintf(imname, "aol%ld_wfsresm_ave", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDoutmave = create_image_ID(imname, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);
    for(ii=0; ii<wfsxysize; ii++)
        data.image[IDoutave].array.F[ii] = 0.0;

    if(sprintf(imname, "aol%ld_wfsres_rms", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDoutrms = create_image_ID(imname, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);
    for(ii=0; ii<wfsxysize; ii++)
        data.image[IDoutrms].array.F[ii] = 0.0;

    free(sizearray);
    printf("alpha = %f\n", alpha);
    while(1)
    {
        if(data.image[IDimWFS0].md[0].sem==0)
        {
            while(cnt==data.image[IDimWFS0].md[0].cnt0) // test if new frame exists
                usleep(5);
            cnt = data.image[IDimWFS0].md[0].cnt0;
        }
        else
            sem_wait(data.image[IDimWFS0].semptr[3]);

        // imWFS0/tot0 - WFSref -> out

        data.image[IDout].md[0].write = 1;
        for(ii=0; ii<wfsxysize; ii++)
            data.image[IDout].array.F[ii] = data.image[IDimWFS0].array.F[ii]/data.image[IDtot].array.F[0] - data.image[IDwfsref].array.F[ii];
        data.image[IDout].md[0].cnt0++;
        data.image[IDout].md[0].write = 0;
        COREMOD_MEMORY_image_set_sempost_byID(IDout, -1);


        // apply mask

        data.image[IDoutm].md[0].write = 1;
        for(ii=0; ii<wfsxysize; ii++)
            data.image[IDoutm].array.F[ii] = data.image[IDout].array.F[ii] * data.image[IDwfsmask].array.F[ii];
        data.image[IDoutm].md[0].cnt0++;
        data.image[IDoutm].md[0].write = 0;
        COREMOD_MEMORY_image_set_sempost_byID(IDoutm, -1);


        // apply gain

        data.image[IDoutave].md[0].write = 1;
        for(ii=0; ii<wfsxysize; ii++)
            data.image[IDoutave].array.F[ii] = (1.0-alpha)*data.image[IDoutave].array.F[ii] + alpha*data.image[IDout].array.F[ii];
        data.image[IDoutave].md[0].cnt0++;
        data.image[IDoutave].md[0].write = 0;
        COREMOD_MEMORY_image_set_sempost_byID(IDoutave, -1);

        // apply mask

        data.image[IDoutmave].md[0].write = 1;
        for(ii=0; ii<wfsxysize; ii++)
            data.image[IDoutmave].array.F[ii] = data.image[IDoutave].array.F[ii] * data.image[IDwfsmask].array.F[ii];
        data.image[IDoutmave].md[0].cnt0++;
        data.image[IDoutmave].md[0].write = 0;
        COREMOD_MEMORY_image_set_sempost_byID(IDoutmave, -1);

        // compute RMS

        data.image[IDoutrms].md[0].write = 1;
        for(ii=0; ii<wfsxysize; ii++)
            data.image[IDoutrms].array.F[ii] = (1.0-alpha)*data.image[IDoutrms].array.F[ii] + alpha*(data.image[IDout].array.F[ii]-data.image[IDoutave].array.F[ii])*(data.image[IDout].array.F[ii]-data.image[IDoutave].array.F[ii]);
        data.image[IDoutrms].md[0].cnt0++;
        data.image[IDoutrms].md[0].write = 0;
        COREMOD_MEMORY_image_set_sempost_byID(IDoutrms, -1);

    }


    return(IDout);
}




// includes mode filtering (limits, multf)
long AOloopControl_ComputeOpenLoopModes(long loop)
{
    long IDout;
    long IDmodeval; // WFS measurement

    long IDmodevalDM_C; // DM correction, circular buffer to include history
    long modevalDM_bsize = 20; // circular buffer size
    long modevalDMindex = 0; // index in the circular buffer
    long modevalDMindexl = 0;

    long IDmodevalDM; // DM correction at WFS measurement time
    long IDmodevalDMnow; // current DM correction
    long IDmodevalDMnowfilt; // current DM correction filtered
    long modevalDMindex0, modevalDMindex1;
    float alpha;

    long IDmodevalPF; // predictive filter output

    long IDblknb;
    char imname[200];
    float *modegain;
    float *modemult;
    float *modelimit;
    long *modeblock;
    long i, n, ID, m, blk, NBmodes;
    unsigned int blockNBmodes[100];
    uint32_t *sizeout;
    float framelatency = 2.8;
    long framelatency0, framelatency1;
    long IDgainb;
    long cnt;


    // FILTERING
    int FILTERMODE = 1;
    //long IDmodeLIMIT;
    //long IDmodeMULT;

    // TELEMETRY
    long block;
    long blockstatcnt = 0;

    double blockaveOLrms[100];
    double blockaveCrms[100]; // correction RMS
    double blockaveWFSrms[100]; // WFS residual RMS
    double blockavelimFrac[100];

    double allaveOLrms;
    double allaveCrms;
    double allaveWFSrms;
    double allavelimFrac;

    float limitblockarray[100];
    long IDatlimbcoeff;

    float coeff;

    int RT_priority = 80; //any number from 0-99
    struct sched_param schedpar;


    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    sched_setscheduler(0, SCHED_FIFO, &schedpar);
#endif



    // read AO loop gain, mult
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);


    // INPUT
    if(sprintf(imname, "aol%ld_modeval", loop) < 1)// measured from WFS
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodeval = read_sharedmem_image(imname);
    NBmodes = data.image[IDmodeval].md[0].size[0];

    modegain = (float*) malloc(sizeof(float)*NBmodes);
    modemult = (float*) malloc(sizeof(float)*NBmodes);
    modelimit = (float*) malloc(sizeof(float)*NBmodes);

    modeblock = (long*) malloc(sizeof(long)*NBmodes);






    // CONNECT to arrays holding gain, limit, and multf values for blocks
    if(aoconfID_gainb == -1)
    {
        if(sprintf(imname, "aol%ld_gainb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_gainb = read_sharedmem_image(imname);
    }

    if(aoconfID_multfb == -1)
    {
        if(sprintf(imname, "aol%ld_multfb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_multfb = read_sharedmem_image(imname);
    }

    if(aoconfID_limitb == -1)
    {
        if(sprintf(imname, "aol%ld_limitb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_limitb = read_sharedmem_image(imname);
    }



    // CONNECT to arrays holding gain, limit and multf values for individual modes
    if(aoconfID_GAIN_modes == -1)
    {
        if(sprintf(imname, "aol%ld_DMmode_GAIN", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_GAIN_modes = read_sharedmem_image(imname);
    }
    printf("aoconfID_GAIN_modes = %ld\n", aoconfID_GAIN_modes);


    if(aoconfID_LIMIT_modes == -1)
    {
        if(sprintf(imname, "aol%ld_DMmode_LIMIT", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_LIMIT_modes = read_sharedmem_image(imname);
    }
    if(aoconfID_LIMIT_modes == -1)
        FILTERMODE = 0;


    if(aoconfID_MULTF_modes == -1)
    {
        if(sprintf(imname, "aol%ld_DMmode_MULTF", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_MULTF_modes = read_sharedmem_image(imname);
    }
    if(aoconfID_MULTF_modes == -1)
        FILTERMODE = 0;










    // predictive control output
    if(sprintf(imname, "aol%ld_modevalPF", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodevalPF = read_sharedmem_image(imname);
    if(IDmodevalPF != -1)
    {
        long ii;
        for(ii=0; ii<data.image[IDmodevalPF].md[0].size[0]*data.image[IDmodevalPF].md[0].size[1]; ii++)
            data.image[IDmodevalPF].array.F[ii] = 0.0;
    }


    // OUPUT
    sizeout = (uint32_t*) malloc(sizeof(uint32_t)*2);
    sizeout[0] = NBmodes;
    sizeout[1] = 1;

    if(sprintf(imname, "aol%ld_modeval_ol", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDout = create_image_ID(imname, 2, sizeout, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);

    if(sprintf(imname, "aol%ld_mode_blknb", loop) < 1) // block indices
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDblknb = create_image_ID(imname, 2, sizeout, _DATATYPE_UINT16, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);

    if(sprintf(imname, "aol%ld_modeval_dm_now", loop) < 1) // current modal DM correction
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodevalDMnow = create_image_ID(imname, 2, sizeout, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);

    if(sprintf(imname, "aol%ld_modeval_dm_now_filt", loop) < 1) // current modal DM correction, filtered
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodevalDMnowfilt = create_image_ID(imname, 2, sizeout, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);


    if(sprintf(imname, "aol%ld_modeval_dm", loop) < 1) // modal DM correction at time of currently available WFS measurement
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodevalDM = create_image_ID(imname, 2, sizeout, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);

    sizeout[1] = modevalDM_bsize;
    if(sprintf(imname, "aol%ld_modeval_dm_C", loop) < 1) // modal DM correction, circular buffer
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodevalDM_C = create_image_ID(imname, 2, sizeout, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);



    // auto limit tuning
    sizeout[0] = AOconf[loop].DMmodesNBblock;
    sizeout[1] = 1;
    if(sprintf(imname, "aol%ld_autotune_lim_bcoeff", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDatlimbcoeff = create_image_ID(imname, 2, sizeout, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(imname, 10);

    free(sizeout);

    printf("%ld modes\n", NBmodes);


    // Read from shared mem the DM mode files to indentify blocks
    data.image[IDblknb].md[0].write = 1;
    m = 0;
    blk = 0;
    while(m<NBmodes)
    {
        if(sprintf(imname, "aol%ld_DMmodes%02ld", loop, blk) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        printf("Loading %s  (%2ld/%2ld)\n", imname, m, NBmodes);
        fflush(stdout);

        ID = read_sharedmem_image(imname);
        if(ID==-1)
        {
            printf("ERROR: could not load %s from shared memory\n", imname);
            exit(0);
        }
        n = data.image[ID].md[0].size[2];
        printf(" -> found %2ld modes\n", n);
        blockNBmodes[blk] = n;

        for(i=0; i<n; i++)
        {
            modeblock[m] = blk;
            data.image[IDblknb].array.UI16[m] = blk;
            m++;
        }
        blk++;
    }
    COREMOD_MEMORY_image_set_sempost_byID(IDblknb, -1);
    data.image[IDblknb].md[0].cnt0++;
    data.image[IDblknb].md[0].write = 0;






    framelatency = AOconf[loop].hardwlatency_frame + AOconf[loop].wfsmextrlatency_frame;
    framelatency0 = (long) framelatency;
    framelatency1 = framelatency0 + 1;
    alpha = framelatency - framelatency0;

    // initialize arrays
    data.image[IDmodevalDM].md[0].write = 1;
    data.image[IDmodevalDMnow].md[0].write = 1;
    data.image[IDmodevalDM_C].md[0].write = 1;
    for(m=0; m<NBmodes; m++)
    {
        data.image[IDmodevalDM].array.F[m] = 0.0;
        data.image[IDmodevalDMnow].array.F[m] = 0.0;
        for(modevalDMindex=0; modevalDMindex<modevalDM_bsize; modevalDMindex++)
            data.image[IDmodevalDM_C].array.F[modevalDMindex*NBmodes+m] = 0;
        data.image[IDout].array.F[m] = 0.0;
    }
    COREMOD_MEMORY_image_set_sempost_byID(IDmodevalDM, -1);
    COREMOD_MEMORY_image_set_sempost_byID(IDmodevalDMnow, -1);
    COREMOD_MEMORY_image_set_sempost_byID(IDmodevalDM_C, -1);
    data.image[IDmodevalDM].md[0].cnt0++;
    data.image[IDmodevalDMnow].md[0].cnt0++;
    data.image[IDmodevalDM_C].md[0].cnt0++;
    data.image[IDmodevalDM].md[0].write = 0;
    data.image[IDmodevalDMnow].md[0].write = 0;
    data.image[IDmodevalDM_C].md[0].write = 0;

    printf("FILTERMODE = %d\n", FILTERMODE);
    list_image_ID();

    modevalDMindex = 0;
    modevalDMindexl = 0;
    cnt = 0;

    blockstatcnt = 0;
    for(block=0; block<AOconf[loop].DMmodesNBblock; block++)
    {
        blockaveOLrms[block] = 0.0;
        blockaveCrms[block] = 0.0;
        blockaveWFSrms[block] = 0.0;
        blockavelimFrac[block] = 0.0;
    }
    allaveOLrms = 0.0;
    allaveCrms = 0.0;
    allaveWFSrms = 0.0;
    allavelimFrac = 0.0;




    while (1)
    {
        // read WFS measured modes (residual)

        if(data.image[IDmodeval].md[0].sem==0)
        {
            while(cnt==data.image[IDmodeval].md[0].cnt0) // test if new frame exists
                usleep(5);
            cnt = data.image[IDmodeval].md[0].cnt0;
        }
        else
            sem_wait(data.image[IDmodeval].semptr[4]);

        // drive sem4 to zero
        while(sem_trywait(data.image[IDmodeval].semptr[4])==0) {}
        AOconf[loop].statusM = 3;





        // write gain, mult, limit into arrays
        for(m=0; m<NBmodes; m++)
        {
            modegain[m] = AOconf[loop].gain * data.image[aoconfID_gainb].array.F[modeblock[m]] * data.image[aoconfID_GAIN_modes].array.F[m];
            modemult[m] = AOconf[loop].mult * data.image[aoconfID_multfb].array.F[modeblock[m]] * data.image[aoconfID_MULTF_modes].array.F[m];
            modelimit[m] = data.image[aoconfID_limitb].array.F[modeblock[m]] * data.image[aoconfID_LIMIT_modes].array.F[m];
        }


        //
        // UPDATE CURRENT DM MODES STATE
        //
        //  current state =   modemult   x   ( last state   - modegain * WFSmodeval  )
        //
        // modevalDMindexl = last index in the IDmodevalDM_C buffer
        //
        data.image[IDmodevalDMnow].md[0].write = 1;
        for(m=0; m<NBmodes; m++)
            data.image[IDmodevalDMnow].array.F[m] = modemult[m]*(data.image[IDmodevalDM_C].array.F[modevalDMindexl*NBmodes+m] - modegain[m]*data.image[IDmodeval].array.F[m]);

        AOconf[loop].statusM = 4;



        //
        //  MIX PREDICTION WITH CURRENT DM STATE
        //
        if(AOconf[loop].ARPFon==1)
        {
            if(IDmodevalPF==-1)
            {
                if(sprintf(imname, "aol%ld_modevalPF", loop) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                IDmodevalPF = read_sharedmem_image(imname);
            }
            else
            {
                sem_wait(data.image[IDmodevalPF].semptr[3]);
                for(m=0; m<NBmodes; m++)
                    data.image[IDmodevalDMnow].array.F[m] = -AOconf[loop].ARPFgain*data.image[IDmodevalPF].array.F[m] + (1.0-AOconf[loop].ARPFgain)*data.image[IDmodevalDMnow].array.F[m];
                // drive semaphore to zero
                while(sem_trywait(data.image[IDmodevalPF].semptr[3])==0) {}
            }
        }


        AOconf[loop].statusM = 5;

        data.image[IDmodevalDMnowfilt].md[0].write = 1;
        // FILTERING MODE VALUES
        // THIS FILTERING GOES TOGETHER WITH THE WRITEBACK ON DM TO KEEP FILTERED AND ACTUAL VALUES IDENTICAL
        for(m=0; m<NBmodes; m++)
            data.image[IDmodevalDMnowfilt].array.F[m] = data.image[IDmodevalDMnow].array.F[m];


        if(AOconf[loop].AUTOTUNE_LIMITS_ON==1) // automatically adjust modal limits
        {
            data.image[IDatlimbcoeff].md[0].write = 1;
            for(block=0; block<AOconf[loop].DMmodesNBblock; block++)
                limitblockarray[block] = 0.0;

            data.image[aoconfID_LIMIT_modes].md[0].write = 1;
            data.image[aoconfID_limitb].md[0].write = 1;
            for(m=0; m<NBmodes; m++)
            {
                block = data.image[IDblknb].array.UI16[m];

                if( fabs(AOconf[loop].AUTOTUNE_LIMITS_mcoeff*data.image[IDmodevalDMnowfilt].array.F[m]) > modelimit[m])
                    data.image[aoconfID_LIMIT_modes].array.F[m] *= (1.0 + AOconf[loop].AUTOTUNE_LIMITS_delta);
                else
                    data.image[aoconfID_LIMIT_modes].array.F[m] *= (1.0 - AOconf[loop].AUTOTUNE_LIMITS_delta*0.01*AOconf[loop].AUTOTUNE_LIMITS_perc);


                limitblockarray[block] += data.image[aoconfID_LIMIT_modes].array.F[m];

            }
            COREMOD_MEMORY_image_set_sempost_byID(aoconfID_LIMIT_modes, -1);
            data.image[aoconfID_LIMIT_modes].md[0].cnt0++;
            data.image[aoconfID_LIMIT_modes].md[0].write = 0;

            data.image[IDatlimbcoeff].md[0].write = 1;
            for(block=0; block<AOconf[loop].DMmodesNBblock; block++)
            {
                data.image[IDatlimbcoeff].array.F[block] = limitblockarray[block] / blockNBmodes[block];
                coeff = ( 1.0 + (data.image[IDatlimbcoeff].array.F[block]-1.0)*AOconf[loop].AUTOTUNE_LIMITS_delta*0.1 );
                if(coeff < 1.0-AOconf[loop].AUTOTUNE_LIMITS_delta )
                    coeff = 1.0-AOconf[loop].AUTOTUNE_LIMITS_delta;
                if(coeff> 1.0+AOconf[loop].AUTOTUNE_LIMITS_delta )
                    coeff = 1.0+AOconf[loop].AUTOTUNE_LIMITS_delta;
                data.image[aoconfID_limitb].array.F[block] = data.image[aoconfID_limitb].array.F[block] * coeff;
            }
            COREMOD_MEMORY_image_set_sempost_byID(IDatlimbcoeff, -1);
            data.image[IDatlimbcoeff].md[0].cnt0++;
            data.image[IDatlimbcoeff].md[0].write = 0;


            COREMOD_MEMORY_image_set_sempost_byID(aoconfID_limitb, -1);
            data.image[aoconfID_limitb].md[0].cnt0++;
            data.image[aoconfID_limitb].md[0].write = 0;

        }





        if(FILTERMODE == 1)
        {
            for(m=0; m<NBmodes; m++)
            {
                block = data.image[IDblknb].array.UI16[m];

                if(data.image[IDmodevalDMnowfilt].array.F[m] > modelimit[m])
                {
                    blockavelimFrac[block] += 1.0;
                    data.image[IDmodevalDMnowfilt].array.F[m] = modelimit[m];
                }
                if(data.image[IDmodevalDMnowfilt].array.F[m] < -modelimit[m])
                {
                    blockavelimFrac[block] += 1.0;
                    data.image[IDmodevalDMnowfilt].array.F[m] = -modelimit[m];
                }
            }
        }



        COREMOD_MEMORY_image_set_sempost_byID(IDmodevalDMnow, -1);
        data.image[IDmodevalDMnow].md[0].cnt1 = modevalDMindex;
        data.image[IDmodevalDMnow].md[0].cnt0++;
        data.image[IDmodevalDMnow].md[0].write = 0;

        COREMOD_MEMORY_image_set_sempost_byID(IDmodevalDMnowfilt, -1);
        data.image[IDmodevalDMnowfilt].md[0].cnt1 = modevalDMindex;
        data.image[IDmodevalDMnowfilt].md[0].cnt0++;
        data.image[IDmodevalDMnowfilt].md[0].write = 0;

        AOconf[loop].statusM = 6;

        //
        // update current location of dm correction circular buffer
        //
        data.image[IDmodevalDM_C].md[0].write = 1;
        for(m=0; m<NBmodes; m++)
            data.image[IDmodevalDM_C].array.F[modevalDMindex*NBmodes+m] = data.image[IDmodevalDMnowfilt].array.F[m];
        COREMOD_MEMORY_image_set_sempost_byID(IDmodevalDM_C, -1);
        data.image[IDmodevalDM_C].md[0].cnt1 = modevalDMindex;
        data.image[IDmodevalDM_C].md[0].cnt0++;
        data.image[IDmodevalDM_C].md[0].write = 0;



        AOconf[loop].statusM1 = 6;



        //
        // COMPUTE DM STATE AT TIME OF WFS MEASUREMENT
        // LINEAR INTERPOLATION BETWEEN NEAREST TWO VALUES
        //
        modevalDMindex0 = modevalDMindex - framelatency0;
        if(modevalDMindex0<0)
            modevalDMindex0 += modevalDM_bsize;
        modevalDMindex1 = modevalDMindex - framelatency1;
        if(modevalDMindex1<0)
            modevalDMindex1 += modevalDM_bsize;

        data.image[IDmodevalDM].md[0].write = 1;
        for(m=0; m<NBmodes; m++)
            data.image[IDmodevalDM].array.F[m] = (1.0-alpha)*data.image[IDmodevalDM_C].array.F[modevalDMindex0*NBmodes+m] + alpha*data.image[IDmodevalDM_C].array.F[modevalDMindex1*NBmodes+m];
        COREMOD_MEMORY_image_set_sempost_byID(IDmodevalDM, -1);
        data.image[IDmodevalDM].md[0].cnt0++;
        data.image[IDmodevalDM].md[0].write = 0;


        AOconf[loop].statusM1 = 7;

        //
        // OPEN LOOP STATE = most recent WFS reading - time-lagged DM
        //
        data.image[IDout].md[0].write = 1;
        for(m=0; m<NBmodes; m++)
            data.image[IDout].array.F[m] = data.image[IDmodeval].array.F[m] - data.image[IDmodevalDM].array.F[m];
        COREMOD_MEMORY_image_set_sempost_byID(IDout, -1);
        data.image[IDout].md[0].cnt0++;
        data.image[IDout].md[0].write = 0;



        AOconf[loop].statusM1 = 8;


        modevalDMindexl = modevalDMindex;
        modevalDMindex++;
        if(modevalDMindex==modevalDM_bsize)
            modevalDMindex = 0;


        // TELEMETRY
        for(m=0; m<NBmodes; m++)
        {
            block = data.image[IDblknb].array.UI16[m];

            blockaveOLrms[block] += data.image[IDout].array.F[m]*data.image[IDout].array.F[m];
            blockaveCrms[block] += data.image[IDmodevalDMnow].array.F[m]*data.image[IDmodevalDMnow].array.F[m];
            blockaveWFSrms[block] += data.image[IDmodeval].array.F[m]*data.image[IDmodeval].array.F[m];
        }

        blockstatcnt ++;
        if(blockstatcnt == AOconf[loop].AveStats_NBpt)
        {
            for(block=0; block<AOconf[loop].DMmodesNBblock; block++)
            {
                AOconf[loop].blockave_OLrms[block] = sqrt(blockaveOLrms[block]/blockstatcnt);
                AOconf[loop].blockave_Crms[block] = sqrt(blockaveCrms[block]/blockstatcnt);
                AOconf[loop].blockave_WFSrms[block] = sqrt(blockaveWFSrms[block]/blockstatcnt);
                AOconf[loop].blockave_limFrac[block] = (blockavelimFrac[block])/blockstatcnt;

                allaveOLrms += blockaveOLrms[block];
                allaveCrms += blockaveCrms[block];
                allaveWFSrms += blockaveWFSrms[block];
                allavelimFrac += blockavelimFrac[block];

                blockaveOLrms[block] = 0.0;
                blockaveCrms[block] = 0.0;
                blockaveWFSrms[block] = 0.0;
                blockavelimFrac[block] = 0.0;
            }

            AOconf[loop].ALLave_OLrms = sqrt(allaveOLrms/blockstatcnt);
            AOconf[loop].ALLave_Crms = sqrt(allaveCrms/blockstatcnt);
            AOconf[loop].ALLave_WFSrms = sqrt(allaveWFSrms/blockstatcnt);
            AOconf[loop].ALLave_limFrac = allavelimFrac/blockstatcnt;

            allaveOLrms = 0.0;
            allaveCrms = 0.0;
            allaveWFSrms = 0.0;
            allavelimFrac = 0.0;

            blockstatcnt = 0;
        }

        AOconf[loop].statusM1 = 9;
    }

    free(modegain);
    free(modemult);
    free(modelimit);

    free(modeblock);

    return(IDout);
}




//
// gains autotune
//
// input: modeval_ol
// APPLIES new gain values if AUTOTUNE_GAINS_ON
//
int_fast8_t AOloopControl_AutoTuneGains(long loop, const char *IDout_name)
{
    long IDmodevalOL;
    long IDmodeval;
    long IDmodeval_dm;
    long IDmodeval_dm_now;
    long IDmodeval_dm_now_filt;

    long NBmodes;
    char imname[200];
    long m;
    double diff1, diff2, diff3, diff4;
    float *array_mvalOL1;
    float *array_mvalOL2;
    float *array_mvalOL3;
    float *array_mvalOL4;
    double *array_sig1;
    double *array_sig2;
    double *array_sig3;
    double *array_sig4;
    float *array_sig;
    float *array_asq;
    long double *ave0;
    long double *sig0;
    long double *sig1;
    long double *sig2;
    long double *sig3;
    long double *sig4;
    float *stdev;

    float gain;
    long NBgain;
    long kk, kkmin;
    float errmin;
    float *errarray;
    float mingain = 0.01;
    float gainfactstep = 1.05;
    float *gainval_array;
    float *gainval1_array;
    float *gainval2_array;

    long long cnt = 0;
    float latency;
    FILE *fp;

    int RT_priority = 80; //any number from 0-99
    struct sched_param schedpar;


    long IDout;
    uint32_t *sizearray;

    float gain0; // corresponds to evolution timescale
    long cntstart;



    long IDblk;
    unsigned short block;
    float *modegain;
    float *modemult;


    float *NOISEfactor;
    long cnt00, cnt01;
    long IDsync;


    int TESTMODE = 1;
    int TEST_m = 30;
    FILE *fptest;


    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    sched_setscheduler(0, SCHED_FIFO, &schedpar);
#endif





    // read AO loop gain, mult
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);


    AOconf[loop].AUTOTUNEGAIN_evolTimescale = 0.05;

    gain0 = 1.0/(AOconf[loop].loopfrequ*AOconf[loop].AUTOTUNEGAIN_evolTimescale);




    // CONNECT to arrays holding gain, limit, and multf values for blocks

    if(aoconfID_gainb == -1)
    {
        if(sprintf(imname, "aol%ld_gainb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        aoconfID_gainb = read_sharedmem_image(imname);
    }

    if(aoconfID_multfb == -1)
    {
        if(sprintf(imname, "aol%ld_multfb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        aoconfID_multfb = read_sharedmem_image(imname);
    }

    // CONNECT to arrays holding gain, limit and multf values for individual modes

    if(aoconfID_GAIN_modes == -1)
    {
        if(sprintf(imname, "aol%ld_DMmode_GAIN", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        aoconfID_GAIN_modes = read_sharedmem_image(imname);
    }
    printf("aoconfID_GAIN_modes = %ld\n", aoconfID_GAIN_modes);

    if(aoconfID_MULTF_modes == -1)
    {
        if(sprintf(imname, "aol%ld_DMmode_MULTF", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");
        aoconfID_MULTF_modes = read_sharedmem_image(imname);
    }



    // INPUT
    if(sprintf(imname, "aol%ld_modeval_ol", loop) < 1) // measured from WFS
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodevalOL = read_sharedmem_image(imname);
    NBmodes = data.image[IDmodevalOL].md[0].size[0];

    if(sprintf(imname, "aol%ld_modeval", loop) < 1) // measured from WFS
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodeval = read_sharedmem_image(imname);

    if(sprintf(imname, "aol%ld_modeval_dm", loop) < 1) // measured from WFS
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodeval_dm = read_sharedmem_image(imname);

    if(sprintf(imname, "aol%ld_modeval_dm_now", loop) < 1) // current modal DM correction
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodeval_dm_now = read_sharedmem_image(imname);

    if(sprintf(imname, "aol%ld_modeval_dm_now_filt", loop) < 1) // current modal DM correction, filtered
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodeval_dm_now_filt = read_sharedmem_image(imname);




    // blocks
    if(sprintf(imname, "aol%ld_mode_blknb", loop) < 1) // block indices
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDblk = read_sharedmem_image(imname);


    modegain = (float*) malloc(sizeof(float)*NBmodes);
    modemult = (float*) malloc(sizeof(float)*NBmodes);
    NOISEfactor = (float*) malloc(sizeof(float)*NBmodes);

    // write gain, mult into arrays
    for(m=0; m<NBmodes; m++)
    {
        block = data.image[IDblk].array.UI16[m];
        modegain[m] = AOconf[loop].gain * data.image[aoconfID_gainb].array.F[block] * data.image[aoconfID_GAIN_modes].array.F[m];
        modemult[m] = AOconf[loop].mult * data.image[aoconfID_multfb].array.F[block] * data.image[aoconfID_MULTF_modes].array.F[m];
        NOISEfactor[m] = 1.0 + modemult[m]*modemult[m]*modegain[m]*modegain[m]/(1.0-modemult[m]*modemult[m]);
    }






    sizearray = (uint32_t*) malloc(sizeof(uint32_t)*3);
    sizearray[0] = NBmodes;
    sizearray[1] = 1;
    IDout = create_image_ID(IDout_name, 2, sizearray, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(IDout_name, 10);
    free(sizearray);




    // last open loop move values
    array_mvalOL1 = (float*) malloc(sizeof(float)*NBmodes);
    array_mvalOL2 = (float*) malloc(sizeof(float)*NBmodes);
    array_mvalOL3 = (float*) malloc(sizeof(float)*NBmodes);
    array_mvalOL4 = (float*) malloc(sizeof(float)*NBmodes);
    array_sig1 = (double*) malloc(sizeof(double)*NBmodes);
    array_sig2 = (double*) malloc(sizeof(double)*NBmodes);
    array_sig3 = (double*) malloc(sizeof(double)*NBmodes);
    array_sig4 = (double*) malloc(sizeof(double)*NBmodes);

    array_sig = (float*) malloc(sizeof(float)*NBmodes);
    array_asq = (float*) malloc(sizeof(float)*NBmodes);
    ave0 = (long double*) malloc(sizeof(long double)*NBmodes);
    sig0 = (long double*) malloc(sizeof(long double)*NBmodes);
    sig1 = (long double*) malloc(sizeof(long double)*NBmodes);
    sig2 = (long double*) malloc(sizeof(long double)*NBmodes);
    sig3 = (long double*) malloc(sizeof(long double)*NBmodes);
    sig4 = (long double*) malloc(sizeof(long double)*NBmodes);
    stdev = (float*) malloc(sizeof(float)*NBmodes);

    // prepare gain array
    latency = AOconf[loop].hardwlatency_frame + AOconf[loop].wfsmextrlatency_frame;
    printf("latency = %f frame\n", latency);
    NBgain = 0;
    gain = mingain;
    while(gain<1.0)
    {
        gain *= gainfactstep;
        NBgain++;
    }
    gainval_array = (float*) malloc(sizeof(float)*NBgain);
    gainval1_array = (float*) malloc(sizeof(float)*NBgain);
    gainval2_array = (float*) malloc(sizeof(float)*NBgain);

    kk = 0;
    gain = mingain;
    while(kk<NBgain)
    {
        gainval_array[kk] = gain;
        gainval1_array[kk] = (latency + 1.0/gain)*(latency + 1.0/(gain+gain0));
        gainval2_array[kk] = (gain/(1.0-gain));

        //printf("gain   %4ld  %12f   %12f  %12f\n", kk, gainval_array[kk], gainval1_array[kk], gainval2_array[kk]);
        gain *= gainfactstep;
        kk++;
    }
    errarray = (float*) malloc(sizeof(float)*NBgain);


    // drive sem5 to zero
    while(sem_trywait(data.image[IDmodevalOL].semptr[5])==0) {}



    for(m=0; m<NBmodes; m++)
    {
        array_mvalOL1[m] = 0.0;
        array_mvalOL2[m] = 0.0;
        array_sig1[m] = 0.0;
        array_sig2[m] = 0.0;
        ave0[m] = 0.0;
        sig0[m] = 0.0;
        sig1[m] = 0.0;
        sig2[m] = 0.0;
        sig3[m] = 0.0;
        sig4[m] = 0.0;
        stdev[m] = 0.0;
    }



    // TEST mode
    IDsync = image_ID("dm00disp10");
    if(IDsync!=-1)
    {
        list_image_ID();
        printf("SYNCHRO SIGNAL WAIT\n");
        fflush(stdout);

        cnt00 = data.image[IDsync].md[0].cnt1 - 1;
        cnt01 = cnt00+1;

        while(cnt01>=cnt00)
        {
            cnt00 = cnt01;
            sem_wait(data.image[IDsync].semptr[4]);
            cnt01 = data.image[IDsync].md[0].cnt1;
        }
        printf("START MEASUREMENT  [%6ld %6ld] \n", cnt00, cnt01);
        fflush(stdout);
    }



    if(TESTMODE==1)
        fptest = fopen("test_autotunegain.dat", "w");

    cnt = 0;
    cntstart = 10;
    while(cnt<5000)
    {
        sem_wait(data.image[IDmodevalOL].semptr[5]);


        data.image[IDout].md[0].write = 1;

        for(m=0; m<NBmodes; m++)
        {
            diff1 = data.image[IDmodevalOL].array.F[m] - array_mvalOL1[m];
            diff2 = data.image[IDmodevalOL].array.F[m] - array_mvalOL2[m];
            diff3 = data.image[IDmodevalOL].array.F[m] - array_mvalOL3[m];
            diff4 = data.image[IDmodevalOL].array.F[m] - array_mvalOL4[m];
            array_mvalOL4[m] = array_mvalOL3[m];
            array_mvalOL3[m] = array_mvalOL2[m];
            array_mvalOL2[m] = array_mvalOL1[m];
            array_mvalOL1[m] = data.image[IDmodevalOL].array.F[m];

            if(cnt>cntstart)
            {
                ave0[m] += data.image[IDmodevalOL].array.F[m];
                sig0[m] += data.image[IDmodevalOL].array.F[m]*data.image[IDmodevalOL].array.F[m];
                sig1[m] += diff1*diff1;
                sig2[m] += diff2*diff2;
                sig3[m] += diff3*diff3;
                sig4[m] += diff4*diff4;
            }
        }

        if(TESTMODE==1)
            fprintf(fptest, "%5lld %+12.10f %+12.10f %+12.10f %+12.10f %+12.10f\n", cnt, data.image[IDmodeval].array.F[TEST_m], data.image[IDmodevalOL].array.F[TEST_m], data.image[IDmodeval_dm].array.F[TEST_m], data.image[IDmodeval_dm_now].array.F[TEST_m], data.image[IDmodeval_dm_now_filt].array.F[TEST_m]);

        cnt++;
    }
    if(TESTMODE==1)
        fclose(fptest);

    data.image[IDout].md[0].write = 1;
    for(m=0; m<NBmodes; m++)
    {
        ave0[m] /= cnt-cntstart;
        sig0[m] /= cnt-cntstart;
        array_sig1[m] = sig1[m]/(cnt-cntstart);
        array_sig2[m] = sig2[m]/(cnt-cntstart);
        array_sig3[m] = sig3[m]/(cnt-cntstart);
        array_sig4[m] = sig4[m]/(cnt-cntstart);


        //		array_asq[m] = (array_sig2[m]-array_sig1[m])/3.0;
        array_asq[m] = (array_sig4[m]-array_sig1[m])/15.0;
        if(array_asq[m]<0.0)
            array_asq[m] = 0.0;
        array_sig[m] = (4.0*array_sig1[m] - array_sig2[m])/6.0;

        stdev[m] = sig0[m] - NOISEfactor[m]*array_sig[m] - ave0[m]*ave0[m];
        if(stdev[m]<0.0)
            stdev[m] = 0.0;
        stdev[m] = sqrt(stdev[m]);

        for(kk=0; kk<NBgain; kk++)
            errarray[kk] = array_asq[m] * gainval1_array[kk] + array_sig[m] * gainval2_array[kk];

        errmin = errarray[0];
        kkmin = 0;

        for(kk=0; kk<NBgain; kk++)
            if(errarray[kk]<errmin)
            {
                errmin = errarray[kk];
                kkmin = kk;
            }

        data.image[IDout].array.F[m] = gainval_array[kkmin];
    }

    COREMOD_MEMORY_image_set_sempost_byID(IDout, -1);
    data.image[IDout].md[0].cnt0++;
    data.image[IDout].md[0].write = 0;





    if(AOconf[loop].AUTOTUNE_GAINS_ON==1) // automatically adjust gain values
    {
    }


    fp = fopen("optgain.dat", "w");
    for(m=0; m<NBmodes; m++)
        fprintf(fp, "%5ld   %12.10f %12.10f %12.10f %12.10f %12.10f   %6.4f  %16.14f %16.14f  %6.2f\n", m, (float) ave0[m], (float) sig0[m], stdev[m], sqrt(array_asq[m]), sqrt(array_sig[m]), data.image[IDout].array.F[m], array_sig1[m], array_sig4[m], NOISEfactor[m]);
    fclose(fp);

    free(gainval_array);
    free(gainval1_array);
    free(gainval2_array);
    free(errarray);

    free(array_mvalOL1);
    free(array_mvalOL2);
    free(array_mvalOL3);
    free(array_mvalOL4);
    free(ave0);
    free(sig0);
    free(sig1);
    free(sig2);
    free(sig3);
    free(sig4);
    free(array_sig1);
    free(array_sig2);
    free(array_sig3);
    free(array_sig4);
    free(array_sig);
    free(array_asq);

    free(modegain);
    free(modemult);
    free(NOISEfactor);

    free(stdev);

    return(0);
}





long AOloopControl_dm2dm_offload(const char *streamin, const char *streamout, float twait, float offcoeff, float multcoeff)
{
    long IDin, IDout;
    long cnt = 0;
    long xsize, ysize, xysize;
    long ii;
    //long IDtmp;


    IDin = image_ID(streamin);
    IDout = image_ID(streamout);

    xsize = data.image[IDin].md[0].size[0];
    ysize = data.image[IDin].md[0].size[1];
    xysize = xsize*ysize;

    while(1)
    {
        printf("%8ld : offloading   %s -> %s\n", cnt, streamin, streamout);

        data.image[IDout].md[0].write = 1;
        for(ii=0; ii<xysize; ii++)
            data.image[IDout].array.F[ii] = multcoeff*(data.image[IDout].array.F[ii] + offcoeff*data.image[IDin].array.F[ii]);
        COREMOD_MEMORY_image_set_sempost_byID(IDout, -1);
        data.image[IDout].md[0].cnt0++;
        data.image[IDout].md[0].write = 0;

        usleep((long) (1000000.0*twait));
        cnt++;
    }

    return(IDout);
}





/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 7. PREDICTIVE CONTROL                                                    */
/* =============================================================================================== */
/* =============================================================================================== */




//
// IDcoeff_name is AO telemetry file
// size:   #modes, 1, #samples
//
int_fast8_t AOloopControl_mapPredictiveFilter(const char *IDmodecoeff_name, long modeout, double delayfr)
{
    long IDmodecoeff;
    long NBsamples;
    long NBmodes;
    double SVDeps = 0.001;

    long IDtrace;

    long modesize = 15;
    long modeoffset;
    long modeouto;
    long filtsize = 20;
    double val;

    long ii, jj, m;


    modeoffset = modeout - (long) (modesize/2);
    modeouto = modeout-modeoffset;

    IDmodecoeff = image_ID(IDmodecoeff_name);
    NBmodes = data.image[IDmodecoeff].md[0].size[0];
    NBsamples = data.image[IDmodecoeff].md[0].size[2];


    // reformat measurements
    IDtrace = create_2Dimage_ID("trace", NBsamples, modesize);

    for(ii=0; ii<NBsamples; ii++)
        for(m=0; m<modesize; m++)
            data.image[IDtrace].array.F[m*NBsamples+ii] = data.image[IDmodecoeff].array.F[ii*NBmodes+m];


    val = AOloopControl_testPredictiveFilter("trace", modeouto, delayfr, filtsize, "filt", SVDeps);
    delete_image_ID("filt");

    return(0);
}



///
/// predictive control based on SVD
///
/// input:
///     mode values trace  [ii: time, jj: mode number]
///     mode index
///     delayfr [delay in frame unit]
///     filtsize [number of samples in filter]
///
double AOloopControl_testPredictiveFilter(const char *IDtrace_name, long modeout, double delayfr, long filtsize, const char *IDfilt_name, double SVDeps)
{
    long IDtrace;
    long IDmatA;
    long NBtraceVec; // number of measurement vectors in trace
    long NBmvec; // number of measurements in measurement matrix
    long NBch; // number of channels in measurement
    long IDmatC;
    long IDfilt;
    long l,m;
    float *marray; // measurement array
    FILE *fp;
    float tmpv;
    long delayfr_int;
    float delayfr_x;
    long ch, l1;
    double err0, err1;
    float v0;
    float NoiseAmpl = 0.02;



    IDtrace = image_ID(IDtrace_name);

    NBtraceVec = data.image[IDtrace].md[0].size[0];
    NBch = data.image[IDtrace].md[0].size[1];


    NBmvec = NBtraceVec - filtsize - (long) (delayfr+1.0);




    // build measurement matrix

    fp = fopen("tracepts.txt","w");
    IDmatA = create_2Dimage_ID("WFPmatA", NBmvec, filtsize*NBch);
    // each column is a measurement
    for(m=0; m<NBmvec; m++) // column index
    {
        fprintf(fp, "%5ld %f\n", m, data.image[IDtrace].array.F[NBtraceVec*modeout+m+filtsize]);
        for(l=0; l<filtsize; l++)
            for(ch=0; ch<NBch; ch++)
            {
                l1 = ch*filtsize + l;
                data.image[IDmatA].array.F[l1*NBmvec+m] = data.image[IDtrace].array.F[NBtraceVec*ch + (m+l)];
            }
    }
    fclose(fp);






    // build measurement vector
    delayfr_int = (int) delayfr;
    delayfr_x = delayfr - delayfr_int;
    printf("%f  = %ld + %f\n", delayfr, delayfr_int, delayfr_x);
    marray = (float*) malloc(sizeof(float)*NBmvec);
    fp = fopen("tracepts1.txt","w");
    for(m=0; m<NBmvec; m++)
    {
        marray[m] = data.image[IDtrace].array.F[NBtraceVec*modeout+(m+filtsize+delayfr_int)]*(1.0-delayfr_x) + data.image[IDtrace].array.F[NBtraceVec*modeout+(m+filtsize+delayfr_int+1)]*delayfr_x;
        fprintf(fp, "%5ld %f %f\n", m, data.image[IDtrace].array.F[NBtraceVec*modeout+m+filtsize], marray[m]);
    }
    fclose(fp);


    linopt_compute_SVDpseudoInverse("WFPmatA", "WFPmatC", SVDeps, 10000, "WFP_VTmat");

    save_fits("WFPmatA", "!WFPmatA.fits");
    save_fits("WFPmatC", "!WFPmatC.fits");
    IDmatC = image_ID("WFPmatC");

    IDfilt = create_2Dimage_ID(IDfilt_name, filtsize, NBch);
    for(l=0; l<filtsize; l++)
        for(ch=0; ch<NBch; ch++)
        {
            tmpv = 0.0;
            for(m=0; m<NBmvec; m++)
                tmpv += data.image[IDmatC].array.F[(ch*filtsize+l)*NBmvec+m] * marray[m];
            data.image[IDfilt].array.F[ch*filtsize+l] = tmpv;
        }

    fp = fopen("filt.txt", "w");
    tmpv = 0.0;
    for(l=0; l<filtsize; l++)
        for(ch=0; ch<NBch; ch++)
        {
            tmpv += data.image[IDfilt].array.F[ch*filtsize+l];
            fprintf(fp, "%3ld %3ld %f %f\n", ch, l, data.image[IDfilt].array.F[l], tmpv);
        }
    fclose(fp);
    printf("filter TOTAL = %f\n", tmpv);

    // TEST FILTER

    // col #1 : time index m
    // col #2 : value at index m
    // col #3 : predicted value at m+delay
    // col #4 : actual value at m+delay
    fp = fopen("testfilt.txt", "w");
    err0 = 0.0;
    err1 = 0.0;
    for(m=filtsize; m<NBtraceVec-(delayfr_int+1); m++)
    {
        tmpv = 0.0;
        for(l=0; l<filtsize; l++)
            for(ch=0; ch<NBch; ch++)
                tmpv += data.image[IDfilt].array.F[ch*filtsize+l]*data.image[IDtrace].array.F[NBtraceVec*ch + (m-filtsize+l)];

        fprintf(fp, "%5ld %20f %20f %20f\n", m, data.image[IDtrace].array.F[NBtraceVec*modeout + m], tmpv, marray[m-filtsize]);

        v0 = tmpv - marray[m-filtsize];
        err0 += v0*v0;

        v0 = data.image[IDtrace].array.F[NBtraceVec*modeout + m] - marray[m-filtsize];
        err1 += v0*v0;
    }
    fclose(fp);
    free(marray);

    err0 = sqrt(err0/(NBtraceVec-filtsize-(delayfr_int+1)));
    err1 = sqrt(err1/(NBtraceVec-filtsize-(delayfr_int+1)));
    printf("Prediction error (using optimal filter)   :   %f\n", err0);
    printf("Prediction error (using last measurement) :   %f\n", err1);

    return(err1);
}







long AOloopControl_builPFloop_WatchInput(long loop, long PFblock)
{
    long IDinb0;
    long IDinb1;
    char imnameb0[500];
    char imnameb1[500];
    long cnt0, cnt1;
    long cnt0_old, cnt1_old;
    long IDinb;

    long twaitus = 100000; // 0.1 sec

    long PFblockStart;
    long PFblockEnd;
    long PFblockSize;
    long PFblockOrder;
    float PFblockLag;
    float PFblockdgain;
    FILE *fp;
    char fname[500];
    int ret;

    int Tupdate = 0;
    time_t t;
    struct tm *uttime;
    struct timespec timenow;
    long xsize, ysize, zsize, xysize;
    int cube;

    long IDout;
    uint32_t *imsizearray;
    uint8_t atype;
    char imnameout[500];
    long ii, kk;
    long ave;

    char inmaskname[200];
    char inmaskfname[200];
    char outmaskfname[200];
    long IDinmask;


    // read PF block parameters
    if(sprintf(fname, "conf/param_PFblock_%03ld.txt", PFblock) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if((fp = fopen(fname, "r"))==NULL)
    {
        printf("ERROR: File \"%s\" NOT FOUND\n", fname);
        exit(0);
    }
    else
    {
        if(fscanf(fp, "%50ld %50ld %50ld %50f %50f\n", &PFblockStart, &PFblockEnd, &PFblockOrder, &PFblockLag, &PFblockdgain) != 5)
            printERROR(__FILE__, __func__, __LINE__, "Cannot read parameters from file");
        fclose(fp);
    }
    PFblockSize = PFblockEnd - PFblockStart;


    if(sprintf(imnameb0, "aol%ld_modeval_ol_logbuff0", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if(sprintf(imnameb1, "aol%ld_modeval_ol_logbuff1", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDinb0 = read_sharedmem_image(imnameb0);
    IDinb1 = read_sharedmem_image(imnameb1);

    cnt0_old = data.image[IDinb0].md[0].cnt0;
    cnt1_old = data.image[IDinb1].md[0].cnt0;

    xsize = data.image[IDinb0].md[0].size[0];
    ysize = data.image[IDinb0].md[0].size[1];
    xysize = xsize*ysize;
    zsize = data.image[IDinb0].md[0].size[2];
    atype = data.image[IDinb0].md[0].atype;


    list_image_ID();


    if(system("mkdir -p PredictiveControl") != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    if(sprintf(inmaskname, "inmaskPFb%ld", PFblock) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDinmask = create_2Dimage_ID(inmaskname, xysize, 1);
    for(ii=0; ii<xysize; ii++)
        data.image[IDinmask].array.F[ii] = 0.0;
    for(ii=PFblockStart; ii<PFblockEnd; ii++)
        data.image[IDinmask].array.F[ii] = 1.0;

    if(sprintf(inmaskfname, "!./PredictiveControl/inmaskPF%ld.fits", PFblock) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    save_fits(inmaskname, inmaskfname);
    if(sprintf(outmaskfname, "!./PredictiveControl/outmaskPF%ld.fits", PFblock) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    save_fits(inmaskname, outmaskfname);





    printf("Create aol%ld_modevalol_PFb%ld  : %ld x 1 x %ld\n", loop, PFblock, PFblockSize, zsize);
    fflush(stdout);
    imsizearray = (uint32_t*) malloc(sizeof(uint32_t)*3);
    imsizearray[0] = PFblockSize;
    imsizearray[1] = 1;
    imsizearray[2] = zsize;

    if(sprintf(imnameout, "aol%ld_modevalol_PFb%ld", loop, PFblock) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDout = create_image_ID(imnameout, 3, imsizearray, atype, 1, 1);
    free(imsizearray);
    COREMOD_MEMORY_image_set_semflush(imnameout, -1);
    printf("Done\n");
    fflush(stdout);


    while(1)
    {
        cnt0 = data.image[IDinb0].md[0].cnt0;
        cnt1 = data.image[IDinb1].md[0].cnt0;

        if(cnt0!=cnt0_old)
        {
            cube = 0;
            cnt0_old = cnt0;
            IDinb = IDinb0;
            Tupdate = 1;
        }

        if(cnt1!=cnt1_old)
        {
            cube = 1;
            cnt1_old = cnt1;
            IDinb = IDinb1;
            Tupdate = 1;
        }

        if(Tupdate == 1)
        {
            t = time(NULL);
            uttime = gmtime(&t);
            clock_gettime(CLOCK_REALTIME, &timenow);
            printf("%02d:%02d:%02ld.%09ld  NEW TELEMETRY BUFFER AVAILABLE [%d]\n", uttime->tm_hour, uttime->tm_min, timenow.tv_sec % 60, timenow.tv_nsec, cube);


            data.image[IDout].md[0].write = 1;

            for(kk=0; kk<zsize; kk++)
                for(ii=0; ii<PFblockSize; ii++)
                    data.image[IDout].array.F[kk*PFblockSize + ii] = data.image[IDinb].array.F[kk*xysize + (ii+PFblockStart)];

            for(ii=0; ii<PFblockSize; ii++)
            {
                ave = 0.0;
                for(kk=0; kk<zsize; kk++)
                    ave += data.image[IDout].array.F[kk*PFblockSize + ii];

                ave /= zsize;
                for(kk=0; kk<zsize; kk++)
                    data.image[IDout].array.F[kk*PFblockSize + ii] -= ave;
            }


            COREMOD_MEMORY_image_set_sempost_byID(IDout, -1);
            data.image[IDout].md[0].cnt0++;
            data.image[IDout].md[0].write = 0;

            Tupdate = 0;
        }


        usleep(twaitus);
    }

    return (IDout);
}






/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 8.   LOOP CONTROL INTERFACE                                              */
/* =============================================================================================== */
/* =============================================================================================== */




int_fast8_t AOloopControl_setLoopNumber(long loop)
{


    printf("LOOPNUMBER = %ld\n", loop);
    LOOPNUMBER = loop;

    /** append process name with loop number */


    return 0;
}


int_fast8_t AOloopControl_setparam(long loop, const char *key, double value)
{
    int pOK=0;
    char kstring[200];



    strcpy(kstring, "PEperiod");
    if((strncmp (key, kstring, strlen(kstring)) == 0)&&(pOK==0))
    {
        //AOconf[loop].WFScamPEcorr_period = (long double) value;
        pOK = 1;
    }

    if(pOK==0)
        printf("Parameter not found\n");



    return (0);
}





/* =============================================================================================== */
/** @name AOloopControl - 8.1. LOOP CONTROL INTERFACE - MAIN CONTROL : LOOP ON/OFF START/STOP/STEP/RESET  */
/* =============================================================================================== */


int_fast8_t AOloopControl_loopon()
{

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].cntmax = AOconf[LOOPNUMBER].cnt-1;

    AOconf[LOOPNUMBER].on = 1;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_loopoff()
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].on = 0;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_loopkill()
{

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].kill = 1;

    return 0;
}


int_fast8_t AOloopControl_loopstep(long loop, long NBstep)
{

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[loop].cntmax = AOconf[loop].cnt + NBstep;
    AOconf[LOOPNUMBER].RMSmodesCumul = 0.0;
    AOconf[LOOPNUMBER].RMSmodesCumulcnt = 0;

    AOconf[loop].on = 1;

    while(AOconf[loop].on==1)
        usleep(100); // THIS WAITING IS OK


    return 0;
}


int_fast8_t AOloopControl_loopreset()
{
    long k;
    long mb;

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_cmd_modes==-1)
    {
        char name[200];
        if(sprintf(name, "DMmode_cmd_%ld", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_cmd_modes = read_sharedmem_image(name);
    }

    AOconf[LOOPNUMBER].on = 0;
    for(k=0; k<AOconf[LOOPNUMBER].NBDMmodes; k++)
        data.image[aoconfID_cmd_modes].array.F[k] = 0.0;

    for(mb=0; mb<AOconf[LOOPNUMBER].DMmodesNBblock; mb)
    {
        AOloopControl_setgainblock(mb, 0.0);
        AOloopControl_setlimitblock(mb, 0.01);
        AOloopControl_setmultfblock(mb, 0.95);
    }

    return 0;
}



/* =============================================================================================== */
/** @name AOloopControl - 8.2. LOOP CONTROL INTERFACE - DATA LOGGING                               */
/* =============================================================================================== */



/* =============================================================================================== */
/** @name AOloopControl - 8.3. LOOP CONTROL INTERFACE - PRIMARY DM WRITE                           */
/* =============================================================================================== */

int_fast8_t AOloopControl_DMprimaryWrite_on()
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].DMprimaryWrite_ON = 1;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_DMprimaryWrite_off()
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].DMprimaryWrite_ON = 0;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


/* =============================================================================================== */
/** @name AOloopControl - 8.4. LOOP CONTROL INTERFACE - INTEGRATOR AUTO TUNING                     */
/* =============================================================================================== */


int_fast8_t AOloopControl_AUTOTUNE_LIMITS_on()
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].AUTOTUNE_LIMITS_ON = 1;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_AUTOTUNE_LIMITS_off()
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].AUTOTUNE_LIMITS_ON = 0;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_set_AUTOTUNE_LIMITS_delta(float AUTOTUNE_LIMITS_delta)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].AUTOTUNE_LIMITS_delta = AUTOTUNE_LIMITS_delta;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_set_AUTOTUNE_LIMITS_perc(float AUTOTUNE_LIMITS_perc)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].AUTOTUNE_LIMITS_perc = AUTOTUNE_LIMITS_perc;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}

int_fast8_t AOloopControl_set_AUTOTUNE_LIMITS_mcoeff(float AUTOTUNE_LIMITS_mcoeff)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].AUTOTUNE_LIMITS_mcoeff = AUTOTUNE_LIMITS_mcoeff;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}

int_fast8_t AOloopControl_AUTOTUNE_GAINS_on()
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].AUTOTUNE_GAINS_ON = 1;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_AUTOTUNE_GAINS_off()
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].AUTOTUNE_GAINS_ON = 0;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}





/* =============================================================================================== */
/** @name AOloopControl - 8.5. LOOP CONTROL INTERFACE - PREDICTIVE FILTER ON/OFF                   */
/* =============================================================================================== */


int_fast8_t AOloopControl_ARPFon()
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].ARPFon = 1;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_ARPFoff()
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].ARPFon = 0;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}



/* =============================================================================================== */
/** @name AOloopControl - 8.6. LOOP CONTROL INTERFACE - TIMING PARAMETERS                          */
/* =============================================================================================== */



int_fast8_t AOloopControl_set_loopfrequ(float loopfrequ)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].loopfrequ = loopfrequ;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_set_hardwlatency_frame(float hardwlatency_frame)
{

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].hardwlatency_frame = hardwlatency_frame;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_set_complatency_frame(float complatency_frame)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].complatency_frame = complatency_frame;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_set_wfsmextrlatency_frame(float wfsmextrlatency_frame)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].wfsmextrlatency_frame = wfsmextrlatency_frame;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}



/* =============================================================================================== */
/** @name AOloopControl - 8.7. LOOP CONTROL INTERFACE - CONTROL LOOP PARAMETERS                    */
/* =============================================================================================== */



int_fast8_t AOloopControl_setgain(float gain)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].gain = gain;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_setARPFgain(float gain)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].ARPFgain = gain;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_setWFSnormfloor(float WFSnormfloor)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].WFSnormfloor = WFSnormfloor;
    printf("SHOWING PARAMETERS ...\n");
    fflush(stdout);
    AOloopControl_showparams(LOOPNUMBER);
    printf("DONE ...\n");
    fflush(stdout);

    return 0;
}


int_fast8_t AOloopControl_setmaxlimit(float maxlimit)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].maxlimit = maxlimit;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_setmult(float multcoeff)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].mult = multcoeff;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}


int_fast8_t AOloopControl_setframesAve(long nbframes)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].framesAve = nbframes;
    AOloopControl_showparams(LOOPNUMBER);

    return 0;
}




int_fast8_t AOloopControl_set_modeblock_gain(long loop, long blocknb, float gain, int add)
{
    long IDcontrM0; // local storage
    long IDcontrMc0; // local storage
    long IDcontrMcact0; // local storage
    long ii, kk;
    char name[200];
    char name1[200];
    char name2[200];
    char name3[200];
    long ID;
    double eps=1e-6;

    long NBmodes;
    long m, m1;
    long sizeWFS;


    printf("AOconf[loop].DMmodesNBblock = %ld\n", AOconf[loop].DMmodesNBblock);
    fflush(stdout);

    /*if(AOconf[loop].CMMODE==0)
    {
        printf("Command has no effect: modeblock gain not compatible with CMMODE = 0\n");
        fflush(stdout);
    }
    else*/
     
    if (AOconf[loop].DMmodesNBblock<2)
    {
        if(sprintf(name2, "aol%ld_contrMc00", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        if(sprintf(name3, "aol%ld_contrMcact00_00", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        // for CPU mode
        printf("UPDATING Mc matrix (CPU mode)\n");
        ID = image_ID(name2);
        data.image[aoconfID_contrMc].md[0].write = 1;
        memcpy(data.image[aoconfID_contrMc].array.F, data.image[ID].array.F, sizeof(float)*AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS*AOconf[loop].sizexDM*AOconf[loop].sizeyDM);
        data.image[aoconfID_contrMc].md[0].cnt0++;
        data.image[aoconfID_contrMc].md[0].write = 0;

        // for GPU mode
        printf("UPDATING Mcact matrix (GPU mode)\n");
        ID = image_ID(name3);
        data.image[aoconfID_contrMcact[0]].md[0].write = 1;
        memcpy(data.image[aoconfID_contrMcact[0]].array.F, data.image[ID].array.F, sizeof(float)*AOconf[loop].activeWFScnt*AOconf[loop].activeDMcnt);
        data.image[aoconfID_contrMcact[0]].md[0].cnt0++;
        data.image[aoconfID_contrMcact[0]].md[0].write = 0;
    }
    else
    {
        NBmodes = 0;
        for(kk=0; kk<AOconf[loop].DMmodesNBblock; kk++)
            NBmodes += AOconf[loop].NBmodes_block[kk];



        if(sprintf(name, "aol%ld_gainb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_gainb = image_ID(name);
        if((blocknb<AOconf[loop].DMmodesNBblock)&&(blocknb>-1))
            data.image[aoconfID_gainb].array.F[blocknb] = gain;

        sizeWFS = AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS;



        if(add==1)
        {
            IDcontrMc0 = image_ID("contrMc0");
            if(IDcontrMc0==-1)
                IDcontrMc0 = create_3Dimage_ID("contrMc0", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, AOconf[loop].sizexDM*AOconf[loop].sizeyDM);


            IDcontrMcact0 = image_ID("contrMcact0");
            if(IDcontrMcact0==-1)
                IDcontrMcact0 = create_2Dimage_ID("contrMcact0", AOconf[loop].activeWFScnt, AOconf[loop].activeDMcnt);

            //arith_image_zero("contrM0");
            arith_image_zero("contrMc0");
            arith_image_zero("contrMcact0");


            m = 0;
            for(kk=0; kk<AOconf[loop].DMmodesNBblock; kk++)
            {
                //sprintf(name1, "aol%ld_contrM%02ld", loop, kk);
                if(sprintf(name2, "aol%ld_contrMc%02ld", loop, kk) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                if(sprintf(name3, "aol%ld_contrMcact%02ld_00", loop, kk) < 1)
                    printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

                printf("Adding %4ld / %4ld  (%5.3f)   %s  %s   [%ld]\n", kk, AOconf[loop].DMmodesNBblock, data.image[aoconfID_gainb].array.F[kk], name, name1, aoconfID_gainb);

                //ID = image_ID(name1);

                //printf("updating %ld modes  [%ld]\n", data.image[ID].md[0].size[2], aoconfID_gainb);
                //	fflush(stdout); // TEST



                if(data.image[aoconfID_gainb].array.F[kk]>eps)
                {
                    ID = image_ID(name2);
# ifdef _OPENMP
                    #pragma omp parallel for
# endif
                    for(ii=0; ii<AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS*AOconf[loop].sizexDM*AOconf[loop].sizeyDM; ii++)
                        data.image[IDcontrMc0].array.F[ii] += data.image[aoconfID_gainb].array.F[kk]*data.image[ID].array.F[ii];

                    ID = image_ID(name3);
# ifdef _OPENMP
                    #pragma omp parallel for
# endif
                    for(ii=0; ii<AOconf[loop].activeWFScnt*AOconf[loop].activeDMcnt; ii++)
                        data.image[IDcontrMcact0].array.F[ii] += data.image[aoconfID_gainb].array.F[kk]*data.image[ID].array.F[ii];
                }

            }

            // for CPU mode
            printf("UPDATING Mc matrix (CPU mode)\n");
            data.image[aoconfID_contrMc].md[0].write = 1;
            memcpy(data.image[aoconfID_contrMc].array.F, data.image[IDcontrMc0].array.F, sizeof(float)*AOconf[loop].sizexWFS*AOconf[loop].sizeyWFS*AOconf[loop].sizexDM*AOconf[loop].sizeyDM);
            data.image[aoconfID_contrMc].md[0].cnt0++;
            data.image[aoconfID_contrMc].md[0].write = 0;


            // for GPU mode
            printf("UPDATING Mcact matrix (GPU mode)\n");
            data.image[aoconfID_contrMcact[0]].md[0].write = 1;
            memcpy(data.image[aoconfID_contrMcact[0]].array.F, data.image[IDcontrMcact0].array.F, sizeof(float)*AOconf[loop].activeWFScnt*AOconf[loop].activeDMcnt);
            data.image[aoconfID_contrMcact[0]].md[0].cnt0++;
            data.image[aoconfID_contrMcact[0]].md[0].write = 0;

            initcontrMcact_GPU[0] = 0;
        }
    }

    return(0);
}




int_fast8_t AOloopControl_scanGainBlock(long NBblock, long NBstep, float gainStart, float gainEnd, long NBgain)
{
    long k, kg;
    float gain;
    float bestgain= 0.0;
    float bestval = 10000000.0;
    float val;
    char name[200];



    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_cmd_modes==-1)
    {
        if(sprintf(name, "aol%ld_DMmode_cmd", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_cmd_modes = read_sharedmem_image(name);
    }


    printf("Block: %ld, NBstep: %ld, gain: %f->%f (%ld septs)\n", NBblock, NBstep, gainStart, gainEnd, NBgain);

    for(kg=0; kg<NBgain; kg++)
    {
        for(k=0; k<AOconf[LOOPNUMBER].NBDMmodes; k++)
            data.image[aoconfID_cmd_modes].array.F[k] = 0.0;

        gain = gainStart + 1.0*kg/(NBgain-1)*(gainEnd-gainStart);
        AOloopControl_setgainblock(NBblock, gain);
        AOloopControl_loopstep(LOOPNUMBER, NBstep);
        val = sqrt(AOconf[LOOPNUMBER].RMSmodesCumul/AOconf[LOOPNUMBER].RMSmodesCumulcnt);
        printf("%2ld  %6.4f  %10.8lf\n", kg, gain, val);

        if(val<bestval)
        {
            bestval = val;
            bestgain = gain;
        }
    }
    printf("BEST GAIN = %f\n", bestgain);

    AOloopControl_setgainblock(NBblock, bestgain);

    return(0);
}








/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 9. STATUS / TESTING / PERF MEASUREMENT                                   */
/* =============================================================================================== */
/* =============================================================================================== */





int_fast8_t AOloopControl_printloopstatus(long loop, long nbcol, long IDmodeval_dm, long IDmodeval, long IDmodevalave, long IDmodevalrms, long ksize)
{
    long k, kmin, kmax;
    long col;
    float val;
    long nbl = 1;
    float AVElim = 0.01; // [um]
    float RMSlim = 0.01; // [um]
    char imname[200];

    long IDblknb;


    printw("    loop number %ld    ", loop);


    if(AOconf[loop].on == 1)
        printw("loop is ON     ");
    else
        printw("loop is OFF    ");

    /*  if(AOconf[loop].logon == 1)
          printw("log is ON   ");
      else
          printw("log is OFF  ");

    */


    if(sprintf(imname, "aol%ld_mode_blknb", loop) < 1) // block indices
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDblknb = image_ID(imname);

    if(IDblknb==-1)
        IDblknb = read_sharedmem_image(imname);



    if(aoconfID_LIMIT_modes == -1)
    {
        if(sprintf(imname, "aol%ld_DMmode_LIMIT", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_LIMIT_modes = read_sharedmem_image(imname);
    }


    printw("   STATUS = %3d  ", AOconf[loop].status);

    kmax = (wrow-28)*(nbcol);


    printw("    Gain = %5.3f   maxlim = %5.3f     GPU = %d    kmax=%ld\n", AOconf[loop].gain, AOconf[loop].maxlimit, AOconf[loop].GPU0, kmax);
    printw("    DMprimWrite = %d   Predictive control state: %d        ARPF gain = %5.3f   AUTOTUNE LIM = %d (perc = %.2f %%  delta = %.3f nm mcoeff=%4.2f) GAIN = %d\n", AOconf[loop].DMprimaryWrite_ON, AOconf[loop].ARPFon, AOconf[loop].ARPFgain, AOconf[loop].AUTOTUNE_LIMITS_ON, AOconf[loop].AUTOTUNE_LIMITS_perc, 1000.0*AOconf[loop].AUTOTUNE_LIMITS_delta, AOconf[loop].AUTOTUNE_LIMITS_mcoeff, AOconf[loop].AUTOTUNE_GAINS_ON);
    printw(" TIMIMNG :  lfr = %9.3f Hz    hw lat = %5.3f fr   comp lat = %5.3f fr  wfs extr lat = %5.3f fr\n", AOconf[loop].loopfrequ, AOconf[loop].hardwlatency_frame, AOconf[loop].complatency_frame, AOconf[loop].wfsmextrlatency_frame);
    nbl++;
    nbl++;
    nbl++;
    printw("loop iteration CNT : %lld\n", AOconf[loop].cnt);
    nbl++;

    printw("\n");
    nbl++;

    printw("=========== %6ld modes, %3ld blocks ================|------------ Telemetry [nm] ----------------|    |     LIMITS         |\n", AOconf[loop].NBDMmodes, AOconf[loop].DMmodesNBblock);
    nbl++;
    printw("BLOCK  #modes [ min - max ]    gain   limit   multf  |       dmC     Input  ->       WFS   Ratio  |    | hits/step    perc  |\n");
    nbl++;
    printw("\n");
    nbl++;

    for(k=0; k<AOconf[loop].DMmodesNBblock; k++)
    {
        if(k==0)
            kmin = 0;
        else
            kmin = AOconf[loop].indexmaxMB[k-1];

        attron(A_BOLD);
        printw("%3ld", k);
        attroff(A_BOLD);

        printw("    %4ld [ %4ld - %4ld ]   %5.3f  %7.5f  %5.3f", AOconf[loop].NBmodes_block[k], kmin, AOconf[loop].indexmaxMB[k]-1, data.image[aoconfID_gainb].array.F[k], data.image[aoconfID_limitb].array.F[k], data.image[aoconfID_multfb].array.F[k]);
        printw("  |  %8.2f  %8.2f  ->  %8.2f", 1000.0*AOconf[loop].blockave_Crms[k], 1000.0*AOconf[loop].blockave_OLrms[k], 1000.0*AOconf[loop].blockave_WFSrms[k]);

        attron(A_BOLD);
        printw("   %5.3f  ", AOconf[loop].blockave_WFSrms[k]/AOconf[loop].blockave_OLrms[k]);
        attroff(A_BOLD);

        if( AOconf[loop].blockave_limFrac[k] > 0.01 )
            attron(A_BOLD | COLOR_PAIR(2));

        printw("| %2ld | %9.3f  %6.2f\% |\n", k, AOconf[loop].blockave_limFrac[k],  100.0*AOconf[loop].blockave_limFrac[k]/AOconf[loop].NBmodes_block[k]);
        attroff(A_BOLD | COLOR_PAIR(2));

        nbl++;
    }


    printw("\n");
    nbl++;

    printw(" ALL   %4ld                                        ", AOconf[loop].NBDMmodes);
    printw("  |  %8.2f  %8.2f  ->  %8.2f", 1000.0*AOconf[loop].ALLave_Crms, 1000.0*AOconf[loop].ALLave_OLrms, 1000.0*AOconf[loop].ALLave_WFSrms);

    attron(A_BOLD);
    printw("   %5.3f  ", AOconf[loop].ALLave_WFSrms/AOconf[loop].ALLave_OLrms);
    attroff(A_BOLD);

    printw("| %2ld | %9.3f  %6.2f\% |\n", k, AOconf[loop].ALLave_limFrac,  100.0*AOconf[loop].ALLave_limFrac/AOconf[loop].NBDMmodes);

    printw("\n");
    nbl++;

    //printw("            MODAL RMS (ALL MODES) : %6.4lf     AVERAGE :  %8.6lf       ( %20g / %8lld )\n", sqrt(AOconf[loop].RMSmodes), sqrt(AOconf[loop].RMSmodesCumul/AOconf[loop].RMSmodesCumulcnt), AOconf[loop].RMSmodesCumul, AOconf[loop].RMSmodesCumulcnt);


    print_header(" [ gain 1000xlimit  mult ] MODES [nm]    DM correction -- WFS value -- WFS average -- WFS RMS     ", '-');
    nbl++;




    if(kmax>AOconf[loop].NBDMmodes)
        kmax = AOconf[loop].NBDMmodes;

    col = 0;
    for(k=0; k<kmax; k++)
    {
        attron(A_BOLD);
        printw("%4ld ", k);
        attroff(A_BOLD);

        printw("[%5.3f %8.4f %5.3f] ", AOconf[loop].gain * data.image[aoconfID_gainb].array.F[data.image[IDblknb].array.UI16[k]] * data.image[aoconfID_GAIN_modes].array.F[k], 1000.0 * data.image[aoconfID_limitb].array.F[data.image[IDblknb].array.UI16[k]] * data.image[aoconfID_LIMIT_modes].array.F[k], AOconf[loop].mult * data.image[aoconfID_multfb].array.F[data.image[IDblknb].array.UI16[k]] * data.image[aoconfID_MULTF_modes].array.F[k]);

        // print current value on DM
        val = data.image[IDmodeval_dm].array.F[k];
        if(fabs(val)>0.99*AOconf[loop].maxlimit)
        {
            attron(A_BOLD | COLOR_PAIR(2));
            printw("%+8.3f ", 1000.0*val);
            attroff(A_BOLD | COLOR_PAIR(2));
        }
        else
        {
            if(fabs(val)>0.99*AOconf[loop].maxlimit*data.image[aoconfID_LIMIT_modes].array.F[k])
            {
                attron(COLOR_PAIR(1));
                printw("%+8.3f ", 1000.0*val);
                attroff(COLOR_PAIR(1));
            }
            else
                printw("%+8.3f ", 1000.0*val);
        }

        // last reading from WFS
        printw("%+8.3f ", 1000.0*data.image[IDmodeval].array.F[k]);


        // Time average
        val = data.image[IDmodevalave].array.F[(ksize-1)*AOconf[loop].NBDMmodes+k];
        if(fabs(val)>AVElim)
        {
            attron(A_BOLD | COLOR_PAIR(2));
            printw("%+8.3f ", 1000.0*val);
            attroff(A_BOLD | COLOR_PAIR(2));
        }
        else
            printw("%+8.3f ", 1000.0*val);


        // RMS variation
        val = sqrt(data.image[IDmodevalrms].array.F[(ksize-1)*AOconf[loop].NBDMmodes+k]);
        if(fabs(val)>RMSlim)
        {
            attron(A_BOLD | COLOR_PAIR(2));
            printw("%8.3f ", 1000.0*val);
            attroff(A_BOLD | COLOR_PAIR(2));
        }
        else
            printw("%8.3f ", 1000.0*val);

        col++;
        if(col==nbcol)
        {
            col = 0;
            printw("\n");
        }
        else
            printw(" | ");
    }

    return(0);
}


int_fast8_t AOloopControl_loopMonitor(long loop, double frequ, long nbcol)
{
    char name[200];
    // DM mode values
    long IDmodeval_dm;

    // WFS modes values
    long IDmodeval;
    long ksize;
    long IDmodevalave;
    long IDmodevalrms;
    char fname[200];


    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    printf("MEMORY HAS BEEN INITIALIZED\n");
    fflush(stdout);

    // load arrays that are required
    if(aoconfID_cmd_modes==-1)
    {
        if(sprintf(name, "aol%ld_DMmode_cmd", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_cmd_modes = read_sharedmem_image(name);
    }

    if(aoconfID_meas_modes==-1)
    {
        if(sprintf(name, "aol%ld_DMmode_meas", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_meas_modes = read_sharedmem_image(name);
    }


    if(aoconfID_RMS_modes==-1)
    {
        if(sprintf(name, "aol%ld_DMmode_RMS", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_RMS_modes = read_sharedmem_image(name);
    }

    if(aoconfID_AVE_modes==-1)
    {
        if(sprintf(name, "aol%ld_DMmode_AVE", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_AVE_modes = read_sharedmem_image(name);
    }


    // blocks
    if(aoconfID_gainb == -1)
    {
        if(sprintf(name, "aol%ld_gainb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_gainb = read_sharedmem_image(name);
    }

    if(aoconfID_multfb == -1)
    {
        if(sprintf(name, "aol%ld_multfb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_multfb = read_sharedmem_image(name);
    }

    if(aoconfID_limitb == -1)
    {
        if(sprintf(name, "aol%ld_limitb", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_limitb = read_sharedmem_image(name);
    }


    // individual modes

    if(aoconfID_GAIN_modes==-1)
    {
        if(sprintf(name, "aol%ld_DMmode_GAIN", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_GAIN_modes = read_sharedmem_image(name);
    }

    if(aoconfID_LIMIT_modes==-1)
    {
        if(sprintf(name, "aol%ld_DMmode_LIMIT", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_LIMIT_modes = read_sharedmem_image(name);
    }

    if(aoconfID_MULTF_modes==-1)
    {
        if(sprintf(name, "aol%ld_DMmode_MULTF", loop) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_MULTF_modes = read_sharedmem_image(name);
    }








    // real-time DM mode value

    if(sprintf(fname, "aol%ld_modeval_dm_now", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodeval_dm = read_sharedmem_image(fname);

    // real-time WFS mode value
    if(sprintf(fname, "aol%ld_modeval", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodeval = read_sharedmem_image(fname);

    // averaged WFS residual modes, computed by CUDACOMP_extractModesLoop
    if(sprintf(fname, "aol%ld_modeval_ave", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodevalave = read_sharedmem_image(fname);
    ksize = data.image[IDmodevalave].md[0].size[1]; // number of averaging line, each line is 2x averaged of previous line

    // averaged WFS residual modes RMS, computed by CUDACOMP_extractModesLoop
    if(sprintf(fname, "aol%ld_modeval_rms", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodevalrms = read_sharedmem_image(fname);




    initscr();
    getmaxyx(stdscr, wrow, wcol);


    start_color();
    init_pair(1, COLOR_BLUE, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_GREEN, COLOR_BLACK);
    init_pair(4, COLOR_RED, COLOR_BLACK);

    while( !kbdhit() )
    {
        usleep((long) (1000000.0/frequ));
        clear();
        attron(A_BOLD);
        print_header(" PRESS ANY KEY TO STOP MONITOR ", '-');
        attroff(A_BOLD);

        AOloopControl_printloopstatus(loop, nbcol, IDmodeval_dm, IDmodeval, IDmodevalave, IDmodevalrms, ksize);

        refresh();
    }
    endwin();

    return 0;
}


// if updateconf=1, update configuration
int_fast8_t AOloopControl_statusStats(int updateconf)
{
    long k;
    long NBkiter = 100000;
    long statusmax = 21;
    long *statuscnt;
    long *statusMcnt;
    float usec0, usec1;
    int st, stM;
    int RT_priority = 91; //any number from 0-99
    struct sched_param schedpar;
    const char *statusdef[21];
    const char *statusMdef[21];
    int gpu;
    int nbgpu;
    struct timespec t1;
    struct timespec t2;
    struct timespec tdiff;
    double tdiffv;
    long *statusgpucnt;
    long *statusgpucnt2;
    double loopiterus;

    long long loopcnt;
    char imname[200];
    long long wfsimcnt;
    long long dmCcnt;

    int ret;


    float loopfrequ_measured, complatency_measured, wfsmextrlatency_measured;
    float complatency_frame_measured, wfsmextrlatency_frame_measured;


    FILE *fp;

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    statusdef[0] = "LOAD IMAGE";
    statusdef[1] = "DARK SUBTRACT";
    statusdef[2] = "COMPUTE WFS IMAGE TOTAL";
    statusdef[3] = "NORMALIZE WFS IMAGE";
    statusdef[4] = "SUBTRACT REFERENCE";
    statusdef[5] = "MULTIPLYING BY CONTROL MATRIX -> MODE VALUES : SETUP";
    statusdef[6] = "START CONTROL MATRIX MULTIPLICATION: CHECK IF NEW CM EXISTS";
    statusdef[7] = "CONTROL MATRIX MULT: CREATE COMPUTING THREADS";
    statusdef[8] = "CONTROL MATRIX MULT: WAIT FOR THREADS TO COMPLETE";
    statusdef[9] = "CONTROL MATRIX MULT: COMBINE TRHEADS RESULTS";
    statusdef[10] = "CONTROL MATRIX MULT: INCREMENT COUNTER AND EXIT FUNCTION";
    statusdef[11] = "MULTIPLYING BY GAINS";

    if(AOconf[LOOPNUMBER].CMMODE==0)
    {
        statusdef[12] = "ENTER SET DM MODES";
        statusdef[13] = "START DM MODES MATRIX MULTIPLICATION";
        statusdef[14] = "MATRIX MULT: CREATE COMPUTING THREADS";
        statusdef[15] = "MATRIX MULT: WAIT FOR THREADS TO COMPLETE";
        statusdef[16] = "MATRIX MULT: COMBINE TRHEADS RESULTS";
        statusdef[17] = "MATRIX MULT: INCREMENT COUNTER AND EXIT FUNCTION";
    }
    else
    {
        statusdef[12] = "REMOVE NAN VALUES";
        statusdef[13] = "ENFORCE STROKE LIMITS";
        statusdef[14] = "-";
        statusdef[15] = "-";
        statusdef[16] = "-";
        statusdef[17] = "-";
    }

    statusdef[18] = "LOG DATA";
    statusdef[19] = "READING IMAGE";
    statusdef[20] = "WAIT FOR IMAGE";




    statusMdef[0] = "";
    statusMdef[1] = "";
    statusMdef[2] = "EXTRACT WFS MODES";
    statusMdef[3] = "UPDATE CURRENT DM STATE";
    statusMdef[4] = "MIX PREDICTION WITH CURRENT DM STATE";
    statusMdef[5] = "MODAL FILTERING / CLIPPING";
    statusMdef[6] = "INTER-PROCESS LATENCY";
    statusMdef[7] = "";
    statusMdef[8] = "";
    statusMdef[9] = "";
    statusMdef[10] = "MODES TO DM ACTUATORS (GPU)";
    statusMdef[11] = "";
    statusMdef[12] = "";
    statusMdef[13] = "";
    statusMdef[14] = "";
    statusMdef[15] = "";
    statusMdef[16] = "";
    statusMdef[17] = "";
    statusMdef[18] = "";
    statusMdef[19] = "";
    statusMdef[20] = "WAIT FOR IMAGE imWFS0";







    usec0 = 50.0;
    usec1 = 150.0;



    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    sched_setscheduler(0, SCHED_FIFO, &schedpar);
#endif

    nbgpu = AOconf[LOOPNUMBER].GPU0;


    printf("Measuring loop status distribution \n");
    fflush(stdout);

    statuscnt = (long*) malloc(sizeof(long)*statusmax);
    statusMcnt = (long*) malloc(sizeof(long)*statusmax);
    statusgpucnt = (long*) malloc(sizeof(long)*nbgpu*10);
    statusgpucnt2 = (long*) malloc(sizeof(long)*nbgpu*10);


    for(st=0; st<statusmax; st++)
    {
        statuscnt[st] = 0;
        statusMcnt[st] = 0;
    }

    for(st=0; st<nbgpu*10; st++)
    {
        statusgpucnt[st] = 0;
        statusgpucnt2[st] = 0;
    }


    if(sprintf(imname, "aol%ld_wfsim", LOOPNUMBER) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    aoconfID_wfsim = read_sharedmem_image(imname);

    if(sprintf(imname, "aol%ld_dmC", LOOPNUMBER) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    aoconfID_dmC = read_sharedmem_image(imname);


    wfsimcnt = data.image[aoconfID_wfsim].md[0].cnt0;
    dmCcnt = data.image[aoconfID_dmC].md[0].cnt0;

    loopcnt = AOconf[LOOPNUMBER].cnt;
    clock_gettime(CLOCK_REALTIME, &t1);
    for(k=0; k<NBkiter; k++)
    {
        usleep((long) (usec0 + usec1*(1.0*k/NBkiter)));
        st = AOconf[LOOPNUMBER].status;
        stM = AOconf[LOOPNUMBER].statusM;
        if(st<statusmax)
            statuscnt[st]++;
        if(stM<statusmax)
            statusMcnt[stM]++;
        for(gpu=0; gpu<AOconf[LOOPNUMBER].GPU0; gpu++)
        {
            // 1st matrix mult
            st = 10*gpu + AOconf[LOOPNUMBER].GPUstatus[gpu];
            statusgpucnt[st]++;

            // 2nd matrix mult
            st = 10*gpu + AOconf[LOOPNUMBER].GPUstatus[10+gpu];
            statusgpucnt2[st]++;
        }
    }
    loopcnt = AOconf[LOOPNUMBER].cnt - loopcnt;
    wfsimcnt = data.image[aoconfID_wfsim].md[0].cnt0 - wfsimcnt;
    dmCcnt = data.image[aoconfID_dmC].md[0].cnt0 - dmCcnt;

    clock_gettime(CLOCK_REALTIME, &t2);
    tdiff = info_time_diff(t1, t2);
    tdiffv = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
    printf("\n");
    loopiterus = 1.0e6*tdiffv/loopcnt;
    printf("Time diff = %f sec \n", tdiffv);
    printf("Loop freq = %8.2f Hz   -> single interation = %8.3f us\n", 1.0*loopcnt/tdiffv, loopiterus);
    printf("Number of iterations    loop: %10lld   wfs: %lld   dmC : %lld\n", loopcnt, wfsimcnt, dmCcnt);
    printf("MISSED FRAMES = %lld    fraction = %7.4f %%\n", wfsimcnt-loopcnt, 100.0*(wfsimcnt-loopcnt)/wfsimcnt);

    printf("\n");



    loopfrequ_measured = 1.0*loopcnt/tdiffv;
    if(updateconf==1)
        AOconf[LOOPNUMBER].loopfrequ = loopfrequ_measured;

    complatency_frame_measured = 1.0-1.0*statuscnt[20]/NBkiter;
    if(updateconf==1)
        AOconf[LOOPNUMBER].complatency_frame = complatency_frame_measured;

    complatency_measured = complatency_frame_measured/loopfrequ_measured;
    if(updateconf==1)
        AOconf[LOOPNUMBER].complatency = complatency_measured;



    wfsmextrlatency_frame_measured = 1.0-1.0*statusMcnt[20]/NBkiter;
    printf("==========> %ld %ld -> %f\n", statusMcnt[20], NBkiter, wfsmextrlatency_frame_measured);
    if(updateconf==1)
        AOconf[LOOPNUMBER].wfsmextrlatency_frame = wfsmextrlatency_frame_measured;

    wfsmextrlatency_measured = wfsmextrlatency_frame_measured / loopfrequ_measured;
    if(updateconf==1)
        AOconf[LOOPNUMBER].wfsmextrlatency = wfsmextrlatency_measured;

    if(updateconf==1)
    {
        fp = fopen("conf/param_loopfrequ.txt", "w");
        fprintf(fp, "%8.3f", AOconf[LOOPNUMBER].loopfrequ);
        fclose(fp);
    }

    if((fp=fopen("./conf/param_hardwlatency.txt", "r"))==NULL)
    {
        printf("WARNING: file ./conf/param_hardwlatency.txt missing\n");
    }
    else
    {
        if(fscanf(fp, "%50f", &AOconf[LOOPNUMBER].hardwlatency) != 1)
            printERROR(__FILE__, __func__, __LINE__, "Cannot read parameter from file");

        printf("hardware latency = %f\n", AOconf[LOOPNUMBER].hardwlatency);
        fclose(fp);
        fflush(stdout);
    }

    printf("hardwlatency = %f\n", AOconf[LOOPNUMBER].hardwlatency);
    if(updateconf==1)
    {
        AOconf[LOOPNUMBER].hardwlatency_frame = AOconf[LOOPNUMBER].hardwlatency * AOconf[LOOPNUMBER].loopfrequ;

        fp = fopen("conf/param_hardwlatency_frame.txt", "w");
        fprintf(fp, "%8.3f", AOconf[LOOPNUMBER].hardwlatency_frame);
        fclose(fp);

        fp = fopen("conf/param_complatency.txt", "w");
        fprintf(fp, "%8.6f", AOconf[LOOPNUMBER].complatency);
        fclose(fp);

        fp = fopen("conf/param_complatency_frame.txt", "w");
        fprintf(fp, "%8.3f", AOconf[LOOPNUMBER].complatency_frame);
        fclose(fp);

        fp = fopen("conf/param_wfsmextrlatency.txt", "w");
        fprintf(fp, "%8.6f", AOconf[LOOPNUMBER].wfsmextrlatency);
        fclose(fp);

        fp = fopen("conf/param_wfsmextrlatency_frame.txt", "w");
        fprintf(fp, "%8.3f", AOconf[LOOPNUMBER].wfsmextrlatency_frame);
        fclose(fp);
    }



    for(st=0; st<statusmax; st++)
        printf("STATUS %2d     %5.2f %%    [   %6ld  /  %6ld  ]   [ %9.3f us] %s\n", st, 100.0*statuscnt[st]/NBkiter, statuscnt[st], NBkiter, loopiterus*statuscnt[st]/NBkiter , statusdef[st]);




    if(AOconf[LOOPNUMBER].GPU0!=0)
    {
        printf("\n");
        printf("          ----1--------2--------3--------4--------5--------6----\n");
        printf("                   wait im | ->GPU |     COMPUTE     |   ->CPU  \n");
        printf("          ------------------------------------------------------\n");

        for(gpu=0; gpu<AOconf[LOOPNUMBER].GPU0; gpu++)
        {
            printf("GPU %2d  : ", gpu);
            printf("  %5.2f %%",  100.0*statusgpucnt[10*gpu+1]/NBkiter);
            printf("  %5.2f %%",  100.0*statusgpucnt[10*gpu+2]/NBkiter);
            printf("  %5.2f %%",  100.0*statusgpucnt[10*gpu+3]/NBkiter);
            printf("  %5.2f %%",  100.0*statusgpucnt[10*gpu+4]/NBkiter);
            printf("  %5.2f %%",   100.0*statusgpucnt[10*gpu+5]/NBkiter);
            printf("  %5.2f %%\n",  100.0*statusgpucnt[10*gpu+6]/NBkiter);
        }
        for(gpu=0; gpu<AOconf[LOOPNUMBER].GPU0; gpu++)
        {
            printf("GPU %2d  : ", gpu);
            printf(" %5.2f us",  loopiterus*statusgpucnt[10*gpu+1]/NBkiter);
            printf(" %5.2f us",  loopiterus*statusgpucnt[10*gpu+2]/NBkiter);
            printf(" %5.2f us",  loopiterus*statusgpucnt[10*gpu+3]/NBkiter);
            printf(" %5.2f us",  loopiterus*statusgpucnt[10*gpu+4]/NBkiter);
            printf(" %5.2f us",   loopiterus*statusgpucnt[10*gpu+5]/NBkiter);
            printf(" %5.2f us\n",  loopiterus*statusgpucnt[10*gpu+6]/NBkiter);
        }

        printf("\n");
        if(AOconf[LOOPNUMBER].CMMODE == 0)
        {
            printf("          ----1--------2--------3--------4--------5--------6----\n");
            for(gpu=0; gpu<AOconf[LOOPNUMBER].GPU0; gpu++)
            {
                printf("GPU %2d  : ", gpu);
                printf("  %5.2f %%",  100.0*statusgpucnt2[10*gpu+1]/NBkiter);
                printf("  %5.2f %%",  100.0*statusgpucnt2[10*gpu+2]/NBkiter);
                printf("  %5.2f %%",  100.0*statusgpucnt2[10*gpu+3]/NBkiter);
                printf("  %5.2f %%",  100.0*statusgpucnt2[10*gpu+4]/NBkiter);
                printf("  %5.2f %%",   100.0*statusgpucnt2[10*gpu+5]/NBkiter);
                printf("  %5.2f %%\n",  100.0*statusgpucnt2[10*gpu+6]/NBkiter);
            }
        }
    }



    for(st=0; st<statusmax; st++)
        if(strlen(statusMdef[st])>0)
            printf("STATUSM %2d     %5.2f %%    [   %6ld  /  %6ld  ]   [ %9.3f us] %s\n", st, 100.0*statusMcnt[st]/NBkiter, statusMcnt[st], NBkiter, loopiterus*statusMcnt[st]/NBkiter , statusMdef[st]);


    free(statuscnt);
    free(statusMcnt);
    free(statusgpucnt);
    free(statusgpucnt2);


    return 0;
}




int_fast8_t AOloopControl_resetRMSperf()
{
    long k;
    char name[200];
    long kmin, kmax;


    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    AOconf[LOOPNUMBER].RMSmodesCumul = 0.0;
    AOconf[LOOPNUMBER].RMSmodesCumulcnt = 0;


    return 0;
}






int_fast8_t AOloopControl_showparams(long loop)
{

    printf("loop number %ld\n", loop);

    if(AOconf[loop].on == 1)
        printf("loop is ON\n");
    else
        printf("loop is OFF\n");

    printf("Global gain = %f   maxlim = %f\n  multcoeff = %f  GPU = %d\n", AOconf[loop].gain, AOconf[loop].maxlimit, AOconf[loop].mult, AOconf[loop].GPU0);
    printf("    Predictive control state: %d        ARPF gain = %5.3f   AUTOTUNE: lim %d gain %d\n", AOconf[loop].ARPFon, AOconf[loop].ARPFgain, AOconf[loop].AUTOTUNE_LIMITS_ON,  AOconf[loop].AUTOTUNE_GAINS_ON);
    printf("WFS norm floor = %f\n", AOconf[loop].WFSnormfloor);

    printf("loopfrequ               =  %8.2f Hz\n", AOconf[loop].loopfrequ);
    printf("hardwlatency_frame      =  %8.2f fr\n", AOconf[loop].hardwlatency_frame);
    printf("complatency_frame       =  %8.2f fr\n", AOconf[loop].complatency_frame);
    printf("wfsmextrlatency_frame   =  %8.2f fr\n", AOconf[loop].wfsmextrlatency_frame);


    return 0;
}



int_fast8_t AOcontrolLoop_TestDMSpeed(const char *dmname, long delayus, long NBpts, float ampl)
{
    long IDdm;
    long dmxsize, dmysize, dmsize;
    long ii, jj, kk;
    long ID1;
    float pha;
    float x, y, x1;
    char *ptr;

    long IDdm0, IDdm1; // DM shapes



    IDdm = image_ID(dmname);
    dmxsize = data.image[IDdm].md[0].size[0];
    dmysize = data.image[IDdm].md[0].size[1];
    dmsize = dmxsize*dmysize;



    ID1 = create_3Dimage_ID("dmpokeseq", dmxsize, dmysize, NBpts);
    for(kk=0; kk<NBpts; kk++)
    {
        pha = 2.0*M_PI*kk/NBpts;
        for(ii=0; ii<dmxsize; ii++)
            for(jj=0; jj<dmysize; jj++)
            {
                x = (2.0*ii/dmxsize)-1.0;
                y = (2.0*jj/dmysize)-1.0;
                x1 = x*cos(pha)-y*sin(pha);
                data.image[ID1].array.F[kk*dmsize+jj*dmxsize+ii] = ampl*x1;
            }
    }

    while(1)
    {
        for(kk=0; kk<NBpts; kk++)
        {
            ptr = (char*) data.image[ID1].array.F;
            ptr += sizeof(float)*dmsize*kk;
            data.image[IDdm].md[0].write = 1;
            memcpy(data.image[IDdm].array.F, ptr, sizeof(float)*dmsize);
            data.image[IDdm].md[0].write = 0;
            data.image[IDdm].md[0].cnt0 ++;
            usleep(delayus);
        }
    }

    return(0);
}





/**
 *  ## Purpose
 * 
 * Measure hardware latency between DM and WFS streams
 * 
 * 
 * ## Arguments
 * 
 * @param[in]
 * dmname	char*
 * 			DM actuation stream to which function sends pokes
 * 
 * @param[in]
 * wfsname	char*
 * -		WFS image stream
 * 
 * @param[in]
 * OPDamp	FLOAT
 * 			Poke amplitude \[um\]
 * 
 * @param[in]
 * NBiter	LONG
 * 			Number of poke cycles
 * 
 */
int_fast8_t AOcontrolLoop_TestSystemLatency(const char *dmname, char *wfsname, float OPDamp, long NBiter)
{
    long IDdm;
    long dmxsize, dmysize, dmsize;
    long IDwfs;
    long wfsxsize, wfsysize, wfssize;
    long twait0us = 100000;

    double tdouble_start;
    double tdouble_end;
    long wfscntstart;
    long wfscntend;

    struct timespec tstart;
    struct timespec tnow;
    struct timespec *tarray;
    double tdouble, tlastdouble;
    double tstartdouble;
    double dtmax = 1.0;  // Max running time per iteration
    double dt, dt1;
    double *dtarray;
    double a, b;
    char command[200];
    long IDdm0, IDdm1; // DM shapes
    long ii, jj;
    float x, y;

    long IDwfsc;
    long wfs_NBframesmax = 20;
    long wfsframe;
    long NBwfsframe;
    long twaitus = 30000; // initial wait [us]
    double dtoffset0 = 0.002; // 2 ms
    long wfsframeoffset = 10;

    long IDwfsref;

    unsigned long wfscnt0;
    char *ptr;
    long kk, kkmax;
    double *valarray;
    double tmp;
    double dtoffset;
    long kkoffset;

    long iter;

    double latencymax = 0.0;
    float *latencyarray;
    float *latencysteparray;
    float latencyave, latencystepave;

    FILE *fp;
    int RT_priority = 80; //any number from 0-99
    struct sched_param schedpar;
    double latency;
    float minlatency, maxlatency;
    double wfsdt;

    uint8_t atype;
    uint32_t naxes[3];


    schedpar.sched_priority = RT_priority;
#ifndef __MACH__
    // r = seteuid(euid_called); //This goes up to maximum privileges
    sched_setscheduler(0, SCHED_FIFO, &schedpar); //other option is SCHED_RR, might be faster
    // r = seteuid(euid_real);//Go back to normal privileges
#endif

    latencyarray = (float*) malloc(sizeof(float)*NBiter);
    latencysteparray = (float*) malloc(sizeof(float)*NBiter);

    IDdm = image_ID(dmname);
    dmxsize = data.image[IDdm].md[0].size[0];
    dmysize = data.image[IDdm].md[0].size[1];
    dmsize = dmxsize*dmysize;

    IDdm0 = create_2Dimage_ID("_testdm0", dmxsize, dmysize);
    IDdm1 = create_2Dimage_ID("_testdm1", dmxsize, dmysize);
    for(ii=0; ii<dmxsize; ii++)
        for(jj=0; jj<dmysize; jj++)
        {
            x = (2.0*ii-1.0*dmxsize)/dmxsize;
            y = (2.0*jj-1.0*dmxsize)/dmysize;
            data.image[IDdm0].array.F[jj*dmxsize+ii] = 0.0;
            data.image[IDdm1].array.F[jj*dmxsize+ii] = OPDamp*(sin(8.0*x)+sin(8.0*y));
        }

    //system("mkdir -p tmp");
    //save_fits("_testdm0", "!tmp/_testdm0.fits");
    //save_fits("_testdm1", "!tmp/_testdm1.fits");

    IDwfs = image_ID(wfsname);
    wfsxsize = data.image[IDwfs].md[0].size[0];
    wfsysize = data.image[IDwfs].md[0].size[1];
    wfssize = wfsxsize*wfsysize;
    atype = data.image[IDwfs].md[0].atype;

    naxes[0] = wfsxsize;
    naxes[1] = wfsysize;
    naxes[2] = wfs_NBframesmax;
    IDwfsc = create_image_ID("_testwfsc", 3, naxes, atype, 0, 0);
    //    IDwfsc = create_3Dimage_ID("_testwfsc", wfsxsize, wfsysize, wfs_NBframesmax);


    // coarse estimage of frame rate
    clock_gettime(CLOCK_REALTIME, &tnow);
    tdouble_start = 1.0*tnow.tv_sec + 1.0e-9*tnow.tv_nsec;
    wfscntstart = data.image[IDwfs].md[0].cnt0;
    sleep(5.0);
    clock_gettime(CLOCK_REALTIME, &tnow);
    tdouble_end = 1.0*tnow.tv_sec + 1.0e-9*tnow.tv_nsec;
    wfscntend = data.image[IDwfs].md[0].cnt0;
    wfsdt = (tdouble_end - tdouble_start)/(wfscntend-wfscntstart);

    printf("wfs dt = %f sec\n", wfsdt);


    // update times
    dtmax = wfsdt*wfs_NBframesmax*1.2 + 0.5;
    twaitus = 1000000.0*wfsdt;
    dtoffset0 = 1.5*wfsdt;


    tarray = (struct timespec *) malloc(sizeof(struct timespec)*wfs_NBframesmax);
    dtarray = (double*) malloc(sizeof(double)*wfs_NBframesmax);

    if(system("mkdir -p timingstats") != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    if ((fp=fopen("timingstats/hardwlatency.txt", "w"))==NULL)
    {
        printf("ERROR: cannot create file \"timingstats/hardwlatency.txt\"\\n");
        exit(0);
    }

    clock_gettime(CLOCK_REALTIME, &tnow);
    tdouble_start = 1.0*tnow.tv_sec + 1.0e-9*tnow.tv_nsec;
    wfscntstart = data.image[IDwfs].md[0].cnt0;
    wfsframeoffset = (long) (0.3*wfs_NBframesmax);



    for(iter=0; iter<NBiter; iter++)
    {
        printf("ITERATION %5ld / %5ld\n", iter, NBiter);
        fflush(stdout);


        printf("write to %s\n", dmname);
        fflush(stdout);
        copy_image_ID("_testdm0", dmname, 1);

        unsigned int dmstate = 0;

        // waiting time
        usleep(twaitus);


        // and waiting frames
        wfscnt0 = data.image[IDwfs].md[0].cnt0;
        for(wfsframe=0; wfsframe<wfs_NBframesmax; wfsframe++)
        {
            while(wfscnt0==data.image[IDwfs].md[0].cnt0)
            {
                usleep(50);
            }
            wfscnt0 = data.image[IDwfs].md[0].cnt0;
        }

        dt = 0.0;
        clock_gettime(CLOCK_REALTIME, &tstart);
        tstartdouble = 1.0*tstart.tv_sec + 1.0e-9*tstart.tv_nsec;
        tlastdouble = tstartdouble;



        wfsframe = 0;
        wfscnt0 = data.image[IDwfs].md[0].cnt0;
        printf("\n");
        while( (dt < dtmax) && (wfsframe<wfs_NBframesmax) )
        {
            // WAITING for image
            while(wfscnt0==data.image[IDwfs].md[0].cnt0)
            {
                usleep(10);
            }
            wfscnt0 = data.image[IDwfs].md[0].cnt0;
            printf("[%8ld]  %f  %f\n", wfsframe, dt, dtmax);
            fflush(stdout);

            if(atype == _DATATYPE_FLOAT)
            {
                // copy image to cube slice
                ptr = (char*) data.image[IDwfsc].array.F;
                ptr += sizeof(float)*wfsframe*wfssize;
                memcpy(ptr, data.image[IDwfs].array.F, sizeof(float)*wfssize);
            }

            if(atype == _DATATYPE_UINT16)
            {
                // copy image to cube slice
                ptr = (char*) data.image[IDwfsc].array.UI16;
                ptr += sizeof(short)*wfsframe*wfssize;
                memcpy(ptr, data.image[IDwfs].array.UI16, sizeof(short)*wfssize);
            }

            clock_gettime(CLOCK_REALTIME, &tarray[wfsframe]);

            tdouble = 1.0*tarray[wfsframe].tv_sec + 1.0e-9*tarray[wfsframe].tv_nsec;
            dt = tdouble - tstartdouble;
            //  dt1 = tdouble - tlastdouble;
            dtarray[wfsframe] = dt;
            tlastdouble = tdouble;

            // apply DM pattern #1
            if((dmstate==0)&&(dt>dtoffset0)&&(wfsframe>wfsframeoffset))
            {
                usleep((long) (ran1()*1000000.0*wfsdt));
                printf("\nDM STATE CHANGED ON ITERATION %ld\n\n", wfsframe);
                kkoffset = wfsframe;
                dmstate = 1;
                copy_image_ID("_testdm1", dmname, 1);

                clock_gettime(CLOCK_REALTIME, &tnow);
                tdouble = 1.0*tnow.tv_sec + 1.0e-9*tnow.tv_nsec;
                dt = tdouble - tstartdouble;
                dtoffset = dt; // time at which DM command is sent
            }
            wfsframe++;
        }
        printf("\n\n %ld frames recorded\n", wfsframe);
        fflush(stdout);
        copy_image_ID("_testdm0", dmname, 1);
        dmstate = 0;


        // Computing difference between consecutive images
        NBwfsframe = wfsframe;
        valarray = (double*) malloc(sizeof(double)*NBwfsframe);
        double valmax = 0.0;
        double valmaxdt = 0.0;
        for(kk=1; kk<NBwfsframe; kk++)
        {
            valarray[kk] = 0.0;
            if(atype == _DATATYPE_FLOAT)
                for(ii=0; ii<wfssize; ii++)
                {
                    tmp = data.image[IDwfsc].array.F[kk*wfssize+ii] - data.image[IDwfsc].array.F[(kk-1)*wfssize+ii];
                    valarray[kk] += tmp*tmp;
                }
            if(atype == _DATATYPE_UINT16)
                for(ii=0; ii<wfssize; ii++)
                {
                    tmp = data.image[IDwfsc].array.UI16[kk*wfssize+ii] - data.image[IDwfsc].array.UI16[(kk-1)*wfssize+ii];
                    valarray[kk] += 1.0*tmp*tmp;
                }
            if(valarray[kk]>valmax)
            {
                valmax = valarray[kk];
                valmaxdt = 0.5*(dtarray[kk-1]+dtarray[kk]);
                kkmax = kk-kkoffset;
            }
        }


        //
        //
        //
        for(wfsframe=1; wfsframe<NBwfsframe; wfsframe++)
            fprintf(fp, "%ld   %10.2f     %g\n", wfsframe-kkoffset, 1.0e6*(0.5*(dtarray[wfsframe]+dtarray[wfsframe-1])-dtoffset), valarray[wfsframe]);

        printf("mean interval =  %10.2f ns\n", 1.0e9*(dt-dtoffset)/NBwfsframe);
        fflush(stdout);

        free(valarray);

        latency = valmaxdt-dtoffset;
        // latencystep = kkmax;

        printf("Hardware latency = %f ms  = %ld frames\n", 1000.0*latency, kkmax);
        if(latency > latencymax)
        {
            latencymax = latency;
            save_fits("_testwfsc", "!./timingstats/maxlatencyseq.fits");
        }
        fprintf(fp, "# %5ld  %8.6f\n", iter, (valmaxdt-dtoffset));
        latencysteparray[iter] = 1.0*kkmax;
        latencyarray[iter] = (valmaxdt-dtoffset);
    }
    fclose(fp);

    clock_gettime(CLOCK_REALTIME, &tnow);
    tdouble_end = 1.0*tnow.tv_sec + 1.0e-9*tnow.tv_nsec;
    wfscntend = data.image[IDwfs].md[0].cnt0;



    free(dtarray);
    free(tarray);

    latencyave = 0.0;
    latencystepave = 0.0;
    minlatency = latencyarray[0];
    maxlatency = latencyarray[0];
    for(iter=0; iter<NBiter; iter++)
    {
        if(latencyarray[iter]>maxlatency)
            maxlatency = latencyarray[iter];

        if(latencyarray[iter]<minlatency)
            minlatency = latencyarray[iter];

        latencyave += latencyarray[iter];
        latencystepave += latencysteparray[iter];
    }
    latencyave /= NBiter;
    latencystepave /= NBiter;

    quick_sort_float(latencyarray, NBiter);

    printf("AVERAGE LATENCY = %8.3f ms   %f frames\n", latencyave*1000.0, latencystepave);
    printf("min / max over %ld measurements: %8.3f ms / %8.3f ms\n", NBiter, minlatency*1000.0, maxlatency*1000.0);

    if(sprintf(command, "echo %8.6f > conf/param_hardwlatency.txt", latencyarray[NBiter/2]) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if(system(command) != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    if(sprintf(command, "echo %f %f %f %f %f > timinstats/hardwlatencyStats.txt", latencyarray[NBiter/2], latencyave, minlatency, maxlatency, latencystepave) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if(system(command) != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");


    dt = tdouble_end - tdouble_start;
    printf("FRAME RATE = %.3f Hz\n", 1.0*(wfscntend-wfscntstart)/dt);

    if(sprintf(command, "echo %.3f > conf/param_loopfrequ.txt", 1.0*(wfscntend-wfscntstart)/dt ) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if(system(command) != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    free(latencyarray);
    free(latencysteparray);

    return 0;
}




// waits on semaphore 3

long AOloopControl_blockstats(long loop, const char *IDout_name)
{
    long IDout;
    uint32_t *sizeout;
    long NBmodes;
    char fname[200];
    long IDmodeval, ID;
    long m, blk, n, i;
    long cnt;
    long IDblockRMS, IDblockRMS_ave;
    long NBblock;

    float *rmsarray;
    int *indexarray;

    float alpha = 0.0001;


    if(sprintf(fname, "aol%ld_modeval", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDmodeval = read_sharedmem_image(fname);
    NBmodes = data.image[IDmodeval].md[0].size[0];

    sizeout = (uint32_t*) malloc(sizeof(uint32_t)*2);
    sizeout[0] = NBmodes;
    sizeout[1] = 1;
    IDout = create_image_ID(IDout_name, 2, sizeout, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(IDout_name, 10);

    printf("%ld modes\n", NBmodes);


    m = 0;
    blk = 0;
    while(m<NBmodes)
    {
        if(sprintf(fname, "aol%ld_DMmodes%02ld", loop, blk) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        ID = read_sharedmem_image(fname);
        n = data.image[ID].md[0].size[2];
        for(i=0; i<n; i++)
        {
            data.image[IDout].array.F[m] = blk;
            m++;
        }
        blk++;
    }
    NBblock = blk;

    rmsarray = (float*) malloc(sizeof(float)*NBblock);


    indexarray = (int*) malloc(sizeof(int)*NBmodes);
    for(m=0; m<NBmodes; m++)
        indexarray[m] = (int) (0.1 + data.image[IDout].array.F[m]);


    printf("NBblock = %ld\n", NBblock);
    sizeout[0] = NBblock;
    sizeout[1] = 1;

    if(sprintf(fname, "aol%ld_blockRMS", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDblockRMS = create_image_ID(fname, 2, sizeout, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(fname, 10);

    if(sprintf(fname, "aol%ld_blockRMS_ave", loop) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    IDblockRMS_ave = create_image_ID(fname, 2, sizeout, _DATATYPE_FLOAT, 1, 0);
    COREMOD_MEMORY_image_set_createsem(fname, 10);


    cnt =  0;
    for(;;)
    {
        if(data.image[IDmodeval].md[0].sem==0)
        {
            while(cnt==data.image[IDmodeval].md[0].cnt0) // test if new frame exists
                usleep(5);
            cnt = data.image[IDmodeval].md[0].cnt0;
        }
        else
            sem_wait(data.image[IDmodeval].semptr[3]);

        for(blk=0; blk<NBblock; blk++)
            rmsarray[blk] = 0.0;

        for(m=0; m<NBmodes; m++)
            rmsarray[indexarray[m]] += data.image[IDmodeval].array.F[m]*data.image[IDmodeval].array.F[m];

        data.image[IDblockRMS].md[0].write = 1;
        for(blk=0; blk<NBblock; blk++)
            data.image[IDblockRMS].array.F[blk] = rmsarray[blk];
        COREMOD_MEMORY_image_set_sempost_byID(IDblockRMS, -1);
        data.image[IDblockRMS].md[0].cnt0++;
        data.image[IDblockRMS].md[0].write = 0;

        data.image[IDblockRMS_ave].md[0].write = 1;
        for(blk=0; blk<NBblock; blk++)
            data.image[IDblockRMS_ave].array.F[blk] = (1.0-alpha)* data.image[IDblockRMS_ave].array.F[blk] + alpha*rmsarray[blk];
        COREMOD_MEMORY_image_set_sempost_byID(IDblockRMS_ave, -1);
        data.image[IDblockRMS_ave].md[0].cnt0++;
        data.image[IDblockRMS_ave].md[0].write = 0;

    }

    free(sizeout);
    free(rmsarray);
    free(indexarray);


    return(IDout);
}


int_fast8_t AOloopControl_InjectMode( long index, float ampl )
{
    long i;
    char name[200];


    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_DMmodes==-1)
    {
        if(sprintf(name, "aol%ld_DMmodes", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_DMmodes = read_sharedmem_image(name);
    }

    if(aoconfID_dmRM==-1)
        aoconfID_dmRM = read_sharedmem_image(AOconf[LOOPNUMBER].dmRMname);


    if((index<0)||(index>AOconf[LOOPNUMBER].NBDMmodes-1))
    {
        printf("Invalid mode index... must be between 0 and %ld\n", AOconf[LOOPNUMBER].NBDMmodes);
    }
    else
    {
        float *arrayf;

        arrayf = (float*) malloc(sizeof(float)*AOconf[LOOPNUMBER].sizeDM);

        for(i=0; i<AOconf[LOOPNUMBER].sizeDM; i++)
            arrayf[i] = ampl*data.image[aoconfID_DMmodes].array.F[index*AOconf[LOOPNUMBER].sizeDM+i];



        data.image[aoconfID_dmRM].md[0].write = 1;
        memcpy (data.image[aoconfID_dmRM].array.F, arrayf, sizeof(float)*AOconf[LOOPNUMBER].sizeDM);
        data.image[aoconfID_dmRM].md[0].cnt0++;
        data.image[aoconfID_dmRM].md[0].write = 0;

        free(arrayf);
        AOconf[LOOPNUMBER].DMupdatecnt ++;
    }

    return(0);
}


//
// Measures mode temporal response (measurement and rejection)
//
long AOloopControl_TestDMmodeResp(const char *DMmodes_name, long index, float ampl, float fmin, float fmax, float fmultstep, float avetime, long dtus, const char *DMmask_name, const char *DMstream_in_name, const char *DMstream_out_name, const char *IDout_name)
{
    long IDout;
    long IDmodes, IDdmmask, IDdmin, IDdmout;
    long dmxsize, dmysize, dmsize, NBmodes;
    float f;
    struct timespec tstart;
    long nbf;
    float runtime;
    long IDrec_dmout;
    long ii, k, kk, kmax;
    long IDdmtmp;
    float pha, coeff;
    float *timearray;
    char *ptr;
    long k1;
    long IDcoeff;
    float SVDeps = 1.0e-3;
    int SVDreuse = 0;
    long IDcoeffarray;
    long m;
    float coscoeff, sincoeff;
    float PSDamp, PSDpha;
    FILE *fp;
    char fname[200];
    long kmaxmax = 100000;
    long ID;


    kk = index;

    IDmodes = image_ID(DMmodes_name);
    IDdmin = image_ID(DMstream_in_name);
    IDdmout = image_ID(DMstream_out_name);
    IDdmmask = image_ID(DMmask_name);

    dmxsize = data.image[IDmodes].md[0].size[0];
    dmysize = data.image[IDmodes].md[0].size[1];
    dmsize = dmxsize*dmysize;
    NBmodes = data.image[IDmodes].md[0].size[2];


    if(data.image[IDdmin].md[0].size[0]!=data.image[IDmodes].md[0].size[0])
    {
        printf("ERROR: x size of \"%s\"  (%ld) does not match x size of \"%s\" (%ld)\n", DMstream_in_name, (long) data.image[IDdmin].md[0].size[0], DMmodes_name, (long) data.image[IDmodes].md[0].size[0]);
        exit(0);
    }

    if(data.image[IDdmin].md[0].size[1]!=data.image[IDmodes].md[0].size[1])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match y size of \"%s\" (%ld)\n", DMstream_in_name, (long) data.image[IDdmin].md[0].size[1], DMmodes_name, (long) data.image[IDmodes].md[0].size[1]);
        exit(0);
    }

    if(data.image[IDdmout].md[0].size[0]!=data.image[IDmodes].md[0].size[0])
    {
        printf("ERROR: x size of \"%s\"  (%ld) does not match x size of \"%s\" (%ld)\n", DMstream_out_name, (long) data.image[IDdmout].md[0].size[0], DMmodes_name, (long) data.image[IDmodes].md[0].size[0]);
        exit(0);
    }

    if(data.image[IDdmout].md[0].size[1]!=data.image[IDmodes].md[0].size[1])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match y size of \"%s\" (%ld)\n", DMstream_out_name, (long) data.image[IDdmout].md[0].size[1], DMmodes_name, (long) data.image[IDmodes].md[0].size[1]);
        exit(0);
    }

    if(data.image[IDdmmask].md[0].size[0]!=data.image[IDmodes].md[0].size[0])
    {
        printf("ERROR: x size of \"%s\"  (%ld) does not match x size of \"%s\" (%ld)\n", DMmask_name, (long) data.image[IDdmmask].md[0].size[0], DMmodes_name, (long) data.image[IDmodes].md[0].size[0]);
        exit(0);
    }

    if(data.image[IDdmmask].md[0].size[1]!=data.image[IDmodes].md[0].size[1])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match y size of \"%s\" (%ld)\n", DMmask_name, (long) data.image[IDdmmask].md[0].size[1], DMmodes_name, (long) data.image[IDmodes].md[0].size[1]);
        exit(0);
    }



    nbf = 0;
    for(f=fmin; f<fmax; f*=fmultstep)
        nbf++;



    // TEST
    // Save DM mode
    ID = create_2Dimage_ID("testmrespm", dmxsize, dmysize);
    for(ii=0; ii<dmsize; ii++)
        data.image[ID].array.F[ii] = data.image[IDmodes].array.F[kk*dmsize+ii];
    save_fits("testmrespm", "!testmrespm.fits");


    // SET UP RECORDING CUBES
    kmax = (long) (avetime/(1.0e-6*dtus));
    if(kmax>kmaxmax)
        kmax = kmaxmax;

    timearray = (float*) malloc(sizeof(float)*kmax);
    IDrec_dmout = create_3Dimage_ID("_tmprecdmout", dmxsize, dmysize, kmax);

    IDcoeffarray = create_2Dimage_ID("_tmpcoeffarray", kmax, NBmodes);

    if(sprintf(fname, "mode%03ld_PSD.txt", kk) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if( (fp = fopen(fname, "w"))==NULL)
    {
        printf("ERROR: cannot create file \"%s\"", fname);
        exit(0);
    }

    IDout = create_2Dimage_ID(IDout_name, nbf, NBmodes);
    IDdmtmp = create_2Dimage_ID("_tmpdm", dmxsize, dmysize);

    for(f=fmin; f<fmax; f*=fmultstep)
    {
        runtime = 0.0;
        clock_gettime(CLOCK_REALTIME, &tstart);
        k = 0;
        while((runtime < avetime)&&(k<kmax))
        {
            clock_gettime(CLOCK_REALTIME, &tnow);
            tdiff = info_time_diff(tstart, tnow);
            runtime = 1.0*tdiff.tv_sec + 1.0e-9*tdiff.tv_nsec;
            pha = 2.0*M_PI*runtime*f;
            coeff = ampl*cos(pha);

            printf("mode %4ld  f = %f ( %f -> %f)   runtime = %10.3f sec    ampl = %f   pha = %f   coeff = %f\n", kk, f, fmin, fmax, runtime, ampl, pha, coeff);
            fflush(stdout);

            // APPLY MODE TO DM
            data.image[IDdmin].md[0].write = 1;
            for(ii=0; ii<dmsize; ii++)
                data.image[IDdmin].array.F[ii] = coeff*data.image[IDmodes].array.F[kk*dmsize+ii];
            data.image[IDdmin].md[0].cnt0++;
            data.image[IDdmin].md[0].write = 0;

            // RECORD
            ptr = (char*) data.image[IDrec_dmout].array.F;
            ptr += sizeof(float)*k*dmsize;
            memcpy(ptr, data.image[IDdmout].array.F, sizeof(float)*dmsize); //out->in

            timearray[k] = runtime;

            usleep(dtus);
            k++;
        }

        // ZERO DM
        data.image[IDdmin].md[0].write = 1;
        for(ii=0; ii<dmsize; ii++)
            data.image[IDdmin].array.F[ii] = 0.0;
        data.image[IDdmin].md[0].cnt0++;
        data.image[IDdmin].md[0].write = 0;


        k1 = k;
        //    save_fits("_tmprecdmout", "!_tmprecdmout.fits");


        printf("\n\n");

        // PROCESS RECORDED DATA
        for(k=0; k<k1; k++)
        {
            printf("\r  %5ld / %5ld     ", k, k1);
            fflush(stdout);

            ptr = (char*) data.image[IDrec_dmout].array.F;
            ptr += sizeof(float)*k*dmsize;
            memcpy(data.image[IDdmtmp].array.F, ptr, sizeof(float)*dmsize);
            // decompose in modes
            linopt_imtools_image_fitModes("_tmpdm", DMmodes_name, DMmask_name, SVDeps, "dmcoeffs", SVDreuse);
            SVDreuse = 1;
            IDcoeff = image_ID("dmcoeffs");
            for(m=0; m<NBmodes; m++)
                data.image[IDcoeffarray].array.F[m*kmax+k] = data.image[IDcoeff].array.F[m];
            delete_image_ID("dmcoeffs");

        }
        printf("\n\n");

        save_fits("_tmpcoeffarray", "!_tmpcoeffarray.fits");

        // EXTRACT AMPLITUDE AND PHASE
        coscoeff = 0.0;
        sincoeff = 0.0;
        for(k=k1/4; k<k1; k++)
        {
            pha = 2.0*M_PI*timearray[k]*f;
            coscoeff += cos(pha)*data.image[IDcoeffarray].array.F[kk*kmax+k];
            sincoeff += sin(pha)*data.image[IDcoeffarray].array.F[kk*kmax+k];
        }
        coscoeff /= (0.5*k1*0.75);
        sincoeff /= (0.5*k1*0.75);

        PSDamp = coscoeff*coscoeff + sincoeff*sincoeff;
        PSDpha = atan2(-sincoeff, -coscoeff);
        fp = fopen(fname, "a");
        fprintf(fp, "    %20f %20.18f %20f\n", f, sqrt(PSDamp)/ampl, PSDpha);
        fclose(fp);
    }

    delete_image_ID("_tmpdm");

    free(timearray);


    return(IDout);
}


//
//
//
long AOloopControl_TestDMmodes_Recovery(const char *DMmodes_name, float ampl, const char *DMmask_name, const char *DMstream_in_name, const char *DMstream_out_name, const char *DMstream_meas_name, long tlagus, long NBave, const char *IDout_name, const char *IDoutrms_name, const char *IDoutmeas_name, const char *IDoutmeasrms_name)
{
    long IDout, IDoutrms, IDoutmeas, IDoutmeasrms;
    long IDmodes, IDdmmask, IDdmin, IDmeasout, IDdmout;
    long dmxsize, dmysize, dmsize, NBmodes;
    long kk;
    long IDdmtmp, IDmeastmp;
    int SVDreuse = 0;
    float SVDeps = 1.0e-3;
    long IDcoeffarray;
    long IDcoeffarraymeas;
    long cntdmout;
    long IDcoeff;
    long ii, i, kk1;


    IDmodes = image_ID(DMmodes_name);
    IDdmin = image_ID(DMstream_in_name);
    IDdmout = image_ID(DMstream_out_name);
    IDmeasout = image_ID(DMstream_meas_name);
    IDdmmask = image_ID(DMmask_name);

    dmxsize = data.image[IDmodes].md[0].size[0];
    dmysize = data.image[IDmodes].md[0].size[1];
    dmsize = dmxsize*dmysize;
    NBmodes = data.image[IDmodes].md[0].size[2];


    //
    // CHECK IMAGE SIZES
    //
    if(data.image[IDdmin].md[0].size[0]!=data.image[IDmodes].md[0].size[0])
    {
        printf("ERROR: x size of \"%s\"  (%ld) does not match x size of \"%s\" (%ld)\n", DMstream_in_name, (long) data.image[IDdmin].md[0].size[0], DMmodes_name, (long) data.image[IDmodes].md[0].size[0]);
        exit(0);
    }

    if(data.image[IDdmin].md[0].size[1]!=data.image[IDmodes].md[0].size[1])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match y size of \"%s\" (%ld)\n", DMstream_in_name, (long) data.image[IDdmin].md[0].size[1], DMmodes_name, (long) data.image[IDmodes].md[0].size[1]);
        exit(0);
    }

    if(data.image[IDdmout].md[0].size[0]!=data.image[IDmodes].md[0].size[0])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match x size of \"%s\" (%ld)\n", DMstream_out_name, (long) data.image[IDdmout].md[0].size[0], DMmodes_name, (long) data.image[IDmodes].md[0].size[0]);
        exit(0);
    }

    if(data.image[IDdmout].md[0].size[1]!=data.image[IDmodes].md[0].size[1])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match y size of \"%s\" (%ld)\n", DMstream_out_name, (long) data.image[IDdmout].md[0].size[1], DMmodes_name, (long) data.image[IDmodes].md[0].size[1]);
        exit(0);
    }

    if(data.image[IDmeasout].md[0].size[0]!=data.image[IDmodes].md[0].size[0])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match x size of \"%s\" (%ld)\n", DMstream_meas_name, (long) data.image[IDmeasout].md[0].size[0], DMmodes_name, (long) data.image[IDmodes].md[0].size[0]);
        exit(0);
    }

    if(data.image[IDmeasout].md[0].size[1]!=data.image[IDmodes].md[0].size[1])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match y size of \"%s\" (%ld)\n", DMstream_meas_name, (long) data.image[IDmeasout].md[0].size[1], DMmodes_name, (long) data.image[IDmodes].md[0].size[1]);
        exit(0);
    }

    if(data.image[IDdmmask].md[0].size[0]!=data.image[IDmodes].md[0].size[0])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match x size of \"%s\" (%ld)\n", DMmask_name, (long) data.image[IDdmmask].md[0].size[0], DMmodes_name, (long) data.image[IDmodes].md[0].size[0]);
        exit(0);
    }

    if(data.image[IDdmmask].md[0].size[1]!=data.image[IDmodes].md[0].size[1])
    {
        printf("ERROR: y size of \"%s\"  (%ld) does not match y size of \"%s\" (%ld)\n", DMmask_name, (long) data.image[IDdmmask].md[0].size[1], DMmodes_name, (long) data.image[IDmodes].md[0].size[1]);
        exit(0);
    }


    IDout = create_2Dimage_ID(IDout_name, NBmodes, NBmodes);
    IDoutrms = create_2Dimage_ID(IDoutrms_name, NBmodes, NBmodes);
    IDoutmeas = create_2Dimage_ID(IDoutmeas_name, NBmodes, NBmodes);
    IDoutmeasrms = create_2Dimage_ID(IDoutmeasrms_name, NBmodes, NBmodes);
    IDdmtmp = create_2Dimage_ID("_tmpdm", dmxsize, dmysize);
    IDmeastmp = create_2Dimage_ID("_tmpmeas", dmxsize, dmysize);

    IDcoeffarray = create_2Dimage_ID("_coeffarray", NBmodes, NBave);
    IDcoeffarraymeas = create_2Dimage_ID("_coeffarraymeas", NBmodes, NBave);

    printf("Initialize SVD ... ");
    fflush(stdout);
    linopt_imtools_image_fitModes("_tmpdm", DMmodes_name, DMmask_name, SVDeps, "dmcoeffs", SVDreuse);
    SVDreuse = 1;
    printf("done\n");
    fflush(stdout);

    printf("\n\n");

    for(kk=0; kk<NBmodes; kk++)
    {
        printf("\r Mode %5ld / %5ld       ", kk, NBmodes);
        fflush(stdout);

        // APPLY MODE TO DM
        data.image[IDdmin].md[0].write = 1;
        for(ii=0; ii<dmsize; ii++)
            data.image[IDdmin].array.F[ii] = ampl*data.image[IDmodes].array.F[kk*dmsize+ii];
        COREMOD_MEMORY_image_set_sempost_byID(IDdmin, -1);
        data.image[IDdmin].md[0].cnt0++;
        data.image[IDdmin].md[0].write = 0;

        // WAIT
        usleep(tlagus);


        // RECORD DM SHAPES INTO MODES

        // POSITIVE
        cntdmout = 0;
        i = 0;
        while(i<NBave)
        {
            while(cntdmout==data.image[IDdmout].md[0].cnt0)
                usleep(20);

            cntdmout =  data.image[IDdmout].md[0].cnt0;


            memcpy(data.image[IDdmtmp].array.F, data.image[IDdmout].array.F, sizeof(float)*dmsize);
            memcpy(data.image[IDmeastmp].array.F, data.image[IDmeasout].array.F, sizeof(float)*dmsize);

            // decompose in modes
            linopt_imtools_image_fitModes("_tmpdm", DMmodes_name, DMmask_name, SVDeps, "dmcoeffs", SVDreuse);
            IDcoeff = image_ID("dmcoeffs");
            for(kk1=0; kk1<NBmodes; kk1++)
                data.image[IDcoeffarray].array.F[kk1*NBave+i] = 0.5*data.image[IDcoeff].array.F[kk1];
            delete_image_ID("dmcoeffs");

            linopt_imtools_image_fitModes("_tmpmeas", DMmodes_name, DMmask_name, SVDeps, "dmcoeffs", SVDreuse);
            IDcoeff = image_ID("dmcoeffs");
            for(kk1=0; kk1<NBmodes; kk1++)
                data.image[IDcoeffarraymeas].array.F[kk1*NBave+i] = 0.5*data.image[IDcoeff].array.F[kk1];
            delete_image_ID("dmcoeffs");


            i++;
        }

        // NEGATIVE

        // APPLY MODE TO DM
        data.image[IDdmin].md[0].write = 1;
        for(ii=0; ii<dmsize; ii++)
            data.image[IDdmin].array.F[ii] = -ampl*data.image[IDmodes].array.F[kk*dmsize+ii];
        COREMOD_MEMORY_image_set_sempost_byID(IDdmin, -1);
        data.image[IDdmin].md[0].cnt0++;
        data.image[IDdmin].md[0].write = 0;

        // WAIT
        usleep(tlagus);

        cntdmout = 0;
        i = 0;
        while(i<NBave)
        {
            while(cntdmout==data.image[IDdmout].md[0].cnt0)
                usleep(20);

            cntdmout =  data.image[IDdmout].md[0].cnt0;

            memcpy(data.image[IDdmtmp].array.F, data.image[IDdmout].array.F, sizeof(float)*dmsize);
            memcpy(data.image[IDmeastmp].array.F, data.image[IDmeasout].array.F, sizeof(float)*dmsize);

            // decompose in modes
            linopt_imtools_image_fitModes("_tmpdm", DMmodes_name, DMmask_name, SVDeps, "dmcoeffs", SVDreuse);
            IDcoeff = image_ID("dmcoeffs");
            for(kk1=0; kk1<NBmodes; kk1++)
                data.image[IDcoeffarray].array.F[kk1*NBave+i] -= 0.5*data.image[IDcoeff].array.F[kk1];
            delete_image_ID("dmcoeffs");
            i++;

            linopt_imtools_image_fitModes("_tmpmeas", DMmodes_name, DMmask_name, SVDeps, "dmcoeffs", SVDreuse);
            IDcoeff = image_ID("dmcoeffs");
            for(kk1=0; kk1<NBmodes; kk1++)
                data.image[IDcoeffarraymeas].array.F[kk1*NBave+i] = 0.5*data.image[IDcoeff].array.F[kk1];
            delete_image_ID("dmcoeffs");
        }


        // PROCESSS

        for(kk1=0; kk1<NBmodes; kk1++)
        {
            data.image[IDout].array.F[kk1*NBmodes+kk] = 0.0;
            data.image[IDoutrms].array.F[kk1*NBmodes+kk] = 0.0;
            data.image[IDoutmeas].array.F[kk1*NBmodes+kk] = 0.0;
            data.image[IDoutmeasrms].array.F[kk1*NBmodes+kk] = 0.0;
        }
        for(kk1=0; kk1<NBmodes; kk1++)
        {
            for(i=0; i<NBave; i++)
            {
                data.image[IDout].array.F[kk1*NBmodes+kk] += data.image[IDcoeffarray].array.F[kk1*NBave+i];
                data.image[IDoutrms].array.F[kk1*NBmodes+kk] += data.image[IDcoeffarray].array.F[kk1*NBave+i]*data.image[IDcoeffarray].array.F[kk1*NBave+i];
                data.image[IDoutmeas].array.F[kk1*NBmodes+kk] += data.image[IDcoeffarraymeas].array.F[kk1*NBave+i];
                data.image[IDoutmeasrms].array.F[kk1*NBmodes+kk] += data.image[IDcoeffarraymeas].array.F[kk1*NBave+i]*data.image[IDcoeffarraymeas].array.F[kk1*NBave+i];
            }
            data.image[IDout].array.F[kk1*NBmodes+kk] /= NBave*ampl;
            data.image[IDoutrms].array.F[kk1*NBmodes+kk] = sqrt(data.image[IDoutrms].array.F[kk1*NBmodes+kk]/NBave);
            data.image[IDoutmeas].array.F[kk1*NBmodes+kk] /= NBave*ampl;
            data.image[IDoutmeasrms].array.F[kk1*NBmodes+kk] = sqrt(data.image[IDoutmeasrms].array.F[kk1*NBmodes+kk]/NBave);
        }
    }
    printf("\n\n");
    fflush(stdout);

    delete_image_ID("_tmpdm");
    delete_image_ID("_tmpmeas");
    delete_image_ID("_coeffarray");



    return IDout;
}




//
// measure response matrix sensitivity
//
int_fast8_t AOloopControl_AnalyzeRM_sensitivity(const char *IDdmmodes_name, const char *IDdmmask_name, const char *IDwfsref_name, const char *IDwfsresp_name, const char *IDwfsmask_name, float amplimitnm, float lambdanm, const char *foutname)
{
    FILE *fp;
    long IDdmmodes;
    long IDdmmask;
    long IDwfsref;
    long IDwfsresp;
    long IDwfsmask;

    long dmxsize, dmysize, dmxysize;
    long NBmodes;
    long wfsxsize, wfsysize, wfsxysize;
    long mode, mode1;

    long ii;

    double dmmoderms, dmmodermscnt;
    double wfsmoderms, wfsmodermscnt;
    double tmp1;

    double wfsreftot, wfsmasktot, aveval;
    double SNR, SNR1; // single pixel SNR

    float frac = 0.0;
    float pcnt;
    long IDoutXP, IDoutXP_WFS;
    double XPval;

    double sigmarad;
    double eff; // efficiency




    printf("amplimit = %f nm\n", amplimitnm);


    IDdmmodes = image_ID(IDdmmodes_name);
    dmxsize = data.image[IDdmmodes].md[0].size[0];
    dmysize = data.image[IDdmmodes].md[0].size[1];
    NBmodes = data.image[IDdmmodes].md[0].size[2];
    dmxysize = dmxsize * dmysize;

    IDdmmask = image_ID(IDdmmask_name);

    IDwfsref = image_ID(IDwfsref_name);
    wfsxsize = data.image[IDwfsref].md[0].size[0];
    wfsysize = data.image[IDwfsref].md[0].size[1];
    wfsxysize = wfsxsize * wfsysize;

    IDwfsresp = image_ID(IDwfsresp_name);
    IDwfsmask = image_ID(IDwfsmask_name);

    wfsreftot = 0.0;
    for(ii=0; ii<wfsxysize; ii++)
        wfsreftot += data.image[IDwfsref].array.F[ii];

    wfsmasktot = 0.0;
    for(ii=0; ii<wfsxysize; ii++)
        wfsmasktot += data.image[IDwfsmask].array.F[ii];


    list_image_ID();
    printf("NBmodes = %ld\n", NBmodes);
    printf("wfs size = %ld %ld\n", wfsxsize, wfsysize);
    printf("wfs resp ID : %ld\n", IDwfsresp);
    printf("wfs mask ID : %ld\n", IDwfsmask);

    printf("wfsmasktot = %f\n", wfsmasktot);

    fp = fopen(foutname, "w");

    fprintf(fp, "# col 1 : mode index\n");
    fprintf(fp, "# col 2 : average value (should be zero)\n");
    fprintf(fp, "# col 3 : DM mode RMS\n");
    fprintf(fp, "# col 4 : WFS mode RMS\n");
    fprintf(fp, "# col 5 : SNR for a 1um DM motion with 1 ph\n");
    fprintf(fp, "# col 6 : fraction of flux used in measurement\n");
    fprintf(fp, "# col 7 : Photon Efficiency\n");
    fprintf(fp, "\n");



    for(mode=0; mode<NBmodes; mode++)
    {
        dmmoderms = 0.0;
        dmmodermscnt = 0.0;
        aveval = 0.0;
        for(ii=0; ii<dmxysize; ii++)
        {
            tmp1 = data.image[IDdmmodes].array.F[mode*dmxysize+ii]*data.image[IDdmmask].array.F[ii];
            aveval += tmp1;
            dmmoderms += tmp1*tmp1;
            dmmodermscnt += data.image[IDdmmask].array.F[ii];
        }
        dmmoderms = sqrt(dmmoderms/dmmodermscnt);
        aveval /= dmmodermscnt;

        SNR = 0.0;
        wfsmoderms = 0.0;
        wfsmodermscnt = 0.0;
        pcnt = 0.0;
        for(ii=0; ii<wfsxysize; ii++)
        {
            tmp1 = data.image[IDwfsresp].array.F[mode*wfsxysize+ii]*data.image[IDwfsmask].array.F[ii];
            wfsmoderms += tmp1*tmp1;
            wfsmodermscnt = 1.0;
            wfsmodermscnt += data.image[IDwfsmask].array.F[ii];

            if(data.image[IDwfsmask].array.F[ii]>0.1)
                if(data.image[IDwfsref].array.F[ii]>fabs(data.image[IDwfsresp].array.F[mode*wfsxysize+ii]*amplimitnm*0.001))
                {
                    SNR1 = data.image[IDwfsresp].array.F[mode*wfsxysize+ii]/sqrt(data.image[IDwfsref].array.F[ii]);
                    SNR1 /= wfsreftot;
                    SNR += SNR1*SNR1;
                    pcnt += data.image[IDwfsref].array.F[ii];
                }
        }
        frac = pcnt/wfsreftot;

        wfsmoderms = sqrt(wfsmoderms/wfsmodermscnt);
        SNR = sqrt(SNR); // SNR for 1 ph, 1um DM actuation
        // -> sigma for 1ph = 1/SNR [DMum]

        // 1umDM act = 2.0*M_PI * ( 2.0 / (lambdanm*0.001) ) rad WF
        // -> sigma for 1ph = (1/SNR) * 2.0*M_PI * ( 2.0 / (lambdanm*0.001) ) rad WF
        sigmarad = (1.0/SNR) * 2.0*M_PI * ( 2.0 / (lambdanm*0.001) );

        // SNR is in DMum per sqrt(Nph)
        // factor 2.0 for DM reflection

        eff = 1.0/(sigmarad*sigmarad);


        fprintf(fp, "%5ld   %16f   %16f   %16f    %16g      %12g        %12.10f\n", mode, aveval, dmmoderms, wfsmoderms, SNR, frac, eff);
    }

    fclose(fp);


    // computing DM space cross-product
    IDoutXP = create_2Dimage_ID("DMmodesXP", NBmodes, NBmodes);

    for(mode=0; mode<NBmodes; mode++)
        for(mode1=0; mode1<mode+1; mode1++)
        {
            XPval = 0.0;
            for(ii=0; ii<dmxysize; ii++)
                XPval += data.image[IDdmmask].array.F[ii]*data.image[IDdmmodes].array.F[mode*dmxysize+ii]*data.image[IDdmmodes].array.F[mode1*dmxysize+ii];

            data.image[IDoutXP].array.F[mode*NBmodes+mode1] = XPval/dmmodermscnt;
        }
    save_fits("DMmodesXP", "!DMmodesXP.fits");


    // computing WFS space cross-product
    IDoutXP_WFS = create_2Dimage_ID("WFSmodesXP", NBmodes, NBmodes);
    for(mode=0; mode<NBmodes; mode++)
        for(mode1=0; mode1<mode+1; mode1++)
        {
            XPval = 0.0;
            for(ii=0; ii<wfsxysize; ii++)
                XPval += data.image[IDwfsresp].array.F[mode*wfsxysize+ii]*data.image[IDwfsresp].array.F[mode1*wfsxysize+ii];

            data.image[IDoutXP_WFS].array.F[mode*NBmodes+mode1] = XPval/wfsxysize;
        }
    save_fits("WFSmodesXP", "!WFSmodesXP.fits");


    return(0);
}



/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 10. FOCAL PLANE SPECKLE MODULATION / CONTROL                             */
/* =============================================================================================== */
/* =============================================================================================== */





// optimize LO - uses simulated downhill simplex
int_fast8_t AOloopControl_OptimizePSF_LO(const char *psfstream_name, const char *IDmodes_name, const char *dmstream_name, long delayframe, long NBframes)
{
    long IDpsf;
    long IDmodes;
    long IDdmstream;
    long IDdm;
    long psfxsize, psfysize;
    long dmxsize, dmysize;
    long NBmodes;
    long mode;
    double ampl;
    double x;
    long ii, jj;

    long IDdmbest;
    long IDpsfarray;


    ampl = 0.01; // modulation amplitude

    IDpsf = image_ID(psfstream_name);
    IDmodes = image_ID(IDmodes_name);
    IDdmstream = image_ID(dmstream_name);

    psfxsize = data.image[IDpsf].md[0].size[0];
    psfysize = data.image[IDpsf].md[0].size[1];

    IDdmbest = create_2Dimage_ID("dmbest", dmxsize, dmysize);
    IDdm = create_2Dimage_ID("dmcurr", dmxsize, dmysize);

    dmxsize = data.image[IDdm].md[0].size[0];
    dmysize = data.image[IDdm].md[0].size[1];

    NBmodes = data.image[IDmodes].md[0].size[2];

    for(ii=0; ii<dmxsize*dmysize; ii++)
        data.image[IDdmbest].array.F[ii] = data.image[IDdm].array.F[ii];


    for(mode=0; mode<NBmodes; mode ++)
    {
        for(x=-ampl; x<1.01*ampl; x += ampl)
        {
            // apply DM pattern
            for(ii=0; ii<dmxsize*dmysize; ii++)
                data.image[IDdm].array.F[ii] = data.image[IDdmbest].array.F[ii]+ampl*data.image[IDmodes].array.F[dmxsize*dmysize*mode+ii];

            data.image[IDdmstream].md[0].write = 1;
            memcpy(data.image[IDdmstream].array.F, data.image[IDdm].array.F, sizeof(float)*dmxsize*dmysize);
            data.image[IDdmstream].md[0].cnt0++;
            data.image[IDdmstream].md[0].write = 0;



        }
    }


    return(0);
}


//
// modulate using linear combination of two probes A and B
//
//
// delay is in sec
//
int_fast8_t AOloopControl_DMmodulateAB(const char *IDprobeA_name, const char *IDprobeB_name, const char *IDdmstream_name, const char *IDrespmat_name, const char *IDwfsrefstream_name, double delay, long NBprobes)
{
    long IDprobeA;
    long IDprobeB;
    long dmxsize, dmysize;
    long dmsize;
    long IDdmstream;

    long IDrespmat;
    long IDwfsrefstream;
    long wfsxsize, wfsysize;
    long wfssize;

    long IDdmC;
    long IDwfsrefC;

    float *coeffA;
    float *coeffB;
    int k;
    long act, wfselem;

    FILE *fp;
    char flogname[200];
    int loopOK;
    long dmframesize, wfsframesize;
    char timestr[200];
    time_t t;
    struct tm *uttime;
    struct timespec *thetime = (struct timespec *)malloc(sizeof(struct timespec));
    long ii;
    int semval;



    IDprobeA = image_ID(IDprobeA_name);
    dmxsize = data.image[IDprobeA].md[0].size[0];
    dmysize = data.image[IDprobeA].md[0].size[1];
    dmsize = dmxsize*dmysize;

    IDprobeB = image_ID(IDprobeB_name);
    IDdmstream = image_ID(IDdmstream_name);
    IDrespmat = image_ID(IDrespmat_name);
    IDwfsrefstream = image_ID(IDwfsrefstream_name);
    wfsxsize = data.image[IDwfsrefstream].md[0].size[0];
    wfsysize = data.image[IDwfsrefstream].md[0].size[1];
    wfssize = wfsxsize*wfsysize;

    coeffA = (float*) malloc(sizeof(float)*NBprobes);
    coeffB = (float*) malloc(sizeof(float)*NBprobes);

    IDdmC = create_3Dimage_ID("MODdmC", dmxsize, dmysize, NBprobes);
    IDwfsrefC = create_3Dimage_ID("WFSrefC", wfsxsize, wfsysize, NBprobes);

    coeffA[0] = 0.0;
    coeffB[0] = 0.0;
    for(k=1; k<NBprobes; k++)
    {
        coeffA[k] = cos(2.0*M_PI*(k-1)/(NBprobes-1));
        coeffB[k] = sin(2.0*M_PI*(k-1)/(NBprobes-1));
    }


    // prepare MODdmC and WFSrefC
    for(k=0; k<NBprobes; k++)
    {
        for(act=0; act<dmsize; act++)
            data.image[IDdmC].array.F[k*dmsize+act] = coeffA[k]*data.image[IDprobeA].array.F[act] + coeffB[k]*data.image[IDprobeB].array.F[act];

        for(wfselem=0; wfselem<wfssize; wfselem++)
            for(act=0; act<dmsize; act++)
                data.image[IDwfsrefC].array.F[k*wfssize+wfselem] += data.image[IDdmC].array.F[k*dmsize+act]*data.image[IDrespmat].array.F[act*wfssize+wfselem];
    }

    save_fl_fits("MODdmC", "!test_MODdmC.fits");
    save_fl_fits("WFSrefC", "!test_WFSrefC.fits");

    t = time(NULL);
    uttime = gmtime(&t);
    if(sprintf(flogname, "logfpwfs_%04d-%02d-%02d_%02d:%02d:%02d.txt", 1900+uttime->tm_year, 1+uttime->tm_mon, uttime->tm_mday, uttime->tm_hour, uttime->tm_min, uttime->tm_sec) < 1)
        printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

    if((fp=fopen(flogname,"w"))==NULL)
    {
        printf("ERROR: cannot create file \"%s\"\n", flogname);
        exit(0);
    }
    fclose(fp);


    dmframesize = sizeof(float)*dmsize;
    wfsframesize = sizeof(float)*wfssize;

    list_image_ID();



    if (sigaction(SIGINT, &data.sigact, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &data.sigact, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGBUS, &data.sigact, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    /*   if (sigaction(SIGSEGV, &data.sigact, NULL) == -1) {
           perror("sigaction");
           exit(EXIT_FAILURE);
       }*/
    if (sigaction(SIGABRT, &data.sigact, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGHUP, &data.sigact, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGPIPE, &data.sigact, NULL) == -1) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }



    k = 0;
    loopOK = 1;

    while(loopOK == 1)
    {
        printf("Applying probe # %d   %ld %ld\n", k, IDdmstream, IDwfsrefstream);
        fflush(stdout);

        // apply probe
        char *ptr0;
        ptr0 = (char*) data.image[IDdmC].array.F;
        ptr0 += k*dmframesize;
        data.image[IDdmstream].md[0].write = 1;
        memcpy(data.image[IDdmstream].array.F, (void*) ptr0, dmframesize);
        sem_getvalue(data.image[IDdmstream].semptr[0], &semval);
        if(semval<SEMAPHORE_MAXVAL)
            sem_post(data.image[IDdmstream].semptr[0]);
        data.image[IDdmstream].md[0].cnt0++;
        data.image[IDdmstream].md[0].write = 0;

        // apply wfsref offset
        ptr0 = (char*) data.image[IDwfsrefC].array.F;
        ptr0 += k*wfsframesize;
        data.image[IDwfsrefstream].md[0].write = 1;
        memcpy(data.image[IDwfsrefstream].array.F, (void*) ptr0, wfsframesize);
        sem_getvalue(data.image[IDwfsrefstream].semptr[0], &semval);
        if(semval<SEMAPHORE_MAXVAL)
            sem_post(data.image[IDwfsrefstream].semptr[0]);
        data.image[IDwfsrefstream].md[0].cnt0++;
        data.image[IDwfsrefstream].md[0].write = 0;

        // write time in log
        t = time(NULL);
        uttime = gmtime(&t);
        clock_gettime(CLOCK_REALTIME, thetime);

        if(sprintf(timestr, "%02d %02d %02d.%09ld", uttime->tm_hour, uttime->tm_min, uttime->tm_sec, thetime->tv_nsec) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        printf("time = %s\n", timestr);
        if((fp = fopen(flogname, "a"))==NULL)
        {
            printf("ERROR: cannot open file \"%s\"\n", flogname);
            exit(0);
        }
        fprintf(fp, "%s %2d %10f %10f\n", timestr, k, coeffA[k], coeffB[k]);
        fclose(fp);

        usleep((long) (1.0e6*delay));
        k++;
        if(k==NBprobes)
            k = 0;

        if((data.signal_INT == 1)||(data.signal_TERM == 1)||(data.signal_ABRT==1)||(data.signal_BUS==1)||(data.signal_SEGV==1)||(data.signal_HUP==1)||(data.signal_PIPE==1))
            loopOK = 0;
    }

    data.image[IDdmstream].md[0].write = 1;
    for(ii=0; ii<dmsize; ii++)
        data.image[IDdmstream].array.F[ii] = 0.0;
    sem_getvalue(data.image[IDdmstream].semptr[0], &semval);
    if(semval<SEMAPHORE_MAXVAL)
        sem_post(data.image[IDdmstream].semptr[0]);
    data.image[IDdmstream].md[0].cnt0++;
    data.image[IDdmstream].md[0].write = 0;


    data.image[IDwfsrefstream].md[0].write = 1;
    for(ii=0; ii<wfssize; ii++)
        data.image[IDwfsrefstream].array.F[ii] = 0.0;
    sem_getvalue(data.image[IDwfsrefstream].semptr[0], &semval);
    if(semval<SEMAPHORE_MAXVAL)
        sem_post(data.image[IDwfsrefstream].semptr[0]);
    data.image[IDwfsrefstream].md[0].cnt0++;
    data.image[IDwfsrefstream].md[0].write = 0;



    free(coeffA);
    free(coeffB);


    return(0);
}



/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 11. PROCESS LOG FILES                                                    */
/* =============================================================================================== */
/* =============================================================================================== */


int_fast8_t AOloopControl_logprocess_modeval(const char *IDname)
{
    long ID;
    long NBmodes;
    long NBframes;

    long IDout_ave;
    long IDout_rms;

    long m;
    long ID1dtmp;
    FILE *fp;

    long ID1dPSD;
    FILE *fpPSD;
    long IDft;
    char fname[200];



    ID = image_ID(IDname);
    NBmodes = data.image[ID].md[0].size[0]*data.image[ID].md[0].size[1];
    NBframes = data.image[ID].md[0].size[2];

    IDout_ave = create_2Dimage_ID("modeval_ol_ave", data.image[ID].md[0].size[0], data.image[ID].md[0].size[1]);
    IDout_rms = create_2Dimage_ID("modeval_ol_rms", data.image[ID].md[0].size[0], data.image[ID].md[0].size[1]);

    ID1dtmp = create_2Dimage_ID("modeval1d", data.image[ID].md[0].size[2], 1);
    ID1dPSD = create_2Dimage_ID("modevalPSD", data.image[ID].md[0].size[2]/2, 1);

    if(system("mkdir -p modePSD") != 0)
        printERROR(__FILE__, __func__, __LINE__, "system() returns non-zero value");

    fp = fopen("moveval_stats.dat", "w");
    for(m=0; m<NBmodes; m++)
    {
        double ave = 0.0;
        double rms;
        long kk;
        double tmpv;

        for(kk=0; kk<NBframes; kk++)
            ave += data.image[ID].array.F[kk*NBmodes+m];
        ave /= NBframes;
        data.image[IDout_ave].array.F[m] = ave;
        rms = 0.0;
        for(kk=0; kk<NBframes; kk++)
        {
            tmpv = (data.image[ID].array.F[kk*NBmodes+m]-ave);
            rms += tmpv*tmpv;
        }
        rms = sqrt(rms/NBframes);
        data.image[IDout_rms].array.F[m] = rms;


        for(kk=0; kk<NBframes; kk++)
            data.image[ID1dtmp].array.F[kk] = data.image[ID].array.F[kk*NBmodes+m];
        do1drfft("modeval1d", "modeval1d_FT");
        IDft = image_ID("modeval1d_FT");

        if(sprintf(fname, "./modePSD/modevalPSD_%04ld.dat", m) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        fpPSD = fopen(fname, "w");
        for(kk=0; kk<NBframes/2; kk++)
        {
            data.image[ID1dPSD].array.F[kk] = data.image[IDft].array.CF[kk].re*data.image[IDft].array.CF[kk].re + data.image[IDft].array.CF[kk].im*data.image[IDft].array.CF[kk].im;
            fprintf(fpPSD, "%03ld %g\n", kk, data.image[ID1dPSD].array.F[kk]);
        }
        delete_image_ID("modeval1d_FT");
        fclose(fpPSD);


        fprintf(fp, "%4ld  %12.8f  %12.8f\n", m, data.image[IDout_ave].array.F[m], data.image[IDout_rms].array.F[m]);
    }
    fclose(fp);



    return 0;
}

















































































































































































//
// tweak zonal response matrix in accordance to WFS response to modes
//
// INPUT :
//    ZRMimname   : starting response matrix
//    DMimCname   : cube of DM displacements
//    WFSimCname  : cube of WFS signal
//    DMmaskname  : DM pixel mask
//    WFSmaskname : WFS pixel mask
//
// OUTPUT:
//    RMoutname   : output response matrix
//

long AOloopControl_TweakRM(char *ZRMinname, char *DMinCname, char *WFSinCname, char *DMmaskname, char *WFSmaskname, char *RMoutname)
{
    long IDout, IDzrmin, IDdmin, IDwfsin, IDwfsmask, IDdmmask;
    long wfsxsize, wfsysize, wfssize;
    long dmxsize, dmysize, dmsize;
    long NBframes;


    // input response matrix
    IDzrmin = image_ID(ZRMinname);
    wfsxsize = data.image[IDzrmin].md[0].size[0];
    wfsysize = data.image[IDzrmin].md[0].size[1];

    // DM input frames
    IDdmin = image_ID(DMinCname);
    dmxsize = data.image[IDdmin].md[0].size[0];
    dmysize = data.image[IDdmin].md[0].size[1];
    dmsize = dmxsize*dmysize;

    if(dmsize != data.image[IDzrmin].md[0].size[2])
    {
        printf("ERROR: total number of DM actuators (%ld) does not match zsize of RM (%ld)\n", dmsize, (long) data.image[IDzrmin].md[0].size[2]);
        exit(0);
    }

    NBframes = data.image[IDdmin].md[0].size[2];


    // input WFS frames
    IDwfsin = image_ID(WFSinCname);
    if((data.image[IDwfsin].md[0].size[0] != wfsxsize) || (data.image[IDwfsin].md[0].size[1] != wfsysize) || (data.image[IDwfsin].md[0].size[2] != NBframes))
    {
        printf("ERROR: size of WFS mask image \"%s\" (%ld %ld %ld) does not match expected size (%ld %ld %ld)\n", WFSmaskname, (long) data.image[IDwfsin].md[0].size[0], (long) data.image[IDwfsin].md[0].size[1], (long) data.image[IDwfsin].md[0].size[2], wfsxsize, wfsysize, NBframes);
        exit(0);
    }

    // DM mask
    IDdmmask = image_ID(DMmaskname);
    if((data.image[IDdmmask].md[0].size[0] != dmxsize) || (data.image[IDdmmask].md[0].size[1] != dmysize))
    {
        printf("ERROR: size of DM mask image \"%s\" (%ld %ld) does not match expected size (%ld %ld)\n", DMmaskname, (long) data.image[IDdmmask].md[0].size[0], (long) data.image[IDdmmask].md[0].size[1], dmxsize, dmysize);
        exit(0);
    }



    // ARRANGE DATA IN MATRICES





    return(IDout);
}












































































































/* =============================================================================================== */
/* =============================================================================================== */
/** @name AOloopControl - 12. OBSOLETE ?                                                           */ 
/* =============================================================================================== */
/* =============================================================================================== */




int_fast8_t AOloopControl_setgainrange(long m0, long m1, float gainval)
{
    long k;
    long kmax;

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_GAIN_modes==-1)
    {
        char name[200];
        if(sprintf(name, "aol%ld_DMmode_GAIN", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_GAIN_modes = read_sharedmem_image(name);
    }

    kmax = m1+1;
    if(kmax>AOconf[LOOPNUMBER].NBDMmodes)
        kmax = AOconf[LOOPNUMBER].NBDMmodes-1;

    for(k=m0; k<kmax; k++)
        data.image[aoconfID_GAIN_modes].array.F[k] = gainval;

    return 0;
}




int_fast8_t AOloopControl_setlimitrange(long m0, long m1, float limval)
{
    long k;
    long kmax;

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_LIMIT_modes==-1)
    {
        char name[200];
        if(sprintf(name, "aol%ld_DMmode_LIMIT", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_LIMIT_modes = read_sharedmem_image(name);
    }

    kmax = m1+1;
    if(kmax>AOconf[LOOPNUMBER].NBDMmodes)
        kmax = AOconf[LOOPNUMBER].NBDMmodes-1;

    for(k=m0; k<kmax; k++)
        data.image[aoconfID_LIMIT_modes].array.F[k] = limval;

    return 0;
}




int_fast8_t AOloopControl_setmultfrange(long m0, long m1, float multfval)
{
    long k;
    long kmax;

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_MULTF_modes==-1)
    {
        char name[200];
        if(sprintf(name, "aol%ld_DMmode_MULTF", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_MULTF_modes = read_sharedmem_image(name);
    }

    kmax = m1+1;
    if(kmax>AOconf[LOOPNUMBER].NBDMmodes)
        kmax = AOconf[LOOPNUMBER].NBDMmodes-1;

    for(k=m0; k<kmax; k++)
        data.image[aoconfID_MULTF_modes].array.F[k] = multfval;

    return 0;
}




int_fast8_t AOloopControl_setgainblock(long mb, float gainval)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_gainb == -1)
    {
        char imname[200];
        if(sprintf(imname, "aol%ld_gainb", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_gainb = read_sharedmem_image(imname);
    }


    if(mb<AOconf[LOOPNUMBER].DMmodesNBblock)
        data.image[aoconfID_gainb].array.F[mb] = gainval;

    return 0;
}




int_fast8_t AOloopControl_setlimitblock(long mb, float limitval)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_limitb == -1)
    {
        char imname[200];
        if(sprintf(imname, "aol%ld_limitb", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_limitb = read_sharedmem_image(imname);
    }

    if(mb<AOconf[LOOPNUMBER].DMmodesNBblock)
        data.image[aoconfID_limitb].array.F[mb] = limitval;

    return 0;
}




int_fast8_t AOloopControl_setmultfblock(long mb, float multfval)
{
    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_multfb == -1)
    {
        char imname[200];
        if(sprintf(imname, "aol%ld_multfb", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_multfb = read_sharedmem_image(imname);
    }

    if(mb<AOconf[LOOPNUMBER].DMmodesNBblock)
        data.image[aoconfID_multfb].array.F[mb] = multfval;

    return 0;
}

























//
// create dynamic test sequence
//
long AOloopControl_mkTestDynamicModeSeq(const char *IDname_out, long NBpt, long NBmodes)
{
    long IDout;
    long xsize, ysize, xysize;
    long ii, kk;
    float ampl0;
    float ampl;
    float pha0;
    char name[200];
    long m;

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_DMmodes==-1)
    {
        if(sprintf(name, "aol%ld_DMmodes", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_DMmodes = read_sharedmem_image(name);
    }
    xsize = data.image[aoconfID_DMmodes].md[0].size[0];
    ysize = data.image[aoconfID_DMmodes].md[0].size[1];
    xysize = xsize*ysize;

    IDout = create_3Dimage_ID(IDname_out, xsize, ysize, NBpt);

    for(kk=0; kk<NBpt; kk++)
    {
        for(ii=0; ii<xysize; ii++)
            data.image[IDout].array.F[kk*xysize+ii] = 0.0;

        for(m=0; m<NBmodes; m++)
        {
            ampl0 = 1.0;
            pha0 = M_PI*(1.0*m/NBmodes);
            ampl = ampl0 * sin(2.0*M_PI*(1.0*kk/NBpt)+pha0);
            for(ii=0; ii<xysize; ii++)
                data.image[IDout].array.F[kk*xysize+ii] += ampl * data.image[aoconfID_DMmodes].array.F[m*xysize+ii];
        }
    }

    return(IDout);
}






int_fast8_t AOloopControl_AutoTune()
{
    long block;
    long NBgain = 10;
    long NBstep = 10000;
    float gain;
    char name[200];
    long k, kg;
    float bestgain= 0.0;
    float bestval = 10000000.0;
    float val;

    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(1);

    if(aoconfID_cmd_modes==-1)
    {
        if(sprintf(name, "aol%ld_DMmode_cmd", LOOPNUMBER) < 1)
            printERROR(__FILE__, __func__, __LINE__, "sprintf wrote <1 char");

        aoconfID_cmd_modes = read_sharedmem_image(name);
    }

    // initialize
    for(block=0; block<AOconf[LOOPNUMBER].DMmodesNBblock; block++)
    {
        AOloopControl_setgainblock(block, 0.0);
        AOloopControl_setlimitblock(block, 0.1);
        AOloopControl_setmultfblock(block, 0.8);
    }


    for(block=0; block<AOconf[LOOPNUMBER].DMmodesNBblock; block++)
    {
        float gainStart = 0.0;
        float gainEnd = 1.0;
        int gOK = 1;


        // tune block gain
        gain = gainStart;
        bestval = 100000000.0;
        while((gOK==1)&&(gain<gainEnd))
        {
            for(k=0; k<AOconf[LOOPNUMBER].NBDMmodes; k++)
                data.image[aoconfID_cmd_modes].array.F[k] = 0.0;

            gain += 0.01;
            gain *= 1.1;

            AOloopControl_setgainblock(block, gain);
            AOloopControl_loopstep(LOOPNUMBER, NBstep);
            val = sqrt(AOconf[LOOPNUMBER].RMSmodesCumul/AOconf[LOOPNUMBER].RMSmodesCumulcnt);
            printf("%2ld  %6.4f  %10.8lf\n", kg, gain, val);

            if(val<bestval)
            {
                bestval = val;
                bestgain = gain;
            }
            else
                gOK = 0;
        }
        printf("BLOCK %ld  : BEST GAIN = %f\n", block, bestgain);

        AOloopControl_setgainblock(block, bestgain);
    }


    return(0);
}










/** Record periodic camera signal (to be used if there is a periodic camera error)
 *
 * folds the signal onto one period
 *
 */
/*
int_fast8_t AOloopControl_Measure_WFScam_PeriodicError(long loop, long NBframes, long NBpha, char *IDout_name)
{
    FILE *fp;
    char fname[200];
    long ii, jj, kk, kk1, kkmax;
    long IDrc, IDrefim;
    long IDout;

    double period; /// in frames
    double period_start = 1000.0;
    double period_end = 1200.0;
    double period_step;
    double pha;
    long *phacnt;
    long phal;
    long double rmsval;
    double rmsvalmin, rmsvalmax;
    double periodopt;
    long cnt;
    long p, p0, p1, pmin, pmax;

    double intpart;
    double tmpv1;

    double *coarsermsarray;
    double rmsvalmin1;
    int lOK;
    double level1, level2, level3;
    long pp1, pp2, pp3;
    int level1OK, level2OK, level3OK;

    long kw;
    char kname[200];
    char comment[200];


    if(AOloopcontrol_meminit==0)
        AOloopControl_InitializeMemory(0);


    IDrc = create_3Dimage_ID("Rcube", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, NBframes);
    IDout = create_3Dimage_ID(IDout_name, AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS, NBpha);

    printf("SETTING UP... (loop %ld)\n", LOOPNUMBER);
    fflush(stdout);

  //  sprintf(fname, "./conf/AOloop.conf");
    AOloopControl_loadconfigure(LOOPNUMBER, 1, 10);
    //exit(0);

    printf("Importing WFS camera image shared memory ... \n");
    aoconfID_wfsim = read_sharedmem_image(AOconf[loop].WFSname);



    for(kk=0; kk<NBframes; kk++)
    {
        Read_cam_frame(loop, 0, 1, 0, 0);
        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            data.image[IDrc].array.F[kk*AOconf[loop].sizeWFS+ii] = data.image[aoconfID_imWFS1].array.F[ii];
    }

    save_fits("Rcube", "!Rcube.fits");

    IDrefim = create_2Dimage_ID("refim", AOconf[loop].sizexWFS, AOconf[loop].sizeyWFS);
    for(kk=0; kk<NBframes; kk++)
    {
        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            data.image[IDrefim].array.F[ii] += data.image[IDrc].array.F[kk*AOconf[loop].sizeWFS+ii];
    }
    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
        data.image[IDrefim].array.F[ii] /= NBframes;

    for(kk=0; kk<NBframes; kk++)
    {
        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            data.image[IDrc].array.F[kk*AOconf[loop].sizeWFS+ii] -= data.image[IDrefim].array.F[ii];
    }
    save_fits("Rcube", "!R1cube.fits");


    // find periodicity ( coarse search )
    fp = fopen("wfscampe_coarse.txt","w");
    fclose(fp);

    pmax = (long) NBframes/2;
    pmin = 0;
    rmsvalmin = 1.0e20;



    rmsvalmax = 0.0;
    p0 = 200;
    coarsermsarray = (double*) malloc(sizeof(double)*pmax);
    for(p=p0; p<pmax; p++)
    {
        rmsval = 0.0;
        kkmax = 100;
        if(kkmax+pmax>NBframes)
        {
            printf("ERROR: pmax, kkmax not compatible\n");
            exit(0);
        }

        for(kk=0; kk<kkmax; kk++)
        {
            kk1 = kk+p;
            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            {
                tmpv1 = data.image[IDrc].array.F[kk*AOconf[loop].sizeWFS+ii] - data.image[IDrc].array.F[kk1*AOconf[loop].sizeWFS+ii];
                rmsval += tmpv1*tmpv1;
            }
        }
        rmsval = sqrt(rmsval/kkmax/AOconf[loop].sizeWFS);

        if(rmsval<rmsvalmin)
        {
            rmsvalmin = rmsval;
            pmin = p;
        }
        if(rmsval>rmsvalmax)
            rmsvalmax = rmsval;

        coarsermsarray[p] = rmsval;

        printf("%20ld  %20g     [ %20ld  %20g ]\n", p, (double) rmsval, pmin, rmsvalmin);
        fp = fopen("wfscampe_coarse.txt","a");
        fprintf(fp, "%20ld %20g\n", p, (double) rmsval);
        fclose(fp);
    }

    level1 = rmsvalmin + 0.2*(rmsvalmax-rmsvalmin);
    level1OK = 0; /// toggles to 1 when curve first goes above level1

    level2 = rmsvalmin + 0.8*(rmsvalmax-rmsvalmin);
    level2OK = 0; /// toggles to 1 when curve first goes above level2 after level1OK

    level3 = rmsvalmin + 0.2*(rmsvalmax-rmsvalmin);
    level3OK = 0; /// toggles to 1 when curve first goes above level3 after level2OK

    p = p0;
    p1 = 0;
    lOK = 0;
    rmsvalmin1 = rmsvalmax;
    while((lOK==0)&&(p<pmax))
    {
        if(level1OK==0)
            if(coarsermsarray[p]>level1)
            {
                level1OK = 1;
                pp1 = p;
            }

        if((level1OK==1)&&(level2OK==0))
            if(coarsermsarray[p]>level2)
            {
                level2OK = 1;
                pp2 = p;
            }

        if((level1OK==1)&&(level2OK==1)&&(level3OK==0))
            if(coarsermsarray[p]<level3)
            {
                pp3 = p;
                level3OK = 1;
            }

        if((level1OK==1)&&(level2OK==1)&&(level3OK==1))
        {
            if(coarsermsarray[p] < rmsvalmin1)
            {
                rmsvalmin1 = coarsermsarray[p];
                p1 = p;
            }

            if(coarsermsarray[p]>level2)
                lOK = 1;
        }
        p++;
    }

    free(coarsermsarray);


    printf("APPROXIMATE PERIOD = %ld   [%ld %ld %ld]  [%f %f %f]\n", p1, pp1, pp2, pp3, level1, level2, level3);

    // find periodicity ( fine search )

    periodopt = 0.0;
    rmsvalmax = 0.0;

    fp = fopen("wfscampe.txt","w");
    fclose(fp);

    period_start = 1.0*p1 - 15.0;
    period_end = 1.0*p1 + 15.0;

    phacnt = (long*) malloc(sizeof(long)*NBpha);
    period_step = (period_end-period_start)/300.0;
    for(period=period_start; period<period_end; period += period_step)
    {
        for(kk=0; kk<NBpha; kk++)
            phacnt[kk] = 0;

        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            data.image[IDout].array.F[phal*AOconf[loop].sizeWFS+ii] = 0.0;

        for(kk=0; kk<NBframes; kk++)
        {
            pha = 1.0*kk/period;
            pha = modf(pha, &intpart);
            phal = (long) (1.0*NBpha*pha);

            if(phal>NBpha-1)
                phal = NBpha-1;
            if(phal<0)
                phal = 0;

            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                data.image[IDout].array.F[phal*AOconf[loop].sizeWFS+ii] += data.image[IDrc].array.F[kk*AOconf[loop].sizeWFS+ii];

            phacnt[phal]++;
        }

        rmsval = 0.0;
        cnt = 0;
        for(kk=0; kk<NBpha; kk++)
        {
            if(phacnt[kk]>0)
            {
                cnt++;
                for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
                {
                    data.image[IDout].array.F[kk*AOconf[loop].sizeWFS+ii] /= phacnt[kk];
                    rmsval = data.image[IDout].array.F[kk*AOconf[loop].sizeWFS+ii]*data.image[IDout].array.F[kk*AOconf[loop].sizeWFS+ii];
                }
            }
        }


        rmsval = sqrt(rmsval/AOconf[loop].sizeWFS/cnt);
        if(rmsval>rmsvalmax)
        {
            rmsvalmax = rmsval;
            periodopt = period;
        }
        printf("%20f  %20g     [ %20f  %20g ]\n", period, (double) rmsval, periodopt, rmsvalmax);
        fp = fopen("wfscampe.txt","a");
        fprintf(fp, "%20f %20g\n", period, (double) rmsval);
        fclose(fp);
    }

    printf("EXACT PERIOD = %f\n", periodopt);

    kw = 0;
    sprintf(kname, "PERIOD");
    strcpy(data.image[IDout].kw[kw].name, kname);
    data.image[IDout].kw[kw].type = 'D';
    data.image[IDout].kw[kw].value.numf = (double) periodopt;
    sprintf(comment, "WFS cam error period");
    strcpy(data.image[IDout].kw[kw].comment, comment);


    /// building phase cube
    period = periodopt;

    for(kk=0; kk<NBpha; kk++)
        phacnt[kk] = 0;
    for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
        data.image[IDout].array.F[phal*AOconf[loop].sizeWFS+ii] = 0.0;

    for(kk=0; kk<NBframes; kk++)
    {
        pha = 1.0*kk/period;
        pha = modf(pha, &intpart);
        phal = (long) (1.0*NBpha*pha);

        if(phal>NBpha-1)
            phal = NBpha-1;
        if(phal<0)
            phal = 0;

        for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            data.image[IDout].array.F[phal*AOconf[loop].sizeWFS+ii] += data.image[IDrc].array.F[kk*AOconf[loop].sizeWFS+ii];
        phacnt[phal]++;
    }

    rmsval = 0.0;
    cnt = 0;
    for(kk=0; kk<NBpha; kk++)
    {
        if(phacnt[kk]>0)
        {
            cnt++;
            for(ii=0; ii<AOconf[loop].sizeWFS; ii++)
            {
                data.image[IDout].array.F[kk*AOconf[loop].sizeWFS+ii] /= phacnt[kk];
            }
        }
    }



    free(phacnt);

    return(0);
}
*/






/** remove WFS camera periodic error
 *
 * pha: phase from 0.0 to 1.0
 */
/*
int_fast8_t AOloopControl_Remove_WFScamPE(char *IDin_name, char *IDcorr_name, double pha)
{
    long IDin;
    long IDcorr;
    long phal;
    long xsize, ysize, zsize, xysize;
    long ii;


    IDin = image_ID(IDin_name);
    IDcorr = image_ID(IDcorr_name);

    xsize = data.image[IDcorr].md[0].size[0];
    ysize = data.image[IDcorr].md[0].size[1];
    zsize = data.image[IDcorr].md[0].size[2];
    xysize = xsize*ysize;

    phal = (long) (1.0*pha*zsize);
    if(phal>zsize-1)
        phal -= zsize;



    for(ii=0; ii<xysize; ii++) {
        data.image[IDin].array.F[ii] -= data.image[IDcorr].array.F[xysize*phal+ii];
    }


    return(0);
}
*/











