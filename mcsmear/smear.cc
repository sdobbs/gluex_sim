// $Id$
//
// Created June 22, 2005  David Lawrence

#include "smear.h"

#include <iostream>
#include <iomanip>
#include <vector>
using namespace std;

#include <FCAL/DFCALGeometry.h>
#include <CCAL/DCCALGeometry.h>
#include <BCAL/DBCALGeometry.h>

#include <math.h>
#include "units.h"
#include <TF1.h>
#include <TH2.h>

#include "DRandom2.h"

#include "mcsmear_globals.h"

#ifndef _DBG_
#define _DBG_ cout<<__FILE__<<":"<<__LINE__<<" "
#define _DBG__ cout<<__FILE__<<":"<<__LINE__<<endl
#endif

void InitCDCGeometry(void);
void InitFDCGeometry(void);

pthread_mutex_t mutex_fdc_smear_function = PTHREAD_MUTEX_INITIALIZER;

bool CDC_GEOMETRY_INITIALIZED = false;
int CDC_MAX_RINGS=0;

DFCALGeometry *fcalGeom = NULL;
DCCALGeometry *ccalGeom = NULL;
bool FDC_GEOMETRY_INITIALIZED = false;
unsigned int NFDC_WIRES_PER_PLANE;

double SampleGaussian(double sigma);
double SamplePoisson(double lambda);
double SampleRange(double x1, double x2);


// Polynomial interpolation on a grid.
// Adapted from Numerical Recipes in C (2nd Edition), pp. 121-122.
static void polint(float *xa, float *ya,int n,float x, float *y,float *dy){
  int i,m,ns=0;
  float den,dif,dift,ho,hp,w;

  float *c=(float *)calloc(n,sizeof(float));
  float *d=(float *)calloc(n,sizeof(float));

  dif=fabs(x-xa[0]);
  for (i=0;i<n;i++){
    if ((dift=fabs(x-xa[i]))<dif){
      ns=i;
      dif=dift;
    }
    c[i]=ya[i];
    d[i]=ya[i];
  }
  *y=ya[ns--];

  for (m=1;m<n;m++){
    for (i=1;i<=n-m;i++){
      ho=xa[i-1]-x;
      hp=xa[i+m-1]-x;
      w=c[i+1-1]-d[i-1];
      if ((den=ho-hp)==0.0){
   free(c);
   free(d);
   return;
      }
  
      den=w/den;
      d[i-1]=hp*den;
      c[i-1]=ho*den;
      
    }
    
    *y+=(*dy=(2*ns<(n-m) ?c[ns+1]:d[ns--]));
  }
  free(c);
  free(d);
}

//-----------
// Smear
//-----------
void Smear::SmearEvent(hddm_s::HDDM *record)
{
    GetAndSetSeeds(record);

	SmearCDC(record);
    SmearFDC(record);
    SmearFCAL(record);
    SmearCCAL(record);
    //SmearBCAL(record);
    dBCALSmearer->SmearEvent(record);
    SmearTOF(record);
    SmearSTC(record);
    SmearCherenkov(record);
    SmearTAGM(record);
    SmearTAGH(record);
    SmearPS(record);
    SmearPSC(record);
    SmearFMWPC(record);
}

//-----------
// SetSeeds
//-----------
void Smear::SetSeeds(const char *vals)
{
   /// This is called from the command line parser to
   /// set the initial seeds based on user input from
   /// the command line.
   //
   //
   stringstream ss(vals);
   Int_t seed1, seed2, seed3;
   ss >> seed1 >> seed2 >> seed3;
   UInt_t *useed1 = reinterpret_cast<UInt_t*>(&seed1);
   UInt_t *useed2 = reinterpret_cast<UInt_t*>(&seed2);
   UInt_t *useed3 = reinterpret_cast<UInt_t*>(&seed3);
   gDRandom.SetSeeds(*useed1, *useed2, *useed3);

   cout << "Seeds set from command line. Any random number" << endl;
   cout << "seeds found in the input file will be ignored!" << endl;
   IGNORE_SEEDS = true;
}

//-----------
// GetAndSetSeeds
//-----------
void Smear::GetAndSetSeeds(hddm_s::HDDM *record)
{
   // Check if non-zero seed values exist in the input HDDM file.
   // If so, use them to set the seeds for the random number
   // generator. Otherwise, make sure the seeds that are used
   // are stored in the output event.
   
   if (record == 0)
      return;
   else if (record->getReactions().size() == 0)
      return;

   hddm_s::ReactionList::iterator reiter = record->getReactions().begin();
   if (reiter->getRandoms().size() == 0) {
      // No seeds stored in event. Add them
      hddm_s::RandomList blank_rand = reiter->addRandoms();
      blank_rand().setSeed1(0);
      blank_rand().setSeed2(0);
      blank_rand().setSeed3(0);
      blank_rand().setSeed4(0);
   }

   UInt_t seed1, seed2, seed3;
   hddm_s::Random my_rand = reiter->getRandom();

   if (!IGNORE_SEEDS) {
      // Copy seeds from event record to local variables
      seed1 = my_rand.getSeed1();
      seed2 = my_rand.getSeed2();
      seed3 = my_rand.getSeed3();
      
      // If the seeds in the event are all zeros it means they
      // were not set. In this case, initialize seeds to constants
      // to guarantee the seeds are used if this input file were
      // smeared again with the same command a second time. These
      // are set here to the fractional part of the cube roots of
      // the first three primes, truncated to 9 digits.
      if ((seed1 == 0) || (seed2 == 0) || (seed3 == 0)){
         uint64_t eventNo = record->getPhysicsEvent().getEventNo();
         seed1 = 259921049 + eventNo;
         seed2 = 442249570 + eventNo;
         seed3 = 709975946 + eventNo;
      }
      
      // Set the seeds in the random generator.
      gDRandom.SetSeeds(seed1, seed2, seed3);
   }

   // Copy seeds from generator to local variables
   gDRandom.GetSeeds(seed1, seed2, seed3);

   // Copy seeds from local variables to event record
   my_rand.setSeed1(seed1);
   my_rand.setSeed2(seed2);
   my_rand.setSeed3(seed3);
}

//-----------
// SmearCDC
//-----------
void Smear::SmearCDC(hddm_s::HDDM *record)
{
   /// Smear the drift times of all CDC hits.
   /// This will add cdcStrawHit objects generated by smearing values in the
   /// cdcStrawTruthHit objects that hdgeant outputs. Any existing cdcStrawHit
   /// objects will be replaced.

   double t_max = config->TRIGGER_LOOKBACK_TIME + cdc_config->CDC_TIME_WINDOW;
   double threshold = cdc_config->CDC_THRESHOLD_FACTOR * cdc_config->CDC_PEDESTAL_SIGMA; // for sparsification

   // Loop over all cdcStraw tags
   hddm_s::CdcStrawList straws = record->getCdcStraws();
   hddm_s::CdcStrawList::iterator iter;
   for (iter = straws.begin(); iter != straws.end(); ++iter) {
 
      // If the element already contains a cdcStrawHit list then delete it.
      hddm_s::CdcStrawHitList hits = iter->getCdcStrawHits();
      if (hits.size() > 0) {
         static bool warned = false;
         iter->deleteCdcStrawHits();
         if (!warned) {
            warned = true;
            cerr << endl;
            cerr << "WARNING: CDC hits already exist in input file! Overwriting!"
                 << endl << endl;
         }
      }

      // Create new cdcStrawHit from cdcStrawTruthHit information
      hddm_s::CdcStrawTruthHitList thits = iter->getCdcStrawTruthHits();
      hddm_s::CdcStrawTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++ titer) {
         // Pedestal-smeared charge
         double q = titer->getQ() + SampleGaussian(cdc_config->CDC_PEDESTAL_SIGMA);
         // Smear out the CDC drift time using the specified sigma.
         // This is for timing resolution from the electronics;
         // diffusion is handled in hdgeant.
         double t = titer->getT() + SampleGaussian(cdc_config->CDC_TDRIFT_SIGMA)*1.0e9;
         if (t > config->TRIGGER_LOOKBACK_TIME && t < t_max && q > threshold) {
            hits = iter->addCdcStrawHits();
            hits().setT(t);
            hits().setQ(q);
         }

         if (config->DROP_TRUTH_HITS) {
            iter->deleteCdcStrawTruthHits();
         }
      }
   }
}


//-----------
// SmearFDC
//-----------
void Smear::SmearFDC(hddm_s::HDDM *record)
{
   double t_max = config->TRIGGER_LOOKBACK_TIME + fdc_config->FDC_TIME_WINDOW;
   double threshold = fdc_config->FDC_THRESHOLD_FACTOR * fdc_config->FDC_PED_NOISE; // for sparsification

   hddm_s::FdcChamberList chambers = record->getFdcChambers();
   hddm_s::FdcChamberList::iterator iter;
   for (iter = chambers.begin(); iter != chambers.end(); ++iter) {

      // Add pedestal noise to strip charge data
      hddm_s::FdcCathodeStripList strips = iter->getFdcCathodeStrips();
      hddm_s::FdcCathodeStripList::iterator siter;
      for (siter = strips.begin(); siter != strips.end(); ++siter) {
          // If a fdcCathodeHit already exists delete it
          siter->deleteFdcCathodeHits();
          hddm_s::FdcCathodeTruthHitList thits = 
                                         siter->getFdcCathodeTruthHits();
          hddm_s::FdcCathodeTruthHitList::iterator titer;
          for (titer = thits.begin(); titer != thits.end(); ++titer) {
            double q = titer->getQ() + SampleGaussian(fdc_config->FDC_PED_NOISE);
            double t = titer->getT() +
                       SampleGaussian(fdc_config->FDC_TDRIFT_SIGMA)*1.0e9;
            if (q > threshold && t > config->TRIGGER_LOOKBACK_TIME && t < t_max) {
               hddm_s::FdcCathodeHitList hits = siter->addFdcCathodeHits();
               hits().setQ(q);
               hits().setT(t);
            }
            fdc_cathode_charge->Fill(q);
         }

         if (config->DROP_TRUTH_HITS)
            siter->deleteFdcCathodeTruthHits();
      }

      // Add drift time varation to the anode data 
      hddm_s::FdcAnodeWireList wires = iter->getFdcAnodeWires();
      hddm_s::FdcAnodeWireList::iterator witer;
      for (witer = wires.begin(); witer != wires.end(); ++witer) {
         // If a fdcAnodeHit exists already delete it
         witer->deleteFdcAnodeHits();
         hddm_s::FdcAnodeTruthHitList thits = witer->getFdcAnodeTruthHits();
         hddm_s::FdcAnodeTruthHitList::iterator titer;
         for (titer = thits.begin(); titer != thits.end(); ++titer) {
            double t = titer->getT() + SampleGaussian(fdc_config->FDC_TDRIFT_SIGMA)*1.0e9;
            if (t > config->TRIGGER_LOOKBACK_TIME && t < t_max) {
               hddm_s::FdcAnodeHitList hits = witer->addFdcAnodeHits();
               hits().setT(t);
               hits().setDE(titer->getDE());
            }
         }
         fdc_anode_mult->Fill(witer->getFdcAnodeHits().size());

         if (config->DROP_TRUTH_HITS)
            witer->deleteFdcAnodeTruthHits();
      }
   }
}

//-----------
// SmearFCAL
//-----------
void Smear::SmearFCAL(hddm_s::HDDM *record)
{
   /// Smear the FCAL hits using the nominal resolution of the individual blocks.
   /// The way this works is a little funny and warrants a little explanation.
   /// The information coming from hdgeant is truth information indexed by 
   /// row and column, but containing energy deposited and time. The mcsmear
   /// program will copy the truth information from the fcalTruthHit element
   /// to a new fcalHit element, smearing the values with the appropriate detector
   /// resolution.
   ///
   /// To access the "truth" values in DANA, get the DFCALHit objects using the
   /// "TRUTH" tag.
   
   if (!fcalGeom)
      fcalGeom = new DFCALGeometry();

   hddm_s::FcalBlockList blocks = record->getFcalBlocks();
   hddm_s::FcalBlockList::iterator iter;
   for (iter = blocks.begin(); iter != blocks.end(); ++iter) {
      iter->deleteFcalHits();
      hddm_s::FcalTruthHitList thits = iter->getFcalTruthHits();
      hddm_s::FcalTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++titer) {
         // Simulation simulates a grid of blocks for simplicity. 
         // Do not bother smearing inactive blocks. They will be
         // discarded in DEventSourceHDDM.cc while being read in
         // anyway.
         if (!fcalGeom->isBlockActive(iter->getRow(), iter->getColumn()))
            continue;
         // Smear the energy and timing of the hit
         double sigma = fcal_config->FCAL_PHOT_STAT_COEF/sqrt(titer->getE());
         double E = titer->getE() * (1.0 + SampleGaussian(sigma));
         // Smear the time by 200 ps (fixed for now) 7/2/2009 DL
         double t = titer->getT() + SampleGaussian(fcal_config->FCAL_TSIGMA); 
         // Apply a single block threshold. 
         if (E >= fcal_config->FCAL_BLOCK_THRESHOLD) {
            hddm_s::FcalHitList hits = iter->addFcalHits();
            hits().setE(E);
            hits().setT(t);
         }
      }

      if (DROP_TRUTH_HITS)
         iter->deleteFcalTruthHits();
   }
}

//-----------
// SmearCCAL
//-----------
void Smear::SmearCCAL(hddm_s::HDDM *record)
{
   /// Smear the CCAL hits using the same procedure as the FCAL above.
   /// See those comments for details.

   if (!ccalGeom)
      ccalGeom = new DCCALGeometry();

   hddm_s::CcalBlockList blocks = record->getCcalBlocks();   
   hddm_s::CcalBlockList::iterator iter;
   for (iter = blocks.begin(); iter != blocks.end(); ++iter) {
      iter->deleteCcalHits();
      hddm_s::CcalTruthHitList thits = iter->getCcalTruthHits();   
      hddm_s::CcalTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++titer) {
         // Simulation simulates a grid of blocks for simplicity. 
         // Do not bother smearing inactive blocks. They will be
         // discarded in DEventSourceHDDM.cc while being read in
         // anyway.
         if (!ccalGeom->isBlockActive(iter->getRow(), iter->getColumn()))
            continue;
         // Smear the energy and timing of the hit
         double sigma = ccal_config->CCAL_PHOT_STAT_COEF/sqrt(titer->getE()) ;
         double E = titer->getE() * (1.0 + SampleGaussian(sigma));
         // Smear the time by 200 ps (fixed for now) 7/2/2009 DL
         double t = titer->getT() + SampleGaussian(ccal_config->CCAL_SIGMA);
         // Apply a single block threshold. If the (smeared) energy is below this,
         // then set the energy and time to zero. 
         if (E > ccal_config->CCAL_BLOCK_THRESHOLD) {
            hddm_s::CcalHitList hits = iter->addCcalHits();
            hits().setE(E);
            hits().setT(t);
         }
      }

      if (config->DROP_TRUTH_HITS)
         iter->deleteCcalTruthHits();
   }
}

//-----------
// SmearTOF
//-----------
void SmearTOF(hddm_s::HDDM *record)
{
   hddm_s::FtofCounterList tofs = record->getFtofCounters();
   hddm_s::FtofCounterList::iterator iter;
   for (iter = tofs.begin(); iter != tofs.end(); ++iter) {
      // take care of hits
      iter->deleteFtofHits();
      hddm_s::FtofTruthHitList thits = iter->getFtofTruthHits();
      hddm_s::FtofTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++titer) {
         // Smear the time
         double t = titer->getT() + SampleGaussian(ftof_config->TOF_SIGMA);
         // Smear the energy
         double npe = titer->getDE() * 1000. * ftof_config->TOF_PHOTONS_PERMEV;
         npe = npe +  SampleGaussian(sqrt(npe));
         float NewE = npe/ftof_config->TOF_PHOTONS_PERMEV/1000.;
         if (NewE > ftof_config->TOF_BAR_THRESHOLD) {
            hddm_s::FtofHitList hits = iter->addFtofHits();
            hits().setEnd(titer->getEnd());
            hits().setT(t);
            hits().setDE(NewE);
         }
      }
    
      if (config->DROP_TRUTH_HITS) {
         iter->deleteFtofTruthHits();
      }
   }
}

//-----------
// SmearSTC - smear hits in the start counter
//-----------
void Smear::SmearSTC(hddm_s::HDDM *record)
{
   hddm_s::StcPaddleList pads = record->getStcPaddles();
   hddm_s::StcPaddleList::iterator iter;
   for (iter = pads.begin(); iter != pads.end(); ++iter) {
      iter->deleteStcHits();
      hddm_s::StcTruthHitList thits = iter->getStcTruthHits();
      hddm_s::StcTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++titer) {
         // smear the time
         double t = titer->getT() + SampleGaussian(sc_config->START_SIGMA);
         // smear the energy
         double npe = titer->getDE() * 1000. *  sc_config->START_PHOTONS_PERMEV;
         npe = npe +  SampleGaussian(sqrt(npe));
         double NewE = npe/sc_config->START_PHOTONS_PERMEV/1000.;
         if (NewE > sc_config->START_PADDLE_THRESHOLD) {
            hddm_s::StcHitList hits = iter->addStcHits();
            hits().setT(t);
            hits().setDE(NewE);
         }
      }

      if (config->DROP_TRUTH_HITS)
         iter->deleteStcTruthHits();
   }
}

//-----------
// SmearCherenkov
//-----------
void Smear::SmearCherenkov(hddm_s::HDDM *record)
{
}

//-----------
// SmearTAGM
//-----------
void Smear::SmearTAGM(hddm_s::HDDM *record)
{
   hddm_s::MicroChannelList tagms = record->getMicroChannels();
   hddm_s::MicroChannelList::iterator iter;
   for (iter = tagms.begin(); iter != tagms.end(); ++iter) {
      iter->deleteTaggerHits();
      hddm_s::TaggerTruthHitList thits = iter->getTaggerTruthHits();
      hddm_s::TaggerTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++titer) {
         // smear the time
         double t = titer->getT() + SampleGaussian(tagm_config->TAGM_TSIGMA);
         double tADC = titer->getT() + SampleGaussian(tagm_config->TAGM_FADC_TSIGMA);
         double npe = SamplePoisson(titer->getDE() * tagm_config->TAGM_NPIX_PER_GEV);
         hddm_s::TaggerHitList hits = iter->addTaggerHits();
         hits().setT(t);
         hits().setTADC(tADC);
         hits().setNpe(npe);
      }

      if (config->DROP_TRUTH_HITS)
         iter->deleteTaggerTruthHits();
   }
}

//-----------
// SmearTAGH
//-----------
void Smear::SmearTAGH(hddm_s::HDDM *record)
{
   hddm_s::HodoChannelList taghs = record->getHodoChannels();
   hddm_s::HodoChannelList::iterator iter;
   for (iter = taghs.begin(); iter != taghs.end(); ++iter) {
      iter->deleteTaggerHits();
      hddm_s::TaggerTruthHitList thits = iter->getTaggerTruthHits();
      hddm_s::TaggerTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++titer) {
         // smear the time
         double t = titer->getT() + SampleGaussian(tagh_config->TAGH_TSIGMA);
         double tADC = titer->getT() + SampleGaussian(tagh_config->TAGH_FADC_TSIGMA);
         double npe = SamplePoisson(titer->getDE() * tagh_config->TAGH_NPE_PER_GEV);
         hddm_s::TaggerHitList hits = iter->addTaggerHits();
         hits().setT(t);
         hits().setTADC(tADC);
         hits().setNpe(npe);
      }

      if (config->DROP_TRUTH_HITS)
         iter->deleteTaggerTruthHits();
   }
}

//-----------
// SmearPS - smear hits in the pair spectrometer fine counters
//-----------
void Smear::SmearPS(hddm_s::HDDM *record)
{
   hddm_s::PsTileList tiles = record->getPsTiles();
   hddm_s::PsTileList::iterator iter;
   for (iter = tiles.begin(); iter != tiles.end(); ++iter) {
      iter->deletePsHits();
      hddm_s::PsTruthHitList thits = iter->getPsTruthHits();
      hddm_s::PsTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++titer) {
         // smear the time
         double t = titer->getT() + SampleGaussian(ps_config->PS_SIGMA);
         // convert energy deposition in number of fired pixels
         double npe = SamplePoisson(titer->getDE() * ps_config->PS_NPIX_PER_GEV);
	 hddm_s::PsHitList hits = iter->addPsHits();
	 hits().setT(t);
	 hits().setDE(npe/ps_config->PS_NPIX_PER_GEV);
      }

      if (config->DROP_TRUTH_HITS)
         iter->deletePsTruthHits();
   }
}

//-----------
// SmearPSC - smear hits in the pair spectrometer coarse counters
//-----------
void Smear::SmearPSC(hddm_s::HDDM *record)
{
   hddm_s::PscPaddleList paddles = record->getPscPaddles();
   hddm_s::PscPaddleList::iterator iter;
   for (iter = paddles.begin(); iter != paddles.end(); ++iter) {
      iter->deletePscHits();
      hddm_s::PscTruthHitList thits = iter->getPscTruthHits();
      hddm_s::PscTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++titer) {
         // smear the time
         double t = titer->getT() + SampleGaussian(psc_config->PSC_SIGMA);
         // smear the energy
         double npe = titer->getDE() * 1000. *  psc_config->PSC_PHOTONS_PERMEV;
         npe = npe +  SampleGaussian(sqrt(npe));
         double NewE = npe/psc_config->PSC_PHOTONS_PERMEV/1000.;
         if (NewE > psc_config->PSC_THRESHOLD) {
            hddm_s::PscHitList hits = iter->addPscHits();
            hits().setT(t);
            hits().setDE(NewE);
         }
      }

      if (config->DROP_TRUTH_HITS)
         iter->deletePscTruthHits();
   }
}

//-----------
// SmearFMWPC - smear hits in the forward MWPC
//-----------
void Smear::SmearFMWPC(hddm_s::HDDM *record)
{
   hddm_s::FmwpcChamberList chambers = record->getFmwpcChambers();
   hddm_s::FmwpcChamberList::iterator iter;
   for (iter = chambers.begin(); iter != chambers.end(); ++iter) {
      iter->deleteFmwpcHits();
      hddm_s::FmwpcTruthHitList thits = iter->getFmwpcTruthHits();
      hddm_s::FmwpcTruthHitList::iterator titer;
      for (titer = thits.begin(); titer != thits.end(); ++titer) {
         // smear the time and energy
         double t = titer->getT() + SampleGaussian(fmwpc_config->FMWPC_TSIGMA);
         double dE = titer->getDE() + SampleGaussian(fmwpc_config->FMWPC_ASIGMA);
         if (dE > fmwpc_config->FMWPC_THRESHOLD) {
            hddm_s::FmwpcHitList hits = iter->addFmwpcHits();
            hits().setT(t);
            hits().setDE(dE);
         }
      }

      if (config->DROP_TRUTH_HITS)
         iter->deleteFmwpcTruthHits();
   }
}

