#ifndef gallery_AssnsBranchData_h
#define gallery_AssnsBranchData_h
// vim: set sw=2 expandtab :

#include "canvas/Utilities/TypeID.h"
#include "canvas/Persistency/Common/EDProduct.h"
#include "gallery/BranchData.h"

#include <memory>
#include <string>

class TBranch;
class TClass;

namespace art {

class EDProduct;
class PrincipalBase;

typedef PrincipalBase EDProductGetterFinder;

} // namespace art

namespace gallery {

class EventNavigator;

class AssnsBranchData : public BranchData {

public:

  virtual
  ~AssnsBranchData();

  AssnsBranchData(art::TypeID const& type, TClass* iTClass, TBranch* branch, EventNavigator const* eventNavigator,
                  art::EDProductGetterFinder const* finder, std::string&& iBranchName, art::TypeID const& infoType,
                  art::TypeID const& infoPartnerType);

private:

  virtual
  void
  updateFile(TBranch* iBranch) override;

  virtual
  art::EDProduct const*
  getIt_() const override;

  virtual
  art::EDProduct const*
  uniqueProduct_() const override;

  virtual
  art::EDProduct const*
  uniqueProduct_(art::TypeID const& wanted_wrapper_type) const override;

private:

  art::TypeID
  secondary_wrapper_type_{};

  mutable
  std::unique_ptr<art::EDProduct>
  secondaryProduct_{nullptr};

  mutable
  long long
  secondaryLastProduct_{-1};

};

} // namespace gallery

#endif /* gallery_AssnsBranchData_h */

// Local Variables:
// mode: c++
// End:
