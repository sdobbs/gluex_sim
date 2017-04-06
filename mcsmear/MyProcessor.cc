// Author: David Lawrence  Sat Jan 29 09:37:37 EST 2011
//
//
// MyProcessor.cc
//

#include <iostream>
#include <cmath>
#include <vector>
#include <map>

using namespace std;

#include <strings.h>

#include "MyProcessor.h"
#include "hddm_s_merger.h"

#include <JANA/JEvent.h>

#include <HDDM/DEventSourceHDDM.h>
#include <TRACKING/DMCThrown.h>
#include <DRandom2.h>

extern char *OUTFILENAME;
extern std::map<hddm_s::istream*,double> files2merge;
extern std::map<hddm_s::istream*,hddm_s::streamposition> start2merge;

static pthread_mutex_t output_file_mutex;
static pthread_t output_file_mutex_last_owner;
static pthread_mutex_t input_file_mutex;
static pthread_t input_file_mutex_last_owner;

#include <JANA/JCalibration.h>
//static JCalibration *jcalib=NULL;
static bool locCheckCCDBContext = true;

//-----------
// PrintCCDBWarning
//-----------
static void PrintCCDBWarning(string context)
{
	jout << endl;
	jout << "===============================================================================" << endl;
	jout << "                            !!!!! WARNING !!!!!" << endl;
	jout << "You have either not specified a CCDB variation, or specified a variation" << endl;
	jout << "that appears inconsistent with processing simulated data." << endl;
	jout << "Be sure that this is what you want to do!" << endl;
	jout << endl;
	jout << "  JANA_CALIB_CONTEXT = " << context << endl;
	jout << endl;
	jout << "For a more detailed list of CCDB variations used for simulations" << endl;
	jout << "see the following wiki page:" << endl;
	jout << endl;
	jout << "   https://halldweb.jlab.org/wiki/index.php/Simulations#Simulation_Conditions" << endl;
	jout << "===============================================================================" << endl;
	jout << endl;
}



void mcsmear_thread_HUP_sighandler(int sig)
{
   jerr<<" Caught HUP signal for thread 0x"<<hex<<pthread_self()<<dec<<" thread exiting..."<<endl;

   // We use output_file_mutex_owner to keep track (sort of)
   // of which thread has the mutex locked. This mutex is only
   // locked at the end of the evnt method. Once the lock is
   // obtained, this value is set to hold the id of the thread
   // that locked it. It may help in debugging to know the last
   // known thread to have locked the mutex when the signal
   // handler was called
   jerr<<endl;
   jerr<<" Last thread to lock output file mutex: 0x"<<hex<<pthread_self()<<dec<<endl;
   jerr<<" Attempting to unlock mutex to avoid deadlock." <<endl;
   jerr<<" However, the output file is likely corrupt at" <<endl;
   jerr<<" this point and the process should be restarted ..." <<endl;
   jerr<<endl;
   pthread_mutex_unlock(&output_file_mutex);
   pthread_exit(NULL);
}


//------------------------------------------------------------------
// init   -Open output file 
//------------------------------------------------------------------
jerror_t MyProcessor::init(void)
{
   // open HDDM file
   ofs = new ofstream(OUTFILENAME);
   if (!ofs->is_open()){
      cout<<" Error opening output file \""<<OUTFILENAME<<"\"!"<<endl;
      exit(-1);
   }
   fout = new hddm_s::ostream(*ofs);
   Nevents_written = 0;

   HDDM_USE_COMPRESSION = 2;
   gPARMS->SetDefaultParameter("HDDM:USE_COMPRESSION", HDDM_USE_COMPRESSION,
                          "Turn on/off compression of the output HDDM stream."
                          " \"0\"=no compression, \"1\"=bz2 compression, \"2\"=z compression (default)");
   HDDM_USE_INTEGRITY_CHECKS = true;
   gPARMS->SetDefaultParameter("HDDM:USE_INTEGRITY_CHECKS",
                                HDDM_USE_INTEGRITY_CHECKS,
                          "Turn on/off automatic integrity checking on the"
                          " output HDDM stream."
                          " Set to \"0\" to turn off (it's on by default)");

   // enable on-the-fly bzip2 compression on output stream
   if (HDDM_USE_COMPRESSION == 0) {
      jout << " HDDM compression disabled" << std::endl;
   } else if (HDDM_USE_COMPRESSION == 1) {
      jout << " Enabling bz2 compression of output HDDM file stream" 
           << std::endl;
      fout->setCompression(hddm_s::k_bz2_compression);
   } else {
      jout << " Enabling z compression of output HDDM file stream (default)" 
           << std::endl;
      fout->setCompression(hddm_s::k_z_compression);
   }

   // enable a CRC data integrity check at the end of each event record
   if (HDDM_USE_INTEGRITY_CHECKS) {
      jout << " Enabling CRC data integrity check in output HDDM file stream"
           << std::endl;
      fout->setIntegrityChecks(hddm_s::k_crc32_integrity);
   }
   else {
      jout << " HDDM integrity checks disabled" << std::endl;
   }

   // We set the mutex type to "ERRORCHECK" so that if the
   // signal handler is called, we can unlock the mutex
   // safely whether we have it locked or not.
   pthread_mutexattr_t attr;
   pthread_mutexattr_init(&attr);
   pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
   pthread_mutex_init(&output_file_mutex, NULL);
   pthread_mutex_init(&input_file_mutex, NULL);
   
   // pthreads does not provide an "invalid" value for 
   // a pthread_t that we can initialize with. Furthermore,
   // the pthread_t may be a simple as an integer or as
   // a complicated structure. Hence, to make this portable
   // we clear it with bzero.
   bzero(&output_file_mutex_last_owner, sizeof(pthread_t));
   bzero(&input_file_mutex_last_owner, sizeof(pthread_t));
   
   return NOERROR;
}

jerror_t MyProcessor::brun(JEventLoop *loop, int locRunNumber)
{
    // Generally, simulations should be generated and analyzed with a non-default
    // set of calibrations, since the calibrations needed for simulations are
    // different than those needed for data.  
    // Conventionally, the CCDB variations needed for simulations start with the 
    // string "mc".  To guard against accidentally not setting the variation correctly
    // we check to see if the variation is set and if it contains the string "mc".
    // Note that for now, we only print a warning and do not exit immediately.
    // It might be advisable to apply some tougher love.
    jout << "In MyProcessor::brun()" << endl;
    if(locCheckCCDBContext) {
        // only do this once
        locCheckCCDBContext = false;        

        // load the CCDB context
        DApplication* locDApp = dynamic_cast<DApplication*>(japp);
        //DGeometry *dgeom=locDApp->GetDGeometry(locRunNumber);
        JCalibration* jcalib = locDApp->GetJCalibration(locRunNumber);
    
        string context = jcalib->GetContext();
      
        jout << "checking context = " << context << endl;

        // Really we should parse the context string, but since "mc" shouldn't show up
        // outside of the context, we just search the whole string.
        // Also make sure that the variation is being set
        if( (context.find("variation") == string::npos) || (context.find("mc") == string::npos) ) {
            PrintCCDBWarning(context);
        }

        std::map<string, float> parms;
        jcalib->Get("CDC/cdc_parms", parms);
        hddm_s_merger::set_cdc_min_delta_t_ns(parms.at("CDC_TWO_HIT_RESOL"));
        jcalib->Get("TOF/tof_parms", parms);
        hddm_s_merger::set_ftof_min_delta_t_ns(parms.at("TOF_TWO_HIT_RESOL"));
        jcalib->Get("FDC/fdc_parms", parms);
        hddm_s_merger::set_fdc_min_delta_t_ns(parms.at("FDC_TWO_HIT_RESOL"));
        jcalib->Get("START_COUNTER/start_parms", parms);
        hddm_s_merger::set_stc_min_delta_t_ns(parms.at("START_TWO_HIT_RESOL"));
        jcalib->Get("BCAL/bcal_parms", parms);
        hddm_s_merger::set_bcal_min_delta_t_ns(parms.at("BCAL_TWO_HIT_RESOL"));
        jcalib->Get("FCAL/fcal_parms", parms);
        hddm_s_merger::set_fcal_min_delta_t_ns(parms.at("FCAL_TWO_HIT_RESOL"));
    }
   

	// load configuration parameters for all the detectors
	if(smearer != NULL)
		delete smearer;
	smearer = new Smear(config, loop);

#ifdef HAVE_RCDB
	// Pull configuration parameters from RCDB
	config->ParseRCDBConfigFile(locRunNumber);
#endif  // HAVE_RCDB

    // rewind any merger input files to the beginning
    std::map<hddm_s::istream*,hddm_s::streamposition>::iterator iter;
    for (iter = start2merge.begin(); iter != start2merge.end(); ++iter) {
        iter->first->setPosition(iter->second);
    }

	return NOERROR;
}

//------------------------------------------------------------------
// evnt - Do processing for each event here
//------------------------------------------------------------------
jerror_t MyProcessor::evnt(JEventLoop *loop, uint64_t eventnumber)
{
   JEvent& event = loop->GetJEvent();
   JEventSource *source = event.GetJEventSource();
   DEventSourceHDDM *hddm_source = dynamic_cast<DEventSourceHDDM*>(source);
   if (!hddm_source) {
      cerr << " This program MUST be used with an HDDM file as input!" << endl;
      exit(-1);
   }
   hddm_s::HDDM *record = (hddm_s::HDDM*)event.GetRef();
   if (!record)
      return NOERROR;

   // Smear values
   smearer->SmearEvent(record);

   // Load any external events to be merged during smearing
   std::map<hddm_s::istream*,double>::iterator iter;
   for (iter = files2merge.begin(); iter != files2merge.end(); ++ iter) {
      int count = iter->second;
      if (count != iter->second) {
         count = gDRandom.Poisson(iter->second);
      }
      for (int i=0; i < count; ++i) {
         hddm_s::HDDM record2;
         *iter->first >> record2;
         if (iter->first->eof()) {
            pthread_mutex_lock(&input_file_mutex);
            input_file_mutex_last_owner = pthread_self();
            if (iter->first->eof())
               iter->first->setPosition(start2merge.at(iter->first));
            *iter->first >> *record;
            if (iter->first->eof()) {
               pthread_mutex_unlock(&input_file_mutex);
               std::cerr << "Trying to merge from empty input file, "
                         << "cannot continue!" << std::endl;
               exit(-1);
            }
            pthread_mutex_unlock(&input_file_mutex);
         }
         *record += record2;
      }
   }

   // Write event to output file
   pthread_mutex_lock(&output_file_mutex);
   output_file_mutex_last_owner = pthread_self();
   *fout << *record;
   Nevents_written++;
   pthread_mutex_unlock(&output_file_mutex);

   return NOERROR;
}

//------------------------------------------------------------------
// fini   -Close output file here
//------------------------------------------------------------------
jerror_t MyProcessor::fini(void)
{
   if (fout)
      delete fout;
   if (ofs) {
      ofs->close();
      cout << endl << "Closed HDDM file" << endl;
   }
   cout << " " << Nevents_written << " event written to " << OUTFILENAME
        << endl;
   
   return NOERROR;
}
