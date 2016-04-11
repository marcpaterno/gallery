
#include "gallery/DataGetterHelper.h"

#include "gallery/AssnsBranchData.h"
#include "gallery/EventNavigator.h"

#include "canvas/Persistency/Provenance/BranchDescription.h"
#include "canvas/Persistency/Provenance/BranchListIndex.h"
#include "canvas/Persistency/Provenance/History.h"
#include "canvas/Persistency/Provenance/ProcessHistory.h"
#include "canvas/Persistency/Provenance/ProductID.h"
#include "canvas/Persistency/Provenance/TypeTools.h"
#include "canvas/Utilities/Exception.h"
#include "canvas/Utilities/WrappedClassName.h"

#include "canvas/Persistency/Common/CacheStreamers.h"
#include "canvas/Persistency/Provenance/TransientStreamer.h"
#include "canvas/Persistency/Common/detail/setPtrVectorBaseStreamer.h"
#include "canvas/Persistency/Common/RefCoreStreamer.h"

#include "TClass.h"
#include "TTree.h"

#include <algorithm>
#include <cstring>

namespace {
  std::string const underscore("_");
  std::string const period(".");
  std::string const emptyString;
}

namespace gallery {

  bool DataGetterHelper::streamersInitialized_ = false;

  DataGetterHelper::DataGetterHelper(EventNavigator const* eventNavigator,
                                     std::shared_ptr<HistoryGetterBase> historyGetter) :
    eventNavigator_(eventNavigator),
    tree_(nullptr),
    edProductTClass_(TClass::GetClass("art::EDProduct")),
    historyGetter_(historyGetter),
    initializedForProcessHistory_(false) {

    initializeStreamers();
  }

  DataGetterHelper::~DataGetterHelper() {
    for (auto label : labels_) {
      delete [] label;
    }
  }

  void DataGetterHelper::getByLabel(std::type_info const& typeInfoOfWrapper,
                                    art::InputTag const& inputTag,
                                    art::EDProduct const*& edProduct) const {

    edProduct = nullptr; // this nullptr indicates product not found yet

    if (!initializedForProcessHistory_) initializeForProcessHistory();

    art::TypeID type(typeInfoOfWrapper);
    InfoForTypeLabelInstance const& info = getInfoForTypeLabelInstance(type,
                                                                       inputTag.label(),
                                                                       inputTag.instance());

    if (inputTag.process().empty()) {
      // search in reverse order of the ProcessHistory
      for (auto reverseIter = info.branchDataIndexOrderedByHistory().rbegin(),
                       iEnd = info.branchDataIndexOrderedByHistory().rend();
           reverseIter != iEnd; ++reverseIter) {
        readBranchData(*reverseIter, edProduct, type);
        // If the product was present in the input file and we successfully read it then we are done
        if (edProduct) return;
      }
    } else {  // process is not empty

      auto itProcess = processNameToProcessIndex_.find(inputTag.process());
      if (itProcess != processNameToProcessIndex_.end()) {
        unsigned int processIndex = itProcess->second;

        unsigned int branchDataIndex = 0;
        if (getBranchDataIndex(info.processIndexToBranchDataIndex(), processIndex, branchDataIndex)) {
          readBranchData(branchDataIndex, edProduct, type);
        }
      }
    }
  }

  void DataGetterHelper::updateFile(TFile* iFile, TTree* iTree, bool initializeTheCache) {
    tree_ = iTree;
    branchMapReader_.updateFile(iFile);

    if (initializeTheCache) {
      tree_->SetCacheSize();
      tree_->AddBranchToCache(eventNavigator_->eventAuxiliaryBranch(), kTRUE);
    }

    for (auto& info : infoVector_) {
      std::vector<std::pair<unsigned int, unsigned int> > old;
      old.swap(info.processIndexToBranchDataIndex());
      info.processIndexToBranchDataIndex().reserve(processNames_.size());
      for (unsigned int processIndex = 0; processIndex < processNames_.size(); ++processIndex) {
        unsigned int branchDataIndex = 0;
        if (getBranchDataIndex(old, processIndex, branchDataIndex)) {
          BranchData& branchData = *branchDataVector_[branchDataIndex];
          TBranch* branch = tree_->GetBranch(branchData.branchName().c_str());

          // This will update the pointer to the TBranch in BranchData.
          // This loop is sufficient to update all BranchData objects in the vector.
          branchData.updateFile(branch);

          // A little paranoia here. What if in one input file Assns<A,B,C> is written into the
          // file and Assns<B,A,C> is written into another? The following deals properly with
          // that case by constructing a new AssnsBranchData object, although I imagine it might
          // never happen in practice.
          if (info.isAssns() && branch != nullptr) {
            TClass* tClass = getTClassUsingBranchDescription(processIndex, info);
            art::TypeID typeIDInDescription(tClass->GetTypeInfo());
            art::TypeID typeIDInBranchData(branchData.tClass()->GetTypeInfo());
            std::string branchName = branchData.branchName();
            if (typeIDInDescription != typeIDInBranchData) {
              branchDataVector_[branchDataIndex].reset(new AssnsBranchData(typeIDInDescription, tClass, branch,
                                                                           edProductTClass_, eventNavigator_, this,
                                                                           std::move(branchName), info.type(), info.partnerType()));
            }
          }
          info.processIndexToBranchDataIndex().push_back(uupair(processIndex, branchDataIndex));
          if (initializeTheCache && branch) {
            tree_->AddBranchToCache(branch, kTRUE);
          }
        } else if (branchMapReader_.branchInRegistryOfAnyOpenedFile(info.branchIDs()[processIndex])) {
          std::string branchName(buildBranchName(info, processNames_[processIndex]));
          addBranchData(std::move(branchName), processIndex, info, initializeTheCache);
        }
      }
      updateBranchDataIndexOrderedByHistory(info);
    }

    if (initializeTheCache) {
      tree_->StopCacheLearningPhase();
    }

    // These must be cleared because the BranchIDLists may be different in
    // different input files.
    branchDataIndexMap_.clear();
    branchDataMissingSet_.clear();

    updateEvent();
  }

  void DataGetterHelper::initializeTTreeCache() {
    tree_->SetCacheSize();
    tree_->AddBranchToCache(eventNavigator_->eventAuxiliaryBranch(), kTRUE);

    for (auto& info : infoVector_) {
      for (auto const& i : info.processIndexToBranchDataIndex()) {
        unsigned int branchDataIndex = i.second;
        BranchData& branchData = *branchDataVector_[branchDataIndex];
        TBranch* branch = branchData.branch();
        if (branch) {
          tree_->AddBranchToCache(branch,kTRUE);
        }
      }
    }
    tree_->StopCacheLearningPhase();
  }

  void DataGetterHelper::updateEvent() {
    branchMapReader_.updateEvent(historyGetter_.get());
    initializedForProcessHistory_ = false;
  }

  void DataGetterHelper::initializeStreamers() {
    if (streamersInitialized_) return;
    streamersInitialized_ = true;

    art::setCacheStreamers();
    art::setProvenanceTransientStreamers();
    art::detail::setBranchDescriptionStreamer();
    art::detail::setPtrVectorBaseStreamer();
    art::configureRefCoreStreamer();
  }

  void DataGetterHelper::initializeForProcessHistory() const {
    initializedForProcessHistory_ = true;

    // Do nothing if the process names in the process history are the same
    art::ProcessHistoryID processHistoryID = historyGetter_->processHistoryID();
    if (processHistoryID == previousProcessHistoryID_) return;
    previousProcessHistoryID_ = processHistoryID;

    art::ProcessHistory const& processHistory = historyGetter_->processHistory();
    if (previousProcessHistoryNames_.size() == processHistory.size()) {
      bool same = true;
      auto iPrevious = previousProcessHistoryNames_.begin();
      for (auto i = processHistory.begin(), iEnd = processHistory.end();
           i != iEnd; ++i, ++iPrevious) {
        if (i->processName() != *iPrevious) {
          same = false;
          break;
        }
      }
      if (same) return;
    }
    previousProcessHistoryNames_.clear();

    // update for the new process history
    orderedProcessIndexes_.clear();
    for (auto i = processHistory.begin(), iEnd = processHistory.end(); i != iEnd; ++i) {
      std::string const& processName = i->processName();
      previousProcessHistoryNames_.push_back(processName);
      auto itFind = processNameToProcessIndex_.find(processName);
      if (itFind == processNameToProcessIndex_.end()) {
        addProcess(processName);
        itFind = processNameToProcessIndex_.find(processName);
      }
      orderedProcessIndexes_.push_back(itFind->second);
    }
    for (auto& info : infoVector_) {
      updateBranchDataIndexOrderedByHistory(info);
    }
  }

  void DataGetterHelper::addProcess(std::string const& processName) const {
    unsigned int processIndex = processNames_.size();
    processNames_.push_back(processName);
    processNameToProcessIndex_[processName] = processIndex;
    for (auto& info : infoVector_) {
      std::string branchName(buildBranchName(info, processName));
      art::BranchID branchID(branchName);
      info.branchIDs().push_back(branchID);
      if (branchMapReader_.branchInRegistryOfAnyOpenedFile(branchID)) {
        addBranchData(std::move(branchName), processIndex, info);
      }
    }
  }

  std::string DataGetterHelper::buildBranchName(InfoForTypeLabelInstance const& info,
                                                std::string const& processName) {
    std::string branchName(info.type().friendlyClassName());
    unsigned int branchNameSize = branchName.size() +
                                  info.label().size() +
                                  info.instance().size() +
                                  processName.size() + 4;
    branchName.reserve(branchNameSize);
    branchName += underscore;
    branchName += info.label();
    branchName += underscore;
    branchName += info.instance();
    branchName += underscore;
    branchName += processName;
    branchName += period;
    return branchName;
  }

  void DataGetterHelper::addBranchData(std::string&& branchName,
                                       unsigned int processIndex,
                                       InfoForTypeLabelInstance const& info,
                                       bool initializeTheCache) const {

    TBranch* branch = tree_->GetBranch(branchName.c_str());
    if (branch == nullptr) return;

    unsigned int branchDataIndex = branchDataVector_.size();

    if (info.isAssns()) {
      TClass* tClass = getTClassUsingBranchDescription(processIndex, info);
      art::TypeID typeIDInDescription(tClass->GetTypeInfo());
      branchDataVector_.emplace_back(new AssnsBranchData(typeIDInDescription, tClass, branch, edProductTClass_,
                                                         eventNavigator_, this, std::move(branchName), info.type(), info.partnerType()));
    } else {
      branchDataVector_.emplace_back(new BranchData(info.type(), info.tClass(), branch, edProductTClass_,
                                                    eventNavigator_, this, std::move(branchName)));
    }
    info.processIndexToBranchDataIndex().push_back(uupair(processIndex, branchDataIndex));
    if (initializeTheCache && branch) {
      tree_->AddBranchToCache(branch,kTRUE);
    }
  }

  TClass*
  DataGetterHelper::getTClassUsingBranchDescription(unsigned int processIndex,
                                                    InfoForTypeLabelInstance const& info) const {

    art::BranchDescription const* branchDescription =
      branchMapReader_.branchIDToBranch(info.branchIDs()[processIndex]);
    if (branchDescription == nullptr) {
      throw art::Exception(art::errors::LogicError)
        << "In DataGetterHelper::getTClassUsingBranchDescription. TBranch exists but no BranchDescription in ProductRegistry.\n"
        << "This shouldn't be possible. For type " << info.type().className() << "\n";
    }
    TClass* tClass = TClass::GetClass(branchDescription->wrappedName().c_str());
    if (tClass == nullptr) {
      throw art::Exception(art::errors::DictionaryNotFound)
        << "In DataGetterHelper::getTClassUsingBranchDescription. Missing dictionary for wrapped Assns class.\n"
        << branchDescription->wrappedName() << "\n";
    }
    return tClass;
  }

  DataGetterHelper::InfoForTypeLabelInstance&
  DataGetterHelper::getInfoForTypeLabelInstance(art::TypeID const& type,
                                                std::string const& label,
                                                std::string const& instance) const {
    if (label.empty()) {
      throw art::Exception(art::errors::LogicError)
        << "getValidHandle was passed an empty module label. Not allowed.\n";
    }

    TypeLabelInstanceKey key(type, label.c_str(), instance.c_str());

    auto itFind = infoMap_.find(key);
    if (itFind == infoMap_.end()) {
      addTypeLabelInstance(type, label, instance);
      itFind = infoMap_.find(key);
    }
    return infoVector_[itFind->second];
  }

  void DataGetterHelper::addTypeLabelInstance(art::TypeID const& type,
                                              std::string const& label,
                                              std::string const& instance) const {

    unsigned int infoIndex = infoVector_.size();
    infoVector_.emplace_back(type, label, instance);
    insertIntoInfoMap(type, label, instance, infoIndex);

    InfoForTypeLabelInstance const& info = infoVector_[infoIndex];

    if (info.isAssns()) {
      insertIntoInfoMap(info.partnerType(), label, instance, infoIndex);
    }

    unsigned int processIndex = 0;
    info.branchIDs().reserve(processNames_.size());
    for(auto const& processName : processNames_) {
      std::string branchName(buildBranchName(info, processName));
      art::BranchID branchID(branchName);
      info.branchIDs().push_back(branchID);
      if (branchMapReader_.branchInRegistryOfAnyOpenedFile(branchID)) {
        addBranchData(std::move(branchName), processIndex, info);
      }
      ++processIndex;
    }
    updateBranchDataIndexOrderedByHistory(info);
  }

  void DataGetterHelper::insertIntoInfoMap(art::TypeID const& type,
                                           std::string const& label,
                                           std::string const& instance,
                                           unsigned int infoIndex) const {
    char const* pLabel = label.c_str();
    char const* pInstance = instance.c_str();

    size_t labelLen = strlen(pLabel) + 1;
    char* newLabel = new char[labelLen];
    std::strncpy(newLabel, pLabel, labelLen);
    // The only purpose of the labels_ vector is to save
    // the pointer so we can delete the allocated memory
    labels_.push_back(newLabel);

    char const* newInstance = emptyString.c_str();
    size_t newInstanceLen = strlen(pInstance) + 1;
    if (newInstanceLen > 1) {
      char* temp = new char[newInstanceLen];
      std::strncpy(temp, pInstance, newInstanceLen);
      labels_.push_back(temp);
      newInstance = temp;
    }

    TypeLabelInstanceKey newKey(type, newLabel, newInstance);

    infoMap_[newKey] = infoIndex;
  }

  void DataGetterHelper::readBranchData(unsigned int branchDataIndex,
                                        art::EDProduct const*& edProduct,
                                        art::TypeID const& type) const {

    BranchData const* branchData =  branchDataVector_[branchDataIndex].get();
    edProduct = branchData->uniqueProduct(type);
  }

  bool DataGetterHelper::getBranchDataIndex(std::vector<std::pair<unsigned int, unsigned int> > const& processIndexToBranchDataIndex,
                                            unsigned int processIndex,
                                            unsigned int & branchDataIndex) const {
    auto itBranchDataIndex = std::lower_bound(processIndexToBranchDataIndex.cbegin(),
                                              processIndexToBranchDataIndex.cend(),
                                              uupair(processIndex, 0),
                                              [](uupair const& l, uupair const& r) { return l.first < r.first; });
    if (itBranchDataIndex != processIndexToBranchDataIndex.cend() &&
        itBranchDataIndex->first == processIndex) {
      branchDataIndex = itBranchDataIndex->second;
      return true;
    }
    return false;
  }

  void DataGetterHelper::updateBranchDataIndexOrderedByHistory(InfoForTypeLabelInstance const& info) const {
    info.branchDataIndexOrderedByHistory().clear();
    if (info.branchDataIndexOrderedByHistory().capacity() < orderedProcessIndexes_.size()) {
      info.branchDataIndexOrderedByHistory().reserve(orderedProcessIndexes_.size());
    }
    for (auto processIndex : orderedProcessIndexes_) {
      unsigned int branchDataIndex = 0;
      if (getBranchDataIndex(info.processIndexToBranchDataIndex(), processIndex, branchDataIndex) &&
          branchDataVector_[branchDataIndex]->branch() != nullptr) {
        info.branchDataIndexOrderedByHistory().push_back(branchDataIndex);
      }
    }
  }

  bool
  DataGetterHelper::getByBranchDescription(art::BranchDescription const& desc,
                                           unsigned int & branchDataIndex) const {

    if (!initializedForProcessHistory_) initializeForProcessHistory();

    TClass* tClass = TClass::GetClass(desc.wrappedName().c_str());
    std::type_info const& typeInfoOfWrapper = *tClass->GetTypeInfo();
    art::TypeID type(typeInfoOfWrapper);
    InfoForTypeLabelInstance const& info = getInfoForTypeLabelInstance(type,
                                                                       desc.moduleLabel(),
                                                                       desc.productInstanceName());
    auto itProcess = processNameToProcessIndex_.find(desc.processName());
    if (itProcess != processNameToProcessIndex_.end()) {
      unsigned int processIndex = itProcess->second;
      if (getBranchDataIndex(info.processIndexToBranchDataIndex(), processIndex, branchDataIndex)) {
        return branchDataVector_[branchDataIndex]->branch() != nullptr;
      }
    }
    return false;
  }

  art::EDProductGetter const* DataGetterHelper::getEDProductGetterImpl(art::ProductID const& productID) const {
    art::ProcessIndex processIndex = productID.processIndex();
    art::BranchListIndexes const& branchListIndexes = historyGetter_->history().branchListIndexes();
    if (processIndex == 0 || processIndex > branchListIndexes.size()) {
      throw art::Exception(art::errors::LogicError)
        << "DataGetterHelper::getEDProductGetterImpl out of range processIndex in ProductID.\n";
    }

    std::pair<unsigned short, unsigned short> key(productID.productIndex(),
                                                  branchListIndexes[processIndex - 1]);
    unsigned int branchDataIndex = 0;
    auto itFind =  branchDataIndexMap_.find(key);
    if (itFind == branchDataIndexMap_.end()) {
      if (branchDataMissingSet_.find(key) != branchDataMissingSet_.end()) {
        return &invalidBranchData_;
      }
      art::BranchDescription const* branchDescription = branchMapReader_.productToBranch(productID);
      if (branchDescription) {
        if (!getByBranchDescription(*branchDescription, branchDataIndex)) {
          branchDataMissingSet_.insert(key);
          return &invalidBranchData_;
        }
      } else {
        return &invalidBranchData_;
      }
      branchDataIndexMap_.emplace(key, branchDataIndex);
    } else {
      branchDataIndex = itFind->second;
    }
    return  branchDataVector_[branchDataIndex].get();
  }

  DataGetterHelper::InfoForTypeLabelInstance::
  InfoForTypeLabelInstance(art::TypeID const& iType,
                           std::string const& iLabel,
                           std::string const& iInstance) :
    type_(iType),
    label_(iLabel),
    instance_(iInstance),
    tClass_(nullptr),
    isAssns_(false),
    partnerType_() {

    tClass_ = TClass::GetClass(type_.typeInfo());

    // Note we allow tClass_ to be null at this point. If the branch
    // is actually in the input file and there is an attempt to construct
    // a BranchData object with it null, a missing dictionary exception
    // will be thrown.  This also means we just assume it is not an
    // Assns. Even if this assumption is wrong, we will get the correct
    // behavior from getByLabel. A missing dictionary exception will
    // get thrown if the branch is in the input file and a ProductNotFound
    // exception will get thrown if it is not.

    if (tClass_) {

      TClass* firstTemplateArgument = art::type_of_template_arg(tClass_, 0);
      if (!firstTemplateArgument) {
        throw art::Exception(art::errors::DictionaryNotFound)
          << "In InfoForTypeLabelInstance constructor.\nMissing dictionary for PROD even though the Wrapper<PROD> dictionary exists.\n"
          << tClass_->GetName() << "\n";
      }
      if (std::string(firstTemplateArgument->GetName()).find("art::Assns<") == 0ul) {

        isAssns_ = true;

        std::string partner = firstTemplateArgument->GetName();
        partner += "::partner_t";
        TClass* unwrappedPartnerTClass = TClass::GetClass(partner.c_str());
        if (!unwrappedPartnerTClass) {
          throw art::Exception(art::errors::DictionaryNotFound)
            << "In InfoForTypeLabelInstance constructor.\nMissing dictionary for partner of Assns class.\n"
            << partner << "\n";
        }
        std::string wrappedPartnerName = art::wrappedClassName(unwrappedPartnerTClass->GetName());
        TClass* partnerTClass = TClass::GetClass(wrappedPartnerName.c_str());
        if (!partnerTClass) {
          throw art::Exception(art::errors::DictionaryNotFound)
            << "In InfoForTypeLabelInstance constructor.\nMissing dictionary for wrapped partner of Assns class.\n"
            << wrappedPartnerName << "\n";
        }
        partnerType_ = art::TypeID(partnerTClass->GetTypeInfo());
      }
    }
  }
}