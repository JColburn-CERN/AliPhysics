/************************************************************************************
 * Copyright (C) 2019, Copyright Holders of the ALICE Collaboration                 *
 * All rights reserved.                                                             *
 *                                                                                  *
 * Redistribution and use in source and binary forms, with or without               *
 * modification, are permitted provided that the following conditions are met:      *
 *     * Redistributions of source code must retain the above copyright             *
 *       notice, this list of conditions and the following disclaimer.              *
 *     * Redistributions in binary form must reproduce the above copyright          *
 *       notice, this list of conditions and the following disclaimer in the        *
 *       documentation and/or other materials provided with the distribution.       *
 *     * Neither the name of the <organization> nor the                             *
 *       names of its contributors may be used to endorse or promote products       *
 *       derived from this software without specific prior written permission.      *
 *                                                                                  *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND  *
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED    *
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE           *
 * DISCLAIMED. IN NO EVENT SHALL ALICE COLLABORATION BE LIABLE FOR ANY              *
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES       *
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;     *
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND      *
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT       *
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS    *
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                     *
 ************************************************************************************/
#include <algorithm>
#include <iostream>

#include <THistManager.h>

#include "AliAnalysisManager.h"
#include "AliAnalysisTaskEmcalTriggerNormalization.h"
#include "AliCDBEntry.h"
#include "AliCDBManager.h"
#include "AliEmcalDownscaleFactorsOCDB.h"
#include "AliEmcalTriggerStringDecoder.h"
#include "AliInputEventHandler.h"
#include "AliLog.h"
#include "AliMultSelection.h"
#include "AliTriggerClass.h"
#include "AliTriggerConfiguration.h"
#include "AliVEvent.h"

ClassImp(PWG::EMCAL::AliAnalysisTaskEmcalTriggerNormalization)

using namespace PWG::EMCAL;

const std::vector<std::string> AliAnalysisTaskEmcalTriggerNormalization::fgkTriggerClusterLabels = {
  "ANY", "CENT", "CENTNOTRD", "CALO", "CENTBOTH", "OnlyCENT", "OnlyCENTNOTRD", "CALOBOTH", "OnlyCALO", "OnlyCALOFAST", "CENTNOPMD", "ALL", "ALLNOTRD",
  "ALLBOTH", "OnlyALL", "OnlyALLNOTRD"
};

int AliAnalysisTaskEmcalTriggerNormalization::GetIndexFromTriggerClusterLabel(EMCAL_STRINGVIEW triggerclusterlabel) {
  int index = -1;
  auto pos = std::find(fgkTriggerClusterLabels.begin(), fgkTriggerClusterLabels.end(), triggerclusterlabel.data());
  if(pos != fgkTriggerClusterLabels.end()) {
    index = pos - fgkTriggerClusterLabels.begin();
  }
  return index;
}

AliAnalysisTaskEmcalTriggerNormalization::AliAnalysisTaskEmcalTriggerNormalization():
    AliAnalysisTaskEmcal(),
    fHistos(nullptr),
    fMBTriggerClasses(),
    fCacheTriggerClasses(),
    fCacheDownscaleFactors(),
    fUseCentralityForpPb(false)
{
}

AliAnalysisTaskEmcalTriggerNormalization::AliAnalysisTaskEmcalTriggerNormalization(const char *name):
    AliAnalysisTaskEmcal(name, kTRUE),
    fHistos(nullptr),
    fMBTriggerClasses(),
    fCacheTriggerClasses(),
    fCacheDownscaleFactors(),
    fUseCentralityForpPb(false)
{
  SetMakeGeneralHistograms(true);
}

void AliAnalysisTaskEmcalTriggerNormalization::UserCreateOutputObjects(){
  AliAnalysisTaskEmcal::UserCreateOutputObjects();

  fHistos = new THistManager("HistosTriggerNorm");

  fHistos->CreateTH2("hTriggerNorm", "Histogram for the trigger normalization", 11, -0.5, 10.5, 100, 0., 100.);
  fHistos->CreateTH2("hTriggerLuminosity", "Histogram for the trigger luminosity (INT7-triggered cluster)", 11, -0.5, 10.5, 100, 0., 100.);
  auto normhist = static_cast<TH1 *>(fHistos->GetListOfHistograms()->FindObject("hTriggerNorm"));
  auto luminosityHist = static_cast<TH1 *>(fHistos->GetListOfHistograms()->FindObject("hTriggerLuminosity"));
  std::array<std::string, 9> triggers = {"INT7", "EG1", "EG2", "EJ1", "EJ2", "DG1", "DG2", "DJ1", "DJ2"};
  for(size_t ib = 0; ib < triggers.size(); ib++){
    normhist->GetXaxis()->SetBinLabel(ib+1, triggers[ib].data());
    luminosityHist->GetXaxis()->SetBinLabel(ib+1, triggers[ib].data());

    // Create cluster counter histogram for trigger
    fHistos->CreateTH1(Form("hClusterCounter%s", triggers[ib].data()), Form("Trigger cluster counter for trigger class %s", triggers[ib].data()), kTrgClusterN, -0.5, kTrgClusterN - 0.5);
    auto triggercluserhist = static_cast<TH1 *>(fHistos->GetListOfHistograms()->FindObject(Form("hClusterCounter%s", triggers[ib].data())));
    for(auto clusterindex = 0; clusterindex < TriggerCluster_t::kTrgClusterN; clusterindex++) {
      triggercluserhist->GetXaxis()->SetBinLabel(clusterindex+1, GetTriggerClusterLabels(clusterindex).data());
    }
  }

  for(auto h : *fHistos->GetListOfHistograms()) fOutput->Add(h);
  PostData(1, fOutput);
}

Bool_t AliAnalysisTaskEmcalTriggerNormalization::Run(){
  if(!fMBTriggerClasses.size()) throw MBTriggerNotSetException();

  double centralitypercentile = 99.;
  if(this->GetBeamType() == AliAnalysisTaskEmcal::kAA || (fUseCentralityForpPb && (GetBeamType() == AliAnalysisTaskEmcal::kpA))) {
    AliMultSelection *mult = static_cast<AliMultSelection*>(InputEvent()->FindListObject("MultSelection"));
    if(mult){
      centralitypercentile = mult->GetMultiplicityPercentile(fCentEst.Data());
    } else {
      throw CentralityNotSetException();
    }
  }

  std::string triggerstring(fInputEvent->GetFiredTriggerClasses().Data());
  auto triggerclusters = GetTriggerClusterIndices(triggerstring);

  auto normhist = static_cast<TH2 *>(fHistos->FindObject("hTriggerNorm")),
       luminosityHist = static_cast<TH2 *>(fHistos->FindObject("hTriggerLuminosity")); 

  // Min. bias trigger (reference trigger)
  if(fInputHandler->IsEventSelected() & AliVEvent::kINT7) {
    auto match_triggerclass = MatchTrigger(triggerstring, fMBTriggerClasses);
    AliDebugStream(2) << "Matched trigger: " << match_triggerclass << std::endl;
    if(match_triggerclass.length()){
      auto mbbin = normhist->GetXaxis()->FindBin("INT7");
      luminosityHist->Fill(normhist->GetXaxis()->GetBinCenter(mbbin), centralitypercentile, 1.);    // Luminometer, luminosity counter filled with 1

      // Fill histogram for the trigger clusters firing the event
      auto triggerclusterhist = static_cast<TH1 *>(fHistos->GetListOfHistograms()->FindObject("hClusterCounterINT7"));
      for(auto triggercluster : triggerclusters) {
        triggerclusterhist->Fill(triggercluster + 1);
      }

      if(fCacheTriggerClasses.size()) {
        // Run contains EMCAL trigger, fill normhist and luminosities for EMCAL trigger
        double mbweight = 1./PWG::EMCAL::AliEmcalDownscaleFactorsOCDB::Instance()->GetDownscaleFactorForTriggerClass(match_triggerclass.data());
        normhist->Fill(normhist->GetXaxis()->GetBinCenter(mbbin), centralitypercentile, mbweight);
      
        // Fill luminosity for the various EMCAL triggers
        for(auto emcaltrigger : fCacheDownscaleFactors)  {
          auto triggerEnabled = fCacheTriggerClasses.find(emcaltrigger.first);
          if(triggerEnabled == fCacheTriggerClasses.end()) continue;    // trigger class was not enabled in the run
          auto triggerbin = luminosityHist->GetXaxis()->FindBin(emcaltrigger.first.data());
          auto triggerweight = emcaltrigger.second * mbweight;
          luminosityHist->Fill(luminosityHist->GetXaxis()->GetBinCenter(triggerbin), centralitypercentile, triggerweight);
        }
      }
    }
  }

  // EMCAL triggers
  const UInt_t EL0BIT = AliVEvent::kEMC7,
               EGABIT = AliVEvent::kEMCEGA,
               EJEBIT = AliVEvent::kEMCEJE;
  const Int_t NEMCAL_TRIGGERS = 10;
  const std::array<std::string, NEMCAL_TRIGGERS> EMCAL_TRIGGERS = {{"EMC7", "EG1", "EG2", "EJ1", "EJ2", "DMC7" "DG1", "DG2", "DJ1", "DJ2"}};
  const std::array<UInt_t, NEMCAL_TRIGGERS> EMCAL_TRIGGERBITS = {{EL0BIT, EGABIT, EGABIT, EJEBIT, EJEBIT, EL0BIT, EGABIT, EGABIT, EJEBIT, EJEBIT}};
  bool hasEMCALtrigger = std::find_if(EMCAL_TRIGGERS.begin(), EMCAL_TRIGGERS.end(), [&triggerstring](const std::string &t) { return triggerstring.find(t) != std::string::npos; } ) != EMCAL_TRIGGERS.end();
  if(hasEMCALtrigger) {
    AliDebugStream(2) << "Found EMCAL trigger: "  << triggerstring << std::endl;
  }
  for(Int_t i = 0; i < NEMCAL_TRIGGERS; i++) {      
    auto &triggerclass = EMCAL_TRIGGERS[i];
    auto emctriggerfound = fCacheTriggerClasses.find(triggerclass);
    if(emctriggerfound == fCacheTriggerClasses.end()) continue;     // trigger class was not enabled in the run
    if(!(fInputHandler->IsEventSelected() & EMCAL_TRIGGERBITS[i])) continue;
    if(fInputEvent->GetFiredTriggerClasses().Contains(triggerclass.data())) continue;
      double triggerweight = 1./fCacheDownscaleFactors[triggerclass];
      auto triggerbin = normhist->GetXaxis()->FindBin(triggerclass.data());
      normhist->Fill(normhist->GetXaxis()->GetBinCenter(triggerbin), centralitypercentile, triggerweight);

      // Fill histogram for the trigger clusters firing the event
      auto triggerclusterhist = static_cast<TH1 *>(fHistos->GetListOfHistograms()->FindObject(Form("hClusterCounter%s", triggerclass.data())));
      for(auto triggercluster : triggerclusters) {
        triggerclusterhist->Fill(triggercluster + 1);
      }
  }
  return true;
}

std::string AliAnalysisTaskEmcalTriggerNormalization::MatchTrigger(EMCAL_STRINGVIEW triggerstring, const std::vector<std::string> &triggerclasses) const {
  std::string result;
  auto triggers = PWG::EMCAL::Triggerinfo::DecodeTriggerString(triggerstring);
  for(const auto &t : triggers) {
    bool found = false;
    for(const auto &c : triggerclasses) {
      if(t.IsTriggerClass(c)) {
        result = t.ExpandClassName();
        found = true;
        break;
      }
    }
    if(found) break;
  }
  return result;
}

std::vector<AliAnalysisTaskEmcalTriggerNormalization::TriggerCluster_t> AliAnalysisTaskEmcalTriggerNormalization::GetTriggerClusterIndices(EMCAL_STRINGVIEW triggerstring) const {
  // decode trigger string in order to determine the trigger clusters
  AliDebugGeneralStream("AliAnalysisTaskEmcalTriggerNormalization::GetTriggerClusterIndices", 4) << "Triggerstring: " << triggerstring.data() << std::endl;
  std::vector<TriggerCluster_t> result;
  result.emplace_back(kTrgClusterANY);      // cluster ANY always included

  // Data - separate trigger clusters
  std::vector<std::string> clusternames;
  auto triggerinfos = PWG::EMCAL::Triggerinfo::DecodeTriggerString(triggerstring.data());
  for(auto t : triggerinfos) {
    if(std::find(clusternames.begin(), clusternames.end(), t.Triggercluster()) == clusternames.end()) clusternames.emplace_back(t.Triggercluster());
  }
  bool isCENT = (std::find(clusternames.begin(), clusternames.end(), "CENT") != clusternames.end()),
       isCENTNOTRD = (std::find(clusternames.begin(), clusternames.end(), "CENTNOTRD") != clusternames.end()),
       isCALO = (std::find(clusternames.begin(), clusternames.end(), "CALO") != clusternames.end()),
       isCALOFAST = (std::find(clusternames.begin(), clusternames.end(), "CALOFAST") != clusternames.end()),
       isCENTNOPMD = (std::find(clusternames.begin(), clusternames.end(), "CENTNOPMD") != clusternames.end()),
       isALL = (std::find(clusternames.begin(), clusternames.end(), "ALL") != clusternames.end()),
       isALLNOTRD = (std::find(clusternames.begin(), clusternames.end(), "ALLNOTRD") != clusternames.end());
  AliDebugGeneralStream("AliAnalysisEmcalTriggerSelectionHelperImpl::GetTriggerClusterIndices", 4) << "Selected trigger clusters: CENT: " << (isCENT ? "yes" : "no") << ", CENTNOTRD: " << (isCENTNOTRD ? "yes" : "no") << ", CALO: " << (isCALO ? "yes" : "no") << ", CALOFAST: " << (isCALOFAST ? "yes" :  "no") << std::endl;
  if(isCENT || isCENTNOTRD) {
    if(isCENT) {
      result.emplace_back(kTrgClusterCENT);
      if(isCENTNOTRD) {
        result.emplace_back(kTrgClusterCENTNOTRD);
        result.emplace_back(kTrgClusterCENTBOTH);
      } else result.emplace_back(kTrgClusterOnlyCENT);
    } else {
      result.emplace_back(kTrgClusterCENTNOTRD);
      result.emplace_back(kTrgClusterOnlyCENTNOTRD);
    }
  }
  if(isCALO || isCALOFAST) {
    if(isCALO) {
      result.emplace_back(kTrgClusterCALO);
      if(isCALOFAST) {
        result.emplace_back(kTrgClusterCALOFAST);
        result.emplace_back(kTrgClusterCALOBOTH);
      } else result.emplace_back(kTrgClusterOnlyCALO);
    } else {
      result.emplace_back(kTrgClusterCALOFAST);
      result.emplace_back(kTrgClusterOnlyCALOFAST);
    }
  }
  if(isALL || isALLNOTRD) {
    if(isALL) {
      result.emplace_back(kTrgClusterALL);
      if(isALLNOTRD) {
        result.emplace_back(kTrgClusterALLNOTRD);
        result.emplace_back(kTrgClusterALLBOTH);
      } else result.emplace_back(kTrgClusterOnlyALL);
    } else {
      result.emplace_back(kTrgClusterALLNOTRD);
      result.emplace_back(kTrgClusterOnlyALLNOTRD);
    }
  }
  if(isCENTNOPMD) result.emplace_back(kTrgClusterCENTNOPMD);
  return result;
}

void AliAnalysisTaskEmcalTriggerNormalization::ResetDownscaleFactors(){
  std::vector<std::string> emcaltriggers = {"EMC7", "EG1", "EG2", "DG1", "DG2", "EJ1", "EJ2", "DJ1", "DJ2"};
  for(auto trg : emcaltriggers) fCacheDownscaleFactors[trg] = 1.;
}

void AliAnalysisTaskEmcalTriggerNormalization::RunChanged(int newrun){
  PWG::EMCAL::AliEmcalDownscaleFactorsOCDB::Instance()->SetRun(newrun);

  fCacheTriggerClasses.clear();
  ResetDownscaleFactors();
  std::vector<std::string> emcalL1triggers = {"EG1", "EG2", "DG1", "DG2", "EJ1", "EJ2", "DJ1", "DJ2"};
  std::vector<std::string> emcalL0triggers = {"EMC7", "DMC7"};
  // Load trigger classes used in run
  auto triggerconfiguration = static_cast<AliTriggerConfiguration *>(AliCDBManager::Instance()->Get("GRP/CTP/Config")->GetObject());
  for(auto triggerclassObject : triggerconfiguration->GetClasses()) {
    auto triggerclass = static_cast<AliTriggerClass *>(triggerclassObject);
    auto triggers = Triggerinfo::DecodeTriggerString(triggerclass->GetName()).at(0);    // class name can only have 1 entry
    if(!(triggers.BunchCrossing() == "B" || triggers.BunchCrossing() == "S" || triggers.BunchCrossing() == "Z")) continue;
    // check if the trigger is an EMCAL trigger
    std::string emcalTriggerType;
    const auto &triggerclassname = triggers.Triggerclass();
    for(auto emctrg : emcalL1triggers) {
      if(triggerclassname.find(emctrg) != std::string::npos) {
        emcalTriggerType = emctrg; 
      } 
    }
    if(!emcalTriggerType.length()) {
      // trigger is not a Level1-trigger, check if it is a Level0 trigger
      // Level0 condition must be checked after the Level1 condition because 
      // Level0 can be required for Level1, then the Level0 string is part of the Level1 string
      for(auto emctrg : emcalL0triggers) {
        if(triggerclassname.find(emctrg) != std::string::npos) {
          emcalTriggerType = emctrg; 
        }
      }
      if(!emcalTriggerType.length()) continue;    // trigger is also not an EMCAL Level0 trigger
    }
    // trigger is either an EMCAL Level0 or EMCAL Level1 trigger
    // check if the downscale factor is not > 0.
    auto downscalefactorTrigger = AliEmcalDownscaleFactorsOCDB::Instance()->GetDownscaleFactorForTriggerClass(triggerclass->GetName());
    if(downscalefactorTrigger > 1e-7) {
      // consider trigger class enabled
      fCacheDownscaleFactors[emcalTriggerType] = downscalefactorTrigger;
      fCacheTriggerClasses[emcalTriggerType] = triggerclass->GetName();
      AliInfoStream() << "Found EMCAL trigger: " << emcalTriggerType << ", downscale factor " << downscalefactorTrigger << "(based on " << triggerclass->GetName() << ")\n";
    }
  }
}

AliAnalysisTaskEmcalTriggerNormalization *AliAnalysisTaskEmcalTriggerNormalization::AddTaskEmcalTriggerNormalization(const char *name) {
  auto mgr = AliAnalysisManager::GetAnalysisManager();
  if(!mgr) {
    AliErrorGeneralStream("AliAnalysisTaskEmcalTriggerNormalization::AddTaskTriggerNormalization") << "No analysis manager defined" << std::endl;
    return nullptr;
  }

  auto task = new AliAnalysisTaskEmcalTriggerNormalization(name);
  mgr->AddTask(task);

  std::string outputfile = mgr->GetCommonFileName();
  outputfile += Form(":EmcalTriggerNorm%s", name);

  mgr->ConnectInput(task, 0, mgr->GetCommonInputContainer());
  mgr->ConnectOutput(task, 1, mgr->CreateContainer("EmcalNormalizationHistograms", TList::Class(), AliAnalysisManager::kOutputContainer, outputfile.data()));

  return task;
}